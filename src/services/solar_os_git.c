#include "solar_os_git.h"
#include "solar_os_git_transport.h"
#include "solar_os_git_state.h"

#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "git2.h"
#include "git2/sys/errors.h"
#include "git2/sys/alloc.h"
#include "solar_os_memory.h"
#ifdef ESP_PLATFORM
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

static atomic_flag git_busy = ATOMIC_FLAG_INIT;

static void cooperate(void)
{
#ifdef ESP_PLATFORM
    static SOLAR_OS_GIT_STATE_BSS int64_t last_yield;
    int64_t now = esp_timer_get_time();
    if (now - last_yield >= 20000) {
        last_yield = now;
        vTaskDelay(1);
    }
#endif
}

static void *git_allocate(size_t size, const char *file, int line)
{
    (void)file; (void)line;
    cooperate();
    return solar_os_memory_alloc(size, SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "git");
}

static void *git_reallocate(void *ptr, size_t size, const char *file, int line)
{
    (void)file; (void)line;
    cooperate();
    return solar_os_memory_realloc(ptr, size, SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "git");
}

static int fail(const char *message)
{
    git_error_set_str(GIT_ERROR_INVALID, message);
    return -1;
}

static bool cancelled(const solar_os_git_options_t *o)
{
    return o && o->cancelled && o->cancelled(o->user);
}

static void output(const solar_os_git_options_t *o, const char *text, size_t size)
{
    if (o && o->output) o->output(o->user, text, size);
}

static void print(const solar_os_git_options_t *o, const char *fmt, ...)
{
    char line[512];
    va_list args;
    va_start(args, fmt);
    int size = vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    if (size > 0) output(o, line, (size_t)size < sizeof(line) ? (size_t)size : sizeof(line) - 1);
}

static int progress(const git_indexer_progress *p, void *payload)
{
    cooperate();
    if (cancelled(payload)) return GIT_EUSER;
    if (p->received_bytes > 16U * 1024U * 1024U)
        return fail("transfer exceeds 16 MiB; use a smaller repository");
    return 0;
}

static int sideband(const char *data, int len, void *payload)
{
    cooperate();
    if (cancelled(payload)) return GIT_EUSER;
    if (len > 0) output(payload, data, (size_t)len);
    return 0;
}

static int push_status(const char *ref, const char *status, void *payload)
{
    if (status) {
        git_error_set(GIT_ERROR_NET, "%s: %s", ref, status);
        return -1;
    }
    print(payload, "updated %s\n", ref);
    return cancelled(payload) ? GIT_EUSER : 0;
}

static int checkout_notify(git_checkout_notify_t why, const char *path,
    const git_diff_file *base, const git_diff_file *target,
    const git_diff_file *workdir, void *payload)
{
    (void)why; (void)base; (void)workdir;
    cooperate();
    if (cancelled(payload)) return GIT_EUSER;
    if (target && (target->mode == GIT_FILEMODE_LINK || target->mode == GIT_FILEMODE_COMMIT)) {
        git_error_set(GIT_ERROR_CHECKOUT, "unsupported symlink or submodule: %s", path);
        return -1;
    }
    return 0;
}

static git_checkout_options checkout_options(const solar_os_git_options_t *o)
{
    git_checkout_options c;
    git_checkout_options_init(&c, GIT_CHECKOUT_OPTIONS_VERSION);
    c.checkout_strategy = GIT_CHECKOUT_SAFE;
    c.notify_flags = GIT_CHECKOUT_NOTIFY_ALL;
    c.notify_cb = checkout_notify;
    c.notify_payload = (void *)o;
    return c;
}

static git_remote_callbacks callbacks(const solar_os_git_options_t *o)
{
    git_remote_callbacks cb;
    git_remote_init_callbacks(&cb, GIT_REMOTE_CALLBACKS_VERSION);
    cb.payload = (void *)o;
    cb.transfer_progress = progress;
    cb.sideband_progress = sideband;
    cb.push_update_reference = push_status;
    cb.transport = solar_os_git_transport_factory;
    return cb;
}

static int configure_repository(git_repository *repo)
{
    git_config *config = NULL;
    int rc = git_repository_config(&config, repo);
    if (!rc) rc = git_config_set_bool(config, "core.filemode", 0);
    if (!rc) rc = git_config_set_bool(config, "core.symlinks", 0);
    if (!rc) rc = git_config_set_bool(config, "core.ignorecase", 1);
    git_config_free(config);
    return rc;
}

static int clone_repository(git_repository **out, const char *path, int bare, void *payload)
{
    (void)payload;
    int rc = git_repository_init(out, path, bare);
    if (!rc) rc = configure_repository(*out);
    return rc;
}

static int full_path(char *out, size_t size, const char *cwd, const char *arg)
{
    char raw[512];
    int n = arg[0] == '/' ? snprintf(raw, sizeof(raw), "%s", arg) :
        snprintf(raw, sizeof(raw), "%s/%s", cwd, arg);
    if (n < 0 || (size_t)n >= sizeof(raw) || size < 2) return fail("path too long");
    size_t used = 1;
    out[0] = '/';
    const char *p = raw;
    while (*p) {
        while (*p == '/') p++;
        const char *part = p;
        while (*p && *p != '/') p++;
        size_t len = (size_t)(p - part);
        if (!len || (len == 1 && *part == '.')) continue;
        if (len == 2 && part[0] == '.' && part[1] == '.') {
            while (used > 1 && out[used - 1] != '/') used--;
            if (used > 1) used--;
            continue;
        }
        if (used + len + 2 > size) return fail("path too long");
        if (used > 1) out[used++] = '/';
        memcpy(out + used, part, len); used += len;
    }
    out[used] = 0;
    return 0;
}

static int init_or_clone(const char *cwd, int argc, char **argv,
                         const solar_os_git_options_t *o)
{
    git_repository *repo = NULL;
    bool clone = strcmp(argv[0], "clone") == 0;
    const char *branch = NULL;
    int pos = 1;
    if (clone && argc > 3 && strcmp(argv[pos], "-b") == 0) {
        branch = argv[pos + 1]; pos += 2;
    }
    if ((!clone && argc > 2) || (clone && (argc <= pos || argc > pos + 2)))
        return fail("usage: git init [directory] | git clone [-b branch] <url> [directory]");
    char derived[256];
    const char *dir = !clone ? (argc == 2 ? argv[1] : ".") :
        (argc == pos + 2 ? argv[pos + 1] : NULL);
    if (!dir) {
        const char *url = argv[pos];
        const char *base = strrchr(url, '/');
        if (!base) base = strrchr(url, ':');
        base = base ? base + 1 : url;
        size_t len = strlen(base);
        if (len > 4 && strcmp(base + len - 4, ".git") == 0) len -= 4;
        if (!len || len >= sizeof(derived) || base[0] == '.' || strchr(base, '?') || strchr(base, '#'))
            return fail("specify a destination directory");
        memcpy(derived, base, len); derived[len] = 0; dir = derived;
    }
    char path[512];
    if (full_path(path, sizeof(path), cwd, dir) < 0) return -1;
    int rc;
    if (clone) {
        git_clone_options opts;
        git_clone_options_init(&opts, GIT_CLONE_OPTIONS_VERSION);
        opts.local = GIT_CLONE_NO_LOCAL;
        opts.checkout_branch = branch;
        opts.repository_cb = clone_repository;
        opts.checkout_opts = checkout_options(o);
        opts.fetch_opts.callbacks = callbacks(o);
        opts.fetch_opts.follow_redirects = GIT_REMOTE_REDIRECT_NONE;
        print(o, "cloning into %s\n", path);
        rc = git_clone(&repo, argv[pos], path, &opts);
    } else {
        git_repository_init_options opts;
        git_repository_init_options_init(&opts, GIT_REPOSITORY_INIT_OPTIONS_VERSION);
        opts.flags = GIT_REPOSITORY_INIT_MKPATH | GIT_REPOSITORY_INIT_NO_REINIT;
        opts.initial_head = "main";
        rc = git_repository_init_ext(&repo, path, &opts);
        if (!rc) rc = configure_repository(repo);
    }
    if (!rc) print(o, "%s %s\n", clone ? "cloned" : "initialized", path);
    git_repository_free(repo);
    return rc;
}

static int head_commit(git_commit **out, git_repository *repo)
{
    git_object *obj = NULL;
    int rc = git_revparse_single(&obj, repo, "HEAD^{commit}");
    if (!rc) *out = (git_commit *)obj;
    return rc;
}

static int status_repository(git_repository *repo, const solar_os_git_options_t *o)
{
    git_status_options opts;
    git_status_options_init(&opts, GIT_STATUS_OPTIONS_VERSION);
    opts.flags = GIT_STATUS_OPT_INCLUDE_UNTRACKED | GIT_STATUS_OPT_RECURSE_UNTRACKED_DIRS;
    git_status_list *list = NULL;
    int rc = git_status_list_new(&list, repo, &opts);
    if (rc) return rc;
    git_reference *head = NULL;
    int head_rc = git_repository_head(&head, repo);
    if (head_rc == GIT_EUNBORNBRANCH) {
        head_rc = git_reference_lookup(&head, repo, "HEAD");
        if (!head_rc) {
            const char *name = git_reference_symbolic_target(head);
            if (name) print(o, "On %s\n", !strncmp(name, "refs/heads/", 11) ? name + 11 : name);
            print(o, "No commits yet\n");
        }
    } else if (!head_rc) print(o, "On %s\n", git_reference_shorthand(head));
    git_reference_free(head);
    if (head_rc) { git_status_list_free(list); return head_rc; }
    size_t count = git_status_list_entrycount(list);
    for (size_t i = 0; i < count && !cancelled(o); i++) {
        const git_status_entry *entry = git_status_byindex(list, i);
        const git_diff_delta *delta = entry->index_to_workdir ? entry->index_to_workdir : entry->head_to_index;
        unsigned s = entry->status;
        char x = s & GIT_STATUS_INDEX_NEW ? 'A' : s & GIT_STATUS_INDEX_DELETED ? 'D' :
            s & GIT_STATUS_INDEX_RENAMED ? 'R' : s & GIT_STATUS_INDEX_MODIFIED ? 'M' : ' ';
        char y = s & GIT_STATUS_WT_NEW ? '?' : s & GIT_STATUS_WT_DELETED ? 'D' :
            s & GIT_STATUS_WT_MODIFIED ? 'M' : ' ';
        if (s & GIT_STATUS_CONFLICTED) x = y = 'U';
        if (y == '?') x = '?';
        print(o, "%c%c %s\n", x, y, delta ? delta->new_file.path : "?");
    }
    if (!count) print(o, "working tree clean\n");
    git_status_list_free(list);
    return cancelled(o) ? GIT_EUSER : 0;
}

static int diff_line(const git_diff_delta *delta, const git_diff_hunk *hunk,
                      const git_diff_line *line, void *payload)
{
    (void)delta; (void)hunk;
    if (cancelled(payload)) return GIT_EUSER;
    if (line->origin == '+' || line->origin == '-' || line->origin == ' ')
        output(payload, &line->origin, 1);
    output(payload, line->content, line->content_len);
    return 0;
}

static int diff_repository(git_repository *repo, bool cached, const solar_os_git_options_t *o)
{
    git_diff *diff = NULL;
    git_commit *head = NULL;
    git_tree *tree = NULL;
    int rc = 0;
    git_diff_options opts;
    git_diff_options_init(&opts, GIT_DIFF_OPTIONS_VERSION);
    opts.max_size = 1024 * 1024;
    if (cached) {
        int unborn = git_repository_head_unborn(repo);
        if (unborn < 0) rc = unborn;
        if (!rc && !unborn) {
            rc = head_commit(&head, repo);
            if (!rc) rc = git_commit_tree(&tree, head);
        }
        if (!rc) rc = git_diff_tree_to_index(&diff, repo, tree, NULL, &opts);
    } else rc = git_diff_index_to_workdir(&diff, repo, NULL, &opts);
    if (!rc) rc = git_diff_print(diff, GIT_DIFF_FORMAT_PATCH, diff_line, (void *)o);
    git_diff_free(diff); git_tree_free(tree); git_commit_free(head);
    return rc;
}

static int stage_path(const char *path, const char *matched, void *payload)
{
    (void)matched;
    cooperate();
    if (cancelled(payload)) return GIT_EUSER;
    /* Never stage another repository's metadata via an explicit pathspec. */
    if (strcmp(path, ".git") == 0 || strncmp(path, ".git/", 5) == 0) return 1;
    return 0;
}

static int add_paths(git_repository *repo, const char *cwd, int argc, char **argv,
                     const solar_os_git_options_t *o)
{
    if (argc < 2) return fail("usage: git add <path>...");
    git_index *index = NULL;
    int rc = git_repository_index(&index, repo);
    const char *root = git_repository_workdir(repo);
    size_t root_len = root ? strlen(root) : 0;
    if (root_len > 1 && root[root_len - 1] == '/') root_len--;
    char **paths = solar_os_memory_calloc((size_t)argc - 1, sizeof(char *),
        SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "git.paths");
    if (!paths) { git_index_free(index); return fail("no memory"); }
    for (int i = 1; !rc && i < argc; i++) {
        char absolute[512];
        rc = full_path(absolute, sizeof(absolute), cwd, argv[i]);
        if (rc) break;
        /* libgit2 canonicalizes pathspecs, but reject traversal before passing
         * a path relative to the repository root. */
        if (!root || strncmp(absolute, root, root_len) != 0 || (root_len != 1 &&
            (absolute[root_len] != '/' && absolute[root_len] != 0))) {
            rc = fail("path is outside the repository"); break;
        }
        const char *relative = root_len == 1 ? (absolute[1] ? absolute + 1 : ".") :
            absolute[root_len] ? absolute + root_len + 1 : ".";
        paths[i - 1] = solar_os_memory_alloc(strlen(relative) + 1,
            SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "git.path");
        if (paths[i - 1]) strcpy(paths[i - 1], relative);
        if (!paths[i - 1]) rc = fail("no memory");
    }
    git_strarray specs = { paths, (size_t)argc - 1 };
    if (!rc) rc = git_index_add_all(index, &specs, GIT_INDEX_ADD_DEFAULT, stage_path, (void *)o);
    if (!rc) rc = git_index_update_all(index, &specs, stage_path, (void *)o);
    if (!rc) rc = git_index_write(index);
    for (int i = 1; i < argc; i++) free(paths[i - 1]);
    free(paths); git_index_free(index);
    return rc;
}

static int commit_repository(git_repository *repo, const char *message,
                              const solar_os_git_options_t *o)
{
    git_index *index = NULL;
    git_tree *tree = NULL, *oldtree = NULL;
    git_commit *parent = NULL;
    git_signature *signature = NULL;
    git_oid tree_id, commit_id;
    int rc = git_repository_index(&index, repo);
    if (!rc && git_index_has_conflicts(index)) rc = fail("resolve index conflicts before committing");
    if (!rc) rc = git_index_write_tree(&tree_id, index);
    if (!rc) rc = git_tree_lookup(&tree, repo, &tree_id);
    int unborn = git_repository_head_unborn(repo);
    if (!rc && unborn < 0) rc = unborn;
    if (!rc && unborn && git_tree_entrycount(tree) == 0) rc = fail("nothing staged to commit");
    if (!rc && !unborn) {
        rc = head_commit(&parent, repo);
        if (!rc) rc = git_commit_tree(&oldtree, parent);
        if (!rc && git_oid_equal(git_tree_id(oldtree), &tree_id)) rc = fail("nothing staged to commit");
    }
    if (!rc) rc = git_signature_default(&signature, repo);
    if (!rc && cancelled(o)) rc = GIT_EUSER;
    if (!rc) rc = git_commit_create(&commit_id, repo, "HEAD", signature, signature,
        NULL, message, tree, parent ? 1 : 0, (const git_commit **)&parent);
    if (!rc) print(o, "committed %.12s\n", git_oid_tostr_s(&commit_id));
    git_signature_free(signature); git_tree_free(oldtree); git_tree_free(tree);
    git_commit_free(parent); git_index_free(index);
    return rc;
}

static int branches(git_repository *repo, int argc, char **argv,
                     const solar_os_git_options_t *o)
{
    git_reference *ref = NULL;
    git_commit *head = NULL;
    int rc = 0;
    if (argc == 1) {
        git_branch_iterator *iter = NULL;
        git_branch_t type;
        rc = git_branch_iterator_new(&iter, repo, GIT_BRANCH_ALL);
        if (rc) return rc;
        while (!(rc = git_branch_next(&ref, &type, iter))) {
            print(o, "%c %s\n", git_branch_is_head(ref) == 1 ? '*' : ' ', git_reference_shorthand(ref));
            git_reference_free(ref); ref = NULL;
            if (cancelled(o)) { rc = GIT_EUSER; break; }
        }
        if (rc == GIT_ITEROVER) rc = 0;
        git_branch_iterator_free(iter);
    } else if (argc == 2 && argv[1][0] != '-') {
        rc = head_commit(&head, repo);
        if (!rc) rc = git_branch_create(&ref, repo, argv[1], head, 0);
    } else if (argc == 3 && strcmp(argv[1], "-d") == 0) {
        rc = git_branch_lookup(&ref, repo, argv[2], GIT_BRANCH_LOCAL);
        if (!rc) rc = head_commit(&head, repo);
        if (!rc && !git_oid_equal(git_commit_id(head), git_reference_target(ref))) {
            rc = git_graph_descendant_of(repo, git_commit_id(head), git_reference_target(ref));
            if (rc == 0) rc = fail("branch has unmerged commits");
            else if (rc == 1) rc = 0;
        }
        if (!rc) rc = git_branch_delete(ref);
    } else rc = fail("usage: git branch [name | -d name]");
    git_reference_free(ref); git_commit_free(head);
    return rc;
}

static int switch_unborn_branch(git_repository *repo, const char *name,
                                 const solar_os_git_options_t *o)
{
    int valid = 0;
    int rc = git_branch_name_is_valid(&valid, name);
    if (rc) return rc;
    if (!valid) return fail("invalid branch name");
    git_reference *existing = NULL;
    rc = git_branch_lookup(&existing, repo, name, GIT_BRANCH_LOCAL);
    git_reference_free(existing);
    if (!rc) return fail("branch already exists");
    if (rc != GIT_ENOTFOUND) return rc;
    char refname[512];
    int len = snprintf(refname, sizeof(refname), "refs/heads/%s", name);
    if (len < 0 || (size_t)len >= sizeof(refname)) return fail("branch name too long");
    /* No tree exists yet. Select the initial branch without touching the
     * worktree or index; the first commit creates the branch reference. */
    rc = git_repository_set_head(repo, refname);
    if (!rc) print(o, "switched to %s (no commits yet)\n", name);
    return rc;
}

static int switch_branch(git_repository *repo, int argc, char **argv,
                          const solar_os_git_options_t *o)
{
    bool create = argc == 3 && (!strcmp(argv[1], "-b") || !strcmp(argv[1], "-c"));
    if ((!create && argc != 2) || argv[argc - 1][0] == '-')
        return fail("usage: git checkout [-b] <branch> | git switch [-c] <branch>");
    if (create) {
        int unborn = git_repository_head_unborn(repo);
        if (unborn < 0) return unborn;
        if (unborn) return switch_unborn_branch(repo, argv[2], o);
    }
    git_reference *ref = NULL, *local = NULL;
    git_commit *commit = NULL;
    int rc;
    if (create) {
        rc = head_commit(&commit, repo);
        if (!rc) rc = git_branch_create(&ref, repo, argv[2], commit, 0);
    } else {
        rc = git_branch_lookup(&ref, repo, argv[1], GIT_BRANCH_LOCAL);
        if (rc == GIT_ENOTFOUND) {
            char remote_name[300];
            snprintf(remote_name, sizeof(remote_name), "origin/%s", argv[1]);
            rc = git_branch_lookup(&ref, repo, remote_name, GIT_BRANCH_REMOTE);
            if (!rc) rc = git_commit_lookup(&commit, repo, git_reference_target(ref));
            if (!rc) rc = git_branch_create(&local, repo, argv[1], commit, 0);
            if (!rc) rc = git_branch_set_upstream(local, remote_name);
            if (!rc) { git_reference_free(ref); ref = local; local = NULL; }
        }
        if (!rc && !commit) rc = git_commit_lookup(&commit, repo, git_reference_target(ref));
    }
    git_checkout_options opts = checkout_options(o);
    if (!rc) rc = git_checkout_tree(repo, (git_object *)commit, &opts);
    if (!rc) rc = git_repository_set_head(repo, git_reference_name(ref));
    if (!rc) print(o, "switched to %s\n", git_reference_shorthand(ref));
    git_reference_free(local); git_reference_free(ref); git_commit_free(commit);
    return rc;
}

static int remotes(git_repository *repo, int argc, char **argv,
                    const solar_os_git_options_t *o)
{
    git_remote *remote = NULL;
    int rc = 0;
    if (argc == 1 || (argc == 2 && !strcmp(argv[1], "-v"))) {
        git_strarray names = {0};
        rc = git_remote_list(&names, repo);
        for (size_t i = 0; !rc && i < names.count; i++) {
            rc = git_remote_lookup(&remote, repo, names.strings[i]);
            if (!rc) print(o, "%s  %s\n", names.strings[i], git_remote_url(remote));
            git_remote_free(remote); remote = NULL;
        }
        git_strarray_dispose(&names);
    } else if (argc == 4 && (!strcmp(argv[1], "add") || !strcmp(argv[1], "set-url"))) {
        solar_os_git_ssh_url_t parsed;
        if (strncmp(argv[3], "https://", 8) && !solar_os_git_parse_ssh_url(argv[3], &parsed))
            return fail("use an SSH or HTTPS remote URL");
        if (!strcmp(argv[1], "add")) rc = git_remote_create(&remote, repo, argv[2], argv[3]);
        else rc = git_remote_set_url(repo, argv[2], argv[3]);
    } else rc = fail("usage: git remote [-v | add <name> <url> | set-url <name> <url>]");
    git_remote_free(remote);
    return rc;
}

static int fetch_remote(git_repository *repo, const char *name, const solar_os_git_options_t *o)
{
    git_remote *remote = NULL;
    git_fetch_options opts;
    git_fetch_options_init(&opts, GIT_FETCH_OPTIONS_VERSION);
    opts.callbacks = callbacks(o);
    opts.follow_redirects = GIT_REMOTE_REDIRECT_NONE;
    int rc = git_remote_lookup(&remote, repo, name);
    if (!rc) rc = git_remote_fetch(remote, NULL, &opts, "SolarOS fetch");
    git_remote_free(remote);
    return rc;
}

static int pull_repository(git_repository *repo, const char *remote_name,
                           const char *branch, const solar_os_git_options_t *o)
{
    git_reference *head = NULL, *upstream = NULL, *updated = NULL;
    git_annotated_commit *incoming = NULL;
    git_commit *target = NULL;
    git_buf configured_remote = GIT_BUF_INIT;
    git_merge_analysis_t analysis;
    git_merge_preference_t preference;
    int rc = git_repository_head(&head, repo);
    if (!rc && !git_reference_is_branch(head)) rc = fail("pull requires a local branch");
    if (!rc && !remote_name) {
        rc = git_branch_upstream_remote(&configured_remote, repo, git_reference_name(head));
        if (!rc) remote_name = configured_remote.ptr;
    }
    if (!rc && !branch) {
        rc = git_branch_upstream(&upstream, head);
        if (!rc) {
            char prefix[256];
            int len = snprintf(prefix, sizeof(prefix), "refs/remotes/%s/", remote_name);
            if (len < 0 || (size_t)len >= sizeof(prefix) ||
                strncmp(git_reference_name(upstream), prefix, (size_t)len))
                rc = fail("upstream belongs to another remote; specify remote and branch");
        }
        git_reference_free(upstream); upstream = NULL;
    }
    if (!rc) rc = fetch_remote(repo, remote_name, o);
    if (!rc) {
        if (branch) {
            char name[512];
            int len = snprintf(name, sizeof(name), "refs/remotes/%s/%s", remote_name, branch);
            rc = len < 0 || (size_t)len >= sizeof(name) ? fail("branch name too long") :
                git_reference_lookup(&upstream, repo, name);
        } else rc = git_branch_upstream(&upstream, head);
    }
    if (!rc) rc = git_annotated_commit_from_ref(&incoming, repo, upstream);
    if (!rc) rc = git_merge_analysis(&analysis, &preference, repo,
                                     (const git_annotated_commit **)&incoming, 1);
    if (!rc && (analysis & GIT_MERGE_ANALYSIS_UP_TO_DATE)) print(o, "already up to date\n");
    else if (!rc && (analysis & GIT_MERGE_ANALYSIS_FASTFORWARD)) {
        rc = git_commit_lookup(&target, repo, git_annotated_commit_id(incoming));
        git_checkout_options opts = checkout_options(o);
        if (!rc) rc = git_checkout_tree(repo, (git_object *)target, &opts);
        if (!rc) rc = git_reference_set_target(&updated, head, git_commit_id(target), "SolarOS pull: fast-forward");
        if (!rc) print(o, "fast-forwarded %.12s\n", git_oid_tostr_s(git_commit_id(target)));
    } else if (!rc) rc = fail("branches have diverged; merge or rebase on a desktop, then pull again");
    git_reference_free(updated); git_reference_free(upstream); git_reference_free(head);
    git_annotated_commit_free(incoming); git_commit_free(target);
    git_buf_dispose(&configured_remote);
    return rc;
}

static int push_repository(git_repository *repo, const char *name,
                            const char *branch, bool track, const solar_os_git_options_t *o)
{
    git_reference *head = NULL;
    git_remote *remote = NULL;
    int rc = git_repository_head(&head, repo);
    if (rc == GIT_EUNBORNBRANCH) rc = fail("no commits to push; add files and commit first");
    if (!rc && !git_reference_is_branch(head)) rc = fail("push requires a local branch");
    if (!rc) rc = git_remote_lookup(&remote, repo, name);
    char spec[1024];
    if (!rc) {
        int len = snprintf(spec, sizeof(spec), "%s:refs/heads/%s",
            git_reference_name(head), branch ? branch : git_reference_shorthand(head));
        if (len < 0 || (size_t)len >= sizeof(spec)) rc = fail("branch name too long");
    }
    git_push_options opts;
    git_push_options_init(&opts, GIT_PUSH_OPTIONS_VERSION);
    opts.pb_parallelism = 1;
    opts.callbacks = callbacks(o);
    opts.follow_redirects = GIT_REMOTE_REDIRECT_NONE;
    char *specs[] = {spec};
    git_strarray refspecs = {specs, 1};
    if (!rc) rc = git_remote_push(remote, &refspecs, &opts);
    if (!rc && track) {
        char upstream[512];
        int len = snprintf(upstream, sizeof(upstream), "%s/%s", name,
            branch ? branch : git_reference_shorthand(head));
        if (len < 0 || (size_t)len >= sizeof(upstream)) rc = fail("upstream name too long");
        else rc = git_branch_set_upstream(head, upstream);
        if (rc) print(o, "push succeeded, but upstream setup failed\n");
    }
    git_remote_free(remote); git_reference_free(head);
    return rc;
}

static int log_repository(git_repository *repo, int count, const solar_os_git_options_t *o)
{
    git_revwalk *walk = NULL;
    git_commit *commit = NULL;
    git_oid id;
    int rc = git_revwalk_new(&walk, repo);
    if (!rc) rc = git_revwalk_push_head(walk);
    for (int i = 0; !rc && i < count; i++) {
        rc = git_revwalk_next(&id, walk);
        if (rc) break;
        rc = git_commit_lookup(&commit, repo, &id);
        if (!rc) print(o, "%.12s %s\n", git_oid_tostr_s(&id), git_commit_summary(commit));
        git_commit_free(commit); commit = NULL;
        if (cancelled(o)) rc = GIT_EUSER;
    }
    if (rc == GIT_ITEROVER) rc = 0;
    git_revwalk_free(walk);
    return rc;
}

static int dispatch(git_repository *repo, const char *cwd, int argc, char **argv,
                     const solar_os_git_options_t *o)
{
    const char *cmd = argv[0];
    if (!strcmp(cmd, "status") && argc == 1) return status_repository(repo, o);
    if (!strcmp(cmd, "diff") && (argc == 1 || (argc == 2 && !strcmp(argv[1], "--cached"))))
        return diff_repository(repo, argc == 2, o);
    if (!strcmp(cmd, "add")) return add_paths(repo, cwd, argc, argv, o);
    if (!strcmp(cmd, "commit") && argc == 3 && !strcmp(argv[1], "-m") && argv[2][0])
        return commit_repository(repo, argv[2], o);
    if (!strcmp(cmd, "branch")) return branches(repo, argc, argv, o);
    if (!strcmp(cmd, "checkout") || !strcmp(cmd, "switch")) return switch_branch(repo, argc, argv, o);
    if (!strcmp(cmd, "remote")) return remotes(repo, argc, argv, o);
    if (!strcmp(cmd, "config") && argc == 3 &&
        (!strcmp(argv[1], "user.name") || !strcmp(argv[1], "user.email"))) {
        git_config *cfg = NULL;
        int rc = git_repository_config(&cfg, repo);
        if (!rc) rc = git_config_set_string(cfg, argv[1], argv[2]);
        git_config_free(cfg);
        return rc;
    }
    if (!strcmp(cmd, "fetch") && argc <= 2) return fetch_remote(repo, argc == 2 ? argv[1] : "origin", o);
    if (!strcmp(cmd, "pull") && argc <= 3) return pull_repository(repo, argc > 1 ? argv[1] : NULL, argc > 2 ? argv[2] : NULL, o);
    if (!strcmp(cmd, "push")) {
        bool track = argc > 1 && !strcmp(argv[1], "-u");
        int pos = track ? 2 : 1;
        if (argc > pos + 2) return fail("usage: git push [-u] [remote [branch]]");
        return push_repository(repo, argc > pos ? argv[pos] : "origin",
            argc > pos + 1 ? argv[pos + 1] : NULL, track, o);
    }
    if (!strcmp(cmd, "log")) {
        int count = 10;
        if (argc == 3 && !strcmp(argv[1], "-n")) {
            char *end;
            long parsed = strtol(argv[2], &end, 10);
            if (*end || parsed < 1 || parsed > 1000) return fail("log count must be 1..1000");
            count = (int)parsed;
        } else if (argc != 1) return fail("usage: git log [-n count]");
        return log_repository(repo, count, o);
    }
    return fail("unsupported command or arguments; run git help");
}

int solar_os_git_run(const char *cwd, int argc, char **argv,
                    const solar_os_git_options_t *o)
{
    if (o && o->error && o->error_size) o->error[0] = 0;
    if (!cwd || cwd[0] != '/' || argc < 1 || !argv) return -1;
    if (atomic_flag_test_and_set(&git_busy)) {
        if (o && o->error && o->error_size) snprintf(o->error, o->error_size, "Git is busy in another session");
        return -1;
    }
    git_allocator allocator = { .gmalloc = git_allocate, .grealloc = git_reallocate,
        .gfree = solar_os_memory_free };
    git_libgit2_opts(GIT_OPT_SET_ALLOCATOR, &allocator);
    int rc = git_libgit2_init();
    bool initialized = rc >= 0;
    git_repository *repo = NULL;
    if (initialized) {
        git_libgit2_opts(GIT_OPT_SET_MWINDOW_SIZE, (size_t)65536);
        git_libgit2_opts(GIT_OPT_SET_MWINDOW_MAPPED_LIMIT, (size_t)524288);
        git_libgit2_opts(GIT_OPT_SET_CACHE_MAX_SIZE, (ssize_t)262144);
#ifdef ESP_PLATFORM
        /* Mounted VFS volumes have no Unix user ownership. */
        git_libgit2_opts(GIT_OPT_SET_OWNER_VALIDATION, 0);
#endif
        rc = solar_os_git_transport_init(o);
    }
    if (!rc && cancelled(o)) rc = GIT_EUSER;
    if (!rc && (!strcmp(argv[0], "init") || !strcmp(argv[0], "clone")))
        rc = init_or_clone(cwd, argc, argv, o);
    else if (!rc) {
        rc = git_repository_open_ext(&repo, cwd, 0, NULL);
        if (!rc && git_repository_is_bare(repo)) rc = fail("a working tree is required");
        if (!rc) rc = dispatch(repo, cwd, argc, argv, o);
    }
    if (rc && o && o->error && o->error_size) {
        const git_error *error = git_error_last();
        snprintf(o->error, o->error_size, "%s", cancelled(o) ? "cancelled" :
                 error && error->message ? error->message : "Git operation failed");
    }
    git_repository_free(repo);
    if (initialized) {
        solar_os_git_transport_shutdown();
        git_libgit2_shutdown();
    }
    atomic_flag_clear(&git_busy);
    return rc;
}

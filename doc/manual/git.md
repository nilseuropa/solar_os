+++
id = "git"
title = "Git version control"
section = "network"
summary = "Version small projects locally and synchronize Git repositories over SSH or anonymous HTTPS"
aliases = ["version-control"]
keywords = "git clone branch diff commit pull push fetch github bitbucket ssh https"
packages_any = ["service_git"]
+++
# Git version control

Use `git` to version text files, Python scripts, Lua applications and other small
projects on device storage. The Git package is included in the full flavor on
boards with Wi-Fi and PSRAM. Local commands also work offline. Repositories use
the standard `.git` format and can also be opened by desktop Git. Commands work
in display and port shells.

## Clone a public project

Connect Wi-Fi and set the clock, then clone over HTTPS without signing in:

```text
git clone https://github.com/OWNER/PROJECT.git
cd PROJECT
git status
git log
```

Use the repository's final HTTPS URL, including `.git`. HTTPS verifies the server
certificate with the SolarOS certificate bundle. HTTPS authentication and HTTP
redirects are not supported. An HTTPS clone can later use SSH for authenticated
fetch and push:

```text
git remote set-url origin git@github.com:OWNER/PROJECT.git
```

## Set up SSH authentication

Generate a device key with `sshkey gen 3072`, then use `sshkey pub` to display its
public key. Add that public key to the GitHub or Bitbucket account that can access
the repository. GitHub requires at least 3072 bits for newly added RSA keys.
To replace an existing key pair, preserve a backup first, then run
`sshkey gen -f 3072` and register the new public key with the remote hosts you use.
Keep the private key on the device. Git uses the existing
`/.ssh/id_rsa`, `/.ssh/id_rsa.pub`, `/.ssh/hosts` aliases and `/.ssh/known_hosts`
policy. The first connection records a host key; a changed key is rejected.

```text
git clone git@github.com:OWNER/PROJECT.git
git clone git@bitbucket.org:WORKSPACE/PROJECT.git
git clone ssh://git@github.com:22/OWNER/PROJECT.git project
git clone -b feature git@github.com:OWNER/PROJECT.git project
```

SSH uses public-key authentication with the default device identity. HTTPS does
not request credentials. SSH aliases resolve hosts but do not supply usernames;
the default Git username is `git`. SSH URL paths must not contain single quotes,
percent escapes, backslashes or control characters.

## Save and review changes

Set the author identity separately in each repository:

```text
git config user.name "Your Name"
git config user.email "you@example.com"
git status
git diff
git add script.py README.txt
git diff --cached
git commit -m "Add temperature logger"
git push
```

`git add .` stages additions, modifications and deletions below the current
directory. Git honors `.gitignore`. `git diff` compares the working files to the
index; `git diff --cached` compares the index to the latest commit. Delete files
with `rm` and stage the deletion with `git add .`.

Start a new local project with `git init project`, enter it with `cd project`,
set the author identity, add files and make a commit. New repositories start on
`main`. To publish to an existing empty repository:

```text
git remote add origin git@github.com:OWNER/PROJECT.git
git push -u origin main
```

## Branches and synchronization

```text
git branch
git checkout -b experiment
git checkout main
git switch -c another-experiment
git branch -d experiment
git fetch
git pull
git pull origin main
git push origin main
```

`branch` lists local and fetched remote branches. `branch NAME` creates a branch
at the current commit. `checkout NAME` or `switch NAME` changes to a local branch,
or creates a tracking branch from `origin/NAME`. Branch deletion rejects the
current branch and commits that are not merged into the current branch.

Before the first commit, `checkout -b NAME` or `switch -c NAME` selects the initial
branch name and preserves existing files and staging. Status reports
`No commits yet`. Add files and create the first commit before pushing; the push
creates the remote branch. An empty clone also needs a first commit before push.

Fetch updates remote tracking branches without checking out files. Pull applies
fast-forward updates to the current branch and preserves conflicting local
changes. Diverged branches require a merge or rebase on a desktop. Pull without
an explicit branch uses the current branch's upstream. Push sends the current
branch to the same branch name on `origin` by default, or to the explicitly
named remote branch. `push -u` also sets the current branch's upstream for future
pulls. A push rejects non-fast-forward updates and reports remote
permission and branch protection failures. Force push is not supported.

## Limits

This client is intended for small projects. Incoming transfers are limited to
16 MiB and Git object caches are bounded. Available storage and PSRAM may impose
smaller limits. Repositories with long histories can exceed those limits even
when their working files are small. Diff treats files larger than 1 MiB as binary.
Symbolic links and submodules cannot be checked out. External attribute filters,
Git LFS, hooks, merge, rebase and stash are unavailable. Use ordinary files with
paths that fit the mounted filesystem; FAT volumes cannot represent filenames
that differ only in letter case or Unix executable permissions.

Press `Ctrl+]` to cancel. Network operations have a 30-second inactivity timeout.
Remote progress appears as it arrives during clone, fetch and push.
Cancelled transfers do not advance branch references. A failed clone may leave
its destination directory for inspection. Filesystem errors can leave
`.solaros-backup` files beside Git metadata; preserve them until the interrupted
write has been inspected. Metadata replacement recovers from ordinary rename
errors but is not atomic across sudden power loss.

## Quick reference

`git help` lists the supported command syntax. `git log -n 20` lists recent
commits. `git remote -v` lists configured remotes. Run commands inside a project;
Git discovers a parent repository when working in a subdirectory.

"""Git interoperability tests.

Run with SOLAR_OS_LIBGIT2_SOURCE_DIR pointing to the pinned libgit2 source.
SOLAR_OS_GIT_HOST_BUILD can reuse an existing host build of that source.
No external repositories or credentials are used by these tests.
"""
import os
from pathlib import Path
import socket
import shutil
import subprocess
import tempfile
import time
import unittest

REPO = Path(__file__).resolve().parents[1]


class GitClientTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        source = os.environ.get("SOLAR_OS_LIBGIT2_SOURCE_DIR")
        if not source:
            raise unittest.SkipTest("set SOLAR_OS_LIBGIT2_SOURCE_DIR for Git integration tests")
        cls.temp = tempfile.TemporaryDirectory()
        cls.work = Path(cls.temp.name)
        cls.source = Path(source)
        cls.build = Path(os.environ.get("SOLAR_OS_GIT_HOST_BUILD", cls.work / "build"))
        if not (cls.build / "libgit2.a").exists():
            subprocess.run(["cmake", "-S", str(cls.source), "-B", str(cls.build),
                "-DBUILD_SHARED_LIBS=OFF", "-DBUILD_TESTS=OFF", "-DBUILD_CLI=OFF",
                "-DUSE_SSH=OFF", "-DUSE_HTTPS=OFF", "-DREGEX_BACKEND=builtin"],
                check=True, capture_output=True)
            subprocess.run(["cmake", "--build", str(cls.build), "-j8"], check=True, capture_output=True)
        # Exercise the embedded regex workspace adaptation on the host too.
        # Replace only that upstream object in a private archive copy.
        generated = cls.work / "generated"
        generated.mkdir()
        subprocess.run(["python3", str(REPO / "components/solaros_git/patch_sources.py"),
                        str(cls.source), str(generated)], check=True)
        pcre_object = cls.work / "pcre2_compile.c.o"
        subprocess.run(["cc", "-std=c11", "-D_GNU_SOURCE", "-DHAVE_CONFIG_H",
            "-DPCRE2_CODE_UNIT_WIDTH=8", "-DPCRE2_STATIC", "-DPCRE2_EXPORT=",
            "-DPCRE2_EXP_DECL=", "-DPCRE2_EXP_DEFN=",
            "-I" + str(cls.build / "deps/pcre2"), "-I" + str(cls.source / "deps/pcre2"),
            "-include", str(REPO / "components/solaros_git/solar_os_git_port.h"),
            "-c", str(generated / "pcre2_compile.c"), "-o", str(pcre_object)], check=True)
        cls.library = cls.work / "libgit2-port.a"
        shutil.copyfile(cls.build / "libgit2.a", cls.library)
        subprocess.run(["ar", "r", str(cls.library), str(pcre_object)], check=True)
        # Exercise the embedded streaming buffers and non-threaded TLS key
        # reuse, including repeated init/shutdown in one process.
        for filename in ("filebuf.c", "futils.c", "blob.c", "odb.c", "filter.c", "thread.c"):
            obj = cls.work / (filename + ".o")
            flags = ["-DLIBGIT2_NO_FEATURES_H"] if filename == "thread.c" else []
            subprocess.run(["cc", "-std=c11", "-D_GNU_SOURCE", *flags,
                "-I" + str(cls.source / "include"), "-I" + str(cls.build / "include"),
                "-I" + str(cls.build / "gen_headers"), "-I" + str(cls.source / "src/util"),
                "-I" + str(cls.source / "src/libgit2"),
                "-c", str(generated / filename), "-o", str(obj)], check=True)
            subprocess.run(["ar", "r", str(cls.library), str(obj)], check=True)
        transport = (REPO / "src/services/solar_os_git_transport.c").read_text()
        parser = transport.split("static bool copy_part", 1)[1].split("typedef struct solar_os_git_ssh", 1)[0]
        (cls.work / "parser.c").write_text(
            '#include "solar_os_git_transport.h"\n#include <string.h>\n#include <ctype.h>\n'
            + "static bool copy_part" + parser)
        cls.binary = cls.work / "git-client"
        subprocess.run(["cc", "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-Wall", "-Wextra", "-Werror",
            "-I" + str(REPO / "src/services"), "-I" + str(REPO / "src"),
            "-I" + str(cls.source / "include"), "-I" + str(cls.build / "include"),
            str(REPO / "src/services/solar_os_git.c"), str(REPO / "tests/host/git_client_test.c"),
            str(cls.work / "parser.c"), str(cls.library), "-lz", "-lpthread", "-ldl",
            "-o", str(cls.binary)], check=True)
        (cls.work / "url.c").write_text(
            '#include "solar_os_git_transport.h"\n#include <stdio.h>\n'
            'int main(int n,char **v){solar_os_git_ssh_url_t u;'
            'if(n!=2||!solar_os_git_parse_ssh_url(v[1],&u))return 1;'
            'printf("%s|%s|%u|%s",u.username,u.host,u.port,u.path);return 0;}\n')
        cls.url_binary = cls.work / "url-parser"
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
            "-I" + str(REPO / "src/services"), "-I" + str(cls.source / "include"),
            "-I" + str(cls.build / "include"), str(cls.work / "parser.c"),
            str(cls.work / "url.c"), "-o", str(cls.url_binary)], check=True)
        (cls.work / "esp_random.h").write_text(
            '#include <stddef.h>\nstatic inline void esp_fill_random(void *p,size_t n)'
            '{unsigned char *b=p;while(n--)*b++=42;}\n')
        cls.port_binary = cls.work / "git-port"
        subprocess.run(["cc", "-std=c11", "-D_GNU_SOURCE", "-Wall", "-Wextra", "-Werror",
            "-I" + str(cls.work), "-I" + str(cls.source / "include"),
            "-I" + str(cls.build / "include"), "-I" + str(cls.build / "gen_headers"),
            "-I" + str(cls.source / "src/util"),
            "-include", str(REPO / "components/solaros_git/solar_os_git_port.h"),
            str(REPO / "components/solaros_git/solar_os_git_port.c"),
            str(REPO / "tests/host/git_port_test.c"), str(cls.library),
            "-lz", "-lpthread", "-ldl", "-o", str(cls.port_binary)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.root = Path(self.directory.name)
        self.project = self.root / "project"
        self.run_git(self.root, "init", "project")
        self.run_git(self.project, "config", "user.name", "SolarOS Test")
        self.run_git(self.project, "config", "user.email", "test@example.invalid")

    def tearDown(self):
        self.directory.cleanup()

    def test_repeated_commands_reuse_tls_slots(self):
        self.run_git(self.project, "__test_lifecycle")

    def run_git(self, cwd, *args, ok=True, env=None):
        result = subprocess.run([str(self.binary), str(cwd), *args], text=True, capture_output=True, env=env)
        if ok:
            self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        else:
            self.assertNotEqual(result.returncode, 0, result.stdout)
        return result.stdout + result.stderr

    def desktop(self, cwd, *args):
        return subprocess.run(["git", "-C", str(cwd), *args], check=True, text=True, capture_output=True).stdout

    def commit(self, text="print('hello')\n"):
        (self.project / "main.py").write_text(text)
        self.run_git(self.project, "add", ".")
        self.run_git(self.project, "commit", "-m", "Save script")

    def test_local_workflow_and_desktop_interoperability(self):
        self.commit()
        self.assertIn("working tree clean", self.run_git(self.project, "status"))
        (self.project / "main.py").write_text("print('updated')\n")
        self.assertIn("+print('updated')", self.run_git(self.project, "diff"))
        self.run_git(self.project, "add", "main.py")
        self.assertIn("+print('updated')", self.run_git(self.project, "diff", "--cached"))
        self.run_git(self.project, "commit", "-m", "Update script")
        self.assertIn("Update script", self.run_git(self.project, "log", "-n", "1"))
        self.assertIn("Update script", self.desktop(self.project, "log", "-1", "--format=%s"))
        self.desktop(self.project, "fsck", "--strict")
        self.assertEqual(self.desktop(self.project, "status", "--porcelain"), "")

    def test_fatfs_metadata_replacement_and_path_resolution(self):
        subprocess.run([str(self.port_binary), str(self.root)], check=True)

    def test_staging_deletion_ignore_and_subdirectory(self):
        self.commit()
        (self.project / ".gitignore").write_text("ignored.txt\n")
        (self.project / "ignored.txt").write_text("ignored")
        sub = self.project / "sub"
        sub.mkdir()
        (sub / "hello.txt").write_text("hello")
        self.run_git(sub, "add", ".")
        self.assertIn("sub/hello.txt", self.desktop(self.project, "diff", "--cached", "--name-only"))
        (self.project / "main.py").unlink()
        self.run_git(self.project, "add", ".")
        self.run_git(self.project, "commit", "-m", "Remove and add")
        files = self.desktop(self.project, "ls-files")
        self.assertNotIn("ignored.txt", files)
        self.assertNotIn("main.py", files)

    def test_branch_switch_protects_changes_and_unmerged_commits(self):
        self.commit()
        self.run_git(self.project, "checkout", "-b", "feature")
        self.commit("print('feature')\n")
        self.run_git(self.project, "switch", "main")
        self.assertIn("unmerged", self.run_git(self.project, "branch", "-d", "feature", ok=False))
        (self.project / "main.py").write_text("unsaved\n")
        before = self.desktop(self.project, "rev-parse", "HEAD")
        self.run_git(self.project, "checkout", "feature", ok=False)
        self.assertEqual((self.project / "main.py").read_text(), "unsaved\n")
        self.assertEqual(self.desktop(self.project, "rev-parse", "HEAD"), before)
        self.run_git(self.project, "branch", "-d", "main", ok=False)

    def test_traversal_cancellation_and_missing_identity(self):
        self.assertIn("nothing staged", self.run_git(self.project, "commit", "-m", "empty", ok=False))

    def test_regex_workspace_exhaustion_is_an_error(self):
        self.run_git(self.root, "__test_regex")
        result = self.run_git(self.root, "__test_regex", ok=False,
            env={**os.environ, "SOLAR_OS_GIT_TEST_WORKSPACE_OOM": "1"})
        self.assertTrue("memory" in result.lower() or "failed" in result.lower(), result)
        (self.root / "outside.txt").write_text("secret")
        self.run_git(self.project, "add", "../outside.txt", ok=False)
        self.run_git(self.project, "log", "-n", "0", ok=False)
        self.run_git(self.project, "status", env={**os.environ, "SOLAR_OS_GIT_TEST_CANCEL": "1"}, ok=False)
        self.commit()
        self.assertIn("nothing staged", self.run_git(self.project, "commit", "-m", "empty", ok=False))

    def test_ssh_urls_and_command_injection_rejection(self):
        cases = {
            "git@github.com:owner/project.git": "git|github.com|22|owner/project.git",
            "git@bitbucket.org:workspace/project.git": "git|bitbucket.org|22|workspace/project.git",
            "ssh://user@host.example:2222/project.git": "user|host.example|2222|/project.git",
            "github.com:owner/project.git": "git|github.com|22|owner/project.git",
        }
        for url, expected in cases.items():
            with self.subTest(url=url):
                result = subprocess.run([str(self.url_binary), url], text=True, capture_output=True)
                self.assertEqual(result.returncode, 0)
                self.assertEqual(result.stdout, expected)
        for url in ["https://github.com/a/b.git", "ssh://host:65536/a", "ssh://host:99999999999999/a",
                    "git@github.com:a';touch bad;'", "git@github.com:a\nb", "git@github.com:%27",
                    "-host:a", "git@host:-option", "ssh://host/", "git@host:"]:
            with self.subTest(url=url):
                self.assertNotEqual(subprocess.run([str(self.url_binary), url], capture_output=True).returncode, 0)

    def test_checkout_rejects_symlink(self):
        self.commit()
        self.desktop(self.project, "checkout", "-b", "links")
        (self.project / "link").symlink_to("main.py")
        self.desktop(self.project, "add", "link")
        self.desktop(self.project, "commit", "-m", "Add link")
        self.desktop(self.project, "checkout", "main")
        # Desktop may set core.symlinks to false from SolarOS config. The tree
        # still contains a link mode and must be rejected before materializing it.
        self.assertIn("unsupported symlink", self.run_git(self.project, "checkout", "links", ok=False))

    def test_clone_fetch_pull_push_pack_protocol(self):
        self.commit()
        bare = self.root / "origin.git"
        self.desktop(self.root, "clone", "--bare", str(self.project), str(bare))
        self.desktop(bare, "repack", "-ad")
        sock = socket.socket()
        sock.bind(("127.0.0.1", 0))
        port = sock.getsockname()[1]
        sock.close()
        daemon = subprocess.Popen(["git", "daemon", "--reuseaddr", "--export-all", "--enable=receive-pack",
            "--listen=127.0.0.1", f"--port={port}", f"--base-path={self.root}", str(self.root)],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            for _ in range(50):
                try:
                    with socket.create_connection(("127.0.0.1", port), timeout=.1): break
                except OSError: time.sleep(.02)
            url = f"git://127.0.0.1:{port}/origin.git"
            clone = self.root / "clone"
            self.run_git(self.root, "clone", url, "clone")
            self.run_git(clone, "config", "user.name", "Test")
            self.run_git(clone, "config", "user.email", "test@example.invalid")
            (clone / "new.txt").write_text("from SolarOS\n")
            self.run_git(clone, "add", ".")
            self.run_git(clone, "commit", "-m", "Push from SolarOS")
            self.run_git(clone, "push")
            self.assertEqual(self.desktop(bare, "show", "main:new.txt"), "from SolarOS\n")
            self.run_git(clone, "checkout", "-b", "topic")
            self.run_git(clone, "push", "-u", "origin", "topic")
            self.assertEqual(self.desktop(clone, "config", "--get", "branch.topic.remote").strip(), "origin")
            self.assertEqual(self.desktop(clone, "config", "--get", "branch.topic.merge").strip(), "refs/heads/topic")
            self.desktop(clone, "remote", "add", "backup", url)
            self.run_git(clone, "push", "-u", "backup", "topic")
            self.assertIn("up to date", self.run_git(clone, "pull"))
            self.run_git(clone, "checkout", "main")
            peer = self.root / "peer"
            self.desktop(self.root, "clone", str(bare), str(peer))
            self.desktop(peer, "config", "user.name", "Peer")
            self.desktop(peer, "config", "user.email", "peer@example.invalid")
            (peer / "main.py").write_text("print('remote change')\n")
            self.desktop(peer, "commit", "-am", "Remote update")
            self.desktop(peer, "push")
            self.run_git(clone, "pull")
            self.assertIn("remote change", (clone / "main.py").read_text())
            self.desktop(clone, "fsck", "--strict")
            (clone / "main.py").write_text("local dirty change\n")
            (peer / "main.py").write_text("conflicting remote change\n")
            self.desktop(peer, "commit", "-am", "Conflict")
            self.desktop(peer, "push")
            before = self.desktop(clone, "rev-parse", "HEAD")
            self.run_git(clone, "pull", ok=False)
            self.assertEqual((clone / "main.py").read_text(), "local dirty change\n")
            self.assertEqual(self.desktop(clone, "rev-parse", "HEAD"), before)
            self.run_git(clone, "push", ok=False)
            (clone / "main.py").write_text("local committed change\n")
            self.run_git(clone, "add", ".")
            self.run_git(clone, "commit", "-m", "Diverge locally")
            before = self.desktop(clone, "rev-parse", "HEAD")
            self.assertIn("diverged", self.run_git(clone, "pull", ok=False))
            self.assertEqual(self.desktop(clone, "rev-parse", "HEAD"), before)
            self.assertEqual((clone / "main.py").read_text(), "local committed change\n")
            hook = bare / "hooks/pre-receive"
            hook.write_text("#!/bin/sh\necho 'fixture rejects push' >&2\nexit 1\n")
            hook.chmod(0o755)
            self.assertIn("fixture rejects push", self.run_git(clone, "push", "origin", "denied", ok=False))
            self.assertNotIn("refs/heads/denied", self.desktop(bare, "show-ref"))
        finally:
            daemon.terminate()
            daemon.wait(timeout=5)


if __name__ == "__main__":
    unittest.main()

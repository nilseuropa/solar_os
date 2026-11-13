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
projects on device storage. Git is included in the full flavor on boards with
Wi-Fi and PSRAM. Local commands work offline, and repositories use the standard
`.git` format supported by desktop Git. Commands work in display and port shells.

## Clone a public project

Connect Wi-Fi and set the clock, then clone over HTTPS without signing in:

```text
git clone https://github.com/OWNER/PROJECT.git
cd PROJECT
git status
```

Use the final repository URL, including `.git`. The server certificate is
verified against the system certificate bundle. HTTPS authentication and HTTP
redirects are unsupported. To push to an HTTPS-cloned project, change its remote
to SSH:

```text
git remote set-url origin git@github.com:OWNER/PROJECT.git
```

An empty clone has no first commit. Add project files and commit them before
pushing, as described below.

## Set up SSH authentication

If no key is configured, create a device key and display its public key:

```text
sshkey gen 3072
sshkey pub
```

Add the public key to the GitHub or Bitbucket account that can access the
repository. Push requires write access. GitHub requires at least 3072 bits for
new RSA keys. If a key already exists, follow [SSH identity keys](ssh_keys.md)
to preserve or replace it. Keep the private key on the device.

Git uses the default `/.ssh/id_rsa` pair, SSH host aliases and known-host policy.
The first connection records the server's host key; a changed key is rejected.
SSH URLs default to username `git`; host aliases do not supply usernames.

```text
git clone git@github.com:OWNER/PROJECT.git
git clone git@bitbucket.org:WORKSPACE/PROJECT.git
git clone ssh://git@github.com:22/OWNER/PROJECT.git project
git clone -b feature git@github.com:OWNER/PROJECT.git project
```

SSH URL paths cannot contain single quotes, percent escapes, backslashes or
control characters.

## Create a local project

```text
git init project
cd project
```

New local repositories start on `main`. Cloned repositories may use another
branch name. Create or copy your project files into the directory, then make
the first commit using the commands below.

## Review and commit changes

Set the author name and email separately in each repository. Replace `script.py`
with the file or files you want to version:

```text
git config user.name "Your Name"
git config user.email "you@example.com"
git status
git diff
git add script.py
git diff --cached
git commit -m "Save project"
```

`git status` shows staged, modified and untracked files. `git diff` compares
working files to staging; `git diff --cached` compares staging to the latest
commit, or shows staged additions before the first commit.

`git add .` stages additions, modifications and deletions below the current
directory and honors `.gitignore`. Delete files with `rm`, then stage the
deletion with `git add .`. A first commit requires at least one staged file.

## Publish a branch

Create the repository on GitHub or Bitbucket first. For a new local project,
configure its SSH remote:

```text
git remote add origin git@github.com:OWNER/PROJECT.git
```

A clone already has an `origin` remote; use `git remote -v` to inspect it or
`git remote set-url origin URL` to change it.

After making a commit, create a branch and publish it:

```text
git checkout -b experiment
git push -u origin experiment
```

Push creates the remote branch if it does not exist. `-u` also sets the current
branch's upstream for future pulls. To publish the current branch, use
`git push -u origin`.

You can select an initial branch name before the first commit with
`git checkout -b NAME` or `git switch -c NAME`. This preserves files and staging;
status reports `No commits yet`. Make the first commit before pushing.

## Switch branches and synchronize

```text
git branch
git switch experiment
git fetch
git pull
git push
```

`branch` lists local and fetched remote branches. `branch NAME` creates a branch
at the current commit. `checkout NAME` or `switch NAME` selects a local branch,
or creates a tracking branch from `origin/NAME`. `checkout -b NAME` and
`switch -c NAME` create and select a branch. `branch -d NAME` deletes a local
branch, rejecting the current branch and branches with unmerged commits.

Fetch updates remote tracking branches without changing working files. Pull
uses the current branch's upstream unless given `git pull REMOTE BRANCH`.
It applies fast-forward updates and preserves conflicting local changes.
Diverged branches require a merge or rebase on a desktop.

Push sends the current branch to the same name on `origin` by default. Use
`git push REMOTE BRANCH` to send it to another remote branch name. Non-fast-forward
updates, missing permissions and branch protection are reported as errors.
Force push is unsupported.

## Limits and interrupted operations

Incoming transfers are limited to 16 MiB. Available storage and PSRAM may impose
smaller limits; a long history can exceed them even if the working files are
small. Diff treats files larger than 1 MiB as binary. Symbolic links and
submodules cannot be checked out. External attribute filters, Git LFS, hooks,
merge, rebase and stash are unavailable. FAT volumes cannot represent filenames
that differ only in letter case or Unix executable permissions.

Progress appears as it arrives during transfers. Press `Ctrl+]` to cancel;
network operations have a 30-second inactivity timeout. Cancelled transfers do
not advance branch references. A failed clone may leave its destination for
inspection; review it before retrying. Keep storage mounted and avoid powering
off during writes. If an error leaves `.solaros-backup` files beside Git metadata,
preserve them until the interrupted operation has been inspected.

## Quick reference

Run commands inside a project or a subdirectory; Git discovers the parent
repository. Use relative paths or full mounted paths such as `/sdcard/project`.
`git help` lists syntax, `git status` shows pending changes, and `git log -n 20`
lists recent commits. Set `user.name` and `user.email`, stage files with `git add`,
and make a commit before the first push. `checkout -b NAME` and `switch -c NAME`
create and select a branch; before the first commit they select its initial name.
`git push -u origin NAME` publishes the current branch and sets its upstream.
Pull is fast-forward only. HTTPS is anonymous; authenticated fetch and push use
SSH with the device's key. Press `Ctrl+]` to cancel.

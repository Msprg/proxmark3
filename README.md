# local-tools

Personal build/flash helpers for the proxmark3 fork. This orphan branch shares
no history with master and is never meant to be merged.

- `root/` — files symlinked into the root of a proxmark3 checkout
- `hooks/` — git hooks, symlinked into the repo's hooks dir
- `setup.sh` — creates the links, adds them to `.git/info/exclude`, installs the hooks

## Setup

From a clone of the fork's master (fresh, or `git reset --hard <fork>/master`):

```sh
./setup-local-tools.sh
```

It fetches this branch, checks it out at `../<repo dir>-tools`, and runs
`setup.sh` here. It warns about local branches that still track one of the
`root/` files; drop that commit from them or they keep their own copy.

The links are ignored via `.git/info/exclude`, so they survive branch switches.
If a checkout replaces or removes one (a branch tracking the same path,
`git clean -X`), `hooks/post-checkout` puts it back on the next checkout. Run
`setup.sh` again to restore them immediately.

## Editing

Edit through the links, then commit in the tools worktree:

```sh
git -C ../proxmark3-tools commit -am "..."
git -C ../proxmark3-tools push fork local-tools
```

A new file dropped into `root/` gets linked on the next `setup.sh` run or
checkout. After removing one from `root/`, delete its dangling link by hand.

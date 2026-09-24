#!/usr/bin/env bash
# Fork only: check out the local-tools branch as a sibling worktree and link its
# helpers into this checkout. Safe to re-run.
#
# usage: setup-local-tools.sh [worktree-dir]   (default ../<this dir>-tools)
set -euo pipefail
cd "$(dirname "$0")"
TOP=$(git rev-parse --show-toplevel)
BRANCH=local-tools

if ! git rev-parse -q --verify "refs/heads/$BRANCH" >/dev/null; then
    remote=
    for r in $(git remote); do
        if git ls-remote --exit-code --heads "$r" "$BRANCH" >/dev/null 2>&1; then
            remote=$r
            break
        fi
    done
    if [ -z "$remote" ]; then
        echo "no remote has a $BRANCH branch" >&2
        exit 1
    fi
    git fetch -q "$remote" "+refs/heads/$BRANCH:refs/remotes/$remote/$BRANCH"
    git branch -q --track "$BRANCH" "$remote/$BRANCH"
fi

DIR=$(git worktree list --porcelain |
      awk -v b="branch refs/heads/$BRANCH" '/^worktree /{w=substr($0, 10)} $0 == b {print w}')
if [ -z "$DIR" ]; then
    DIR=${1:-$TOP/../$(basename "$TOP")-tools}
    git worktree add -q "$DIR" "$BRANCH"
elif [ ! -x "$DIR/setup.sh" ]; then
    echo "$BRANCH is registered at missing $DIR, run: git worktree prune" >&2
    exit 1
fi

# a branch tracking a tool file gets that copy instead of the link
FILES=()
while IFS= read -r f; do
    FILES+=("$f")
done < <(git ls-tree -r --name-only "$BRANCH" root/ | sed 's|^root/||')
[ ${#FILES[@]} -gt 0 ] || exec "$DIR/setup.sh" "$TOP"
for b in $(git for-each-ref --format='%(refname:short)' refs/heads); do
    tracked=$(git ls-tree --name-only "$b" -- "${FILES[@]}" | tr "\n" " ")
    if [ -n "$tracked" ]; then
        echo "warning: branch $b tracks: $tracked" >&2
    fi
done

exec "$DIR/setup.sh" "$TOP"

#!/usr/bin/env bash
# Fork only: check out the local-tools branch as a sibling worktree and link its
# helpers into this checkout. Re-run to update: an existing worktree is
# fast-forwarded from its remote first.
#
# usage: setup-local-tools.sh [worktree-dir]   (default ../<this dir>-tools)
set -euo pipefail
cd "$(dirname "$0")"
TOP=$(git rev-parse --show-toplevel)
BRANCH=local-tools

FRESH=0
if ! git rev-parse -q --verify "refs/heads/$BRANCH" >/dev/null; then
    FRESH=1
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

# fast-forward the worktree from the branch's upstream; never fatal, the links are made anyway
update() {
    local remote ref up old
    remote=$(git config "branch.$BRANCH.remote" || true)
    ref=$(git config "branch.$BRANCH.merge" || true)
    if [ -z "$remote" ] || [ -z "$ref" ]; then
        echo "note: $BRANCH tracks no remote branch, not updated" >&2
        return 0
    fi
    up=refs/remotes/$remote/${ref#refs/heads/}
    if ! git fetch -q "$remote" "+$ref:$up" 2>/dev/null; then
        echo "warning: could not fetch $BRANCH from $remote, not updated" >&2
        return 0
    fi
    up=$(git rev-parse "$up")
    old=$(git rev-parse "refs/heads/$BRANCH")
    if [ "$old" = "$up" ]; then
        echo "local-tools: up to date (${old:0:9})"
    elif git merge-base --is-ancestor "$up" "$old"; then
        echo "note: $BRANCH has $(git rev-list --count "$up..$old") unpushed commit(s)" >&2
    elif ! git merge-base --is-ancestor "$old" "$up"; then
        echo "warning: $BRANCH and $remote have diverged, not updated (rebase in $DIR)" >&2
    elif git -C "$DIR" merge -q --ff-only "$up" 2>/dev/null; then
        echo "local-tools: updated ${old:0:9}..${up:0:9}"
    else
        echo "warning: $BRANCH not updated, local edits in $DIR are in the way (commit or stash them)" >&2
    fi
}
[ $FRESH = 1 ] || update

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

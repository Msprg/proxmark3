#!/usr/bin/env bash
# Link root/* into a proxmark3 checkout, hide the links from git, install hooks/*.
#
# usage: setup.sh [--link-only] [checkout]
#   checkout     defaults to the main worktree
#   --link-only  links + excludes only, no hooks (what hooks/post-checkout runs)
set -euo pipefail
# set when run from a hook, would redirect git -C
unset GIT_DIR GIT_WORK_TREE GIT_INDEX_FILE

TOOLS=$(dirname "$(readlink -f "$0")")
LINK_ONLY=0
if [ "${1:-}" = --link-only ]; then
    LINK_ONLY=1
    shift
fi
TARGET=${1:-$(git -C "$TOOLS" worktree list --porcelain | sed -n '1s/^worktree //p')}
TARGET=$(git -C "$TARGET" rev-parse --show-toplevel)
[ "$TARGET" = "$TOOLS" ] && exit 0

mapfile -t FILES < <(cd "$TOOLS/root" && find . \( -type f -o -type l \) | sed 's|^\./||' | sort)

link() {
    local rel dst
    for rel in "${FILES[@]}"; do
        dst=$TARGET/$rel
        # checkout has its own copy
        if git -C "$TARGET" ls-files --error-unmatch -- "$rel" >/dev/null 2>&1; then
            continue
        elif [ -L "$dst" ] || [ ! -e "$dst" ]; then
            mkdir -p "$(dirname "$dst")"
            ln -sfnr "$TOOLS/root/$rel" "$dst"
        else
            echo "local-tools: $dst is not a link, left alone" >&2
        fi
    done
}

exclude() {
    local file
    file=$(git -C "$TARGET" rev-parse --path-format=absolute --git-common-dir)/info/exclude
    mkdir -p "$(dirname "$file")"
    touch "$file"
    sed -i '/^# >>> local-tools/,/^# <<< local-tools/d' "$file"
    {
        echo "# >>> local-tools (managed by $TOOLS/setup.sh)"
        printf '/%s\n' "${FILES[@]}"
        echo "# <<< local-tools"
    } >> "$file"
}

hooks() {
    local dir h dst
    # honours core.hooksPath
    dir=$(git -C "$TARGET" rev-parse --path-format=absolute --git-path hooks)
    mkdir -p "$dir"
    for h in "$TOOLS"/hooks/*; do
        dst=$dir/$(basename "$h")
        if [ -e "$dst" ] && [ ! -L "$dst" ]; then
            echo "local-tools: $dst exists, not replaced" >&2
        else
            ln -sfn "$h" "$dst"
        fi
    done
}

exclude
link
[ $LINK_ONLY = 1 ] && exit 0
hooks
echo "local-tools: ${#FILES[@]} file(s) linked into $TARGET"

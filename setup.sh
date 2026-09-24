#!/usr/bin/env bash
# Link root/* into a proxmark3 checkout, hide the links from git, install hooks/*.
# Linux/WSL, macOS (bash 3.2, BSD tools) and MSYS2/ProxSpace.
#
# usage: setup.sh [--link-only] [checkout]
#   checkout     defaults to the main worktree
#   --link-only  links + excludes only, no hooks (what hooks/post-checkout runs)
set -euo pipefail
# set when run from a hook, would redirect git -C
unset GIT_DIR GIT_WORK_TREE GIT_INDEX_FILE

case $(uname -s) in
    # native symlinks or fail, never silent copies
    MINGW* | MSYS* | CYGWIN*) export MSYS=winsymlinks:nativestrict CYGWIN=winsymlinks:nativestrict ;;
esac

# physical path of $1 with symlinks followed (no readlink -f before macOS 12.3)
resolve() {
    local p=$1 l
    while [ -L "$p" ]; do
        l=$(readlink "$p")
        case $l in
            /*) p=$l ;;
            *) p=$(dirname "$p")/$l ;;
        esac
    done
    echo "$(cd "$(dirname "$p")" && pwd -P)/$(basename "$p")"
}

# $1 relative to dir $2, both physical; absolute if only / is shared (no ln -r on BSD)
relpath() {
    local to=$1 from=$2 up=
    while [ "$from" != / ] && [ "${to#"$from"/}" = "$to" ]; do
        from=$(dirname "$from")
        up=../$up
    done
    if [ "$from" = / ]; then
        echo "$to"
    else
        echo "$up${to#"$from"/}"
    fi
}

# git prints some paths relative to $TARGET
gitpath() {
    case $1 in
        /* | [A-Za-z]:*) echo "$1" ;;
        *) echo "$TARGET/$1" ;;
    esac
}

TOOLS=$(dirname "$(resolve "$0")")
LINK_ONLY=0
if [ "${1:-}" = --link-only ]; then
    LINK_ONLY=1
    shift
fi
TARGET=${1:-$(git -C "$TOOLS" worktree list --porcelain | sed -n '1s/^worktree //p')}
TARGET=$(cd "$(git -C "$TARGET" rev-parse --show-toplevel)" && pwd -P)
[ "$TARGET" = "$TOOLS" ] && exit 0

FILES=()
while IFS= read -r f; do
    FILES+=("$f")
done < <(cd "$TOOLS/root" && find . \( -type f -o -type l \) | sed 's|^\./||' | sort)
[ ${#FILES[@]} -gt 0 ] || exit 0

link() {
    local rel dst
    for rel in "${FILES[@]}"; do
        dst=$TARGET/$rel
        # checkout has its own copy
        if git -C "$TARGET" ls-files --error-unmatch -- "$rel" >/dev/null 2>&1; then
            continue
        elif [ -L "$dst" ] || [ ! -e "$dst" ]; then
            mkdir -p "$(dirname "$dst")"
            ln -sfn "$(relpath "$TOOLS/root/$rel" "$(cd "$(dirname "$dst")" && pwd -P)")" "$dst"
        else
            echo "local-tools: $dst is not a link, left alone" >&2
        fi
    done
}

exclude() {
    local file
    file=$(gitpath "$(git -C "$TARGET" rev-parse --git-common-dir)")/info/exclude
    mkdir -p "$(dirname "$file")"
    touch "$file"
    # sed -i differs between GNU and BSD
    awk '/^# >>> local-tools/ {skip = 1} !skip {print} /^# <<< local-tools/ {skip = 0}' "$file" > "$file.tmp"
    {
        echo "# >>> local-tools (managed by $TOOLS/setup.sh)"
        printf '/%s\n' "${FILES[@]}"
        echo "# <<< local-tools"
    } >> "$file.tmp"
    mv "$file.tmp" "$file"
}

hooks() {
    local dir h dst
    # honours core.hooksPath
    dir=$(gitpath "$(git -C "$TARGET" rev-parse --git-path hooks)")
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

#!/bin/bash
# Builds t/vtbench against revisions of the library and compares the instructions they need.
#
#   t/vtbench-compare.sh [--mib N] REV [REV ...]
#
# REV is anything git understands (a commit, a tag, a branch) or WORKTREE for the files as they are now.
# The first one is the reference. For example, what the uncommitted changes cost since the last commit:
#
#   t/vtbench-compare.sh HEAD WORKTREE
#
# Needs gcc, git and valgrind. Works in a temporary directory, nothing in the repository changes.
set -e
here="$(cd "$(dirname "$0")/.." && pwd)"
mib=2
if [ "$1" = "--mib" ]; then mib="$2"; shift 2; fi
[ $# -ge 1 ] || { sed -n '2,11p' "$0" | sed 's/^# \{0,1\}//'; exit 1; }

tmp="$(mktemp -d)"
trap '[ -n "$tmp" ] && rm -rf -- "$tmp"' EXIT

builds=()
n=0
for rev in "$@"; do
  n=$((n + 1))
  dir="$tmp/$n"
  mkdir -p "$dir"
  if [ "$rev" = "WORKTREE" ]; then
    cp -r "$here/src" "$here/include" "$dir/"
  else
    git -C "$here" archive "$rev" src include | tar -x -C "$dir"
  fi
  defs=()
  grep -q vterm_screen_rewrap_scrollback "$dir/include/vterm.h" || defs+=(-DNO_REWRAP)
  gcc -O3 -flto=auto -DNDEBUG -std=gnu99 -w "${defs[@]}" -I"$dir/include" -I"$dir/src" \
      "$here/t/vtbench.c" "$dir"/src/[a-z]*.c -o "$dir/vtbench"
  builds+=("$rev=$dir/vtbench")
done

python3 "$here/t/vtbench-icount.py" --mib "$mib" "${builds[@]}"

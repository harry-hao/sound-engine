#!/bin/sh
set -eu

cd "$(dirname "$0")/.."
: "${CLANG_FORMAT:=clang-format}"

command -v "$CLANG_FORMAT" >/dev/null 2>&1 || {
    echo "error: $CLANG_FORMAT is required (set CLANG_FORMAT to override)" >&2
    exit 1
}

find . -path ./.git -prune -o \( -name '*.c' -o -name '*.h' \) -type f -print0 |
    xargs -0 "$CLANG_FORMAT" --style=file --dry-run --Werror

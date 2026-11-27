#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# One product version (ADR-011): fails when include/qmp/qmp_version.h, the generator and
# CHANGELOG.md disagree. With a tag name as the argument (the release workflow passes
# "$GITHUB_REF_NAME"), the tag must be "v<version>" too.
#
#   scripts/check-version.sh            # consistency inside the tree
#   scripts/check-version.sh v0.3.0     # ... and against a release tag
#   scripts/check-version.sh --print    # print the version and exit
set -eu
cd "$(dirname "$0")/.."

header=include/qmp/qmp_version.h
part() {
    v=$(sed -n "s/^#define QMP_VERSION_$1 \([0-9][0-9]*\)\$/\1/p" "$header")
    [ -n "$v" ] || { echo "check-version: QMP_VERSION_$1 not found in $header" >&2; exit 1; }
    echo "$v"
}
version="$(part MAJOR).$(part MINOR).$(part PATCH)"

if [ "${1:-}" = "--print" ]; then
    echo "$version"
    exit 0
fi

rc=0
fail() { echo "check-version: $*" >&2; rc=1; }

gen=$(sed -n 's/^__version__ = "\(.*\)"$/\1/p' codegen/protoc_gen_qmp/__init__.py)
[ "$gen" = "$version" ] ||
    fail "codegen/protoc_gen_qmp/__init__.py says '$gen', $header says '$version'"

# The newest released section of the changelog is the first "## [x.y.z]" heading.
log=$(sed -n 's/^## \[\([0-9][0-9.]*\)\] - [0-9-]*$/\1/p' CHANGELOG.md | head -n 1)
[ "$log" = "$version" ] ||
    fail "the newest release in CHANGELOG.md is '$log', $header says '$version'"
grep -q '^## \[Unreleased\]$' CHANGELOG.md || fail "CHANGELOG.md has no [Unreleased] section"

if [ -n "${1:-}" ] && [ "$1" != "v$version" ]; then
    fail "tag '$1' does not match v$version from $header"
fi

[ "$rc" -eq 0 ] && echo "check-version: $version ok${1:+ (tag $1)}"
exit "$rc"

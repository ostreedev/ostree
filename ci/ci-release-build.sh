#!/usr/bin/env bash
set -euo pipefail

# Makes sure that is_release_build is only set to yes in a release commit. A
# release commit must be titled: "Release $MAJOR.$MINOR". Also checks that the
# release version in the build system matches the commit msg.

# if running under PAPR, use the branch/PR HEAD actually
# being tested rather than the merge sha
HEAD=${PAPR_COMMIT:-HEAD}

git log --format=%B -n 1 $HEAD > log.txt
trap "rm -f log.txt" EXIT

if grep -q ^is_release_build=yes configure.ac; then
    echo "*** is_release_build is set to yes ***"

    V=$(grep -Po '^#define PACKAGE_VERSION "\K[0-9]+\.[0-9]+(?=")' config.h)
    if [ -z "$V" ]; then
        echo "ERROR: couldn't read PACKAGE_VERSION"
        exit 1
    fi
    echo "OK: release version is $V"

    # check if the commit title indicates a release and has the correct version
    if ! grep -q "^Release $V" log.txt; then
        echo "ERROR: release commit doesn't match version"
        echo "Commit message:"
        cat log.txt
        echo "Build version: $V"
        exit 1
    fi
    echo "OK: release commit matches version"

    if grep -q "^LIBOSTREE_$V" src/libostree/libostree-devel.sym; then
        echo "ERROR: devel syms still references release version"
        exit 1
    fi
    echo "OK: devel syms no longer reference release version"

else
    echo "*** is_release_build is set to no ***"

    if grep -qE "^Release [0-9]+\.[0-9]+" log.txt; then
        echo "ERROR: release commit does not have is_release_build=yes"
        exit 1
    fi
    echo "OK: commit is not a release"
fi

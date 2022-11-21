#!/bin/bash
set -xeuo pipefail

TOP=$(git rev-parse --show-toplevel)
GITREV=$(git rev-parse HEAD)
gitdescribe=$(git describe --always --tags --match 'v2???.*' $GITREV)
version=$(echo "$gitdescribe" | sed -e 's,-,\.,g' -e 's,^v,,')
name=libostree
PKG_VER="${name}-${version}"

TARFILE=${PKG_VER}.tar
TARFILE_TMP=${TARFILE}.tmp

if ! test -f ${TOP}/libglnx/README.md || ! test -f ${TOP}/bsdiff/README.md; then
    git submodule update --init
fi

echo "Archiving ${PKG_VER} at ${GITREV} to ${TARFILE_TMP}"
(cd ${TOP}; git archive --format=tar --prefix=${PKG_VER}/ ${GITREV}) > ${TARFILE_TMP}
ls -al ${TARFILE_TMP}
(cd ${TOP}; git submodule status) | while read line; do
    rev=$(echo ${line} | cut -f 1 -d ' '); path=$(echo ${line} | cut -f 2 -d ' ')
    echo "Archiving ${path} at ${rev}"
    (cd ${TOP}/${path}; git archive --format=tar --prefix=${PKG_VER}/${path}/ ${rev}) > submodule.tar
    tar -A -f ${TARFILE_TMP} submodule.tar
    rm submodule.tar
done
mv ${TARFILE_TMP} ${TARFILE}
xz "${TARFILE}"

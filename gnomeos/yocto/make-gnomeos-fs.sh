# Copyright (C) 2011 Colin Walters <walters@verbum.org>
#

set -e
set -x

if test $(id -u) = 0; then
    cat <<EOF
This script should not be run as root.
EOF
    exit 1
fi


mkdir gnomeos-fs
cd gnomeos-fs


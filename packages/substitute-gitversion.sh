#!/bin/bash

set -e

git_version=$(git describe)
git_version_rpm=$(echo ${git_version} | sed -e 's,-,\.,g' -e 's,^v,,')
exec sed -e "s,^Version:.*,Version: ${git_version_rpm}," "$@"

#!/bin/bash
# Fix the spec file for Packit COPR builds.
# Called from .packit.yaml create-archive action.
set -xeuo pipefail

spec="$1"
version=$(git describe --always --tags --match 'v2???.*' | sed -e 's,-,\.,g' -e 's,^v,,')
sed -i "s,^Version:.*,Version: ${version}," "$spec"
sed -i 's/^Patch/# Patch/g' "$spec"
sed -i 's,%autorelease,1%{?dist},g' "$spec"

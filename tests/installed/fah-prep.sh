#!/bin/bash
set -xeuo pipefail
# If we're using devmapper, expand the root
if lvm lvs atomicos/docker-pool &>/dev/null; then
    systemctl stop docker
    lvm lvremove -f atomicos/docker-pool
fi
lvm lvextend -r -l +100%FREE atomicos/root
ostree admin unlock
rsync -rlv ./ostree/insttree/usr/ /usr/

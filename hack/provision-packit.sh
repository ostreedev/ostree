#!/bin/bash
# Provision a Packit package-mode VM into an ostree image-mode system.
#
# This script:
# 1. Creates a COPR repo file from the Packit test artifacts
# 2. Builds a bootc container image with the updated ostree RPMs
# 3. Runs bootc install to-filesystem --replace=alongside
#
# After this script completes, the TMT plan reboots into image mode.
set -exuo pipefail

OSTREE_TEMPDIR=$(mktemp -d)
trap 'rm -rf -- "$OSTREE_TEMPDIR"' EXIT

# Copy files in hack to OSTREE_TEMPDIR
cp -a . "$OSTREE_TEMPDIR"

# Keep testing farm run folder
cp -r /var/ARTIFACTS "$OSTREE_TEMPDIR"

# Copy ostree repo
cp -r /var/share/test-artifacts "$OSTREE_TEMPDIR"

# Some rhts-*, rstrnt-* and tmt-* commands are in /usr/local/bin
if [[ -d /var/lib/tmt/scripts ]]; then
    cp -r /var/lib/tmt/scripts "$OSTREE_TEMPDIR"
    ls -al "${OSTREE_TEMPDIR}/scripts"
else
    cp -r /usr/local/bin "$OSTREE_TEMPDIR"
    ls -al "${OSTREE_TEMPDIR}/bin"
fi

ARCH=$(uname -m)
source /etc/os-release

# Determine the base bootc image
case "${ID}-${VERSION_ID}" in
    centos-9)   BASE=quay.io/centos-bootc/centos-bootc:stream9 ;;
    centos-10)  BASE=quay.io/centos-bootc/centos-bootc:stream10 ;;
    fedora-*)   BASE=quay.io/fedora/fedora-bootc:${VERSION_ID} ;;
    *)          echo "Unsupported OS: ${ID}-${VERSION_ID}" >&2; exit 1 ;;
esac


if [[ "$ID" == "rhel" ]]; then
    # OSCI gating only
    CURRENT_COMPOSE_ID=$(skopeo inspect --no-tags --retry-times=5 --tls-verify=false "docker://${BASE}" | jq -r '.Labels."redhat.compose-id"')

    if [[ -n ${CURRENT_COMPOSE_ID} ]]; then
        if [[ ${CURRENT_COMPOSE_ID} == *-updates-* ]]; then
            BATCH_COMPOSE="updates/"
        else
            BATCH_COMPOSE=""
        fi
    else
        BATCH_COMPOSE="updates/"
        CURRENT_COMPOSE_ID=latest-RHEL-$VERSION_ID
    fi

    # use latest compose if specific compose is not accessible
    RC=$(curl -skIw '%{http_code}' -o /dev/null "http://${NIGHTLY_COMPOSE_SITE}/rhel-${VERSION_ID%%.*}/nightly/${BATCH_COMPOSE}RHEL-${VERSION_ID%%.*}/${CURRENT_COMPOSE_ID}/STATUS")
    if [[ $RC != "200" ]]; then
        CURRENT_COMPOSE_ID=latest-RHEL-${VERSION_ID%%}
    fi

    # generate rhel repo
    tee "${OSTREE_TEMPDIR}/rhel.repo" >/dev/null <<REPOEOF
[rhel-baseos]
name=baseos
baseurl=http://${NIGHTLY_COMPOSE_SITE}/rhel-${VERSION_ID%%.*}/nightly/${BATCH_COMPOSE}RHEL-${VERSION_ID%%.*}/${CURRENT_COMPOSE_ID}/compose/BaseOS/${ARCH}/os/
enabled=1
gpgcheck=0

[rhel-appstream]
name=appstream
baseurl=http://${NIGHTLY_COMPOSE_SITE}/rhel-${VERSION_ID%%.*}/nightly/${BATCH_COMPOSE}RHEL-${VERSION_ID%%.*}/${CURRENT_COMPOSE_ID}/compose/AppStream/${ARCH}/os/
enabled=1
gpgcheck=0
REPOEOF
    cp "${OSTREE_TEMPDIR}/rhel.repo" /etc/yum.repos.d
fi

ls -al /etc/yum.repos.d
cat /etc/yum.repos.d/test-artifacts.repo
ls -al /var/share/test-artifacts

# The Packit COPR repo is already configured on the test VM.
# Copy it for use inside the container build.
cp /etc/yum.repos.d/test-artifacts.repo "$OSTREE_TEMPDIR/"

# Let's check things in hack folder
ls -al "$OSTREE_TEMPDIR"

# Build the container image with updated ostree RPMs
podman build --jobs=4 --from "$BASE" \
    -t localhost/ostree:latest \
    -v "$OSTREE_TEMPDIR":/ostree-test:z \
    -f "$OSTREE_TEMPDIR/Containerfile.packit" \
    "$OSTREE_TEMPDIR"

# Install to the running system (will be activated after reboot)
podman run \
    --env BOOTC_SKIP_SELINUX_HOST_CHECK=1 \
    --rm -ti --privileged \
    -v /:/target --pid=host --security-opt label=disable \
    -v /dev:/dev -v /var/lib/containers:/var/lib/containers \
    localhost/ostree:latest \
    bootc install to-filesystem --skip-fetch-check --replace=alongside /target

echo "Provisioning complete. System will reboot into image mode."

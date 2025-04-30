#!/bin/bash
# https://github.com/ostreedev/ostree/issues/1667
set -xeuo pipefail

. ${KOLA_EXT_DATA}/libinsttest.sh

# we don't just use `rpm-ostree initramfs-etc` here because we want to be able
# to test more things

# dracut prints all the cmdline args, including those from /etc/cmdline.d, so
# the way we test that an initrd was included is to just add kargs there and
# grep for it
create_initrd_with_dracut_karg() {
  local karg=$1; shift
  local d
  d=$(mktemp -dp /var/tmp)
  mkdir -p "${d}/etc/cmdline.d"
  echo "${karg}" > "${d}/etc/cmdline.d/${karg}.conf"
  echo "etc/cmdline.d/${karg}.conf" | \
    cpio -D "${d}" -o -H newc --reproducible > "/var/tmp/${karg}.img"
}

check_for_dracut_karg() {
  local karg=$1; shift
  # https://github.com/dracutdevs/dracut/blob/38ea7e821b/modules.d/98dracut-systemd/dracut-cmdline.sh#L17
  journalctl -b 0 -t dracut-cmdline \
    --grep "Using kernel command line parameters:.* ${karg} "
}

case "${AUTOPKGTEST_REBOOT_MARK:-}" in
  "")
    create_initrd_with_dracut_karg ostree.test1
    # let's use the deploy API first
    ostree admin deploy "${host_commit}" \
      --overlay-initrd /var/tmp/ostree.test1.img
    /tmp/autopkgtest-reboot "2"
    ;;
  2)
    # verify that ostree.test1 is here
    check_for_dracut_karg ostree.test1
    img_sha=$(sha256sum < /var/tmp/ostree.test1.img | cut -f 1 -d ' ')
    test -f "/boot/ostree/initramfs-overlays/${img_sha}.img"

    # now let's change to ostree.test2
    create_initrd_with_dracut_karg ostree.test2

    # let's use the staging API this time
    ostree admin deploy "${host_commit}" --stage \
      --overlay-initrd /var/tmp/ostree.test2.img
    /tmp/autopkgtest-reboot "3"
    ;;
  3)
    # verify that ostree.test1 is gone, but ostree.test2 is here
    if check_for_dracut_karg ostree.test1; then
      assert_not_reached "Unexpected ostree.test1 karg found"
    fi
    check_for_dracut_karg ostree.test2

    # both the new and old initrds should still be there since they're
    # referenced in the BLS
    test1_sha=$(sha256sum < /var/tmp/ostree.test1.img | cut -f 1 -d ' ')
    test2_sha=$(sha256sum < /var/tmp/ostree.test2.img | cut -f 1 -d ' ')
    test -f "/boot/ostree/initramfs-overlays/${test1_sha}.img"
    test -f "/boot/ostree/initramfs-overlays/${test2_sha}.img"

    # OK, now let's deploy an identical copy of this test
    ostree admin deploy "${host_commit}" \
      --overlay-initrd /var/tmp/ostree.test2.img

    # Now the deployment with ostree.test1 should've been GC'ed; check that its
    # initrd was cleaned up
    test ! -f "/boot/ostree/initramfs-overlays/${test1_sha}.img"
    test -f "/boot/ostree/initramfs-overlays/${test2_sha}.img"

    # deploy again to check that no bootconfig swap was needed; this verifies
    # that deployment overlay initrds can be successfully compared
    ostree admin deploy "${host_commit}" \
      --overlay-initrd /var/tmp/ostree.test2.img |& tee /tmp/out.txt
    assert_file_has_content /tmp/out.txt 'bootconfig swap: no'

    # finally, let's check that we can overlay multiple initrds
    ostree admin deploy "${host_commit}" --stage \
      --overlay-initrd /var/tmp/ostree.test1.img \
      --overlay-initrd /var/tmp/ostree.test2.img
    /tmp/autopkgtest-reboot "4"
    ;;
  4)
    check_for_dracut_karg ostree.test1
    check_for_dracut_karg ostree.test2
    test1_sha=$(sha256sum < /var/tmp/ostree.test1.img | cut -f 1 -d ' ')
    test2_sha=$(sha256sum < /var/tmp/ostree.test2.img | cut -f 1 -d ' ')
    test -f "/boot/ostree/initramfs-overlays/${test1_sha}.img"
    test -f "/boot/ostree/initramfs-overlays/${test2_sha}.img"
    ;;
  *) fatal "Unexpected AUTOPKGTEST_REBOOT_MARK=${AUTOPKGTEST_REBOOT_MARK}" ;;
esac

#!/bin/bash
set -xeu

# booted VM provision

cloudinit=0
case ${1:-} in
  cloudinit) cloudinit=1 ;;
  "") ;;
  *) echo "Unhandled flag: ${1:-}" 1>&2; exit 1 ;;
esac

# Extra packages we install
grep -Ev -e '^#' packages.txt | xargs dnf -y install

# Cloud bits
cat <<KARGEOF >> /usr/lib/bootc/kargs.d/20-console.toml
kargs = ["console=ttyS0,115200n8"]
KARGEOF
if test $cloudinit = 1; then
  dnf -y install cloud-init
  ln -s ../cloud-init.target /usr/lib/systemd/system/default.target.wants
  # Allow root SSH login for testing with bcvk/tmt
  mkdir -p /etc/cloud/cloud.cfg.d
  cat > /etc/cloud/cloud.cfg.d/80-enable-root.cfg <<'CLOUDEOF'
# Enable root login for testing
disable_root: false

# In image mode, the host root filesystem is mounted at /sysroot, not /
# That is the one we should attempt to resize, not what is mounted at /
growpart:
  mode: auto
  devices: ["/sysroot"]
resize_rootfs: false
CLOUDEOF
fi

dnf clean all
# Stock extra cleaning of logs and caches in general (mostly dnf)
rm /var/log/* /var/cache /var/lib/{dnf,rpm-state,rhsm} -rf
# And clean root's homedir
rm /var/roothome/.config -rf
cat >/usr/lib/tmpfiles.d/bootc-cloud-init.conf <<'EOF'
d /var/lib/cloud 0755 root root - -
EOF



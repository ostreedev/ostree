#
# Copyright (C) 2011 Red Hat, Inc.
#
IMAGE_LINGUAS = " "

LICENSE = "LGPL2"

inherit core-image

IMAGE_INSTALL += "ostree-init"

# remove not needed ipkg informations
ROOTFS_POSTPROCESS_COMMAND += "remove_packaging_data_files ; "

gnomeos_rootfs_postinst() {
	echo "GNOME OS Unix login" > ${IMAGE_ROOTFS}/etc/issue
}

ROOTFS_POSTPROCESS_COMMAND += " gnomeos_rootfs_postinst ; "

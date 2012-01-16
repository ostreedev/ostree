SUMMARY = "Root switcher"
LICENSE = "GPLv2+"
LIC_FILES_CHKSUM = "file://COPYING;md5=94d55d512a9ba36caa9b7df079bae19f"

SRC_URI = "file:///src/ostree-init"
S = "${WORKDIR}/ostree-init"

inherit autotools

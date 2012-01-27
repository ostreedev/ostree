SUMMARY = "GNOME OS management tool"
LICENSE = "GPLv2+"
LIC_FILES_CHKSUM = "file://COPYING;md5=97285cb818cf231e6a36f72c82592235"

SRC_URI = "file:///src/ostree"
S = "${WORKDIR}/ostree"

RDEPENDS += "libarchive"

inherit autotools

EXTRA_OECONF = "--without-soup-gnome --with-libarchive"

FILES_${PN} += "${libdir}/ostree/ ${libdir}/ostbuild"

BBCLASSEXTEND = "native"


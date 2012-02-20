SUMMARY = "GNOME OS management tool"
LICENSE = "GPLv2+"
LIC_FILES_CHKSUM = "file://COPYING;md5=97285cb818cf231e6a36f72c82592235"

SRC_URI = "git://git.gnome.org/ostree;tag=00ad0a2ea7965de5852e35395fcfd9c9df4ebc2a"
S = "${WORKDIR}/git"

DEPENDS += "libarchive glib-2.0"

inherit autotools

EXTRA_OECONF = "--without-soup-gnome --with-libarchive"

FILES_${PN} += "${libdir}/ostree/ ${libdir}/ostbuild"

BBCLASSEXTEND = "native"


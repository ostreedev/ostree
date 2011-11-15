SUMMARY = "GNOME OS management tool"
LICENSE = "GPLv2+"
LIC_FILES_CHKSUM = "file://COPYING;md5=97285cb818cf231e6a36f72c82592235"

SRC_URI = "git://git.gnome.org/ostree;tag=18f0b537a45f12852e4ec6b174440cbfe7702e4d"
S = "${WORKDIR}/git"

inherit autotools

EXTRA_OECONF = "--without-soup-gnome"

BBCLASSEXTEND = "native"


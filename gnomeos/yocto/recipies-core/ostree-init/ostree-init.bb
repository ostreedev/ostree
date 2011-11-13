SUMMARY = "Root switcher"
LICENSE = "GPLv2+"
LIC_FILES_CHKSUM = "file://COPYING;md5=94d55d512a9ba36caa9b7df079bae19f"

SRC_URI = "file://ostree-init.c \
	   file://Makefile.in \
	   file://configure \
	   file://COPYING"

S = "${WORKDIR}"

inherit autotools

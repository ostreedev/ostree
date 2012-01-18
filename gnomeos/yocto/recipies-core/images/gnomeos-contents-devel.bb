#
# Copyright (C) 2011 Colin Walters <walters@verbum.org>
#
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COREBASE}/LICENSE;md5=3f40d7994397109285ec7b81fdeb3b58 \
                    file://${COREBASE}/meta/COPYING.MIT;md5=3da9cfbcb788c80a0384361b4de20420"

inherit gnomeos-contents

RECIPE_PACKAGES += "task-core-sdk \
		python-dev \
		bison flex \
		git \
		gdb \
		libxml-parser-perl \
		gettext-dev \
		"

IMAGE_INSTALL += "libuuid-dev \
		  libblkid-dev \
		  libpam-dev \
		  libtiff-dev \
		  libjpeg-dev \
		  libltdl-dev \
		  libsndfile-dev \
		  libatomics-ops-dev \
		  libogg-dev \
		  speex-dev \
		  libvorbis-dev \
		  libstdc++-dev \
		  libcap-dev \
		  libcap-bin \
		  libgpg-error-dev \
		  libtasn1-dev \
		  libtasn1-bin \
		  libgcrypt-dev \
		  libgnutls-dev \
		  icu-dev \
		  "

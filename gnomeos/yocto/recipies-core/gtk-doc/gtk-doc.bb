LICENSE = "GPLv2+"
LIC_FILES_CHKSUM = "file://COPYING-DOCS;md5=18ba770020b624031bc7c8a7b055d776"

DEPENDS = "perl-native"

SRC_URI = "git://git.gnome.org/gtk-doc;tag=GTK_DOC_1_18"
S = "${WORKDIR}/git"

inherit autotools gettext

BBCLASSEXTEND = "native"

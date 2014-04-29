AC_PREREQ([2.63])
AC_INIT([ostree], [2014.4], [walters@verbum.org])
AC_CONFIG_HEADER([config.h])
AC_CONFIG_MACRO_DIR([m4])
AC_CONFIG_AUX_DIR([build-aux])

AM_INIT_AUTOMAKE([1.11 -Wno-portability foreign no-define tar-ustar no-dist-gzip dist-xz])
AM_MAINTAINER_MODE([enable])
AM_SILENT_RULES([yes])
AC_USE_SYSTEM_EXTENSIONS
AC_SYS_LARGEFILE

AC_PROG_CC
AM_PROG_CC_C_O

changequote(,)dnl
if test "x$GCC" = "xyes"; then
  WARN_CFLAGS="-Wall -Wstrict-prototypes -Werror=missing-prototypes \
		-Werror=implicit-function-declaration \
		-Werror=pointer-arith -Werror=init-self -Werror=format=2 \
		-Werror=format-security \
		-Werror=missing-include-dirs -Werror=aggregate-return \
		-Werror=declaration-after-statement"
fi
changequote([,])dnl
AC_SUBST(WARN_CFLAGS)

# Initialize libtool
LT_PREREQ([2.2.4])
LT_INIT([disable-static])

OSTREE_FEATURES=""
AC_SUBST([OSTREE_FEATURES])

AC_ARG_ENABLE(installed_tests,
              AS_HELP_STRING([--enable-installed-tests],
                             [Install test programs (default: no)]),,
              [enable_installed_tests=no])
AM_CONDITIONAL(BUILDOPT_INSTALL_TESTS, test x$enable_installed_tests = xyes)

AC_CHECK_HEADER([attr/xattr.h],,[AC_MSG_ERROR([You must have attr/xattr.h from libattr])])

PKG_PROG_PKG_CONFIG

GIO_DEPENDENCY="gio-unix-2.0 >= 2.34.0 libgsystem >= 2014.2"
PKG_CHECK_MODULES(OT_DEP_GIO_UNIX, $GIO_DEPENDENCY)

dnl 5.1.0 is an arbitrary version here
PKG_CHECK_MODULES(OT_DEP_LZMA, liblzma >= 5.1.1alpha)

SOUP_DEPENDENCY="libsoup-2.4 >= 2.39.1"
AC_ARG_WITH(soup,
	    AS_HELP_STRING([--with-soup], [Use libsoup @<:@default=yes@:>@]),
	    [], [with_soup=check])
AS_IF([test x$with_soup != xno ], [
  AC_MSG_CHECKING([for $SOUP_DEPENDENCY])
  PKG_CHECK_EXISTS($SOUP_DEPENDENCY, have_soup=yes, have_soup=no)
  AC_MSG_RESULT([$have_soup])
    AS_IF([ test x$have_soup = xno && test x$with_soup != xcheck], [
       AC_MSG_ERROR([libsoup is enabled but could not be found])
  ])
  AS_IF([test x$have_soup = xyes], [
    PKG_CHECK_MODULES(OT_DEP_SOUP, $SOUP_DEPENDENCY)
    AC_DEFINE(HAVE_LIBSOUP, 1, [Define if we have libsoup.pc])
    with_soup=yes
  ], [
    with_soup=no
  ])
], [ with_soup=no ])
if test x$with_soup != xno; then OSTREE_FEATURES="$OSTREE_FEATURES +libsoup"; fi
AM_CONDITIONAL(USE_LIBSOUP, test x$with_soup != xno)

m4_ifdef([GOBJECT_INTROSPECTION_CHECK], [
  GOBJECT_INTROSPECTION_CHECK([1.34.0])
])
AM_CONDITIONAL(BUILDOPT_INTROSPECTION, test "x$found_introspection" = xyes)

LIBGPGME_DEPENDENCY="1.1.8"

AC_ARG_WITH(gpgme,
            AS_HELP_STRING([--without-gpgme], [Do not use gpgme]),
            :, with_gpgme=maybe)

AS_IF([ test x$with_gpgme != xno ], [
   AC_MSG_CHECKING([for $LIBGPGME_DEPENDENCY])
   m4_ifdef([AM_PATH_GPGME], [
      AM_PATH_GPGME($LIBGPGME_DEPENDENCY, have_gpgme=yes, have_gpgme=no)
      ],[
      AM_CONDITIONAL([have_gpgme],[false])
      ])
   AC_MSG_RESULT([$have_gpgme])
   AS_IF([ test x$have_gpgme = xno && test x$with_gpgme != xmaybe ], [
       AC_MSG_ERROR([gpgme is enabled but could not be found])
   ])
   AS_IF([ test x$have_gpgme = xyes], [
       AC_DEFINE(HAVE_GPGME, 1, [Define if we have gpgme])
       with_gpgme=yes
       AC_PATH_PROG(GPGVPATH, [gpgv2 gpgv])
       AC_SUBST(GPGVPATH)
       ], [ with_gpgme=no ])
], [ with_gpgme=no ])
if test x$with_gpgme != xno; then OSTREE_FEATURES="$OSTREE_FEATURES +gpgme"; fi
AM_CONDITIONAL(USE_GPGME, test $with_gpgme != no)

LIBARCHIVE_DEPENDENCY="libarchive >= 2.8.0"

GTK_DOC_CHECK([1.15], [--flavour no-tmpl])
AC_PATH_PROG([XSLTPROC], [xsltproc])

AC_ARG_WITH(libarchive,
	    AS_HELP_STRING([--without-libarchive], [Do not use libarchive]),
	    :, with_libarchive=maybe)

AS_IF([ test x$with_libarchive != xno ], [
    AC_MSG_CHECKING([for $LIBARCHIVE_DEPENDENCY])
    PKG_CHECK_EXISTS($LIBARCHIVE_DEPENDENCY, have_libarchive=yes, have_libarchive=no)
    AC_MSG_RESULT([$have_libarchive])
    AS_IF([ test x$have_libarchive = xno && test x$with_libarchive != xmaybe ], [
       AC_MSG_ERROR([libarchive is enabled but could not be found])
    ])
    AS_IF([ test x$have_libarchive = xyes], [
        AC_DEFINE(HAVE_LIBARCHIVE, 1, [Define if we have libarchive.pc])
	PKG_CHECK_MODULES(OT_DEP_LIBARCHIVE, $LIBARCHIVE_DEPENDENCY)
        save_LIBS=$LIBS
        LIBS=$OT_DEP_LIBARCHIVE_LIBS
        AC_CHECK_FUNCS(archive_read_support_filter_all)
        LIBS=$save_LIBS
	with_libarchive=yes
    ], [
	with_libarchive=no
    ])
], [ with_libarchive=no ])
if test x$with_libarchive != xno; then OSTREE_FEATURES="$OSTREE_FEATURES +libarchive"; fi
AM_CONDITIONAL(USE_LIBARCHIVE, test $with_libarchive != no)

dnl This is what is in RHEL7 anyways
SELINUX_DEPENDENCY="libselinux >= 2.1.13"

AC_ARG_WITH(selinux,
	    AS_HELP_STRING([--without-selinux], [Do not use SELinux]),
	    :, with_selinux=maybe)

AS_IF([ test x$with_selinux != xno ], [
    AC_MSG_CHECKING([for $SELINUX_DEPENDENCY])
    PKG_CHECK_EXISTS($SELINUX_DEPENDENCY, have_selinux=yes, have_selinux=no)
    AC_MSG_RESULT([$have_selinux])
    AS_IF([ test x$have_selinux = xno && test x$with_selinux != xmaybe ], [
       AC_MSG_ERROR([SELinux is enabled but could not be found])
    ])
    AS_IF([ test x$have_selinux = xyes], [
        AC_DEFINE(HAVE_SELINUX, 1, [Define if we have libselinux.pc])
	PKG_CHECK_MODULES(OT_DEP_SELINUX, $SELINUX_DEPENDENCY)
	with_selinux=yes
    ], [
	with_selinux=no
    ])
], [ with_selinux=no ])
if test x$with_selinux != xno; then OSTREE_FEATURES="$OSTREE_FEATURES +selinux"; fi
AM_CONDITIONAL(USE_SELINUX, test $with_selinux != no)

AC_ARG_WITH(dracut,
            AS_HELP_STRING([--with-dracut],
                           [Install dracut module (default: no)]),,
              [with_dracut=no])
AM_CONDITIONAL(BUILDOPT_DRACUT, test x$with_dracut = xyes)

AC_ARG_WITH(mkinitcpio,
            AS_HELP_STRING([--with-mkinitcpio],
                           [Install mkinitcpio module (default: no)]),,
              [with_mkinitcpio=no])
AM_CONDITIONAL(BUILDOPT_MKINITCPIO, test x$with_mkinitcpio = xyes)

AS_IF([test "x$with_dracut" = "xyes" || test "x$with_mkinitcpio" = "xyes"], [
  with_systemd=yes
  AC_ARG_WITH([systemdsystemunitdir],
              AS_HELP_STRING([--with-systemdsystemunitdir=DIR], [Directory for systemd service files]),
              [],
              [with_systemdsystemunitdir=$($PKG_CONFIG --variable=systemdsystemunitdir systemd)])
  AS_IF([test "x$with_systemdsystemunitdir" != "xno"], [
    AC_SUBST([systemdsystemunitdir], [$with_systemdsystemunitdir])
  ])
])
AM_CONDITIONAL(BUILDOPT_SYSTEMD, test x$with_systemd = xyes)

dnl for tests
AS_IF([test "x$found_introspection" = xyes], [
  AC_PATH_PROG(GJS, [gjs])
  if test -n "$GJS"; then
    have_gjs=yes
  else
    have_gjs=no
  fi
], [have_gjs=no])
AM_CONDITIONAL(BUILDOPT_GJS, test x$have_gjs = xyes)

AC_CONFIG_FILES([
Makefile
doc/Makefile
src/libostree/ostree-1.pc
])
AC_OUTPUT

echo "
    OSTree $VERSION
    ===============


    introspection:                                $found_introspection
    libsoup (retrieve remote HTTP repositories):  $with_soup
    SELinux:                                      $with_selinux
    libarchive (parse tar files directly):        $with_libarchive
    gpgme (sign commits):                         $with_gpgme
    documentation:                                $enable_gtk_doc
    gjs-based tests:                              $have_gjs
    dracut:                                       $with_dracut
    mkinitcpio:                                   $with_mkinitcpio"
AS_IF([test "x$with_systemd" = "xyes"], [
    echo "    systemd unit dir:                             $with_systemdsystemunitdir"
])
echo ""

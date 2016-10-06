AC_PREREQ([2.63])
dnl If incrementing the version here, remember to update libostree.sym too
AC_INIT([ostree], [2016.11], [walters@verbum.org])
AC_CONFIG_HEADER([config.h])
AC_CONFIG_MACRO_DIR([buildutil])
AC_CONFIG_AUX_DIR([build-aux])

AM_INIT_AUTOMAKE([1.13 -Wno-portability foreign no-define tar-ustar no-dist-gzip dist-xz
                  color-tests subdir-objects])
AM_MAINTAINER_MODE([enable])
AM_SILENT_RULES([yes])
AC_USE_SYSTEM_EXTENSIONS
AC_SYS_LARGEFILE

AC_PROG_CC
AM_PROG_CC_C_O
AC_PROG_YACC

CC_CHECK_FLAGS_APPEND([WARN_CFLAGS], [CFLAGS], [\
        -pipe \
        -Wall \
	-Werror=empty-body \
	-Werror=strict-prototypes \
	-Werror=missing-prototypes \
	-Werror=implicit-function-declaration \
        "-Werror=format=2 -Werror=format-security -Werror=format-nonliteral" \
	-Werror=pointer-arith -Werror=init-self \
        -Werror=missing-declarations \
        -Werror=return-type \
        -Werror=overflow \
        -Werror=int-conversion \
	-Werror=missing-include-dirs -Werror=aggregate-return \
	-Werror=declaration-after-statement \
])
AC_SUBST(WARN_CFLAGS)

# Initialize libtool
LT_PREREQ([2.2.4])
LT_INIT([disable-static])

OSTREE_FEATURES=""
AC_SUBST([OSTREE_FEATURES])

GLIB_TESTS
LIBGLNX_CONFIGURE

AC_CHECK_HEADER([sys/xattr.h],,[AC_MSG_ERROR([You must have sys/xattr.h from glibc])])

AS_IF([test "$YACC" != "bison -y"], [AC_MSG_ERROR([bison not found but required])])

PKG_PROG_PKG_CONFIG

AM_PATH_GLIB_2_0

dnl When bumping the gio-unix-2.0 dependency (or glib-2.0 in general),
dnl remember to bump GLIB_VERSION_MIN_REQUIRED and
dnl GLIB_VERSION_MAX_ALLOWED in Makefile.am
GIO_DEPENDENCY="gio-unix-2.0 >= 2.40.0"
PKG_CHECK_MODULES(OT_DEP_GIO_UNIX, $GIO_DEPENDENCY)

dnl 5.1.0 is an arbitrary version here
PKG_CHECK_MODULES(OT_DEP_LZMA, liblzma >= 5.0.5)

dnl Needed for rollsum
PKG_CHECK_MODULES(OT_DEP_ZLIB, zlib)

dnl We're not actually linking to this, just using the header
PKG_CHECK_MODULES(OT_DEP_E2P, e2p)

dnl When bumping the libsoup-2.4 dependency, remember to bump
dnl SOUP_VERSION_MIN_REQUIRED and SOUP_VERSION_MAX_ALLOWED in
dnl Makefile.am
SOUP_DEPENDENCY="libsoup-2.4 >= 2.39.1"
AC_ARG_WITH(soup,
	    AS_HELP_STRING([--with-soup], [Use libsoup @<:@default=yes@:>@]),
	    [], [with_soup=check])
AS_IF([test x$with_soup != xno ], [
  AC_ARG_ENABLE(libsoup_client_certs,
                AS_HELP_STRING([--enable-libsoup-client-certs],
                               [Require availability of new enough libsoup TLS client cert API (default: auto)]),,
                [enable_libsoup_client_certs=auto])
  AC_MSG_CHECKING([for $SOUP_DEPENDENCY])
  PKG_CHECK_EXISTS($SOUP_DEPENDENCY, have_soup=yes, have_soup=no)
  AC_MSG_RESULT([$have_soup])
    AS_IF([ test x$have_soup = xno && test x$with_soup != xcheck], [
       AC_MSG_ERROR([libsoup is enabled but could not be found])
  ])
  AS_IF([test x$have_soup = xyes], [
    PKG_CHECK_MODULES(OT_DEP_SOUP, $SOUP_DEPENDENCY)
    AC_DEFINE([HAVE_LIBSOUP], 1, [Define if we have libsoup.pc])
    with_soup=yes
    save_CFLAGS=$CFLAGS
    CFLAGS=$OT_DEP_SOUP_CFLAGS
    have_libsoup_client_certs=no
    AC_CHECK_DECL([SOUP_SESSION_TLS_INTERACTION], [
        AC_DEFINE([HAVE_LIBSOUP_CLIENT_CERTS], 1, [Define if we have libsoup client certs])
        have_libsoup_client_certs=yes
        ], [], [#include <libsoup/soup.h>])
    AS_IF([test x$enable_libsoup_client_certs = xyes && test x$have_libsoup_client_certs != xyes], [
      AC_MSG_ERROR([libsoup client certs explicitly requested but not found])
    ]) 
    CFLAGS=$save_CFLAGS
  ], [
    with_soup=no
  ])
], [ with_soup=no ])
if test x$with_soup != xno; then OSTREE_FEATURES="$OSTREE_FEATURES +libsoup"; fi
AM_CONDITIONAL(USE_LIBSOUP, test x$with_soup != xno)
AM_CONDITIONAL(HAVE_LIBSOUP_CLIENT_CERTS, test x$have_libsoup_client_certs = xyes)

m4_ifdef([GOBJECT_INTROSPECTION_CHECK], [
  GOBJECT_INTROSPECTION_CHECK([1.34.0])
])
AM_CONDITIONAL(BUILDOPT_INTROSPECTION, test "x$found_introspection" = xyes)

LIBGPGME_DEPENDENCY="1.1.8"

PKG_CHECK_MODULES(OT_DEP_GPGME, gpgme-pthread >= $LIBGPGME_DEPENDENCY, have_gpgme=yes, [
    m4_ifdef([AM_PATH_GPGME_PTHREAD], [
       AM_PATH_GPGME_PTHREAD($LIBGPGME_DEPENDENCY, have_gpgme=yes, have_gpgme=no)
    ],[ have_gpgme=no ])
])
AS_IF([ test x$have_gpgme = xno ], [
   AC_MSG_ERROR([Need GPGME_PTHREAD version $LIBGPGME_DEPENDENCY or later])
])
OSTREE_FEATURES="$OSTREE_FEATURES +gpgme"

LIBARCHIVE_DEPENDENCY="libarchive >= 2.8.0"
# What's in RHEL7.2.
FUSE_DEPENDENCY="fuse >= 2.9.2"

# check for gtk-doc
m4_ifdef([GTK_DOC_CHECK], [
GTK_DOC_CHECK([1.15], [--flavour no-tmpl])
],[
enable_gtk_doc=no
AM_CONDITIONAL([ENABLE_GTK_DOC], false)
])

AC_ARG_ENABLE(man,
              [AS_HELP_STRING([--enable-man],
                              [generate man pages [default=auto]])],,
              enable_man=maybe)

AS_IF([test "$enable_man" != no], [
  AC_PATH_PROG([XSLTPROC], [xsltproc])
  AS_IF([test -z "$XSLTPROC"], [
    AS_IF([test "$enable_man" = yes], [
      AC_MSG_ERROR([xsltproc is required for --enable-man])
    ])
    enable_man=no
  ])
  enable_man=yes
])
AM_CONDITIONAL(ENABLE_MAN, test "$enable_man" != no)

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
        AC_DEFINE([HAVE_LIBARCHIVE], 1, [Define if we have libarchive.pc])
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
        AC_DEFINE([HAVE_SELINUX], 1, [Define if we have libselinux.pc])
	PKG_CHECK_MODULES(OT_DEP_SELINUX, $SELINUX_DEPENDENCY)
	with_selinux=yes
    ], [
	with_selinux=no
    ])
], [ with_selinux=no ])
if test x$with_selinux != xno; then OSTREE_FEATURES="$OSTREE_FEATURES +selinux"; fi
AM_CONDITIONAL(USE_SELINUX, test $with_selinux != no)

dnl This is what is in RHEL7.2 right now, picking it arbitrarily
LIBMOUNT_DEPENDENCY="mount >= 2.23.0"

AC_ARG_WITH(libmount,
	    AS_HELP_STRING([--without-libmount], [Do not use libmount]),
	    :, with_libmount=maybe)

AS_IF([ test x$with_libmount != xno ], [
    AC_MSG_CHECKING([for $LIBMOUNT_DEPENDENCY])
    PKG_CHECK_EXISTS($LIBMOUNT_DEPENDENCY, have_libmount=yes, have_libmount=no)
    AC_MSG_RESULT([$have_libmount])
    AS_IF([ test x$have_libmount = xno && test x$with_libmount != xmaybe ], [
       AC_MSG_ERROR([libmount is enabled but could not be found])
    ])
    AS_IF([ test x$have_libmount = xyes], [
        AC_DEFINE([HAVE_LIBMOUNT], 1, [Define if we have libmount.pc])
	PKG_CHECK_MODULES(OT_DEP_LIBMOUNT, $LIBMOUNT_DEPENDENCY)
	with_libmount=yes
    ], [
	with_libmount=no
    ])
], [ with_libmount=no ])
if test x$with_libmount != xno; then OSTREE_FEATURES="$OSTREE_FEATURES +libmount"; fi
AM_CONDITIONAL(USE_LIBMOUNT, test $with_libmount != no)

# Enabled by default because I think people should use it.
AC_ARG_ENABLE(rofiles-fuse,
              [AS_HELP_STRING([--enable-rofiles-fuse],
                              [generate rofiles-fuse helper [default=yes]])],,
              enable_rofiles_fuse=yes)
AS_IF([ test x$enable_rofiles_fuse != xno ], [
    PKG_CHECK_MODULES(BUILDOPT_FUSE, $FUSE_DEPENDENCY)
], [enable_rofiles_fuse=no])
AM_CONDITIONAL(BUILDOPT_FUSE, test x$enable_rofiles_fuse = xyes)

AC_ARG_WITH(dracut,
            AS_HELP_STRING([--with-dracut],
                           [Install dracut module (default: no)]),,
              [with_dracut=no])
case x$with_dracut in
    xno) ;;
    xyes) ;;
    xyesbutnoconf) ;;
    *) AC_MSG_ERROR([Unknown --with-dracut value $with_dracut])
esac    
AM_CONDITIONAL(BUILDOPT_DRACUT, test x$with_dracut = xyes || test x$with_dracut = xyesbutnoconf)
AM_CONDITIONAL(BUILDOPT_DRACUT_CONF, test x$with_dracut = xyes)

AC_ARG_WITH(mkinitcpio,
            AS_HELP_STRING([--with-mkinitcpio],
                           [Install mkinitcpio module (default: no)]),,
              [with_mkinitcpio=no])
AM_CONDITIONAL(BUILDOPT_MKINITCPIO, test x$with_mkinitcpio = xyes)

dnl We have separate checks for libsystemd and the unit dir for historical reasons
PKG_CHECK_MODULES([LIBSYSTEMD], [libsystemd], [have_libsystemd=yes], [have_libsystemd=no])
AM_CONDITIONAL(BUILDOPT_LIBSYSTEMD, test x$have_libsystemd = xyes)
AM_COND_IF(BUILDOPT_LIBSYSTEMD,
           AC_DEFINE([HAVE_LIBSYSTEMD], 1, [Define if we have libsystemd]))

AS_IF([test "x$have_libsystemd" = "xyes"], [
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

AC_ARG_WITH(builtin-grub2-mkconfig,
            AS_HELP_STRING([--with-builtin-grub2-mkconfig],
                           [Use a builtin minimal grub2-mkconfig to generate a GRUB2 configuration file (default: no)]),,
              [with_builtin_grub2_mkconfig=no])
AM_CONDITIONAL(BUILDOPT_BUILTIN_GRUB2_MKCONFIG, test x$with_builtin_grub2_mkconfig = xyes)
AM_COND_IF(BUILDOPT_BUILTIN_GRUB2_MKCONFIG,
           AC_DEFINE([USE_BUILTIN_GRUB2_MKCONFIG], 1, [Define if using internal ostree-grub-generator]))
AC_ARG_WITH(grub2-mkconfig-path,
            AS_HELP_STRING([--with-grub2-mkconfig-path],
                           [Path to grub2-mkconfig]))
AS_IF([test x$with_grub2_mkconfig_path = x], [
  dnl Otherwise, look for the path to the system generator.  On some
  dnl distributions GRUB2 *-mkconfig executable has 'grub2' prefix and
  dnl on some 'grub'.  We default to grub2-mkconfig.
  AC_CHECK_PROGS(GRUB2_MKCONFIG, [grub2-mkconfig grub-mkconfig], [grub2-mkconfig])
],[GRUB2_MKCONFIG=$with_grub2_mkconfig_path])
AC_DEFINE_UNQUOTED([GRUB2_MKCONFIG_PATH], ["$GRUB2_MKCONFIG"], [The system grub2-mkconfig executible name])

AC_ARG_WITH(static-compiler,
            AS_HELP_STRING([--with-static-compiler],
                           [Use the given compiler to build ostree-prepare-root statically linked (default: no)]),,
              [with_static_compiler=no])
AM_CONDITIONAL(BUILDOPT_USE_STATIC_COMPILER, test x$with_static_compiler != xno)
AC_SUBST(STATIC_COMPILER, $with_static_compiler)

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
apidoc/Makefile
src/libostree/ostree-1.pc
])
AC_OUTPUT

echo "
    OSTree $VERSION
    ===============


    introspection:                                $found_introspection
    rofiles-fuse:                                 $enable_rofiles_fuse
    libsoup (retrieve remote HTTP repositories):  $with_soup
    libsoup TLS client certs:                     $have_libsoup_client_certs
    SELinux:                                      $with_selinux
    systemd:                                      $have_libsystemd
    libmount:                                     $with_libmount
    libarchive (parse tar files directly):        $with_libarchive
    static deltas:                                yes (always enabled now)
    O_TMPFILE:                                    $enable_otmpfile
    wrpseudo-compat:                              $enable_wrpseudo_compat
    man pages (xsltproc):                         $enable_man
    api docs (gtk-doc):                           $enable_gtk_doc
    gjs-based tests:                              $have_gjs
    dracut:                                       $with_dracut
    mkinitcpio:                                   $with_mkinitcpio
    Static compiler for ostree-prepare-root:      $with_static_compiler"
AS_IF([test x$with_builtin_grub2_mkconfig = xyes], [
    echo "    builtin grub2-mkconfig (instead of system):   $with_builtin_grub2_mkconfig"
], [
    echo "    grub2-mkconfig path:                          $GRUB2_MKCONFIG"
])
echo ""

AC_PREREQ([2.63])
AC_INIT([ostree], [0], [walters@verbum.org])
AC_CONFIG_HEADER([config.h])
AC_CONFIG_MACRO_DIR([m4])
AC_CONFIG_AUX_DIR([build-aux])

AM_INIT_AUTOMAKE([1.11 -Wno-portability foreign no-define tar-ustar no-dist-gzip dist-xz])
AM_MAINTAINER_MODE([enable])
AM_SILENT_RULES([yes])

AC_PROG_CC
AM_PROG_CC_C_O

changequote(,)dnl
if test "x$GCC" = "xyes"; then
  WARN_CFLAGS="-Wall -Werror=missing-prototypes"
fi
changequote([,])dnl
AC_SUBST(WARN_CFLAGS)

# Initialize libtool
LT_PREREQ([2.2.4])
LT_INIT([disable-static])

PKG_PROG_PKG_CONFIG

GIO_DEPENDENCY="gio-unix-2.0 >= 2.28"
SOUP_DEPENDENCY="libsoup-gnome-2.4 >= 2.34.0"
LIBARCHIVE_DEPENDENCY="libarchive >= 2.8.0"

PKG_CHECK_MODULES(OT_DEP_GIO_UNIX, $GIO_DEPENDENCY)

AC_ARG_WITH(soup-gnome,
	    AS_HELP_STRING([--without-soup-gnome], [Do not use libsoup-gnome (implies no pull support)]),
	    :, with_soup_gnome=maybe)
if test x$with_soup_gnome != xno; then
    PKG_CHECK_EXISTS($SOUP_DEPENDENCY, have_soup_gnome=yes, have_soup_gnome=no)
    if test x$have_soup_gnome = xno && test x$with_soup_gnome != xmaybe; then
       AC_MSG_ERROR([libsoup-gnome is enabled but could not be found])
    fi
    if test x$have_soup_gnome = xyes; then
	PKG_CHECK_MODULES(OT_DEP_SOUP, $SOUP_DEPENDENCY)
    else
	with_soup_gnome=no
    fi		
fi
AM_CONDITIONAL(USE_LIBSOUP_GNOME, test $with_soup_gnome != no)

AC_ARG_WITH(libarchive,
	    AS_HELP_STRING([--without-libarchive], [Do not use libarchive]),
	    :, with_libarchive=maybe)
if test x$with_libarchive != xno; then
    PKG_CHECK_EXISTS($LIBARCHIVE_DEPENDENCY, have_libarchive=yes, have_libarchive=no)
    if test x$have_libarchive = xno && test x$with_libarchive != xmaybe; then
       AC_MSG_ERROR([libarchive is enabled but could not be found])
    fi
    if test x$have_libarchive = xyes; then
        AC_DEFINE(HAVE_LIBARCHIVE, 1, [Define if we have libarchive.pc])
	PKG_CHECK_MODULES(OT_DEP_LIBARCHIVE, $LIBARCHIVE_DEPENDENCY)
    else
	with_libarchive=no
    fi		
fi
AM_CONDITIONAL(USE_LIBARCHIVE, test $with_libarchive != no)

AM_PATH_PYTHON

AC_CONFIG_FILES([
Makefile
])
AC_OUTPUT

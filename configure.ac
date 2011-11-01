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
  case " $CFLAGS " in
  *[\ \	]-Wall[\ \	]*) ;;
  *) CFLAGS="$CFLAGS -Wall" ;;
  esac
  case " $CFLAGS " in
  *[\ \	]-Werror=missing-prototypes[\ \	]*) ;;
  *) CFLAGS="$CFLAGS -Werror=missing-prototypes" ;;
  esac
fi
changequote([,])dnl

# Initialize libtool
AC_PROG_LIBTOOL
LT_PREREQ([2.2])
LT_INIT

PKG_PROG_PKG_CONFIG

PKG_CHECK_MODULES(GIO_UNIX, [gio-unix-2.0 >= 2.28])
PKG_CHECK_MODULES(OT_COREBIN_DEP, [libsoup-gnome-2.4 >= 2.34.0 gio-unix-2.0 >= 2.28])

AM_PATH_PYTHON

AC_CONFIG_FILES([
Makefile
])
AC_OUTPUT

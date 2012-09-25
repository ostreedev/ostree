AC_PREREQ([2.63])
AC_INIT([ostree], [2012.11], [walters@verbum.org])
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

AC_ARG_ENABLE(triggers-only,
	    AS_HELP_STRING([--enable-triggers-only], [Only build and install triggers]),,
	    enable_triggers_only=no)
AM_CONDITIONAL(TRIGGERS_ONLY, test x$enable_triggers_only != xno)

AC_CHECK_HEADER([attr/xattr.h],,[AC_MSG_ERROR([You must have attr/xattr.h from libattr])])

PKG_PROG_PKG_CONFIG

AC_ARG_ENABLE(embedded-dependencies,
	    AS_HELP_STRING([--enable-embedded-dependencies], [Use embedded GLib and libsoup copies]),,
	    enable_embedded_dependencies=no)
AM_CONDITIONAL(BUILD_EMBEDDED_DEPENDENCIES, test x$enable_embedded_dependencies = xyes)
AS_IF([test x$enable_embedded_dependencies = xyes], [
  echo "NOTICE: Embedded dependencies (GLib and libsoup) enabled"
  with_soup=yes
], [
  GIO_DEPENDENCY="gio-unix-2.0 >= 2.33.4"
  PKG_CHECK_MODULES(OT_DEP_GIO_UNIX, $GIO_DEPENDENCY)

  SOUP_DEPENDENCY="libsoup-2.4 >= 2.39.1"
  AC_ARG_WITH(soup,
	    AS_HELP_STRING([--with-soup], [Use libsoup @<:@default=yes@:>@]),
	    [], [with_soup=check])
  if test x$enable_triggers_only != xno; then
    with_soup=no
  fi
  AS_IF([test x$with_soup != xno ], [
    AC_MSG_CHECKING([for $SOUP_DEPENDENCY])
    PKG_CHECK_EXISTS($SOUP_DEPENDENCY, have_soup=yes, have_soup=no)
    AC_MSG_RESULT([$have_soup])
    AS_IF([ test x$have_soup = xno && test x$with_soup != xcheck], [
       AC_MSG_ERROR([libsoup is enabled but could not be found])
    ])
    AS_IF([test x$have_soup = xyes], [
	PKG_CHECK_MODULES(OT_DEP_SOUP, $SOUP_DEPENDENCY)
	with_soup=yes
    ], [
	with_soup=no
    ])
  ], [ with_soup=no ])
])
AM_CONDITIONAL(USE_LIBSOUP, test x$with_soup != xno)

LIBARCHIVE_DEPENDENCY="libarchive >= 2.8.0"

AC_ARG_ENABLE(documentation,
              AC_HELP_STRING([--enable-documentation],
                             [build documentation]),,
              enable_documentation=yes)
if test x$enable_triggers_only != xno; then
   enable_documentation=no
fi
if test x$enable_documentation = xyes; then
   AC_PATH_PROG([XSLTPROC], [xsltproc])
   if test x$XSLTPROC = x; then
      AC_MSG_ERROR([xsltproc is required to build documentation])
   fi
fi
AM_CONDITIONAL(BUILD_DOCUMENTATION, test x$enable_documentation = xyes)

AC_ARG_WITH(libarchive,
	    AS_HELP_STRING([--without-libarchive], [Do not use libarchive]),
	    :, with_libarchive=maybe)
if test x$enable_triggers_only != xno; then
	with_libarchive=no
fi

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
	with_libarchive=yes
    ], [
	with_libarchive=no
    ])
], [ with_libarchive=no ])
AM_CONDITIONAL(USE_LIBARCHIVE, test $with_libarchive != no)

AS_IF([test x$cross_compiling != xyes && test -d /etc/kernel && test x$enable_triggers_only != xno ],
      [kernel_updates_default=yes],
      [kernel_updates_default=no])

AC_ARG_ENABLE(kernel-updates,
              AS_HELP_STRING([--enable-kernel-updates],
	      [Install configuration scripts to handle kernel updates
	       in the host system]), ,
	      enable_kernel_updates=${kernel_updates_default})
AM_CONDITIONAL(ENABLE_KERNEL_UPDATES, test $enable_kernel_updates != no)

AC_CONFIG_FILES([
Makefile
embedded-dependencies/Makefile
])
AC_OUTPUT

echo "
    OSTree $VERSION
    ===============


    triggers only: $enable_triggers_only
    embedded dependencies: $enable_embedded_dependencies
    libsoup (retrieve remote HTTP repositories): $with_soup
    libarchive (parse tar files directly): $with_libarchive
    documentation: $enable_documentation
    kernel updates integration: $enable_kernel_updates
"

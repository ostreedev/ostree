AC_PREREQ([2.63])
dnl If doing a final release, remember to follow the instructions to
dnl update libostree-released.sym from libostree-devel.sym, and update the checksum
dnl in test-symbols.sh, and also set is_release_build=yes below.  Then make
dnl another post-release commit to bump the version, and set is_release_build=no.
m4_define([year_version], [2017])
m4_define([release_version], [8])
m4_define([package_version], [year_version.release_version])
AC_INIT([libostree], [package_version], [walters@verbum.org])
is_release_build=yes
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

dnl Versioning information
AC_SUBST([YEAR_VERSION], [year_version])
AC_SUBST([RELEASE_VERSION], [release_version])
AC_SUBST([PACKAGE_VERSION], [package_version])

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
        -Werror=parenthesis \
        -Werror=incompatible-pointer-types \
        -Werror=misleading-indentation \
	-Werror=missing-include-dirs -Werror=aggregate-return \
  -Werror=unused-result \
])
AC_SUBST(WARN_CFLAGS)

AC_MSG_CHECKING([for -fsanitize=address in CFLAGS])
if echo $CFLAGS | grep -q -e -fsanitize=address; then
  AC_MSG_RESULT([yes])
  using_asan=yes
else
  AC_MSG_RESULT([no])
fi
AM_CONDITIONAL(BUILDOPT_ASAN, [test x$using_asan = xyes])

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

AM_PATH_GLIB_2_0(,,AC_MSG_ERROR([GLib not found]))

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

dnl Arbitrary version that's in CentOS7.2 now
CURL_DEPENDENCY=7.29.0
AC_ARG_WITH(curl,
	    AS_HELP_STRING([--with-curl], [Use libcurl @<:@default=no@:>@]),
	    [], [with_curl=no])
AS_IF([test x$with_curl != xno ], [
    PKG_CHECK_MODULES(OT_DEP_CURL, libcurl >= $CURL_DEPENDENCY)
    with_curl=yes
    AC_DEFINE([HAVE_LIBCURL], 1, [Define if we have libcurl.pc])
    dnl Currently using libcurl requires soup for trivial-httpd for tests
    with_soup_default=yes
], [with_soup_default=check])
AM_CONDITIONAL(USE_CURL, test x$with_curl != xno)
if test x$with_curl = xyes; then OSTREE_FEATURES="$OSTREE_FEATURES libcurl"; fi

dnl When bumping the libsoup-2.4 dependency, remember to bump
dnl SOUP_VERSION_MIN_REQUIRED and SOUP_VERSION_MAX_ALLOWED in
dnl Makefile.am
SOUP_DEPENDENCY="libsoup-2.4 >= 2.39.1"
AC_ARG_WITH(soup,
	    AS_HELP_STRING([--with-soup], [Use libsoup @<:@default=yes@:>@]),
	    [], [with_soup=$with_soup_default])
AS_IF([test x$with_soup != xno], [
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
if test x$with_soup != xno; then OSTREE_FEATURES="$OSTREE_FEATURES libsoup"; fi
AM_CONDITIONAL(USE_LIBSOUP, test x$with_soup != xno)
AM_CONDITIONAL(HAVE_LIBSOUP_CLIENT_CERTS, test x$have_libsoup_client_certs = xyes)

AC_ARG_ENABLE(trivial-httpd-cmdline,
  [AS_HELP_STRING([--enable-trivial-httpd-cmdline],
  [Continue to support "ostree trivial-httpd" [default=no]])],,
  enable_trivial_httpd_cmdline=no)
AM_CONDITIONAL(BUILDOPT_TRIVIAL_HTTPD, test x$enable_trivial_httpd_cmdline = xyes)
AM_COND_IF(BUILDOPT_TRIVIAL_HTTPD,
  [AC_DEFINE([BUILDOPT_ENABLE_TRIVIAL_HTTPD_CMDLINE], 1, [Define if we are enabling ostree trivial-httpd entrypoint])]
)

AS_IF([test x$with_curl = xyes && test x$with_soup = xno], [
  AC_MSG_ERROR([Curl enabled, but libsoup is not; libsoup is needed for tests])
])
AM_CONDITIONAL(USE_CURL_OR_SOUP, test x$with_curl != xno || test x$with_soup != xno)
AS_IF([test x$with_curl != xno || test x$with_soup != xno],
            [AC_DEFINE([HAVE_LIBCURL_OR_LIBSOUP], 1, [Define if we have soup or curl])])
AS_IF([test x$with_curl = xyes], [fetcher_backend=curl], [test x$with_soup = xyes], [fetcher_backend=libsoup], [fetcher_backend=none])

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
OSTREE_FEATURES="$OSTREE_FEATURES gpgme"

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
  ],[
    enable_man=yes
  ])
])
AM_CONDITIONAL(ENABLE_MAN, test "$enable_man" != no)

AC_ARG_ENABLE(rust,
  [AS_HELP_STRING([--enable-rust],
  [Compile Rust code instead of C [default=no]])],,
   [enable_rust=no; rust_debug_release=no])

AS_IF([test "$enable_rust" = yes], [
   AC_PATH_PROG([cargo], [cargo])
   AS_IF([test -z "$cargo"], [AC_MSG_ERROR([cargo is required for --enable-rust])])
   AC_PATH_PROG([rustc], [rustc])
   AS_IF([test -z "$rustc"], [AC_MSG_ERROR([rustc is required for --enable-rust])])

   dnl These bits based on gnome:librsvg/configure.ac

   dnl By default, we build in public release mode.
   AC_ARG_ENABLE(rust-debug,
      AC_HELP_STRING([--enable-rust-debug],
       [Build Rust code with debugging information [default=no]]),
       [rust_debug_release=$enableval],
       [rust_debug_release=release])

   AC_MSG_CHECKING(whether to build Rust code with debugging information)
   if test "x$rust_debug_release" = "xyes" ; then
      rust_debug_release=debug
      AC_MSG_RESULT(yes)
   else
      AC_MSG_RESULT(no)
   fi
   RUST_TARGET_SUBDIR=${rust_debug_release}
   AC_SUBST([RUST_TARGET_SUBDIR])
])
AM_CONDITIONAL(RUST_DEBUG, [test "x$rust_debug_release" = "xdebug"])
AM_CONDITIONAL(ENABLE_RUST, [test "$enable_rust" != no])

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
if test x$with_libarchive != xno; then OSTREE_FEATURES="$OSTREE_FEATURES libarchive"; fi
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
if test x$with_selinux != xno; then OSTREE_FEATURES="$OSTREE_FEATURES selinux"; fi
AM_CONDITIONAL(USE_SELINUX, test $with_selinux != no)

AC_ARG_WITH(smack,
AS_HELP_STRING([--with-smack], [Enable smack]),
:, with_smack=no)
AS_IF([ test x$with_smack = xyes], [
  AC_DEFINE([WITH_SMACK], 1, [Define if we have smack.pc])
])
AM_CONDITIONAL(USE_SMACK, test $with_smack != no)

dnl begin openssl (really just libcrypto right now)
OPENSSL_DEPENDENCY="libcrypto >= 1.0.1"
AC_ARG_WITH(openssl,
AS_HELP_STRING([--with-openssl], [Enable use of OpenSSL libcrypto (checksums)]),
:, with_openssl=no)

AS_IF([ test x$with_openssl != xno ], [
      PKG_CHECK_MODULES(OT_DEP_OPENSSL, $OPENSSL_DEPENDENCY)
      AC_DEFINE([HAVE_OPENSSL], 1, [Define if we have openssl])
      with_openssl=yes
], [
      with_openssl=no
])
if test x$with_openssl != xno; then OSTREE_FEATURES="$OSTREE_FEATURES openssl"; fi
AM_CONDITIONAL(USE_OPENSSL, test $with_openssl != no)
dnl end openssl

dnl Avahi dependency for finding repos
AVAHI_DEPENDENCY="avahi-client >= 0.6.31 avahi-glib >= 0.6.31"

AC_ARG_WITH(avahi,
            AS_HELP_STRING([--without-avahi], [Do not use Avahi]),
            :, with_avahi=maybe)

AS_IF([ test x$with_avahi != xno ], [
    AC_MSG_CHECKING([for $AVAHI_DEPENDENCY])
    PKG_CHECK_EXISTS($AVAHI_DEPENDENCY, have_avahi=yes, have_avahi=no)
    AC_MSG_RESULT([$have_avahi])
    AS_IF([ test x$have_avahi = xno && test x$with_avahi != xmaybe ], [
       AC_MSG_ERROR([Avahi is enabled but could not be found])
    ])
    AS_IF([ test x$have_avahi = xyes], [
        AC_DEFINE([HAVE_AVAHI], 1, [Define if we have avahi-client.pc and avahi-glib.pc])
        PKG_CHECK_MODULES(OT_DEP_AVAHI, $AVAHI_DEPENDENCY)
        with_avahi=yes
    ], [
        with_avahi=no
    ])
], [ with_avahi=no ])
if test x$with_avahi != xno; then OSTREE_FEATURES="$OSTREE_FEATURES avahi"; fi
AM_CONDITIONAL(USE_AVAHI, test $with_avahi != no)

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
  save_LIBS=$LIBS
  LIBS=$OT_DEP_LIBMOUNT_LIBS
  AC_CHECK_FUNCS(mnt_unref_cache)
  LIBS=$save_LIBS
    ], [
	with_libmount=no
    ])
], [ with_libmount=no ])
if test x$with_libmount != xno; then OSTREE_FEATURES="$OSTREE_FEATURES libmount"; fi
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
  AC_ARG_WITH([systemdsystemgeneratordir],
              AS_HELP_STRING([--with-systemdsystemgeneratordir=DIR], [Directory for systemd generators]),
              [],
              [with_systemdsystemgeneratordir=$($PKG_CONFIG --variable=systemdsystemgeneratordir systemd)])
  AS_IF([test "x$with_systemdsystemgeneratordir" != "xno"], [
    AC_SUBST([systemdsystemgeneratordir], [$with_systemdsystemgeneratordir])
  ])
])
AM_CONDITIONAL(BUILDOPT_SYSTEMD, test x$with_systemd = xyes)
dnl If we have both, we use the "new /var" model with ostree-system-generator
AM_CONDITIONAL(BUILDOPT_SYSTEMD_AND_LIBMOUNT,[test x$with_systemd = xyes && test x$with_libmount = xyes])
AM_COND_IF(BUILDOPT_SYSTEMD_AND_LIBMOUNT,
  AC_DEFINE([BUILDOPT_LIBSYSTEMD_AND_LIBMOUNT], 1, [Define if systemd and libmount]))

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

dnl for tests (but we can't use asan with gjs or any introspection,
dnl see https://github.com/google/sanitizers/wiki/AddressSanitizerAsDso for more info)
AS_IF([test "x$found_introspection" = xyes && test x$using_asan != xyes], [
  AC_PATH_PROG(GJS, [gjs])
  if test -n "$GJS"; then
    have_gjs=yes
  else
    have_gjs=no
  fi
], [have_gjs=no])
AM_CONDITIONAL(BUILDOPT_GJS, test x$have_gjs = xyes)

# Do we enable building experimental (non-stable) API?
# The OSTREE_ENABLE_EXPERIMENTAL_API #define is used internally and in public
# headers, so any consumer of libostree who wants to use experimental API must
#    #define OSTREE_ENABLE_EXPERIMENTAL_API 1
# before including libostree headers. This means the name in the AC_DEFINE below
# is public API.
AC_ARG_ENABLE([experimental-api],
  [AS_HELP_STRING([--enable-experimental-api],
                  [Enable unstable experimental API in libostree [default=no]])],,
  [enable_experimental_api=no])
AS_IF([test x$enable_experimental_api = xyes],
  [AC_DEFINE([OSTREE_ENABLE_EXPERIMENTAL_API],[1],[Define if experimental API should be enabled])
   OSTREE_FEATURES="$OSTREE_FEATURES experimental"]
)
AM_CONDITIONAL([ENABLE_EXPERIMENTAL_API],[test x$enable_experimental_api = xyes])
AM_CONDITIONAL([BUILDOPT_IS_DEVEL_BUILD],[test x$is_release_build != xyes])
AM_COND_IF([BUILDOPT_IS_DEVEL_BUILD],
            AC_DEFINE([BUILDOPT_IS_DEVEL_BUILD], [1], [Define if doing a development build])
            release_build_type=devel,
            release_build_type=release)
OSTREE_FEATURES="$OSTREE_FEATURES $release_build_type"

AC_CONFIG_FILES([
Makefile
apidoc/Makefile
src/libostree/ostree-1.pc
src/libostree/ostree-version.h
])
AC_OUTPUT

echo "
    libOSTree $VERSION ($release_build_type)
    ===============


    introspection:                                $found_introspection
    Rust (internal oxidation):                    $rust_debug_release
    rofiles-fuse:                                 $enable_rofiles_fuse
    HTTP backend:                                 $fetcher_backend
    \"ostree trivial-httpd\":                       $enable_trivial_httpd_cmdline
    SELinux:                                      $with_selinux
    OpenSSL libcrypto (checksums):                $with_openssl
    systemd:                                      $have_libsystemd
    libmount:                                     $with_libmount
    libarchive (parse tar files directly):        $with_libarchive
    static deltas:                                yes (always enabled now)
    O_TMPFILE:                                    $enable_otmpfile
    wrpseudo-compat:                              $enable_wrpseudo_compat
    man pages (xsltproc):                         $enable_man
    api docs (gtk-doc):                           $enable_gtk_doc
    installed tests:                              $enable_installed_tests
    gjs-based tests:                              $have_gjs
    dracut:                                       $with_dracut
    mkinitcpio:                                   $with_mkinitcpio
    Static compiler for ostree-prepare-root:      $with_static_compiler
    Experimental API                              $enable_experimental_api"
AS_IF([test x$with_builtin_grub2_mkconfig = xyes], [
    echo "    builtin grub2-mkconfig (instead of system):   $with_builtin_grub2_mkconfig"
], [
    echo "    grub2-mkconfig path:                          $GRUB2_MKCONFIG"
])
echo ""

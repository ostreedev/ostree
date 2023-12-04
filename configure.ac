AC_PREREQ([2.63])
dnl To perform a release, follow the instructions in `docs/CONTRIBUTING.md`.
m4_define([year_version], [2023])
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

dnl automake variables we want in pkg-config
pkglibexecdir=$libexecdir/$PACKAGE
AC_SUBST(pkglibexecdir)

AS_IF([echo "$CFLAGS" | grep -q -E -e '-Werror($| )'], [], [
CC_CHECK_FLAGS_APPEND([WARN_CFLAGS], [CFLAGS], [\
  -pipe \
  -Wall \
  -Werror=shadow \
  -Werror=empty-body \
  -Werror=strict-prototypes \
  -Werror=missing-prototypes \
  -Werror=implicit-function-declaration \
  "-Werror=format=2 -Werror=format-security -Werror=format-nonliteral" \
  -Werror=pointer-arith -Werror=init-self \
  -Werror=missing-declarations \
  -Werror=return-type \
  -Werror=switch \
  -Werror=overflow \
  -Werror=int-conversion \
  -Werror=parentheses \
  -Werror=undef \
  -Werror=incompatible-pointer-types \
  -Werror=misleading-indentation \
  -Werror=missing-include-dirs  \
  -Wstrict-aliasing=2 \
  -Werror=unused-result \
])])
AC_SUBST(WARN_CFLAGS)

AC_ARG_ENABLE(sanitizers,
              AS_HELP_STRING([--enable-sanitizers],
                             [Enable ASAN and UBSAN (default: no)]),,
              [enable_sanitizers=no])
AM_CONDITIONAL(BUILDOPT_ASAN, [test x$enable_sanitizers != xno])
AM_COND_IF([BUILDOPT_ASAN], [
  sanitizer_flags="-fsanitize=address -fsanitize=undefined -fsanitize-undefined-trap-on-error"
  CFLAGS="$CFLAGS ${sanitizer_flags}"
  CXXFLAGS="$CXXFLAGS ${sanitizer_flags}"
  AC_DEFINE([BUILDOPT_ASAN], 1, [Define if we are building with asan and ubsan])
])

AC_MSG_CHECKING([for -fsanitize=thread in CFLAGS])
if echo $CFLAGS | grep -q -e -fsanitize=thread; then
  AC_MSG_RESULT([yes])
  using_tsan=yes
else
  AC_MSG_RESULT([no])
fi
AM_CONDITIONAL(BUILDOPT_TSAN, [test x$using_tsan = xyes])
AM_COND_IF([BUILDOPT_TSAN],
  [AC_DEFINE([BUILDOPT_TSAN], 1, [Define if we are building with -fsanitize=thread])])

# Initialize libtool
LT_PREREQ([2.2.4])
LT_INIT([disable-static])

dnl We have an always-on feature now to signify the fix for 
dnl https://github.com/ostreedev/ostree/pull/2874/commits/de6fddc6adee09a93901243dc7074090828a1912
dnl "commit: fix ostree deployment on 64-bit inode fs"
OSTREE_FEATURES="inode64"
AC_SUBST([OSTREE_FEATURES])

GLIB_TESTS
LIBGLNX_CONFIGURE

dnl These bits attempt to mirror https://github.com/coreutils/gnulib/blob/e369b04cca4da1534c98628b8ee4648bfca2bb3a/m4/parse-datetime.m4#L27
AC_CHECK_FUNCS([nanotime clock_gettime])
AC_STRUCT_TIMEZONE
AC_CHECK_HEADER([sys/xattr.h],,[AC_MSG_ERROR([You must have sys/xattr.h from glibc])])

AS_IF([test "$YACC" != "bison -y"], [AC_MSG_ERROR([bison not found but required])])

PKG_PROG_PKG_CONFIG

# PKG_CHECK_VAR added to pkg-config 0.28
m4_define_default(
    [PKG_CHECK_VAR],
    [AC_ARG_VAR([$1], [value of $3 for $2, overriding pkg-config])
     AS_IF([test -z "$$1"], [$1=`$PKG_CONFIG --variable="$3" "$2"`])
     AS_IF([test -n "$$1"], [$4], [$5])])

PKG_CHECK_VAR(BASH_COMPLETIONSDIR, [bash-completion], [completionsdir], ,
  BASH_COMPLETIONSDIR="${datadir}/bash-completion/completions")
AC_SUBST(BASH_COMPLETIONSDIR)

AM_PATH_GLIB_2_0(,,AC_MSG_ERROR([GLib not found]))

dnl When bumping the gio-unix-2.0 dependency (or glib-2.0 in general),
dnl remember to bump GLIB_VERSION_MIN_REQUIRED and
dnl GLIB_VERSION_MAX_ALLOWED in Makefile.am
GIO_DEPENDENCY="gio-unix-2.0 >= 2.66.0"
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
AC_ARG_ENABLE(http2,
AS_HELP_STRING([--disable-http2],
  [Disable use of http2 (default: no)]),,
  [enable_http2=yes])
AS_IF([test x$enable_http2 != xno ], [
  AC_DEFINE([BUILDOPT_HTTP2], 1, [Define if we enable http2 by default])
], [
  OSTREE_FEATURES="$OSTREE_FEATURES no-http2"
])

SOUP3_DEPENDENCY="libsoup-3.0 >= 3.0.0"
AC_ARG_WITH(soup3,
	    AS_HELP_STRING([--with-soup3], [Use libsoup3 @<:@default=no@:>@]),
	    [], [with_soup3=check])
AS_IF([test x$with_soup3 != xno], [
    AC_MSG_CHECKING([for $SOUP3_DEPENDENCY])
    PKG_CHECK_EXISTS($SOUP3_DEPENDENCY, have_soup3=yes, have_soup3=no)
    AC_MSG_RESULT([$have_soup3])
    AS_IF([ test x$have_soup3 = xno && test x$with_soup3 != xcheck], [
       AC_MSG_ERROR([libsoup3 is enabled but could not be found])
    ])
    AS_IF([test x$have_soup3 = xyes], [
      with_soup3=yes
      PKG_CHECK_MODULES(OT_DEP_SOUP3, $SOUP3_DEPENDENCY)
      AC_DEFINE([HAVE_LIBSOUP3], 1, [Define if we have libsoup3])
      OSTREE_FEATURES="$OSTREE_FEATURES libsoup3"
      dnl And disable libsoup2
      with_soup_default=no
      dnl soup3 always supports client certs
      have_libsoup_client_certs=yes
    ], [with_soup3=no])
], [with_soup_default=check])
AM_CONDITIONAL(USE_LIBSOUP3, test x$with_soup3 = xyes)

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

dnl Some components use either soup2 or soup3.
AM_CONDITIONAL([USE_LIBSOUP_OR_LIBSOUP3],
               [test x$with_soup = xyes || test x$with_soup3 = xyes])
AS_IF([test x$with_soup = xyes || test x$with_soup3 = xyes], [
  AC_DEFINE([HAVE_LIBSOUP_OR_LIBSOUP3], 1, [Define if we have libsoup.pc or libsoup3.pc])
])

AS_IF([test x$with_curl = xyes && test x$with_soup = xno && test x$with_soup3 = xno], [
  AC_MSG_WARN([Curl enabled, but libsoup is not; libsoup is needed for tests (make check, etc.)])
])
AM_CONDITIONAL(USE_CURL_OR_SOUP, test x$with_curl != xno || test x$with_soup != xno || test x$with_soup3 != xno)
AS_IF([test x$with_curl != xno || test x$with_soup != xno || test x$with_soup3 != xno],
            [AC_DEFINE([HAVE_LIBCURL_OR_LIBSOUP], 1, [Define if we have soup or curl])])
AS_IF([test x$with_curl = xyes], [fetcher_backend=curl],
      [test x$with_soup = xyes], [fetcher_backend=libsoup],
      [test x$with_soup3 = xyes], [fetcher_backend=libsoup3],
      [fetcher_backend=none])

m4_ifdef([GOBJECT_INTROSPECTION_CHECK], [
  GOBJECT_INTROSPECTION_CHECK([1.51.5])
])
AM_CONDITIONAL(BUILDOPT_INTROSPECTION, test "x$found_introspection" = xyes)

LIBGPGME_DEPENDENCY="1.8.0"
LIBGPGME_PTHREAD_DEPENDENCY="1.1.8"
AC_ARG_WITH(gpgme,
	    AS_HELP_STRING([--with-gpgme], [Use gpgme @<:@default=yes@:>@]),
	    [], [with_gpgme=yes])
AS_IF([test x$with_gpgme != xno], [
    have_gpgme=yes
    PKG_CHECK_MODULES([OT_DEP_GPGME], [gpgme >= $LIBGPGME_DEPENDENCY gpg-error], [have_gpgme=yes], [have_gpgme=no])
    ]
)
AS_IF([test x$with_gpgme != xno && test x$have_gpgme != xyes], [
    PKG_CHECK_MODULES(OT_DEP_GPGME, gpgme-pthread >= $LIBGPGME_PTHREAD_DEPENDENCY, have_gpgme=yes, [
        m4_ifdef([AM_PATH_GPGME_PTHREAD], [
            AM_PATH_GPGME_PTHREAD($LIBGPGME_PTHREAD_DEPENDENCY, have_gpgme=yes, have_gpgme=no)
        ],[ have_gpgme=no ])
    ])
    AS_IF([ test x$have_gpgme = xno ], [
       AC_MSG_ERROR([Need GPGME_PTHREAD version $LIBGPGME_PTHREAD_DEPENDENCY or later])
    ])
    OSTREE_FEATURES="$OSTREE_FEATURES gpgme"
    PKG_CHECK_MODULES(OT_DEP_GPG_ERROR, [gpg-error], [], [
dnl This apparently doesn't ship a pkg-config file either, and we need
dnl to link to it directly.
        AC_PATH_PROG(GPG_ERROR_CONFIG, [gpg-error-config], [AC_MSG_ERROR([Missing gpg-error-config])])
        OT_DEP_GPG_ERROR_CFLAGS="$( $GPG_ERROR_CONFIG --cflags )"
        OT_DEP_GPG_ERROR_LIBS="$( $GPG_ERROR_CONFIG --libs )"
    ])
    OT_DEP_GPGME_CFLAGS="${OT_DEP_GPGME_CFLAGS} ${OT_DEP_GPG_ERROR_CFLAGS}"
    OT_DEP_GPGME_LIBS="${OT_DEP_GPGME_LIBS} ${OT_DEP_GPG_ERROR_LIBS}"
    ]
)
AS_IF([test x$with_gpgme != xno && test x$have_gpgme != xyes],
      [AC_MSG_ERROR([Need GPGME_PTHREAD and GPG_ERROR])]
)
AS_IF([test x$have_gpgme = xyes],
    [ OSTREE_FEATURES="$OSTREE_FEATURES gpgme" ],
    [
    AC_DEFINE([OSTREE_DISABLE_GPGME], 1, [Define to disable internal GPGME support])
    have_gpgme=no
    ]
)
AM_CONDITIONAL(USE_GPGME, test "x$have_gpgme" = xyes)

dnl composefs won't work at all without this
AC_MSG_CHECKING([for MOUNT_ATTR_IDMAP])
AC_COMPILE_IFELSE(
	[AC_LANG_PROGRAM([
			#include <sys/mount.h>
			#include <linux/mount.h>
  ],[int foo = MOUNT_ATTR_IDMAP;]
	)],
	[AC_MSG_RESULT(yes)
	 have_mount_attr_idmap=yes],
	[AC_MSG_RESULT(no)])
dnl These are needed by libcomposefs to use the new mount API optionally
AC_MSG_CHECKING([for new mount API (fsconfig)])
AC_COMPILE_IFELSE(
	[AC_LANG_SOURCE([[
			#include <sys/mount.h>
			int cmd = FSCONFIG_CMD_CREATE;
	]])],
	[AC_MSG_RESULT(yes)
		 AC_DEFINE([HAVE_FSCONFIG_CMD_CREATE_SYS_MOUNT_H], 1, [Define if FSCONFIG_CMD_CREATE is available in sys/mount.h])],
	[AC_MSG_RESULT(no)])
AC_COMPILE_IFELSE(
	[AC_LANG_SOURCE([[
			/* also make sure it doesn't conflict with <sys/mount.h> since it is always used.  */
			#include <sys/mount.h>
			#include <linux/mount.h>
			int cmd = FSCONFIG_CMD_CREATE;
	]])],
	[AC_MSG_RESULT(yes)
		 AC_DEFINE([HAVE_FSCONFIG_CMD_CREATE_LINUX_MOUNT_H], 1, [Define if FSCONFIG_CMD_CREATE is available in linux/mount.h])],
	[AC_MSG_RESULT(no)])

composefs_default=yes
if test x"$have_mount_attr_idmap" != xyes; then
  composefs_default=no
fi
AC_ARG_WITH(composefs,
	    AS_HELP_STRING([--with-composefs], [Support composefs]),
	    :, with_composefs=$composefs_default)

if test x$with_composefs != xno; then OSTREE_FEATURES="$OSTREE_FEATURES composefs";
   AC_DEFINE([HAVE_COMPOSEFS], 1, [Define if we have libcomposefs])
fi
AM_CONDITIONAL(USE_COMPOSEFS, test $with_composefs != no)

LIBSODIUM_DEPENDENCY="1.0.14"
AC_ARG_WITH(ed25519_libsodium,
	    AS_HELP_STRING([--with-ed25519-libsodium], [Use libsodium for ed25519 @<:@default=no@:>@]),
	    [], [with_ed25519_libsodium=no])
AS_IF([test x$with_ed25519_libsodium != xno], [
    AC_DEFINE([HAVE_LIBSODIUM], 1, [Define if using libsodium])
    PKG_CHECK_MODULES(OT_DEP_LIBSODIUM, libsodium >= $LIBSODIUM_DEPENDENCY, have_libsodium=yes,  have_libsodium=no)
    AS_IF([ test x$have_libsodium = xno ], [
       AC_MSG_ERROR([Need LIBSODIUM version $LIBSODIUM_DEPENDENCY or later])
    ])
], with_ed25519_libsodium=no )
AM_CONDITIONAL(USE_LIBSODIUM, test "x$have_libsodium" = xyes)

LIBARCHIVE_DEPENDENCY="libarchive >= 2.8.0"
FUSE3_DEPENDENCY="fuse3 >= 3.1.1"
# What's in RHEL7.2.
FUSE_DEPENDENCY="fuse >= 2.9.2"

AC_CHECK_HEADERS([linux/fsverity.h])
AS_IF([test x$ac_cv_header_linux_fsverity_h = xyes ],
  [OSTREE_FEATURES="$OSTREE_FEATURES ex-fsverity"])

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

AC_ARG_ENABLE([man-html],
              [AS_HELP_STRING([--enable-man-html],
                              [generate man HTML pages [default=no]])],,
              enable_man_html=no)

AS_IF([test "$enable_man_html" = yes && test "$enable_man" = no], [
  AC_MSG_ERROR([--enable-man is required for --enable-man-html])
])
AM_CONDITIONAL(ENABLE_MAN_HTML, test "$enable_man_html" = yes)

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

dnl crypto
AC_ARG_WITH(crypto,
AS_HELP_STRING([--with-crypto], [Choose library for checksums, one of glib, openssl, gnutls (default: glib)]),
:, with_crypto=glib)

AS_IF([test $with_crypto = glib],
      [],
      [test $with_crypto = openssl],
      [with_openssl=yes],
      [test $with_crypto = gnutls],
      [],
      [AC_MSG_ERROR([Invalid --with-crypto $with_crypto])]
      )

dnl begin openssl (really just libcrypto right now)
dnl Note this option is now deprecated in favor of --with-crypto=openssl
OPENSSL_DEPENDENCY="libcrypto >= 1.0.1"
AC_ARG_WITH(openssl,
AS_HELP_STRING([--with-openssl], [Enable use of OpenSSL libcrypto (checksums)]),with_openssl=$withval,with_openssl=no)
AS_IF([ test x$with_openssl != xno ], [
      PKG_CHECK_MODULES(OT_DEP_CRYPTO, $OPENSSL_DEPENDENCY)
      AC_DEFINE([HAVE_OPENSSL], 1, [Define if we have openssl])
      with_crypto=openssl
      with_openssl=yes
], [
      with_openssl=no
])
if test x$with_openssl != xno; then OSTREE_FEATURES="$OSTREE_FEATURES openssl"; fi
AM_CONDITIONAL(USE_OPENSSL, test $with_openssl != no)
dnl end openssl

if test x$with_openssl != xno || test x$with_ed25519_libsodium != xno; then
   AC_DEFINE([HAVE_ED25519], 1, [Define if ed25519 is supported ])
   OSTREE_FEATURES="$OSTREE_FEATURES sign-ed25519"
fi

dnl begin gnutls; in contrast to openssl this one only
dnl supports --with-crypto=gnutls
GNUTLS_DEPENDENCY="gnutls >= 3.5.0"
AS_IF([ test $with_crypto = gnutls ], [
      PKG_CHECK_MODULES(OT_DEP_CRYPTO, $GNUTLS_DEPENDENCY)
      AC_DEFINE([HAVE_GNUTLS], 1, [Define if we have gnutls])
      OSTREE_FEATURES="$OSTREE_FEATURES gnutls"
])
AM_CONDITIONAL(USE_GNUTLS, test $with_crypto = gnutls)
dnl end gnutls

dnl we always inject libsodium into our crypto deps in addition to openssl/gnutls
OT_DEP_CRYPTO_CFLAGS="${OT_DEP_CRYPTO_CFLAGS} ${OT_DEP_LIBSODIUM_CFLAGS}"
OT_DEP_CRYPTO_LIBS="${OT_DEP_CRYPTO_LIBS} ${OT_DEP_LIBSODIUM_LIBS}"

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
    PKG_CHECK_MODULES([FUSE3], [$FUSE3_DEPENDENCY],
                      [
                        FUSE_USE_VERSION=31
                        BUILDOPT_FUSE_CFLAGS="$FUSE3_CFLAGS"
                        BUILDOPT_FUSE_LIBS="$FUSE3_LIBS"
                      ],
                      [PKG_CHECK_MODULES([FUSE], [$FUSE_DEPENDENCY],
                                         [
                                           FUSE_USE_VERSION=26
                                           BUILDOPT_FUSE_CFLAGS="$FUSE_CFLAGS"
                                           BUILDOPT_FUSE_LIBS="$FUSE_LIBS"
                                         ])])
    AC_DEFINE_UNQUOTED([FUSE_USE_VERSION], [$FUSE_USE_VERSION], [Define to the FUSE API version])
    AC_SUBST([BUILDOPT_FUSE_CFLAGS])
    AC_SUBST([BUILDOPT_FUSE_LIBS])
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
AC_ARG_WITH(libsystemd,
            AS_HELP_STRING([--without-libsystemd], [Do not use libsystemd]),
            :, with_libsystemd=maybe)

AS_IF([ test x$with_libsystemd != xno ], [
    AC_MSG_CHECKING([for libsystemd])
    PKG_CHECK_EXISTS(libsystemd, have_libsystemd=yes, have_libsystemd=no)
    AC_MSG_RESULT([$have_libsystemd])
    AS_IF([ test x$have_libsystemd = xno && test x$with_libsystemd != xmaybe ], [
       AC_MSG_ERROR([libsystemd is enabled but could not be found])
    ])
    AS_IF([ test x$have_libsystemd = xyes], [
        AC_DEFINE([HAVE_LIBSYSTEMD], 1, [Define if we have libsystemd.pc])
        PKG_CHECK_MODULES([LIBSYSTEMD], [libsystemd])
        with_libsystemd=yes
    ], [
        with_libsystemd=no
    ])
], [ with_libsystemd=no ])

AS_IF([test "x$with_libsystemd" = "xyes"], [
  AC_ARG_WITH([systemdsystemunitdir],
              AS_HELP_STRING([--with-systemdsystemunitdir=DIR], [Directory for systemd service files]),
              [],
              [with_systemdsystemunitdir=$($PKG_CONFIG --variable=systemdsystemunitdir systemd)])
  AS_IF([test "x$with_systemdsystemunitdir" != "xno"], [
    AC_SUBST([systemdsystemunitdir], [$with_systemdsystemunitdir])
    AC_DEFINE_UNQUOTED([SYSTEM_DATA_UNIT_PATH], ["$with_systemdsystemunitdir"], ["unit path"])
  ])
  AC_ARG_WITH([systemdsystemgeneratordir],
              AS_HELP_STRING([--with-systemdsystemgeneratordir=DIR], [Directory for systemd generators]),
              [],
              [with_systemdsystemgeneratordir=$($PKG_CONFIG --variable=systemdsystemgeneratordir systemd)])
  AS_IF([test "x$with_systemdsystemgeneratordir" != "xno"], [
    AC_SUBST([systemdsystemgeneratordir], [$with_systemdsystemgeneratordir])
  ])
])
AM_CONDITIONAL(BUILDOPT_SYSTEMD, test x$with_libsystemd = xyes)
dnl If we have both, we use the "new /var" model with ostree-system-generator
AM_CONDITIONAL(BUILDOPT_SYSTEMD_AND_LIBMOUNT,[test x$with_libsystemd = xyes && test x$with_libmount = xyes])
AM_COND_IF(BUILDOPT_SYSTEMD_AND_LIBMOUNT,
  AC_DEFINE([BUILDOPT_LIBSYSTEMD_AND_LIBMOUNT], 1, [Define if systemd and libmount]))
if test x$with_libsystemd = xyes; then OSTREE_FEATURES="$OSTREE_FEATURES systemd"; fi

AC_ARG_WITH(modern-grub,
	    AS_HELP_STRING([--with-modern-grub],
                           [Omit grub linux and initrd suffixes for EFI/BIOS booting on GRUB >2.02 (default: no)]),,
              [with_modern_grub=no])
AS_IF([ test x$with_modern_grub = xyes], [
  AC_DEFINE([WITH_MODERN_GRUB], 1, [Define if we have a GRUB version newer than 2.02])
])

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
AC_DEFINE_UNQUOTED([GRUB2_MKCONFIG_PATH], ["$GRUB2_MKCONFIG"], [The system grub2-mkconfig executable name])

AC_ARG_WITH(static-compiler,
            AS_HELP_STRING([--with-static-compiler],
                           [Use the given compiler to build ostree-prepare-root statically linked (default: no)]),,
              [with_static_compiler=no])
AM_CONDITIONAL(BUILDOPT_USE_STATIC_COMPILER, test "x$with_static_compiler" != xno)
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

AM_CONDITIONAL([BUILDOPT_IS_DEVEL_BUILD],[test x$is_release_build != xyes])
AM_COND_IF([BUILDOPT_IS_DEVEL_BUILD],
            AC_DEFINE([BUILDOPT_IS_DEVEL_BUILD], [1], [Define if doing a development build])
            release_build_type=devel,
            release_build_type=release)
OSTREE_FEATURES="$OSTREE_FEATURES $release_build_type"

# P2P API is public in OSTree >= 2018.6
OSTREE_FEATURES="$OSTREE_FEATURES p2p"

# Strip leading whitespace
OSTREE_FEATURES=$(echo ${OSTREE_FEATURES})

AC_CONFIG_FILES([
Makefile
apidoc/Makefile
src/libostree/ostree-1.pc
src/libostree/ostree-version.h
])
AC_OUTPUT

echo "
    libostree $VERSION ($release_build_type)
    features: $OSTREE_FEATURES
    ===============


    introspection:                                $found_introspection
    rofiles-fuse:                                 $enable_rofiles_fuse
    HTTP backend:                                 $fetcher_backend
    libsoup:                                      $with_soup
    libsoup3:                                     $with_soup3
    SELinux:                                      $with_selinux
    fs-verity:                                    $ac_cv_header_linux_fsverity_h
    cryptographic checksums:                      $with_crypto
    systemd:                                      $with_libsystemd
    libmount:                                     $with_libmount
    libsodium (ed25519 signatures):               $with_ed25519_libsodium
    openssl (ed25519 signatures):                 $with_openssl
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
    Composefs:                                    $with_composefs"
AS_IF([test x$with_builtin_grub2_mkconfig = xyes], [
    echo "    builtin grub2-mkconfig (instead of system):   $with_builtin_grub2_mkconfig"
], [
    echo "    grub2-mkconfig path:                          $GRUB2_MKCONFIG"
])
echo ""

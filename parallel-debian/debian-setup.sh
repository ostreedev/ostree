#!/bin/sh
# This script sets up things we want to ship with the OS tree.  It should
# NOT set up caches.  For example, do NOT run ldconfig in here.

set -e
set -x

echo gnomeos >./etc/hostname

cat >./etc/default/locale <<EOF
LANG="en_US.UTF-8"
EOF

cp -p ./usr/share/sysvinit/inittab ./etc/inittab
cp -p ./usr/share/base-files/nsswitch.conf ./etc/nsswitch.conf

cat >./etc/pam.d/common-account <<EOF
account [success=1 new_authtok_reqd=done default=ignore]        pam_unix.so 
account requisite                       pam_deny.so
account required                        pam_permit.so
EOF
cat >./etc/pam.d/common-auth <<EOF
auth    [success=1 default=ignore]      pam_unix.so nullok_secure
auth    requisite                       pam_deny.so
auth    required                        pam_permit.so
EOF
cat >./etc/pam.d/common-password <<EOF
password        [success=1 default=ignore]      pam_unix.so obscure sha512
password        requisite                       pam_deny.so
password        required                        pam_permit.so
EOF
cat >./etc/pam.d/common-session <<EOF
session [default=1]                     pam_permit.so
session requisite                       pam_deny.so
session required                        pam_permit.so
session required        pam_unix.so 
EOF

# base-passwd
cp -p ./usr/share/base-passwd/passwd.master ./etc/passwd
cp -p ./usr/share/base-passwd/group.master ./etc/group

# Service rc.d defaults
setuprc () {
    name=$1
    shift
    type=$1
    shift
    priority=$1
    shift
    
    for x in $@; do
	ln -s ../init.d/$name ./etc/rc${x}.d/${type}${priority}${name}
    done
}
    
setuprc rsyslog S 10 2 3 4 5
setuprc rsyslog S 30 0 6 
setuprc rsyslog K 90 1
setuprc cron S 89 2 3 4 5

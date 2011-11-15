### BEGIN INIT INFO
# Provides:          bootmisc
# Required-Start:    $local_fs mountvirtfs
# Required-Stop:     $local_fs
# Default-Start:     S
# Default-Stop:      0 6
# Short-Description: Misc and other.
### END INIT INFO

. /etc/default/rcS
#
# Put a nologin file in /etc to prevent people from logging in before
# system startup is complete.
#
if test "$DELAYLOGIN" = yes
then
  echo "System bootup in progress - please wait" > /etc/nologin
  cp /etc/nologin /etc/nologin.boot
fi

#
# Set pseudo-terminal access permissions.
#
if test -c /dev/ttyp0
then
	chmod 666 /dev/tty[p-za-e][0-9a-f]
	chown root:tty /dev/tty[p-za-e][0-9a-f]
fi

#
# Apply /proc settings if defined
#
SYSCTL_CONF="/etc/sysctl.conf"
if [ -f "${SYSCTL_CONF}" ]
then
	if [ -x "/sbin/sysctl" ]
	then
		/sbin/sysctl -p "${SYSCTL_CONF}"
	else
		echo "To have ${SYSCTL_CONF} applied during boot, install package <procps>."
	fi
fi

#
# Update /etc/motd.
#
if test "$EDITMOTD" != no
then
	uname -a > /etc/motd.tmp
	sed 1d /etc/motd >> /etc/motd.tmp
	mv /etc/motd.tmp /etc/motd
fi

#
# This is as good a place as any for a sanity check
# /tmp should be a symlink to /var/tmp to cut down on the number
# of mounted ramdisks.
if test ! -L /tmp && test -d /var/tmp
then
	rm -rf /tmp
	ln -sf /var/tmp /tmp
fi

# Set the system clock from hardware clock
# If the timestamp is 1 day or more recent than the current time,
# use the timestamp instead.
/etc/init.d/hwclock.sh start
if test -e /etc/timestamp
then
	SYSTEMDATE=`date  -u +%2m%2d%2H%2M%4Y`
	read TIMESTAMP < /etc/timestamp
        NEEDUPDATE=`expr \( $TIMESTAMP \> $SYSTEMDATE + 10000 \)`
        if [ $NEEDUPDATE -eq 1 ]; then 
		date -u $TIMESTAMP
		/etc/init.d/hwclock.sh stop
	fi
fi
: exit 0

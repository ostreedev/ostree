#! /bin/sh
### BEGIN INIT INFO
# Provides:          mountall
# Required-Start:    checkfs
# Required-Stop: 
# Default-Start:     S
# Default-Stop:
# Short-Description: Mount all filesystems.
# Description:
### END INIT INFO

PATH=/sbin:/bin
. /lib/init/vars.sh

. /lib/lsb/init-functions
. /lib/init/mount-functions.sh
. /lib/init/swap-functions.sh

# for ntfs-3g to get correct file name encoding
if [ -r /etc/default/locale ]; then
	. /etc/default/locale
	export LANG
fi

do_start() {
	#
	# Mount local file systems in /etc/fstab.
	#
	mount_all_local() {
	    mount -a -t nonfs,nfs4,smbfs,cifs,ncp,ncpfs,coda,ocfs2,gfs,gfs2 \
		-O no_netdev
	}
	pre_mountall
	if [ "$VERBOSE" = no ]
	then
		log_action_begin_msg "Mounting local filesystems"
		mount_all_local
		log_action_end_msg $?
	else
		log_daemon_msg "Will now mount local filesystems"
		mount_all_local
		log_end_msg $?
	fi
	post_mountall

	case "$(uname -s)" in
	  *FreeBSD)
		INITCTL=/etc/.initctl
		;;
	  *)
		INITCTL=/dev/initctl
		;;
	esac

	#
	# We might have mounted something over /dev, see if
	# /dev/initctl is there.  Look for /usr/share/sysvinit/update-rc.d
	# to verify that sysvinit (and not upstart) is installed).
	#
	if [ ! -p $INITCTL ] && [ -f /usr/share/sysvinit/update-rc.d ]; then
		rm -f $INITCTL
		mknod -m 600 $INITCTL p
		kill -USR1 1
	fi

	# Execute swapon command again, in case we want to swap to
	# a file on a now mounted filesystem.
	swaponagain 'swapfile'
}

case "$1" in
  start|"")
	do_start
	;;
  restart|reload|force-reload)
	echo "Error: argument '$1' not supported" >&2
	exit 3
	;;
  stop)
	# No-op
	;;
  *)
	echo "Usage: mountall.sh [start|stop]" >&2
	exit 3
	;;
esac

:

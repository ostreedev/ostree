#!/bin/sh
### BEGIN INIT INFO
# Provides:          finish.sh
# Required-Start:    $remote_fs rmnologin
# Required-Stop:
# Default-Start:     2 3 4 5
# Default-Stop:
# Short-Description: Finish system start
# Description:       
### END INIT INFO

if ! test -e /etc/.configured; then
	> /etc/.configured
fi

SUMMARY = "SysV init scripts"
DESCRIPTION = "Initscripts provide the basic system startup initialization scripts for the system.  These scripts include actions such as filesystem mounting, fsck, RTC manipulation and other actions routinely performed at system startup.  In addition, the scripts are also used during system shutdown to reverse the actions performed at startup."
SECTION = "base"
LICENSE = "GPLv2"
LIC_FILES_CHKSUM = "file://COPYING;md5=751419260aa954499f7abaabaa882bbe"
PR = "r666"

INHIBIT_DEFAULT_DEPS = "1"

SRC_URI = "file://functions \
           file://halt \
           file://umountfs \
           file://hostname.sh \
           file://mountall.sh \
           file://banner.sh \
           file://finish.sh \
           file://mountnfs.sh \
           file://NetworkManager \
           file://dbus \
           file://gnomemisc \
           file://reboot \
           file://single \
           file://sendsigs \
           file://udev \
           file://urandom \
           file://rmnologin.sh \
           file://umountnfs.sh \
           file://device_table.txt \
           file://save-rtc.sh \
	   file://GPLv2.patch"

SRC_URI_append_arm = " file://alignment.sh"

KERNEL_VERSION = ""

HALTARGS ?= "-d -f"

do_configure() {
	sed -i -e "s:SED_HALTARGS:${HALTARGS}:g" ${WORKDIR}/halt
	sed -i -e "s:SED_HALTARGS:${HALTARGS}:g" ${WORKDIR}/reboot
}

do_install () {
#
# Create directories and install device independent scripts
#
	install -d ${D}${sysconfdir}/init.d
	install -d ${D}${sysconfdir}/rcS.d
	install -d ${D}${sysconfdir}/rc0.d
	install -d ${D}${sysconfdir}/rc1.d
	install -d ${D}${sysconfdir}/rc2.d
	install -d ${D}${sysconfdir}/rc3.d
	install -d ${D}${sysconfdir}/rc4.d
	install -d ${D}${sysconfdir}/rc5.d
	install -d ${D}${sysconfdir}/rc6.d
	install -d ${D}${sysconfdir}/default

	install -m 0644    ${WORKDIR}/functions		${D}${sysconfdir}/init.d
#	install -m 0755    ${WORKDIR}/finish.sh		${D}${sysconfdir}/init.d
	install -m 0755    ${WORKDIR}/halt		${D}${sysconfdir}/init.d
	install -m 0755    ${WORKDIR}/hostname.sh	${D}${sysconfdir}/init.d
	install -m 0755    ${WORKDIR}/mountall.sh	${D}${sysconfdir}/init.d
	install -m 0755    ${WORKDIR}/NetworkManager	${D}${sysconfdir}/init.d
	install -m 0755    ${WORKDIR}/gnomemisc		${D}${sysconfdir}/init.d
	install -m 0755    ${WORKDIR}/dbus              ${D}${sysconfdir}/init.d
	install -m 0755    ${WORKDIR}/mountnfs.sh	${D}${sysconfdir}/init.d
	install -m 0755    ${WORKDIR}/reboot		${D}${sysconfdir}/init.d
	install -m 0755    ${WORKDIR}/rmnologin.sh	${D}${sysconfdir}/init.d
	install -m 0755    ${WORKDIR}/sendsigs		${D}${sysconfdir}/init.d
	install -m 0755    ${WORKDIR}/single		${D}${sysconfdir}/init.d
	install -m 0755    ${WORKDIR}/udev		${D}${sysconfdir}/init.d
	install -m 0755    ${WORKDIR}/umountnfs.sh	${D}${sysconfdir}/init.d
	install -m 0755    ${WORKDIR}/urandom		${D}${sysconfdir}/init.d
	install -m 0755    ${WORKDIR}/save-rtc.sh	${D}${sysconfdir}/init.d
#
# Install device dependent scripts
#
	install -m 0755 ${WORKDIR}/banner.sh	${D}${sysconfdir}/init.d/banner.sh
	install -m 0755 ${WORKDIR}/umountfs	${D}${sysconfdir}/init.d/umountfs
#
# Create runlevel links
#
	ln -sf		../init.d/rmnologin.sh	${D}${sysconfdir}/rc2.d/S99rmnologin.sh
	ln -sf		../init.d/rmnologin.sh	${D}${sysconfdir}/rc3.d/S99rmnologin.sh
	ln -sf		../init.d/rmnologin.sh	${D}${sysconfdir}/rc4.d/S99rmnologin.sh
	ln -sf		../init.d/rmnologin.sh	${D}${sysconfdir}/rc5.d/S99rmnologin.sh
	ln -sf		../init.d/sendsigs	${D}${sysconfdir}/rc6.d/S20sendsigs
#	ln -sf		../init.d/urandom	${D}${sysconfdir}/rc6.d/S30urandom
	ln -sf		../init.d/umountnfs.sh	${D}${sysconfdir}/rc6.d/S31umountnfs.sh
	ln -sf		../init.d/umountfs	${D}${sysconfdir}/rc6.d/S40umountfs
	ln -sf		../init.d/reboot	${D}${sysconfdir}/rc6.d/S90reboot
	ln -sf		../init.d/sendsigs	${D}${sysconfdir}/rc0.d/S20sendsigs
#	ln -sf		../init.d/urandom	${D}${sysconfdir}/rc0.d/S30urandom
	ln -sf		../init.d/umountnfs.sh	${D}${sysconfdir}/rc0.d/S31umountnfs.sh
	ln -sf		../init.d/umountfs	${D}${sysconfdir}/rc0.d/S40umountfs
	ln -sf		../init.d/halt		${D}${sysconfdir}/rc0.d/S90halt
	ln -sf		../init.d/save-rtc.sh	${D}${sysconfdir}/rc0.d/S25save-rtc.sh
	ln -sf		../init.d/save-rtc.sh	${D}${sysconfdir}/rc6.d/S25save-rtc.sh
	ln -sf		../init.d/banner.sh	${D}${sysconfdir}/rcS.d/S02banner.sh
	ln -sf		../init.d/udev		${D}${sysconfdir}/rcS.d/S04udev
	ln -sf		../init.d/mountall.sh	${D}${sysconfdir}/rcS.d/S35mountall.sh
	ln -sf		../init.d/hostname.sh	${D}${sysconfdir}/rcS.d/S39hostname.sh
	ln -sf		../init.d/dbus	        ${D}${sysconfdir}/rcS.d/S40dbus
	ln -sf		../init.d/NetworkManager	${D}${sysconfdir}/rcS.d/S41NetworkManager
	ln -sf		../init.d/gnomemisc     ${D}${sysconfdir}/rcS.d/S04gnomemisc
	ln -sf		../init.d/mountnfs.sh	${D}${sysconfdir}/rcS.d/S45mountnfs.sh
#	ln -sf		../init.d/urandom	${D}${sysconfdir}/rcS.d/S55urandom
#	ln -sf		../init.d/finish.sh	${D}${sysconfdir}/rcS.d/S99finish.sh
	# udev will run at S03 if installed

	install -m 0755		${WORKDIR}/device_table.txt		${D}${sysconfdir}/device_table
}

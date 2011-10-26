
all: ostree_switch_root chroot_break

ostree_switch_root: ostree_switch_root.c Makefile
	gcc -Wall -o $@ $<

chroot_break: chroot_break.c Makefile
	gcc -Wall -o $@ $<

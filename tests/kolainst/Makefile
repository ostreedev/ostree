DESTDIR ?=

TESTDIRS := $(shell find -mindepth 1 -maxdepth 1 -type d)
LIBSCRIPTS := $(shell ls *.sh)

KOLA_TESTDIR = $(DESTDIR)/usr/lib/coreos-assembler/tests/kola/ostree/

all:
	for x in $(LIBSCRIPTS); do bash -n "$${x}"; done
	(cd ../inst && cargo run --release -- list-destructive) > destructive-list.txt

install:
	install -D -m 0644 -t $(KOLA_TESTDIR) $(LIBSCRIPTS)
	for x in $(TESTDIRS); do rsync -rlv ./$${x} $(KOLA_TESTDIR)/; done
	install -D -m 0755 -t $(KOLA_TESTDIR)/nondestructive-rs ../inst/target/release/ostree-test
	install -D -m 0644 destructive-stamp.ign $(KOLA_TESTDIR)/destructive-rs/config.ign
	./install-wrappers.sh destructive-list.txt $(KOLA_TESTDIR)/destructive-rs

localinstall:
	rm -rf ../kola
	make install KOLA_TESTDIR=../kola

all: generate-libostree-sys generate-libostree

.PHONY: update-gir-files


# -- cargo package helpers --
pre-package:
	cp LICENSE libostree-sys/
	cp README.md LICENSE libostree/


# -- gir generation --
tools/bin/gir:
	cargo install --root tools --git https://github.com/gtk-rs/gir.git -- gir

gir/%: tools/bin/gir
	tools/bin/gir -c conf/$*.toml

generate-libostree-sys: gir/libostree-sys

generate-libostree: gir/libostree


# -- gir file management --
update-gir-files: \
	remove-gir-files \
	gir-files \
	gir-files/GLib-2.0.gir \
	gir-files/Gio-2.0.gir \
	gir-files/GObject-2.0.gir

remove-gir-files:
	rm -f gir-files/G*-2.0.gir

gir-files:
	mkdir -p gir-files

%.gir:
	curl -o $@ -L https://github.com/gtk-rs/gir-files/raw/master/${@F}

gir-files/OSTree-1.0.gir:
	echo TODO
	exit 1

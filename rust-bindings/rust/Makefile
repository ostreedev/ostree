all: gir/libostree gir/ostree-sys

.PHONY: update-gir-files


# -- gir generation --
target/tools/bin/gir:
	cargo install --root target/tools --git https://github.com/gtk-rs/gir.git --rev ffda6f9 -- gir

gir/%: target/tools/bin/gir
	target/tools/bin/gir -c conf/$*.toml


# -- LGPL docs generation --
target/tools/bin/rustdoc-stripper:
	cargo install --root target/tools -- rustdoc-stripper

merge-lgpl-docs: target/tools/bin/gir target/tools/bin/rustdoc-stripper
	target/tools/bin/gir -c conf/libostree.toml -m doc
	target/tools/bin/rustdoc-stripper -g -o target/vendor.md


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

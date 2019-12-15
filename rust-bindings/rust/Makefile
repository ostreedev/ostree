GIR_VERSION := d1e88f94e89a84d7aae7a51b3ff46b71838c42ff
RUSTDOC_STRIPPER_VERSION := 0.1.9

all: gir

.PHONY: gir gir-report update-gir-files remove-gir-files merge-lgpl-docs


# -- gir generation --
target/tools/bin/gir:
	cargo install --root target/tools --git https://github.com/gtk-rs/gir.git --rev $(GIR_VERSION) -- gir

gir: target/tools/bin/gir
	target/tools/bin/gir -c conf/ostree-sys.toml
	sed -i '/^from gir-files/d' sys/src/auto/versions.txt
	target/tools/bin/gir -c conf/ostree.toml
	sed -i '/^from gir-files/d' src/auto/versions.txt

gir-report: gir
	target/tools/bin/gir -c conf/ostree.toml -m not_bound


# -- LGPL docs generation --
target/tools/bin/rustdoc-stripper:
	cargo install --root target/tools --version $(RUSTDOC_STRIPPER_VERSION) -- rustdoc-stripper

merge-lgpl-docs: target/tools/bin/gir target/tools/bin/rustdoc-stripper
	target/tools/bin/gir -c conf/ostree.toml -m doc
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
	echo Best to build libostree with all features and use that
	exit 1

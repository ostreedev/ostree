GIR_REPO := https://github.com/gtk-rs/gir.git
GIR_VERSION := 2d1ffab19eb5f9a2f0d7a294dbf07517dab4d989
RUSTDOC_STRIPPER_VERSION := 0.1.13
OSTREE_VER := 2020.4

all: gir

.PHONY: gir gir-report update-gir-files remove-gir-files merge-lgpl-docs ci-build-stages


# -- gir generation --
target/tools/bin/gir:
	cargo install --root target/tools --git $(GIR_REPO) --rev $(GIR_VERSION) -- gir

gir: target/tools/bin/gir
	target/tools/bin/gir -c conf/ostree-sys.toml
	target/tools/bin/gir -c conf/ostree.toml

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
	gir-files/GObject-2.0.gir \
	gir-files/OSTree-1.0.gir

remove-gir-files:
	rm -f gir-files/*.gir

gir-files:
	mkdir -p gir-files

%.gir:
	curl -o $@ -L https://github.com/gtk-rs/gir-files/raw/master/${@F}

gir-files/OSTree-1.0.gir:
	podman build \
		--build-arg OSTREE_VER=$(OSTREE_VER) \
		-t ostree-build \
		.
	podman run \
		--rm \
		-v $(PWD)/gir-files:/gir-files \
		ostree-build \
		bash -eu -c "cp /build/OSTree-1.0.gir /gir-files/"

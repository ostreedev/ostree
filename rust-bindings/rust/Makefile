all: generate-libostree-sys generate-libostree

.PHONY: update-gir-files

# tools
tools/bin/gir:
	cargo install --root tools --git https://github.com/gtk-rs/gir.git -- gir

tools/bin/rustdoc-stripper:
	cargo install --root tools rustdoc-stripper

# gir generate
gir/%: tools/bin/gir
	tools/bin/gir -c conf/$*.toml

generate-libostree-sys: gir/libostree-sys

generate-libostree: gir/libostree #update-docs

# docs
update-docs: tools/bin/gir tools/bin/rustdoc-stripper
	tools/bin/gir -c conf/libostree.toml -m doc
	#sed -i \
	#	-e "s/trait RepoExt::fn list_refs/trait RepoExtManual::fn list_refs/" \
	#	-e "s/trait RepoExt::fn list_refs_ext/trait RepoExtManual::fn list_refs_ext/" \
	#	-e "s/trait RepoExt::fn traverse_commit/trait RepoExtManual::fn traverse_commit/" \
	#	libostree/vendor.md
	tools/bin/rustdoc-stripper -g -o libostree/vendor.md
	rm libostree/vendor.md

# gir file management
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

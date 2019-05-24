all: gir

.PHONY: update-gir-files remove-gir-files merge-lgpl-docs


# -- gir generation --
target/tools/bin/gir:
	#cargo install --root target/tools --git https://github.com/gtk-rs/gir.git --rev fec179c697a03e4aa98c610f7b98fd1b0ceb9344 -- gir
	cargo install --root target/tools --git https://github.com/fkrull/gir.git --branch fixup-gconstpointer -- gir

gir: target/tools/bin/gir
	target/tools/bin/gir -c conf/ostree-sys.toml
	target/tools/bin/gir -c conf/ostree.toml


# -- LGPL docs generation --
target/tools/bin/rustdoc-stripper:
	cargo install --root target/tools --version 0.1.5 -- rustdoc-stripper

merge-lgpl-docs: target/tools/bin/gir target/tools/bin/rustdoc-stripper
	target/tools/bin/gir -c conf/ostree.toml -m doc
	for sym in list_refs list_refs_ext traverse_commit; do \
		sed -e "s/<!-- impl Repo::fn $$sym -->/<!-- trait RepoExtManual::fn $$sym -->/" -i target/vendor.md; \
	done
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

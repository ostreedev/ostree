srpm:
	dnf install -y git
	ci/make-git-snapshot.sh
	curl -LO https://src.fedoraproject.org/rpms/ostree/raw/rawhide/f/ostree.spec
	sed -ie "s,^Version:.*,Version: $$(git describe --always --tags | sed -e 's,-,\.,g' -e 's,^v,,')," ostree.spec
	sed -ie 's/^Patch/# Patch/g' ostree.spec  # we don't want any downstream patches
	rpmbuild -bs --define "_sourcedir ${PWD}" --define "_specdir ${PWD}" --define "_builddir ${PWD}" --define "_srcrpmdir ${PWD}" --define "_rpmdir ${PWD}" --define "_buildrootdir ${PWD}/.build" ostree.spec
	mv *.src.rpm $$outdir

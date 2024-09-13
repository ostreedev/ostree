srpm:
	dnf install -y git
	# similar to https://github.com/actions/checkout/issues/760, but for COPR
	git config --global --add safe.directory '*'
	ci/make-git-snapshot.sh
	curl -LO https://src.fedoraproject.org/rpms/ostree/raw/rawhide/f/ostree.spec
	sed -ie "s,^Version:.*,Version: $$(git describe --always --tags --match 'v2???.*' | sed -e 's,-,\.,g' -e 's,^v,,')," ostree.spec
	sed -ie 's/^Patch/# Patch/g' ostree.spec  # we don't want any downstream patches
	sed -i 's,%autorelease,1%{?dist},g' ostree.spec # COPR does not support autorelease macro
	rpmbuild -bs --define "_sourcedir ${PWD}" --define "_specdir ${PWD}" --define "_builddir ${PWD}" --define "_srcrpmdir ${PWD}" --define "_rpmdir ${PWD}" --define "_buildrootdir ${PWD}/.build" ostree.spec
	mv *.src.rpm $$outdir

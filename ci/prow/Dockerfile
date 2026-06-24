FROM quay.io/coreos-assembler/fcos-buildroot:testing-devel as builder
WORKDIR /src
COPY . .
RUN ./ci/build.sh
RUN make -C tests/kolainst
RUN make -C tests/kolainst install DESTDIR=/cosa/component-tests

# Build RPMs inside the cosa base image so they link against its libraries.
# The buildroot (Fedora N+1) and cosa (Fedora N) may ship different sonames.
FROM quay.io/coreos-assembler/coreos-assembler:latest as rpmbuild
USER root
COPY --from=builder /src /src
WORKDIR /src
RUN dnf -y install /usr/bin/rpmbuild gcc
RUN <<EORUN
set -xeuo pipefail
git config --global --add safe.directory '*'
git submodule update --init
ci/make-git-snapshot.sh
ci/packit-fix-spec.sh ostree.spec
rpmbuild -bs --define "_sourcedir ${PWD}" --define "_specdir ${PWD}" \
    --define "_builddir ${PWD}" --define "_srcrpmdir ${PWD}" \
    --define "_rpmdir ${PWD}" --define "_buildrootdir ${PWD}/.build" ostree.spec
dnf builddep -y *.src.rpm
ci/rpmbuild-cwd --rebuild *.src.rpm
mkdir -p /rpms
find . -name '*.rpm' -not -name '*.src.rpm' -exec mv {} /rpms/ \;
EORUN

FROM quay.io/coreos-assembler/coreos-assembler:latest
WORKDIR /srv
USER root
# Install RPMs built against this image's libraries
COPY --from=rpmbuild /rpms/*.rpm /tmp/rpms/
RUN dnf -y install --allowerasing $(ls /tmp/rpms/ostree-*.rpm | grep -v -e devel -e debug) && \
    rm -rf /tmp/rpms
# Stage RPMs for cosa overrides (used by fcos-e2e.sh)
COPY --from=rpmbuild /rpms /cosa/rpms
# Copy test binaries that aren't packaged as RPMs
COPY --from=builder /cosa/component-tests /cosa/component-tests
RUN rsync -rlv /cosa/component-tests/ /
# Grab all of our ci scripts
COPY --from=builder /src/ci/ /ci/
RUN ln -sr /ci/prow/fcos-e2e.sh /usr/bin/fcos-e2e
USER builder

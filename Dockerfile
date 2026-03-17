
ARG base=quay.io/centos-bootc/centos-bootc:stream9

FROM $base as buildroot
# This installs our package dependencies, and we want to cache it independently of the rest.
# Basically we don't want changing a .rs file to blow out the cache of packages.
COPY ci /ci
RUN /ci/installdeps.sh

# This image holds the source code
FROM $base as src
COPY . /src

# This image holds only the main program sources, helping ensure that
# when one edits the tests it doesn't recompile the whole program
FROM src as binsrc
RUN --network=none rm tests-unit-container -rf && touch -r src .

FROM buildroot as build
COPY --from=binsrc /src /build
WORKDIR /build
RUN --mount=type=cache,target=/ccache <<EORUN
set -xeuo pipefail
mkdir -p /var/roothome
env NOCONFIGURE=1 ./autogen.sh
export CC="ccache gcc" CCACHE_DIR=/ccache
env ./configure \
    --sysconfdir=/etc --prefix=/usr --libdir=/usr/lib64 \
    --with-openssl --with-selinux --with-composefs \
    --with-dracut=yesbutnoconf \
    --disable-gtk-doc --with-curl --without-soup
make -j $(nproc)
make install DESTDIR=/out
EORUN

# This image holds both the main binary and the tests
FROM $base as bin-and-test
RUN rpm -e --nodeps ostree{,-libs}
COPY --from=build /out/ /
COPY --from=src /src/tests-unit-container /tests

# Build integration test binary
FROM $base as integration-build
RUN dnf -y install cargo rust
COPY tests/bootc-integration/Cargo.toml tests/bootc-integration/Cargo.lock /build/tests/bootc-integration/
WORKDIR /build/tests/bootc-integration
RUN --mount=type=cache,target=/root/.cargo/registry \
    --mount=type=cache,target=/root/.cargo/git \
    cargo fetch
COPY tests/bootc-integration /build/tests/bootc-integration
RUN --network=none \
    --mount=type=cache,target=/root/.cargo/registry \
    --mount=type=cache,target=/root/.cargo/git \
    --mount=type=cache,target=/build/tests/bootc-integration/target \
    cargo build --release && \
    cp target/release/ostree-bootc-integration-tests /usr/bin/ostree-bootc-integration-tests

# Override userspace
FROM $base as rootfs
# Remove the default binaries to ensure we're getting our overrides
RUN rpm -e --nodeps ostree{,-libs}
COPY --from=build /out/ /

# The default final container, with also a regenerated
# initramfs in case ostree-prepare-root changed.
FROM rootfs
COPY --from=integration-build /usr/bin/ostree-bootc-integration-tests /usr/bin/ostree-bootc-integration-tests
# https://docs.fedoraproject.org/en-US/bootc/initramfs/#_regenerating_the_initrd
# since we have ostree-prepare-root there
RUN set -x; kver=$(cd /usr/lib/modules && echo *); dracut -vf /usr/lib/modules/$kver/initramfs.img $kver


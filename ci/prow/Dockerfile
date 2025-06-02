FROM quay.io/coreos-assembler/fcos-buildroot:testing-devel as builder
WORKDIR /src
COPY . .
RUN ./ci/build.sh
RUN make -C target/c install DESTDIR=/cosa/component-install
RUN make -C tests/kolainst
RUN make -C tests/kolainst install DESTDIR=/cosa/component-tests
# Uncomment this to fake a build to test the code below
#RUN mkdir -p /cosa/component-install/usr/bin && echo foo > /cosa/component-install/usr/bin/foo

FROM quay.io/coreos-assembler/coreos-assembler:latest
WORKDIR /srv
USER root
# Copy binaries from the build
COPY --from=builder /cosa /cosa
# Merge them to the real root since we're used at compose time
RUN rsync -rlv /cosa/component-install/ /
# Merge installed tests
RUN rsync -rlv /cosa/component-tests/ /
# Grab all of our ci scripts
COPY --from=builder /src/ci/ /ci/
RUN ln -sr /ci/prow/fcos-e2e.sh /usr/bin/fcos-e2e
USER builder

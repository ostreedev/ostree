FROM registry.fedoraproject.org/fedora:latest

RUN dnf install -y curl gcc make tar xz 'dnf-command(builddep)'
RUN dnf builddep -y ostree

ARG OSTREE_VER
ENV OSTREE_SRC=https://github.com/ostreedev/ostree/releases/download/v${OSTREE_VER}/libostree-${OSTREE_VER}.tar.xz
RUN mkdir /src && \
    cd /src && \
    curl -L -o /ostree.tar.xz ${OSTREE_SRC} && \
    tar -xa --strip-components=1 -f /ostree.tar.xz && \
    rm -r /ostree.tar.xz
RUN mkdir /build && \
    cd /build && \
    /src/configure \
        --with-openssl \
        --with-curl \
        && \
    make -j4

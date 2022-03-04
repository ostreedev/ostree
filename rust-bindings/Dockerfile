FROM registry.fedoraproject.org/fedora:latest

RUN dnf install -y gcc git make 'dnf-command(builddep)'
RUN dnf builddep -y ostree

ARG OSTREE_REPO
ARG OSTREE_VERSION
RUN mkdir /src && \
    cd /src && \
    git init . && \
    git fetch --depth 1 $OSTREE_REPO $OSTREE_VERSION && \
    git checkout FETCH_HEAD && \
    git submodule update --init
RUN mkdir /build && \
    cd /build && \
    NOCONFIGURE=1 /src/autogen.sh && \
    /src/configure \
        --with-openssl \
        --with-curl \
        && \
    make -j4

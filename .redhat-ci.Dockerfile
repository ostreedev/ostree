FROM fedora:24
MAINTAINER Jonathan Lebon <jlebon@redhat.com>

RUN dnf install -y \
        gcc \
        sudo \
        which \
        attr \
        fuse \
        gjs \
        parallel \
        clang \
        lib{ub,a,t}san \
        gnome-desktop-testing \
        redhat-rpm-config \
        elfutils \
        'dnf-command(builddep)' \
 && dnf builddep -y \
        ostree \
 && dnf clean all

# create an unprivileged user for testing
RUN adduser testuser

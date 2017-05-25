FROM fedora:25

RUN dnf install -y \
        gcc \
        git \
        sudo \
        which \
        attr \
        fuse \
        gjs \
        parallel \
        coccinelle \
        clang \
        libubsan \
        libasan \
        libtsan \
        PyYAML \
        gnome-desktop-testing \
        redhat-rpm-config \
        elfutils \
        'dnf-command(builddep)' \
 && dnf builddep -y \
        ostree \
 && dnf clean all

# create an unprivileged user for testing
RUN adduser testuser

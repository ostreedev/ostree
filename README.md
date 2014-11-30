OSTree is a tool for managing bootable, immutable, versioned
filesystem trees. While it takes over some of the roles of tradtional
"package managers" like dpkg and rpm, it is not a package system; nor
is it a tool for managing full disk images. Instead, it sits between
those levels, offering a blend of the advantages (and disadvantages)
of both.

For more information, see:

https://live.gnome.org/Projects/OSTree

Submitting patches
------------------

You can:

 1. Send mail to ostree-list@gnome.org, with the patch attached
 1. Submit a pull request against https://github.com/GNOME/ostree
 1. Attach them to https://bugzilla.gnome.org/

Please look at "git log" and match the commit log style.


Running the test suite
----------------------

Currently, ostree uses https://wiki.gnome.org/GnomeGoals/InstalledTests
To run just ostree's tests:

    ./configure ... --enable-installed-tests
    gnome-desktop-testing-runner -p 0 ostree/



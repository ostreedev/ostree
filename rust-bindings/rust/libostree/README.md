# Rust bindings for libostree
libostree is both a shared library and suite of command line tools that combines a "git-like" model for committing and
downloading bootable filesystem trees, along with a layer for deploying them and managing the bootloader configuration.
The core OSTree model is like git in that it checksums individual files and has a content-addressed-object store. It's
unlike git in that it "checks out" the files via hardlinks, and they thus need to be immutable to prevent corruption.

[libostree site](https://ostree.readthedocs.io) | [libostree git repo](https://github.com/ostreedev/ostree)

This project provides [Rust](https://rust-lang.org) bindings for libostree. They are automatically generated, but rather
incomplete as of yet.

## Setup
The `libostree` crate requires libostree and the libostree development headers. On Debian/Ubuntu, they can be installed
with:

```ShellSession
$ sudo apt-get install libostree-1 libostree-dev
```

To use the crate, add it to your `Cargo.toml`:

```toml
[dependencies]
libostree = "0.1"
```

To use features from later libostree versions, you need to specify the release version as well: 

```toml
[dependencies.libostree]
version = "0.1"
features = ["v2018_7"]
```

## License
The libostree crate is licensed under the MIT license. See the LICENSE file for details.

libostree itself is licensed under the LGPL2+. See its [licensing information](https://ostree.readthedocs.io#licensing)
for more information.

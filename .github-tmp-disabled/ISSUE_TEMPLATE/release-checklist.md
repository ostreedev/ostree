# Release process

The release process follows the usual PR-and-review flow, allowing an external reviewer to have a final check before publishing a release.

## Requirements

This guide requires:

 * a web browser (and network connectivity)
 * `git`
 * GPG setup and personal key for signing
 * [git-evtag](https://github.com/cgwalters/git-evtag/)
 * write access to the git repository
 * upload access to this project on GitHub

## Release checklist

- Prepare local environment:
  - [ ]  `git remote get-url --push origin`
  - [ ]  validate that the output above points to `git@github.com:ostreedev/ostree.git`
  - [ ] `git checkout main && git pull`
  - [ ] `git clean -fd`
  - [ ] `RELEASE_VER=yyyy.n` (matching `package_version` in `configure.ac`)
  - [ ] `git checkout -b release-${RELEASE_VER}`

- Prepare the release commits:
  - [ ] `sed -i -e 's/^is_release_build=no/is_release_build=yes/' configure.ac`
  - [ ] move the new-symbols stanza (if any) from `src/libostree/libostree-devel.sym` to `src/libostree/libostree-released.sym`
  - [ ] comment the `src/libostree/libostree-devel.sym` include in `Makefile-libostree.am`
  - [ ] update `tests/test-symbols.sh` with the new digest from `sha256sum src/libostree/libostree-released.sym`
  - [ ] `git commit -a -m "Release ${RELEASE_VER}"`
  - [ ] `RELEASE_COMMIT=$(git rev-parse HEAD)`
  - [ ] `./autogen.sh && make dist`
  - [ ] update `year_version` and `release_version` in `configure.ac` for the next development cycle
  - [ ] `sed -i -e 's/^is_release_build=yes/is_release_build=no/' configure.ac`
  - [ ] `git commit -a -m 'configure: post-release version bump'`

- Open a PR to create the release:
  - [ ] `git push -u origin release-${RELEASE_VER}`
  - [ ] open a web browser and create a PR for the branch above, titled `Release ${RELEASE_VER}`
  - [ ] make sure the resulting PR contains two commits
  - [ ] in the PR body, write a short summary of relevant changes since last release (using `git shortlog` too)

- [ ] get the PR reviewed, approved and merged

- Publish the tag:
  - [ ] `git fetch origin && git checkout ${RELEASE_COMMIT}`
  - [ ] `git-evtag sign v${RELEASE_VER}`
  - [ ] `git push --tags origin v${RELEASE_VER}`

- Publish the release and artifacts on GitHub:
  - [ ] find the new tag in the [GitHub tag list](https://github.com/ostreedev/ostree/tags) and click the triple dots menu, then create a release for it
  - [ ] write a short changelog (i.e. re-use the PR content)
  - [ ] attach `libostree-{RELEASE_VER}.tar.xz`
  - [ ] publish release

- Clean up:
  - [ ] `git clean -fd`
  - [ ] `git checkout main`
  - [ ] `git pull`
  - [ ] `git push origin :release-${RELEASE_VER}`
  - [ ] `git branch -d release-${RELEASE_VER}`

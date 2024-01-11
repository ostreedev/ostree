#!/bin/bash
set -xeuo pipefail

. ${KOLA_EXT_DATA}/libinsttest.sh

case "${AUTOPKGTEST_REBOOT_MARK:-}" in
  "")
    # create a new ostree commit with some toplevel content
    mkdir -p /var/tmp/rootfs/foobar
    (cd /var/tmp/rootfs/foobar
     touch an_empty_file
     echo 'foobar' > a_non_empty_file
     echo 'foobar' > another_file
     ln -s an_empty_file a_working_symlink
     ln -s enoent a_broken_symlink
     mkdir an_empty_subdir
     mkdir a_nonempty_subdir
     echo foobar > a_nonempty_subdir/foobar
     mkdir -p a_deeply/deeply/nested/subdir
     echo foobar > a_deeply/deeply/nested/subdir/foobar

     # test content deletion
     mkdir a_dir_to_delete
     touch a_file_to_delete
     ln -s enoent a_symlink_to_delete

     # opaque directory
     mkdir a_dir_to_make_opaque
     touch a_dir_to_make_opaque/base
    )

    ostree commit --no-bindings -P -b foobar --tree=ref="${host_commit}" --tree=dir=/var/tmp/rootfs
    rpm-ostree rebase :foobar
    systemctl enable ostree-state-overlay@foobar.service
    /tmp/autopkgtest-reboot "2"
    ;;
  "2")
    if ! test -d /foobar; then
      fatal "no /foobar toplevel dir"
    fi
    if [[ $(findmnt /foobar -no SOURCE) != overlay ]]; then
      fatal "/foobar is not overlay"
    fi

    cd /foobar

    # create some state files (i.e. not shadowing)
    echo "state" > state
    echo "state" > a_nonempty_subdir/state
    echo "state" > a_deeply/deeply/nested/subdir/state
    ln -s foobar state_symlink
    mkdir state_dir

    # and shadow some base files

    # make empty file non-empty
    echo shadow > an_empty_file
    # make a file become a directory
    rm a_non_empty_file && mkdir a_non_empty_file
    # make a file become a symlink
    ln -sf some_target another_file
    # override a working symlink
    ln -sf another_file a_working_symlink
    # override a non-working symlink
    ln -sf enoent2 a_broken_symlink
    # make dir become a file
    rmdir an_empty_subdir
    touch an_empty_subdir
    # override file in a shallow subdir
    echo shadow > a_nonempty_subdir/foobar
    # override file in a deep subdir
    echo shadow > a_deeply/deeply/nested/subdir/foobar
    # delete some base files
    rmdir a_dir_to_delete
    rm a_file_to_delete
    rm a_symlink_to_delete
    # opaque directory
    rm -rf a_dir_to_make_opaque
    mkdir a_dir_to_make_opaque
    touch a_dir_to_make_opaque/state

    # check that rebooting without upgrading maintains state
    /tmp/autopkgtest-reboot "3"
    ;;
  "3")
    cd /foobar

    # check state is still there
    assert_file_has_content state state
    assert_file_has_content a_nonempty_subdir/state state
    assert_file_has_content a_deeply/deeply/nested/subdir/state state
    [[ $(readlink state_symlink) == foobar ]]
    test -d state_dir

    # check shadowings
    assert_file_has_content an_empty_file shadow
    test -d a_non_empty_file
    [[ $(readlink another_file) == some_target ]]
    [[ $(readlink a_working_symlink) == another_file ]]
    [[ $(readlink a_broken_symlink) == enoent2 ]]
    test -f an_empty_subdir
    assert_file_has_content a_nonempty_subdir/foobar shadow
    assert_file_has_content a_deeply/deeply/nested/subdir/foobar shadow
    ! test -e a_dir_to_delete
    ! test -e a_file_to_delete
    ! test -e a_symlink_to_delete
    # opaque directory
    test -d a_dir_to_make_opaque
    ! test -e a_dir_to_make_opaque/base
    test -e a_dir_to_make_opaque/state

    # now reboot into an upgrade
    ostree commit --no-bindings -P -b foobar --tree=ref="${host_commit}"
    rpm-ostree upgrade
    /tmp/autopkgtest-reboot "4"
    ;;
  "4")
    cd /foobar

    # check state is still there
    assert_file_has_content state state
    assert_file_has_content a_nonempty_subdir/state state
    assert_file_has_content a_deeply/deeply/nested/subdir/state state
    [[ $(readlink state_symlink) == foobar ]]
    test -d state_dir

    # check shadowings are gone
    test -f an_empty_file
    assert_file_has_content a_non_empty_file foobar
    assert_file_has_content another_file foobar
    [[ $(readlink a_working_symlink) == an_empty_file ]]
    [[ $(readlink a_broken_symlink) == enoent ]]
    test -d an_empty_subdir
    test -d a_nonempty_subdir
    assert_file_has_content a_nonempty_subdir/foobar foobar
    assert_file_has_content a_deeply/deeply/nested/subdir/foobar foobar
    test -d a_dir_to_delete
    test -f a_file_to_delete
    test -L a_symlink_to_delete
    # opaque directory
    test -d a_dir_to_make_opaque
    test -e a_dir_to_make_opaque/base
    ! test -e a_dir_to_make_opaque/state
    ;;
  *) fatal "Unexpected AUTOPKGTEST_REBOOT_MARK=${AUTOPKGTEST_REBOOT_MARK}" ;;
esac

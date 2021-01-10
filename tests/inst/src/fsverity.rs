//! Test fsverity

use anyhow::Result;
use sh_inline::bash;

use crate::test::*;

// Not *really*  destructive, but must run as root
// and also leaks loopback devices on failure
#[itest(destructive = true)]
fn fsverity() -> Result<()> {
    if !check_ostree_feature("ex-fsverity")? {
        return Ok(());
    }

    // Create tempdir and sparse disk file in it
    let td = tempfile::tempdir_in("/var/tmp")?;
    let tmp_disk = td.path().join("disk");
    let tmp_disk = tmp_disk.as_path();
    let mnt = td.path().join("mnt");
    let mnt = mnt.as_path();
    std::fs::create_dir(&mnt)?;
    // Create filesystem on it, loopback mount and create a repo
    let repopath = mnt.join("repo");
    let repopath = repopath.as_path();
    bash!(
        "truncate -s 500M {tmp_disk}
         mkfs.ext4 -b $(getconf PAGE_SIZE) -O verity {tmp_disk}
           mount -o loop {tmp_disk} {mnt}
           echo foo > {mnt}/foo
           echo bar > {mnt}/bar
           ",
        tmp_disk = tmp_disk,
        mnt = mnt
    )?;
    bash!(
        r#"
set -x
ostree --repo={repopath} init --mode=bare
k=fsverity-key.pem
c=fsverity-cert.pem
openssl req -batch -newkey rsa:4096 -nodes -keyout {repopath}/$k -x509 -out {repopath}/$c
cat >>{repopath}/config << EOF
[ex-fsverity]
required=true
key=$k
cert=$c
EOF
mkdir {mnt}/testtree
cp -a /usr/bin/ostree {mnt}/testtree
ostree --repo={repopath} commit -b testverity --tree=dir={mnt}/testtree
find {repopath}/objects -type f | while read f; do
  fsverity measure $f
  if echo somedata >> $f; then
    echo 'modified fsverity file!' 1>&2; exit 1
  fi
done
    "#,
        repopath = repopath,
        mnt = mnt,
    )?;
    Ok(())
}

use anyhow::{Context, Result};
use openat_ext::{FileExt, OpenatDirExt};
use rand::Rng;
use sh_inline::bash;
use std::fs::File;
use std::io::prelude::*;
use std::os::unix::fs::FileExt as UnixFileExt;
use std::path::Path;

use crate::test::*;

/// Each time this is invoked it changes file contents
/// in the target root, in a predictable way.
pub(crate) fn mkroot<P: AsRef<Path>>(p: P) -> Result<()> {
    let p = p.as_ref();
    let verpath = p.join("etc/.mkrootversion");
    let v: u32 = if verpath.exists() {
        let s = std::fs::read_to_string(&verpath)?;
        let v: u32 = s.trim_end().parse()?;
        v + 1
    } else {
        0
    };
    mkvroot(p, v)
}

// Like mkroot but supports an explicit version
pub(crate) fn mkvroot<P: AsRef<Path>>(p: P, v: u32) -> Result<()> {
    let p = p.as_ref();
    for v in &["usr/bin", "etc"] {
        std::fs::create_dir_all(p.join(v))?;
    }
    let verpath = p.join("etc/.mkrootversion");
    write_file(&verpath, &format!("{}", v))?;
    write_file(p.join("usr/bin/somebinary"), &format!("somebinary v{}", v))?;
    write_file(p.join("etc/someconf"), &format!("someconf v{}", v))?;
    write_file(p.join("usr/bin/vmod2"), &format!("somebinary v{}", v % 2))?;
    write_file(p.join("usr/bin/vmod3"), &format!("somebinary v{}", v % 3))?;
    Ok(())
}

/// Returns `true` if a file is ELF; see https://en.wikipedia.org/wiki/Executable_and_Linkable_Format
pub(crate) fn is_elf(f: &mut File) -> Result<bool> {
    let mut buf = [0; 5];
    let n = f.read_at(&mut buf, 0)?;
    if n < buf.len() {
        anyhow::bail!("Failed to read expected {} bytes", buf.len());
    }
    Ok(buf[0] == 0x7F && &buf[1..4] == b"ELF")
}

pub(crate) fn mutate_one_executable_to(
    f: &mut File,
    name: &std::ffi::OsStr,
    dest: &openat::Dir,
) -> Result<()> {
    let mut destf = dest
        .write_file(name, 0o755)
        .context("Failed to open for write")?;
    f.copy_to(&destf).context("Failed to copy")?;
    // ELF is OK with us just appending some junk
    let extra = rand::thread_rng()
        .sample_iter(&rand::distributions::Alphanumeric)
        .take(10)
        .collect::<String>();
    destf
        .write_all(extra.as_bytes())
        .context("Failed to append extra data")?;
    Ok(())
}

/// Find ELF files in the srcdir, write new copies to dest (only percentage)
pub(crate) fn mutate_executables_to(
    src: &openat::Dir,
    dest: &openat::Dir,
    percentage: u32,
) -> Result<u32> {
    use nix::sys::stat::Mode as NixMode;
    assert!(percentage > 0 && percentage <= 100);
    let mut mutated = 0;
    for entry in src.list_dir(".")? {
        let entry = entry?;
        if src.get_file_type(&entry)? != openat::SimpleType::File {
            continue;
        }
        let meta = src.metadata(entry.file_name())?;
        let st = meta.stat();
        let mode = NixMode::from_bits_truncate(st.st_mode);
        // Must be executable
        if !mode.intersects(NixMode::S_IXUSR | NixMode::S_IXGRP | NixMode::S_IXOTH) {
            continue;
        }
        // Not suid
        if mode.intersects(NixMode::S_ISUID | NixMode::S_ISGID) {
            continue;
        }
        // Greater than 1k in size
        if st.st_size < 1024 {
            continue;
        }
        let mut f = src.open_file(entry.file_name())?;
        if !is_elf(&mut f)? {
            continue;
        }
        if !rand::thread_rng().gen_ratio(percentage, 100) {
            continue;
        }
        mutate_one_executable_to(&mut f, entry.file_name(), dest)
            .with_context(|| format!("Failed updating {:?}", entry.file_name()))?;
        mutated += 1;
    }
    Ok(mutated)
}

// Given an ostree ref, use the running root filesystem as a source, update
// `percentage` percent of binary (ELF) files
pub(crate) fn update_os_tree<P: AsRef<Path>>(
    repo_path: P,
    ostref: &str,
    percentage: u32,
) -> Result<()> {
    assert!(percentage > 0 && percentage <= 100);
    let repo_path = repo_path.as_ref();
    let tempdir = tempfile::tempdir_in(repo_path.join("tmp"))?;
    let mut mutated = 0;
    {
        let tempdir = openat::Dir::open(tempdir.path())?;
        let binary_dirs = &["usr/bin", "usr/sbin", "usr/lib", "usr/lib64"];
        let rootfs = openat::Dir::open("/")?;
        for v in binary_dirs {
            let v = *v;
            if let Some(src) = rootfs.sub_dir_optional(v)? {
                tempdir.ensure_dir("usr", 0o755)?;
                tempdir.ensure_dir(v, 0o755)?;
                let dest = tempdir.sub_dir(v)?;
                mutated += mutate_executables_to(&src, &dest, percentage)
                    .with_context(|| format!("Replacing binaries in {}", v))?;
            }
        }
    }
    assert!(mutated > 0);
    println!("Mutated ELF files: {}", mutated);
    bash!("ostree --repo={repo} commit --consume -b {ostref} --base={ostref} --tree=dir={tempdir} --owner-uid 0 --owner-gid 0 --selinux-policy-from-base --link-checkout-speedup --no-bindings --no-xattrs",
        repo = repo_path,
        ostref = ostref,
        tempdir = tempdir.path()).context("Failed to commit updated content")?;
    Ok(())
}

use anyhow::{Context, Result};
use cap_std::fs::Dir;
use cap_std_ext::cap_std;
use cap_std_ext::dirext::*;
use cap_std_ext::rustix::fs::MetadataExt;
use rand::Rng;
use std::fs::File;
use std::io::prelude::*;
use std::os::unix::fs::FileExt as UnixFileExt;
use std::path::Path;
use xshell::cmd;

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
    write_file(&verpath, &format!("{v}"))?;
    write_file(p.join("usr/bin/somebinary"), &format!("somebinary v{v}"))?;
    write_file(p.join("etc/someconf"), &format!("someconf v{v}"))?;
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
    dest: &Dir,
) -> Result<()> {
    let perms = f.metadata()?.permissions();
    dest.atomic_replace_with(name, |w| {
        std::io::copy(f, w)?;
        // ELF is OK with us just appending some junk
        let extra = rand::thread_rng()
            .sample_iter(&rand::distributions::Alphanumeric)
            .take(10)
            .collect::<Vec<u8>>();
        w.write_all(&extra).context("Failed to append extra data")?;
        w.get_mut()
            .as_file_mut()
            .set_permissions(cap_std::fs::Permissions::from_std(perms))?;
        Ok::<_, anyhow::Error>(())
    })
}

/// Find ELF files in the srcdir, write new copies to dest (only percentage)
pub(crate) fn mutate_executables_to(src: &Dir, dest: &Dir, percentage: u32) -> Result<u32> {
    assert!(percentage > 0 && percentage <= 100);
    println!("Mutating {percentage} executables");
    let mut mutated = 0;
    // Retry until we change at least one
    'outer: loop {
        let mut candidates = 0;
        for entry in src.entries()? {
            let entry = entry?;
            if entry.file_type()? != cap_std::fs::FileType::file() {
                continue;
            }
            let meta = entry.metadata()?;
            let mode = meta.mode();
            // Must be executable
            if mode & (libc::S_IXUSR | libc::S_IXGRP | libc::S_IXOTH) == 0 {
                continue;
            }
            // Not suid
            if mode & (libc::S_ISUID | libc::S_ISGID) == 0 {
                continue;
            }
            // Greater than 1k in size
            if meta.size() < 1024 {
                continue;
            }
            let mut f = entry.open()?.into_std();
            if !is_elf(&mut f)? {
                continue;
            }
            candidates += 1;
            if !rand::thread_rng().gen_ratio(percentage, 100) {
                continue;
            }
            mutate_one_executable_to(&mut f, &entry.file_name(), dest)
                .with_context(|| format!("Failed updating {:?}", entry.file_name()))?;
            mutated += 1;
            break 'outer;
        }
        println!("Changed {mutated} binaries of {candidates}");
        // If there's nothing to change, we're done
        if candidates == 0 {
            break;
        }
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
    let sh = xshell::Shell::new()?;
    assert!(percentage > 0 && percentage <= 100);
    let repo_path = repo_path.as_ref();
    let tempdir = tempfile::tempdir_in(repo_path.join("tmp"))?;
    let mut mutated = 0;
    {
        let tempdir = Dir::open_ambient_dir(tempdir.path(), cap_std::ambient_authority())?;
        let binary_dirs = &["usr/bin", "usr/lib", "usr/lib64"];
        let rootfs = Dir::open_ambient_dir("/", cap_std::ambient_authority())?;
        for v in binary_dirs {
            let v = *v;
            if let Some(src) = rootfs.open_dir_optional(v)? {
                tempdir.create_dir_all(v)?;
                let dest = tempdir.open_dir(v)?;
                mutated += mutate_executables_to(&src, &dest, percentage)
                    .with_context(|| format!("Replacing binaries in {v}"))?;
            }
        }
    }
    assert!(mutated > 0);
    println!("Mutated ELF files: {}", mutated);
    let tempdir = tempdir.path();
    cmd!(sh, "ostree --repo={repo_path} commit --consume -b {ostref} --base={ostref} --tree=dir={tempdir} --owner-uid 0 --owner-gid 0 --selinux-policy-from-base --link-checkout-speedup --no-bindings --no-xattrs").run().context("Failed to commit updated content")
}

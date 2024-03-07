//! Test that interrupting an upgrade is safe.
//!
//! This test builds on coreos-assembler's "external tests":
//! https://github.com/coreos/coreos-assembler/blob/main/mantle/kola/README-kola-ext.md
//! Key to this in particular is coreos-assembler implementing the Debian autopkgtest reboot API.
//!
//! The basic model of this test is:
//!
//! Copy the OS content in to an archive repository, and generate a "synthetic"
//! update for it by randomly mutating ELF files.  Time how long upgrading
//! to that takes, to use as a baseline in a range of time we will target
//! for interrupt.
//!
//! Start a webserver, pointing rpm-ostree at the updated content.  We
//! alternate between a few "interrupt strategies", from `kill -9` on
//! rpm-ostreed, or rebooting normally, or an immediate forced reboot
//! (with no filesystem sync).
//!
//! The state of the tests is passed by serializing JSON into the
//! AUTOPKGTEST_REBOOT_MARK.

use anyhow::{Context, Result};
use ostree_ext::gio;
use ostree_ext::ostree;
use rand::seq::SliceRandom;
use rand::Rng;
use serde::{Deserialize, Serialize};
use sh_inline::bash;
use std::collections::BTreeMap;
use std::io::Write;
use std::path::Path;
use std::time;
use strum::IntoEnumIterator;
use strum_macros::EnumIter;
use xshell::cmd;

use crate::test::*;

const ORIGREF: &str = "orig-booted";
const TESTREF: &str = "testcontent";
const TDATAPATH: &str = "/var/tmp/ostree-test-transaction-data.json";
const SRVREPO: &str = "/var/tmp/ostree-test-srv";
// Percentage of ELF files to change per update
const TREEGEN_PERCENTAGE: u32 = 15;
/// Total number of reboots
const ITERATIONS: u32 = 10;
/// Try at most this number of times per iteration to interrupt
const ITERATION_RETRIES: u32 = 15;
// We mostly want to test forced interrupts since those are
// most likely to break.
const FORCE_INTERRUPT_PERCENTAGE: u32 = 85;
/// Multiply the average cycle time by this to ensure we sometimes
/// fail to interrupt too.
const FORCE_REBOOT_AFTER_MUL: f64 = 1.1f64;
/// Amount of time in seconds we will delay each web request.
/// FIXME: this should be a function of total number of objects or so
const WEBSERVER_DELAY_SECS: f64 = 0.005;

/// We choose between these at random
#[derive(EnumIter, Debug, PartialEq, Eq, PartialOrd, Ord, Clone, Serialize, Deserialize)]
#[serde(rename_all = "kebab-case")]
enum PoliteInterruptStrategy {
    None,
    Stop,
    Reboot,
}

/// We choose between these at random
#[derive(EnumIter, Debug, PartialEq, Eq, Clone, PartialOrd, Ord, Serialize, Deserialize)]
#[serde(rename_all = "kebab-case")]
enum ForceInterruptStrategy {
    Kill9,
    Reboot,
}

#[derive(Debug, PartialEq, Eq, Clone, PartialOrd, Ord, Serialize, Deserialize)]
#[serde(rename_all = "kebab-case")]
enum InterruptStrategy {
    Polite(PoliteInterruptStrategy),
    Force(ForceInterruptStrategy),
}

#[derive(Debug, PartialEq, Eq, PartialOrd, Ord, Serialize, Deserialize)]
#[serde(rename_all = "kebab-case")]
enum UpdateResult {
    NotCompleted,
    Staged,
    Completed,
}

/// The data passed across reboots by serializing
/// into the AUTOPKGTEST_REBOOT_MARK
#[derive(Serialize, Deserialize, Debug, Default)]
#[serde(rename_all = "kebab-case")]
struct RebootMark {
    /// Reboot strategy that was used for this last reboot
    reboot_strategy: Option<InterruptStrategy>,
    /// Counts attempts to interrupt an upgrade
    iter: u32,
    /// Counts times upgrade completed before we tried to interrupt
    before: u32,
    /// Results for "polite" interrupt attempts
    polite: BTreeMap<PoliteInterruptStrategy, BTreeMap<UpdateResult, u32>>,
    /// Results for "forced" interrupt attempts
    force: BTreeMap<ForceInterruptStrategy, BTreeMap<UpdateResult, u32>>,
}

impl RebootMark {
    fn get_results_map(
        &mut self,
        strategy: &InterruptStrategy,
    ) -> &mut BTreeMap<UpdateResult, u32> {
        match strategy {
            InterruptStrategy::Polite(t) => {
                self.polite.entry(t.clone()).or_insert_with(BTreeMap::new)
            }
            InterruptStrategy::Force(t) => {
                self.force.entry(t.clone()).or_insert_with(BTreeMap::new)
            }
        }
    }
}

impl InterruptStrategy {
    pub(crate) fn is_noop(&self) -> bool {
        matches!(
            self,
            InterruptStrategy::Polite(PoliteInterruptStrategy::None)
        )
    }
}

/// TODO add readonly sysroot handling into base ostree
fn testinit() -> Result<()> {
    assert!(std::path::Path::new("/run/ostree-booted").exists());
    bash!(
        r"if ! test -w /sysroot; then
   mount -o remount,rw /sysroot
fi"
    )?;
    Ok(())
}

/// Given a booted ostree, generate a modified version and write it
/// into our srvrepo.  This is fairly hacky; it'd be better if we
/// reworked the tree mutation to operate on an ostree repo
/// rather than a filesystem.
fn generate_update(commit: &str) -> Result<()> {
    println!("Generating update from {}", commit);
    crate::treegen::update_os_tree(SRVREPO, TESTREF, TREEGEN_PERCENTAGE)
        .context("Failed to generate new content")?;
    // Amortize the prune across multiple runs; we don't want to leak space,
    // but traversing all the objects is expensive.  So here we only prune 1/5 of the time.
    if rand::thread_rng().gen_ratio(1, 5) {
        bash!(
            "ostree --repo=${srvrepo} prune --refs-only --depth=1",
            srvrepo = SRVREPO
        )?;
    }
    Ok(())
}

/// Create an archive repository of current OS content.  This is a bit expensive;
/// in the future we should try a trick using the `parent` property on this repo,
/// and then teach our webserver to redirect to the system for objects it doesn't
/// have.
fn generate_srv_repo(commit: &str) -> Result<()> {
    bash!(
        r#"
        ostree --repo=${srvrepo} init --mode=archive
        ostree --repo=${srvrepo} config set archive.zlib-level 1
        ostree --repo=${srvrepo} pull-local /sysroot/ostree/repo ${commit}
        ostree --repo=${srvrepo} refs --create=${testref} ${commit}
        "#,
        srvrepo = SRVREPO,
        commit = commit,
        testref = TESTREF
    )
    .context("Failed to generate srv repo")?;
    generate_update(commit)?;
    Ok(())
}

#[derive(Serialize, Deserialize, Debug)]
struct TransactionalTestInfo {
    cycle_time: time::Duration,
}

#[derive(Serialize, Deserialize, Debug, Default)]
struct Kill9Stats {
    interrupted: u32,
    staged: u32,
    success: u32,
}

#[derive(Serialize, Deserialize, Debug, Default)]
struct RebootStats {
    interrupted: u32,
    success: u32,
}

fn upgrade_and_finalize() -> Result<()> {
    bash!(
        "rpm-ostree upgrade
        systemctl start ostree-finalize-staged
        systemctl stop ostree-finalize-staged"
    )
    .context("Upgrade and finalize failed")?;
    Ok(())
}

async fn run_upgrade_or_timeout(timeout: time::Duration) -> Result<bool> {
    let upgrade = tokio::task::spawn_blocking(upgrade_and_finalize);
    tokio::pin!(upgrade);
    Ok(tokio::select! {
        res = upgrade => {
            let _res = res?;
            true
        },
        _ = tokio::time::sleep(timeout) => {
            false
        }
    })
}

/// The set of commits that we should see
#[derive(Debug)]
struct CommitStates {
    booted: String,
    orig: String,
    prev: String,
    target: String,
}

impl CommitStates {
    pub(crate) fn describe(&self, commit: &str) -> Option<&'static str> {
        if commit == self.booted {
            Some("booted")
        } else if commit == self.orig {
            Some("orig")
        } else if commit == self.prev {
            Some("prev")
        } else if commit == self.target {
            Some("target")
        } else {
            None
        }
    }
}

fn query_status() -> Result<rpmostree_client::Status> {
    let client = rpmostree_client::CliClient::new("ostreetest");
    rpmostree_client::query_status(&client).map_err(anyhow::Error::msg)
}

/// In the case where we've entered via a reboot, this function
/// checks the state of things, and also generates a new update
/// if everything was successful.
fn parse_and_validate_reboot_mark<M: AsRef<str>>(
    commitstates: &mut CommitStates,
    mark: M,
) -> Result<RebootMark> {
    let markstr = mark.as_ref();
    let mut mark: RebootMark = serde_json::from_str(markstr)
        .with_context(|| format!("Failed to parse reboot mark {:?}", markstr))?;
    // The first failed reboot may be into the original booted commit
    let status = query_status()?;
    let firstdeploy = &status.deployments[0];
    // The first deployment should not be staged
    assert!(!firstdeploy.staged.unwrap_or(false));
    assert!(firstdeploy.booted);
    assert_eq!(firstdeploy.checksum, commitstates.booted);
    let reboot_type = if let Some(t) = mark.reboot_strategy.as_ref() {
        t.clone()
    } else {
        anyhow::bail!("No reboot strategy in mark");
    };
    if commitstates.booted == commitstates.target {
        mark.get_results_map(&reboot_type)
            .entry(UpdateResult::Completed)
            .and_modify(|result_e| {
                *result_e += 1;
            })
            .or_insert(1);
        println!("Successfully updated to {}", commitstates.target);
        // Since we successfully updated, generate a new commit to target
        generate_update(&firstdeploy.checksum)?;
        // Update the target state
        let srvrepo_obj = ostree::Repo::new(&gio::File::for_path(SRVREPO));
        srvrepo_obj.open(gio::Cancellable::NONE)?;
        commitstates.target = srvrepo_obj.resolve_rev(TESTREF, false)?.unwrap().into();
    } else if commitstates.booted == commitstates.orig || commitstates.booted == commitstates.prev {
        println!(
            "Failed update to {} (booted={})",
            commitstates.target, commitstates.booted
        );
        mark.get_results_map(&reboot_type)
            .entry(UpdateResult::NotCompleted)
            .and_modify(|result_e| {
                *result_e += 1;
            })
            .or_insert(1);
    } else {
        anyhow::bail!("Unexpected target commit: {}", firstdeploy.checksum);
    };
    // Empty this out
    mark.reboot_strategy = None;
    Ok(mark)
}

fn validate_pending_commit(pending_commit: &str, commitstates: &CommitStates) -> Result<()> {
    if pending_commit != commitstates.target {
        bash!("rpm-ostree status -v")?;
        bash!("ostree show ${pending_commit}", pending_commit)?;
        anyhow::bail!(
            "Expected target commit={} but pending={} ({:?})",
            commitstates.target,
            pending_commit,
            commitstates.describe(pending_commit)
        );
    }
    Ok(())
}

/// In the case where we did a kill -9 of rpm-ostree, check the state
fn validate_live_interrupted_upgrade(commitstates: &CommitStates) -> Result<UpdateResult> {
    let status = query_status()?;
    let firstdeploy = &status.deployments[0];
    let pending_commit = firstdeploy.checksum.as_str();
    let res = if firstdeploy.staged.unwrap_or(false) {
        assert!(!firstdeploy.booted);
        validate_pending_commit(pending_commit, &commitstates)?;
        UpdateResult::Staged
    } else if pending_commit == commitstates.booted {
        UpdateResult::NotCompleted
    } else if pending_commit == commitstates.target {
        UpdateResult::Completed
    } else {
        anyhow::bail!(
            "Unexpected pending commit: {} ({:?})",
            pending_commit,
            commitstates.describe(pending_commit)
        );
    };
    Ok(res)
}

fn impl_transaction_test<M: AsRef<str>>(
    booted_commit: &str,
    tdata: &TransactionalTestInfo,
    mark: Option<M>,
) -> Result<()> {
    let sh = xshell::Shell::new()?;
    let polite_strategies = PoliteInterruptStrategy::iter().collect::<Vec<_>>();
    let force_strategies = ForceInterruptStrategy::iter().collect::<Vec<_>>();

    // Gather the expected possible commits
    let mut commitstates = {
        let srvrepo_obj = ostree::Repo::new(&gio::File::for_path(SRVREPO));
        srvrepo_obj.open(gio::Cancellable::NONE)?;
        let sysrepo_obj = ostree::Repo::new(&gio::File::for_path("/sysroot/ostree/repo"));
        sysrepo_obj.open(gio::Cancellable::NONE)?;

        CommitStates {
            booted: booted_commit.to_string(),
            orig: sysrepo_obj.resolve_rev(ORIGREF, false)?.unwrap().into(),
            prev: srvrepo_obj
                .resolve_rev(&format!("{TESTREF}^"), false)?
                .unwrap()
                .into(),
            target: srvrepo_obj.resolve_rev(TESTREF, false)?.unwrap().into(),
        }
    };

    let mut mark = if let Some(mark) = mark {
        let markstr = mark.as_ref();
        // In the successful case, this generates a new target commit,
        // so we pass via &mut.
        parse_and_validate_reboot_mark(&mut commitstates, markstr)
            .context("Failed to parse reboot mark")?
    } else {
        RebootMark {
            ..Default::default()
        }
    };
    // Drop the &mut
    let commitstates = commitstates;

    assert_ne!(commitstates.booted.as_str(), commitstates.target.as_str());

    // Also verify we didn't do a global sync()
    {
        let out = cmd!(
            sh,
            "journalctl -u ostree-finalize-staged --grep='Starting global sync'"
        )
        .ignore_status()
        .read()?;
        assert!(!out.contains("Starting global sync"));
    }

    let rt = tokio::runtime::Runtime::new()?;
    let cycle_time_ms = (tdata.cycle_time.as_secs_f64() * 1000f64 * FORCE_REBOOT_AFTER_MUL) as u64;
    // Set when we're trying an interrupt strategy that isn't a reboot, so we will
    // re-enter the loop below.
    let mut live_strategy: Option<InterruptStrategy> = None;
    let mut retries = 0;
    // This loop is for the non-rebooting strategies - we might use kill -9
    // or not interrupt at all.  But if we choose a reboot strategy
    // then we'll exit implicitly via the reboot, and reenter the function
    // above.
    loop {
        // Make sure previously failed services (if any) can run.  Ignore errors here though.
        let _ = cmd!(sh, "systemctl reset-failed").run()?;
        // Save the previous strategy as a string so we can use it in error
        // messages below
        let prev_strategy_str = format!("{:?}", live_strategy);
        // Process the results of the previous run if any, and reset
        // live_strategy to None
        if let Some(last_strategy) = live_strategy.take() {
            mark.iter += 1;
            retries = 0;
            let res = validate_live_interrupted_upgrade(&commitstates)?;
            if last_strategy.is_noop() {
                assert_eq!(res, UpdateResult::Completed)
            }
            mark.get_results_map(&last_strategy)
                .entry(res)
                .and_modify(|result_e| {
                    *result_e += 1;
                })
                .or_insert(1);
        }
        // If we've reached our target iterations, exit the test successfully
        if mark.iter == ITERATIONS {
            // TODO also add ostree admin fsck to check the deployment directories
            println!("Performing final validation...");
            cmd!(sh, "ostree fsck").run()?;
            return Ok(());
        }
        let mut rng = rand::thread_rng();
        // Pick a strategy for this attempt
        let strategy: InterruptStrategy = if rand::thread_rng()
            .gen_ratio(FORCE_INTERRUPT_PERCENTAGE, 100)
        {
            InterruptStrategy::Force(force_strategies.choose(&mut rng).expect("strategy").clone())
        } else {
            InterruptStrategy::Polite(
                polite_strategies
                    .choose(&mut rng)
                    .expect("strategy")
                    .clone(),
            )
        };
        println!("Using interrupt strategy: {:?}", strategy);
        // Interrupt usually before the upgrade would
        // complete, but also a percentage of the time after.
        // The no-op case is special in that we want to wait for it to complete
        let sleeptime = if strategy.is_noop() {
            // In the no-op case, sleep for minimum of 20x the cycle time, or one day
            let ms = std::cmp::min(cycle_time_ms.saturating_mul(20), 24 * 60 * 60 * 1000);
            time::Duration::from_millis(ms)
        } else {
            time::Duration::from_millis(rng.gen_range(0..cycle_time_ms))
        };
        println!(
            "force-reboot-time={:?} cycle={:?} status:{:?}",
            sleeptime, tdata.cycle_time, &mark
        );
        // Reset the target ref to booted, and perform a cleanup
        // to ensure we're re-downloading objects each time
        let testref = TESTREF;
        (|| -> Result<()> {
            cmd!(sh, "systemctl stop rpm-ostreed").run()?;
            cmd!(sh, "systemctl stop ostree-finalize-staged").run()?;
            cmd!(sh, "systemctl stop ostree-finalize-staged-hold").run()?;
            cmd!(sh, "ostree reset testrepo:{testref} {booted_commit}").run()?;
            cmd!(sh, "rpm-ostree cleanup -pbrm").run()?;
            Ok(())
        })()
        .with_context(|| {
            format!(
                "Failed pre-upgrade cleanup (prev strategy: {})",
                prev_strategy_str.as_str()
            )
        })?;

        // The heart of the test - start an upgrade and wait a random amount
        // of time to interrupt.  If the result is true, then the upgrade completed
        // successfully before the timeout.
        let res: Result<bool> = rt.block_on(async move { run_upgrade_or_timeout(sleeptime).await });
        let res = res.context("Failed during upgrade")?;
        if res {
            if !strategy.is_noop() {
                println!(
                    "Failed to interrupt upgrade, attempt {}/{}",
                    retries, ITERATION_RETRIES
                );
                retries += 1;
                mark.before += 1;
            } else {
                live_strategy = Some(strategy);
            }
            let status = query_status()?;
            let firstdeploy = &status.deployments[0];
            let pending_commit = firstdeploy.checksum.as_str();
            validate_pending_commit(pending_commit, &commitstates)
                .context("Failed to validate pending commit")?;
        } else {
            // Our timeout fired before the upgrade completed; execute
            // the interrupt strategy.
            match strategy {
                InterruptStrategy::Force(ForceInterruptStrategy::Kill9) => {
                    cmd!(sh, "systemctl kill -s KILL rpm-ostreed")
                        .ignore_status()
                        .run()?;
                    cmd!(sh, "systemctl kill -s KILL ostree-finalize-staged")
                        .ignore_status()
                        .run()?;
                    cmd!(sh, "systemctl kill -s KILL ostree-finalize-staged-hold")
                        .ignore_status()
                        .run()?;
                    live_strategy = Some(strategy);
                }
                InterruptStrategy::Force(ForceInterruptStrategy::Reboot) => {
                    mark.reboot_strategy = Some(strategy);
                    prepare_reboot(serde_json::to_string(&mark)?)?;
                    // This is a forced reboot - no syncing of the filesystem.
                    cmd!(sh, "reboot -ff").run()?;
                    std::thread::sleep(time::Duration::from_secs(60));
                    // Shouldn't happen
                    anyhow::bail!("failed to reboot");
                }
                InterruptStrategy::Polite(PoliteInterruptStrategy::None) => {
                    anyhow::bail!("Failed to wait for uninterrupted upgrade");
                }
                InterruptStrategy::Polite(PoliteInterruptStrategy::Reboot) => {
                    mark.reboot_strategy = Some(strategy);
                    return Err(reboot(serde_json::to_string(&mark)?).into());
                    // We either rebooted, or failed to reboot
                }
                InterruptStrategy::Polite(PoliteInterruptStrategy::Stop) => {
                    cmd!(sh, "systemctl stop rpm-ostreed")
                        .ignore_status()
                        .run()?;
                    cmd!(sh, "systemctl stop ostree-finalize-staged")
                        .ignore_status()
                        .run()?;
                    cmd!(sh, "systemctl stop ostree-finalize-staged-hold")
                        .ignore_status()
                        .run()?;
                    live_strategy = Some(strategy);
                }
            }
        }
    }
}

// See ostree-sysroot.c; we want this off for our tests
fn suppress_ostree_global_sync(sh: &xshell::Shell) -> Result<()> {
    let dropindir = "/etc/systemd/system/ostree-finalize-staged.service.d";
    std::fs::create_dir_all(dropindir)?;
    // Aslo opt-in to the new bootloader naming
    std::fs::write(
        Path::new(dropindir).join("50-test-options.conf"),
        "[Service]\nEnvironment=OSTREE_SYSROOT_OPTS=skip-sync\n",
    )?;
    cmd!(sh, "systemctl daemon-reload").run()?;
    Ok(())
}

pub(crate) fn itest_transactionality() -> Result<()> {
    let sh = xshell::Shell::new()?;
    testinit()?;
    let mark = get_reboot_mark()?;
    let cancellable = Some(gio::Cancellable::new());
    let sysroot = ostree::Sysroot::new_default();
    sysroot.load(cancellable.as_ref())?;
    assert!(sysroot.is_booted());
    let booted = sysroot.booted_deployment().expect("booted deployment");
    let commit: String = booted.csum().into();
    // We need this static across reboots
    let srvrepo = Path::new(SRVREPO);
    let firstrun = !srvrepo.exists();
    if mark.as_ref().is_some() {
        if firstrun {
            anyhow::bail!("Missing {:?}", srvrepo);
        }
    } else {
        if !firstrun {
            anyhow::bail!("Unexpected {:?}", srvrepo);
        }
        generate_srv_repo(&commit)?;
    }

    // Let's assume we're changing about 200 objects each time;
    // that leads to probably 300 network requests, so we want
    // a low average delay.
    let webserver_opts = TestHttpServerOpts {
        random_delay: Some(time::Duration::from_secs_f64(WEBSERVER_DELAY_SECS)),
        ..Default::default()
    };
    with_webserver_in(&srvrepo, &webserver_opts, move |addr| {
        let url = format!("http://{addr}");
        cmd!(sh, "ostree remote delete --if-exists testrepo").run()?;
        cmd!(
            sh,
            "ostree remote add --set=gpg-verify=false testrepo {url}"
        )
        .run()?;

        if firstrun {
            // Also disable zincati because we don't want automatic updates
            // in our reboots, and it currently fails to start.  The less
            // we have in each reboot, the faster reboots are.
            cmd!(sh, "systemctl disable --now zincati").run()?;
            suppress_ostree_global_sync(&sh)?;
            // And prepare for updates
            cmd!(sh, "rpm-ostree cleanup -pr").run()?;
            generate_update(&commit)?;
            // Directly set the origin, so that we're not dependent on the pending deployment.
            // FIXME: make this saner
            let origref = ORIGREF;
            let testref = TESTREF;
            bash!(
                "
                ostree admin set-origin testrepo ${url} ${testref}
                ostree refs --create testrepo:${testref} ${commit}
                ostree refs --create=${origref} ${commit}
                ",
                url,
                origref,
                testref,
                commit
            )?;
            // We gather a single "cycle time" at start as a way of gauging how
            // long an upgrade should take, so we know when to interrupt.  This
            // obviously has some pitfalls, mainly when there are e.g. other competing
            // VMs when we start but not after (or vice versa) we can either
            // interrupt almost always too early, or too late.
            let start = time::Instant::now();
            upgrade_and_finalize().context("Firstrun upgrade failed")?;
            let end = time::Instant::now();
            let cycle_time = end.duration_since(start);
            let tdata = TransactionalTestInfo { cycle_time };
            let mut f = std::io::BufWriter::new(std::fs::File::create(&TDATAPATH)?);
            serde_json::to_writer(&mut f, &tdata)?;
            f.flush()?;
            cmd!(sh, "rpm-ostree status").run()?;
        }

        let tdata = {
            let mut f = std::io::BufReader::new(std::fs::File::open(&TDATAPATH)?);
            serde_json::from_reader(&mut f).context("Failed to parse test info JSON")?
        };

        impl_transaction_test(commit.as_str(), &tdata, mark.as_ref())?;

        Ok(())
    })?;
    Ok(())
}

use std::io::Write;

use anyhow::{Context, Result, bail};
use clap::Parser;
use xshell::{Shell, cmd};

/// Maximum time to wait for SSH to become available (in seconds).
const SSH_TIMEOUT_SECS: u64 = 300;
/// Interval between SSH readiness checks (in seconds).
const SSH_POLL_INTERVAL_SECS: u64 = 10;

#[derive(Debug, Parser)]
pub(crate) struct RunTmtArgs {
    /// Container image to boot in VMs.
    #[clap(long, default_value = "localhost/ostree:latest")]
    image: String,

    /// Only run plans whose name contains one of these filters.
    #[clap(long)]
    filter: Vec<String>,

    /// Extra arguments to pass to `tmt run`.
    #[clap(last = true)]
    tmt_args: Vec<String>,
}

/// Information extracted from `bcvk libvirt inspect`.
#[derive(serde::Deserialize)]
struct BcvkInspect {
    ssh_port: u16,
    ssh_private_key: String,
}

pub(crate) fn run_tmt(sh: &Shell, args: RunTmtArgs) -> Result<()> {
    check_dependencies(sh)?;

    let image = &args.image;
    let plans = discover_plans(sh, &args.filter)?;
    if plans.is_empty() {
        eprintln!("No test plans found");
        return Ok(());
    }
    eprintln!("Found {} test plan(s):", plans.len());
    for p in &plans {
        eprintln!("  {p}");
    }

    let random_suffix: u16 = std::process::id() as u16;
    let mut failures: Vec<(String, String)> = Vec::new();

    for plan in &plans {
        let plan_name = sanitize_plan_name(plan);
        let vm_name = format!("ostree-tmt-{random_suffix}-{plan_name}");

        eprintln!();
        eprintln!("========================================");
        eprintln!("Running plan: {plan}");
        eprintln!("VM name: {vm_name}");
        eprintln!("========================================");

        match run_plan(sh, &args, image, plan, &vm_name) {
            Ok(()) => eprintln!("Plan {plan} passed"),
            Err(e) => {
                eprintln!("Plan {plan} failed: {e:#}");
                failures.push((plan.clone(), format!("{e:#}")));
            }
        }

        // Always clean up the VM
        cleanup_vm(sh, &vm_name);
    }

    eprintln!();
    if failures.is_empty() {
        eprintln!("All {} test plan(s) passed", plans.len());
        Ok(())
    } else {
        eprintln!(
            "{} of {} plan(s) failed:",
            failures.len(),
            plans.len()
        );
        for (plan, err) in &failures {
            eprintln!("  {plan}: {err}");
        }
        bail!("Some test plans failed");
    }
}

/// Verify that required tools are available.
fn check_dependencies(sh: &Shell) -> Result<()> {
    for tool in ["bcvk", "tmt"] {
        cmd!(sh, "{tool} --version")
            .quiet()
            .ignore_stdout()
            .run()
            .with_context(|| format!("`{tool}` not found; is it installed?"))?;
    }
    Ok(())
}

/// Discover TMT plans and optionally filter them.
fn discover_plans(sh: &Shell, filters: &[String]) -> Result<Vec<String>> {
    let output = cmd!(sh, "tmt plan ls").read()?;
    let plans: Vec<String> = output
        .lines()
        .map(|l| l.trim().to_owned())
        .filter(|l| l.starts_with('/'))
        .collect();

    if filters.is_empty() {
        return Ok(plans);
    }

    Ok(plans
        .into_iter()
        .filter(|p| filters.iter().any(|f| p.contains(f)))
        .collect())
}

/// Run a single TMT plan in a dedicated bcvk VM.
fn run_plan(
    sh: &Shell,
    args: &RunTmtArgs,
    image: &str,
    plan: &str,
    vm_name: &str,
) -> Result<()> {
    // Launch the VM
    cmd!(sh, "bcvk libvirt run --name {vm_name} --detach {image}")
        .run()
        .context("Failed to launch VM")?;

    // Wait for SSH
    wait_for_ssh(sh, vm_name)?;

    // Extract SSH connection details
    let inspect_raw = cmd!(sh, "bcvk libvirt inspect {vm_name} --format json").read()?;
    let inspect: BcvkInspect =
        serde_json::from_str(&inspect_raw).context("Failed to parse bcvk inspect output")?;

    // Write SSH key to a temporary file
    let mut key_file = tempfile::NamedTempFile::new()?;
    key_file.write_all(inspect.ssh_private_key.as_bytes())?;
    key_file.flush()?;
    let key_path = key_file.path();

    let ssh_port = inspect.ssh_port.to_string();
    let tmt_args = &args.tmt_args;

    // Run tmt with connect provisioner
    cmd!(
        sh,
        "tmt run --id {vm_name} --all
            provision --how connect
                --guest localhost --user root
                --port {ssh_port} --key {key_path}
            plan --name {plan}
            {tmt_args...}"
    )
    .run()
    .with_context(|| format!("tmt plan {plan} failed"))?;

    Ok(())
}

/// Wait for SSH to become available on a bcvk VM.
fn wait_for_ssh(sh: &Shell, vm_name: &str) -> Result<()> {
    eprintln!("Waiting for SSH on {vm_name}...");
    let max_attempts = SSH_TIMEOUT_SECS / SSH_POLL_INTERVAL_SECS;

    for i in 1..=max_attempts {
        if cmd!(sh, "bcvk libvirt ssh {vm_name} -- true")
            .quiet()
            .ignore_stdout()
            .ignore_stderr()
            .run()
            .is_ok()
        {
            eprintln!("SSH ready after ~{}s", i * SSH_POLL_INTERVAL_SECS);
            return Ok(());
        }

        std::thread::sleep(std::time::Duration::from_secs(SSH_POLL_INTERVAL_SECS));
    }

    bail!(
        "Timeout waiting for SSH on {vm_name} after {SSH_TIMEOUT_SECS}s"
    );
}

/// Clean up a bcvk VM, ignoring errors.
fn cleanup_vm(sh: &Shell, vm_name: &str) {
    let _ = cmd!(sh, "bcvk libvirt rm --stop --force {vm_name}")
        .quiet()
        .ignore_stdout()
        .ignore_stderr()
        .run();
}

/// Sanitize a TMT plan name for use in a VM name.
fn sanitize_plan_name(plan: &str) -> String {
    plan.rsplit('/')
        .next()
        .unwrap_or(plan)
        .chars()
        .map(|c| if c.is_ascii_alphanumeric() { c } else { '-' })
        .collect()
}

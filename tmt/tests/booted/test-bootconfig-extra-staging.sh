#!/bin/bash
# Test: bootconfig-extra keys survive cross-consumer re-staging
#
# Exercises the "previously-staged fallback" path in
# ostree_sysroot_stage_tree_with_options().  Covers:
#
# Scenario 1 (boot 0-1): bootc source-tracked kargs + rpm-ostree re-staging
#   bootc sets x-options-source-* keys, then rpm-ostree re-stages on the
#   same boot.  The replacement staged deployment must inherit the extension
#   keys from the previously-staged deployment.
#
# Scenario 2 (boot 2-3): Multiple sources + rpm-ostree local kargs coexistence
#   Two bootc sources (tuned + dracut) set independently, then rpm-ostree
#   appends a local karg.  All three karg "owners" must coexist after reboot,
#   and the ownership metadata must survive rpm-ostree's re-staging even
#   though the booted entry carries a stale tombstone from scenario 1.
#   This is the collision case raised in PR #3611 review.
#
# Scenario 3 (boot 3-4): Source replacement while rpm-ostree kargs are present
#   With rpm-ostree local kargs already active, bootc replaces one source's
#   kargs and removes the other.  The rpm-ostree kargs must be unaffected
#   and the replaced/removed kargs must be gone.
#
# Boot 5 verifies the final cleanup (bootc source removal + rpm-ostree
# delete on the same boot, another cross-consumer staging).
#
# Six boots / five reboots in total; each cycle takes several minutes in
# CI, so phases are kept as dense as possible.
#
# This is a multi-reboot TMT test.  TMT_REBOOT_COUNT tracks which boot
# we are on; tmt-reboot triggers a managed reboot between phases.
#
# Requires: bootc, rpm-ostree, ostree with bootconfig-extra support (>= 2026.5)
# See: https://github.com/ostreedev/ostree/pull/3611
# See: https://github.com/ostreedev/ostree/issues/3609
# See: https://github.com/bootc-dev/bootc/pull/2330
# See: https://github.com/bootc-dev/bootc/issues/899
# See: docs/bootconfig-extra.md for the design

set -xeuo pipefail

REBOOT_COUNT="${TMT_REBOOT_COUNT:-0}"

# Save system kargs on first boot so we can verify they survive all phases
SYSTEM_KARGS_FILE="/var/ostree-test-system-kargs.txt"

parse_cmdline() {
    cat /proc/cmdline
}

read_bls_source_keys() {
    # Find the booted deployment's BLS entry by matching the ostree=
    # parameter from /proc/cmdline against the options line in each entry.
    local booted_ostree
    booted_ostree=$(sed 's/.* ostree=//;s/ .*//' /proc/cmdline)
    local entry
    for entry in /boot/loader/entries/ostree-*.conf; do
        if grep -q "^options .*ostree=.*${booted_ostree}" "$entry" 2>/dev/null; then
            grep '^x-options-source-' "$entry" 2>/dev/null || true
            return
        fi
    done
    echo ""
}

save_system_kargs() {
    # Save well-known system kargs that must never be lost
    local cmdline
    cmdline=$(parse_cmdline)
    echo "$cmdline" | tr ' ' '\n' | grep -E '^(root=|rw$|console=)' > "$SYSTEM_KARGS_FILE"
}

verify_system_kargs() {
    local cmdline
    cmdline=$(parse_cmdline)
    while IFS= read -r karg; do
        if ! echo "$cmdline" | grep -qw "$karg"; then
            echo "FAIL: system karg '$karg' was lost" >&2
            echo "  cmdline: $cmdline" >&2
            exit 1
        fi
    done < "$SYSTEM_KARGS_FILE"
    echo "ok: system kargs preserved"
}

assert_cmdline_contains() {
    local karg="$1"
    local msg="${2:-expected '$karg' in cmdline}"
    local cmdline
    cmdline=$(parse_cmdline)
    if ! echo "$cmdline" | grep -qw "$karg"; then
        echo "FAIL: $msg" >&2
        echo "  cmdline: $cmdline" >&2
        exit 1
    fi
}

assert_cmdline_not_contains() {
    local karg="$1"
    local msg="${2:-unexpected '$karg' in cmdline}"
    local cmdline
    cmdline=$(parse_cmdline)
    if echo "$cmdline" | grep -qw "$karg"; then
        echo "FAIL: $msg" >&2
        echo "  cmdline: $cmdline" >&2
        exit 1
    fi
}

is_staged() {
    bootc status --json | python3 -c \
        "import sys,json; s=json.load(sys.stdin); print('yes' if s.get('status',{}).get('staged') else 'no')"
}

# ---------- Scenario 1: cross-consumer staging ----------

boot_0() {
    echo "=== Boot 0: Cross-consumer staging (bootc then rpm-ostree) ==="

    # Save system kargs for verification across all reboots
    save_system_kargs

    # Step 1: bootc stages source-tracked kargs
    bootc loader-entries set-options-for-source \
        --source crosstest \
        --options "cross1=a cross2=b"

    test "$(is_staged)" = "yes" || { echo "FAIL: bootc should have staged a deployment" >&2; exit 1; }
    echo "ok: bootc staged crosstest source kargs"

    # Step 2: rpm-ostree re-stages with an unrelated karg, replacing
    # the staged deployment.  The crosstest extension keys must survive.
    rpm-ostree kargs --append=rpmarg=yes

    test "$(is_staged)" = "yes" || { echo "FAIL: should still be staged after rpm-ostree kargs" >&2; exit 1; }
    echo "ok: cross-consumer staging set up"

    tmt-reboot
}

boot_1() {
    echo "=== Boot 1: Verify cross-consumer staging results ==="

    assert_cmdline_contains "cross1=a" "crosstest cross1=a should survive rpm-ostree re-staging"
    assert_cmdline_contains "cross2=b" "crosstest cross2=b should survive rpm-ostree re-staging"
    assert_cmdline_contains "rpmarg=yes" "rpm-ostree rpmarg=yes should be present"
    verify_system_kargs

    # Verify BLS source key
    local source_keys
    source_keys=$(read_bls_source_keys)
    echo "$source_keys" | grep -q "x-options-source-crosstest" \
        || { echo "FAIL: x-options-source-crosstest BLS key missing" >&2; exit 1; }
    echo "$source_keys" | grep "x-options-source-crosstest" | grep -q "cross1=a" \
        || { echo "FAIL: crosstest key missing cross1=a" >&2; exit 1; }
    echo "ok: cross-consumer staging verified"

    # Clean up scenario 1
    bootc loader-entries set-options-for-source --source crosstest
    rpm-ostree kargs --delete=rpmarg=yes
    echo "ok: scenario 1 cleaned up"

    tmt-reboot
}

# ---------- Scenario 2: multiple sources + rpm-ostree coexistence ----------

boot_2() {
    echo "=== Boot 2: Verify cleanup, set up triple coexistence ==="

    # Verify scenario 1 cleanup
    assert_cmdline_not_contains "cross1=a" "crosstest should be gone"
    assert_cmdline_not_contains "rpmarg=yes" "rpmarg should be gone"
    verify_system_kargs
    echo "ok: scenario 1 cleanup verified"

    # Set two independent bootc sources
    bootc loader-entries set-options-for-source \
        --source tuned --options "nohz=on rcu_nocbs=2-7"
    bootc loader-entries set-options-for-source \
        --source dracut --options "rd.driver.pre=vfio-pci"

    # Now rpm-ostree adds a local karg on top -- three owners coexisting
    rpm-ostree kargs --append=localkarg=fromrpm

    test "$(is_staged)" = "yes" || { echo "FAIL: should be staged" >&2; exit 1; }
    echo "ok: triple coexistence staged (tuned + dracut + rpm-ostree)"

    tmt-reboot
}

boot_3() {
    echo "=== Boot 3: Verify triple coexistence ==="

    # All three owners' kargs must be present on the cmdline
    assert_cmdline_contains "nohz=on" "tuned nohz=on should be present"
    assert_cmdline_contains "rcu_nocbs=2-7" "tuned rcu_nocbs=2-7 should be present"
    assert_cmdline_contains "rd.driver.pre=vfio-pci" "dracut karg should be present"
    assert_cmdline_contains "localkarg=fromrpm" "rpm-ostree local karg should be present"
    verify_system_kargs
    echo "ok: triple coexistence kargs verified on cmdline"

    # The ownership metadata must have survived too.  When rpm-ostree
    # re-staged in boot 2 it passed the booted deployment -- which still
    # carried the empty x-options-source-crosstest tombstone from scenario
    # 1's cleanup -- as merge_deployment, without touching extension keys.
    # ostree must therefore inherit the previously-staged set (tuned +
    # dracut) rather than the stale on-disk tombstone.  Without the
    # metadata, bootc cannot compute the diff in scenario 3 and the old
    # kargs would be orphaned on the cmdline.
    local source_keys
    source_keys=$(read_bls_source_keys)
    echo "$source_keys" | grep "^x-options-source-tuned " | grep -qw "rcu_nocbs=2-7" \
        || { echo "FAIL: x-options-source-tuned key missing or stale after rpm-ostree re-staging: $source_keys" >&2; exit 1; }
    echo "$source_keys" | grep "^x-options-source-dracut " | grep -qw "rd.driver.pre=vfio-pci" \
        || { echo "FAIL: x-options-source-dracut key missing or stale after rpm-ostree re-staging: $source_keys" >&2; exit 1; }
    echo "ok: source ownership metadata survived rpm-ostree re-staging"

    # ---------- Scenario 3: source replacement with rpm-ostree kargs active ----------
    # Builds on the state verified above, so stage it from this boot
    # rather than spending another reboot on it.

    # Replace tuned source with different kargs.  The dracut source
    # and rpm-ostree local karg must be unaffected.
    bootc loader-entries set-options-for-source \
        --source tuned --options "nohz=on skew_tick=1"

    # Remove dracut source entirely
    bootc loader-entries set-options-for-source --source dracut

    echo "ok: source replacement + removal staged"

    tmt-reboot
}

boot_4() {
    echo "=== Boot 4: Verify source replacement results ==="

    # tuned: old rcu_nocbs gone, new skew_tick present, nohz preserved
    assert_cmdline_contains "nohz=on" "tuned nohz=on should survive replacement"
    assert_cmdline_contains "skew_tick=1" "tuned skew_tick=1 should be added"
    assert_cmdline_not_contains "rcu_nocbs=2-7" "old tuned rcu_nocbs should be gone"

    # dracut: fully removed
    assert_cmdline_not_contains "rd.driver.pre=vfio-pci" "dracut karg should be gone"

    # rpm-ostree: unaffected by bootc source operations
    assert_cmdline_contains "localkarg=fromrpm" \
        "rpm-ostree local karg must survive bootc source replacement"

    verify_system_kargs

    # Ownership metadata: tuned updated, dracut tombstoned (empty value)
    local source_keys
    source_keys=$(read_bls_source_keys)
    echo "$source_keys" | grep "^x-options-source-tuned " | grep -qw "skew_tick=1" \
        || { echo "FAIL: x-options-source-tuned not updated: $source_keys" >&2; exit 1; }
    if echo "$source_keys" | grep "^x-options-source-tuned " | grep -qw "rcu_nocbs=2-7"; then
        echo "FAIL: x-options-source-tuned still lists old karg: $source_keys" >&2; exit 1
    fi
    if echo "$source_keys" | grep "^x-options-source-dracut " | grep -qw "rd.driver.pre=vfio-pci"; then
        echo "FAIL: x-options-source-dracut should be tombstoned: $source_keys" >&2; exit 1
    fi

    echo "ok: source replacement verified, rpm-ostree kargs unaffected"

    # Final cleanup
    bootc loader-entries set-options-for-source --source tuned
    rpm-ostree kargs --delete=localkarg=fromrpm
    echo "ok: all test kargs cleaned up"

    tmt-reboot
}

boot_5() {
    echo "=== Boot 5: Final cleanup verification ==="

    assert_cmdline_not_contains "nohz=on" "tuned should be gone"
    assert_cmdline_not_contains "skew_tick=1" "tuned should be gone"
    assert_cmdline_not_contains "localkarg=fromrpm" "rpm-ostree karg should be gone"
    assert_cmdline_not_contains "rd.driver.pre=vfio-pci" "dracut should be gone"
    verify_system_kargs

    # Source key tombstones may linger (empty-valued x-options-source-*
    # keys).  Only check that no source keys have non-empty values.
    local source_keys
    source_keys=$(read_bls_source_keys)
    local live_keys
    live_keys=$(echo "$source_keys" | grep -v '^\S\+ *$' | grep -v '^$' || true)
    # Check for keys that have actual values (not just the key name with no value)
    for key in $live_keys; do
        local val
        val=$(echo "$key" | sed 's/^x-options-source-[^ ]* *//')
        if [ -n "$val" ]; then
            echo "FAIL: source key still has value: $key" >&2
            exit 1
        fi
    done

    echo "ok: all scenarios passed, system clean"
}

case "$REBOOT_COUNT" in
    0) boot_0 ;;
    1) boot_1 ;;
    2) boot_2 ;;
    3) boot_3 ;;
    4) boot_4 ;;
    5) boot_5 ;;
    *)
        echo "FAIL: unexpected reboot count: $REBOOT_COUNT" >&2
        exit 1
        ;;
esac

echo "PASS: boot $REBOOT_COUNT complete"

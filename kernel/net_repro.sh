#!/usr/bin/env bash
# CARMIX Track B - B6 REPRODUCIBILITY wrapper.
# One command boots BOTH QEMU machines from clean THREE times and emits the
# measured cold/warm-per-hop/reject table a third party can reproduce:
#   1. clean        : B1 round-trip + B2 cold + B3/B4 three warm hops (per-hop bytes)
#   2. leaf-tamper  : B5a - A corrupts a LEAF block on the wire; B rejects (BLAKE3)
#   3. node-tamper  : B5b - A corrupts a NODE block on the wire; B rejects (BLAKE3)
# Each variant is a full rebuild of nettest.elf via net_build.sh (kernel.c and the
# proven modules are NOT touched; store/ + sls/ are LINKED only).
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
run(){ # $1=label  $2=NETTEST_DEFS
  echo "############################################################"
  echo "## VARIANT: $1   (NETTEST_DEFS='$2')"
  echo "############################################################"
  NETTEST_DEFS="$2" bash "$HERE/net_build.sh" > "/tmp/repro_$1.log" 2>&1
  cp "$HERE/serial_A.log" "/tmp/repro_${1}_A.log"
  cp "$HERE/serial_B.log" "/tmp/repro_${1}_B.log"
  echo "  [A] $(grep -c '#### A DONE' "$HERE/serial_A.log" >/dev/null && echo 'completed' || echo 'STALLED')   [B] $(grep -c '#### B DONE' "$HERE/serial_B.log" >/dev/null && echo 'completed' || echo 'STALLED')"
}

run clean       ""
run leaf-tamper "-DTAMPER_TEST"
run node-tamper "-DNODE_TAMPER_TEST"

echo ""
echo "================= B6 CONSOLIDATED MEASURED TABLE ================="
echo "--- clean: B (destination) per-hop bytes-on-wire ---"
sed -n '/MEASURED (per hop)/,/B DONE/p' /tmp/repro_clean_B.log | grep -E 'cold|hop[0-9]|tracks'
echo ""
echo "--- clean: A (source) per-hop check ---"
grep -E 'warm hop|B4 CHECK' /tmp/repro_clean_A.log
echo ""
echo "--- leaf-tamper (B5a): B rejects the forged LEAF block ---"
grep -E 'TAMPER\(leaf\)' /tmp/repro_leaf-tamper_A.log || true
grep -E 'MISMATCH|cold pull done' /tmp/repro_leaf-tamper_B.log
echo ""
echo "--- node-tamper (B5b): B rejects the forged NODE block (distinct from leaf) ---"
grep -E 'TAMPER\(node\)' /tmp/repro_node-tamper_A.log || true
grep -E 'MISMATCH|cold pull done' /tmp/repro_node-tamper_B.log
echo ""
echo "--- D2 trust-boundary table (signed-authz gate; from the clean run) ---"
sed -n '/verify-before-remint/,/STEP 12/p' /tmp/repro_clean_B.log | grep -E 'case0|A[0-9]|TRUST BOUNDARY|legit case0'
echo ""
echo "--- K KEY-LIFECYCLE table (Step 12 distribution/rotation/revocation; clean run) ---"
sed -n '/apply lifecycle/,/B DONE/p' /tmp/repro_clean_B.log | grep -E '^  \[|KEY-LIFECYCLE TABLE|KA[0-9]|legit enroll|KEY LIFECYCLE'
echo ""
echo "Full serial per variant: /tmp/repro_<variant>_{A,B}.log"
echo "================================================================="

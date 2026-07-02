# CARMIX pre-public verification (CHECK-AND-REPORT ONLY)

Auditor pass over the PUBLIC copy at `Desktop/CARMIX`, with `Desktop/revoOS` as the
byte-identity reference. No proven module, behavior, or feature was changed. The only writes
are this report, `kernel/boot_full.log` (the captured transcript), `TESTING.md`, and two
`.gitignore` lines. Build/boot was run with `BOOT_SECS=180`; the full serial transcript is in
`kernel/boot_full.log` and every quote below is from it.

---

## THE PROOF BOUNDARY (added 2026-07-02)  -  READ THIS FIRST

This report reproduces serial lines verbatim, and some of those lines print the
words `PROVEN` / `HOLDS`. To an outside reader those are runtime observations in
an emulator, not machine-checked mathematics. The exact boundary is:

- **Machine-checked (Coq, all `Qed`, nothing `Admitted`):** only the results in
  `proofs/Carmix.v`. The two headline results are `anti_amplification` (a valid
  re-minted seven-permission capability never exceeds its source) and
  `acyclic_graph` (a finite content-addressed object tree is acyclic, under the
  single stated `Identity_injective` collision-resistance hypothesis). The same
  file also machine-checks the W^X / forbidden-permission corollaries of that one
  `re_mint`. Re-verify with `cd proofs && make` (coqc + coqchk); see
  `docs/PROOFS.md`.
- **Everything else in this report is model-proven and C-tested, observed in the
  QEMU emulator.** The self-test PASS lines in Section A (capability gate, store,
  GC, filesystem/persistence, migration, userland, driver, graphics, DRCC
  exercise) are designed and tested against attack tables and gated predicates,
  not machine-checked. The word `PROVEN` on a serial line means "gated by a
  computed predicate in `kernel.c`", not "proved in Coq".
- **DRCC (Deterministically Reproducible Consistent Cut) is paper-argued.** A
  first measured exercise exists (U-3 in `kernel/U3_LOG.md`; CV8 in
  `kernel/CV_LOG.md`, commit `1570fab`) covering the exercised workloads and
  schedules only; a universal proof is pending.

Evidence pointers: the Coq boundary is `proofs/Carmix.v`; the runtime transcript
is `kernel/boot_full.log` (Section B below); the two-machine byte counts are
`kernel/net_repro.sh`; per-subsystem detail is in the dated `kernel/*_LOG.md`
records.

---

## SECTION A  -  CHEAT / OVERCLAIM AUDIT

For each self-test stage: what the PASS line CLAIMS vs what the code in `kernel.c` ACTUALLY
does, with a verdict. **HONEST** = the print matches the code's gated predicate.
**SERVICED-DIRECTLY-AND-SAID-SO** = the stage drives a core routine directly (not via a live
async event) AND the serial line says so. **OVERCLAIM** = a PASS line asserts something the
code does not do (target: zero).

| STAGE | PASS line CLAIMS | code ACTUALLY does | VERDICT |
|-------|------------------|--------------------|---------|
| B0 | "serial OK" | wrote to COM1, we are executing | HONEST |
| B1 | "long mode CONFIRMED" | reads CS + EFER.LMA bit (kmain 4226-4229) | HONEST |
| B3 | "rw-readback=OK ... reused OK" | falloc->map_page->write 0xCAFEBABE...->read back==; LIFO realloc f3==f1 (4258-4269) | HONEST |
| B4 | "DRAWN TO SCREEN OK" | fills + draw_str then get_px readback of bg/rect/text (4272-4283) | HONEST |
| B5 | "timer OK" | PIT armed, hlt-loops until ticks advances 5 (4286-4289) | HONEST |
| E0 | "0 undefined / 0 libc / 0 CHERI" | llvm-nm/objdump greps in build.sh (38-41); log shows 0/0/0 | HONEST |
| E1 | "result=51 -> OK" | real cvsasx_wasm_load->check->exec, out==51 gated (505-515) | HONEST |
| E2 | "control=ACCEPT; 6/6 by INTENDED gate -> HOLDS" | each attack calls cvsasx_sw_cap_remint, asserts DISTINCT status AND c.valid==0 (534-548) | HONEST |
| E3 | "software SFI fault=1 (kernel intact)" | runs OOB module, reads backend fault flag (550-555) | HONEST |
| E4 | "DRAWN (WASM RESULT=51)" | draws panel, get_px readback gate (557-566) | HONEST |
| R0 | "MATCH (...trustworthy)" | cvsasx_blake3("")==KAT_EMPTY byte compare, aborts if not (605-609) | HONEST |
| R2 | "checkpoint -> content-addressed" | cvsasx_store_put of live k_linmem (613-615) | HONEST |
| R3 | "bit-identical=y ... REMAT OK" | store_get into FRESH buf, byte-compare, fresh swcap remint==OK gated (616-629) | HONEST |
| R4 | "PROVEN (O(changed), no over/under-report)" | real cvsasx_sls_diff; nch==1, self-diff nch0==0, 3-leaf nch3==3, tree_ok all gated; ratio=full/diff computed (631-675) | HONEST |
| R5 | "DRAWN (FULL/DIFF/RATIO visible)" | draws + get_px readback (677-687) | HONEST |
| S2 | "CONSOLE SCROLLS OK" | scroll_console then get_px proves marker moved up one line + 15 real scrolls (296-316) | HONEST |
| S3 | "WINDOW DRAWN OK" | compose() then get_px of desktop/frame/titlebar/content/title-text (317-326) | HONEST |
| S4 | "WINDOW MOVES OK" | key-driven move (line 333 SAYS "programmatic for the self-test"), get_px readback | HONEST (scripted, labelled) |
| S5 | "SHELL CONSOLE OK" | console region inside window rect, get_px text-inside + no-leak-left (341-353) | HONEST |
| S6 | "REMAT APP OK" | store_put/get counter round trip, remat==counter + drawn gate (354-369) | HONEST |
| A7 | "MOUSE MOVES CURSOR OK" | mouse_packet() injected, cur_x advances by dx, get_px cursor at new/old (372-385) | HONEST (scripted, labelled) |
| A8 | "DRAG MOVES WINDOW OK" | mouse_packet down+drag, window x changes, frame get_px (386-403) | HONEST (scripted) |
| A9 | "Z-ORDER OK" | distinct bg colors; get_px overlap == FRONT bg != back bg, genuine-overlap checked (404-417) | HONEST |
| E1(10b) | "FOCUS RAISE OK" | on_pointer clicks, get_px overlap follows raised window's bg (419-433) | HONEST (scripted) |
| E2(10b) | "FRONT-DRAG OK" | mouse_packet drag on focused titlebar, x moves, still-front (435-451) | HONEST (scripted) |
| E3(10b) | "RESIZE+CLAMP OK" | grip drag grows w/h, min-clamp==120x80 checked (452-471) | HONEST (scripted) |
| E4(10b) | live shell banner | run_shell loops poll_input() forever  -  REAL PS/2 input (472-479) | HONEST (live, terminal state) |
| P0 | "COOPERATIVE SWITCH OK" | task_create + switch_to interleave, switches counted | HONEST |
| P1 | "COUNTER SURVIVES YIELD/RESUME OK" | task checkpoints via store_put, yields, store_get rematerializes; before==after gated | HONEST |
| P2 | "PREEMPTED WITHOUT YIELD OK" | no-yield loop task, only timer ISR switches; timer-switches counted | HONEST |
| U0 | "TASK IS A CONTENT-ADDRESSED OBJECT OK" | dematerialize twice -> SAME hash (byte cmp), 1-byte perturb -> DIFFERENT hash (1042-1060) | HONEST |
| U1 | "ACTIVATE-BY-HASH ROUND-TRIP OK" | scribbles live stack 0xCC, materializes from H+store, hash_match byte cmp + counter survives (1062-1083) | HONEST |
| U2 | "INCREMENTAL COST TRACKS DIRTY SET OK" | dematerialize_incremental, dcS==1, bytes/cycles measured small<<large (1085-1100) | HONEST |
| U3 | "CROSSOVER ... THRASHES (CONFIRMED)" | rdtsc fast switch_to vs unified path sweep; honest "no size where hash beats switch" (1102-1121) | HONEST |
| U4 | "PERSISTENCE-AS-NOOP OK" | drop live task, rematerialize from ONLY H, counter+phase survive | HONEST |
| M0 | "FRAME DATABASE OK" | reserve/free/reuse, conservation sum==total, exhaustion returns 0 fail-closed | HONEST |
| M1 | "PER-TASK MAPPINGS OK" | same VA -> distinct phys per space, isolation read-back, unmap TLB-invalidate | HONEST |
| M2 | "MATERIALIZE + SHARE-BY-HASH OK" | rm_materialize verifies content, 2nd ref refcount=2; MEASURED dedup cyc via rdtsc | HONEST |
| M3 | "EVICTION + WRITEBACK + TEMPORAL POLICY OK" | refdrop refcount, writeback-before-reuse, evicted content re-verified | HONEST |
| M4 | "HW DIRTY BIT MATCHES SW DIFF, CHEAPER" | PTE D-bit scan vs memcmp; both find {1,4,6}; rdtsc costs (hw 9x cheaper) | HONEST |
| M5 | "FRAGMENTATION MEASURED OK" | 24 frames, 29% internal slack computed, checkerboard largest-run measured | HONEST |
| F0 | "#PF HANDLER + SAFETY OK" | pins metadata; protection (present=1) classified NOT-a-miss, pf_service returns 0 | HONEST |
| F1 | "REMATERIALIZING FAULT-IN (A) OK" | armed not-present, live touch -> #PF -> binding A -> materialize+verify -> read-after-fault correct | HONEST (live #PF) |
| F2 | "FAIL-CLOSED ON VERIFY FAILURE OK" | corrupt store; rm_materialize re-hash != requested -> ASSERT fires, no map, returns 0 | HONEST |
| F3 | "DEDUP-AT-FAULT OK" | two vaddrs one hash -> same frame, refcount++, store-fetches==1 | HONEST |
| F4 | "BINDING B + MEASURED A-vs-B OK" | 8+8 faults, per-fault rdtsc service cyc, avg table, B cheaper by measured delta | HONEST |
| F5 | "RE-ENTRANCY GUARDED + GAP NAMED OK" | guard refuses re-entry (returns 0); honestly NAMES the IST/TSS upgrade gap | HONEST |
| US0 | "RING-3 KERNEL ACCESS REFUSED AS PROTECTION OK" | real IRETQ to CPL3, ring-3 reads supervisor page -> live #PF present+user classified protection | HONEST (live) |
| US1 | "ROUND TRIP OK" | INT 0x80 SYS_ADD(40,2)==42 + SYS_GETTIME reached ring 3 | HONEST |
| US2 | "RE-MINT AT THE CROSSING OK" | SYS_READOBJ via re-minted bounded cap returns us_obj[7]==8 | HONEST |
| US3 | "TABLE HOLDS" | 6 amplifications each DISTINCT status fail-closed + replay refused + legit accept | HONEST |
| US4 | "HASH-PASSING CLOSES COPY-AT-USE TOCTOU" | store_get + cvsasx_hash_eq(loaded,hash); TOCTOU mutation -> different hash; honest "RELOCATES trust" caveat (2479-2517) | HONEST |
| L0 | "ELF valid ... REJECTED malformed OK" | real elf_parse, PT_LOAD enumerated, corrupted magic -> -1 | HONEST |
| L1 | "W^X HELD OK" | segments materialized+BLAKE3-verified, R-X vs RW- mappings, no W+X | HONEST |
| L2 | "LOADED ELF ran in ring 3 OK" | enters materialized-by-hash code, its own movq marker + SYS_WRITE observed | HONEST (live ring-3) |
| L3 | "authority bounded AT BIRTH" | in-ceiling accept, over-ceiling BAD_BOUNDS, +STORE_CAP AMPLIFY_PERMS, distinct | HONEST |
| L4 | "code shares ONE frame, data private OK" | same code frame refcount=3, distinct data/stack frames; honest "run SEQUENTIALLY" note | HONEST |
| C0 | "TWO RING-3 CONCURRENTLY TIMER-INTERLEAVED OK" | 2 spaces, shared code by hash, interleaved SYS_WRITE under 100Hz preemption | HONEST (live) |
| C1 | "PER-PROCESS CEILINGS ... no leak OK" | each over-ceiling DISTINCT status; cross-process referent -> status 8 refused | HONEST |
| C2 | "...REMATERIALIZE...COUNTER SURVIVED OK" | conc_dematerialize to H, frames freed (VA resolves NO), conc_rematerialize BLAKE3-verified, counter==before, resume to 8 (3760-3784) | HONEST |
| C3 | "MEASURED resume-resident vs resume-from-hash" | rdtsc over 256 context restores vs one real rematerialize; ratio printed; "COMPUTED via rdtsc" (3790-3809) | HONEST |
| PP0 | "KEPT RESIDENT, CHEAP RESUME OK" | should_dematerialize under no-pressure -> KEEP, proc.present checked | HONEST |
| PP1 | "PRESSURE-GATED ... freed frames OK" | no-pressure KEEP; induced pressure DEMAT; frames-freed measured (3340-3362) | HONEST |
| PP2 | "0% mispredict on this mix OK" | per-workload signal -> decision vs truth, mispred counted (==0) (3375-3399) | HONEST |
| PP3 | "LONGER-THAN-D* GAIN, SHORTER LOSS OK" | break-even from MEASURED C3 cyc; NET table from the formula (3402-3427) | HONEST |
| PP4 | "BACKOFF BOUNDED THRASH OK" | cooldown budget; demats==1 over 7 rounds, kept-by-backoff==6 (3431-3463) | HONEST |
| FA0 | "MEASURED PROGRESS DEFICIT OK" | tax = LIVE C3 (fromhash-resident); V pays it, R does not; deficit measured (3553-3567) | HONEST |
| FA1 | "DEFICIT STOPS GROWING ... RECOVERS OK" | control OFF deficit climbs, ON it stops; progress recovers (3571-3595) | HONEST |
| FA2 | "BOUNDED CONVERGENCE ... neither starved OK" | per-turn trace, grant bounded by FA_GRANT_CAP, V rises toward R never exceeds (3597-3621) | HONEST |
| FA (caveat) | "rests on 4/10 research ... NOT proven-optimal" | explicit honesty caveat, not a PASS overclaim | HONEST |
| PM0 | "EXTEND-HEAP ... DEMAND-ZERO BACKED ... OK" | ring-3 extend-heap through gate; 3 demand-zero #PFs; sum==0x6666, base==PM_HEAP_VA (4004-4027) | HONEST (live #PF) |
| PM1 | "OVER-CEILING REFUSED ... DISTINCT REASON OK" | in-ceiling grant; over-ceiling status==BAD_BOUNDS gated (4029-4045) | HONEST |
| PM2 | "serviced via the #PF core -> rematerialized by hash (BLAKE3-verified) ... bit-identical OK" | dematerialize_frame (hash+store), free frame, then **pf_service() called DIRECTLY** (4088); comment 4077-4083 explains a live touch on the single-stack handler would halt the boot; pf_last_serviced==1 + bit-identical byte compare gated (4047-4099) | SERVICED-DIRECTLY-AND-SAID-SO |
| PM3 | "DEDUPED TO ONE FRAME BY HASH; COW OK" | hash_eq + full byte-verify, rm_materialize same frame refcount>=2, COW preserves P1; honest tradeoff note (4102-4160) | HONEST |
| PM4 | "HOT PAGE EXCLUDED ... U3 COST MEASURED OK" | hot-write counter, exclusion predicate, rdtsc hash cost AVOIDED measured (4162-4210) | HONEST |

### PM2 special check (requested)

The SHIPPED serial line is exactly:

```
PM2 forced access (serviced via the #PF core) -> rematerialized by hash (BLAKE3-verified), first byte=0x...c0 expect 0x...c0, bit-identical=y, cost=204535485 cyc
```

It explicitly says **"serviced via the #PF core"**  -  NOT a bare "faulted" claim. The code
(`kernel.c:4088`) calls `pf_service(cold_va, 0x4, 0xdead)` directly (the same C core the live
#PF vector runs via `isr_pf` -> `pf_service` at line 130), with a code comment naming why
(`4077-4083`: a live faulting touch into a freshly-unmapped low VA halts the boot on the
single-stack handler; F1 already proves the live #PF entry). **VERDICT: SERVICED-DIRECTLY-
AND-SAID-SO, not an overclaim.** (Contrast: F1/US0/C0/PM0 use REAL live #PF/ring-3 faults,
quoted with `#PF @vaddr=...` entry lines.)

### Cycle-count check (requested)

`rdtsc()` is a genuine `rdtsc` instruction (`kernel.c:921`). Grep for a literal cycle count
printed as MEASURED (`sdec([0-9]+); sputs(" cyc`) returns **zero matches**. Every `cyc`/`cycles`
print uses a variable derived from `rdtsc()` differences. The C3 anchors `g_c3_resident_cyc`
/ `g_c3_fromhash_cyc` consumed by PP/FA are SET from rdtsc in C3 (`3796-3806`) and published
to globals  -  not hardcoded. The one numeric literal near a cyc line, `sdec(4096)` (1507), is
labelled "bytes" (page size), not cycles. **No fabricated cycle counts.**

### "verified" / "BLAKE3" check (requested)

Every "verified" / "BLAKE3-verified" claim is backed by a real hash comparison, not a print:
`rm_materialize` re-hashes loaded bytes and `cvsasx_hash_eq(&chk,h)`, failing closed on
mismatch (`1438-1441`)  -  this is the path F1/F2/F3/F4/C2/PM2 all run; F2 demonstrates the
ASSERT firing on a corrupt store. R0 is a KAT byte-compare; U0/U1 compare hashes byte-by-byte;
US4 uses `cvsasx_hash_eq`. **No print-only "verified" claims found.**

**SECTION A RESULT: ZERO bare overclaims.** Every PASS line maps to a gated, computed
predicate. The one direct-service stage (PM2) says so on the wire.

---

## SECTION B  -  EMULATION PROOF

### Build summary line (from boot_full.log)

```
E0 link OK: kernel.elf = 329232 bytes (backend linked in)
E0 undefined symbols : 0  (expect 0)
E0 libc/host symbols : 0  (expect 0; whole-name match)
E0 CHERI instructions: 0  (expect 0)
```
Limine BIOS stages installed successfully; ISO built; boot reached.

### Stage checklist (final PASS/stall line, verbatim)

```
B1  long mode: CS=0x0000000000000028 EFER.LMA=1 -> 64-bit CONFIRMED
B3  ... rw-readback=OK  free+realloc f3=0x...100000 (reused) OK
B4  ... -> DRAWN TO SCREEN OK
B5  ... ticks incrementing = 8 -> timer OK
E1 wasm validate->check->exec in kernel: result=51 (expect 51) -> OK
E2 anti-amplification in kernel: control=ACCEPT; 6/6 attacks rejected by their INTENDED gate -> HOLDS
E3 OOB guest load@100000 -> software SFI fault=1 (kernel intact) -> OK
E4 result on framebuffer: DRAWN (WASM RESULT=51 visible on screen)
R0 vs official af1349b9..41f3262 -> MATCH (content-addressing trustworthy)
R3 rematerialize: bit-identical=y fresh-cap remint=0 len=64 -> REMAT OK
R4 diff-proportional + soundness: 1-leaf=1? y self-diff=0? y 3-leaf=3? y tree-ok? y -> PROVEN (O(changed), no over/under-report)
R5 result on framebuffer: DRAWN (REMAT OK, FULL/DIFF/RATIO visible)
S2 ... -> CONSOLE SCROLLS OK
S3 ... -> WINDOW DRAWN OK
S4 ... -> WINDOW MOVES OK
S5 ... -> SHELL CONSOLE OK
S6 ... -> REMAT APP OK
A7 ... -> MOUSE MOVES CURSOR OK
A8 ... -> DRAG MOVES WINDOW OK
A9 ... -> Z-ORDER OK (front's color occludes back)
E1 ... -> FOCUS RAISE OK
E2 ... -> FRONT-DRAG OK
E3 ... -> RESIZE+CLAMP OK
P0 ... -> COOPERATIVE SWITCH OK
P1 ... -> COUNTER SURVIVES YIELD/RESUME OK
P2 ... -> PREEMPTED WITHOUT YIELD OK
U0 -> TASK IS A CONTENT-ADDRESSED OBJECT OK
U1 -> ACTIVATE-BY-HASH ROUND-TRIP OK
U2 -> INCREMENTAL COST TRACKS THE DIRTY SET OK (small<<large)
U3 CONCLUSION: ... activate-by-hash pays off ONLY at COARSE boundaries ... (honest crossover)
U4 ... -> PERSISTENCE-AS-NOOP OK (live form == durable form)
M0 -> FRAME DATABASE + FRESH ALLOCATION OK
M1 -> PER-TASK MAPPINGS + map/remap OK
M2 -> MATERIALIZE + SHARE-BY-HASH OK
M3 -> EVICTION + WRITEBACK ROUND-TRIP + TEMPORAL POLICY OK
M4 -> HARDWARE DIRTY BIT MATCHES SOFTWARE DIFF, CHEAPER SCAN
M5 -> FRAGMENTATION MEASURED OK
F0 -> #PF HANDLER + SAFETY (pin + protection-classification) OK
F1 -> REMATERIALIZING FAULT-IN (BINDING A) OK
F2 -> FAIL-CLOSED ON VERIFY FAILURE OK
F3 -> DEDUP-AT-FAULT OK
F4 -> BINDING B + MEASURED A-vs-B OK
F5 -> RE-ENTRANCY GUARDED + PRECISE GAP NAMED OK
US0 ... -> RING-3 KERNEL ACCESS REFUSED AS PROTECTION (not a miss) OK
US1 ... -> RING3->RING0->RING3 ROUND TRIP OK
US2 ... -> RE-MINT AT THE CROSSING OK
US3 result: 6/6 amplifications refused by their INTENDED gate + replay refused + legit accepted -> TABLE HOLDS
US4 result: HASH-PASSING CLOSES THE COPY-AT-USE TOCTOU
L0 ... ELF64 valid ... malformed ... REJECTED (fail-closed) OK
L1 W^X across all segments + stack: HELD (no segment is W+X) OK
L2 -> the LOADED ELF ran in ring 3 ... returned cleanly OK
L3 -> authority bounded AT BIRTH ... each REFUSED by a DISTINCT reason
L4 code share-by-hash: SAME physical frame, refcount>=2 OK / data privacy: distinct ... OK
C0 -> TWO RING-3 PROCESSES RAN CONCURRENTLY, TIMER-INTERLEAVED ... OK
C1 -> PER-PROCESS CEILINGS BOUND EACH UNDER CONCURRENCY ... no leak OK
C2 -> DESCHEDULE->DEMATERIALIZE->REMATERIALIZE->RESUME, COUNTER SURVIVED OK
C3 ratio from-hash/resident ~ 73x (rdtsc under TCG is noisy run-to-run = real)
PP0 -> NO PRESSURE => KEPT RESIDENT, CHEAP RESUME OK
PP1 -> PRESSURE-GATED: ... freed frames under pressure OK
PP2 -> HEURISTIC CHOSE CORRECTLY FOR EACH (0% mispredict on this mix) OK
PP3 -> LONGER-THAN-D* NETS A GAIN, SHORTER NETS A LOSS (anchored in C3) OK
PP4 -> BACKOFF BOUNDED THRASH: paid ONCE, kept resident every retry OK
FA0 -> WITHOUT THE CONTROL ... ACCUMULATES A MEASURED PROGRESS DEFICIT OK
FA1 -> WITH THE CONTROL THE DEFICIT STOPS GROWING ... V's PROGRESS RECOVERS OK
FA2 -> BOUNDED CONVERGENCE: both toward equal net progress, neither starved OK
PM0 -> EXTEND-HEAP GRANTED THROUGH THE GATE, DEMAND-ZERO BACKED ... OK
PM1 -> IN-CEILING GRANTED, OVER-CEILING REFUSED BY THE PROVEN GATE ... OK
PM2 -> IDLE HEAP PAGE DEMATERIALIZED ... + REMATERIALIZED BIT-IDENTICAL ON FAULT OK
PM3 -> TWO INDEPENDENT IDENTICAL HEAP PAGES DEDUPED ... COW PRESERVED PRIVACY OK
PM4 -> UNDER PRESSURE: COLD PAGE DEMATERIALIZED, HOT CHURNING PAGE EXCLUDED ... OK
=== STEP 10b E4: live desktop - click=focus, titlebar=drag, grip=resize, keys=type ===
```

### Counts

- **PASS: every stage above (B/E/R/S/A/E-10b/P/U/M/F/US/L/C/PP/FA/PM).** All terminal lines
  are `OK` / `HOLDS` / `PROVEN`.
- **Honest stalls / named gaps (NOT failures): 2**  -  F5 names the IST/TSS nested-fault upgrade
  gap; FA names the "4/10 research, not proven-optimal" caveat. Both are explicit honesty,
  not PASS overclaims.
- **FAIL: 0.** No `*** FAIL ***`, no `*** WRONG-REASON / AMPLIFIED ***`, no triple-fault, no
  hang. The boot reaches `run_shell` (E4) and parks there polling live input  -  the correct
  terminal state.

**No regression:** the full stage list (B0..PM4 + live E4) is present and green, identical in
structure to the development sequence in MEMORY.

---

## SECTION C  -  OPEN-A-WINDOW / DESKTOP

### Framebuffer + window proof (serial, verbatim)

```
B4  FB 1280x800 bpp=32 @0xffff8000fd000000
    readback bg=0x...101830 rect=0x...20a040 text-set=y  -> DRAWN TO SCREEN OK
S3 readback desktop=0x...204060 frame=0x...c0c0c0 titlebar=0x...000080 content=0x...101820 title-text=y -> WINDOW DRAWN OK
A7 cursor (400,400)->(460,400) ... -> MOUSE MOVES CURSOR OK
A8 window (300,200)->(390,200) via drag; frame-at-new=0x...c0c0c0 old-now-bg=y -> DRAG MOVES WINDOW OK
A9 overlap@(450,400) shows=0x...301020 front(g_win2).bg=0x...301020 back(g_win).bg=0x...102030 ... -> Z-ORDER OK
E1 overlap: B-front=0x...301020 click-A->0x...102030 click-B->0x...301020  -> FOCUS RAISE OK
E2 focused A (200,200)->(320,200) still-front=y ... -> FRONT-DRAG OK
E3 size (400x300)->grow(540x440) ... min-clamp(120x80)=y -> RESIZE+CLAMP OK
```

### Honest statement on input

In the headless self-test run, the desktop draws and every window event (create, focus,
move/drag, resize, type) is driven by SCRIPTED in-kernel self-tests (`mouse_packet()`,
`on_pointer()`, programmatic key/move) and PROVEN by framebuffer pixel-readback over serial.
**No human is clicking** in that run. Only the final stage, `run_shell` (E4), takes LIVE
input: it loops `poll_input()` forever, reading the QEMU-provided PS/2 keyboard and mouse.

### Exact QEMU commands

Headless self-test command (what `build.sh` runs, `build.sh:64-65`):

```
qemu-system-x86_64 -M q35 -m 512M -cdrom carmix.iso \
    -serial stdio -display none -no-reboot \
    -L "$X86TOOLS/usr/share/seabios" -L "$X86TOOLS/usr/share/qemu"
```

Graphical command  -  IMPORTANT: the prebuilt no-sudo QEMU at `$X86TOOLS` was built WITHOUT
gtk/sdl (`-display help` lists ONLY `none`). It DOES include the VNC server (verified: it
listens on 5901 and boots the same `carmix.iso`). So on THIS toolchain, the graphical path is
VNC:

```
qemu-system-x86_64 -M q35 -m 512M -cdrom kernel/carmix.iso \
    -vnc 127.0.0.1:1 -serial stdio -no-reboot \
    -L "$X86TOOLS/usr/share/seabios" -L "$X86TOOLS/usr/share/qemu"
# then: vncviewer 127.0.0.1:5901
```

On a standard distro QEMU built with a window backend, open a native window directly:

```
qemu-system-x86_64 -M q35 -m 512M -cdrom kernel/carmix.iso -display gtk -serial stdio -no-reboot
# (substitute -display sdl if gtk is unavailable)
```

### What a person WILL and WILL NOT see

WILL SEE: CARMIX boots, self-tests stream over serial, the kernel draws its OWN framebuffer
desktop (colored background, windows with titlebars + resize grip, yellow cursor, scrolling
consoles inside windows, WASM/REMAT result panels). At the E4 shell it takes live PS/2
mouse+keyboard from QEMU  -  move the cursor, click to focus/raise a window, drag by the
titlebar, resize by the grip, type into the focused console.

WILL NOT SEE: no third-party GUI apps, no browser, no GPU acceleration. Everything is
software-rendered by CARMIX's own code (the software-render ceiling is by design  -  no vendor
drivers).

---

## SECTION D  -  OUTSIDE-PERSON TEST PATH + HYGIENE

### Docs vs reality

- `docs/REPRODUCE.md`: MATCHES. `bash build.sh`, the 40s default, and `BOOT_SECS=180` to see
  later stages are all correct; the listed self-test descriptions match the observed serial.
- `docs/BUILD.md`: MOSTLY matches but has a small **documentation gap**  -  it lists
  `CVSASX_CLANG`, `CVSASX_LLD`, `LIMINE_DIR`, `X86TOOLS` (BUILD.md:27-32) but NOT
  `CVSASX_NM` / `CVSASX_OBJDUMP`, which `build.sh` ALSO requires (`build.sh:11-12`,
  `${CVSASX_NM:?...}` / `${CVSASX_OBJDUMP:?...}`). BUILD.md also never mentions `BOOT_SECS`.
  A newcomer following BUILD.md verbatim hits a loud `set CVSASX_NM to ...` error and stops at
  the 40s window. Not a build blocker (the script fails clearly and self-corrects), but the
  doc is incomplete. **Fix shipped as `TESTING.md`** (created this pass) which lists all six
  env vars + `BOOT_SECS` + the headless and graphical commands + what green looks like.

### Machine-specific blockers

- **NONE.** No host home-directory path (a Unix or Windows per-user directory) in any tracked file. No lowercase
  `louka` username in tracked files. No `$HOME` / `/root/` / `~/` default in any shipped `.sh`
  script  -  `build.sh` correctly uses `${VAR:?message}` required-env syntax for all six tools.
  (An earlier broad grep flagged LICENSE/kernel.c/nettest.c only on the author name "Loucas
  Louka", a false positive, not a path leak.)

### Publish hygiene (greps over tracked files + working tree)

| Check | Result |
|-------|--------|
| em-dash (U+2014) in tracked files | NONE |
| "fallback" (any case) in tracked files | NONE |
| "ponytail" (any case) in tracked files | NONE |
| host user-paths in tracked files | NONE |
| LICENSE present + AGPL-3.0 | YES  -  "GNU Affero General Public License", "Copyright (c) 2026 Loucas Louka" (correct, not flagged) |

### Build artifacts in the tree

`git ls-files` = 63 tracked files. **Zero** build artifacts tracked (no `.log/.iso/.elf/.o/
.aux/.cache`). Artifacts PRESENT in the working tree but untracked-and-gitignored (fine):

| file | tracked? | gitignored? |
|------|----------|-------------|
| kernel/boot_full.log | no | IGNORED (`*.log` + explicit) |
| kernel/carmix.iso | no | IGNORED (`*.iso`) |
| kernel/nettest.iso | no | IGNORED (`*.iso`) |
| kernel/serial_A.log | no | IGNORED (`*.log`, `serial_*.log`) |
| kernel/serial_B.log | no | IGNORED (`*.log`, `serial_*.log`) |

`.gitignore` covers all of them. `boot_full.log` (the captured serial transcript) stays listed
in `.gitignore` so it is never shipped; this report itself is published as the claim-boundary map.

Pre-existing working-tree note (NOT caused by this pass): `git status` shows `kernel/font8x8.h`
as modified, but the diff is line-endings only (CRLF). Content is byte-identical to both HEAD
and the revoOS reference (`git diff --ignore-cr-at-eol` is empty); mtime 2026-06-26 predates
this audit. No functional impact (the build/boot succeeded). Worth a `git checkout` or a CRLF
normalization before commit, but not a blocker.

### Proven-module byte-identity (comment-stripped: `clang -fpreprocessed -dD -E -P`)

| file | result |
|------|--------|
| carmix/swcap.c | CODE-IDENTICAL |
| carmix/backend_sw_gate.c | CODE-IDENTICAL |
| sls/sls.c | CODE-IDENTICAL |
| gate/sfi_checker.c | CODE-IDENTICAL |
| gate/executor.c | CODE-IDENTICAL |
| gate/optimize.c | CODE-IDENTICAL |
| store/object_store.c | CODE-IDENTICAL |
| cap/cvsasx_cap.h | CODE-IDENTICAL |
| sls/cvsasx_sls.h | CODE-IDENTICAL |

All nine proven modules are byte-identical (code) between revoOS and CARMIX.

---

## BOTTOM-LINE VERDICT

- **HONEST (zero bare overclaims): YES.** Every PASS line maps to a gated, computed predicate.
  The single direct-service stage (PM2) says "serviced via the #PF core" on the wire and names
  why in code; live faults (F1/US0/L2/C0/PM0) use real #PF/ring-3 entries. All cycle counts are
  rdtsc-computed (zero literals); all "verified"/"BLAKE3" claims do a real `cvsasx_hash_eq`/KAT
  compare; the explicit honesty notes (F5 gap, FA caveat, L4 sequential, US4 trust-relocation,
  PM3 tradeoff) strengthen rather than smooth the story.
- **RUNNING (boots + emulates end to end, desktop draws): YES.** Build links with 0 undefined /
  0 libc / 0 CHERI; the 180s boot streams every stage green and reaches the live E4 desktop
  shell with no FAIL, no triple-fault, no hang. Framebuffer draws and window
  focus/drag/resize/type are pixel-readback proven; the VNC graphical command was verified to
  boot the same ISO.
- **TESTABLE (a newcomer can build and see it): YES**, with one minor doc gap now fixed.
  `TESTING.md` (added this pass) gives the complete six-env-var build command + `BOOT_SECS`,
  the headless self-test command, the VNC and native-window graphical commands, and what green
  output looks like. `docs/BUILD.md` should be amended to list `CVSASX_NM`/`CVSASX_OBJDUMP`
  and mention `BOOT_SECS` (not done here  -  report-only pass; the build fails loudly without
  them so it is self-correcting).

**No proven module, behavior, or feature was modified. No commit, no push.**

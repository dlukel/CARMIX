# CARMIX DESKTOP DISTRIBUTION RESEARCH (DD1-DD5)

Scope. Four desktop pillars re-derived from the CARMIX thesis or reported KILL-AS-GENERIC. Verdict exercise,
not a survey. Every pillar carries its thesis-native mechanism NAMED, a nearest-neighbor table, findings, a
verdict from the fixed set, an honest boundary, and a build spec. Critic fixes are incorporated inline (D6
bibliography added to DD3, D3 Nix put in DD1's table and the fusion re-narrowed, D4 parity statuses split and
re-verbed, DD3 delta sentence reconciled with its own router admission, D2 prize register stripped). All
bibliography entries are UNVERIFIED (recall, not fetched). Files referenced as kernel/... and gate/...

---

## DD1 - THE APPLICATION ABI (apps are hashes run through the gate)

Question. Is WASM plus a WASI subset the CARMIX app ABI, with modules as content-addressed objects, launched
by hash, imports bound to CAPABILITIES so an app with no filesystem capability cannot even express a filesystem
call?

### DD0 nearest-neighbor table

| Work (UNVERIFIED) | What it does | Delta vs CARMIX |
|---|---|---|
| Nix store + derivations + narinfo | Hash-addressed store unifies the build recipe (.drv provenance) with the output content under one name; narinfo Ed25519 signs path integrity | Two of the three fused legs (identity-as-hash, provenance-in-same-store) ARE Nix. Nix has NO import-authority notion at all: a store path cannot carry a per-import capability ceiling. CARMIX's only new leg is the AUTHORITY leg (import = anti-amp re-mint) |
| WASI preopens | WASM syscall API with no ambient FS; host hands preopen dirfds at launch, paths relative to them | preopen is a host-OS-path fd table, not content-addressed, no anti-amplification proof, no referent-hash binding. CARMIX resolves each import as a swcap re-mint under the proven monotone ceiling over a BLAKE3 referent |
| Wasmtime + wasmtime-wasi Linker | Production host binds import name-strings to host closures; WASI provides capability-shaped FS | Linker binds names to closures carrying ambient host authority; module identity is file bytes; no anti-amp lattice; no one-store identity-plus-authority |
| WAMR | Small embeddable WASM interpreter/AOT for embedded targets | imports are ambient native registrations; no capability ceiling, no content-addressed identity. An engine, not an authority model |
| Nebulet / kernel-wasm | Runs WASM modules in ring0 as the process model, SFI in place of MMU isolation | trusts a Cranelift JIT (large TCB) with no independent SFI re-checker, no capability anti-amp, no content-addressed identity. CARMIX = small FROZEN checker over an untrusted lowerer plus proven swcap ceiling |
| Fuchsia component model + Zircon | Components receive manifest-declared routed capabilities, no ambient authority, blobs are merkle-verified | closest on "imports = declared capabilities", but Zircon handles are per-process node-local indices (do not survive migration), routing is a plane separate from the blob store and is not a proven monotone lattice, and identity is not fused with authority in one re-mint |
| Component Model / WIT worlds | Worlds statically declare a module's imports/exports as typed interfaces | static declaration matches "undeclared FS means cannot name it", but binding to real authority is host-provided at instantiation with no anti-amp proof and no content-addressed provenance |
| CARMIX native u1_spawn (in-tree, kernel.c:6481) | Spawns a ring-3 ELF whose identity = image hash, born holding a strict subset of the launcher's caps; imports = cap-IPC slots | already realizes the ABI SEMANTICS but leans on MMU/ring-3 hardware isolation, so it is not machine-independent pure logic and does not rematerialize as cleanly as checked IR; import surface is dynamic cap-IPC, not a declared static set |

### Thesis-native mechanism

An import is not a name-string bound to an ambient host closure (Wasmtime Linker, WAMR native registration,
Fuchsia routed handle). An import IS a swcap grant re-minted from the launcher's ceiling through the PROVEN
anti-amplification path (cvsasx_sw_cap_remint, carmix/swcap.c:56, the same monotone re-mint whose Coq lemma the
header names as CLOSED). Load-time resolution: for each declared import the gate re-mints a bounded grant from
the launcher's cap set. An import for which the launcher holds no grant fails closed BEFORE the first
instruction. The module cannot be instantiated with that import, which is exactly what "cannot express a
filesystem call" means (identical shape to u1_spawn's D1 rule "cannot grant what the parent lacks",
kernel.c:6482). Identity: the app IS its BLAKE3 manifest hash in the content store (cvsasx_store_put and
re-hash-on-get). Provenance: {manifest_hash, resolved_import_grant_hashes[], declared-entropy values[]} is one
content-addressed object. Trust anchor is the FROZEN linear-time GIR checker (cvsasx_gate_check, gate/sfi_checker.c),
not the WASM format. WASM is merely one untrusted producer of checkable GIR (// TCB lowerer, re-verified).

### Findings

DD1-a. What the committed gate path executes today, and what the frontend lacks. Path exercised in-kernel at
run_step4/E1 (kernel.c:644-710): cvsasx_wasm_load (EDITABLE frontend) then cvsasx_gate_check (FROZEN) then
cvsasx_gate_exec (FROZEN native IR interpreter over the swcap backend). It runs exactly ONE thing: a single
straight-line i32 function. The frontend (gate/wasm_frontend.c) parses only magic plus version and sections
Type(1), Function(3), Memory(5), Code(10), each hard-capped to exactly 1. lower_expr accepts only i32.const,
local.get/set, i32 arithmetic and compares, one linear-memory load/store, drop, return, end. What it LACKS for
any real .wasm a normal toolchain emits: (1) THE IMPORT SECTION (id 2) is not parsed at all, it hits the
default skip at wasm_frontend.c:136, so there is today NO host-import surface and NO way to bind an import to a
capability. This is THE ABI blocker. (2) control flow (block/loop/if/else/br/br_if/br_table) all fall to
default return -1 at wasm_frontend.c:73, so no real WASM control flow loads (the FROZEN checker/executor already
implement GIR_BR/BR_IF/CALL_INDIRECT, so the backend is AHEAD of the editable frontend). (3) calls (0x10/0x11)
not lowered. (4) types beyond i32. (5) globals/tables/element/data/export/start sections skipped. (6) no
memory.grow, no data-segment init. (7) full join-point stack-height validation is deferred to a validator that
does not exist because control flow is unparsed. Headline: the committed path is a proof-carrying demonstration
of check-then-exec, not a loader for real modules.

DD1-b. Minimal WASI subset worth supporting (no ambient anything). Rule: every WASI function routes through a
re-minted capability, nothing reads ambient state. SUPPORT: args_get/args_sizes_get and
environ_get/environ_sizes_get sourced from a content-addressed arg/env object handed as a grant; path_open,
fd_read, fd_write, fd_close routed through a CA-FS DIRECTORY capability (path_open = fs_resolve, already
capability-gated per kernel/USER_MODEL.md; fd_read = store_get plus re-hash verify; fd_write = fs_dir_put
re-root, a write is a new version never in-place); proc_exit (pure). Clocks and randomness via the provenance
rule: clock_time_get and random_get are ambient nondeterminism, so a module that imports them must DECLARE
them, the value is injected as declared entropy and recorded in the provenance object, making the run
reproducible-by-hash up to that declared edge (this is run_dbg's "halts honestly at the first non-deterministic
external edge"). REFUSE (KILL, no thesis-native form): sock_*, poll_oneoff (liveness, Membrane boundary 1),
monotonic real-time clocks (same boundary), fd_advise/fd_allocate/mutating-fd_seek and any in-place mutation,
and every function that would confer ambient authority (no preopen inherited by default). Precise minimal set:
{ args_*, environ_* (CA object), path_open, fd_read, fd_write, fd_close (CA-FS dir grant), clock_time_get,
random_get (declared-entropy only), proc_exit }.

DD1-c. Hard adjudication vs the named neighbors, including Nix. Nix is now at the table because DD1's original
delta claimed a triple-fusion of identity plus authority plus provenance in one store, and Nix already covers
two of those three legs: a derivation (.drv) unifies the build recipe (provenance) with the output content
under one hash-addressed name, and narinfo Ed25519 binds path integrity. What Nix has NO form of is the
authority leg: a store path names inert files and cannot carry a per-import capability ceiling. So the honest
delta is NOT the triple. Identity-as-hash and provenance-in-one-store are Nix. The candidate delta is strictly
the AUTHORITY leg: import-resolution performed AS a re-mint under the PROVEN anti-amplification ceiling, fused
INTO the same content-addressed store that already holds identity and provenance. Against the WASM neighbors:
WASI preopens are host-OS-path fd tables with no anti-amp proof; Wasmtime Linker binds names to ambient
closures; WAMR registers ambient natives; Nebulet trusts a JIT with no re-checker; Fuchsia is the closest on
"imports = declared capabilities" and even has merkle-verified blobs, but Zircon handles are node-local, its
routing plane is separate from the blob store and is not a proven monotone lattice, and identity is not fused
with authority in one re-mint; WIT worlds declare imports statically but bind to host authority at
instantiation with no anti-amp proof. NONE of the WASM neighbors resolve an import as a re-mint under a proven
monotone lattice, and Nix (which owns identity-plus-provenance) has no import-authority at all. The authority
leg fused into the identity-plus-provenance store is the residue that no single neighbor covers.

DD1-d. Honest fallback if full WASM is out of reach now. It is out of reach now, and the blocker is named:
gate/wasm_frontend.c has no import-section (id 2) parser and no host-import binding path (secondarily no
control-flow/call lowering), and imports are the entire mechanism the ABI needs. The FROZEN checker/executor
are NOT the blocker (they already carry GIR_CALL_INDIRECT/GIR_BR). Two honest interim ABIs already exist
in-tree. (1) THE SFI-CHECKED IR PATH. Since the trust boundary is the GIR checker and not the WASM binary
format, the ABI can ship as "app = a content-addressed GIR module blob plus a declared import list, run through
cvsasx_gate_check then cvsasx_gate_optimize then cvsasx_gate_exec", with WASM deferred to a later frontend.
This keeps the whole proven trust story and is the smallest diff. (2) THE NATIVE ELF PATH. u1_spawn already
realizes the ABI semantics today (identity = image hash, child born holding a strict subset of the launcher's
caps, imports = cap-IPC slots), but it leans on ring-3/MMU hardware isolation so it is not machine-independent
and does not rematerialize/migrate as cleanly as checked IR, and its import surface is dynamic cap-IPC rather
than a declared static set. Recommendation for DE1: ship interim ABI (1), the content-addressed checked-IR
module with declared re-minted imports, because it preserves both the proven ceiling and the machine
independence that native ELF lacks, and it makes the eventual WASM frontend a pure add-on producer.

### Verdict: NOVEL-AS-FUSION (authority leg only)

Honest boundary. The verdict is a FUSION verdict re-narrowed after the critic's Nix check. Plain "WASM is the
app ABI" is COVERED and the gate header already concedes it ("Wasm-as-capability-layer is mainstream, gate/ is
an ENGINEERING module, not the novelty"); proposing WASM-as-ABI as itself novel would be KILL-AS-GENERIC.
Identity-as-hash and provenance-in-one-store are Nix (D3 correction: the original triple-fusion silently
omitted the one neighbor that covers two of its three legs). "Imports are declared capabilities, no ambient
authority" is PARTIALLY-COVERED by WASI preopens and most closely by Fuchsia. What survives is strictly the
AUTHORITY leg: import-resolution done as a re-mint under the PROVEN anti-amplification ceiling, fused into the
same store that already holds Nix-style identity and provenance. No named neighbor does that. Also honest: (a)
the swcap Coq lemma proves the re-mint monotonicity, NOT the WASM lowerer (untrusted, // TCB, re-verified by
the checker), so the ABI guarantee is only as strong as the FROZEN checker's modeled invariant set (VeriWasm
CVE-2021-32629 model-incompleteness applies, which is why the runtime bounds backstop is mandatory). (b) None
of this is built for real modules today; it is a spec over an existing single-i32-function demonstrator and the
import parser does not exist. (c) The WASI subset deliberately amputates sockets/poll/real-time-clocks, so this
is an ABI for deterministic capability-scoped compute, not a POSIX-competitive app surface, and that amputation
is the Membrane real-time/liveness boundary, not an implementation gap.

### Build spec

CARMIX App ABI v0 (DE1-consumable, interim = checked-IR path, WASM frontend deferred).

OBJECTS (all BLAKE3-named in the existing content store):
- code blob = a GIR module (gir_inst_t[]) as bytes.
- manifest = { code_hash, n_params, n_locals, mem_size, imports[], entropy[] }.
  - imports[i] = { kind: FS_DIR | CONSOLE | STORE_READ | ARGS | CLOCK | RANDOM, slot }.
  - entropy[i] = { kind: CLOCK | RANDOM, declared_value_or_seed }.
- app identity = hash(manifest). Different import surface gives a different hash (provenance unification).

LAUNCH run_app(manifest_hash, launcher_capset):
1. store_get(manifest_hash) plus re-hash verify (fail-closed on mismatch).
2. store_get(code_hash) to GIR; cvsasx_gate_check (FROZEN). REJECT means abort, fail-closed.
3. for each imports[i]: cvsasx_sw_cap_remint(launcher_ceiling to grant). If the launcher holds no source
   grant for that kind/slot, ABORT AT LOAD (the module is never instantiated with that import, which is
   "cannot express the call"). Mirrors u1_spawn D1 (kernel.c:6482).
4. for each entropy[i]: bind declared value. NO ambient rdtsc/rdrand reachable.
5. store_put(provenance = {manifest_hash, resolved_grant_hashes[], entropy_values[]}) to prov_hash.
6. cvsasx_gate_optimize (FROZEN, proven-elision only) then cvsasx_gate_exec (FROZEN): linmem load32/store32
   route through the linmem swcap (bounds backstop); host WASI calls dispatch via GIR_CALL_INDIRECT into a
   host-import table whose entries are the re-minted grants from step 3; a slot with no grant has no table
   entry and traps if reached (belt to the load-time refusal).

WASI SURFACE (each = a call over a re-minted grant, see DD1-b): args_*/environ_* (ARGS grant, CA object);
path_open/fd_read/fd_write/fd_close (FS_DIR grant, resolve = fs_resolve, read = store_get plus verify,
write = fs_dir_put re-root); clock_time_get/random_get (declared-entropy only); proc_exit. REFUSE all sock_*,
poll_oneoff, real-time-clock, in-place mutation.

DEFERRED (the named WASM blocker): add to gate/wasm_frontend.c (EDITABLE, frozen trio untouched) an
IMPORT-SECTION (id 2) parser that emits import_decls plus a host-import GIR_CALL_INDIRECT table, plus
control-flow (0x02-0x0e) and call (0x10/0x11) lowering. Until then DE1 consumes GIR module blobs directly
(compiler- or hand-emitted).

ONE runnable check for the ABI core: launch a 2-import manifest where the launcher holds import[0] but NOT
import[1]. Assert step 3 aborts fail-closed with the child never instantiated (re-mint returns error, no grant
table entry). This is the smallest test that fails if "no FS cap means cannot express FS call" ever regresses.

### Bibliography (all UNVERIFIED, recall not fetch, on-subject, no keyword collisions)

[U] Dolstra, The Purely Functional Software Deployment Model (Nix store, derivations), 2006.
[U] WASI / WebAssembly System Interface, capability-based design plus preopens (Bytecode Alliance docs).
[U] WebAssembly Component Model plus WIT worlds (Bytecode Alliance).
[U] Wasmtime Linker / wasmtime-wasi host-import binding (Bytecode Alliance).
[U] WAMR, WebAssembly Micro Runtime (Bytecode Alliance / Intel).
[U] Nebulet, a WASM microkernel running modules in ring0 via SFI (Weller / Rust OSdev).
[U] kernel-wasm, WASM in the Linux/Rust kernel via Cranelift (Wasmer).
[U] Fuchsia Component Framework plus Zircon handles plus verified blobfs merkleroots (Google Fuchsia docs).
[U] Faasm, faaslets as lightweight FaaS isolation with a two-tier state store (Shillaker and Pietzuch, USENIX ATC 2020).
[U] MSWasm, memory-safe WASM with swappable enforcement (Michael et al., POPL 2023).
[U] Cage, WASM memory safety on Arm MTE, 2024.
[U] RockSalt (Morrisett et al., PLDI 2012); VeriWasm (Johnson et al., NDSS 2021); CVE-2021-32629 (Lucet SFI model-incompleteness).
[U] rWasm / vWasm, verified WASM sandboxing (Bosamiya et al., 2022).

---

## DD2 - THE STORE IS THE PACKAGE MANAGER

Install = put a closure. Launch = rematerialize a hash. Uninstall = unpin plus GC. Upgrade = new closure plus
root transition. Rollback = System Root restore. Adjudicated honestly against Nix, Guix, ostree/Silverblue,
ChromeOS A/B.

### DD0 nearest-neighbor table

| Work (UNVERIFIED) | What it does | Delta vs CARMIX |
|---|---|---|
| Nix (Dolstra 2006) | Hash-addressed store; install = realise derivation; uninstall = GC root drop plus collect-garbage; upgrade = new closure plus atomic generation switch; rollback = re-point generation; coexisting versions, no solver | Names inert files only. No authority in the name; rollback never touches a running process; no attestation of the transition. CARMIX adds the per-component ceiling and process-state rollback |
| GNU Guix (Courtes 2015) | Nix model plus bootstrappable/reproducible source-to-binary builds; guix system generations roll back whole OS config | Leads CARMIX on reproducible BUILDS (which CARMIX does not deliver). Still file/config generations only; running processes are not in a generation; no capability ceiling in package identity |
| OSTree / Fedora Silverblue (Walters) | Git-like content-addressed immutable OS trees; atomic deploy; GPG-signed commits; rollback to prior deployment on reboot | Signs a file-tree commit, not a live-authority manifest; rollback reboots into a prior tree and loses all running state. Pre-covers the attestable-transition delta except the authority-ceiling binding |
| ChromeOS / Android A/B update_engine | Two partitions; atomic update to inactive slot; dm-verity verified boot; rollback = boot the other slot | Coarsest granularity (whole-partition image), no cross-version dedup, no per-component authority, reboot discards running state. Verified boot pre-covers part of the attestation delta |
| CRIU checkpoint/restore (Emelyanov 2012) | Checkpoints a live process (fds, sockets, memory) to files, restores it later | Blob is opaque, no content substructure to diff or dedup, and not tied to package identity. CARMIX names the process by page-table-inclusive hash sharing subtrees with its own package closure under one root |
| Docker/OCI image plus registry | Content-addressed layers; pull fetches only missing layers; run launches from an image digest | Addresses build artifacts (tar layers), not running container state; tags are mutable; no capability ceiling. Same file-plane coverage as Nix, weaker dedup granularity |
| Nix signed substituters / narinfo | Ed25519-signs store-path content so a client trusts a binary cache | Signs content INTEGRITY of an inert path, not a whole-system authority manifest or a transition. Closest prior art to the integrity half; does not bind per-component authority |
| seL4 / capability OS lineage | Machine-checked capability kernel; authority as unforgeable caps, no ambient root | Has the authority model CARMIX reuses but no content-addressed package/deployment layer. The delta is fusing this ceiling INTO the store name; the ocap idea itself is not the contribution |

### Thesis-native mechanism

The System Root: one BLAKE3 Merkle-DAG root folding {each live task's page-table-INCLUSIVE task-hash, each MVCC
capability-slot version, the content-store manifest, the FS root} at a quiescent epoch boundary (ROADMAP Cycle
6; kernel/SR_LOG.md SR3). The package layer rides two committed primitives: (a) the content store plus BLAKE3
dedup plus durable-refcount GC plus diff-proportional migration transfer (Cycle 2, PROVEN single-CPU/QEMU), and
(b) the machine-checked anti-amplification swcap ceiling folded into the hashed manifest, minted through the
FROZEN gate trio. Named operations: install = rematerialize a closure by transferring only blocks the
destination lacks, re-verified on read; launch = spawn-from-content-addressed-image; uninstall = drop a GC root
plus refcount reclaim; upgrade = publish new content plus single durable re-root of the name-to-hash directory
(old root still resolves the whole old system); rollback = re-point the durable root at a retained System Root
AND rematerialize the running processes it names (sr_demat/sr_remat: regs plus stack plus heap plus task-hash
round-trip, BLAKE3-verified, with an SR4 revocation floor).

### Findings

What Nix genuinely covers (concede fully; Cycle 2 itself says "This is Nix"). install = realise a derivation
into the hash-addressed store; uninstall = drop GC root plus nix-collect-garbage, identical to CARMIX unpin
plus GC; upgrade = build new closure plus atomic profile-generation switch; rollback OF FILES = re-point to a
prior generation sharing unchanged store paths; diamond dependencies = two versions are two coexisting store
paths, no SAT/PubGrub solver, no /usr namespace conflict; crash-safety = the profile symlink swap is atomic and
ordered after content lands. CARMIX reproduces every one of these and adds nothing on this plane. Guix
additionally leads on the part CARMIX does NOT deliver: bootstrappable reproducible source-to-binary BUILDS;
CARMIX gives reproducible DISTRIBUTION, not reproducible builds. ostree/Silverblue covers atomic
whole-OS-tree deploy plus prior-deployment rollback plus GPG-signed commits, so "attestable transition" is
PARTIALLY pre-covered. ChromeOS/Android A/B covers atomic slot-swap update plus verified boot (dm-verity) plus
rollback to the other slot.

What Nix (and Guix, ostree, A/B) structurally cannot do. A Nix derivation names an INERT build output. Its name
covers files on disk, never live task state. Therefore (1) it has no authority notion (a store path cannot
carry a per-component capability ceiling), and (2) its rollback re-points a profile symlink and an
ALREADY-RUNNING process keeps its old mapping until manually restarted, so the running process is outside the
name. This is the membrane (kernel/ACADEMIC_SYNTHESIS.md 3.6): Nix's name is on the frozen file half; CARMIX's
System Root name reaches the frozen-but-live process half (page-table-inclusive task hash).

The three candidate deltas, weighed. (D1) Package identity carries an authority ceiling. REAL but NARROW. It is
"capability system meets content-addressed store", and per the synthesis's own rule "content-addressed X is not
automatically novel", folding authority into the hash reduces to "a commitment covers its bytes." Weight comes
only from the ceiling being machine-checked (anti_amplification) and per-component. (D2) Update = attestable
root transition. PARTIALLY COVERED by ostree signed commits plus A/B verified boot; the only residue is that
the attested value binds each component's authority ceiling (flip a ceiling, task-hash changes, root changes,
Ed25519 verify fails), and even that root of trust is EMULATED (software AUTH keypair, not a TPM). (D3)
Rollback restores RUNNING PROCESSES not only files. This is the one structural delta no neighbor reaches.
Demonstrated as a primitive in SR_LOG SR3 (two processes A/B, task hash round-trips into fresh frames, both
branches retained and rematerializable). No package/deployment system in this domain names live task state, so
none can roll it back. Combined with CRIU the gap persists: CRIU checkpoints a running process to an OPAQUE
blob with no content substructure to diff/dedup and no binding to package identity, whereas CARMIX's process
shares Merkle subtrees with the package closure under one root.

Exact delta sentence. The same content-addressed store that holds the package closure also holds each running
process as a page-table-inclusive task hash under one signed System Root, so an upgrade is a root transition
whose attested value binds each component's anti-amp ceiling, and a rollback rematerializes running processes
(regs plus stack plus heap), the operation Nix/Guix/ostree/ChromeOS cannot perform because their name covers
inert build outputs, never live task state.

### Verdict: NOVEL-AS-FUSION

Honest boundary. The package-manager thesis per se (install/launch/uninstall/upgrade/rollback-of-files,
diamond-deps-without-a-solver, atomic crash-safe switch, dedup, GC) is COVERED by Nix; claiming novelty there
is generic cloning and is refused. Reproducible source-to-binary BUILDS is Guix's lead and is NOT delivered
here. D1 (ceiling-in-the-name) is real but narrow, its only weight is the machine-checked per-component
ceiling. D2 (attestable transition) is PARTIALLY pre-covered by ostree signed commits plus A/B verified boot,
and CARMIX's root of trust is EMULATED (software Ed25519, no TPM), so it demonstrates the protocol, not the
security (ACADEMIC_SYNTHESIS 3.4). D3 (process-state rollback) is the genuine structural delta Nix cannot
reach, but it is IN-PRINCIPLE: the System Root object, retention policy, and rematerialize-at-root entry point
are unwritten (ROADMAP Cycle 6), and SR_LOG only demonstrates the primitive on two processes at
single-CPU/QEMU. It further obeys three ceilings: it requires a quiescent epoch boundary (on SMP this is the
DRCC consistent-cut problem, UNVALIDATED); it obeys output-commit (cannot un-send bytes already on the NIC);
and it is explicitly for the immutable code/authority plane, since the mutable DATA plane (user files/config/
logs) does NOT want per-write re-rooting, so "rollback the whole computer" honestly means "in-system
content-addressed state up to the last externalized output." Finally, "which hash should I upgrade to"
re-concentrates supply-chain trust in a mutable name-to-hash index (OPEN), exactly as it does for Nix channels.

### Build spec

Do NOT rebuild the package manager. Cycle 2 concedes the store/dedup/GC/coexisting-versions/atomic-switch layer
is Nix and is already PROVEN as reused primitives. The ONLY new code that lands the defensible delta is wiring
the live task-hash (SR_LOG sr_demat/sr_remat) into the installed-set root so a rollback covers processes, plus
signing that root. Minimal run_pm demo, four steps, all on committed primitives: (1) install = transfer only
missing blocks between two store states (migration path); (2) upgrade = re-root the name-to-hash directory,
show the old root still resolves the entire old system, crash between content-write and root-update leaves old
root intact; (3) rollback = re-point the durable root at a retained System Root AND rematerialize a still-RUNNING
process from it (reuse sr_remat: regs plus stack plus heap plus task-hash round-trip, apply the SR4 revocation
floor), this step is the whole delta, everything else is Nix; (4) sign the root at A, verify at B, flip one
component ceiling, show verify reject. Steps 1-2 are Cycle 2 (PROVEN); step 3 depends on the unwritten Cycle 6
System Root object plus retention policy plus rematerialize-at-root entry point (IN-PRINCIPLE); step 4 uses
vendored tweetnacl Ed25519. No new store, no new crypto, no proven-core edit.

### Bibliography (all UNVERIFIED)

[U] Dolstra, The Purely Functional Software Deployment Model (Nix), 2006.
[U] Courtes and Wurmus, Reproducible and User-Controlled Software Environments in HPC with Guix, 2015.
[U] Walters, OSTree/libostree (Fedora Silverblue), 2014-.
[U] Google, ChromeOS/Android update_engine and ChromiumOS verified boot.
[U] Emelyanov, CRIU checkpoint/restore, 2012.
[U] OCI Image Specification and Docker registry content-addressed layers.
[U] Nix signed substituters / narinfo Ed25519 (NixOS docs).
[U] seL4 machine-checked capability microkernel lineage (Klein et al.).

---

## DD3 - WINDOWS AND INPUT UNDER CAPABILITIES

Surfaces as content-addressed framebuffers, composition as rematerialization, and desktop isolation (keylogger
unconstructible, screenshot needs a surface-read cap) as a corollary of the anti-amplification ceiling rather
than compositor policy.

### DD0 nearest-neighbor table

| Work (UNVERIFIED) | What it does | Delta vs CARMIX |
|---|---|---|
| Wayland core security model | Central trusted compositor holds the full input stream and gives events only to the focused surface's client fd | Isolation is compositor POLICY over an ambient input stream the compositor fully holds; CARMIX derives the same effect from the anti-amp ceiling (no cap authorizes unfocused delivery, none can be minted) |
| Wayland screencopy / wlr-screencopy / xdg-desktop-portal ScreenCast | A portal or privileged protocol grants a client the right to copy the screen after a runtime consent dialog | Portal grant is a consent flag backed by compositor trust; CARMIX's surface-read is an unforgeable cap and GFX-3 already PROVES its absence blocks the read at the gate, not by a dialog |
| Qubes OS GUI virtualization (guid/qubes-gui in dom0) | Trusted dom0 GUI daemon composites AppVM window buffers over shared memory and routes input only to the focused VM, enforced by Xen | VM-granular and enforced by a trusted dom0 daemon that holds all input; CARMIX is per-surface with no dom0 and no component holding an input superset deliverable object |
| Fuchsia Scenic plus focus chain plus ViewRef | Scenic/Input Pipeline holds all input and dispatches to the view named by the current focus chain; ViewRef is an eventpair capability identifying a view | ViewRef IDENTIFIES a view but Scenic still holds all input and routes by focus-chain policy; CARMIX makes possession of the endpoint cap itself the delivery channel (move-only) |
| Genode nitpicker | A small trusted GUI multiplexer forwards input to the focused client and prevents cross-client snooping | Minimizes the trusted multiplexer that HOLDS ambient input; CARMIX bounds the router's held authority to a single move-only endpoint cap rather than an all-deliverable superset, and the property lives in the ceiling proof |
| X11 core (baseline anti-pattern) | Ambient input and full screen scraping (XGrabKeyboard, XGetImage) available to any client by default | The exact ambient-authority world the CARMIX ceiling forbids by construction; the negative baseline the fusion is measured against |
| seL4 | Proves capability enforcement and confinement at the kernel boundary; leaves the windowing/input model to userland | CARMIX SPECIFIES the desktop input-focus mechanism (endpoint cap = focus) and DERIVES isolation from its anti-amp ceiling; seL4 supplies the proof discipline but not the compositor form |
| EROS/KeyKOS capability GUI lineage | Capability-structured OS services including framebuffer access as capabilities | Input focus as a MOVE-ONLY input-endpoint capability tied to a machine-checked anti-amp lemma, plus screenshot as a distinct surface-read cap, is not the documented mechanism in that lineage |

### Thesis-native mechanism

The input-endpoint capability. Input is delivered only by writing a captured scancode into a per-surface event
queue through that surface's INPUT-ENDPOINT cap, a cvsasx_swcap_t minted over the queue bytes via the FROZEN
gate (cvsasx_sw_cap_remint at swcap.c:56, cvsasx_swcap_check at swcap.c:98), the same NET-1 endpoint-as-bounded-cap
pattern reused for pixels' downstream ("no cap = no reachability = REFUSED"). Focus IS possession of that cap;
focus transfer is a MOVE (re-mint to exactly one holder, drop the prior), so at most one surface holds it. Two
ceiling facts do the security work: (1) an app cannot mint itself an endpoint cap over the INPUT device stream
(anti-amp, no amplification to a grant it was never handed), so it cannot self-subscribe to keys; (2) a
surface's own draw cap cannot be widened into a surface-READ cap for another surface (PROVEN as GFX-3:
out-of-authority surface read REFUSED, kernel/GFX_LOG.md:63-64, and a surface cap cannot reach FB->address). So
a keylogger is not forbidden by policy, it is unnameable by the APPLICATION: there is no cap an app can hold or
mint that authorizes delivery to an unfocused surface. Screenshot/screen-share is a distinct explicit
surface-read cap, not a subset of any surface's own authority. Surfaces/frames/events are already
content-addressed (GFX-1), present is display-cap-gated (GFX-2), rematerialization re-presents a past frame by
hash (GFX-4).

### Findings

Delta sentence (reconciled with the router admission below). In Wayland, Qubes dom0 guid, Fuchsia Scenic, and
Genode nitpicker a single trusted mediator HOLDS the complete deliverable-input stream and FILTERS it by a
focus policy ("send only to the focused client"). CARMIX also has a small focus router that holds the raw
device-stream authority, but it holds no deliverable-to-a-surface object: the only channel by which an event
reaches a surface is that surface's move-only input-endpoint capability, focus IS possession of that token, and
the anti-amplification ceiling (cvsasx_sw_cap_remint, attenuate-only, no top element) makes both
self-subscription-to-keys and cross-surface reads unconstructible BY THE APPLICATION rather than refused by a
mediator's filter. The fusion is: desktop input/screen isolation derived as a corollary of the proven anti-amp
lemma plus the reused NET-1 endpoint-cap pattern, not as compositor filter code. (The earlier framing "CARMIX
has no object that represents all deliverable input at all" overclaimed; the router does hold the raw device
stream. The honest claim is that no object represents deliverable-to-a-surface input, and unconstructibility is
against the application, not against the router.)

Standing vs the spine (honest split). The SCREENSHOT half is already BUILT and proven: GFX-3 shows a surface
cap cannot read a foreign surface and cannot reach the framebuffer, so "reading another surface requires an
explicit surface-read cap" is a landed property, not a proposal. The KEYLOGGER half is DESIGN, NOT BUILT: the
live path today (poll_input at kernel.c:430, raise_window at kernel.c:403) is a conventional ambient compositor,
port 0x60 is read and echoed (kbd_set1[d] to sputc/kputc) with focus as a mere z-order pointer swap, and there
is no input-endpoint cap and no cap check on key delivery. The DRV-2 INPUT driver reads scancodes through the
device cap, but delivery to windows is ungated. So this front lands as half-proven-by-own-spine,
half-specified-here.

Residual TCB (do not overclaim). A focus ROUTER still exists and holds the raw device-stream authority plus, at
any instant, exactly one surface's endpoint cap. The delta is not "zero TCB" (neighbors also have a small
mediator). The delta is WHERE the property lives and WHAT the mediator holds: neighbors' mediator holds a
deliverable-input SUPERSET and a filter; CARMIX's router holds the raw device stream plus a single move-only
token it re-mints to exactly one holder, and no deliverable-to-a-surface superset object exists. The app-side
unconstructibility is the ceiling corollary; the router's exactly-one discipline is the honest remaining trust,
reasoned about and revocable at the SR revocation floor.

### Verdict: NOVEL-AS-FUSION

Honest boundary. Three edges. (1) NOT BUILT: input-delivery-as-capability is unbuilt; the committed live
desktop (run_shell) reads and echoes scancodes ambiently and focus is a z-order swap (kernel.c:403,430-436).
Only the surface draw/read plus display-cap plus rematerialization halves are committed (GFX-1..5). (2)
Liveness is off the content-addressing membrane (settled boundary 1, not relitigated): a hash names bytes that
exist, so it cannot name "the next keypress." A captured scancode window CAN be a stored object, liveness
cannot. The endpoint cap gates DELIVERY of an already-captured event, it does not make input liveness a hashed
object. (3) The unconstructibility is against the APPLICATION, not against the focus router (a small TCB holding
the device stream plus one live endpoint cap). Headless: no physical monitor is observed, the framebuffer
readback (GFX-2) is the honest substitute, and events here are synthesized, not captured from a real keypress.

### Build spec

Minimal compositor spec for DE4 (reuse ONLY the spine, touch no FROZEN file). Additions are small, most is
already committed.

Reuse as-is: content-addressed surfaces plus surface caps (gfx_touch/gfx_peek via drv_access); the display
device cap plus gfx_present (GFX-2); the event log gfx_evlog / gfx_evt_append (kernel.c:7861-7867); the INPUT
device cap from DRV-2; the gate remint/check (cvsasx_sw_cap_remint, cvsasx_swcap_check), never edited; the NET-1
endpoint-cap pattern as the template.

Add four things:
1. INPUT-ENDPOINT CAP per surface: give each surface a small fixed event queue; mint an endpoint cap over the
   queue bytes via drv_mint(base=&queue, len=bytes, grant=bytes). This is a second cap distinct from the
   surface's draw cap.
2. FOCUS = POSSESSION, transfer = MOVE: a single focus_router holds the INPUT device cap and, at any instant,
   exactly ONE surface's endpoint cap. focus_set(S) re-mints S's endpoint cap into the router and DROPS the
   previous holder's (move, not copy). Invariant to assert: count(live endpoint caps held by router) == 1.
3. DELIVERY THROUGH THE CAP: on a captured scancode, the router does drv_access(&held_endpoint_cap,
   &S.queue[...], len, STORE) then appends via gfx_evt_append. Writing into any surface other than the current
   focus holder is out of the held cap's region and REFUSED by the gate. No other code path writes queues.
4. SURFACE-READ CAP for screenshot: a screenshot/share consumer must present an explicit surface-read cap (a
   gfx_peek-class cap over the target surface's pixel bytes). By GFX-3 this is NOT derivable from the
   requester's own surface cap.

Disproofs DE4 must show (mirror the GFX-x seam style, rdtsc-measured, real framebuffer readback):
- KL-1 unfocused surface receives NO key: hold surface B focus, deliver 'a'; assert B.queue got it and A.queue
  delivery via A's un-held endpoint cap is REFUSED at the gate. Keylogger-unconstructible = the refusal.
- KL-2 app cannot self-subscribe: an app holding only its surface draw cap attempts to drv_access the INPUT
  device stream and is REFUSED (anti-amp, no amplification).
- KL-3 focus move is a move: after focus_set(A), B's endpoint cap is no longer live in the router (assert the
  exactly-one invariant); B stops receiving without any per-key policy check.
- KL-4 screenshot needs the cap: reading surface B's pixels with B's-own or A's draw cap is REFUSED (reuse
  GFX-3); only an explicit surface-read cap succeeds.
Keep it single-CPU, headless, framebuffer-readback-verified, numbers rdtsc-measured this boot. Skipped:
alpha/z-blend, multi-device input fan-out, revocation UX; add when a second input device or an untrusted router
is in scope.

### Bibliography (all UNVERIFIED, recall not fetch, added per critic D6)

[U] Wayland protocol core security model (freedesktop.org Wayland docs).
[U] wlr-screencopy protocol and xdg-desktop-portal ScreenCast (wlroots / freedesktop portals docs).
[U] Qubes OS GUI virtualization, qubes-gui / guid in dom0 (Qubes OS architecture docs).
[U] Fuchsia Scenic, focus chain, and ViewRef eventpair capabilities (Google Fuchsia docs).
[U] Genode nitpicker GUI multiplexer (Genode Foundations, Feske).
[U] X11 core protocol XGrabKeyboard / XGetImage (X.Org docs, cited as negative baseline).
[U] seL4 machine-checked capability microkernel (Klein et al.).
[U] EROS/KeyKOS capability system GUI lineage (Shapiro et al.).

---

## DD4 - USERS ARE DOMAINS

An account = an authority domain plus a ceiling bundle. Login rematerializes the user root slice. Cross-user
isolation IS the standing cross-domain refusal. Per-user dedup domains honor the Armknecht boundary. Fast user
switching = a root-slice switch.

### DD0 nearest-neighbor table

| Work (UNVERIFIED) | What it does | Delta vs CARMIX |
|---|---|---|
| Unix multi-user (uid/gid, DAC, setuid, capabilities(7)) | Authenticates an identity then mediates by consulting ambient uid/gid tables on a shared global namespace; uid 0 bypasses all checks | No ambient table and no top element to bypass; isolation is the anti-amp gate refusal (dest<=source), and the account additionally IS the dedup partition and the rematerializable SR slice, three roles Unix keeps separate or lacks |
| Qubes OS (compartmentalization) | Isolates users/activities as Xen VMs under a trusted dom0; the close neighbor for user-isolation-as-domain-isolation | Isolates by hypervisor VM boundary with a trusted ambient dom0 (a top element); CARMIX isolates by a no-top order and unifies the domain with a proven content-dedup partition (Qubes has none keyed to the qube) and a rematerialize-by-hash login slice (Qubes save/restore is opaque per-VM images) |
| Plan 9 per-process namespaces plus factotum | Each process/user gets a private mutable namespace assembled by bind/mount over 9P; close neighbor for the private-view story | Isolation is namespace non-visibility over a mutable name-to-resource map with 9P auth; CARMIX isolation is a capability-order refusal, the namespace is content-addressed (name-to-hash), and login rematerializes the slice by hash |
| KeyKOS / EROS persistent capability system | No-ambient-authority persistent ocap OS; whole system transparently checkpointed and restored | Checkpoints the whole system opaquely and does not content-dedup per user nor treat login as slice-rematerialization; CARMIX makes the account index the dedup partition and rematerializes only that account's SR slice, per-block BLAKE3-verified |
| seL4 plus CAmkES / core platform | Machine-checked capability separation with static verified isolation between components | partitions are static with no content store, no dedup partition, no rematerialize-by-hash login/switch; CARMIX's account is a dynamic login-acquired domain fused with dedup and a time-travelable SR slice (at the cost that CARMIX's gate is model-proven/C-tested, not seL4's C-level proof) |
| Genode OS Framework | Strictly hierarchical capability delegation with session-based resource routing (close to U-4 delegation) | no content-addressed store, so no dedup-domain and no rematerialize-by-hash slice to fuse; CARMIX adds the Armknecht dedup partition and the SR login slice to the same account index |
| Fuchsia / Zircon capability-routed components | No ambient authority, no global namespace; every capability explicitly routed | no content-addressing/dedup and no whole-state-by-hash; CARMIX unifies the routed-authority domain with a content-dedup partition and a rematerializable per-account state slice |
| Harnik, Pinkas, Shulman-Peleg 2010 (cross-user dedup side channels) | Shows cross-user dedup leaks file existence via store hit/miss timing; recommends disabling cross-user dedup | SETTLED (D7). CARMIX's per-user dedup domain IS this recommended partition; the contribution is the measured cycle counts (DS3), not the boundary. This neighbor COVERS the dedup half |

### Thesis-native mechanism

An account is ONE domain index d that simultaneously indexes three already-built partitions: (1) AUTHORITY, d's
root swcap range in the anti-amp gate (cvsasx_sw_cap_remint, dest<=source, no top element); cross-user
isolation is literally the gate refusal "A requesting a cap into B's range: dest not <= source, refused"
(ROADMAP Cycle 1 PROVEN; kernel.c:8690 "DISJOINT authority slices"). (2) DEDUP, d is the gc_rc domain tag
(gc_rc_t{hash,domain,count}, kernel.c:5449; DS1-DS6), so the cross-user store-hit/miss timing channel is closed
by the same index. (3) STATE, d's System-Root slice, rematerialized by hash at login and re-pointed on switch
(SR3 whole-system time-travel WITH processes: sr_demat/sr_remat plus epoch_tree fold; kernel/SR_LOG.md). Login
= Ed25519 challenge-response over the trust store then acquire d's root swcap plus sr_remat(d's slice). Fast
switch = SR root re-point to d''s slice (SR3 branch/restore). Groups/roles = U-4 delegation subtrees;
revocation = chain-walk intersected with the SR4 revocation floor. Nothing new is invented; the front is these
primitives re-aimed so the account IS the shared index.

### Findings

The front decomposes into three claims with sharply different standing, so a single label would be dishonest.

Claim A: "isolation as the standing cross-domain refusal, not a uid/gid add-on." This is the object-capability
no-ambient-authority principle. Taken verbatim it is COVERED: KeyKOS/EROS, seL4, Genode, Fuchsia/Zircon, and
Qubes all already realize "user/tenant isolation = domain isolation enforced by construction, not by consulting
an ambient uid/gid table a superuser bypasses." ROADMAP Cycle 1 concedes it: "the algebraic no-top order is a
restatement of the ocap no-ambient-authority principle already realized in EROS and seL4, so it is a thin
standalone novelty." The CARMIX-specific increment is only that the no-top order is machine-checked
(proofs/Carmix.v anti-amp lemma) and the identity is a content-addressed image hash, a labeling improvement
over the neighbors, not a new isolation mechanism.

Claim B: "per-user dedup domains honor the Armknecht boundary." SETTLED prior art, do not relitigate (D7). This
is precisely the Harnik/Pinkas/Shulman-Peleg 2010 cross-user dedup side channel and its recommended
countermeasure (disable cross-domain dedup at N-fold storage cost). ACADEMIC_SYNTHESIS states plainly "the
contribution is measuring and integrating it, not discovering it." So the account-as-dedup-domain half is
COVERED by the dedup-channel literature; CARMIX contributes the measured cycle counts (DS3: cross-domain probe
moves from a 42,873-cyc hit to a 1,285,349-cyc miss), not the boundary.

Claim C: "login rematerializes the user root slice; fast switching = a root-slice switch; the account is a
SINGLE index unifying authority plus dedup plus rematerializable state." This is the uncovered residue and the
only defensible fusion. No neighbor unifies a content-dedup partition with the authority domain and a
rematerialize-by-hash state slice under one account index. Qubes isolates authority (Xen VMs) but has no proven
content-store dedup partition keyed to the qube and no rematerialize-by-hash login (it saves/restores opaque VM
images, and dom0 is a trusted ambient root, a top element CARMIX's order lacks). seL4/Genode/Fuchsia have no
content-addressed store or dedup at all, so there is no dedup partition to fuse and no whole-system-state-by-
hash. EROS/KeyKOS persist the whole system by transparent checkpoint but do not content-dedup per user nor
treat login as slice-rematerialization. Plan 9 gives each user a private mutable name-to-resource namespace
(the close neighbor for "private view") but isolation there is namespace non-visibility over 9P plus factotum
authority, not a capability-order refusal, and the namespace is not content-addressed nor rematerialized by
hash. The fusion, one Ed25519-keyed index d that is at once (swcap ceiling, Armknecht dedup tag, SR slice),
with login = acquire-swcap plus sr_remat(d) and fast-switch = SR root re-point, is CARMIX-native and present in
no neighbor.

Net: the front's headline candidate delta ("isolation as cross-domain refusal wearing a login screen") is
COVERED by ocap prior art and the dedup half is settled; the surviving thesis-native contribution is the
narrower Claim C fusion. Hence PARTIALLY-COVERED, not NOVEL-AS-FUSION (headline is covered) and not
KILL-AS-GENERIC (a thesis-native form exists in Claim C).

### Verdict: PARTIALLY-COVERED

Honest boundary. Do not claim "isolation = refusal, not uid/gid" as a CARMIX result: it is the ocap principle
(EROS/seL4/Genode/Fuchsia/Qubes) and ROADMAP Cycle 1 already grades it a thin standalone novelty. Do not claim
the dedup-domain as a security discovery: it is the Harnik et al. 2010 mitigation, contribution = measurement
only (SETTLED, D7). The one novel piece (Claim C triple-index fusion) inherits three labels: (i) the gate half
is model-proven / C-tested, not machine-checked C; (ii) the SR slice half is single-CPU, QUIESCENT-epoch
capture only, concurrent/live per-user switching is SMP-blocked (SR_LOG scope); (iii) login-integrity
attestation is EMULATED (software AUTH keypair, no hardware root, no clock), so a forged-login refusal
demonstrates the protocol against no real adversary (Membrane boundary 4). "No ambient root" holds for the
authority ALGEBRA only: the first identity key is baked out-of-band and the machine owner controlling the raw
store with no anti-rollback state is a de-facto trusted base. Cross-user dedup closure is the CROSS-domain
timing channel only; the within-domain residual and a concurrent cross-core attacker remain out of scope.
Fusion novelty must be stated as "unification of existing partitions under one index," never as new isolation,
new dedup, or new attestation.

### Build spec

No new subsystem; the fusion is an index unification over built primitives. (1) Define account d as one uint32
used identically as the swcap root-range selector, the gc_rc.domain tag (kernel.c:5449), and the SR slice id.
(2) Login: Ed25519 challenge-response against the trust store (SR5 crypto_sign_open path), on success mint d's
root swcap via cvsasx_sw_cap_remint and sr_remat(d's SR slice). (3) Fast switch: SR3 root re-point to d''s
pinned slice (sr_demat current, sr_remat target), authority re-minted through the SR4 revocation floor. (4)
Cross-user isolation: already free, a process under d requesting into d''s range fails the gate (dest not <=
source). (5) Groups/roles: U-4 delegation subtrees off d's root. Verify (reuse existing harnesses): two
identities log in to disjoint roots; A-to-B cap request refused with migration status code; DS1-style identical
content in d and d' gives two physical objects with independent counts; switch d to d' to d round-trips d's SR
slice to its pinned root; a forged Ed25519 login fails crypto_sign_open. Every check already has a precedent
stage (ROADMAP Cycle 1 PROVEN, DS1-DS6, SR3-SR5), so the build is wiring one index through three, not new
mechanism.

### Bibliography (all UNVERIFIED, recall not fetch)

[U] Unix multi-user model, uid/gid, DAC, setuid, capabilities(7) (POSIX / Linux man pages).
[U] Qubes OS, security by compartmentalization (Qubes OS architecture docs, Rutkowska/Wojtczuk).
[U] Plan 9 per-process namespaces plus factotum (Pike et al., Bell Labs).
[U] KeyKOS / EROS persistent capability system (Hardy; Shapiro et al.).
[U] seL4 plus CAmkES / seL4 core platform (Klein et al.).
[U] Genode OS Framework, hierarchical capability delegation (Feske).
[U] Fuchsia / Zircon capability-routed components (Google Fuchsia docs).
[U] Harnik, Pinkas, Shulman-Peleg, Side Channels in Cloud Services: Deduplication in Cloud Storage, 2010.

---

## DD5 - HONEST PARITY TABLE

| Mint capability | CARMIX-native | Status |
|---|---|---|
| Desktop shell (windows, compositor, input, screenshot) | Content-addressed surfaces plus display-capability present plus frame rematerialization-by-hash (GFX-1..5); screenshot requires an explicit surface-read cap (GFX-3 PROVEN); input-delivery as a per-surface move-only input-endpoint cap so a keylogger is unnameable by the application (DD3 spec) | BUILT (surfaces + display-cap + remat + screenshot-needs-a-cap, GFX-1..5/GFX-3) / DESIGN (keylogger-unconstructible half: live path poll_input/raise_window is still an ambient compositor with focus as a z-order swap and no cap check on key delivery). Headless: framebuffer readback substitutes for a monitor |
| App ecosystem (installable applications) | App = BLAKE3 manifest hash; identity plus import authority plus provenance in one store; each import resolved as a swcap re-mint under the proven anti-amp ceiling, so no FS cap means the app cannot express an FS call. Interim ABI = content-addressed checked-GIR module plus declared re-minted imports through the FROZEN gate trio | DESIGN/SPEC. The interim checked-IR ABI is consumable only by hand-authored GIR modules; nothing a toolchain emits runs. WASM frontend is GATED: gate/wasm_frontend.c has no import-section (id 2) parser and no control-flow/call lowering (frozen backend already carries CALL_INDIRECT/BR). The glibc/native application world is OUT-OF-SCOPE |
| Package management (install/remove/upgrade/rollback, deps) | The store IS the package manager: install = transfer only missing blocks plus re-verify; uninstall = drop GC root plus refcount reclaim; coexisting versions = coexisting store paths (no solver). Plus the delta: rollback rematerializes running processes (page-table-inclusive task hash under the System Root), not only files | BUILT (generic file plane: store/dedup/GC/coexisting-versions/atomic-switch, PROVEN Cycle 2 single-CPU/QEMU, and this plane IS Nix, novelty refused) / IN-PRINCIPLE (the process-state-rollback delta: System Root object + retention + remat-at-root unwritten; SR primitive shown on 2 processes). Reproducible source-to-binary BUILDS (Guix's lead) NOT delivered, reproducible distribution only |
| Updates (system update + rollback + integrity) | Upgrade = single durable re-root of the name-to-hash directory (old root still resolves the whole old system); the attested root value binds each component's anti-amp ceiling, so flipping a ceiling changes the task hash, changes the root, fails Ed25519 verify | THIS-PROGRAM (protocol) / EMULATED (security). Attestable transition is PARTIALLY pre-covered by ostree signed commits plus A/B verified boot; the only residue is the per-component authority-ceiling binding. Root of trust is EMULATED (software tweetnacl Ed25519, no TPM, no trustworthy clock), demonstrates the protocol not the security (Membrane Boundary 4, hardware-blocked) |
| Accounts / users (login, isolation, fast switching) | Account = one index d used identically as (swcap root-range selector, gc_rc.domain dedup tag, System-Root slice id). Login = Ed25519 challenge-response then acquire d's root cap plus sr_remat(d's slice); fast switch = SR root re-point; cross-user isolation = the standing gate refusal (dest not <= source) | PARTIALLY-COVERED. Headline "isolation = refusal not uid/gid" is the ocap principle (EROS/seL4/Genode/Fuchsia/Qubes), a thin standalone novelty, not a CARMIX result. Dedup-domain is the settled Harnik 2010 mitigation (contribution = measured cycles only). Surviving delta = the triple-index fusion; SR-slice half is single-CPU/QUIESCENT-epoch only (SMP-blocked); login attestation EMULATED |
| Hardware support (GPU, disk, network, power, peripherals) | Capability-bounded drivers on virtual hardware: framebuffer plus PS/2 drivers behind device caps (DRV-1/2); emulated-VT-d IOMMU DMA windows (IN-PRINCIPLE); ACPI/MADT parse | OUT-OF-SCOPE. Physical GPUs, real NVMe, real NICs, real energy (RAPL/ACPI joules) are not modeled by QEMU; every demo is single-CPU/QEMU and any joule figure is a stated proxy. VT-d is EMULATED. Real-hardware bring-up, DRTM/TPM measured boot, and a trustworthy monotonic clock are hardware-blocked, not deferred features |
| Web browser | None. A browser requires sockets, poll_oneoff, and real-time clocks, all deliberately amputated from the WASI subset | OUT-OF-SCOPE. Reason is structural, not an implementation gap: sock_*/poll_oneoff/monotonic real-time clocks sit on Membrane Boundary 1 (real-time/liveness), which content-addressing cannot name (a hash names bytes that exist, never "the next packet"). Also depends on the absent glibc/browser-engine world |
| Office suite / productivity apps | None native. Same absent app plane as the app ecosystem row | OUT-OF-SCOPE. No glibc, no GUI toolkit, no document-format app corpus. An office app could in principle ship as a checked-GIR module over the DD1 ABI (deterministic, file-cap-scoped), but no such application exists and building one is not in scope. CARMIX delivers the ABI a document app would run on, not the document app |

### Honest summary sentence

CARMIX re-derives the desktop PILLARS thesis-natively on virtual hardware, and it must be read as a mix of
delivered and designed: it BUILDS content-addressed surfaces plus display-cap present plus screenshot-needs-a-cap
(GFX-1..5) and the file-plane package operations (PROVEN, and that plane is Nix), while the differentiating
parts (apps as capability-resolved hashes, rollback that reaches running processes, updates as attestable root
transitions, the account triple-index, and keylogger-unconstructible input) are DESIGN or IN-PRINCIPLE with
their attestation EMULATED, and it is not, and does not claim to be, a Linux Mint replacement, because it has
no physical-GPU/glibc/web-browser world, its network-facing and real-time surfaces are amputated at the
liveness boundary, and its root of trust is emulated until real hardware exists.

### Kill-list

- KILL-AS-GENERIC (DD1): "WASM is the app ABI" as a novelty. The gate header already concedes Wasm-as-capability-
  layer is mainstream and gate/ is an engineering module. Only the AUTHORITY leg (import = anti-amp re-mint)
  fused into the store is defensible; identity-as-hash and provenance-in-one-store are Nix (D3 correction).
- KILL / REFUSE (DD1): sock_*, poll_oneoff, and monotonic real-time-clock WASI functions. No thesis-native
  form, they sit on Membrane Boundary 1 (real-time/liveness). Report the amputation, do not build the surface.
- KILL-AS-GENERIC / COVERED (DD2): the package-manager thesis per se (install/launch/uninstall/upgrade/rollback-
  of-files, diamond deps without a solver, atomic crash-safe switch, dedup, GC). This is Nix, Cycle 2 concedes
  it. Reproducible source-to-binary builds is Guix's lead and is NOT delivered, state it as a gap, do not fake it.
- KILL-AS-GENERIC (DD4): "isolation as the standing cross-domain refusal, not a uid/gid add-on" as a CARMIX
  result. It is the ocap no-ambient-authority principle (EROS/seL4/Genode/Fuchsia/Qubes), graded a thin
  standalone novelty. Claim only the machine-checked no-top labeling, not new isolation.
- SETTLED / do not relitigate (DD4, D7): per-user dedup domains as a security discovery. This is the
  Harnik/Pinkas/Shulman-Peleg 2010 side channel plus its recommended partition; the contribution is the
  measured cycle counts (DS3), not the boundary.
- REFUSE overclaim (DD3): "zero TCB" input isolation. A focus router still holds the raw device stream plus
  exactly one live endpoint cap. The delta is what the mediator holds (a single move-only token, not a
  deliverable-input superset), not the absence of a mediator.
- REFUSE overclaim (DD3): input liveness as a content-addressed object. A hash names a captured scancode window
  that exists, it cannot name "the next keypress." The endpoint cap gates delivery of an already-captured event
  only.
- REFUSE overclaim (cross-cutting, Boundary 4): any security (not protocol) claim resting on the attested root,
  login, or upgrade signature. The root of trust is an emulated software Ed25519 keypair with no TPM and no
  trustworthy clock, it demonstrates the protocol against no real adversary until a hardware root exists.

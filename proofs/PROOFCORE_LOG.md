# PROOFCORE_LOG - P1 (shared-DAG acyclicity) + P2 (DRCC) machine-checked

Two new machine-checked Coq files, both closed by Qed under named assumptions
only. proofs/Carmix.v is byte-identical (untouched); new theorems live in new
files, as required.

- P1: proofs/CarmixDag.v - operational acyclicity of a GENUINELY SHARED DAG
  (one child hash under multiple parents, which Carmix.v's inductive tree cannot
  represent).
- P2: proofs/CarmixDrcc.v - the minimal operational DRCC model, theorem pinned
  literally to the CV8 statement R(sigma) = CF(exec(P, sigma, M0)) at cut B.

## Prover version

```
$ coqc --version
The Coq Proof Assistant, version 8.20.1
compiled with OCaml 5.4.0
```

## coqc commands and exact output (this run)

```
$ rm -f proofs/*.vo proofs/*.vok proofs/*.vos proofs/*.glob proofs/.*.aux
$ coqc proofs/CarmixDag.v
DAG_EXIT=0
$ coqc proofs/CarmixDrcc.v
DRCC_EXIT=0
```

Both compile with no error and no warning; exit code 0 means every proof in each
file closed with Qed (coqc rejects a file that leaves a goal open).

## D2 - no smuggling

```
$ grep -nE "Admitted|(^| )admit\.|Axiom" proofs/CarmixDag.v proofs/CarmixDrcc.v
$ echo $?
1                 # grep exit 1 = zero matches = clean
```

Zero hits: no Admitted, no bare `admit.`, no Axiom. The only assumptions are the
two explicit named Hypothesis lines in CarmixDag.v:

```
proofs/CarmixDag.v:72:  Hypothesis H_CF :   (* BLAKE3 collision-freedom, where actually needed *)
proofs/CarmixDag.v:80:  Hypothesis H_WF :   (* well-founded naming order, hash-before-name *)
```

CarmixDrcc.v needs no Hypothesis at all: its in-model theorem is unconditional
given the explicit DRF premise (classical determinacy of DRF programs). The
model-to-machine assumptions A1..A5 are named in the file header as the honest
gap, NOT smuggled in as fake Coq hypotheses.

Stronger check - Print Assumptions on the key results:

```
$ coqc -Q proofs '' proofs/_chk.v   # Require Import CarmixDag CarmixDrcc.
Print Assumptions ex_acyclic.          -> Closed under the global context
Print Assumptions acyclic.             -> Closed under the global context
Print Assumptions drcc.                -> Closed under the global context
Print Assumptions cv8_drf_converges.   -> Closed under the global context
```

"Closed under the global context" = the proof term depends on no axiom and no
admission whatsoever. (The named Hypotheses H_CF, H_WF are Section variables that
become explicit arguments of `acyclic` once the section closes, and are
discharged concretely by ex_CF / ex_WF in ex_acyclic; hence even the general
`acyclic` is axiom-free.)

## D1 - genuinely shared DAG (what the inductive tree cannot do)

CarmixDag.v ex_store puts ONE node (hash 3) under TWO parents (hash 1 and 2):

```
R(0) -> A(1) -> C(3)
     -> B(2) -> C(3)     ; hash 3 is a single resident node, referenced twice
```

- shared_child_two_parents : edge ex_store 1 3 /\ edge ex_store 2 3   (Qed)
- shared_child_is_one_node  : any node resolving hash 3 IS mkNode 3 []  (Qed)
  In Carmix.v's Obj := node nat (list Obj), the two references would be two
  distinct subterm copies; here they resolve to the one node. That is the
  genuine sharing an inductive tree cannot represent.
- ex_acyclic : forall u, ~ path ex_store u u   (Qed, via the general theorem)

## D3 - one function, proved there (P2) and measured there (CV8)

PROVED (proofs/CarmixDrcc.v)          |  MEASURED (kernel/CV_LOG.md, CV8)
--------------------------------------|--------------------------------------------
Definition R (M0) (sigma) : Hash      |  CV8 DRF workload (each core mutates its
  := CF (exec sigma M0).              |  OWN disjoint slot, concurrent on 2 cores),
Definition CF (M) : Hash              |  N=12 runs -> distinct canonical names=1
  := Hsh (map M canon_locs).          |  CV8  DRF set[0]=fe59feafdda3f9141125bec12c5cb43f
                                      |  N-run DRF hash set is exactly
Theorem drcc :                        |    {fe59feafdda3f9141125bec12c5cb43f}.
  forall M0 sigma sigma',            |
    Permutation sigma sigma' ->       |
    DRF sigma ->                      |
    R M0 sigma = R M0 sigma'.  (Qed)  |

Same function R(sigma) := CF(exec(P, sigma, M0)) at cut B. The theorem names the
universal: for all schedules sigma, sigma' of a DRF program reaching B, R is one
value. CV8 exhibits one instance of that universal: the N=12-run set of measured
R values has size 1 (the single name fe59feaf...). The racy control diverges
(12 names), and drcc's DRF premise correctly does not apply to it
(racy_not_drf : ~ DRF [core0->slot0; core1->slot0], Qed).

Honest gap (as R2 states): the theorem is the in-model determinacy result; A1
(x86-TSO SC-for-DRF), A2 (DRF is asserted, undecidable), A3 (quiescent cut) are
model-to-machine assumptions named, not proved, and not connected to the C.

## Byte-identity guards

- proofs/Carmix.v: `git diff --stat proofs/Carmix.v` empty (untouched).
- proven nine (carmix/swcap.c, carmix/backend_sw_gate.c, sls/sls.c,
  gate/sfi_checker.c, gate/executor.c, gate/optimize.c, store/object_store.c,
  cap/cvsasx_cap.h, sls/cvsasx_sls.h): `git status --porcelain` empty (untouched).

## Artifacts

coqc build artifacts (*.vo *.vok *.vos *.glob .*.aux) were removed before commit;
only the two .v sources and this log are committed.

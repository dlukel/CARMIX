(* ============================================================================
   CARMIX - P2 : Data-Race-free Content Convergence (DRCC), the minimal model

   Prover: Coq 8.20.1.  New file; proofs/Carmix.v is untouched.

   THE STATEMENT, PINNED LITERALLY TO CV8.
   PROOF_CORE_RESEARCH.md (R2) and kernel/CV_LOG.md (CV8) name one function:
     R(sigma) := CF(exec(P, sigma, M0) at cut B).
   CV8 measured, over the DRF workload on two real cores, that the N-run set of
   R values has size 1 (a single canonical name, fe59feaf...).  The theorem
   proved here is the object that set-size-1 is an instance of:
     for all schedules sigma, sigma' of a DRF program P reaching B,
       R(sigma) = R(sigma').
   One function R, measured there, proved here.

   THE MINIMAL OPERATIONAL MODEL (nothing beyond what the statement needs):
     - Threads / actions : an Act carries its thread id and a single write
       (own-slot discipline: core idx writes only its own slot idx&1).
     - Shared memory M : Loc -> Val.
     - Schedule sigma   : an interleaving = a list of actions; two schedules of
       one program reaching B are permutations of the same action multiset.
     - exec(P,sigma,M0) : fold the writes over M0 in schedule order.  Running the
       whole list realises "at cut B" (every pre-B action done, none after).
     - DRF condition     : NoDup (map wloc sigma) - the write-sets are disjoint
       (own-slot: with n=2 the two locations 0,1 differ), so no data race.
     - CF                : hash of the serialization that lists locations in a
       FIXED order (slot0 then slot1), independent of the interleaving.  The hash
       is any function (CV8's BLAKE3); NO injectivity is used - CF being a
       function is enough, so equal memory gives equal name.

   ASSUMPTIONS (named, same discipline as CarmixDag.v).
   The in-model theorem is UNCONDITIONAL given the explicit DRF premise: it needs
   no Coq Hypothesis (that is the honest state - the mathematics is the classical
   determinacy of DRF programs: independent writes commute, so permutations of a
   distinct-location action list yield one memory value; CF is a function of it).
   The model-to-machine assumptions that a real deployment rests on are NOT proved
   here and are NOT smuggled in as fake Coq hypotheses; they are the honest gap
   named in R2:
     A1 SC-for-DRF memory model (x86-TSO + no reorder across the sync point) -
        modelled structurally by the sequential fold; a separate deep theorem,
        not connected to the C.
     A2 DRF is ASSERTED, never decided (undecidable) - here it is the explicit
        premise DRF sigma, exactly the standing CV8 gives it.
     A3 genuine quiescent cut B - modelled by folding the whole schedule; the
        general release-cut is unbuilt.
     A4 per-thread determinism - each action writes a fixed value (no live caps).
     A5 schedule-independent canonical serialization - canon_locs is a fixed
        order; completion-order serialization would break the theorem.

   Every proof closes with Qed; no proof is left open and no bare postulate is
   introduced beyond the explicit DRF premise.
   ========================================================================== *)

From Coq Require Import List Permutation Arith Lia.
Import ListNotations.

Definition Loc := nat.
Definition Val := nat.
Definition Mem := Loc -> Val.

(* One thread action: thread id + a single write to its own slot. *)
Record Act := mkAct { tid : nat ; wloc : Loc ; wval : Val }.

Definition update (M : Mem) (l : Loc) (v : Val) : Mem :=
  fun x => if Nat.eqb x l then v else M x.

Definition apply_act (M : Mem) (a : Act) : Mem := update M (wloc a) (wval a).

(* exec runs the schedule to the cut: fold the writes over M0 in order. *)
Definition exec (sigma : list Act) (M : Mem) : Mem := fold_left apply_act sigma M.

(* DRF condition, CARMIX-shaped: disjoint write-sets (own-slot discipline). *)
Definition DRF (sigma : list Act) : Prop := NoDup (map wloc sigma).

Lemma exec_cons : forall a l M, exec (a :: l) M = exec l (apply_act M a).
Proof. reflexivity. Qed.

(* A location nobody writes keeps its initial value, regardless of order. *)
Lemma exec_notin :
  forall sigma M x, ~ In x (map wloc sigma) -> exec sigma M x = M x.
Proof.
  induction sigma as [|w l IH]; intros M x Hnin.
  - reflexivity.
  - simpl in Hnin.
    assert (x <> wloc w) as Hne by (intro Heq; apply Hnin; left; congruence).
    assert (~ In x (map wloc l)) as Hnl by (intro Hin; apply Hnin; right; exact Hin).
    rewrite exec_cons. rewrite (IH (apply_act M w) x Hnl).
    unfold apply_act, update. apply Nat.eqb_neq in Hne. rewrite Hne. reflexivity.
Qed.

(* Under DRF (distinct locations) a written location ends at its action's value,
   regardless of order - the single writer wins. *)
Lemma exec_in :
  forall sigma M a,
    NoDup (map wloc sigma) -> In a sigma -> exec sigma M (wloc a) = wval a.
Proof.
  induction sigma as [|w l IH]; intros M a Hnd Hin.
  - inversion Hin.
  - simpl in Hnd. rewrite NoDup_cons_iff in Hnd. destruct Hnd as [Hnotin Hnd'].
    simpl in Hin. rewrite exec_cons. destruct Hin as [Heq | Hin'].
    + subst w. rewrite (exec_notin l (apply_act M a) (wloc a) Hnotin).
      unfold apply_act, update. rewrite Nat.eqb_refl. reflexivity.
    + apply (IH (apply_act M w) a Hnd' Hin').
Qed.

(* CONFLUENCE: under DRF, exec is schedule-invariant as a memory value
   (pointwise).  Two schedules of one program reaching B are permutations. *)
Theorem exec_perm_pointwise :
  forall sigma sigma' M x,
    Permutation sigma sigma' -> NoDup (map wloc sigma) ->
    exec sigma M x = exec sigma' M x.
Proof.
  intros sigma sigma' M x Hperm Hnd.
  assert (NoDup (map wloc sigma')) as Hnd'.
  { eapply Permutation_NoDup; [ apply Permutation_map; exact Hperm | exact Hnd ]. }
  destruct (in_dec Nat.eq_dec x (map wloc sigma)) as [Hin | Hnin].
  - apply in_map_iff in Hin. destruct Hin as [a [Hloc Hina]]. subst x.
    rewrite (exec_in sigma M a Hnd Hina).
    assert (In a sigma') as Hina'
      by (eapply Permutation_in; [ exact Hperm | exact Hina ]).
    rewrite (exec_in sigma' M a Hnd' Hina'). reflexivity.
  - assert (~ In x (map wloc sigma')) as Hnin'.
    { intro Hc. apply Hnin.
      eapply Permutation_in;
        [ apply Permutation_sym; apply Permutation_map; exact Hperm | exact Hc ]. }
    rewrite (exec_notin sigma M x Hnin).
    rewrite (exec_notin sigma' M x Hnin'). reflexivity.
Qed.

Section Canonical.

  (* CF's hash: any function of the serialized bytes (CV8 uses BLAKE3).  A plain
     Coq function - "CF is a function" is all the theorem needs, no injectivity. *)
  Variable Hash : Type.
  Variable Hsh : list Val -> Hash.

  (* Fixed canonical order: slot0 then slot1, independent of the interleaving. *)
  Definition canon_locs : list Loc := [0; 1].

  Definition CF (M : Mem) : Hash := Hsh (map M canon_locs).

  (* R(sigma) := CF(exec(P, sigma, M0) at B) - the CV8 function, verbatim. *)
  Definition R (M0 : Mem) (sigma : list Act) : Hash := CF (exec sigma M0).

  (* ==========================================================================
     THE DRCC THEOREM, pinned to the CV8 statement:
       for all schedules sigma, sigma' of a DRF program reaching B,
         R(sigma) = R(sigma').
     CV8's N-run "canonical hash set size = 1" is one instance of this.
     ======================================================================== *)
  Theorem drcc :
    forall M0 sigma sigma',
      Permutation sigma sigma' -> DRF sigma ->
      R M0 sigma = R M0 sigma'.
  Proof.
    intros M0 sigma sigma' Hperm Hdrf. unfold R, CF, DRF in *. f_equal.
    apply map_ext_in. intros x _.
    apply exec_perm_pointwise; [ exact Hperm | exact Hdrf ].
  Qed.

End Canonical.

(* ============================================================================
   CV8 concrete instance: the two-core DRF workload, two opposite schedules,
   ONE canonical name (set size 1) - for ANY hash, so in particular BLAKE3.
   Mirrors u3_drf racy=0: core 0 writes its own slot 0, core 1 its own slot 1.
   ========================================================================== *)

Definition M0 : Mem := fun _ => 0.
Definition wA : Act := mkAct 0 0 7.   (* core 0 writes its OWN slot 0 *)
Definition wB : Act := mkAct 1 1 9.   (* core 1 writes its OWN slot 1 *)
Definition sched_AB : list Act := [wA; wB].   (* one interleaving *)
Definition sched_BA : list Act := [wB; wA].   (* the opposite interleaving *)

Example cv8_drf_converges :
  forall (Hash : Type) (Hsh : list Val -> Hash),
    R Hash Hsh M0 sched_AB = R Hash Hsh M0 sched_BA.
Proof.
  intros Hash Hsh. apply drcc.
  - apply perm_swap.
  - unfold DRF, sched_AB, wA, wB. simpl.
    constructor.
    + simpl. intros [H | H]; [ discriminate H | exact H ].
    + constructor; [ simpl; intros [] | constructor ].
Qed.

(* The racy control (both cores race the SAME slot) is NOT a permutation of
   distinct-location writes, so drcc's DRF premise does not apply - matching
   CV8's diverging racy set.  Shown here just as the non-DRF witness. *)
Example racy_not_drf :
  ~ DRF [mkAct 0 0 7; mkAct 1 0 9].
Proof.
  unfold DRF. simpl. intro Hnd.
  rewrite NoDup_cons_iff in Hnd. destruct Hnd as [Hnotin _].
  apply Hnotin. simpl. left. reflexivity.
Qed.

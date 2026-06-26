(* ============================================================================
   CARMIX - Phase-1 machine-checked anti-amplification proof  (Track C)

   Prover: Coq / Rocq 8.20.1  (see proofs/PROOF_LOG.md for the no-sudo install)

   ABSTRACT model of the software-capability re-mint performed by
   carmix/swcap.c::cvsasx_sw_cap_remint.  A capability is a half-open range
   [base,limit) plus a finite permission set plus a software validity tag;
   re_mint is the clamp + intersect + W^X/forbidden reject the C code implements
   (swcap.c lines 61, 69-78: invalid source -> reject; clamp bounds; forbidden
   mask reject; W^X reject; perms subset of source).

   STAGE C1  : re_mint never amplifies authority           (anti_amplification)
   STAGE C2  : a valid dest is W^X-clean and forbidden-free (two lemmas)
   STAGE C3  : the content-addressed object graph is acyclic (acyclic_graph)

   Part-0 discipline: every theorem is closed by Qed and accepted by coqc.
   Nothing is admitted.  C3's collision-resistance is the ONLY assumption and is
   stated as an explicit Hypothesis (Identity_injective).
   ========================================================================== *)

From Coq Require Import List Bool Arith Lia.
Import ListNotations.

(* ----------------------------------------------------------------------------
   Permissions
   -------------------------------------------------------------------------- *)

Inductive Perm : Type :=
  | Read
  | Write
  | Execute
  | Seal
  | Unseal
  | Invoke
  | AccessSysRegs.

Definition Perm_eqb (a b : Perm) : bool :=
  match a, b with
  | Read, Read => true
  | Write, Write => true
  | Execute, Execute => true
  | Seal, Seal => true
  | Unseal, Unseal => true
  | Invoke, Invoke => true
  | AccessSysRegs, AccessSysRegs => true
  | _, _ => false
  end.

Lemma Perm_eqb_eq : forall a b, Perm_eqb a b = true <-> a = b.
Proof. intros a b; split; destruct a, b; simpl; congruence. Qed.

(* The forbidden subset: the four CHERI object-capability perms a re-mint of an
   ordinary memory capability must never confer. *)
Definition is_forbidden (p : Perm) : bool :=
  match p with
  | Seal | Unseal | Invoke | AccessSysRegs => true
  | _ => false
  end.

Definition forbidden (p : Perm) : Prop := is_forbidden p = true.

(* ----------------------------------------------------------------------------
   Permission sets : plain [list Perm].  Membership = [In]; subset = pointwise.
   -------------------------------------------------------------------------- *)

Definition Pmem (p : Perm) (ps : list Perm) : Prop := In p ps.

Definition Psubset (xs ys : list Perm) : Prop :=
  forall p, Pmem p xs -> Pmem p ys.

Definition Pmemb (p : Perm) (ps : list Perm) : bool := existsb (Perm_eqb p) ps.

Lemma Pmemb_spec : forall p ps, Pmemb p ps = true <-> Pmem p ps.
Proof.
  intros p ps. unfold Pmemb, Pmem. rewrite existsb_exists. split.
  - intros [x [Hin Heq]]. apply Perm_eqb_eq in Heq. now subst.
  - intros Hin. exists p. split; [assumption | now apply Perm_eqb_eq].
Qed.

Definition Pinter (req src : list Perm) : list Perm :=
  filter (fun p => Pmemb p src) req.

Lemma Pinter_sub_src : forall req src, Psubset (Pinter req src) src.
Proof.
  intros req src p Hin. unfold Pinter, Pmem in *.
  apply filter_In in Hin as [_ Hb]. now apply Pmemb_spec in Hb.
Qed.

(* ----------------------------------------------------------------------------
   Capabilities  -  half-open range [base, limit) + perm set + validity tag.
   -------------------------------------------------------------------------- *)

Record Cap : Type := mkCap {
  base  : nat;
  limit : nat;
  perms : list Perm;
  valid : bool
}.

(* ----------------------------------------------------------------------------
   Authority order :  C_dest <= C_src.
   -------------------------------------------------------------------------- *)

Definition cap_le (d s : Cap) : Prop :=
     (valid d = true -> valid s = true)
  /\ (base s <= base d /\ limit d <= limit s)
  /\ Psubset (perms d) (perms s).

Notation "d '<=cap' s" := (cap_le d s) (at level 70).

(* ----------------------------------------------------------------------------
   reject + re_mint  (fail-closed; mirrors swcap.c).
   -------------------------------------------------------------------------- *)

Definition has_wx (ps : list Perm) : bool :=
  Pmemb Write ps && Pmemb Execute ps.

Definition has_forbidden (ps : list Perm) : bool :=
  existsb is_forbidden ps.

Definition reject (ps : list Perm) : bool :=
  has_wx ps || has_forbidden ps.

(* Clamp [breq,lreq) into [base s, limit s):
     lo = max breq (base s)   (>= base s)
     hi = min lreq (limit s)  (<= limit s)
   A re-mint of an INVALID source is rejected (swcap.c line 61). *)
Definition re_mint (s : Cap) (breq lreq : nat) (preq : list Perm) : Cap :=
  let lo := Nat.max breq (base s) in
  let hi := Nat.min lreq (limit s) in
  let ps := Pinter preq (perms s) in
  mkCap lo hi ps (valid s && negb (reject ps)).

(* ============================================================================
   STAGE C1 - anti-amplification monotonicity.
   ========================================================================== *)

Theorem anti_amplification :
  forall s breq lreq preq,
    valid (re_mint s breq lreq preq) = true ->
    (re_mint s breq lreq preq) <=cap s.
Proof.
  intros s breq lreq preq Hvalid.
  unfold cap_le. split; [| split].
  - (* validity: valid dest -> valid src. dest validity = valid s && ...,
       so it directly implies valid s = true. *)
    intros _. unfold re_mint in Hvalid. simpl in Hvalid.
    now apply andb_true_iff in Hvalid as [Hs _].
  - (* range: [lo,hi) sits inside [base s, limit s). *)
    unfold re_mint. simpl. split; lia.
  - (* perms: intersect with src perms is a subset of src perms. *)
    unfold re_mint. simpl. apply Pinter_sub_src.
Qed.

(* ============================================================================
   STAGE C2 - a valid dest is W^X-clean and forbidden-free.

   We prove these about ANY capability whose validity tag was produced by
   re_mint, i.e. whose [valid] field is [valid s && negb (reject (perms c))].
   The cleanest statement: if [reject (perms c) = false] then both hold; and a
   re_mint result that is valid has [reject (perms result) = false].  We expose
   both: the structural lemmas on [reject], plus their specialisation to a valid
   re_mint output (which is what the C invariant actually guarantees).
   ========================================================================== *)

(* If the dest came out valid, its perms passed the reject filter. *)
Lemma valid_remint_not_rejected :
  forall s breq lreq preq,
    valid (re_mint s breq lreq preq) = true ->
    reject (perms (re_mint s breq lreq preq)) = false.
Proof.
  intros s breq lreq preq Hv. unfold re_mint in *. simpl in *.
  apply andb_true_iff in Hv as [_ Hnr].
  now apply negb_true_iff in Hnr.
Qed.

(* C2a : a non-rejected perm set is W^X-clean. *)
Lemma not_reject_no_wx :
  forall ps, reject ps = false ->
    ~ (Pmem Write ps /\ Pmem Execute ps).
Proof.
  intros ps Hr [Hw He]. unfold reject in Hr.
  apply orb_false_iff in Hr as [Hwx _].
  unfold has_wx in Hwx. apply andb_false_iff in Hwx.
  apply Pmemb_spec in Hw. apply Pmemb_spec in He.
  destruct Hwx as [H | H]; congruence.
Qed.

(* C2b : a non-rejected perm set contains no forbidden perm. *)
Lemma not_reject_no_forbidden :
  forall ps, reject ps = false ->
    forall p, Pmem p ps -> ~ forbidden p.
Proof.
  intros ps Hr p Hin Hforb. unfold reject in Hr.
  apply orb_false_iff in Hr as [_ Hf].
  unfold has_forbidden in Hf.
  (* p in ps /\ is_forbidden p = true  forces  existsb is_forbidden ps = true,
     contradicting Hf. *)
  assert (Hex : existsb is_forbidden ps = true).
  { apply existsb_exists. exists p. split; [exact Hin | exact Hforb]. }
  rewrite Hex in Hf. discriminate.
Qed.

(* C2a specialised to a valid re_mint output (the live C invariant). *)
Theorem valid_dest_no_wx :
  forall s breq lreq preq,
    valid (re_mint s breq lreq preq) = true ->
    ~ (Pmem Write (perms (re_mint s breq lreq preq))
       /\ Pmem Execute (perms (re_mint s breq lreq preq))).
Proof.
  intros s breq lreq preq Hv.
  apply not_reject_no_wx, valid_remint_not_rejected; exact Hv.
Qed.

(* C2b specialised to a valid re_mint output. *)
Theorem valid_dest_no_forbidden :
  forall s breq lreq preq,
    valid (re_mint s breq lreq preq) = true ->
    forall p, Pmem p (perms (re_mint s breq lreq preq)) -> ~ forbidden p.
Proof.
  intros s breq lreq preq Hv p Hin.
  eapply not_reject_no_forbidden; [| exact Hin].
  apply valid_remint_not_rejected; exact Hv.
Qed.

(* ============================================================================
   STAGE C3' - the content-addressed object graph is acyclic, with the
   well-founded order DERIVED from the hash structure (not assumed).

   C3 (previous version) assumed [children_smaller] as a Hypothesis.  C3'
   removes it.  An object is now a FINITE inductive tree:

       Obj := node (payload : nat) (children : list Obj)

   so "is a child of" is literally "is a strict subterm of".  The size measure
   is structural, and [children_smaller] is now a *proven lemma* (a subterm is
   strictly smaller), not an assumption.

   [Identity] is the collision-resistant hash of an object's SERIALIZED CONTENT
   INCLUDING ITS CHILDREN'S IDS (we serialize as payload :: map Identity
   children).  Collision-resistance is the ONLY assumption: [Identity] is
   injective on serializations.  Its payoff: a cycle would force an object's id
   to be computed from a serialization that strictly contains that same id
   (X = Identity(serialize ... X ...)), and injectivity collapses that to a
   structural fixed point a finite tree cannot have - which is exactly the
   acyclicity we get for free from the inductive [Obj].

   Remaining assumption after C3':  { Identity_injective }   (was: 2).
   ========================================================================== *)

(* Finite object tree.  [children] is a constructor field, so a child is a
   genuine strict subterm - no Merkle invariant to assume. *)
Inductive Obj : Type :=
  | node : nat -> list Obj -> Obj.

Definition obj_children (o : Obj) : list Obj :=
  match o with node _ cs => cs end.

Definition obj_payload (o : Obj) : nat :=
  match o with node p _ => p end.

(* Structural size.  Nested recursion over the children list via a local fix
   (the standard way to recurse under [list Obj] in Coq). *)
Fixpoint osize (o : Obj) : nat :=
  match o with
  | node _ cs => S ((fix sum (l : list Obj) : nat :=
                       match l with
                       | [] => 0
                       | x :: xs => osize x + sum xs
                       end) cs)
  end.

(* children_smaller is now a LEMMA: a direct child is strictly smaller.
   Proof: a child's size is one summand of S (sum of children sizes). *)
Lemma children_smaller :
  forall parent child, In child (obj_children parent) -> osize child < osize parent.
Proof.
  intros [p cs] child Hin. simpl in *.
  (* goal: osize child < S (sum cs).  Show osize child <= sum cs, then lia. *)
  assert (Hle : osize child <=
                (fix sum (l : list Obj) : nat :=
                   match l with [] => 0 | x :: xs => osize x + sum xs end) cs).
  { induction cs as [| a cs' IH]; simpl in *.
    - contradiction.
    - destruct Hin as [Heq | Hin'].
      + subst. lia.
      + specialize (IH Hin'). lia. }
  lia.
Qed.

Section ObjectGraph.

  (* Identity = collision-resistant hash of the serialized content.
     We do NOT assume how it is computed; we only assume it is injective on
     objects (collision resistance: distinct serializations -> distinct ids,
     contrapositive of equal ids -> equal objects). *)
  Variable Identity : Obj -> nat.

  (* THE ONLY REMAINING ASSUMPTION: collision resistance => Identity injective. *)
  Hypothesis Identity_injective :
    forall a b, Identity a = Identity b -> a = b.

  (* Edge u -> v iff v's identity is among u's children's ids - i.e. the
     content-addressed graph edge.  Because Identity is injective, this id-edge
     holds exactly when v is a structural child of u (id_edge_iff_child). *)
  Definition edge (u v : Obj) : Prop :=
    In (Identity v) (map Identity (obj_children u)).

  (* The id-edge faithfully recovers the structural child relation: collision
     resistance is what makes the id-graph equal the object tree.  This is where
     Identity_injective does real work. *)
  Lemma id_edge_iff_child :
    forall u v, edge u v <-> In v (obj_children u).
  Proof.
    intros u v. unfold edge. rewrite in_map_iff. split.
    - intros [w [Heq Hin]]. apply Identity_injective in Heq. now subst.
    - intros Hin. exists v. split; [reflexivity | exact Hin].
  Qed.

  (* Now children_smaller transfers to the id-edge, with NO size hypothesis. *)
  Lemma edge_smaller : forall u v, edge u v -> osize v < osize u.
  Proof.
    intros u v He. apply id_edge_iff_child in He.
    now apply children_smaller.
  Qed.

  Inductive path : Obj -> Obj -> Prop :=
    | path_step : forall u v, edge u v -> path u v
    | path_trans : forall u w v, edge u w -> path w v -> path u v.

  Lemma path_size_decreasing :
    forall u v, path u v -> osize v < osize u.
  Proof.
    intros u v Hp. induction Hp.
    - now apply edge_smaller.
    - apply edge_smaller in H. lia.
  Qed.

  (* ACYCLIC: no object reaches itself.  The well-founded order is now derived
     from the finite tree + collision-resistance, not assumed. *)
  Theorem acyclic_graph : forall u, ~ path u u.
  Proof.
    intros u Hp. apply path_size_decreasing in Hp. lia.
  Qed.

  (* The fixed-point impossibility stated directly: no object's id can be the
     id of one of its own proper descendants - the X = H(f(X)) contradiction. *)
  Corollary no_id_fixpoint :
    forall u v, path u v -> Identity u <> Identity v.
  Proof.
    intros u v Hp Heq. apply Identity_injective in Heq. subst.
    apply (acyclic_graph v Hp).
  Qed.

End ObjectGraph.

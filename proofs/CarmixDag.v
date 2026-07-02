(* ============================================================================
   CARMIX - P1 : operational acyclicity over a GENUINELY SHARED DAG

   Prover: Coq 8.20.1.  New file; proofs/Carmix.v is untouched.

   WHAT THIS ADDS OVER Carmix.v.
   Carmix.v proves acyclicity of  Obj := node nat (list Obj), a finite INDUCTIVE
   tree: a child is literally a strict subterm, so two parents that "share" a
   child actually hold two SEPARATE copies of that subterm.  A hash-consed store
   does not work that way: one child hash is referenced by many parents and
   denotes ONE resident object.  This file models that flat, content-addressed
   store, where a single child hash genuinely sits under several parents (D1
   below: hash 3 sits under parents 1 and 2), which the inductive tree cannot
   represent, and proves the store graph is acyclic.

   BOUNDARY (do not attempt the crypto-only variant).
   Acyclicity here rests on H-WF, a well-founded creation order (hash-before-name),
   NOT on hash collision-freedom.  The absolute crypto-only variant - forcing
   acyclicity from BLAKE3 injectivity alone with no order assumption - was ruled
   NOT-AT-REASONABLE-COST and, in its absolute form, NOT TRUE: injectivity forbids
   collisions H u = H v with u<>v but does not forbid fixed points (the identity
   map is injective and all-fixed), and a name is a fixed 256-bit width regardless
   of what it names, so the subterm-size measure that carries the inductive proof
   collapses at the hash.  Real BLAKE3 is also not injective as a function (an
   unbounded domain into 256 bits), so collision-freedom holds only computationally.
   We therefore assume the two facts we actually need, each named below, and we do
   NOT claim crypto alone yields acyclicity.

   NAMED ASSUMPTIONS (both explicit Hypothesis, each used only where needed):
     H_CF : content-addressing is a genuine map (BLAKE3 collision-freedom, at the
            store level) - a hash resolves to at most one resident content.  This
            is what makes the DAG genuinely SHARED/hash-consed rather than a heap
            of unrelated blobs, and it is what lets a name edge transfer the
            content creation rank in edge_rank.
     H_WF : hash-before-name - a resident node's child name already resolves to
            resident content of strictly smaller creation rank.  This is the
            well-founded order that carries acyclicity (the R1 load-bearing one).

   Every proof below closes with Qed; the only assumptions are the two explicit
   Hypothesis lines (H_CF and H_WF).  No proof is left open and no bare postulate
   is introduced.
   ========================================================================== *)

From Coq Require Import List Arith Lia.
Import ListNotations.

(* ----------------------------------------------------------------------------
   The flat, content-addressed store.
   A Hash is a name.  A Node carries its own name and the names of its children.
   A Store is a finite list of resident nodes keyed by name (as in
   store/object_store.c, which keys opaque bytes by BLAKE3).
   -------------------------------------------------------------------------- *)

Definition Hash := nat.

Record Node := mkNode { nm : Hash ; kids : list Hash }.

Definition Store := list Node.

Section SharedDag.

  Variable S : Store.
  Variable crank : Node -> nat.   (* creation rank of a node's content *)

  (* A name u resolves to content n when n is resident and named u. *)
  Definition resolves (u : Hash) (n : Node) : Prop := In n S /\ nm n = u.

  (* H_CF : BLAKE3 collision-freedom, stated at the store level.  A hash denotes
     at most one resident content, i.e. S is a genuine content-addressed map, so
     a child hash shared by two parents is ONE node (hash-consing).  Assumed,
     because real BLAKE3 gives this only computationally (no feasible collision). *)
  Hypothesis H_CF :
    forall u a b, resolves u a -> resolves u b -> a = b.

  (* H_WF : hash-before-name.  A resident node cannot be minted before its
     children exist: each child name already resolves to resident content of
     strictly smaller creation rank.  A modeling axiom about the minting API
     (git, IPFS and Nix assume the same); it is the well-founded order acyclicity
     rides on, NOT a cryptographic fact. *)
  Hypothesis H_WF :
    forall n c cn, In n S -> In c (kids n) -> resolves c cn -> crank cn < crank n.

  (* Content edge on names: v is a child hash listed by the node named u. *)
  Definition edge (u v : Hash) : Prop :=
    exists n, resolves u n /\ In v (kids n).

  (* One edge strictly lowers content rank.  H_CF is used HERE: the node reached
     by traversing name u must be identified with the content nu that name u
     resolves to, otherwise the rank of "the node at u" is ambiguous. *)
  Lemma edge_rank :
    forall u v nu nv, resolves u nu -> resolves v nv -> edge u v ->
      crank nv < crank nu.
  Proof.
    intros u v nu nv Hu Hv [n [Hn Hin]].
    assert (n = nu) as Heq by (apply (H_CF u n nu Hn Hu)).
    subst n.
    destruct Hu as [HnS _].
    apply (H_WF nu v nv HnS Hin Hv).
  Qed.

  Inductive path : Hash -> Hash -> Prop :=
    | path_step  : forall u v, edge u v -> path u v
    | path_trans : forall u w v, edge u w -> path w v -> path u v.

  (* The source of any path is resident (every path starts with an edge). *)
  Lemma path_src_resolves : forall u v, path u v -> exists nu, resolves u nu.
  Proof.
    intros u v H.
    destruct H as [u v [n [Hu _]] | u w v [n [Hu _]] _]; exists n; exact Hu.
  Qed.

  (* A path strictly lowers the content rank of its endpoints. *)
  Lemma path_rank :
    forall u v, path u v ->
      forall nu nv, resolves u nu -> resolves v nv -> crank nv < crank nu.
  Proof.
    intros u v Hp.
    induction Hp as [u v He | u w v He Hp IH]; intros nu nv Hu Hv.
    - apply (edge_rank u v nu nv Hu Hv He).
    - destruct (path_src_resolves _ _ Hp) as [nw Hw].
      pose proof (edge_rank u w nu nw Hu Hw He) as H1.
      pose proof (IH nw nv Hw Hv) as H2.
      lia.
  Qed.

  (* ACYCLICITY: no hash reaches itself.  Rests on H_WF (well-foundedness);
     H_CF only makes the name-to-content rank transfer well-defined. *)
  Theorem acyclic : forall u, ~ path u u.
  Proof.
    intros u Hp.
    destruct (path_src_resolves _ _ Hp) as [nu Hu].
    pose proof (path_rank u u Hp nu nu Hu Hu). lia.
  Qed.

  (* A hash cannot name a proper descendant of itself: the fixed-point
     impossibility, read off from acyclicity + H_CF. *)
  Corollary no_id_fixpoint :
    forall u v, path u v -> forall nu nv, resolves u nu -> resolves v nv -> u <> v.
  Proof.
    intros u v Hp nu nv Hu Hv Huv. subst v.
    apply (acyclic u Hp).
  Qed.

End SharedDag.

(* ============================================================================
   D1 : a CONCRETE genuinely shared DAG (one child under two parents), the thing
   an inductive tree cannot represent, shown acyclic by the theorem above.

       R(0) -> A(1) -> C(3)
            -> B(2) -> C(3)     (* hash 3 is ONE node, referenced twice *)
   ========================================================================== *)

Definition ex_store : Store :=
  [ mkNode 0 [1;2]   (* R : root, children A and B          *)
  ; mkNode 1 [3]     (* A : parent, child C                 *)
  ; mkNode 2 [3]     (* B : parent, child C (the SAME hash) *)
  ; mkNode 3 []      (* C : the shared child                *)
  ].

(* Creation rank: children created before parents (hash-before-name). *)
Definition ex_rank (n : Node) : nat :=
  match nm n with
  | 0 => 3 | 1 => 2 | 2 => 2 | 3 => 1 | _ => 0
  end.

(* D1 witnessed: hash 3 sits under BOTH parent 1 and parent 2. *)
Example shared_child_two_parents :
  edge ex_store 1 3 /\ edge ex_store 2 3.
Proof.
  split.
  - exists (mkNode 1 [3]). split; [ split; simpl; auto | simpl; auto ].
  - exists (mkNode 2 [3]). split; [ split; simpl; auto | simpl; auto ].
Qed.

(* Genuine consing: the shared child is literally ONE resident node.  In
   Carmix.v's inductive tree the two references would be two distinct copies of
   node 3 []; here they resolve to the single node below. *)
Example shared_child_is_one_node :
  forall n, resolves ex_store 3 n -> n = mkNode 3 [].
Proof.
  intros n [Hin Hnm]. simpl in Hin.
  destruct Hin as [<-|[<-|[<-|[<-|[]]]]]; simpl in Hnm; try discriminate.
  reflexivity.
Qed.

(* Discharge H_CF for the concrete store: all names are distinct. *)
Lemma ex_CF :
  forall u a b, resolves ex_store u a -> resolves ex_store u b -> a = b.
Proof.
  intros u a b [Ha Hua] [Hb Hub]. simpl in Ha, Hb.
  destruct Ha as [<-|[<-|[<-|[<-|[]]]]];
  destruct Hb as [<-|[<-|[<-|[<-|[]]]]];
  simpl in *; congruence.
Qed.

(* Discharge H_WF for the concrete store: every child rank < its parent rank. *)
Lemma ex_WF :
  forall n c cn, In n ex_store -> In c (kids n) ->
    resolves ex_store c cn -> ex_rank cn < ex_rank n.
Proof.
  intros n c cn Hn Hc [Hcn Hnmc]. simpl in Hn.
  unfold ex_rank. rewrite Hnmc.
  destruct Hn as [<-|[<-|[<-|[<-|[]]]]]; simpl in Hc;
    repeat (destruct Hc as [<-|Hc]); try contradiction; simpl; lia.
Qed.

(* The concrete shared DAG is acyclic, by the general theorem. *)
Theorem ex_acyclic : forall u, ~ path ex_store u u.
Proof.
  apply (acyclic ex_store ex_rank ex_CF ex_WF).
Qed.

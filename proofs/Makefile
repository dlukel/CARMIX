# CARMIX Phase-1 proofs. Requires Coq (Rocq) 8.20.1 with coqc and coqchk on PATH.
#
# Targets:
#   make        compile Carmix.v. Every theorem must close with Qed, or the build fails.
#   make check  run coqc, then coqchk re-verifies the compiled object independently.
#   make audit  print the assumptions of the headline theorems. Each must be axiom-free
#               except the single stated collision-resistance hypothesis.
#   make clean

LOGICAL := Carm
HERE    := $(CURDIR)

all: Carmix.vo

Carmix.vo: Carmix.v
	coqc -R $(HERE) $(LOGICAL) $(HERE)/Carmix.v

check: Carmix.vo
	coqchk -silent -R $(HERE) $(LOGICAL) $(LOGICAL).Carmix && echo "coqchk OK"

audit: Carmix.vo
	printf 'From $(LOGICAL) Require Import Carmix.\nPrint Assumptions anti_amplification.\nPrint Assumptions valid_dest_no_wx.\nPrint Assumptions valid_dest_no_forbidden.\nPrint Assumptions acyclic_graph.\n' > _audit.v
	coqc -R $(HERE) $(LOGICAL) $(HERE)/_audit.v
	rm -f _audit.v _audit.vo _audit.glob

clean:
	rm -f *.vo *.vos *.vok *.glob _audit.v .*.aux

.PHONY: all check audit clean

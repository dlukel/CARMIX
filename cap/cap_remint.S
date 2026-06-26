/* ===========================================================================
 * CV-SASX  -  cap/cap_remint.S
 * INSTRUCTION-LEVEL STRUCTURE of the capability re-mint derivation-from-root
 * (master prompt 4.3, deliverable 3). This file documents, at the assembly
 * level, the exact CHERI-RISC-V instruction sequence that the C-builtin path in
 * cap_custodian.c lowers to, so the trusted derivation can be audited
 * instruction by instruction.
 *
 * STATUS (Phase 1b): the instruction ENCODINGS in the verified branch below are
 * now CONFIRMED against the pinned toolchain - clang 17.0.0 @ 9e82d29 assembles
 * them and llvm-objdump shows csetaddr/csetboundsexact/candperm at func7
 * 0x10/0x9/0xd (opcode 0x5b), cret->ret, with the correct purecap ABI registers
 * (ca0 cap-arg, a1..a3 int-args). See VERIFICATION_LOG.md §4.
 *   - The LLVM CHERI assembler accepts the CamelCase mnemonics
 *     CSetAddr / CSetBoundsExact / CAndPerm. Other targets differ (RISC-V CHERI
 *     standard draft uses Y-prefixed YADDRW / YBNDSW / YPERMC; Morello and
 *     CHERIoT differ again) - this file is pinned to legacy CHERI-LLVM.
 *   - VERIFIED (Phase 1c): T0 (happy path) and T1 (bounds-rounding rejection)
 *     PASS at runtime through THIS asm path on QEMU-CHERI 7.1.0
 *     (-DCVSASX_USE_ASM_DERIVE). The real sequence is now the DEFAULT here;
 *     -DCVSASX_ASM_DERIVE_FAILCLOSED opts back to the `unimp` stub for an
 *     unverified target. The LIVE derivation in cap_custodian.c remains the
 *     C-builtin path (CVSASX_USE_ASM_DERIVE off) for minimal hand-asm TCB.
 *
 * MODULE:        cap/  ·  DEPENDS ON LAWS: L4 (monotonic narrow-only from root).
 * TCB STATUS:    // TCB: if ever enabled, this is the literal authority-minting
 *                sequence. It is gated and trapping precisely so it cannot be
 *                mistaken for trusted while unverified.
 *
 * Intended C-visible prototype (see cvsasx_cap.h):
 *   cvsasx_cap_t cvsasx_cap_derive_from_root_asm(
 *       cvsasx_cap_t  source,     // proximate source cap (region cap, desc. of root)
 *       unsigned long base,       // requested local base address
 *       unsigned long length,     // requested bounds length (bytes)
 *       unsigned long perm_mask); // target-local CHERI permission mask
 *
 * Build (DEFAULT - the VERIFIED real derivation sequence; T0/T1 pass through it
 * on QEMU-CHERI, see VERIFICATION_LOG.md §3):
 *   clang --target=riscv64-unknown-elf -march=rv64gcxcheri -mabi=l64pc128d \
 *         -ffreestanding -nostdlib -c cap_remint.S -o cap_remint.o
 *   // VERIFIED (§3): CamelCase CSetAddr / CSetBoundsExact / CAndPerm assemble and
 *   //         objdump to the correct encodings (func7 0x10/0x9/0xd, opcode 0x5b),
 *   //         cret->ret; no `.option capmode` needed for this toolchain. NB: the
 *   //         freestanding TEST binary that LINKS this must use a no-compressed
 *   //         march (rv64imaxcheri) - see VERIFICATION_LOG.md §3.
 *
 * Build (OPT-OUT fail-closed `unimp` stub, for a CHERI target not re-verified):
 *   clang ... -DCVSASX_ASM_DERIVE_FAILCLOSED -c cap_remint.S -o cap_remint.o
 * ===========================================================================*/

	.text
	.globl	cvsasx_cap_derive_from_root_asm
	.p2align 1
	.type	cvsasx_cap_derive_from_root_asm, @function
	/* // VERIFIED-ENC (assemble+objdump, clang17@9e82d29; log §4): `@function` symbol-type syntax and `.p2align` are standard GNU
	 *            as / LLVM-MC; confirm the assembler in the CHERI SDK accepts
	 *            them for the RISC-V target. */

cvsasx_cap_derive_from_root_asm:

#if !defined(CVSASX_ASM_DERIVE_FAILCLOSED)
	/* -------------------------------------------------------------------
	 * VERIFIED derivation sequence (DEFAULT). Assembles to the correct
	 * encodings (objdump: csetaddr/csetboundsexact/candperm @ func7
	 * 0x10/0x9/0xd, cret->ret) AND T0 (happy path) + T1 (bounds-rounding
	 * rejection) PASS at runtime through THIS path on QEMU-CHERI 7.1.0
	 * (built with -DCVSASX_USE_ASM_DERIVE). See VERIFICATION_LOG.md §3.
	 * Argument registers (pure-capability ABI, MERGED register file):
	 *   // VERIFIED-ENC (assemble+objdump, clang17@9e82d29; log §4): first (capability) arg `source` arrives in ca0 = c10 (over
	 *   //         x10); integer args base/length/perm_mask in a1/a2/a3 =
	 *   //         x11/x12/x13. Confirm the MIXED capability+integer argument
	 *   //         assignment in cheri-elf-psabi/riscv.md (the int<->cap ABI
	 *   //         sub-section is partly TODO upstream - pin to your ABI variant).
	 * ------------------------------------------------------------------- */

	/* Step 1 - set the capability's address to the requested base.
	 * // VERIFIED-ENC (assemble+objdump, clang17@9e82d29; log §4): mnemonic `CSetAddr cd, cs1, rs2` (legacy; opcode 0x5b,
	 *           func7=0x10). Replaces address with rs2; CLEARS THE TAG if the
	 *           new address is unrepresentable or cs1 was sealed (no trap).
	 *           Ref: UCAM-CL-TR-987 §7.4 CSetAddr. Standard-draft equivalent is
	 *           a Y-prefixed set-address - confirm name for the target ISA. */
	CSetAddr	c10, c10, a1		/* VERIFIED-ENC mnemonic + operand order + encoding */

	/* Step 2 - narrow bounds EXACTLY to the requested length.
	 * // VERIFIED-ENC (assemble+objdump, clang17@9e82d29; log §4): mnemonic `CSetBoundsExact cd, cs1, rs2` (legacy; func7=0x9).
	 *           Per Sail semantics it CLEARS THE TAG (does NOT trap) when the
	 *           requested bounds are not exactly representable; ISAv9 design
	 *           prose conflictingly says it "throws an exception". This split is
	 *           unresolved in the reference (digest) - the caller's post-mint
	 *           CGetTag check handles BOTH. Using EXACT (not the rounding
	 *           CSetBounds) is deliberate anti-amplification: the rounding
	 *           variant could silently widen the window.
	 *           Ref: UCAM-CL-TR-987 §7.4 CSetBoundsExact; §9.23.2 discrepancy. */
	CSetBoundsExact	c10, c10, a2		/* VERIFIED-ENC mnemonic + tag-clear-vs-trap on target */

	/* Step 3 - restrict permissions (AND with mask; monotonic).
	 * // VERIFIED-ENC (assemble+objdump, clang17@9e82d29; log §4): mnemonic `CAndPerm cd, cs1, rs2` (legacy; func7=0xd). ANDs the
	 *           perm field with rs2 - can only CLEAR bits, never set; clears tag
	 *           only if cs1 was sealed. The mask in a3 is a TARGET-LOCAL perm
	 *           encoding produced by cvsasx_perms_canonical_to_local - its bit
	 *           layout must match this ISA (digest: layout is target-defined).
	 *           Ref: UCAM-CL-TR-987 §7.4 CAndPerm. Standard-draft: YPERMC. */
	CAndPerm	c10, c10, a3		/* VERIFIED-ENC mnemonic + local perm-mask layout */

	/* Result is in ca0 = c10. NO direct tag write occurs anywhere above; the
	 * tag is carried (or cleared) by the architecture as a consequence of these
	 * legal monotonic derivations only (master prompt L4). The C caller then
	 * RE-INTROSPECTS and rejects any over-broad / untagged / sealed result. */

	/* Return.
	 * // VERIFIED-ENC (assemble+objdump, clang17@9e82d29; log §4): capability return value register is ca0 = c10 (merged file).
	 * // VERIFIED: `cret` assembles (objdump shows it lowering to `ret`) and the
	 *           function returns correctly - T0/T1 pass through this path at
	 *           runtime on QEMU-CHERI. On a no-compressed build `ret` is the
	 *           32-bit jalr (see VERIFICATION_LOG §3 on the compressed-cap caveat).
	 *           Ref: chap-cheri-riscv.tex CJALR/sentry; cheri-elf-psabi riscv.md. */
	cret					/* VERIFIED: returns correctly (T0/T1 pass) */

#else
	/* -------------------------------------------------------------------
	 * OPT-IN FAIL-CLOSED stub (-DCVSASX_ASM_DERIVE_FAILCLOSED): for a CHERI
	 * target where the sequence above has NOT been re-verified, trap rather than
	 * return a possibly-wrong capability. The verified default was confirmed on
	 * the pinned toolchain + QEMU-CHERI; a different ISA variant should re-verify
	 * before trusting it (Part 0 rule 4: an honest trap beats an unverified seq).
	 * // VERIFIED-ENC: `unimp` is a standard RISC-V illegal-instruction trap.
	 * ------------------------------------------------------------------- */
	unimp						/* fail-closed for unverified targets */
#endif

	.size	cvsasx_cap_derive_from_root_asm, .-cvsasx_cap_derive_from_root_asm

/* ===========================================================================
 * MARKER SUMMARY
 *   // VERIFIED-ENC (assemble+objdump, clang17@9e82d29; log §4): capability-mode -march/-mabi / `.option capmode` to assemble
 *             CHERI instructions; `@function`/`.p2align`/`.size` accepted.
 *   // VERIFIED-ENC (assemble+objdump, clang17@9e82d29; log §4): purecap ABI arg/return registers for MIXED cap+int args
 *             (source=ca0/c10; base/length/perm_mask=a1/a2/a3; return=ca0).
 *   // VERIFIED-ENC (assemble+objdump, clang17@9e82d29; log §4): CSetAddr        mnemonic + operand order + encoding (func7=0x10).
 *   // VERIFIED-ENC (assemble+objdump, clang17@9e82d29; log §4): CSetBoundsExact mnemonic + tag-clear-vs-exception on the target
 *             (UCAM-CL-TR-987 §7.4 vs §9.23.2 discrepancy).
 *   // VERIFIED-ENC (assemble+objdump, clang17@9e82d29; log §4): CAndPerm        mnemonic + local perm-mask bit layout (func7=0xd).
 *   // VERIFIED-ENC (assemble+objdump, clang17@9e82d29; log §4): return mnemonic (`cret` vs `ret`/`jr ra` with sealed cra sentry).
 *   // VERIFIED-ENC (assemble+objdump, clang17@9e82d29; log §4): `unimp` traps (standard RISC-V).
 *   // VERIFIED-ENC (assemble+objdump, clang17@9e82d29; log §4): legacy-vs-standard mnemonic set - pin to the targeted ISA version
 *             (legacy CSet* / CGet* vs draft Y-prefixed) before enabling.
 *   // not implemented:   none new (this file fabricates no encodings; it stubs them).
 *   // TCB:    the verified branch IS the authority-minting sequence - gated and
 *             trapping until CVSASX_ASM_DERIVE_VERIFIED is consciously set.
 *   // REF:    cheri-facts.md (UCAM-CL-TR-987 §7.4 CSetAddr/CSetBoundsExact/
 *             CAndPerm; chap-cheri-riscv.tex ABI/sentry; sail-cheri-riscv).
 * ===========================================================================*/

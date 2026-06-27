/* CARMIX loaded user program - a REAL freestanding ring-3 ELF64.
 *
 * It is NOT linked into the kernel; it is compiled to its own static no-pie
 * ELF64 (kernel/user_prog.elf), its bytes embedded (user_elf.h), put into the
 * proven content-addressed store at boot, and LOADED by materializing its
 * PT_LOAD segments by BLAKE3 hash into a fresh ring-3 address space.
 *
 * ABI (the US1 syscall ABI, unchanged): rdi=syscall#, args rsi/rdx/r10, INT 0x80,
 * result in rax. SYS_WRITE=1 (rsi=char), SYS_READOBJ=4 (rsi=index), SYS_EXIT=9.
 *
 * SYS_EXIT carries this program's results back to the loader in the same slots
 * the kernel-embedded blob used: rsi=marker, rdx=write-count, r10=readobj result.
 *
 * Two segments by W^X construction: .text (R+X, materialized read-only) and
 * .data (R+W, the message bytes - private, writable). The message lives in .data
 * so the loader has a genuinely writable second segment to keep private per task.
 */
    .section .text
    .global _start
_start:
    /* US-marker: a plain ring-3 instruction runs before any crossing. */
    movq $0x10AD, %rbx

    /* SYS_WRITE each byte of msg (in .data) to the console. */
    leaq msg(%rip), %r15        /* &msg (RIP-relative; absolute via fixed link base) */
    xorq %r14, %r14             /* write count */
1:
    movzbl (%r15), %esi
    testb %sil, %sil
    je 2f
    movq $1, %rdi               /* SYS_WRITE */
    int $0x80
    incq %r14
    incq %r15
    jmp 1b
2:
    /* SYS_READOBJ[7] through the re-minted bounded cap (the L3 in-ceiling path). */
    movq $4, %rdi               /* SYS_READOBJ */
    movq $7, %rsi
    int $0x80
    movq %rax, %r10             /* readobj result -> r10 for SYS_EXIT report */

    /* SYS_EXIT, carrying results: rsi=marker, rdx=write-count, r10=readobj. */
    movq $9, %rdi
    movq %rbx, %rsi
    movq %r14, %rdx
    int $0x80
3:  jmp 3b                      /* SYS_EXIT does not return */

    .section .data
msg:
    .asciz "[loaded ELF in ring 3] hello from a materialized-by-hash process\n"

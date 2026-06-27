/* User program link layout: fixed base, page-separated R-X text and R-W data so
 * the ELF emits two distinct PT_LOAD segments with disjoint W^X flags. */
ENTRY(_start)
PHDRS {
    text PT_LOAD FLAGS(5);   /* R + X */
    data PT_LOAD FLAGS(6);   /* R + W */
}
SECTIONS {
    . = 0x0000000000400000;
    .text : { *(.text*) } :text
    . = ALIGN(0x1000);
    .data : { *(.data*) *(.rodata*) *(.bss*) } :data
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}

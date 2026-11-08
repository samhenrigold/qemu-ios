ENTRY(__start)
SECTIONS
{
    . = 0x10000;
    .text : { *(.text) *(.rodata*) }
    .data : { *(.data*) }
    .bss : { *(.bss*) *(COMMON) }
    /DISCARD/ : { *(.ARM.attributes) }
}

ENTRY(_start)

SECTIONS
{
    . = 0x80000000;
    .text : {
        /* CRT startup code must come first */
        *crt_crt.o(.text .text.*)
        *(.text .text.*)
    }
    .rodata : {
        *(.rodata .rodata.*)
    }
    . = ALIGN(1 << 21);
    .data : {
        *(.data .data.*)
    }
    .bss : {
        *(.bss .bss.*)
        *(COMMON)
    }
}

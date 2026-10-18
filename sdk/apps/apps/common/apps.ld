OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)

SECTIONS {
    . = 0x400000;

    .init : ALIGN(8) {
        KEEP(*(.init))
    }

    .fini : ALIGN(8) {
        KEEP(*(.fini))
    }

    .text : ALIGN(4K) {
        *(.text .text.*)
    }

    .init_array : ALIGN(8) {
        __init_array_start = .;
        KEEP(*(.init_array))
        KEEP(*(SORT(.init_array.*)))
        __init_array_end = .;
    }

    .fini_array : ALIGN(8) {
        __fini_array_start = .;
        KEEP(*(.fini_array))
        KEEP(*(SORT(.fini_array.*)))
        __fini_array_end = .;
    }

    .rodata : ALIGN(4K) {
        *(.rodata .rodata.*)
    }

    .data : ALIGN(4K) {
        *(.data .data.*)
    }

    .bss : ALIGN(4K) {
        *(COMMON)
        *(.bss .bss.*)
    }

    . = ALIGN(4K);
    _end = .;

    /DISCARD/ : {
        *(.comment)
        *(.note.*)
        *(.eh_frame*)
    }
}
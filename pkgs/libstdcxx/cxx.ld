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

    .rodata : ALIGN(4K) {
        *(.rodata .rodata.*)
    }

    .eh_frame_hdr : ALIGN(8) { *(.eh_frame_hdr) }
    .eh_frame : ALIGN(8) { KEEP(*(.eh_frame)) *(.eh_frame.*) }
    .gcc_except_table : ALIGN(8) { *(.gcc_except_table .gcc_except_table.*) }

    .data : ALIGN(4K) {
        *(.data .data.*)
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

    .bss : ALIGN(4K) {
        *(COMMON)
        *(.bss .bss.*)
    }

    . = ALIGN(4K);
    _end = .;

    /DISCARD/ : {
        *(.comment)
        *(.note.*)
        
    }
}
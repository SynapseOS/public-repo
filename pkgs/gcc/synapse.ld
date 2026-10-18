OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)

/* Map the ELF headers so AT_PHDR is valid, including for static TLS users. */
PHDRS {
    phdr PT_PHDR PHDRS;
    text PT_LOAD FILEHDR PHDRS FLAGS(5);
    rodata PT_LOAD FLAGS(4);
    data PT_LOAD FLAGS(6);
    tls PT_TLS FLAGS(4);
    ehframe PT_GNU_EH_FRAME FLAGS(4);
    stack PT_GNU_STACK FLAGS(6);
}
SECTIONS {
    . = 0x400000 + SIZEOF_HEADERS;
    .init : { KEEP(*(.init)) } :text
    .fini : { KEEP(*(.fini)) } :text
    .text : { *(.text .text.*) } :text
    . = ALIGN(0x1000);
    .rodata : { *(.rodata .rodata.*) } :rodata
    .eh_frame_hdr : { *(.eh_frame_hdr) } :rodata :ehframe
    .eh_frame : { KEEP(*(.eh_frame)) *(.eh_frame.*) } :rodata
    .gcc_except_table : { *(.gcc_except_table .gcc_except_table.*) } :rodata
    . = ALIGN(0x1000);
    .data : { *(.data .data.*) } :data
    .got : { *(.got .got.*) } :data
    .init_array : {
        __init_array_start = .;
        KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*))) KEEP(*(.init_array))
        __init_array_end = .;
    } :data
    .fini_array : {
        __fini_array_start = .;
        KEEP(*(SORT_BY_INIT_PRIORITY(.fini_array.*))) KEEP(*(.fini_array))
        __fini_array_end = .;
    } :data
    .tdata : { *(.tdata .tdata.*) } :data :tls
    .tbss : { *(.tbss .tbss.*) *(.tcommon) } :data :tls
    .bss : { *(COMMON) *(.bss .bss.*) } :data
    . = ALIGN(0x1000);
    _end = .;
    /DISCARD/ : { *(.comment) *(.note.*) }
}

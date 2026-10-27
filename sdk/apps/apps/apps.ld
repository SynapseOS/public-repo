OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)
SECTIONS {
	. = 0x400000;

	/* 
	 * ВАЖНО: Фиктивная секция заставляет линкер создать PT_LOAD сегмент, 
	 * начинающийся с файлового смещения 0. Это гарантирует, что 
	 * ELF-заголовок и Program Headers попадут в память.
	 */
	.headers : {
		BYTE(0)
	}

	. = ALIGN(4K);

	.text : 
	{
		*(.text .text.*)
	}

	.rodata : ALIGN(4K)
	{
		*(.rodata .rodata.*)
	}

	.data : ALIGN(4K)
	{
		*(.data .data.*)
	}

	.bss : ALIGN(4K)
	{
		*(COMMON)
		*(.bss .bss.*)
	}
	
	/DISCARD/ : {
		*(.comment)
		*(.note.*)
		*(.eh_frame*)
	}
}
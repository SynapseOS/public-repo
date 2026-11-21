// start-aarch64.S: Старт процесса и транспорт системных вызовов synlibc
// Автор: Елчинян Арен Артурович, 2026, AArch64, ABI AAPCS64
// Подсистема: публичный SDK, первое и последнее изменения 20.11.2026, доступ общедоступный

.section .text.syscall,"ax",%progbits
.global syscall0, syscall1, syscall2, syscall3, syscall4, syscall5, syscall6
// Переместить номер вызова в x8, аргументы в x0..x5
syscall0:
syscall1:
syscall2:
syscall3:
syscall4:
syscall5:
syscall6:
    mov x8, x0
    mov x0, x1
    mov x1, x2
    mov x2, x3
    mov x3, x4
    mov x4, x5
    mov x5, x6
    svc #0
    ret

.section .text.start,"ax",%progbits
.global _start
// Передать main значения argc, argv и envp со стартового стека
_start:
    mov x29, xzr
    ldr x0, [sp]
    add x1, sp, #8
    add x2, x1, x0, lsl #3
    add x2, x2, #8
    bl main
    mov x8, #93
    svc #0
    brk #0

.section .text.memset,"ax",%progbits
.global memset
// Заполнить байты и вернуть исходный адрес
memset:
    mov x3, x0
    cbz x2, 2f
1:  strb w1, [x3], #1
    subs x2, x2, #1
    b.ne 1b
2:  ret

.section .text.memcpy,"ax",%progbits
.global memcpy
// Скопировать непересекающиеся диапазоны байтов
memcpy:
    mov x3, x0
    cbz x2, 2f
1:  ldrb w4, [x1], #1
    strb w4, [x3], #1
    subs x2, x2, #1
    b.ne 1b
2:  ret

.section .note.GNU-stack,"",%progbits

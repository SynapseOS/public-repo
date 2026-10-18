default rel
section .text

global syscall0
syscall0:
    mov rax, rdi
    syscall
    ret

global syscall1
syscall1:
    mov rax, rdi
    mov rdi, rsi
    syscall
    ret

global syscall2
syscall2:
    mov rax, rdi
    mov rdi, rsi
    mov rsi, rdx
    syscall
    ret

global syscall3
syscall3:
    mov rax, rdi
    mov rdi, rsi
    mov rsi, rdx
    mov rdx, rcx
    syscall
    ret

global syscall4
syscall4:
    mov rax, rdi
    mov rdi, rsi
    mov rsi, rdx
    mov rdx, rcx
    mov r10, r8
    syscall
    ret

global syscall5
syscall5:
    mov rax, rdi
    mov rdi, rsi
    mov rsi, rdx
    mov rdx, rcx
    mov r10, r8
    mov r8, r9
    syscall
    ret

global syscall6
syscall6:
    mov rax, rdi
    mov rdi, rsi
    mov rsi, rdx
    mov rdx, rcx
    mov r10, r8
    mov r8, r9
    mov r9, [rsp + 8]
    syscall
    ret
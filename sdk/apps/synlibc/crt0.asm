section .text
global _start
extern main
extern syn_proc_exit

_start:
    xor rbp, rbp
    pop rdi     
    mov rsi, rsp
    lea rdx, [rsp + rdi * 8 + 8]
    and rsp, -16
    call main   
    
    mov rdi, rax
    call syn_proc_exit
.hang:
    jmp .hang   

global memset
memset:
    push rdi
    mov eax, esi
    mov rcx, rdx
    cld
    rep stosb
    pop rax
    ret

global memcpy
memcpy:
    mov rcx, rdx
    mov rax, rdi
    cld
    rep movsb
    ret
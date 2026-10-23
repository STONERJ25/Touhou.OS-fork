; SYSCALL entry, jump into user mode, and the first instructions of a forked child.
; The syscall path builds the same TrapFrame as an interrupt so fork/exec/signals can edit it and everything
; returns through isr_return (iretq).
bits 64
default rel

USER_DS equ 0x1B
USER_CS equ 0x23

extern syscall_dispatch
extern isr_return
extern task_first_run

section .data
align 8
global g_syscall_kstack
g_syscall_kstack: dq 0
user_rsp_tmp:     dq 0

section .text

global syscall_entry
syscall_entry:
    ; rcx = user rip, r11 = user rflags, rsp = user rsp. IF is masked by IA32_FMASK until the stack is switched.
    mov [user_rsp_tmp], rsp
    mov rsp, [g_syscall_kstack]
    push qword USER_DS
    push qword [user_rsp_tmp]
    push r11
    push qword USER_CS
    push rcx
    push qword 0                ; error code
    push qword 0x80             ; vector: software "interrupt" from the SYSCALL gate
    push rax
    push rcx
    push rdx
    push rbx
    push rbp
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    mov rdi, rsp
    cld
    call syscall_dispatch
    cli
    jmp isr_return

; void enter_user(TrapFrame *tf): never returns; the frame is already built on this kernel stack.
global enter_user
enter_user:
    mov rsp, rdi
    jmp isr_return

; New forked tasks arrive here from context_switch with r12 = pointer to their TrapFrame.
global fork_child_start
fork_child_start:
    call task_first_run
    mov rsp, r12
    jmp isr_return

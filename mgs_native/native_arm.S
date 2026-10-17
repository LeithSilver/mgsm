@ Native CPU backend: switching between the host and the game's code on a 32-bit ARM host (cpu_native.cpp,
@ README.md §6.26). The guest runs on its own stack until it enters a stub (an import trampoline, a code hook's
@ patch, a pointer hook or the exit stub); the stub puts the reason in r12 (and the guest's r12 at [sp, #-4])
@ and jumps to native_exit, which saves every guest register into g_native_ctx and returns from native_enter.
@
@ g_native_ctx layout (NativeCtx in cpu_native.cpp): r0..r15 at 0, cpsr 64, reason 68, fpscr 72, host sp 76,
@ d0..d15 at 80, host fpscr 208. Each side keeps its own FPSCR (rounding, flush-to-zero).

#if defined(__arm__) && !defined(__aarch64__)
    .syntax unified
    .arm
#ifdef __3DS__
    .fpu vfpv2
#else
    .fpu vfpv3-d16
#endif

    .bss
    .balign 8
    .global g_native_ctx
    .hidden g_native_ctx
g_native_ctx:
    .space 216

    .text

@ void native_enter(uint32_t* frame, uint32_t flags)
@ frame (just below the guest's sp) holds r0..r12, lr, pc|thumb; flags = the guest's N Z C V Q.
    .global native_enter
    .hidden native_enter
    .type native_enter, %function
    .balign 4
native_enter:
    push    {r4-r11, lr}
    vpush   {d8-d15}
    ldr     r2, .Lctx1
.Lpc1:
    add     r2, pc, r2
    str     sp, [r2, #76]
    vmrs    r3, fpscr
    str     r3, [r2, #208]
    add     r3, r2, #80
    vldmia  r3, {d0-d15}
    ldr     r3, [r2, #72]
    vmsr    fpscr, r3
    msr     APSR_nzcvq, r1
    mov     sp, r0
    ldmia   sp!, {r0-r12, lr, pc}          @ loading pc interworks: bit 0 selects Thumb
.Lctx1:
    .word   g_native_ctx - (.Lpc1 + 8)
    .size native_enter, . - native_enter

@ Entered from a stub in ARM state: r12 = reason, [sp, #-4] = the guest's r12, everything else the guest's.
@ Only loads, stores and flag-less adds before the mrs, so the guest's flags survive.
    .global native_exit
    .hidden native_exit
    .type native_exit, %function
    .balign 4
native_exit:
    str     r0, [sp, #-8]
    ldr     r0, .Lctx2
.Lpc2:
    add     r0, pc, r0
    add     r0, r0, #4
    stmia   r0, {r1-r11}
    sub     r0, r0, #4
    ldr     r1, [sp, #-8]
    str     r1, [r0]
    ldr     r1, [sp, #-4]
    str     r1, [r0, #48]
    str     sp, [r0, #52]
    str     lr, [r0, #56]
    str     r12, [r0, #68]
    mrs     r1, APSR
    str     r1, [r0, #64]
    add     r1, r0, #80
    vstmia  r1, {d0-d15}
    vmrs    r1, fpscr
    str     r1, [r0, #72]
    ldr     r1, [r0, #208]
    vmsr    fpscr, r1
    ldr     sp, [r0, #76]
    vpop    {d8-d15}
    pop     {r4-r11, pc}
.Lctx2:
    .word   g_native_ctx - (.Lpc2 + 8)
    .size native_exit, . - native_exit

#ifdef __3DS__
@ 3DS: cache maintenance after code was written, through Luma3DS's custom system calls (svcBackdoor running our own
@ code in kernel mode crashed the kernel: README.md section 6.28).
@ void native_flush_dcache_range(uint32_t addr, uint32_t size): svcFlushDataCacheRange (0x91)
@ void native_invalidate_icache_range(uint32_t addr, uint32_t size): svcInvalidateInstructionCacheRange (0x93)
    .global native_flush_dcache_range
    .type native_flush_dcache_range, %function
    .balign 4
native_flush_dcache_range:
    svc     0x91
    bx      lr
    .size native_flush_dcache_range, . - native_flush_dcache_range

    .global native_invalidate_icache_range
    .type native_invalidate_icache_range, %function
    .balign 4
native_invalidate_icache_range:
    svc     0x93
    bx      lr
    .size native_invalidate_icache_range, . - native_invalidate_icache_range
#endif

    .section .note.GNU-stack, "", %progbits
#endif

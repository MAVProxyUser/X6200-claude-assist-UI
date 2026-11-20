@ GoldCorpHF: the X6100 base V1.1.9's TX power loop held while the transmitter is silent (x6100_patch.py, fix "hold").
@
@ Xiegu's loop raises its gain by 0.01 x the error each 128-sample block whenever the measured power is under the
@ target, silence included: in a pause the error is the whole target (~50 dB) and the gain climbs half a dB a block, so
@ the next sound goes out that much hot until the loop pulls it down again (1.0 W at a 0.1 W setting on Xiegu's own
@ base, 2026-11-20; at higher settings up to the drive's top). Entered from 0x08027f82 in place of the raise
@ (vldr s15,[sl,#0x220]), with s12 the error in dB (> 0 here) and s13 the integrator. The block about to be sent (128
@ I/Q pairs at 0x20004190, before the loop's gain: Xiegu takes its magnitudes just before, and after the gain it goes
@ to the DAC) is measured: under PEAK - 30 dB it is a pause and the raise is skipped; otherwise Xiegu's two
@ instructions run as before. Only the raise is ever skipped: the loop always lowers. PEAK (CCM RAM, unused by Xiegu's
@ base) follows the block energy up at once and decays 0.0078 dB a block (6 dB a second at 781 blocks a second).
@ Two guards, so that a PEAK gone wrong can never keep the transmitter quiet: PEAK does not learn in the 100 blocks
@ after a key-down (Xiegu's own SWR hold-off count, [sl,#0x1f0]: what the block holds while the base switches over is
@ no measure of the audio), and no hold lasts over 5 s (what PEAK's decay takes to undo 30 dB): then PEAK learns the
@ block as it is and the loop climbs as Xiegu's does. AM and FM send a carrier, never a pause; CW does not run the loop.
@ SPDX-License-Identifier: LGPL-2.1-or-later

    .syntax unified
    .cpu cortex-m4
    .fpu fpv4-sp-d16
    .thumb

    .section .hook, "ax"
    .global hook_branch
hook_branch:                        @ placed at 0x08027f82 (4 bytes): Xiegu's vldr s15,[sl,#0x220]; reached only with
                                    @ the error > 0 (ble.w at 0x08027f7e takes the other way)
    b.w     alc_hold

    .section .text, "ax"
    .global alc_hold
    .thumb_func
alc_hold:
    push    {r0-r3}
    vpush   {s0-s2}
    movs    r1, #0
    vmov    s0, r1                  @ the block's energy
    movw    r0, #0x4190
    movt    r0, #0x2000
    movs    r1, #128
1:  vldmia  r0!, {s1-s2}
    vmla.f32 s0, s1, s1
    vmla.f32 s0, s2, s2
    subs    r1, r1, #1
    bne     1b
    movw    r0, #0x0650             @ the CCM's clock on (RCC AHB1ENR bit 20, its reset value; Xiegu's application
    movt    r0, #0x4247             @ never touches the RCC, its bootloader sets the clocks): one store to the bit's
    movs    r1, #1                  @ bit-band alias, nothing else in the register touched, then read back before
    str     r1, [r0]                @ the first CCM access (as RF.Guru's patch_init does at start)
    ldr     r1, [r0]
    movw    r0, #0xfff0             @ PEAK at 0x1000fff0, MAGIC at +4, HELD (blocks held in a row) at +8: CCM holds
    movt    r0, #0x1000             @ anything at power-on, so they count only with MAGIC there and PEAK finite and
    movw    r2, #0xcafe             @ not negative; else both start at 0
    movt    r2, #0x600d
    ldr     r1, [r0, #4]
    cmp     r1, r2
    bne     2f
    vldr    s1, [r0]
    vsub.f32 s2, s1, s1             @ 0, or NaN for an infinite or NaN PEAK
    vcmp.f32 s2, #0
    vmrs    APSR_nzcv, fpscr
    bne     2f
    vcmpe.f32 s1, #0
    vmrs    APSR_nzcv, fpscr
    bge     3f
2:  movs    r1, #0
    vmov    s1, r1
    str     r1, [r0, #8]
    str     r2, [r0, #4]
3:  vldr    s2, decay
    vmul.f32 s1, s1, s2             @ PEAK decays
    ldr     r1, [sl, #0x1f0]        @ Xiegu's count of the 100 blocks after a key-down (its SWR hold-off): what the
    cmp     r1, #0                  @ block holds then (the switch from receive) is not learnt
    bgt     5f
    vcmpe.f32 s0, s1
    vmrs    APSR_nzcv, fpscr
    it      gt
    vmovgt.f32 s1, s0               @ and follows the block's energy up at once
5:  vstr    s1, [r0]
    vldr    s2, pause
    vmul.f32 s1, s1, s2
    ldr     r1, [r0, #8]
    vcmpe.f32 s0, s1                @ the block under PEAK - 30 dB: a pause
    vmrs    APSR_nzcv, fpscr
    bpl     6f
    adds    r1, r1, #1              @ held one block more; past HOLD_MAX in a row the hold gives up: PEAK learns the
    cmp     r1, #HOLD_MAX           @ block as it is and the loop raises as Xiegu's does (a PEAK gone wrong can never
    bls     7f                      @ keep the transmitter quiet for longer)
    vstr    s0, [r0]
6:  movs    r1, #0
7:  str     r1, [r0, #8]            @ r1 > 0: held (the flags of cmp or movs: Z clear only when held)
    cmp     r1, #0
    vpop    {s0-s2}                 @ neither pop changes the flags
    pop     {r0-r3}
    bne     4f
    vldr    s15, [sl, #0x220]       @ Xiegu's raise, as it was
    vfma.f32 s13, s12, s15
4:  b.w     XIEGU_STORE             @ 0x08027f8a: Xiegu's store of the integrator

    .equ    HOLD_MAX, 3904          @ 5 s of 781 blocks a second (0xf40: a Thumb-2 immediate)
    .align 2
decay:  .float 0.9982
pause:  .float 0.001

@ GoldCorpHF: the X6100 base V1.1.9's transmit I/Q balance, set per band by the GUI (x6100_patch.py, fix "txiq").
@
@ Every X6100 sends a mirror image of its signal on the other side of the dial (measured 2026-11-20 on 160-6 m at
@ 5 W: -42.5 to -54.5 dBc, Xiegu's own base the same): its analogue quadrature path's I and Q differ a little in
@ gain and phase, and no X6100 base corrects transmit I/Q (registers 48/49 correct the receiver only). Here, in the
@ base's DAC loop (FUN_08024528), each sample's Q is corrected before it is converted for the DAC:
@
@     Q' = Q + (G x Q + P x I) x 1e-5        G, P: register 52, gain in its low half, phase in its high half,
@                                            each signed 16-bit in units of 1e-5 (+-0.33)
@
@ which leaves I alone and cancels the image when G and P match the radio's error on that band (the GUI writes
@ the band's pair on every band change, as it does the DC offsets of registers 50/51). Register 52 is unused by
@ Xiegu's V1.1.9, gdyuldin's r13 and RF.Guru's port; the base keeps every register written over I2C in its block
@ at 0x2000c4c8 (pointer at 0x20000138), so it reads register 52 at 0x2000c598. Zero (Xiegu's base starts with
@ it zero, and a GUI that never writes it) leaves every sample exactly as Xiegu's.
@
@ Entered from 0x08026560 in place of Xiegu's vldr s15,[r5,#4] (the block's Q at r5+4), with s14 its I (loaded at
@ 0x08026552, converted after); r0, r1 and every VFP register but s15 are kept. Back at 0x08026564.
@ SPDX-License-Identifier: LGPL-2.1-or-later

    .syntax unified
    .cpu cortex-m4
    .fpu fpv4-sp-d16
    .thumb

    .section .hook, "ax"
    .global hook_branch
hook_branch:                        @ placed at 0x08026560 (4 bytes): Xiegu's vldr s15,[r5,#4]
    b.w     tx_iq

    .section .text, "ax"
    .global tx_iq
    .thumb_func
tx_iq:
    vldr    s15, [r5, #4]           @ Q, as Xiegu's instruction did
    push    {r0, r1}
    movw    r0, #0xc598
    movt    r0, #0x2000
    ldr     r0, [r0]                @ register 52: P (high half), G (low half)
    cbz     r0, 1f
    vpush   {s0-s1}
    sxth    r1, r0
    vmov    s0, r1
    vcvt.f32.s32 s0, s0             @ G x 1e5
    asrs    r1, r0, #16
    vmov    s1, r1
    vcvt.f32.s32 s1, s1             @ P x 1e5
    vmul.f32 s0, s0, s15            @ G x Q
    vfma.f32 s0, s1, s14            @ + P x I
    vldr    s1, scale
    vfma.f32 s15, s0, s1            @ Q += (G x Q + P x I) x 1e-5
    vpop    {s0-s1}
1:  pop     {r0, r1}
    b.w     XIEGU_NEXT              @ 0x08026564

    .align 2
scale:  .float 0.00001

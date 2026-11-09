// Instruction battery for the Hookshot lifter test. Hand-written, no game code.
// Rules: only x0-x7 / w0-w7 and v0-v7; never x9/x10 (the native stub's), no SP, no branches.
// Memory: base is always x0, which the harness points into a scratch buffer.
    .text
    // integer ALU
    add x1, x2, x3
    add w1, w2, w3, lsl #7
    adds x1, x2, x3, asr #13
    adds w4, w5, #4095
    sub x1, x2, x3, lsr #3
    subs x4, x5, x6
    subs w4, w5, w6, sxtb #2
    add x1, x2, w3, uxtw #3
    cmp x2, x3
    cmn w2, #17
    neg x1, x2
    negs w1, w2
    adc x1, x2, x3
    adcs w1, w2, w3
    sbc x1, x2, x3
    sbcs x4, x5, x6
    and x1, x2, x3
    and w1, w2, #0xff00ff00
    ands x1, x2, #0xfffffffffff0
    orr x1, x2, x3, ror #9
    orn w1, w2, w3
    eor x1, x2, #0x5555555555555555
    eon x4, x5, x6, lsl #1
    bic x1, x2, x3
    bics w1, w2, w3
    tst x2, #0x8000000000000000
    lsl x1, x2, x3
    lsr w1, w2, w3
    asr x1, x2, x3
    ror w1, w2, w3
    lsl x1, x2, #17
    asr w1, w2, #31
    madd x1, x2, x3, x4
    msub w1, w2, w3, w4
    smull x1, w2, w3
    umull x1, w2, w3
    smulh x1, x2, x3
    umulh x1, x2, x3
    smaddl x1, w2, w3, x4
    udiv x1, x2, x3
    sdiv w1, w2, w3
    clz x1, x2
    cls w1, w2
    rbit x1, x2
    rev x1, x2
    rev16 w1, w2
    rev32 x1, x2
    csel x1, x2, x3, eq
    csinc w1, w2, w3, lt
    csinv x1, x2, x3, hi
    csneg w1, w2, w3, ge
    cset x1, mi
    csetm w1, vs
    ccmp x2, x3, #5, ne
    ccmn w2, #3, #10, cs
    ubfx x1, x2, #13, #29
    sbfx w1, w2, #3, #9
    bfi x1, x2, #40, #12
    bfxil w1, w2, #5, #7
    extr x1, x2, x3, #23
    sxtb x1, w2
    uxth w1, w2
    sxtw x1, w2
    movz x1, #0xbeef, lsl #32
    movk x1, #0x1234, lsl #16
    movn w1, #0x42
    // loads and stores through x0
    ldr x1, [x0, #8]
    ldr w2, [x0, #12]
    ldrb w3, [x0, #3]
    ldrsb x4, [x0, #5]
    ldrh w5, [x0, #6]
    ldrsh w6, [x0, #10]
    ldrsw x7, [x0, #20]
    ldur x1, [x0, #-7]
    ldp x1, x2, [x0, #16]
    ldp w3, w4, [x0, #-8]
    ldpsw x5, x6, [x0, #24]
    ldr x1, [x0, #16]!
    ldr x2, [x0], #-24
    str x1, [x0, #8]
    str w2, [x0, #4]
    strb w3, [x0, #1]
    strh w4, [x0, #2]
    stur x5, [x0, #-3]
    stp x1, x2, [x0, #32]
    stp w3, w4, [x0, #-16]!
    str x6, [x0], #40
    ldr x1, [x0, x2, lsl #3]
    ldr q1, [x0, #32]
    str q2, [x0, #-32]
    ldp q3, q4, [x0]
    ldr d5, [x0, #8]
    str s6, [x0, #4]
    // scalar floating point
    fadd s1, s2, s3
    fsub d1, d2, d3
    fmul s1, s2, s3
    fdiv d1, d2, d3
    fmadd s1, s2, s3, s4
    fmsub d1, d2, d3, d4
    fnmadd s1, s2, s3, s4
    fnmul d1, d2, d3
    fsqrt s1, s2
    fsqrt d1, d2
    fabs d1, d2
    fneg s1, s2
    fmax s1, s2, s3
    fminnm d1, d2, d3
    fcmp s2, s3
    fcmpe d2, #0.0
    fccmp s2, s3, #4, gt
    fcsel d1, d2, d3, le
    fcvt d1, s2
    fcvt s1, d2
    fcvt h1, s2
    fcvt s1, h2
    fcvtzs w1, s2
    fcvtzu x1, d2
    fcvtzs x1, d2, #10
    fcvtas w1, d2
    fcvtms x1, s2
    scvtf s1, x2
    ucvtf d1, w2
    scvtf d1, w2, #5
    frintn s1, s2
    frintm d1, d2
    frintp s1, s2
    frintz d1, d2
    frinta s1, s2
    frintx d1, d2
    frinti s1, s2
    fmov s1, #1.5
    fmov d1, #-0.125
    fmov x1, d2
    fmov d1, x2
    fmov w1, s2
    // NEON
    add v1.4s, v2.4s, v3.4s
    sub v1.8h, v2.8h, v3.8h
    mul v1.16b, v2.16b, v3.16b
    mla v1.4s, v2.4s, v3.4s
    and v1.16b, v2.16b, v3.16b
    orr v1.16b, v2.16b, v3.16b
    eor v1.16b, v2.16b, v3.16b
    bic v1.16b, v2.16b, v3.16b
    bsl v1.16b, v2.16b, v3.16b
    fadd v1.4s, v2.4s, v3.4s
    fmul v1.2d, v2.2d, v3.2d
    fmla v1.4s, v2.4s, v3.4s
    fmla v1.4s, v2.4s, v3.s[2]
    fdiv v1.4s, v2.4s, v3.4s
    fsqrt v1.2d, v2.2d
    fmaxnm v1.4s, v2.4s, v3.4s
    faddp v1.4s, v2.4s, v3.4s
    fcmgt v1.4s, v2.4s, v3.4s
    fcvtzs v1.4s, v2.4s
    scvtf v1.4s, v2.4s
    fcvtn v1.4h, v2.4s
    fcvtl v1.2d, v2.2s
    frecpe v1.4s, v2.4s
    frsqrte v1.4s, v2.4s
    frecps v1.4s, v2.4s, v3.4s
    dup v1.4s, w2
    dup v1.8h, v2.h[5]
    ins v1.s[3], w2
    ins v1.d[0], v2.d[1]
    umov w1, v2.b[9]
    smov x1, v2.h[3]
    zip1 v1.16b, v2.16b, v3.16b
    zip2 v1.4s, v2.4s, v3.4s
    uzp1 v1.8h, v2.8h, v3.8h
    trn2 v1.4s, v2.4s, v3.4s
    ext v1.16b, v2.16b, v3.16b, #7
    tbl v1.16b, {v2.16b, v3.16b}, v4.16b
    cmeq v1.4s, v2.4s, v3.4s
    cmgt v1.8h, v2.8h, v3.8h
    cmhi v1.16b, v2.16b, v3.16b
    sshr v1.4s, v2.4s, #7
    ushr v1.2d, v2.2d, #33
    shl v1.8h, v2.8h, #3
    ushl v1.4s, v2.4s, v3.4s
    sshll v1.4s, v2.4h, #2
    shrn v1.8b, v2.8h, #4
    addv s1, v2.4s
    uaddlv h1, v2.16b
    umaxv b1, v2.16b
    saddl v1.4s, v2.4h, v3.4h
    umull v1.2d, v2.2s, v3.2s
    sqadd v1.8h, v2.8h, v3.8h
    uqsub v1.16b, v2.16b, v3.16b
    sqdmulh v1.4s, v2.4s, v3.4s
    xtn v1.8b, v2.8h
    sqxtn v1.4h, v2.4s
    uqxtn v1.2s, v2.2d
    rev64 v1.16b, v2.16b
    cnt v1.16b, v2.16b
    abs v1.4s, v2.4s
    neg v1.2d, v2.2d
    smax v1.4s, v2.4s, v3.4s
    umin v1.8h, v2.8h, v3.8h
    uabd v1.16b, v2.16b, v3.16b
    movi v1.4s, #0x7f, lsl #8
    mvni v1.8h, #0x12
    not v1.16b, v2.16b
    ld1 {v1.16b}, [x0]
    ld1 {v1.4s, v2.4s}, [x0], #32
    ld1r {v3.4s}, [x0]
    ld2 {v1.4s, v2.4s}, [x0]
    st1 {v1.16b}, [x0]
    st1 {v1.2d, v2.2d}, [x0], #32
    st3 {v1.8b, v2.8b, v3.8b}, [x0]

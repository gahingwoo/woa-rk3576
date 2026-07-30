//
//  SMC call shim for ARM64 Windows kernel drivers.
//
//  The WDK has no intrinsic for the SMC instruction, so the SMCCC register
//  marshalling has to be written out. Argument and return placement follow
//  SMCCC: x0..x7 in, x0..x3 out.
//
//  Derived from the ARM Limited ArmSmcLib assembly stub (BSD).
//

    EXPORT RkArmCallSmc
    AREA   s_RkArmCallSmc, CODE, READONLY

RkArmCallSmc
    // Preserve the argument-block pointer; the stack stays quad-word aligned.
    str   x0, [sp, #-16]!

    ldp   x6, x7, [x0, #48]
    ldp   x4, x5, [x0, #32]
    ldp   x2, x3, [x0, #16]
    ldp   x0, x1, [x0, #0]

    smc   #0

    ldr   x9, [sp], #16

    // An SMC returns at most four values; x4..x7 are not written back.
    stp   x2, x3, [x9, #16]
    stp   x0, x1, [x9, #0]

    mov   x0, x9

    ret

    END

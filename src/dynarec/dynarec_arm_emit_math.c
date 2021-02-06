#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <pthread.h>
#include <errno.h>

#include "debug.h"
#include "box86context.h"
#include "dynarec.h"
#include "emu/x86emu_private.h"
#include "emu/x86run_private.h"
#include "x86run.h"
#include "x86emu.h"
#include "box86stack.h"
#include "callback.h"
#include "emu/x86run_private.h"
#include "x86trace.h"
#include "dynarec_arm.h"
#include "dynarec_arm_private.h"
#include "arm_printer.h"
#include "../tools/bridge_private.h"

#include "dynarec_arm_functions.h"
#include "dynarec_arm_helper.h"

// emit ADD32 instruction, from s1 , s2, store result in s1 using s3 and s4 as scratch
void emit_add32(dynarec_arm_t* dyn, int ninst, int s1, int s2, int s3, int s4)
{
    IFX(X_PEND) {
        STR_IMM9(s1, xEmu, offsetof(x86emu_t, op1));
        STR_IMM9(s2, xEmu, offsetof(x86emu_t, op2));
        SET_DF(s3, d_add32);
    } else IFX(X_ALL) {
        SET_DFNONE(s3);
    }
    IFX(X_AF) {
        ORR_REG_LSL_IMM5(s3, s1, s2, 0);    // s3 = op1 | op2
        AND_REG_LSL_IMM5(s4, s1, s2, 0);    // s4 = op1 & op2
    }
    IFX(X_ALL) {
        ADDS_REG_LSL_IMM5(s1, s1, s2, 0);
    } else {
        ADD_REG_LSL_IMM5(s1, s1, s2, 0);
    }
    IFX(X_PEND) {
        STR_IMM9(s1, xEmu, offsetof(x86emu_t, res));
    }
    IFX(X_AF) {
        BIC_REG_LSL_IMM5(s3, s3, s1, 0);   // s3 = (op1 | op2) & ~ res
        ORR_REG_LSL_IMM5(s3, s3, s4, 0);   // s4 = (op1 & op2) | ((op1 | op2) & ~ res)
        MOV_REG_LSR_IMM5(s4, s3, 3);
        BFI(xFlags, s4, F_AF, 1);    // AF: bc & 0x08
    }
    IFX(X_ZF|X_CF) {
        BIC_IMM8(xFlags, xFlags, (1<<F_ZF)|(1<<F_CF), 0);
    }
    IFX(X_ZF) {
        ORR_IMM8_COND(cEQ, xFlags, xFlags, 1<<F_ZF, 0);
    }
    IFX(X_CF) {
        ORR_IMM8_COND(cCS, xFlags, xFlags, 1<<F_CF, 0);
    }
    IFX(X_OF) {
        ORR_IMM8_COND(cVS, xFlags, xFlags, 0b10, 0x0b);
        BIC_IMM8_COND(cVC, xFlags, xFlags, 0b10, 0x0b);
    }
    IFX(X_SF) {
        MOV_REG_LSR_IMM5(s3, s1, 31);
        BFI(xFlags, s3, F_SF, 1);
    }
    IFX(X_PF) {
        emit_pf(dyn, ninst, s1, s3, s4);
    }
}

// emit ADD32 instruction, from s1 , constant c, store result in s1 using s3 and s4 as scratch
void emit_add32c(dynarec_arm_t* dyn, int ninst, int s1, int32_t c, int s3, int s4)
{
    if(s1==xESP && (!dyn->insts || dyn->insts[ninst].x86.need_flags==X_PEND))
    {
        // special case when doing math on ESP and only PEND is needed: ignoring it!
        if(c>=0 && c<256) {
            ADD_IMM8(s1, s1, c);
        } else {
            MOV32(s3, c);
            ADD_REG_LSL_IMM5(s1, s1, s3, 0);
        }
        return;
    }
    IFX(X_PEND) {
        MOV32(s3, c);
        STR_IMM9(s1, xEmu, offsetof(x86emu_t, op1));
        STR_IMM9(s3, xEmu, offsetof(x86emu_t, op2));
        SET_DF(s4, d_add32);
    } else IFX(X_ALL) {
        SET_DFNONE(s4);
    }
    IFX(X_AF) {
        if(c>=0 && c<256) {
            ORR_IMM8(s3, s1, c, 0);             // s3 = op1 | op2
            AND_IMM8(s4, s1, c);                // s4 = op1 & op2
        } else {
            IFX(X_PEND) {} else {MOV32(s3, c);}
            MOV_REG(s4, s3);
            ORR_REG_LSL_IMM5(s3, s1, s3, 0);
            AND_REG_LSL_IMM5(s4, s1, s4, 0);
            PUSH(xSP, 1<<s3);
        }
    }
    if(c>=0 && c<256) {
        IFX(X_ALL) {
            ADDS_IMM8(s1, s1, c);
        } else {
            ADD_IMM8(s1, s1, c);
        }
    } else {
        IFXN(X_PEND, X_AF) {} else {MOV32(s3, c);}
        IFX(X_ALL) {
            ADDS_REG_LSL_IMM5(s1, s1, s3, 0);
        } else {
            ADD_REG_LSL_IMM5(s1, s1, s3, 0);
        }
    }
    IFX(X_PEND) {
        STR_IMM9(s1, xEmu, offsetof(x86emu_t, res));
    }
    IFX(X_AF) {
        if(c<0 || c>=256) {
            POP(xSP, 1<<s3);
        }
        BIC_REG_LSL_IMM5(s3, s3, s1, 0);   // s3 = (op1 | op2) & ~ res
        ORR_REG_LSL_IMM5(s3, s3, s4, 0);   // s4 = (op1 & op2) | ((op1 | op2) & ~ res)
        MOV_REG_LSR_IMM5(s4, s3, 3);
        BFI(xFlags, s4, F_AF, 1);    // AF: bc & 0x08
    }
    IFX(X_ZF|X_CF) {
        BIC_IMM8(xFlags, xFlags, (1<<F_ZF)|(1<<F_CF), 0);
    }
    IFX(X_ZF) {
        ORR_IMM8_COND(cEQ, xFlags, xFlags, 1<<F_ZF, 0);
    }
    IFX(X_CF) {
        ORR_IMM8_COND(cCS, xFlags, xFlags, 1<<F_CF, 0);
    }
    IFX(X_OF) {
        ORR_IMM8_COND(cVS, xFlags, xFlags, 0b10, 0x0b);
        BIC_IMM8_COND(cVC, xFlags, xFlags, 0b10, 0x0b);
    }
    IFX(X_SF) {
        MOV_REG_LSR_IMM5(s3, s1, 31);
        BFI(xFlags, s3, F_SF, 1);
    }
    IFX(X_PF) {
        emit_pf(dyn, ninst, s1, s3, s4);
    }
}

// emit SUB32 instruction, from s1 , s2, store result in s1 using s3 and s4 as scratch
void emit_sub32(dynarec_arm_t* dyn, int ninst, int s1, int s2, int s3, int s4)
{
    IFX(X_PEND) {
        STR_IMM9(s1, xEmu, offsetof(x86emu_t, op1));
        STR_IMM9(s2, xEmu, offsetof(x86emu_t, op2));
        SET_DF(s3, d_sub32);
    } else IFX(X_ALL) {
        SET_DFNONE(s3);
    }
    IFX(X_AF) {
        MVN_REG_LSL_IMM5(s3, s1, 0);
        ORR_REG_LSL_IMM5(s3, s3, s2, 0);    // s3 = ~op1 | op2
        BIC_REG_LSL_IMM5(s4, s2, s1, 0);    // s4 = ~op1 & op2
    }
    IFX(X_ALL) {
        SUBS_REG_LSL_IMM5(s1, s1, s2, 0);
    } else {
        SUB_REG_LSL_IMM5(s1, s1, s2, 0);
    }
    IFX(X_PEND) {
        STR_IMM9(s1, xEmu, offsetof(x86emu_t, res));
    }
    IFX(X_AF) {
        AND_REG_LSL_IMM5(s3, s3, s1, 0);   // s3 = (~op1 | op2) & res
        ORR_REG_LSL_IMM5(s3, s3, s4, 0);   // s4 = (~op1 & op2) | ((~op1 | op2) & res)
        MOV_REG_LSR_IMM5(s4, s3, 3);
        BFI(xFlags, s4, F_AF, 1);    // AF: bc & 0x08
    }
    IFX(X_ZF|X_CF) {
        BIC_IMM8(xFlags, xFlags, (1<<F_ZF)|(1<<F_CF), 0);
    }
    IFX(X_ZF) {
        ORR_IMM8_COND(cEQ, xFlags, xFlags, 1<<F_ZF, 0);
    }
    IFX(X_CF) {
        // inverted carry
        ORR_IMM8_COND(cCC, xFlags, xFlags, 1<<F_CF, 0);
    }
    IFX(X_OF) {
        ORR_IMM8_COND(cVS, xFlags, xFlags, 0b10, 0x0b);
        BIC_IMM8_COND(cVC, xFlags, xFlags, 0b10, 0x0b);
    }
    IFX(X_SF) {
        MOV_REG_LSR_IMM5(s3, s1, 31);
        BFI(xFlags, s3, F_SF, 1);
    }
    IFX(X_PF) {
        emit_pf(dyn, ninst, s1, s3, s4);
    }
}

// emit SUB32 instruction, from s1 , constant c, store result in s1 using s3 and s4 as scratch
void emit_sub32c(dynarec_arm_t* dyn, int ninst, int s1, int32_t c, int s3, int s4)
{
    if(s1==xESP && (!dyn->insts || dyn->insts[ninst].x86.need_flags==X_PEND))
    {
        // special case when doing math on ESP and only PEND is needed: ignoring it!
        if(c>=0 && c<256) {
            SUB_IMM8(s1, s1, c);
        } else {
            MOV32(s3, c);
            SUB_REG_LSL_IMM5(s1, s1, s3, 0);
        }
        return;
    }
    IFX(X_PEND) {
        MOV32(s3, c);
        STR_IMM9(s1, xEmu, offsetof(x86emu_t, op1));
        STR_IMM9(s3, xEmu, offsetof(x86emu_t, op2));
        SET_DF(s4, d_sub32);
    } else IFX(X_ALL) {
        SET_DFNONE(s4);
    }
    IFX(X_AF) {
        if(c>=0 && c<256) {
            MVN_REG_LSL_IMM5(s3, s1, 0);
            AND_IMM8(s4, s3, c);                // s4 = ~op1 & op2
            ORR_IMM8(s3, s3, c, 0);             // s3 = ~op1 | op2
        } else {
            IFX(X_PEND) {} else {MOV32(s3, c);}
            MVN_REG_LSL_IMM5(s4, s1, 0);
            ORR_REG_LSL_IMM5(s3, s4, s3, 0);
            MOV32(s4, c);
            BIC_REG_LSL_IMM5(s4, s4, s1, 0);
            PUSH(xSP, 1<<s3);
        }
    }
    if(c>=0 && c<256) {
        IFX(X_ALL) {
            SUBS_IMM8(s1, s1, c);
        } else {
            SUB_IMM8(s1, s1, c);
        }
    } else {
        IFXN(X_PEND, X_AF) {} else {MOV32(s3, c);}
        IFX(X_ALL) {
            SUBS_REG_LSL_IMM5(s1, s1, s3, 0);
        } else {
            SUB_REG_LSL_IMM5(s1, s1, s3, 0);
        }
    }
    IFX(X_PEND) {
        STR_IMM9(s1, xEmu, offsetof(x86emu_t, res));
    }
    IFX(X_AF) {
        if(c<0 || c>=256) {
            POP(xSP, 1<<s3);
        }
        AND_REG_LSL_IMM5(s3, s3, s1, 0);   // s3 = (~op1 | op2) & res
        ORR_REG_LSL_IMM5(s3, s3, s4, 0);   // s4 = (~op1 & op2) | ((~op1 | op2) & ~ res)
        MOV_REG_LSR_IMM5(s4, s3, 3);
        BFI(xFlags, s4, F_AF, 1);    // AF: bc & 0x08
    }
    IFX(X_ZF|X_CF) {
        BIC_IMM8(xFlags, xFlags, (1<<F_ZF)|(1<<F_CF), 0);
    }
    IFX(X_ZF) {
        ORR_IMM8_COND(cEQ, xFlags, xFlags, 1<<F_ZF, 0);
    }
    IFX(X_CF) {
        // Inverted carry
        ORR_IMM8_COND(cCC, xFlags, xFlags, 1<<F_CF, 0);
    }
    IFX(X_OF) {
        ORR_IMM8_COND(cVS, xFlags, xFlags, 0b10, 0x0b);
        BIC_IMM8_COND(cVC, xFlags, xFlags, 0b10, 0x0b);
    }
    IFX(X_SF) {
        MOV_REG_LSR_IMM5(s3, s1, 31);
        BFI(xFlags, s3, F_SF, 1);
    }
    IFX(X_PF) {
        emit_pf(dyn, ninst, s1, s3, s4);
    }
}

// emit ADD8 instruction, from s1 , s2, store result in s1 using s3 and s4 as scratch, with save_s4 is s4 need to be saved
void emit_add8(dynarec_arm_t* dyn, int ninst, int s1, int s2, int s3, int s4, int save_s4)
{
    IFX(X_PEND) {
        STR_IMM9(s1, xEmu, offsetof(x86emu_t, op1));
        STR_IMM9(s2, xEmu, offsetof(x86emu_t, op2));
        SET_DF(s3, d_add8);
    } else IFX(X_ALL) {
        SET_DFNONE(s3);
    }
    IFX(X_AF|X_OF|X_PF){if(save_s4) {PUSH(xSP, 1<<s4);}}
    IFX(X_AF | X_OF) {
        ORR_REG_LSL_IMM5(s3, s1, s2, 0);    // s3 = op1 | op2
        AND_REG_LSL_IMM5(s4, s1, s2, 0);    // s4 = op1 & op2
    }
    ADD_REG_LSL_IMM5(s1, s1, s2, 0);
    IFX(X_AF|X_OF) {
        BIC_REG_LSL_IMM5(s3, s3, s1, 0);   // s3 = (op1 | op2) & ~ res
        ORR_REG_LSL_IMM5(s3, s3, s4, 0);   // s3 = (op1 & op2) | ((op1 | op2) & ~ res)
        IFX(X_AF) {
            MOV_REG_LSR_IMM5(s4, s3, 3);
            BFI(xFlags, s4, F_AF, 1);    // AF: bc & 0x08
        }
        IFX(X_OF) {
            MOV_REG_LSR_IMM5(s4, s3, 6);
            XOR_REG_LSR_IMM8(s4, s4, s4, 1);
            BFI(xFlags, s4, F_OF, 1);    // OF: ((bc >> 6) ^ ((bc>>6)>>1)) & 1
        }
    }
    IFX(X_CF) {
        MOV_REG_LSR_IMM5(s3, s1, 8);
        BFI(xFlags, s3, F_CF, 1);
    }
    IFX(X_PEND) {
        STR_IMM9(s1, xEmu, offsetof(x86emu_t, res));
    }
    IFX(X_ZF) {
        ANDS_IMM8(s1, s1, 0xff);
        ORR_IMM8_COND(cEQ, xFlags, xFlags, 1<<F_ZF, 0);
        BIC_IMM8_COND(cNE, xFlags, xFlags, 1<<F_ZF, 0);
    }
    IFX(X_SF) {
        MOV_REG_LSR_IMM5(s3, s1, 7);
        BFI(xFlags, s3, F_SF, 1);
    }
    IFX(X_PF) {
        emit_pf(dyn, ninst, s1, s3, s4);
    }
    IFX(X_AF|X_OF|X_PF){if(save_s4) {POP(xSP, 1<<s4);}}
}

// emit ADD8 instruction, from s1 , const c, store result in s1 using s3 and s4 as scratch, with save_s4 is s4 need to be saved
void emit_add8c(dynarec_arm_t* dyn, int ninst, int s1, int c, int s3, int s4)
{
    IFX(X_PEND) {
        MOVW(s3, c&0xff);
        STR_IMM9(s1, xEmu, offsetof(x86emu_t, op1));
        STR_IMM9(s3, xEmu, offsetof(x86emu_t, op2));
        SET_DF(s3, d_add8);
    } else IFX(X_ALL) {
        SET_DFNONE(s3);
    }
    IFX(X_AF | X_OF) {
        ORR_IMM8(s3, s1, c, 0);     // s3 = op1 | op2
        AND_IMM8(s4, s1, c);        // s4 = op1 & op2
    }
    ADD_IMM8(s1, s1, c);

    IFX(X_AF|X_OF) {
        BIC_REG_LSL_IMM5(s3, s3, s1, 0);   // s3 = (op1 | op2) & ~ res
        ORR_REG_LSL_IMM5(s3, s3, s4, 0);   // s4 = (op1 & op2) | ((op1 | op2) & ~ res)
        IFX(X_AF) {
            MOV_REG_LSR_IMM5(s4, s3, 3);
            BFI(xFlags, s4, F_AF, 1);    // AF: bc & 0x08
        }
        IFX(X_OF) {
            MOV_REG_LSR_IMM5(s4, s3, 6);
            XOR_REG_LSR_IMM8(s4, s4, s4, 1);
            BFI(xFlags, s4, F_OF, 1);    // OF: ((bc >> 6) ^ ((bc>>6)>>1)) & 1
        }
    }
    IFX(X_CF) {
        MOV_REG_LSR_IMM5(s3, s1, 8);
        BFI(xFlags, s3, F_CF, 1);
    }
    IFX(X_PEND) {
        STR_IMM9(s1, xEmu, offsetof(x86emu_t, res));
    }
    IFX(X_ZF) {
        ANDS_IMM8(s1, s1, 0xff);
        ORR_IMM8_COND(cEQ, xFlags, xFlags, 1<<F_ZF, 0);
        BIC_IMM8_COND(cNE, xFlags, xFlags, 1<<F_ZF, 0);
    }
    IFX(X_SF) {
        MOV_REG_LSR_IMM5(s3, s1, 7);
        BFI(xFlags, s3, F_SF, 1);
    }
    IFX(X_PF) {
        emit_pf(dyn, ninst, s1, s3, s4);
    }
}

// emit SUB8 instruction, from s1 , s2, store result in s1 using s3 and s4 as scratch, with save_s4 is s4 need to be saved
void emit_sub8(dynarec_arm_t* dyn, int ninst, int s1, int s2, int s3, int s4, int save_s4)
{
    IFX(X_PEND) {
        STR_IMM9(s1, xEmu, offsetof(x86emu_t, op1));
        STR_IMM9(s2, xEmu, offsetof(x86emu_t, op2));
        SET_DF(s3, d_sub8);
    } else IFX(X_ALL) {
        SET_DFNONE(s3);
    }
    IFX(X_AF|X_OF|X_CF|X_PF){if(save_s4) {PUSH(xSP, 1<<s4);}}
    IFX(X_AF|X_OF|X_CF) {
        MVN_REG_LSL_IMM5(s3, s1, 0);
        ORR_REG_LSL_IMM5(s3, s3, s2, 0);    // s3 = ~op1 | op2
        BIC_REG_LSL_IMM5(s4, s2, s1, 0);    // s4 = ~op1 & op2
    }

    SUB_REG_LSL_IMM5(s1, s1, s2, 0);
    IFX(X_PEND) {
        STR_IMM9(s1, xEmu, offsetof(x86emu_t, res));
    }
    IFX(X_AF|X_OF|X_CF) {
        AND_REG_LSL_IMM5(s3, s3, s1, 0);   // s3 = (~op1 | op2) & res
        ORR_REG_LSL_IMM5(s3, s3, s4, 0);   // s3 = (~op1 & op2) | ((~op1 | op2) & res)
        IFX(X_CF) {
            MOV_REG_LSR_IMM5(s4, s3, 7);
            BFI(xFlags, s4, F_CF, 1);    // CF : bc & 0x80
        }
        IFX(X_AF) {
            MOV_REG_LSR_IMM5(s4, s3, 3);
            BFI(xFlags, s4, F_AF, 1);    // AF: bc & 0x08
        }
        IFX(X_OF) {
            MOV_REG_LSR_IMM5(s4, s3, 6);
            XOR_REG_LSR_IMM8(s4, s4, s4, 1);
            BFI(xFlags, s4, F_OF, 1);    // OF: ((bc >> 6) ^ ((bc>>6)>>1)) & 1
        }
    }
    IFX(X_ZF) {
        ANDS_IMM8(s1, s1, 0xff);
        ORR_IMM8_COND(cEQ, xFlags, xFlags, 1<<F_ZF, 0);
        BIC_IMM8_COND(cNE, xFlags, xFlags, 1<<F_ZF, 0);
    }
    IFX(X_SF) {
        MOV_REG_LSR_IMM5(s3, s1, 7);
        BFI(xFlags, s3, F_SF, 1);
    }
    IFX(X_PF) {
        emit_pf(dyn, ninst, s1, s3, s4);
    }
    IFX(X_AF|X_OF|X_CF|X_PF){if(save_s4) {POP(xSP, 1<<s4);}}
}

// emit SUB8 instruction, from s1 , constant c, store result in s1 using s3 and s4 as scratch
void emit_sub8c(dynarec_arm_t* dyn, int ninst, int s1, int c, int s3, int s4)
{
    IFX(X_PEND) {
        MOVW(s3, c&0xff);
        STR_IMM9(s1, xEmu, offsetof(x86emu_t, op1));
        STR_IMM9(s3, xEmu, offsetof(x86emu_t, op2));
        SET_DF(s3, d_sub8);
    } else IFX(X_ALL) {
        SET_DFNONE(s3);
    }
    IFX(X_AF|X_OF|X_CF) {
        MVN_REG_LSL_IMM5(s3, s1, 0);
        MOVW(s4, c&0xff);
        ORR_IMM8(s3, s3, c, 0);             // s3 = ~op1 | op2
        BIC_REG_LSL_IMM5(s4, s4, s1, 0);    // s4 = ~op1 & op2
    }
    SUB_IMM8(s1, s1, c);
    IFX(X_PEND) {
        STR_IMM9(s1, xEmu, offsetof(x86emu_t, res));
    }
    IFX(X_AF|X_OF|X_CF) {
        AND_REG_LSL_IMM5(s3, s3, s1, 0);   // s3 = (~op1 | op2) & res
        ORR_REG_LSL_IMM5(s3, s3, s4, 0);   // s3 = (~op1 & op2) | ((~op1 | op2) & res)
        IFX(X_CF) {
            MOV_REG_LSR_IMM5(s4, s3, 7);
            BFI(xFlags, s4, F_CF, 1);    // CF : bc & 0x80
        }
        IFX(X_AF) {
            MOV_REG_LSR_IMM5(s4, s3, 3);
            BFI(xFlags, s4, F_AF, 1);    // AF: bc & 0x08
        }
        IFX(X_OF) {
            MOV_REG_LSR_IMM5(s4, s3, 6);
            XOR_REG_LSR_IMM8(s4, s4, s4, 1);
            BFI(xFlags, s4, F_OF, 1);    // OF: ((bc >> 6) ^ ((bc>>6)>>1)) & 1
        }
    }
    IFX(X_ZF) {
        ANDS_IMM8(s1, s1, 0xff);
        ORR_IMM8_COND(cEQ, xFlags, xFlags, 1<<F_ZF, 0);
        BIC_IMM8_COND(cNE, xFlags, xFlags, 1<<F_ZF, 0);
    }
    IFX(X_SF) {
        MOV_REG_LSR_IMM5(s3, s1, 7);
        BFI(xFlags, s3, F_SF, 1);
    }
    IFX(X_PF) {
        emit_pf(dyn, ninst, s1, s3, s4);
    }
}

// emit ADD16 instruction, from s1 , s2, store result in s1 using s3 and s4 as scratch, with save_s4 is s4 need to be saved
void emit_add16(dynarec_arm_t* dyn, int ninst, int s1, int s2, int s3, int s4, int save_s4)
{
    IFX(X_PEND) {
        STR_IMM9(s1, xEmu, offsetof(x86emu_t, op1));
        STR_IMM9(s2, xEmu, offsetof(x86emu_t, op2));
        SET_DF(s3, d_add16);
    } else IFX(X_ALL) {
        SET_DFNONE(s3);
    }
    IFX(X_AF|X_OF|X_PF){if(save_s4) {PUSH(xSP, 1<<s4);}}
    IFX(X_AF | X_OF) {
        ORR_REG_LSL_IMM5(s3, s1, s2, 0);    // s3 = op1 | op2
        AND_REG_LSL_IMM5(s4, s1, s2, 0);    // s4 = op1 & op2
    }
    ADD_REG_LSL_IMM5(s1, s1, s2, 0);

    IFX(X_AF|X_OF) {
        BIC_REG_LSL_IMM5(s3, s3, s1, 0);   // s3 = (op1 | op2) & ~ res
        ORR_REG_LSL_IMM5(s3, s3, s4, 0);   // s3 = (op1 & op2) | ((op1 | op2) & ~ res)
        IFX(X_AF) {
            MOV_REG_LSR_IMM5(s4, s3, 3);
            BFI(xFlags, s4, F_AF, 1);    // AF: bc & 0x08
        }
        IFX(X_OF) {
            MOV_REG_LSR_IMM5(s4, s3, 14);
            XOR_REG_LSR_IMM8(s4, s4, s4, 1);
            BFI(xFlags, s4, F_OF, 1);    // OF: ((bc >> 14) ^ ((bc>>14)>>1)) & 1
        }
    }
    IFX(X_CF) {
        MOV_REG_LSR_IMM5(s3, s1, 16);
        BFI(xFlags, s3, F_CF, 1);
    }
    IFX(X_PEND) {
        STR_IMM9(s1, xEmu, offsetof(x86emu_t, res));
    }
    IFX(X_ZF) {
        UXTH(s1, s1, 0);
        TSTS_REG_LSL_IMM5(s1, s1, 0);
        ORR_IMM8_COND(cEQ, xFlags, xFlags, 1<<F_ZF, 0);
        BIC_IMM8_COND(cNE, xFlags, xFlags, 1<<F_ZF, 0);
    }
    IFX(X_SF) {
        MOV_REG_LSR_IMM5(s3, s1, 15);
        BFI(xFlags, s3, F_SF, 1);
    }
    IFX(X_PF) {
        emit_pf(dyn, ninst, s1, s3, s4);
    }
    IFX(X_AF|X_OF|X_PF){if(save_s4) {POP(xSP, 1<<s4);}}
}

// emit ADD16 instruction, from s1 , const c, store result in s1 using s3 and s4 as scratch, with save_s4 is s4 need to be saved
void emit_add16c(dynarec_arm_t* dyn, int ninst, int s1, int c, int s3, int s4)
{
    IFX(X_PEND) {
        MOVW(s3, c);
        STR_IMM9(s1, xEmu, offsetof(x86emu_t, op1));
        STR_IMM9(s3, xEmu, offsetof(x86emu_t, op2));
        SET_DF(s4, d_add16);
    } else IFX(X_ALL) {
        SET_DFNONE(s4);
    }
    IFX(X_AF | X_OF) {
        MOV_REG(s4, s1);
    }
    if(c>=0 && c<256) {
        ADD_IMM8(s1, s1, c);
    } else {
        IFX(X_PEND) {} else {MOVW(s3, c);}
        ADD_REG_LSL_IMM5(s1, s1, s3, 0);
    }

    IFX(X_AF|X_OF) {
        if(c>=0 && c<256) {
            ORR_IMM8(s3, s4, c, 0);     // s3 = op1 | op2
            AND_IMM8(s4, s4, c);        // s4 = op1 & op2
        } else {
            ORR_REG_LSL_IMM5(s3, s3, s4, 0);    // s3 = op1 | op2
            PUSH(xSP, 1<<s3);
            MOVW(s3, c);
            AND_REG_LSL_IMM5(s4, s4, s3, 0);    // s4 = op1 & op2
            POP(xSP, 1<<s3);
        }

        BIC_REG_LSL_IMM5(s3, s3, s1, 0);   // s3 = (op1 | op2) & ~ res
        ORR_REG_LSL_IMM5(s3, s3, s4, 0);   // s3 = (op1 & op2) | ((op1 | op2) & ~ res)
        IFX(X_AF) {
            MOV_REG_LSR_IMM5(s4, s3, 3);
            BFI(xFlags, s4, F_AF, 1);    // AF: bc & 0x08
        }
        IFX(X_OF) {
            MOV_REG_LSR_IMM5(s4, s3, 14);
            XOR_REG_LSR_IMM8(s4, s4, s4, 1);
            BFI(xFlags, s4, F_OF, 1);    // OF: ((bc >> 14) ^ ((bc>>14)>>1)) & 1
        }
    }
    IFX(X_CF) {
        MOV_REG_LSR_IMM5(s3, s1, 16);
        BFI(xFlags, s3, F_CF, 1);
    }
    IFX(X_PEND) {
        STR_IMM9(s1, xEmu, offsetof(x86emu_t, res));
    }
    IFX(X_ZF) {
        UXTH(s1, s1, 0);
        TSTS_REG_LSL_IMM5(s1, s1, 0);
        ORR_IMM8_COND(cEQ, xFlags, xFlags, 1<<F_ZF, 0);
        BIC_IMM8_COND(cNE, xFlags, xFlags, 1<<F_ZF, 0);
    }
    IFX(X_SF) {
        MOV_REG_LSR_IMM5(s3, s1, 15);
        BFI(xFlags, s3, F_SF, 1);
    }
    IFX(X_PF) {
        emit_pf(dyn, ninst, s1, s3, s4);
    }
}

// emit SUB16 instruction, from s1 , s2, store result in s1 using s3 and s4 as scratch, with save_s4 is s4 need to be saved
void emit_sub16(dynarec_arm_t* dyn, int ninst, int s1, int s2, int s3, int s4, int save_s4)
{
    IFX(X_PEND) {
        STR_IMM9(s1, xEmu, offsetof(x86emu_t, op1));
        STR_IMM9(s2, xEmu, offsetof(x86emu_t, op2));
        SET_DF(s3, d_sub16);
    } else IFX(X_ALL) {
        SET_DFNONE(s3);
    }
    IFX(X_AF|X_OF|X_CF|X_PF){if(save_s4) {PUSH(xSP, 1<<s4);}}
    IFX(X_AF|X_OF|X_CF) {
        MVN_REG_LSL_IMM5(s3, s1, 0);
        ORR_REG_LSL_IMM5(s3, s3, s2, 0);    // s3 = ~op1 | op2
        BIC_REG_LSL_IMM5(s4, s2, s1, 0);    // s4 = ~op1 & op2
    }

    SUB_REG_LSL_IMM5(s1, s1, s2, 0);
    IFX(X_PEND) {
        STR_IMM9(s1, xEmu, offsetof(x86emu_t, res));
    }
    IFX(X_AF|X_OF|X_CF) {
        AND_REG_LSL_IMM5(s3, s3, s1, 0);   // s3 = (~op1 | op2) & res
        ORR_REG_LSL_IMM5(s3, s3, s4, 0);   // s3 = (~op1 & op2) | ((~op1 | op2) & res)
        IFX(X_CF) {
            MOV_REG_LSR_IMM5(s4, s3, 15);
            BFI(xFlags, s4, F_CF, 1);    // CF : bc & 0x8000
        }
        IFX(X_AF) {
            MOV_REG_LSR_IMM5(s4, s3, 3);
            BFI(xFlags, s4, F_AF, 1);    // AF: bc & 0x08
        }
        IFX(X_OF) {
            MOV_REG_LSR_IMM5(s4, s3, 14);
            XOR_REG_LSR_IMM8(s4, s4, s4, 1);
            BFI(xFlags, s4, F_OF, 1);    // OF: ((bc >> 14) ^ ((bc>>14)>>1)) & 1
        }
    }
    IFX(X_ZF) {
        UXTH(s1, s1, 0);
        TSTS_REG_LSL_IMM5(s1, s1, 0);
        ORR_IMM8_COND(cEQ, xFlags, xFlags, 1<<F_ZF, 0);
        BIC_IMM8_COND(cNE, xFlags, xFlags, 1<<F_ZF, 0);
    }
    IFX(X_SF) {
        MOV_REG_LSR_IMM5(s3, s1, 15);
        BFI(xFlags, s3, F_SF, 1);
    }
    IFX(X_PF) {
        emit_pf(dyn, ninst, s1, s3, s4);
    }
    IFX(X_AF|X_OF|X_CF|X_PF){if(save_s4) {POP(xSP, 1<<s4);}}
}

// emit SUB16 instruction, from s1 , constant c, store result in s1 using s3 and s4 as scratch
void emit_sub16c(dynarec_arm_t* dyn, int ninst, int s1, int c, int s3, int s4)
{
    IFX(X_PEND) {
        MOVW(s3, c);
        STR_IMM9(s1, xEmu, offsetof(x86emu_t, op1));
        STR_IMM9(s3, xEmu, offsetof(x86emu_t, op2));
        SET_DF(s4, d_sub16);
    } else IFX(X_ALL) {
        SET_DFNONE(s4);
    }
    IFX(X_AF|X_OF|X_CF) {
        MVN_REG_LSL_IMM5(s4, s1, 0);
    }
    if(c>=0 && c<255) {
        SUB_IMM8(s1, s1, c);
    } else {
        IFX(X_PEND) {} else {MOVW(s3, c);}
        SUB_REG_LSL_IMM5(s1, s1, s3, 0);
    }
    IFX(X_PEND) {
        STR_IMM9(s1, xEmu, offsetof(x86emu_t, res));
    }
    IFX(X_AF|X_OF|X_CF) {
        if(c>=0 && c<256) {
            ORR_IMM8(s3, s4, c, 0);     // s3 = ~op1 | op2
            AND_IMM8(s4, s4, c);        // s4 = ~op1 & op2
        } else {
            ORR_REG_LSL_IMM5(s3, s3, s4, 0);    // s3 = ~op1 | op2
            PUSH(xSP, 1<<s3);
            MOVW(s3, c);
            AND_REG_LSL_IMM5(s4, s4, s3, 0);    // s4 = ~op1 & op2
            POP(xSP, 1<<s3);
        }
        AND_REG_LSL_IMM5(s3, s3, s1, 0);   // s3 = (~op1 | op2) & res
        ORR_REG_LSL_IMM5(s3, s3, s4, 0);   // s3 = (~op1 & op2) | ((~op1 | op2) & res)
        IFX(X_CF) {
            MOV_REG_LSR_IMM5(s4, s3, 15);
            BFI(xFlags, s4, F_CF, 1);    // CF : bc & 0x8000
        }
        IFX(X_AF) {
            MOV_REG_LSR_IMM5(s4, s3, 3);
            BFI(xFlags, s4, F_AF, 1);    // AF: bc & 0x08
        }
        IFX(X_OF) {
            MOV_REG_LSR_IMM5(s4, s3, 14);
            XOR_REG_LSR_IMM8(s4, s4, s4, 1);
            BFI(xFlags, s4, F_OF, 1);    // OF: ((bc >> 14) ^ ((bc>>14)>>1)) & 1
        }
    }
    IFX(X_ZF) {
        UXTH(s1, s1, 0);
        TSTS_REG_LSL_IMM5(s1, s1, 0);
        ORR_IMM8_COND(cEQ, xFlags, xFlags, 1<<F_ZF, 0);
        BIC_IMM8_COND(cNE, xFlags, xFlags, 1<<F_ZF, 0);
    }
    IFX(X_SF) {
        MOV_REG_LSR_IMM5(s3, s1, 15);
        BFI(xFlags, s3, F_SF, 1);
    }
    IFX(X_PF) {
        emit_pf(dyn, ninst, s1, s3, s4);
    }
}

// emit INC32 instruction, from s1, store result in s1 using s3 and s4 as scratch
void emit_inc32(dynarec_arm_t* dyn, int ninst, int s1, int s3, int s4)
{
    IFX(X_PEND) {
        STR_IMM9(s1, xEmu, offsetof(x86emu_t, op1));
        SET_DF(s4, d_inc32);
    } else IFX(X_ZF|X_OF|X_AF|X_SF|X_PF) {
        SET_DFNONE(s4);
    }
    IFX(X_AF) {
        ORR_IMM8(s3, s1, 1, 0);             // s3 = op1 | op2
        AND_IMM8(s4, s1, 1);                // s4 = op1 & op2
    }
    IFX(X_ZF|X_OF) {
        ADDS_IMM8(s1, s1, 1);
    } else {
        ADD_IMM8(s1, s1, 1);
    }
    IFX(X_PEND) {
        STR_IMM9(s1, xEmu, offsetof(x86emu_t, res));
    }
    IFX(X_AF) {
        BIC_REG_LSL_IMM5(s3, s3, s1, 0);   // s3 = (op1 | op2) & ~ res
        ORR_REG_LSL_IMM5(s3, s3, s4, 0);   // s4 = (op1 & op2) | ((op1 | op2) & ~ res)
        MOV_REG_LSR_IMM5(s4, s3, 3);
        BFI(xFlags, s4, F_AF, 1);    // AF: bc & 0x08
    }
    IFX(X_ZF) {
        ORR_IMM8_COND(cEQ, xFlags, xFlags, 1<<F_ZF, 0);
        BIC_IMM8_COND(cNE, xFlags, xFlags, 1<<F_ZF, 0);
    }
    IFX(X_OF) {
        ORR_IMM8_COND(cVS, xFlags, xFlags, 0b10, 0x0b);
        BIC_IMM8_COND(cVC, xFlags, xFlags, 0b10, 0x0b);
    }
    IFX(X_SF) {
        MOV_REG_LSR_IMM5(s3, s1, 31);
        BFI(xFlags, s3, F_SF, 1);
    }
    IFX(X_PF) {
        emit_pf(dyn, ninst, s1, s3, s4);
    }
}

// emit INC8 instruction, from s1, store result in s1 using s3 and s4 as scratch, with save_s4 is s4 need to be saved
void emit_inc8(dynarec_arm_t* dyn, int ninst, int s1, int s3, int s4)
{
    IFX(X_PEND) {
        STR_IMM9(s1, xEmu, offsetof(x86emu_t, op1));
        SET_DF(s3, d_inc8);
    } else IFX(X_ZF|X_OF|X_AF|X_SF|X_PF) {
        SET_DFNONE(s3);
    }
    IFX(X_AF | X_OF) {
        ORR_IMM8(s3, s1, 1, 0);     // s3 = op1 | op2
        AND_IMM8(s4, s1, 1);        // s4 = op1 & op2
    }
    ADD_IMM8(s1, s1, 1);
    IFX(X_PEND) {
        STR_IMM9(s1, xEmu, offsetof(x86emu_t, res));
    }
    IFX(X_AF|X_OF) {
        BIC_REG_LSL_IMM5(s3, s3, s1, 0);   // s3 = (op1 | op2) & ~ res
        ORR_REG_LSL_IMM5(s3, s3, s4, 0);   // s4 = (op1 & op2) | ((op1 | op2) & ~ res)
        IFX(X_AF) {
            MOV_REG_LSR_IMM5(s4, s3, 3);
            BFI(xFlags, s4, F_AF, 1);    // AF: bc & 0x08
        }
        IFX(X_OF) {
            MOV_REG_LSR_IMM5(s4, s3, 6);
            XOR_REG_LSR_IMM8(s4, s4, s4, 1);
            BFI(xFlags, s4, F_OF, 1);    // OF: ((bc >> 6) ^ ((bc>>6)>>1)) & 1
        }
    }

    IFX(X_ZF) {
        ANDS_IMM8(s1, s1, 0xff);
        ORR_IMM8_COND(cEQ, xFlags, xFlags, 1<<F_ZF, 0);
        BIC_IMM8_COND(cNE, xFlags, xFlags, 1<<F_ZF, 0);
    }
    IFX(X_SF) {
        MOV_REG_LSR_IMM5(s3, s1, 7);
        BFI(xFlags, s3, F_SF, 1);
    }
    IFX(X_PF) {
        emit_pf(dyn, ninst, s1, s3, s4);
    }
}

// emit INC16 instruction, from s1 , store result in s1 using s3 and s4 as scratch, with save_s4 is s4 need to be saved
void emit_inc16(dynarec_arm_t* dyn, int ninst, int s1, int s3, int s4)
{
    IFX(X_PEND) {
        STR_IMM9(s1, xEmu, offsetof(x86emu_t, op1));
        SET_DF(s3, d_inc16);
    } else IFX(X_ZF|X_OF|X_AF|X_SF|X_PF) {
        SET_DFNONE(s3);
    }
    IFX(X_AF | X_OF) {
        MOV_REG(s4, s1);
    }
    ADD_IMM8(s1, s1, 1);
    IFX(X_PEND) {
        STR_IMM9(s1, xEmu, offsetof(x86emu_t, res));
    }
    IFX(X_AF|X_OF) {
        ORR_IMM8(s3, s4, 1, 0);     // s3 = op1 | op2
        AND_IMM8(s4, s4, 1);        // s4 = op1 & op2

        BIC_REG_LSL_IMM5(s3, s3, s1, 0);   // s3 = (op1 | op2) & ~ res
        ORR_REG_LSL_IMM5(s3, s3, s4, 0);   // s3 = (op1 & op2) | ((op1 | op2) & ~ res)
        IFX(X_AF) {
            MOV_REG_LSR_IMM5(s4, s3, 3);
            BFI(xFlags, s4, F_AF, 1);    // AF: bc & 0x08
        }
        IFX(X_OF) {
            MOV_REG_LSR_IMM5(s4, s3, 14);
            XOR_REG_LSR_IMM8(s4, s4, s4, 1);
            BFI(xFlags, s4, F_OF, 1);    // OF: ((bc >> 14) ^ ((bc>>14)>>1)) & 1
        }
    }
    IFX(X_ZF) {
        UXTH(s1, s1, 0);
        TSTS_REG_LSL_IMM5(s1, s1, 0);
        ORR_IMM8_COND(cEQ, xFlags, xFlags, 1<<F_ZF, 0);
        BIC_IMM8_COND(cNE, xFlags, xFlags, 1<<F_ZF, 0);
    }
    IFX(X_SF) {
        MOV_REG_LSR_IMM5(s3, s1, 15);
        BFI(xFlags, s3, F_SF, 1);
    }
    IFX(X_PF) {
        emit_pf(dyn, ninst, s1, s3, s4);
    }
}

// emit DEC32 instruction, from s1, store result in s1 using s3 and s4 as scratch
void emit_dec32(dynarec_arm_t* dyn, int ninst, int s1, int s3, int s4)
{
    IFX(X_PEND) {
        STR_IMM9(s1, xEmu, offsetof(x86emu_t, op1));
        SET_DF(s4, d_dec32);
    } else IFX(X_ZF|X_OF|X_AF|X_SF|X_PF) {
        SET_DFNONE(s4);
    }
    IFX(X_AF) {
        MVN_REG_LSL_IMM5(s3, s1, 0);
        AND_IMM8(s4, s3, 1);                // s4 = ~op1 & op2
        ORR_IMM8(s3, s3, 1, 0);             // s3 = ~op1 | op2
    }
    IFX(X_ZF|X_OF) {
        SUBS_IMM8(s1, s1, 1);
    } else {
        SUB_IMM8(s1, s1, 1);
    }
    IFX(X_PEND) {
        STR_IMM9(s1, xEmu, offsetof(x86emu_t, res));
    }
    IFX(X_AF) {
        AND_REG_LSL_IMM5(s3, s3, s1, 0);   // s3 = (~op1 | op2) & res
        ORR_REG_LSL_IMM5(s3, s3, s4, 0);   // s4 = (~op1 & op2) | ((~op1 | op2) & ~ res)
        MOV_REG_LSR_IMM5(s4, s3, 3);
        BFI(xFlags, s4, F_AF, 1);    // AF: bc & 0x08
    }
    IFX(X_ZF) {
        ORR_IMM8_COND(cEQ, xFlags, xFlags, 1<<F_ZF, 0);
        BIC_IMM8_COND(cNE, xFlags, xFlags, 1<<F_ZF, 0);
    }
    IFX(X_OF) {
        ORR_IMM8_COND(cVS, xFlags, xFlags, 0b10, 0x0b);
        BIC_IMM8_COND(cVC, xFlags, xFlags, 0b10, 0x0b);
    }
    IFX(X_SF) {
        MOV_REG_LSR_IMM5(s3, s1, 31);
        BFI(xFlags, s3, F_SF, 1);
    }
    IFX(X_PF) {
        emit_pf(dyn, ninst, s1, s3, s4);
    }
}

// emit DEC8 instruction, from s1, store result in s1 using s3 and s4 as scratch
void emit_dec8(dynarec_arm_t* dyn, int ninst, int s1, int s3, int s4)
{
    IFX(X_PEND) {
        STR_IMM9(s3, xEmu, offsetof(x86emu_t, op2));
        SET_DF(s3, d_dec8);
    } else IFX(X_ZF|X_OF|X_AF|X_SF|X_PF) {
        SET_DFNONE(s3);
    }
    IFX(X_AF|X_OF) {
        MVN_REG_LSL_IMM5(s3, s1, 0);
        AND_IMM8(s4, s3, 1);        // s4 = ~op1 & op2
        ORR_IMM8(s3, s3, 1, 0);     // s3 = ~op1 | op2
    }
    SUB_IMM8(s1, s1, 1);
    IFX(X_PEND) {
        STR_IMM9(s1, xEmu, offsetof(x86emu_t, res));
    }
    IFX(X_AF|X_OF) {
        AND_REG_LSL_IMM5(s3, s3, s1, 0);   // s3 = (~op1 | op2) & res
        ORR_REG_LSL_IMM5(s3, s3, s4, 0);   // s3 = (~op1 & op2) | ((~op1 | op2) & res)
        IFX(X_AF) {
            MOV_REG_LSR_IMM5(s4, s3, 3);
            BFI(xFlags, s4, F_AF, 1);    // AF: bc & 0x08
        }
        IFX(X_OF) {
            MOV_REG_LSR_IMM5(s4, s3, 6);
            XOR_REG_LSR_IMM8(s4, s4, s4, 1);
            BFI(xFlags, s4, F_OF, 1);    // OF: ((bc >> 6) ^ ((bc>>6)>>1)) & 1
        }
    }
    IFX(X_ZF) {
        ANDS_IMM8(s1, s1, 0xff);
        ORR_IMM8_COND(cEQ, xFlags, xFlags, 1<<F_ZF, 0);
        BIC_IMM8_COND(cNE, xFlags, xFlags, 1<<F_ZF, 0);
    }
    IFX(X_SF) {
        MOV_REG_LSR_IMM5(s3, s1, 7);
        BFI(xFlags, s3, F_SF, 1);
    }
    IFX(X_PF) {
        emit_pf(dyn, ninst, s1, s3, s4);
    }
}

// emit DEC16 instruction, from s1, store result in s1 using s3 and s4 as scratch
void emit_dec16(dynarec_arm_t* dyn, int ninst, int s1, int s3, int s4)
{
    IFX(X_PEND) {
        STR_IMM9(s1, xEmu, offsetof(x86emu_t, op1));
        SET_DF(s3, d_dec16);
    } else IFX(X_ZF|X_OF|X_AF|X_SF|X_PF) {
        SET_DFNONE(s3);
    }
    IFX(X_AF|X_OF) {
        MVN_REG_LSL_IMM5(s4, s1, 0);
    }
    SUB_IMM8(s1, s1, 1);
    IFX(X_PEND) {
        STR_IMM9(s1, xEmu, offsetof(x86emu_t, res));
    }
    IFX(X_AF|X_OF) {
        ORR_IMM8(s3, s4, 1, 0);     // s3 = ~op1 | op2
        AND_IMM8(s4, s4, 1);        // s4 = ~op1 & op2
        AND_REG_LSL_IMM5(s3, s3, s1, 0);   // s3 = (~op1 | op2) & res
        ORR_REG_LSL_IMM5(s3, s3, s4, 0);   // s3 = (~op1 & op2) | ((~op1 | op2) & res)
        IFX(X_AF) {
            MOV_REG_LSR_IMM5(s4, s3, 3);
            BFI(xFlags, s4, F_AF, 1);    // AF: bc & 0x08
        }
        IFX(X_OF) {
            MOV_REG_LSR_IMM5(s4, s3, 14);
            XOR_REG_LSR_IMM8(s4, s4, s4, 1);
            BFI(xFlags, s4, F_OF, 1);    // OF: ((bc >> 14) ^ ((bc>>14)>>1)) & 1
        }
    }
    IFX(X_ZF) {
        UXTH(s1, s1, 0);
        TSTS_REG_LSL_IMM5(s1, s1, 0);
        ORR_IMM8_COND(cEQ, xFlags, xFlags, 1<<F_ZF, 0);
        BIC_IMM8_COND(cNE, xFlags, xFlags, 1<<F_ZF, 0);
    }
    IFX(X_SF) {
        MOV_REG_LSR_IMM5(s3, s1, 15);
        BFI(xFlags, s3, F_SF, 1);
    }
    IFX(X_PF) {
        emit_pf(dyn, ninst, s1, s3, s4);
    }
}

// emit ADC32 instruction, from s1 , s2, store result in s1 using s3 and s4 as scratch
void emit_adc32(dynarec_arm_t* dyn, int ninst, int s1, int s2, int s3, int s4)
{
    IFX(X_PEND) {
        STR_IMM9(s1, xEmu, offsetof(x86emu_t, op1));
        STR_IMM9(s2, xEmu, offsetof(x86emu_t, op2));
        SET_DF(s3, d_adc32);
    } else IFX(X_ALL) {
        SET_DFNONE(s3);
    }
    IFX(X_AF) {
        MOV_REG(s4, s1);
    }
    MOVS_REG_LSR_IMM5(s3, xFlags, 1);    // load CC (F_CF==0) into ARM CF
    IFX(X_ZF|X_CF|X_OF) {
        ADCS_REG_LSL_IMM5(s1, s1, s2, 0);
    } else {
        ADC_REG_LSL_IMM5(s1, s1, s2, 0);
    }
    IFX(X_PEND) {
        STR_IMM9(s1, xEmu, offsetof(x86emu_t, res));
    }
    IFX(X_AF) {
        ORR_REG_LSL_IMM5(s3, s4, s2, 0);    // s3 = op1 | op2
        AND_REG_LSL_IMM5(s4, s4, s2, 0);    // s4 = op1 & op2
        BIC_REG_LSL_IMM5(s3, s3, s1, 0);   // s3 = (op1 | op2) & ~ res
        ORR_REG_LSL_IMM5(s3, s3, s4, 0);   // s4 = (op1 & op2) | ((op1 | op2) & ~ res)
        MOV_REG_LSR_IMM5(s4, s3, 3);
        BFI(xFlags, s4, F_AF, 1);    // AF: bc & 0x08
    }
    IFX(X_ZF) {
        ORR_IMM8_COND(cEQ, xFlags, xFlags, 1<<F_ZF, 0);
        BIC_IMM8_COND(cNE, xFlags, xFlags, 1<<F_ZF, 0);
    }
    IFX(X_CF) {
        ORR_IMM8_COND(cCS, xFlags, xFlags, 1<<F_CF, 0);
        BIC_IMM8_COND(cCC, xFlags, xFlags, 1<<F_CF, 0);
    }
    IFX(X_OF) {
        ORR_IMM8_COND(cVS, xFlags, xFlags, 0b10, 0x0b);
        BIC_IMM8_COND(cVC, xFlags, xFlags, 0b10, 0x0b);
    }
    IFX(X_SF) {
        MOV_REG_LSR_IMM5(s3, s1, 31);
        BFI(xFlags, s3, F_SF, 1);
    }
    IFX(X_PF) {
        emit_pf(dyn, ninst, s1, s3, s4);
    }
}

// emit ADC32 instruction, from s1 , constant c, store result in s1 using s3 and s4 as scratch
void emit_adc32c(dynarec_arm_t* dyn, int ninst, int s1, int32_t c, int s3, int s4)
{
    IFX(X_PEND) {
        MOV32(s3, c);
        STR_IMM9(s1, xEmu, offsetof(x86emu_t, op1));
        STR_IMM9(s3, xEmu, offsetof(x86emu_t, op2));
        SET_DF(s4, d_adc32);
    } else IFX(X_ALL) {
        SET_DFNONE(s4);
    }
    IFX(X_AF) {
        MOV_REG(s4, s1);
    }
    MOVS_REG_LSR_IMM5(s3, xFlags, 1);    // load CC into ARM CF
    if(c>=0 && c<256) {
        IFX(X_ZF|X_CF|X_OF) {
            ADCS_IMM8(s1, s1, c);
        } else {
            ADC_IMM8(s1, s1, c);
        }
    } else {
        MOV32(s3, c);
        IFX(X_ZF|X_CF|X_OF) {
            ADCS_REG_LSL_IMM5(s1, s1, s3, 0);
        } else {
            ADC_REG_LSL_IMM5(s1, s1, s3, 0);
        }
    }
    IFX(X_PEND) {
        STR_IMM9(s1, xEmu, offsetof(x86emu_t, res));
    }
    IFX(X_AF) {
        if(c>=0 && c<256) {
            ORR_IMM8(s3, s4, c, 0);     // s3 = op1 | op2
            AND_IMM8(s4, s4, c);        // s4 = op1 & op2
        } else {
            ORR_REG_LSL_IMM5(s3, s3, s4, 0);    // s3 = op1 | op2
            PUSH(xSP, 1<<s3);
            MOVW(s3, c);
            AND_REG_LSL_IMM5(s4, s4, s3, 0);    // s4 = op1 & op2
            POP(xSP, 1<<s3);
        }
        BIC_REG_LSL_IMM5(s3, s3, s1, 0);   // s3 = (op1 | op2) & ~ res
        ORR_REG_LSL_IMM5(s3, s3, s4, 0);   // s4 = (op1 & op2) | ((op1 | op2) & ~ res)
        MOV_REG_LSR_IMM5(s4, s3, 3);
        BFI(xFlags, s4, F_AF, 1);    // AF: bc & 0x08
    }
    IFX(X_ZF|X_CF) {
        BIC_IMM8(xFlags, xFlags, (1<<F_ZF)|(1<<F_CF), 0);
    }
    IFX(X_ZF) {
        ORR_IMM8_COND(cEQ, xFlags, xFlags, 1<<F_ZF, 0);
    }
    IFX(X_CF) {
        ORR_IMM8_COND(cCS, xFlags, xFlags, 1<<F_CF, 0);
    }
    IFX(X_OF) {
        ORR_IMM8_COND(cVS, xFlags, xFlags, 0b10, 0x0b);
        BIC_IMM8_COND(cVC, xFlags, xFlags, 0b10, 0x0b);
    }
    IFX(X_SF) {
        MOV_REG_LSR_IMM5(s3, s1, 31);
        BFI(xFlags, s3, F_SF, 1);
    }
    IFX(X_PF) {
        emit_pf(dyn, ninst, s1, s3, s4);
    }
}

// emit ADC8 instruction, from s1 , s2, store result in s1 using s3 and s4 as scratch, with save_s4 is s4 need to be saved
void emit_adc8(dynarec_arm_t* dyn, int ninst, int s1, int s2, int s3, int s4, int save_s4)
{
    IFX(X_PEND) {
        STR_IMM9(s1, xEmu, offsetof(x86emu_t, op1));
        STR_IMM9(s2, xEmu, offsetof(x86emu_t, op2));
        SET_DF(s3, d_adc8);
    } else IFX(X_ALL) {
        SET_DFNONE(s3);
    }
    IFX(X_AF|X_OF|X_PF){if(save_s4) {PUSH(xSP, 1<<s4);}}
    IFX(X_AF | X_OF) {
        MOV_REG(s4, s1);
    }
    MOVS_REG_LSR_IMM5(s3, xFlags, 1);    // load CC into ARM CF
    ADC_REG_LSL_IMM5(s1, s1, s2, 0);
    IFX(X_PEND) {
        STR_IMM9(s1, xEmu, offsetof(x86emu_t, res));
    }
    IFX(X_AF|X_OF) {
        ORR_REG_LSL_IMM5(s3, s4, s2, 0);    // s3 = op1 | op2
        AND_REG_LSL_IMM5(s4, s4, s2, 0);    // s4 = op1 & op2
        BIC_REG_LSL_IMM5(s3, s3, s1, 0);   // s3 = (op1 | op2) & ~ res
        ORR_REG_LSL_IMM5(s3, s3, s4, 0);   // s3 = (op1 & op2) | ((op1 | op2) & ~ res)
        IFX(X_AF) {
            MOV_REG_LSR_IMM5(s4, s3, 3);
            BFI(xFlags, s4, F_AF, 1);    // AF: bc & 0x08
        }
        IFX(X_OF) {
            MOV_REG_LSR_IMM5(s4, s3, 6);
            XOR_REG_LSR_IMM8(s4, s4, s4, 1);
            BFI(xFlags, s4, F_OF, 1);    // OF: ((bc >> 6) ^ ((bc>>6)>>1)) & 1
        }
    }
    IFX(X_CF) {
        MOV_REG_LSR_IMM5(s3, s1, 8);
        BFI(xFlags, s3, F_CF, 1);
    }
    IFX(X_ZF) {
        ANDS_IMM8(s1, s1, 0xff);
        ORR_IMM8_COND(cEQ, xFlags, xFlags, 1<<F_ZF, 0);
        BIC_IMM8_COND(cNE, xFlags, xFlags, 1<<F_ZF, 0);
    }
    IFX(X_SF) {
        MOV_REG_LSR_IMM5(s3, s1, 7);
        BFI(xFlags, s3, F_SF, 1);
    }
    IFX(X_PF) {
        emit_pf(dyn, ninst, s1, s3, s4);
    }
    IFX(X_AF|X_OF|X_PF){if(save_s4) {POP(xSP, 1<<s4);}}
}

// emit ADC8 instruction, from s1 , const c, store result in s1 using s3 and s4 as scratch, with save_s4 is s4 need to be saved
void emit_adc8c(dynarec_arm_t* dyn, int ninst, int s1, int c, int s3, int s4)
{
    IFX(X_PEND) {
        MOVW(s3, c&0xff);
        STR_IMM9(s1, xEmu, offsetof(x86emu_t, op1));
        STR_IMM9(s3, xEmu, offsetof(x86emu_t, op2));
        SET_DF(s4, d_adc8);
    } else IFX(X_ALL) {
        SET_DFNONE(s4);
    }
    IFX(X_AF | X_OF) {
        MOV_REG(s4, s1);
    }
    MOVS_REG_LSR_IMM5(s3, xFlags, 1);    // load CC into ARM CF
    ADC_IMM8(s1, s1, c);
    IFX(X_PEND) {
        STR_IMM9(s1, xEmu, offsetof(x86emu_t, res));
    }
    IFX(X_AF|X_OF) {
        ORR_IMM8(s3, s4, c, 0);     // s3 = op1 | op2
        AND_IMM8(s4, s4, c);        // s4 = op1 & op2
        BIC_REG_LSL_IMM5(s3, s3, s1, 0);   // s3 = (op1 | op2) & ~ res
        ORR_REG_LSL_IMM5(s3, s3, s4, 0);   // s4 = (op1 & op2) | ((op1 | op2) & ~ res)
        IFX(X_AF) {
            MOV_REG_LSR_IMM5(s4, s3, 3);
            BFI(xFlags, s4, F_AF, 1);    // AF: bc & 0x08
        }
        IFX(X_OF) {
            MOV_REG_LSR_IMM5(s4, s3, 6);
            XOR_REG_LSR_IMM8(s4, s4, s4, 1);
            BFI(xFlags, s4, F_OF, 1);    // OF: ((bc >> 6) ^ ((bc>>6)>>1)) & 1
        }
    }
    IFX(X_CF) {
        MOV_REG_LSR_IMM5(s3, s1, 8);
        BFI(xFlags, s3, F_CF, 1);
    }
    IFX(X_ZF) {
        ANDS_IMM8(s1, s1, 0xff);
        ORR_IMM8_COND(cEQ, xFlags, xFlags, 1<<F_ZF, 0);
        BIC_IMM8_COND(cNE, xFlags, xFlags, 1<<F_ZF, 0);
    }
    IFX(X_SF) {
        MOV_REG_LSR_IMM5(s3, s1, 7);
        BFI(xFlags, s3, F_SF, 1);
    }
    IFX(X_PF) {
        emit_pf(dyn, ninst, s1, s3, s4);
    }
}

// emit ADC16 instruction, from s1 , s2, store result in s1 using s3 and s4 as scratch, with save_s4 is s4 need to be saved
void emit_adc16(dynarec_arm_t* dyn, int ninst, int s1, int s2, int s3, int s4, int save_s4)
{
    IFX(X_PEND) {
        STR_IMM9(s1, xEmu, offsetof(x86emu_t, op1));
        STR_IMM9(s2, xEmu, offsetof(x86emu_t, op2));
        SET_DF(s3, d_adc16);
    } else IFX(X_ALL) {
        SET_DFNONE(s3);
    }
    IFX(X_AF|X_OF|X_PF){if(save_s4) {PUSH(xSP, 1<<s4);}}
    IFX(X_AF | X_OF) {
        MOV_REG(s4, s1);
    }
    MOVS_REG_LSR_IMM5(s3, xFlags, 1);    // load CC into ARM CF
    ADC_REG_LSL_IMM5(s1, s1, s2, 0);
    IFX(X_PEND) {
        STR_IMM9(s1, xEmu, offsetof(x86emu_t, res));
    }
    IFX(X_AF|X_OF) {
        ORR_REG_LSL_IMM5(s3, s4, s2, 0);    // s3 = op1 | op2
        AND_REG_LSL_IMM5(s4, s4, s2, 0);    // s4 = op1 & op2
        BIC_REG_LSL_IMM5(s3, s3, s1, 0);   // s3 = (op1 | op2) & ~ res
        ORR_REG_LSL_IMM5(s3, s3, s4, 0);   // s3 = (op1 & op2) | ((op1 | op2) & ~ res)
        IFX(X_AF) {
            MOV_REG_LSR_IMM5(s4, s3, 3);
            BFI(xFlags, s4, F_AF, 1);    // AF: bc & 0x08
        }
        IFX(X_OF) {
            MOV_REG_LSR_IMM5(s4, s3, 14);
            XOR_REG_LSR_IMM8(s4, s4, s4, 1);
            BFI(xFlags, s4, F_OF, 1);    // OF: ((bc >> 14) ^ ((bc>>14)>>1)) & 1
        }
    }
    IFX(X_CF) {
        MOV_REG_LSR_IMM5(s3, s1, 16);
        BFI(xFlags, s3, F_CF, 1);
    }
    IFX(X_ZF) {
        UXTH(s1, s1, 0);
        TSTS_REG_LSL_IMM5(s1, s1, 0);
        ORR_IMM8_COND(cEQ, xFlags, xFlags, 1<<F_ZF, 0);
        BIC_IMM8_COND(cNE, xFlags, xFlags, 1<<F_ZF, 0);
    }
    IFX(X_SF) {
        MOV_REG_LSR_IMM5(s3, s1, 15);
        BFI(xFlags, s3, F_SF, 1);
    }
    IFX(X_PF) {
        emit_pf(dyn, ninst, s1, s3, s4);
    }
    IFX(X_AF|X_OF|X_PF){if(save_s4) {POP(xSP, 1<<s4);}}
}

// emit ADC16 instruction, from s1 , const c, store result in s1 using s3 and s4 as scratch, with save_s4 is s4 need to be saved
void emit_adc16c(dynarec_arm_t* dyn, int ninst, int s1, int c, int s3, int s4)
{
    IFX(X_PEND) {
        MOVW(s3, c);
        STR_IMM9(s1, xEmu, offsetof(x86emu_t, op1));
        STR_IMM9(s3, xEmu, offsetof(x86emu_t, op2));
        SET_DF(s3, d_adc16);
    } else IFX(X_ALL) {
        SET_DFNONE(s3);
    }
    IFX(X_AF | X_OF) {
        MOV_REG(s4, s1);
    }
    MOVS_REG_LSR_IMM5(s3, xFlags, 1);    // load CC into ARM CF
    if(c>=0 && c<256) {
        ADC_IMM8(s1, s1, c);
    } else {
        MOVW(s3, c);
        ADC_REG_LSL_IMM5(s1, s1, s3, 0);
    }
    IFX(X_PEND) {
        STR_IMM9(s1, xEmu, offsetof(x86emu_t, res));
    }
    IFX(X_AF|X_OF) {
        if(c>=0 && c<256) {
            ORR_IMM8(s3, s4, c, 0);     // s3 = op1 | op2
            AND_IMM8(s4, s4, c);        // s4 = op1 & op2
        } else {
            ORR_REG_LSL_IMM5(s3, s3, s4, 0);    // s3 = op1 | op2
            PUSH(xSP, 1<<s3);
            MOVW(s3, c);
            AND_REG_LSL_IMM5(s4, s4, s3, 0);    // s4 = op1 & op2
            POP(xSP, 1<<s3);
        }

        BIC_REG_LSL_IMM5(s3, s3, s1, 0);   // s3 = (op1 | op2) & ~ res
        ORR_REG_LSL_IMM5(s3, s3, s4, 0);   // s3 = (op1 & op2) | ((op1 | op2) & ~ res)
        IFX(X_AF) {
            MOV_REG_LSR_IMM5(s4, s3, 3);
            BFI(xFlags, s4, F_AF, 1);    // AF: bc & 0x08
        }
        IFX(X_OF) {
            MOV_REG_LSR_IMM5(s4, s3, 14);
            XOR_REG_LSR_IMM8(s4, s4, s4, 1);
            BFI(xFlags, s4, F_OF, 1);    // OF: ((bc >> 14) ^ ((bc>>14)>>1)) & 1
        }
    }
    IFX(X_CF) {
        MOV_REG_LSR_IMM5(s3, s1, 16);
        BFI(xFlags, s3, F_CF, 1);
    }
    IFX(X_ZF) {
        UXTH(s1, s1, 0);
        TSTS_REG_LSL_IMM5(s1, s1, 0);
        ORR_IMM8_COND(cEQ, xFlags, xFlags, 1<<F_ZF, 0);
        BIC_IMM8_COND(cNE, xFlags, xFlags, 1<<F_ZF, 0);
    }
    IFX(X_SF) {
        MOV_REG_LSR_IMM5(s3, s1, 15);
        BFI(xFlags, s3, F_SF, 1);
    }
    IFX(X_PF) {
        emit_pf(dyn, ninst, s1, s3, s4);
    }
}

// emit SBB32 instruction, from s1 , s2, store result in s1 using s3 and s4 as scratch
void emit_sbb32(dynarec_arm_t* dyn, int ninst, int s1, int s2, int s3, int s4)
{
    IFX(X_PEND) {
        STR_IMM9(s1, xEmu, offsetof(x86emu_t, op1));
        STR_IMM9(s2, xEmu, offsetof(x86emu_t, op2));
        SET_DF(s3, d_sbb32);
    } else IFX(X_ALL) {
        SET_DFNONE(s3);
    }
    IFX(X_AF) {
        MVN_REG_LSL_IMM5(s4, s1, 0);
    }
    XOR_IMM8(s3, xFlags, 1);                // invert CC because it's reverted for SUB on ARM
    MOVS_REG_LSR_IMM5(s3, s3, 1);       // load into ARM CF
    IFX(X_ZF|X_CF|X_OF) {
        SBCS_REG_LSL_IMM5(s1, s1, s2, 0);
    } else {
        SBC_REG_LSL_IMM5(s1, s1, s2, 0);
    }
    IFX(X_PEND) {
        STR_IMM9(s1, xEmu, offsetof(x86emu_t, res));
    }
    IFX(X_AF) {
        ORR_REG_LSL_IMM5(s3, s4, s2, 0);    // s3 = ~op1 | op2
        AND_REG_LSL_IMM5(s4, s2, s4, 0);    // s4 = ~op1 & op2
        AND_REG_LSL_IMM5(s3, s3, s1, 0);   // s3 = (~op1 | op2) & res
        ORR_REG_LSL_IMM5(s3, s3, s4, 0);   // s4 = (~op1 & op2) | ((~op1 | op2) & res)
        UBFX(s4, s3, 3, 1);
        BFI(xFlags, s4, F_AF, 1);    // AF: bc & 0x08
    }
    IFX(X_ZF|X_CF) {
        BIC_IMM8(xFlags, xFlags, (1<<F_ZF)|(1<<F_CF), 0);
    }
    IFX(X_ZF) {
        ORR_IMM8_COND(cEQ, xFlags, xFlags, 1<<F_ZF, 0);
    }
    IFX(X_CF) {
        // Inverted carry
        ORR_IMM8_COND(cCC, xFlags, xFlags, 1<<F_CF, 0);
    }
    IFX(X_OF) {
        ORR_IMM8_COND(cVS, xFlags, xFlags, 0b10, 0x0b);
        BIC_IMM8_COND(cVC, xFlags, xFlags, 0b10, 0x0b);
    }
    IFX(X_SF) {
        MOV_REG_LSR_IMM5(s3, s1, 31);
        BFI(xFlags, s3, F_SF, 1);
    }
    IFX(X_PF) {
        emit_pf(dyn, ninst, s1, s3, s4);
    }
}

// emit SBB32 instruction, from s1 , constant c, store result in s1 using s3 and s4 as scratch
void emit_sbb32c(dynarec_arm_t* dyn, int ninst, int s1, int32_t c, int s3, int s4)
{
    IFX(X_PEND) {
        MOV32(s3, c);
        STR_IMM9(s1, xEmu, offsetof(x86emu_t, op1));
        STR_IMM9(s3, xEmu, offsetof(x86emu_t, op2));
        SET_DF(s4, d_sbb32);
    } else IFX(X_ALL) {
        SET_DFNONE(s4);
    }
    IFX(X_AF) {
        MVN_REG_LSL_IMM5(s4, s1, 0);
    }
    XOR_IMM8(s3, xFlags, 1);            // invert CC because it's reverted for SUB on ARM
    MOVS_REG_LSR_IMM5(s3, s3, 1);       // load into ARM CF
    if(c>=0 && c<256) {
        IFX(X_ZF|X_CF|X_OF) {
            SBCS_IMM8(s1, s1, c);
        } else {
            SBC_IMM8(s1, s1, c);
        }
    } else {
        MOV32(s3, c);
        IFX(X_ZF|X_CF|X_OF) {
            SBCS_REG_LSL_IMM5(s1, s1, s3, 0);
        } else {
            SBC_REG_LSL_IMM5(s1, s1, s3, 0);
        }
    }
    IFX(X_PEND) {
        STR_IMM9(s1, xEmu, offsetof(x86emu_t, res));
    }
    IFX(X_AF) {
        if(c>=0 && c<256) {
            AND_IMM8(s4, s3, c);                // s4 = ~op1 & op2
            ORR_IMM8(s3, s3, c, 0);             // s3 = ~op1 | op2
        } else {
            ORR_REG_LSL_IMM5(s3, s4, s3, 0);
            PUSH(xSP, 1<<s3);
            MOV32(s3, c);
            AND_REG_LSL_IMM5(s4, s3, s4, 0);
            POP(xSP, 1<<s3);
        }
        AND_REG_LSL_IMM5(s3, s3, s1, 0);   // s3 = (~op1 | op2) & res
        ORR_REG_LSL_IMM5(s3, s3, s4, 0);   // s4 = (~op1 & op2) | ((~op1 | op2) & ~ res)
        UBFX(s4, s3, 3, 1);
        BFI(xFlags, s4, F_AF, 1);    // AF: bc & 0x08
    }
    IFX(X_ZF|X_CF) {
        BIC_IMM8(xFlags, xFlags, (1<<F_ZF)|(1<<F_CF), 0);
    }
    IFX(X_ZF) {
        ORR_IMM8_COND(cEQ, xFlags, xFlags, 1<<F_ZF, 0);
    }
    IFX(X_CF) {
        // Inverted carry
        ORR_IMM8_COND(cCC, xFlags, xFlags, 1<<F_CF, 0);
    }
    IFX(X_OF) {
        ORR_IMM8_COND(cVS, xFlags, xFlags, 0b10, 0x0b);
        BIC_IMM8_COND(cVC, xFlags, xFlags, 0b10, 0x0b);
    }
    IFX(X_SF) {
        MOV_REG_LSR_IMM5(s3, s1, 31);
        BFI(xFlags, s3, F_SF, 1);
    }
    IFX(X_PF) {
        emit_pf(dyn, ninst, s1, s3, s4);
    }
}

// emit SBB8 instruction, from s1 , s2, store result in s1 using s3 and s4 as scratch, with save_s4 is s4 need to be saved
void emit_sbb8(dynarec_arm_t* dyn, int ninst, int s1, int s2, int s3, int s4, int save_s4)
{
    IFX(X_PEND) {
        STR_IMM9(s1, xEmu, offsetof(x86emu_t, op1));
        STR_IMM9(s2, xEmu, offsetof(x86emu_t, op2));
        SET_DF(s3, d_sbb8);
    } else IFX(X_ALL) {
        SET_DFNONE(s3);
    }
    IFX(X_AF|X_OF|X_CF|X_PF){if(save_s4) {PUSH(xSP, 1<<s4);}}
    IFX(X_AF|X_OF|X_CF) {
        MVN_REG_LSL_IMM5(s4, s1, 0);
    }

    XOR_IMM8(s3, xFlags, 1);            // invert CC because it's reverted for SUB on ARM
    MOVS_REG_LSR_IMM5(s3, s3, 1);       // load into ARM CF
    SBC_REG_LSL_IMM5(s1, s1, s2, 0);
    IFX(X_PEND) {
        STR_IMM9(s1, xEmu, offsetof(x86emu_t, res));
    }
    IFX(X_AF|X_OF|X_CF) {
        ORR_REG_LSL_IMM5(s3, s4, s2, 0);    // s3 = ~op1 | op2
        AND_REG_LSL_IMM5(s4, s2, s4, 0);    // s4 = ~op1 & op2
        AND_REG_LSL_IMM5(s3, s3, s1, 0);   // s3 = (~op1 | op2) & res
        ORR_REG_LSL_IMM5(s3, s3, s4, 0);   // s3 = (~op1 & op2) | ((~op1 | op2) & res)
        IFX(X_CF) {
            MOV_REG_LSR_IMM5(s4, s3, 7);
            BFI(xFlags, s4, F_CF, 1);    // CF : bc & 0x80
        }
        IFX(X_AF) {
            MOV_REG_LSR_IMM5(s4, s3, 3);
            BFI(xFlags, s4, F_AF, 1);    // AF: bc & 0x08
        }
        IFX(X_OF) {
            MOV_REG_LSR_IMM5(s4, s3, 6);
            XOR_REG_LSR_IMM8(s4, s4, s4, 1);
            BFI(xFlags, s4, F_OF, 1);    // OF: ((bc >> 6) ^ ((bc>>6)>>1)) & 1
        }
    }
    IFX(X_ZF) {
        ANDS_IMM8(s1, s1, 0xff);
        ORR_IMM8_COND(cEQ, xFlags, xFlags, 1<<F_ZF, 0);
        BIC_IMM8_COND(cNE, xFlags, xFlags, 1<<F_ZF, 0);
    }
    IFX(X_SF) {
        MOV_REG_LSR_IMM5(s3, s1, 7);
        BFI(xFlags, s3, F_SF, 1);
    }
    IFX(X_PF) {
        emit_pf(dyn, ninst, s1, s3, s4);
    }
    IFX(X_AF|X_OF|X_CF|X_PF){if(save_s4) {POP(xSP, 1<<s4);}}
}

// emit SBB8 instruction, from s1 , constant c, store result in s1 using s3 and s4 as scratch
void emit_sbb8c(dynarec_arm_t* dyn, int ninst, int s1, int c, int s3, int s4)
{
    IFX(X_PEND) {
        MOVW(s3, c&0xff);
        STR_IMM9(s1, xEmu, offsetof(x86emu_t, op1));
        STR_IMM9(s3, xEmu, offsetof(x86emu_t, op2));
        SET_DF(s3, d_sbb8);
    } else IFX(X_ALL) {
        SET_DFNONE(s3);
    }
    IFX(X_AF|X_OF|X_CF) {
        MVN_REG_LSL_IMM5(s4, s1, 0);
    }
    XOR_IMM8(s3, xFlags, 1);            // invert CC because it's reverted for SUB on ARM
    MOVS_REG_LSR_IMM5(s3, s3, 1);       // load into ARM CF
    SBC_IMM8(s1, s1, c);
    IFX(X_PEND) {
        STR_IMM9(s1, xEmu, offsetof(x86emu_t, res));
    }
    IFX(X_AF|X_OF|X_CF) {
        ORR_IMM8(s3, s4, c, 0);             // s3 = ~op1 | op2
        AND_IMM8(s4, s4, c);                // s4 = ~op1 & op2
        AND_REG_LSL_IMM5(s3, s3, s1, 0);    // s3 = (~op1 | op2) & res
        ORR_REG_LSL_IMM5(s3, s3, s4, 0);    // s3 = (~op1 & op2) | ((~op1 | op2) & res)
        IFX(X_CF) {
            MOV_REG_LSR_IMM5(s4, s3, 7);
            BFI(xFlags, s4, F_CF, 1);    // CF : bc & 0x80
        }
        IFX(X_AF) {
            MOV_REG_LSR_IMM5(s4, s3, 3);
            BFI(xFlags, s4, F_AF, 1);    // AF: bc & 0x08
        }
        IFX(X_OF) {
            MOV_REG_LSR_IMM5(s4, s3, 6);
            XOR_REG_LSR_IMM8(s4, s4, s4, 1);
            BFI(xFlags, s4, F_OF, 1);    // OF: ((bc >> 6) ^ ((bc>>6)>>1)) & 1
        }
    }
    IFX(X_ZF) {
        ANDS_IMM8(s1, s1, 0xff);
        ORR_IMM8_COND(cEQ, xFlags, xFlags, 1<<F_ZF, 0);
        BIC_IMM8_COND(cNE, xFlags, xFlags, 1<<F_ZF, 0);
    }
    IFX(X_SF) {
        MOV_REG_LSR_IMM5(s3, s1, 7);
        BFI(xFlags, s3, F_SF, 1);
    }
    IFX(X_PF) {
        emit_pf(dyn, ninst, s1, s3, s4);
    }
}

// emit SBB16 instruction, from s1 , s2, store result in s1 using s3 and s4 as scratch, with save_s4 is s4 need to be saved
void emit_sbb16(dynarec_arm_t* dyn, int ninst, int s1, int s2, int s3, int s4, int save_s4)
{
    IFX(X_PEND) {
        STR_IMM9(s1, xEmu, offsetof(x86emu_t, op1));
        STR_IMM9(s2, xEmu, offsetof(x86emu_t, op2));
        SET_DF(s3, d_sbb16);
    } else IFX(X_ALL) {
        SET_DFNONE(s3);
    }
    IFX(X_AF|X_OF|X_CF|X_PF){if(save_s4) {PUSH(xSP, 1<<s4);}}
    IFX(X_AF|X_OF|X_CF) {
        MVN_REG_LSL_IMM5(s4, s1, 0);
    }

    XOR_IMM8(s3, xFlags, 1);            // invert CC because it's reverted for SUB on ARM
    MOVS_REG_LSR_IMM5(s3, s3, 1);       // load into ARM CF
    SBC_REG_LSL_IMM5(s1, s1, s2, 0);
    IFX(X_PEND) {
        STR_IMM9(s1, xEmu, offsetof(x86emu_t, res));
    }
    IFX(X_AF|X_OF|X_CF) {
        ORR_REG_LSL_IMM5(s3, s4, s2, 0);    // s3 = ~op1 | op2
        AND_REG_LSL_IMM5(s4, s2, s4, 0);    // s4 = ~op1 & op2
        AND_REG_LSL_IMM5(s3, s3, s1, 0);   // s3 = (~op1 | op2) & res
        ORR_REG_LSL_IMM5(s3, s3, s4, 0);   // s3 = (~op1 & op2) | ((~op1 | op2) & res)
        IFX(X_CF) {
            MOV_REG_LSR_IMM5(s4, s3, 15);
            BFI(xFlags, s4, F_CF, 1);    // CF : bc & 0x8000
        }
        IFX(X_AF) {
            MOV_REG_LSR_IMM5(s4, s3, 3);
            BFI(xFlags, s4, F_AF, 1);    // AF: bc & 0x08
        }
        IFX(X_OF) {
            MOV_REG_LSR_IMM5(s4, s3, 14);
            XOR_REG_LSR_IMM8(s4, s4, s4, 1);
            BFI(xFlags, s4, F_OF, 1);    // OF: ((bc >> 14) ^ ((bc>>14)>>1)) & 1
        }
    }
    IFX(X_ZF) {
        UXTH(s1, s1, 0);
        TSTS_REG_LSL_IMM5(s1, s1, 0);
        ORR_IMM8_COND(cEQ, xFlags, xFlags, 1<<F_ZF, 0);
        BIC_IMM8_COND(cNE, xFlags, xFlags, 1<<F_ZF, 0);
    }
    IFX(X_SF) {
        MOV_REG_LSR_IMM5(s3, s1, 15);
        BFI(xFlags, s3, F_SF, 1);
    }
    IFX(X_PF) {
        emit_pf(dyn, ninst, s1, s3, s4);
    }
    IFX(X_AF|X_OF|X_CF|X_PF){if(save_s4) {POP(xSP, 1<<s4);}}
}

// emit SBB16 instruction, from s1 , constant c, store result in s1 using s3 and s4 as scratch
void emit_sbb16c(dynarec_arm_t* dyn, int ninst, int s1, int c, int s3, int s4)
{
    IFX(X_PEND) {
        MOVW(s3, c);
        STR_IMM9(s1, xEmu, offsetof(x86emu_t, op1));
        STR_IMM9(s3, xEmu, offsetof(x86emu_t, op2));
        SET_DF(s3, d_sbb16);
    } else IFX(X_ALL) {
        SET_DFNONE(s3);
    }
    IFX(X_AF|X_OF|X_CF) {
        MVN_REG_LSL_IMM5(s4, s1, 0);
    }
    XOR_IMM8(s3, xFlags, 1);            // invert CC because it's reverted for SUB on ARM
    MOVS_REG_LSR_IMM5(s3, s3, 1);       // load into ARM CF
    if(c>=0 && c<255) {
        SBC_IMM8(s1, s1, c);
    } else {
        MOVW(s3, c);
        SBC_REG_LSL_IMM5(s1, s1, s3, 0);
    }
    IFX(X_PEND) {
        STR_IMM9(s1, xEmu, offsetof(x86emu_t, res));
    }
    IFX(X_AF|X_OF|X_CF) {
        if(c>=0 && c<256) {
            ORR_IMM8(s3, s4, c, 0);     // s3 = ~op1 | op2
            AND_IMM8(s4, s4, c);        // s4 = ~op1 & op2
        } else {
            ORR_REG_LSL_IMM5(s3, s3, s4, 0);    // s3 = ~op1 | op2
            PUSH(xSP, 1<<s3);
            MOVW(s3, c);
            AND_REG_LSL_IMM5(s4, s4, s3, 0);    // s4 = ~op1 & op2
            POP(xSP, 1<<s3);
        }
        AND_REG_LSL_IMM5(s3, s3, s1, 0);   // s3 = (~op1 | op2) & res
        ORR_REG_LSL_IMM5(s3, s3, s4, 0);   // s3 = (~op1 & op2) | ((~op1 | op2) & res)
        IFX(X_CF) {
            MOV_REG_LSR_IMM5(s4, s3, 15);
            BFI(xFlags, s4, F_CF, 1);    // CF : bc & 0x8000
        }
        IFX(X_AF) {
            MOV_REG_LSR_IMM5(s4, s3, 3);
            BFI(xFlags, s4, F_AF, 1);    // AF: bc & 0x08
        }
        IFX(X_OF) {
            MOV_REG_LSR_IMM5(s4, s3, 14);
            XOR_REG_LSR_IMM8(s4, s4, s4, 1);
            BFI(xFlags, s4, F_OF, 1);    // OF: ((bc >> 14) ^ ((bc>>14)>>1)) & 1
        }
    }
    IFX(X_ZF) {
        UXTH(s1, s1, 0);
        TSTS_REG_LSL_IMM5(s1, s1, 0);
        ORR_IMM8_COND(cEQ, xFlags, xFlags, 1<<F_ZF, 0);
        BIC_IMM8_COND(cNE, xFlags, xFlags, 1<<F_ZF, 0);
    }
    IFX(X_SF) {
        MOV_REG_LSR_IMM5(s3, s1, 15);
        BFI(xFlags, s3, F_SF, 1);
    }
    IFX(X_PF) {
        emit_pf(dyn, ninst, s1, s3, s4);
    }
}

// emit NEG32 instruction, from s1, store result in s1 using s3 and s4 as scratch
void emit_neg32(dynarec_arm_t* dyn, int ninst, int s1, int s3, int s4)
{
    IFX(X_PEND) {
        STR_IMM9(s1, xEmu, offsetof(x86emu_t, op1));
        SET_DF(s3, d_neg32);
    } else IFX(X_ALL) {
        SET_DFNONE(s3);
    }
    IFX(X_ZF|X_CF) {
        BIC_IMM8(xFlags, xFlags, (1<<F_ZF)|(1<<F_CF), 0);
    }
    IFX(X_CF) {
        TSTS_REG_LSL_IMM5(s1, s1, 0);
        ORR_IMM8_COND(cNE, xFlags, xFlags, 1<<F_CF, 0);
    }
    IFX(X_AF) {
        MOV_REG_LSL_IMM5(s3, s1, 0);
    }
    IFX(X_ZF|X_OF) {
        RSBS_IMM8(s1, s1, 0);
    } else {
        RSB_IMM8(s1, s1, 0);
    }
    IFX(X_PEND) {
        STR_IMM9(s1, xEmu, offsetof(x86emu_t, res));
    }
    IFX(X_ZF) {
        ORR_IMM8_COND(cEQ, xFlags, xFlags, 1<<F_ZF, 0);
    }
    IFX(X_OF) {
        ORR_IMM8_COND(cVS, xFlags, xFlags, 0b10, 0x0b);
        BIC_IMM8_COND(cVC, xFlags, xFlags, 0b10, 0x0b);
    }
    IFX(X_AF) {
        ORR_REG_LSL_IMM5(s3, s3, s1, 0);                        // bc = op1 | res
        MOV_REG_LSR_IMM5(s4, s3, 3);
        BFI(xFlags, s4, F_AF, 1);    // AF: bc & 0x08
    }
    IFX(X_SF) {
        MOV_REG_LSR_IMM5(s3, s1, 31);
        BFI(xFlags, s3, F_SF, 1);
    }
    IFX(X_PF) {
        emit_pf(dyn, ninst, s1, s3, s4);
    }
}

// emit NEG16 instruction, from s1, store result in s1 using s3 and s4 as scratch
void emit_neg16(dynarec_arm_t* dyn, int ninst, int s1, int s3, int s4)
{
    IFX(X_PEND) {
        STR_IMM9(s1, xEmu, offsetof(x86emu_t, op1));
        SET_DF(s3, d_neg16);
    } else IFX(X_ALL) {
        SET_DFNONE(s3);
    }
    IFX(X_ZF|X_CF) {
        BIC_IMM8(xFlags, xFlags, (1<<F_ZF)|(1<<F_CF), 0);
    }
    IFX(X_CF) {
        TSTS_REG_LSL_IMM5(s1, s1, 0);
        ORR_IMM8_COND(cNE, xFlags, xFlags, 1<<F_CF, 0);
    }
    IFX(X_AF|X_OF) {
        MOV_REG_LSL_IMM5(s3, s1, 0);
    }
    RSB_IMM8(s1, s1, 0);
    IFX(X_PEND) {
        STR_IMM9(s1, xEmu, offsetof(x86emu_t, res));
    }
    IFX(X_AF|X_OF) {
        ORR_REG_LSL_IMM5(s3, s3, s1, 0);                        // bc = op1 | res
        IFX(X_AF) {
            MOV_REG_LSR_IMM5(s4, s3, 3);
            BFI(xFlags, s4, F_AF, 1);    // AF: bc & 0x08
        }
        IFX(X_OF) {
            MOV_REG_LSR_IMM5(s4, s3, 14);
            XOR_REG_LSR_IMM8(s4, s4, s4, 1);
            BFI(xFlags, s4, F_OF, 1);    // OF: ((bc >> 14) ^ ((bc>>14)>>1)) & 1
        }
    }
    IFX(X_ZF) {
        UXTH(s1, s1, 0);
        TSTS_REG_LSL_IMM5(s1, s1, 0);
        ORR_IMM8_COND(cEQ, xFlags, xFlags, 1<<F_ZF, 0);
    }
    IFX(X_SF) {
        MOV_REG_LSR_IMM5(s3, s1, 15);
        BFI(xFlags, s3, F_SF, 1);
    }
    IFX(X_PF) {
        emit_pf(dyn, ninst, s1, s3, s4);
    }
}

// emit NEG8 instruction, from s1, store result in s1 using s3 and s4 as scratch
void emit_neg8(dynarec_arm_t* dyn, int ninst, int s1, int s3, int s4)
{
    IFX(X_PEND) {
        STR_IMM9(s1, xEmu, offsetof(x86emu_t, op1));
        SET_DF(s3, d_neg8);
    } else IFX(X_ALL) {
        SET_DFNONE(s3);
    }
    IFX(X_ZF|X_CF) {
        BIC_IMM8(xFlags, xFlags, (1<<F_ZF)|(1<<F_CF), 0);
    }
    IFX(X_CF) {
        TSTS_REG_LSL_IMM5(s1, s1, 0);
        ORR_IMM8_COND(cNE, xFlags, xFlags, 1<<F_CF, 0);
    }
    IFX(X_AF|X_OF) {
        MOV_REG_LSL_IMM5(s3, s1, 0);
    }
    RSB_IMM8(s1, s1, 0);
    IFX(X_PEND) {
        STR_IMM9(s1, xEmu, offsetof(x86emu_t, res));
    }
    IFX(X_AF|X_OF) {
        ORR_REG_LSL_IMM5(s3, s3, s1, 0);                        // bc = op1 | res
        IFX(X_AF) {
            MOV_REG_LSR_IMM5(s4, s3, 3);
            BFI(xFlags, s4, F_AF, 1);    // AF: bc & 0x08
        }
        IFX(X_OF) {
            MOV_REG_LSR_IMM5(s4, s3, 6);
            XOR_REG_LSR_IMM8(s4, s4, s4, 1);
            BFI(xFlags, s4, F_OF, 1);    // OF: ((bc >> 6) ^ ((bc>>6)>>1)) & 1
        }
    }
    IFX(X_ZF) {
        ANDS_IMM8(s1, s1, 0xff);
        ORR_IMM8_COND(cEQ, xFlags, xFlags, 1<<F_ZF, 0);
    }
    IFX(X_SF) {
        MOV_REG_LSR_IMM5(s3, s1, 7);
        BFI(xFlags, s3, F_SF, 1);
    }
    IFX(X_PF) {
        emit_pf(dyn, ninst, s1, s3, s4);
    }
}
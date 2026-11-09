"""Lift insns.bin with Hookshot's own lifter (a64-kit a64emit + nx2/rtgen's 64-bit guest window) and write:
  gen/a64.h, gen/a64_suyu.h, gen/a64_suyu.c, gen/a64_fpx.h  - the runtime the lifted code needs (Hookshot's)
  gen/lifted.c  - one C function per instruction + tables
  gen/native.S  - one stub per instruction that runs the ORIGINAL instruction on the CPU (the reference)
Usage: python gen.py <hookshot clone> <insns.bin> <out dir>
"""
import os
import struct
import sys

hook, binpath, out = sys.argv[1], sys.argv[2], sys.argv[3]
sys.path.insert(0, os.path.join(hook, "nx2"))
import kit  # noqa: E402  (puts a64-kit lift/tools/rt on sys.path)
import rtgen  # noqa: E402  (nx2/rtgen.py: Hookshot's a64.h with A64_ADDR64)
import a64emit, a64_lift, a64_rtgen  # noqa: E402
import capstone  # noqa: E402

PC = 0x10000
words = list(struct.unpack("<%dI" % (os.path.getsize(binpath) // 4), open(binpath, "rb").read()))
os.makedirs(out, exist_ok=True)
rtgen.write(out)          # a64.h + a64_rt.c, 64-bit window
a64_rtgen.write(out)      # a64_suyu.h / a64_suyu.c
texts, flags = a64emit.translate([(PC + 4 * i, w) for i, w in enumerate(words)])
used = set()
texts = [a64_lift.outline_fpx(t, used) for t in texts]
open(os.path.join(out, "a64_fpx.h"), "w", newline="\n").write(a64_lift.fpx_header(used))

md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
names = []
for i, w in enumerate(words):
    ins = next(md.disasm(struct.pack("<I", w), PC + 4 * i), None)
    names.append(("%s %s" % (ins.mnemonic, ins.op_str)) if ins else "?")

c = ['#include "a64.h"', "#include <stdint.h>"]
for i, (t, f) in enumerate(zip(texts, flags)):
    c.append("static void lift%d(Ctx *__restrict c) { %s }" % (i, "c->halted = 7;" if f & a64emit.UNHANDLED else t))
for i in range(len(words)):
    c.append("void pt_native_%d(void *s);" % i)
c.append("const int PT_N = %d;" % len(words))
c.append("void (*const PT_LIFT[])(Ctx *) = {%s};" % ",".join("lift%d" % i for i in range(len(words))))
c.append("void (*const PT_NATIVE[])(void *) = {%s};" % ",".join("pt_native_%d" % i for i in range(len(words))))
c.append("const int PT_FLAGS[] = {%s};" % ",".join(str(f) for f in flags))
c.append("const unsigned PT_WORD[] = {%s};" % ",".join("0x%08x" % w for w in words))
c.append("const char *const PT_NAME[] = {%s};" % ",".join('"%s"' % n.replace('"', "'") for n in names))
# what lifted code calls besides the inlined runtime (as tests/insn_test.py stubs them)
c.append(r"""
void a64_trap(Ctx *c, uint64_t pc, uint32_t insn) { (void)pc; (void)insn; c->halted = 9; }
void recomp_unhandled(GuestContext *c, uint32_t i, uint64_t pc) { (void)i; (void)pc; c->halted = 7; }
void recomp_barrier(void) {}
void recomp_ic_ivau(GuestContext *c, uint64_t a, int v) { (void)c; (void)a; (void)v; }
uint64_t recomp_cntpct(GuestContext *c) { (void)c; return 0; }
uint64_t recomp_ldxr(GuestContext *c, uint64_t a, uint32_t s) { uint64_t v = 0; memcpy(&v, A64_PTR(a), s); return v; }
uint32_t recomp_stxr(GuestContext *c, uint64_t a, uint32_t s, uint64_t v) { memcpy(A64_PTR(a), &v, s); return 0; }
void recomp_ldxp(GuestContext *c, uint64_t a, uint32_t s, uint64_t *lo, uint64_t *hi) { *lo = *hi = 0; memcpy(lo, A64_PTR(a), s); memcpy(hi, A64_PTR(a + s), s); }
uint32_t recomp_stxp(GuestContext *c, uint64_t a, uint32_t s, uint64_t lo, uint64_t hi) { memcpy(A64_PTR(a), &lo, s); memcpy(A64_PTR(a + s), &hi, s); return 0; }
void recomp_clrex(GuestContext *c) { (void)c; }
""")
open(os.path.join(out, "lifted.c"), "w", newline="\n").write("\n".join(c))

# native stubs: x9 = state (x[8] | nzcv fpcr fpsr pad | v[8][2]); the instruction only uses x0-x7, v0-v7
S = ["    .text"]
for i, w in enumerate(words):
    S += ["    .globl _pt_native_%d" % i, "    .p2align 2", "_pt_native_%d:" % i,
          "    mov x9, x0",
          "    ldr x10, [x9, #64]", "    msr nzcv, x10",
          "    ldr x10, [x9, #72]", "    msr fpcr, x10",
          "    ldr x10, [x9, #80]", "    msr fpsr, x10",
          "    ldp q0, q1, [x9, #96]", "    ldp q2, q3, [x9, #128]",
          "    ldp q4, q5, [x9, #160]", "    ldp q6, q7, [x9, #192]",
          "    ldp x0, x1, [x9]", "    ldp x2, x3, [x9, #16]", "    ldp x4, x5, [x9, #32]", "    ldp x6, x7, [x9, #48]",
          "    .long 0x%08x" % w,
          "    stp x0, x1, [x9]", "    stp x2, x3, [x9, #16]", "    stp x4, x5, [x9, #32]", "    stp x6, x7, [x9, #48]",
          "    mrs x10, nzcv", "    str x10, [x9, #64]",
          "    mrs x10, fpcr", "    str x10, [x9, #72]",
          "    mrs x10, fpsr", "    str x10, [x9, #80]",
          "    stp q0, q1, [x9, #96]", "    stp q2, q3, [x9, #128]",
          "    stp q4, q5, [x9, #160]", "    stp q6, q7, [x9, #192]",
          "    msr fpcr, xzr", "    ret"]
open(os.path.join(out, "native.S"), "w", newline="\n").write("\n".join(S) + "\n")
print("lifted %d instructions, %d not translated" % (len(words), sum(1 for f in flags if f & a64emit.UNHANDLED)))

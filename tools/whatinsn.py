#!/usr/bin/env python3
"""Identify the instruction at a kernel virtual address, and classify it."""
import struct, sys
d=open('firmware/kernel.macho','rb').read()
va=int(sys.argv[1],16)
off=va-0xc0008000                      # __TEXT vmaddr, fileoff 0
w,=struct.unpack_from('<I',d,off)
print(f"0x{va:08x} = {w:08x}")
cond=w>>28
def cls(w):
    if (w>>28)==0xF:
        if (w & 0xfff1fe20)==0xf1000000: return "CPS (unconditional)"
        if (w & 0x0c00f000)==0x0400f000: return "PLD"
        if (w & 0xffffff00)==0xf1010000: return "SETEND"
        if (w & 0xfe000000)==0xfa000000: return "BLX immediate"
        if (w & 0x0e500000)==0x08100000: return "RFE"
        if (w & 0x0e5000000)==0x084000000: return "SRS"
        return "other unconditional-space encoding"
    if (w & 0x0e000000)==0x06000000 and (w & 0x10): return "media instruction (REV/UXTB/SEL/...)"
    if (w & 0x0f000010)==0x0e000010: 
        return f"MCR/MRC coproc {(w>>8)&0xf} CRn={(w>>16)&0xf} opc1={(w>>21)&7} CRm={w&0xf} opc2={(w>>5)&7}"
    if (w & 0x0f000000)==0x0e000000: return f"CDP coproc {(w>>8)&0xf}"
    if (w & 0x0e000000)==0x0c000000: return f"LDC/STC coproc {(w>>8)&0xf}"
    if (w & 0x0fb00000)==0x01000000: return "MRS"
    if (w & 0x0f0000f0)==0x01200070: return "BKPT"
    if (w & 0x0fe000f0)==0x00800090: return "UMULL/UMLAL"
    if (w & 0x0fe000f0)==0x00c00090: return "SMULL/SMLAL"
    if (w & 0x0fb000f0)==0x01000090: return "SWP/SWPB"
    if (w & 0x0ff000f0)==0x01600010: return "CLZ"
    if (w & 0x0e000090)==0x00000090: return "extra load/store or multiply"
    return "?"
print("  ->", cls(w))
print("  context:")
for i in range(-3,4):
    a=off+i*4
    ww,=struct.unpack_from('<I',d,a)
    print(f"    0x{0xc0008000+a:08x}: {ww:08x} {'<<<' if i==0 else ''}")

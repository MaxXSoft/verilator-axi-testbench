  .section .text, "ax", @progbits
  .globl _start
_start:
  csrr t0, mcause
  fence.i
  amoadd.w zero, zero, (zero)
  mul zero, zero, zero
#ifdef AXI_TB_PROBE_RVC
  c.nop
#endif
1:
  j 1b

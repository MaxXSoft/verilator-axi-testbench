  .section .text, "ax", @progbits
  .globl _start
_start:
  csrr t0, mcause
  fence.i
  amoadd.w zero, zero, (zero)
  mul zero, zero, zero
1:
  j 1b

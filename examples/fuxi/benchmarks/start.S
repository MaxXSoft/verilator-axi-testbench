  .section .text.init, "ax", @progbits
  .align 2
  .globl _start
_start:
  la sp, __stack_top
  la t0, trap
  csrw mtvec, t0
  call main
  tail benchmark_exit
  .align 2
trap:
  csrr a0, mcause
  li t0, 0x80000000
  or a0, a0, t0
  tail benchmark_exit

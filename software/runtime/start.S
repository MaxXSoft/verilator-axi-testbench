#include "axi_tb_platform.h"

  .section .text.init, "ax", @progbits
  .align 2
  .globl _start
  .type _start, @function
_start:
  /* Fuxi resets in M-mode.  The test environment intentionally initializes
     only mtvec; ELF loading has already initialized RAM and BSS. */
  la t0, axi_tb_trap_vector
  csrw mtvec, t0
  j axi_tb_test_entry
  .size _start, . - _start

  .align 2
  .globl axi_tb_trap_vector
  .type axi_tb_trap_vector, @function
axi_tb_trap_vector:
  csrr t0, mcause
  li t1, 0x80000000
  or t0, t0, t1
  li t1, AXI_TB_EXIT_BASE
  sw t0, 0(t1)
1:
  j 1b
  .size axi_tb_trap_vector, . - axi_tb_trap_vector

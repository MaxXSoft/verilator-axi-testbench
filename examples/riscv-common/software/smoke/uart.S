#include "riscv_test.h"

RVTEST_CODE_BEGIN
  li t0, AXI_TB_UART_BASE

  /* Exercise QEMU-virt/16550 LSR.THRE before every transmitted byte. */
  .macro putc value
1:
  lbu t2, 5(t0)
  andi t2, t2, 0x20
  beqz t2, 1b
  li t1, \value
  sb t1, 0(t0)
  .endm

  putc 'F'
  putc 'u'
  putc 'x'
  putc 'i'
  putc '\n'

  /* LSR.DR must track input availability and RBR must pop one byte. */
2:
  lbu t2, 5(t0)
  andi t2, t2, 1
  beqz t2, 2b
  lbu t3, 0(t0)
  li t4, 'Z'
  bne t3, t4, 3f
  RVTEST_PASS
3:
  li TESTNUM, 1
  RVTEST_FAIL
RVTEST_CODE_END

#include "riscv_test.h"

RVTEST_CODE_BEGIN
  la t0, mmio_irq_handler
  csrw mtvec, t0
  csrw mscratch, zero

  /* Enable machine software interrupts.  The adapter's opt-in test hook
     raises irq_soft only after the marked UART W beat is accepted. */
  li t0, 0x8
  csrw mie, t0
  csrs mstatus, t0

  li t0, AXI_TB_UART_BASE
  li t1, 'Q'
  sb t1, 0(t0)

  /* The precise interrupt resumes here. */
  csrr t0, mscratch
  li t1, 1
  bne t0, t1, mmio_irq_fail
  RVTEST_PASS

  .align 2
mmio_irq_handler:
  csrr t0, mcause
  li t1, 0x80000003         /* machine software interrupt */
  bne t0, t1, mmio_irq_fail

  csrr t0, mscratch
  addi t0, t0, 1
  csrw mscratch, t0

  /* Writing UART MCR clears the adapter's held test IRQ. */
  li t0, AXI_TB_UART_BASE
  sb zero, 4(t0)
  mret

mmio_irq_fail:
  li TESTNUM, 5
  RVTEST_FAIL
RVTEST_CODE_END

#include "riscv_test.h"

RVTEST_CODE_BEGIN
  /* Delegate supervisor software interrupts and enter S-mode. */
  li t0, 0x2
  csrw mideleg, t0
  csrw mie, t0
  la t0, supervisor_entry
  csrw mepc, t0
  li t0, 0x880              /* MPP=S, MPIE=1 */
  csrw mstatus, t0
  mret

supervisor_entry:
  la t0, supervisor_trap
  csrw stvec, t0

  /* Make SSIP eligible, then execute SRET in the same pending state.  A
     correct implementation completes SRET to U-mode before taking SSIP. */
  li t0, 0x2
  csrw sip, t0
  li t1, 0x22               /* SIE=1, SPIE=1, SPP=U */
  la t0, user_target
  csrw sepc, t0
  csrw sstatus, t1
  sret

user_target:
  /* The pending interrupt must trap before this instruction retires. */
  li TESTNUM, 1
  j xret_irq_fail

  .align 2
supervisor_trap:
  li TESTNUM, 2
  csrr t0, scause
  li t1, 0x80000001         /* supervisor software interrupt */
  bne t0, t1, xret_irq_fail

  /* SPP records the interrupted privilege.  It must be U, proving that SRET
     completed before the pending interrupt was accepted. */
  li TESTNUM, 3
  csrr t0, sstatus
  andi t0, t0, 0x100
  bnez t0, xret_irq_fail

  li TESTNUM, 4
  csrr t0, sepc
  la t1, user_target
  bne t0, t1, xret_irq_fail

  csrw sip, zero
  RVTEST_PASS

xret_irq_fail:
  RVTEST_FAIL
RVTEST_CODE_END

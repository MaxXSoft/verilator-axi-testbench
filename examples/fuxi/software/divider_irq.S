#include "riscv_test.h"
#include "fuxi_platform.h"
/* Division reuse must survive neither reset nor a pipeline flush. In
   particular, a timer/interrupt between DIV and REM must not reuse zeros. */
RVTEST_CODE_BEGIN
  li TESTNUM, 4
  divu t0, zero, zero
  li t1, -1
  bne t0, t1, fail
  remu t0, zero, zero
  bnez t0, fail
  la t0, handler
  csrw mtvec, t0
  li s0, 0
  li s1, 100
  li s2, 7
  li s3, 14
  li TESTNUM, 1
  divu t0, s1, s2
  bne t0, s3, fail
  li t0, 8
  csrw mie, t0
  csrsi mstatus, 8
  li t0, FUXI_CLINT_BASE
  li t1, 1
  sw t1, 0(t0)
1: beqz s0, 1b
  li TESTNUM, 2
  divu t0, s1, s2
  bne t0, s3, fail
  li TESTNUM, 3
  remu t0, s1, s2
  li t1, 2
  bne t0, t1, fail
  li TESTNUM, 5
  fence.i
  divu t0, s1, s2
  bne t0, s3, fail
  li TESTNUM, 6
  sfence.vma
  remu t0, s1, s2
  li t1, 2
  bne t0, t1, fail
  RVTEST_PASS
handler:
  csrr t0, mcause
  li t1, 0x80000003
  bne t0, t1, fail
  li t0, FUXI_CLINT_BASE
  sw zero, 0(t0)
  li s0, 1
  mret
fail:
  RVTEST_FAIL
RVTEST_CODE_END

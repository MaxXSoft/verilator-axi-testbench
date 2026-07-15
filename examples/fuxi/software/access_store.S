#include "riscv_test.h"

RVTEST_CODE_BEGIN
  /* This lies in Fuxi's uncached region but outside the UART and Exit
     devices.  Completion must wait for the DECERR BRESP and trap with cause
     7, rather than retiring as soon as the W beat is accepted. */
  li t0, 0x10002000
  sw zero, 0(t0)

  li TESTNUM, 7
  RVTEST_FAIL
RVTEST_CODE_END

#include "riscv_test.h"

RVTEST_CODE_BEGIN
  /* SFENCE.VMA is not a demand access.  Its rs1 value must not route D-cache
     maintenance completion through the uncached data path. */
  li t0, 0x10000000
  sfence.vma t0, zero
  RVTEST_PASS
RVTEST_CODE_END

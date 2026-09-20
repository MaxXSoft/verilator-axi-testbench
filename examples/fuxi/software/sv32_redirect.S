#include "riscv_test.h"

/* The branch sits at the end of a virtual page. Sequential fetch starts a
   walk using root[256], then JR redirects to a different root entry (257).
   That in-flight walk must neither mix VPNs nor fill the target's TLB entry. */
RVTEST_CODE_BEGIN
  la t0, trap
  csrw mtvec, t0
  la t0, root
  li t1, 0xcf
  sw t1, 0(t0)
  /* Identity-map the standard Fuxi MMIO superpage for guest exit. */
  li t1, 0x040000c7
  sw t1, 256(t0)
  li t1, 0x200000cf
  li t2, 2048
  add t2, t0, t2
  sw t1, 0(t2)
  la t1, leaves
  srli t2, t1, 12
  slli t2, t2, 10
  ori t2, t2, 1
  sw t2, 1024(t0)
  la t2, page_a
  srli t2, t2, 12
  slli t2, t2, 10
  ori t2, t2, 0xcf
  sw t2, 0(t1)
  addi t2, t2, 1024
  sw t2, 4(t1)
  addi t2, t2, 1024
  sw t2, 8(t1)
  la t2, target_leaves
  srli t3, t2, 12
  slli t3, t3, 10
  ori t3, t3, 1
  sw t3, 1028(t0)
  la t3, page_c
  srli t3, t3, 12
  slli t3, t3, 10
  ori t3, t3, 0xcf
  sw t3, 0(t2)
  li s0, 0x40400000
  sfence.vma
  srli t0, t0, 12
  li t1, 0x80000000
  or t0, t0, t1
  csrw satp, t0
  li t0, 0x40000ffc
  csrw mepc, t0
  li t0, 0x800
  csrw mstatus, t0
  mret
trap:
  csrr t1, mcause
  addi t1, t1, 128
  li t0, AXI_TB_EXIT_BASE
  sw t1, 0(t0)
1: j 1b
.section .data
.balign 4096
page_a:
  .space 4092
  jr s0
/* A wrong translation must fail even if execution lands on page B. */
page_b:
  li t0, AXI_TB_EXIT_BASE
  li t1, 1
  sw t1, 0(t0)
1: j 1b
.balign 4096
/* Correct translation of 0x40400000 reaches this page. */
page_c:
  li t0, AXI_TB_EXIT_BASE
  sw zero, 0(t0)
1: j 1b
.section .bss
.balign 4096
root: .zero 4096
leaves: .zero 4096
target_leaves: .zero 4096

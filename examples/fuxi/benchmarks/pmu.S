/* Fixed instruction layout for every group and ISA variant. Configuration and
 * snapshots run while all measured counters are frozen. The boundary is the
 * commit of each mcountinhibit write; hook call/return bookkeeping is included. */
  .section .text.pmu, "ax", @progbits
  .option push
  .option norvc
  .align 2
  .globl benchmark_start
benchmark_start:
  li t1, 0x7fd
  csrw mcountinhibit, t1
  csrr t2, mcountinhibit
  bne t2, t1, .Lpmu_unavailable
  la t0, benchmark_pmu_events
  .set slot, 3
  .rept 8
  lw t1, (slot - 3) * 4(t0)
  csrw 0x320 + slot, t1
  csrr t2, 0x320 + slot
  bne t2, t1, .Lpmu_unavailable
  csrw 0xb00 + slot, zero
  csrw 0xb80 + slot, zero
  .set slot, slot + 1
  .endr
  csrw mcycle, zero
  csrw mcycleh, zero
  csrw minstret, zero
  csrw minstreth, zero
  csrw mcountinhibit, zero
  ret

.Lpmu_unavailable:
  li a0, 5
  tail benchmark_exit

  .globl benchmark_stop
benchmark_stop:
  li t1, 0x7fd
  csrw mcountinhibit, t1
  la t0, benchmark_pmu_counters
  csrr t1, mcycle
  sw t1, 0(t0)
  csrr t1, mcycleh
  sw t1, 4(t0)
  csrr t1, minstret
  sw t1, 8(t0)
  csrr t1, minstreth
  sw t1, 12(t0)
  .set slot, 3
  .rept 8
  csrr t1, 0xb00 + slot
  sw t1, (slot - 1) * 8(t0)
  csrr t1, 0xb80 + slot
  sw t1, (slot - 1) * 8 + 4(t0)
  .set slot, slot + 1
  .endr
  ret
  .option pop

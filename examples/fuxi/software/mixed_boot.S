  .section .text.init, "ax", @progbits
  .word 0x544f4f42          /* BOOT: checked by the ELF payload. */
  .org AXI_TB_RESET_PC - AXI_TB_ROM_BASE
  li t0, AXI_TB_RAM_BASE
  jr t0

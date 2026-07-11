#ifndef AXI_TB_PLATFORM_H
#define AXI_TB_PLATFORM_H

/*
 * The CMake software helpers provide these values on every compile command.
 * Keeping them as ordinary preprocessor definitions makes the same address
 * contract available to handwritten assembly, imported riscv-tests, and C.
 */
#ifndef AXI_TB_ROM_BASE
#error "AXI_TB_ROM_BASE must be provided by the software build"
#endif
#ifndef AXI_TB_ROM_SIZE
#error "AXI_TB_ROM_SIZE must be provided by the software build"
#endif
#ifndef AXI_TB_RAM_BASE
#error "AXI_TB_RAM_BASE must be provided by the software build"
#endif
#ifndef AXI_TB_RAM_SIZE
#error "AXI_TB_RAM_SIZE must be provided by the software build"
#endif
#ifndef AXI_TB_UART_BASE
#error "AXI_TB_UART_BASE must be provided by the software build"
#endif
#ifndef AXI_TB_EXIT_BASE
#error "AXI_TB_EXIT_BASE must be provided by the software build"
#endif
#ifndef AXI_TB_RESET_PC
#error "AXI_TB_RESET_PC must be provided by the software build"
#endif

#ifndef __ASSEMBLER__
#include <stdint.h>

static inline void axi_tb_uart_putc(uint8_t value) {
  *(volatile uint8_t *)(uintptr_t)AXI_TB_UART_BASE = value;
}

static inline __attribute__((noreturn)) void axi_tb_exit(uint32_t code) {
  *(volatile uint32_t *)(uintptr_t)AXI_TB_EXIT_BASE = code;
  for (;;) {
  }
}
#endif

#endif /* AXI_TB_PLATFORM_H */

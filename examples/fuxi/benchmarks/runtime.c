#include <stdarg.h>

#include "benchmark.h"
#include "stdio.h"

static uint64_t start_cycles, start_instructions, elapsed_cycles,
    elapsed_instructions;

static uint64_t cycles(void) {
  uint32_t high, low, check;
  do {
    __asm__ volatile("rdcycleh %0; rdcycle %1; rdcycleh %2"
                     : "=r"(high), "=r"(low), "=r"(check)::"memory");
  } while (high != check);
  return ((uint64_t)high << 32) | low;
}

static uint64_t instructions(void) {
  uint32_t high, low, check;
  do {
    __asm__ volatile("rdinstreth %0; rdinstret %1; rdinstreth %2"
                     : "=r"(high), "=r"(low), "=r"(check)::"memory");
  } while (high != check);
  return ((uint64_t)high << 32) | low;
}

void benchmark_start(void) {
  start_cycles = cycles();
  start_instructions = instructions();
}

void benchmark_stop(void) {
  uint64_t end_cycles = cycles();
  uint64_t end_instructions = instructions();
  elapsed_cycles = end_cycles - start_cycles;
  elapsed_instructions = end_instructions - start_instructions;
}

uint64_t benchmark_cycles(void) { return elapsed_cycles; }

void benchmark_report(const char *name, unsigned iterations) {
  printf(
      "BENCH_RESULT name=%s iterations=%u cycles=%08x%08x instret=%08x%08x\n",
      name, iterations, (unsigned)(elapsed_cycles >> 32),
      (unsigned)elapsed_cycles, (unsigned)(elapsed_instructions >> 32),
      (unsigned)elapsed_instructions);
}

void benchmark_exit(unsigned code) {
  *(volatile uint32_t *)0x10001000 = code;
  for (;;) {
  }
}

static void putchar_uart(char value) { *(volatile char *)0x10000000 = value; }

static int print_number(unsigned value, unsigned base, int width,
                        char padding) {
  char digits[32];
  int count = 0, length;
  do {
    digits[count++] = "0123456789abcdef"[value % base];
    value /= base;
  } while (value);
  length = count > width ? count : width;
  while (width-- > count) putchar_uart(padding);
  while (count) putchar_uart(digits[--count]);
  return length;
}

/* Only the integer/string conversion subset used by the two upstream guests. */
static int print_format(const char *format, va_list arguments) {
  int count = 0;
  while (*format) {
    char conversion, padding = ' ';
    int width = 0;
    if (*format != '%') {
      putchar_uart(*format++);
      ++count;
      continue;
    }
    ++format;
    if (*format == '0') {
      padding = '0';
      ++format;
    }
    while (*format >= '0' && *format <= '9')
      width = width * 10 + *format++ - '0';
    if (*format == 'l') ++format; /* RV32 int and long both have 32 bits. */
    conversion = *format++;
    if (conversion == 's') {
      const char *text = va_arg(arguments, const char *);
      while (*text) {
        putchar_uart(*text++);
        ++count;
      }
    } else if (conversion == 'c' || conversion == '%') {
      putchar_uart(conversion == '%' ? '%' : (char)va_arg(arguments, int));
      ++count;
    } else if (conversion == 'd' || conversion == 'u' || conversion == 'x') {
      unsigned value = va_arg(arguments, unsigned);
      if (conversion == 'd' && (int)value < 0) {
        putchar_uart('-');
        ++count;
        value = 0u - value;
      }
      count += print_number(value, conversion == 'x' ? 16 : 10, width, padding);
    } else {
      benchmark_exit(2);
    }
  }
  return count;
}

int printf(const char *format, ...) {
  va_list arguments;
  int count;
  va_start(arguments, format);
  count = print_format(format, arguments);
  va_end(arguments);
  return count;
}

int ee_printf(const char *format, ...) {
  va_list arguments;
  int count;
  va_start(arguments, format);
  count = print_format(format, arguments);
  va_end(arguments);
  return count;
}

int puts(const char *text) { return printf("%s\n", text); }

void *memcpy(void *destination, const void *source, size_t size) {
  unsigned char *dst = destination;
  const unsigned char *src = source;
  while (size--) *dst++ = *src++;
  return destination;
}

void *memset(void *destination, int value, size_t size) {
  unsigned char *dst = destination;
  while (size--) *dst++ = (unsigned char)value;
  return destination;
}

char *strcpy(char *destination, const char *source) {
  char *start = destination;
  do {
    *destination = *source++;
  } while (*destination++);
  return start;
}

int strcmp(const char *left, const char *right) {
  while (*left && *left == *right) {
    ++left;
    ++right;
  }
  return (unsigned char)*left - (unsigned char)*right;
}

/* Dhrystone allocates exactly two records before the measurement starts. */
char *malloc(unsigned size) {
  static unsigned char heap[4096] __attribute__((aligned(16)));
  static unsigned used;
  unsigned offset = used;
  size = (size + 15u) & ~15u;
  if (size > sizeof(heap) - used) benchmark_exit(3);
  used += size;
  return (char *)heap + offset;
}

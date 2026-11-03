/*
 * PHYLAX Snap - RS485 transport of the block bus. See phylax_uart.h.
 *
 * This library is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "phylax_uart.h"

#include <libopencm3/cm3/nvic.h>
#include <libopencm3/stm32/gpio.h>
#include <libopencm3/stm32/rcc.h>
#include <libopencm3/stm32/usart.h>

#include "phylax_bus.h"
#include "timer.h"

// The SysTick counter behind timer_ms(). In the firmware timer_ms() is a
// supervisor call (SVC), and an SVC from inside an interrupt handler of the
// same priority escalates to a HardFault, so the interrupt reads it directly.
extern volatile uint32_t system_millis;

// The clock set up by the bootloader (setup.c): 120 MHz, APB1 = 120 / 4.
#define APB1_HZ 30000000
// Oversampling by 16: BRR = APB1 / baud = 120 = 7.5 -> exactly 250000 baud.
#define BRR_VALUE (APB1_HZ / PHYLAX_UART_BAUD)

#define DE_PORT GPIOA
#define DE_PIN GPIO1

// Before sending, the line must have been quiet this long, so we do not
// talk over the end of someone else's frame (timer_ms() ticks are 1 ms, so
// 2 means at least 1 ms of silence).
#define QUIET_MS 2
#define QUIET_WAIT_MAX_MS 20
#define TX_TIMEOUT_MS 50

#define RX_RING_SIZE 2048  // ~80 ms of continuous traffic at 250 kbit/s

static volatile uint8_t rx_ring[RX_RING_SIZE];
static volatile uint16_t rx_head = 0;  // written by the interrupt
static volatile uint16_t rx_tail = 0;  // written by the main loop
static volatile uint32_t rx_last_ms = 0;
static volatile uint32_t rx_dropped = 0;  // ring full, overrun or bad byte
static bool ready = false;

bool phylax_uart_ready(void) { return ready; }

void phylax_uart_init(void) {
  if (phylax_role() == PHYLAX_ROLE_STOCK) return;

  // DE and /RE low: receive. R8 already holds it low until now.
  gpio_clear(DE_PORT, DE_PIN);
  gpio_mode_setup(DE_PORT, GPIO_MODE_OUTPUT, GPIO_PUPD_NONE, DE_PIN);

  // RX gets a pull-up: while we send, /RE is high and RO floats.
  gpio_mode_setup(GPIOA, GPIO_MODE_AF, GPIO_PUPD_NONE, GPIO2);
  gpio_mode_setup(GPIOA, GPIO_MODE_AF, GPIO_PUPD_PULLUP, GPIO3);
  gpio_set_af(GPIOA, GPIO_AF7, GPIO2 | GPIO3);

  rcc_periph_clock_enable(RCC_USART2);
  USART2_CR1 = 0;
  USART2_CR2 = 0;  // 1 stop bit
  USART2_CR3 = 0;  // no flow control
  USART2_BRR = BRR_VALUE;
  USART2_CR1 = USART_CR1_UE | USART_CR1_TE | USART_CR1_RE | USART_CR1_RXNEIE;

  nvic_enable_irq(NVIC_USART2_IRQ);
  ready = true;
}

// Runs privileged (handler mode) even after the firmware dropped privileges.
// Must not make supervisor calls (timer_ms() and the like).
void usart2_isr(void) {
  uint32_t sr = USART2_SR;
  if ((sr & (USART_SR_RXNE | USART_SR_ORE | USART_SR_FE | USART_SR_NE)) == 0) {
    return;
  }
  // Reading DR after SR clears RXNE and the error flags.
  uint8_t byte = (uint8_t)USART2_DR;
  rx_last_ms = system_millis;  // never timer_ms() here, see above
  if (sr & (USART_SR_ORE | USART_SR_FE | USART_SR_NE)) {
    // A damaged or lost byte: the frame's CRC fails and it is dropped; the
    // decoder finds the next 0xAA 0x55.
    rx_dropped++;
    if (sr & USART_SR_ORE) return;
  }
  uint16_t next = (uint16_t)((rx_head + 1) % RX_RING_SIZE);
  if (next == rx_tail) {
    rx_dropped++;
    return;
  }
  rx_ring[rx_head] = byte;
  rx_head = next;
}

void phylax_uart_poll(void) {
  if (!ready) return;
  uint8_t chunk[64];
  for (;;) {
    size_t n = 0;
    uint16_t head = rx_head;
    while (rx_tail != head && n < sizeof(chunk)) {
      chunk[n++] = rx_ring[rx_tail];
      rx_tail = (uint16_t)((rx_tail + 1) % RX_RING_SIZE);
    }
    if (n == 0) return;
    phylax_bus_rx(chunk, n);
  }
}

static bool wait_flag(uint32_t flag, uint32_t start_ms) {
  while ((USART2_SR & flag) == 0) {
    if (timer_ms() - start_ms > TX_TIMEOUT_MS) return false;
  }
  return true;
}

bool phylax_uart_send(const uint8_t *data, size_t len) {
  if (!ready || len == 0) return false;

  // Carrier sense: let a frame still on the line finish first.
  uint32_t start = timer_ms();
  while (timer_ms() - rx_last_ms < QUIET_MS &&
         timer_ms() - start < QUIET_WAIT_MAX_MS) {
  }

  gpio_set(DE_PORT, DE_PIN);
  // MAX3485 driver enable time is well under a microsecond; the start bit
  // of the first byte follows anyway.
  bool ok = true;
  start = timer_ms();
  for (size_t i = 0; i < len && ok; i++) {
    ok = wait_flag(USART_SR_TXE, start);
    if (ok) USART2_DR = data[i];
  }
  // The last stop bit must be out before the driver is released.
  if (ok) ok = wait_flag(USART_SR_TC, start);
  gpio_clear(DE_PORT, DE_PIN);
  return ok;
}

/*
 * PHYLAX Snap - RS485 transport of the block bus on real hardware.
 *
 * Board (Phylax_Version_3): MAX3485 on USART2.
 *   PA2 = USART2_TX -> DI      PA3 = USART2_RX <- RO
 *   PA1 = DE and /RE (tied), R8 10k pull-down: receiving unless we drive it
 *   250 kbit/s, 8N1. Termination: JP1 (120R) closed on the main block only.
 *
 * Receiving: the USART2 interrupt puts bytes in a ring buffer in RAM;
 * phylax_uart_poll() hands them to phylax_bus_rx() from the main loop.
 * Sending blocks for the frame (a 64-byte Trezor packet is ~3 ms), with PA1
 * high only while the bytes go out.
 *
 * The UART and its interrupt must be set up while the firmware is still
 * privileged, before mpu_config_firmware() (afterwards the NVIC cannot be
 * reached). Firmware started unprivileged (not signed for the bootloader on
 * the board) has no bus.
 *
 * This library is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef __PHYLAX_UART_H__
#define __PHYLAX_UART_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PHYLAX_UART_BAUD 250000

// Privileged only. Does nothing for stock builds.
void phylax_uart_init(void);
bool phylax_uart_ready(void);
// Main loop: received bytes -> phylax_bus_rx().
void phylax_uart_poll(void);
// One whole frame. False if the bus is not set up.
bool phylax_uart_send(const uint8_t *data, size_t len);

#endif

/*
 * This file is part of the Trezor project, https://trezor.io/
 *
 * Copyright (C) 2017 Saleem Rashid <trezor@saleemrashid.com>
 *
 * This library is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdint.h>
#include <unistd.h>

#include "usb.h"

#include "debug.h"
#include "messages.h"
#include "phylax_bus.h"
#include "phylax_relay.h"
#include "timer.h"

static volatile char tiny = 0;

void usbInit(void) { emulatorSocketInit(); }

#if DEBUG_LINK
#define _ISDBG (((iface == 1) ? 'd' : 'n'))
#else
#define _ISDBG ('n')
#endif

// PHYLAX: one 64-byte Trezor packet, from the USB host or (key block) from
// the bus - both are handled exactly the same way.
void usbDeliverPacket(const uint8_t *packet) {
  if (!tiny) {
    msg_read_common('n', packet, USB_PACKET_SIZE);
  } else {
    msg_read_tiny(packet, USB_PACKET_SIZE);
  }
}

bool phylax_transport_send(uint8_t dst, const uint8_t *data, size_t len) {
  return emulatorBusSend(dst, data, len) == len;
}

static void write_main_out(void) {
  const uint8_t *data;
  while ((data = msg_out_data()) != NULL) {
    if (phylax_role() == PHYLAX_ROLE_KEY && phylax_reply_via_bus()) {
      phylax_bus_send_reply_packet(data);  // answer the main block
    } else {
      emulatorSocketWrite(0, data, USB_PACKET_SIZE);
    }
  }
}

static void handle_input(int iface, uint8_t *buffer, size_t n) {
  if (iface == 2) {
    phylax_bus_rx(buffer, n);
    return;
  }
  if (iface == 0) {
    phylax_note_host_input();
  }
  if (!tiny) {
    msg_read_common(_ISDBG, buffer, USB_PACKET_SIZE);
  } else {
    msg_read_tiny(buffer, USB_PACKET_SIZE);
  }
}

void waitAndProcessUSBRequests(uint32_t millis) {
  emulatorPoll();

  // Large enough for a whole bus frame; USB packets use the first 64 bytes.
  static uint8_t buffer[PHYLAX_FRAME_MAX];

  int iface = 0;
  size_t n = emulatorSocketRead(&iface, buffer, sizeof(buffer), millis);
  if (n > 0) {
    if (!tiny) {
      do {
        handle_input(iface, buffer, n);
      } while ((n = emulatorSocketRead(&iface, buffer, sizeof(buffer), 0)) >
               0);
    } else {
      handle_input(iface, buffer, n);
    }
  }

  if (phylax_role() == PHYLAX_ROLE_MAIN) {
    phylax_relay_poll();
  }

  write_main_out();

#if DEBUG_LINK
  const uint8_t *data;
  while ((data = msg_debug_out_data()) != NULL) {
    emulatorSocketWrite(1, data, USB_PACKET_SIZE);
  }
#endif
}

void usbPoll(void) { waitAndProcessUSBRequests(0); }

char usbTiny(char set) {
  char old = tiny;
  tiny = set;
  return old;
}

void usbFlush(uint32_t millis) {
  write_main_out();
  usleep(millis * 1000);
}

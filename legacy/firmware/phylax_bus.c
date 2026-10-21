/*
 * PHYLAX Blocks - block-to-block bus. See phylax_bus.h.
 *
 * This library is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "phylax_bus.h"

#include <string.h>

#include "phylax_relay.h"
#include "usb.h"

#if EMULATOR
#include <stdlib.h>
#endif

#ifndef PHYLAX_ROLE
#define PHYLAX_ROLE PHYLAX_ROLE_STOCK
#endif

// ------------------------------------------------------------ frame layer

uint16_t phylax_crc16(const uint8_t *data, size_t len, uint16_t crc) {
  // CRC-16/CCITT-FALSE: poly 0x1021, init 0xFFFF, no reflection.
  for (size_t i = 0; i < len; i++) {
    crc ^= (uint16_t)data[i] << 8;
    for (int b = 0; b < 8; b++) {
      crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021)
                           : (uint16_t)(crc << 1);
    }
  }
  return crc;
}

size_t phylax_frame_encode(const PhylaxFrame *f, uint8_t *out,
                           size_t out_size) {
  if (f->len > PHYLAX_MAX_PAYLOAD) return 0;
  size_t total = PHYLAX_HEADER_SIZE + f->len + 2;
  if (out_size < total) return 0;
  out[0] = PHYLAX_SYNC1;
  out[1] = PHYLAX_SYNC2;
  out[2] = f->dst;
  out[3] = f->src;
  out[4] = f->type;
  out[5] = f->seq;
  out[6] = f->flags;
  out[7] = f->len & 0xFF;
  out[8] = f->len >> 8;
  memcpy(out + PHYLAX_HEADER_SIZE, f->payload, f->len);
  uint16_t crc = phylax_crc16(out + 2, PHYLAX_HEADER_SIZE - 2 + f->len, 0xFFFF);
  out[PHYLAX_HEADER_SIZE + f->len] = crc & 0xFF;
  out[PHYLAX_HEADER_SIZE + f->len + 1] = crc >> 8;
  return total;
}

enum {
  DEC_SYNC1,
  DEC_SYNC2,
  DEC_HEADER,
  DEC_PAYLOAD,
  DEC_CRC1,
  DEC_CRC2,
};

void phylax_decoder_reset(PhylaxDecoder *dec) {
  dec->state = DEC_SYNC1;
  dec->pos = 0;
  dec->crc = 0xFFFF;
}

bool phylax_decoder_feed(PhylaxDecoder *dec, uint8_t byte) {
  PhylaxFrame *f = &dec->frame;
  switch (dec->state) {
    case DEC_SYNC1:
      if (byte == PHYLAX_SYNC1) dec->state = DEC_SYNC2;
      return false;
    case DEC_SYNC2:
      if (byte == PHYLAX_SYNC2) {
        dec->state = DEC_HEADER;
        dec->pos = 0;
        dec->crc = 0xFFFF;
      } else if (byte != PHYLAX_SYNC1) {
        dec->state = DEC_SYNC1;
      }
      return false;
    case DEC_HEADER:
      dec->crc = phylax_crc16(&byte, 1, dec->crc);
      switch (dec->pos++) {
        case 0: f->dst = byte; break;
        case 1: f->src = byte; break;
        case 2: f->type = byte; break;
        case 3: f->seq = byte; break;
        case 4: f->flags = byte; break;
        case 5: f->len = byte; break;
        case 6:
          f->len |= (uint16_t)byte << 8;
          if (f->len > PHYLAX_MAX_PAYLOAD) {
            phylax_decoder_reset(dec);  // garbage: resync
            return false;
          }
          dec->pos = 0;
          dec->state = f->len ? DEC_PAYLOAD : DEC_CRC1;
          break;
      }
      return false;
    case DEC_PAYLOAD:
      dec->crc = phylax_crc16(&byte, 1, dec->crc);
      f->payload[dec->pos++] = byte;
      if (dec->pos == f->len) dec->state = DEC_CRC1;
      return false;
    case DEC_CRC1:
      dec->pos = byte;  // low byte of the received CRC
      dec->state = DEC_CRC2;
      return false;
    case DEC_CRC2: {
      uint16_t rx = (uint16_t)dec->pos | ((uint16_t)byte << 8);
      bool ok = (rx == dec->crc);
      phylax_decoder_reset(dec);
      return ok;
    }
  }
  phylax_decoder_reset(dec);
  return false;
}

// ------------------------------------------------------------ this block

#if EMULATOR
static uint8_t env_byte(const char *name, uint8_t fallback) {
  const char *v = getenv(name);
  if (v == NULL || *v == '\0') return fallback;
  return (uint8_t)strtol(v, NULL, 0);
}
#endif

PhylaxRole phylax_role(void) {
#if EMULATOR
  static int role = -1;
  if (role < 0) {
    const char *v = getenv("PHYLAX_ROLE");
    role = PHYLAX_ROLE_STOCK;
    if (v != NULL && strcmp(v, "main") == 0) role = PHYLAX_ROLE_MAIN;
    if (v != NULL && strcmp(v, "key") == 0) role = PHYLAX_ROLE_KEY;
  }
  return (PhylaxRole)role;
#else
  return (PhylaxRole)PHYLAX_ROLE;
#endif
}

uint8_t phylax_my_addr(void) {
#if EMULATOR
  return env_byte("PHYLAX_BUS_ADDR", phylax_role() == PHYLAX_ROLE_MAIN
                                         ? PHYLAX_ADDR_MAIN
                                         : PHYLAX_ADDR_KEY_FIRST);
#else
  return phylax_role() == PHYLAX_ROLE_MAIN ? PHYLAX_ADDR_MAIN
                                           : PHYLAX_ADDR_KEY_FIRST;
#endif
}

uint8_t phylax_key_addr(void) {
#if EMULATOR
  return env_byte("PHYLAX_KEY_ADDR", PHYLAX_ADDR_KEY_FIRST);
#else
  return PHYLAX_ADDR_KEY_FIRST;
#endif
}

#if EMULATOR
bool phylax_emu_autoconfirm(void) {
  static int on = -1;
  if (on < 0) {
    const char *v = getenv("PHYLAX_EMU_AUTOCONFIRM");
    on = (v != NULL && strcmp(v, "1") == 0) ? 1 : 0;
  }
  return on == 1;
}
#endif

// ------------------------------------------------------------ sending

static uint8_t tx_seq = 0;

bool phylax_bus_send(uint8_t dst, uint8_t type, const uint8_t *payload,
                     uint16_t len) {
  static PhylaxFrame f;
  static uint8_t wire[PHYLAX_FRAME_MAX];
  if (len > PHYLAX_MAX_PAYLOAD) return false;
  f.dst = dst;
  f.src = phylax_my_addr();
  f.type = type;
  f.seq = tx_seq++;
  f.flags = 0;
  f.len = len;
  if (len) memcpy(f.payload, payload, len);
  size_t n = phylax_frame_encode(&f, wire, sizeof(wire));
  return n > 0 && phylax_transport_send(dst, wire, n);
}

bool phylax_bus_send_trezor_msg(uint8_t dst, uint16_t msg_id,
                                const uint8_t *data, uint32_t len) {
  uint8_t pkt[USB_PACKET_SIZE];
  uint32_t pos = 0;
  // First packet: '?' '#' '#' id(2, big-endian) len(4, big-endian) data...
  memset(pkt, 0, sizeof(pkt));
  pkt[0] = '?';
  pkt[1] = '#';
  pkt[2] = '#';
  pkt[3] = msg_id >> 8;
  pkt[4] = msg_id & 0xFF;
  pkt[5] = (len >> 24) & 0xFF;
  pkt[6] = (len >> 16) & 0xFF;
  pkt[7] = (len >> 8) & 0xFF;
  pkt[8] = len & 0xFF;
  uint32_t chunk = len < USB_PACKET_SIZE - 9 ? len : USB_PACKET_SIZE - 9;
  if (chunk) memcpy(pkt + 9, data, chunk);
  pos = chunk;
  if (!phylax_bus_send(dst, PHYLAX_MSG_TREZOR, pkt, sizeof(pkt))) return false;
  // Continuation packets: '?' + 63 bytes.
  while (pos < len) {
    memset(pkt, 0, sizeof(pkt));
    pkt[0] = '?';
    chunk = len - pos < USB_PACKET_SIZE - 1 ? len - pos : USB_PACKET_SIZE - 1;
    memcpy(pkt + 1, data + pos, chunk);
    pos += chunk;
    if (!phylax_bus_send(dst, PHYLAX_MSG_TREZOR, pkt, sizeof(pkt))) {
      return false;
    }
  }
  return true;
}

// ------------------------------------------------------------ key block

static bool reply_via_bus = false;
static uint8_t reply_to = PHYLAX_ADDR_MAIN;

bool phylax_reply_via_bus(void) { return reply_via_bus; }

void phylax_note_host_input(void) { reply_via_bus = false; }

void phylax_bus_send_reply_packet(const uint8_t *packet) {
  phylax_bus_send(reply_to, PHYLAX_MSG_TREZOR, packet, USB_PACKET_SIZE);
}

// ------------------------------------------------------------ receiving

// Main block: reassemble Trezor messages coming back from a key block.
static struct {
  bool reading;
  uint16_t id;
  uint32_t size;
  uint32_t pos;
  uint8_t data[4096];
} rx_msg;

static void main_rx_trezor_packet(const uint8_t *p) {
  if (!rx_msg.reading) {
    if (p[0] != '?' || p[1] != '#' || p[2] != '#') return;
    rx_msg.id = ((uint16_t)p[3] << 8) | p[4];
    rx_msg.size = ((uint32_t)p[5] << 24) | ((uint32_t)p[6] << 16) |
                  ((uint32_t)p[7] << 8) | p[8];
    if (rx_msg.size > sizeof(rx_msg.data)) {
      phylax_relay_on_key_error("Key block message too big");
      return;
    }
    uint32_t chunk = rx_msg.size < USB_PACKET_SIZE - 9
                         ? rx_msg.size
                         : USB_PACKET_SIZE - 9;
    memcpy(rx_msg.data, p + 9, chunk);
    rx_msg.pos = chunk;
    rx_msg.reading = true;
  } else {
    if (p[0] != '?') {
      rx_msg.reading = false;
      return;
    }
    uint32_t chunk = rx_msg.size - rx_msg.pos < USB_PACKET_SIZE - 1
                         ? rx_msg.size - rx_msg.pos
                         : USB_PACKET_SIZE - 1;
    memcpy(rx_msg.data + rx_msg.pos, p + 1, chunk);
    rx_msg.pos += chunk;
  }
  if (rx_msg.pos >= rx_msg.size) {
    rx_msg.reading = false;
    phylax_relay_on_key_message(rx_msg.id, rx_msg.data, rx_msg.size);
  }
}

static void on_frame(const PhylaxFrame *f) {
  uint8_t me = phylax_my_addr();
  if (f->dst != me && f->dst != PHYLAX_ADDR_BROADCAST) return;

  switch (f->type) {
    case PHYLAX_MSG_PING:
      phylax_bus_send(f->src, PHYLAX_MSG_PONG, f->payload, f->len);
      break;
    case PHYLAX_MSG_TREZOR:
      if (f->len != USB_PACKET_SIZE) break;
      if (phylax_role() == PHYLAX_ROLE_KEY) {
        reply_via_bus = true;
        reply_to = f->src;
        usbDeliverPacket(f->payload);
      } else if (phylax_role() == PHYLAX_ROLE_MAIN) {
        main_rx_trezor_packet(f->payload);
      }
      break;
    default:
      break;  // discovery, storage, ... come later
  }
}

void phylax_bus_rx(const uint8_t *data, size_t len) {
  static PhylaxDecoder dec;
  static bool init = false;
  if (!init) {
    phylax_decoder_reset(&dec);
    init = true;
  }
  for (size_t i = 0; i < len; i++) {
    if (phylax_decoder_feed(&dec, data[i])) {
      on_frame(&dec.frame);
    }
  }
}

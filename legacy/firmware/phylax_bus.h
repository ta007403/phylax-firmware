/*
 * PHYLAX Blocks - block-to-block bus.
 *
 * Frame format (all multi-byte fields little-endian), see the design doc
 * "RS485 bus":
 *
 *   0xAA 0x55 | dst | src | type | seq | flags | len(2) | payload | crc(2)
 *
 * The CRC is CRC-16/CCITT-FALSE over dst..payload. The bus is a shared
 * medium: every block hears every frame and acts only on frames addressed to
 * it (or broadcast). The same frames travel over RS485 on real blocks and
 * over UDP between emulators, so everything above the byte transport is
 * shared.
 *
 * This library is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef __PHYLAX_BUS_H__
#define __PHYLAX_BUS_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PHYLAX_SYNC1 0xAA
#define PHYLAX_SYNC2 0x55
#define PHYLAX_MAX_PAYLOAD 256
#define PHYLAX_HEADER_SIZE 9  // sync(2) dst src type seq flags len(2)
#define PHYLAX_FRAME_MAX (PHYLAX_HEADER_SIZE + PHYLAX_MAX_PAYLOAD + 2)

#define PHYLAX_ADDR_NONE 0x00   // key block not given an address yet
#define PHYLAX_ADDR_MAIN 0x01
#define PHYLAX_ADDR_KEY_FIRST 0x10  // key blocks are handed 0x10..0x1F
#define PHYLAX_ADDR_KEY_LAST 0x1F
#define PHYLAX_ADDR_BROADCAST 0xFF

#define PHYLAX_UID_SIZE 12

// Message types on the bus.
typedef enum {
  PHYLAX_MSG_DISCOVER = 0x01,  // main -> all: who is there?
  PHYLAX_MSG_HELLO = 0x02,     // block -> main: PhylaxHello
  PHYLAX_MSG_ASSIGN = 0x03,    // main -> all: uid(12) + address(1)
  PHYLAX_MSG_PING = 0x04,
  PHYLAX_MSG_PONG = 0x05,
  PHYLAX_MSG_TREZOR = 0x10,  // payload: one 64-byte Trezor USB packet
  PHYLAX_MSG_BUSY = 0x11,
  PHYLAX_MSG_STORE_GET = 0x20,
  PHYLAX_MSG_STORE_PUT = 0x21,
  PHYLAX_MSG_ERROR = 0x7F,
} PhylaxMsgType;

#define PHYLAX_FLAG_MORE 0x01  // more fragments follow

// HELLO payload flags
#define PHYLAX_HELLO_INITIALIZED 0x01
#define PHYLAX_HELLO_LOCKED 0x02
#define PHYLAX_HELLO_HAS_FP 0x04

// What a block is (also sent in HELLO).
typedef enum {
  PHYLAX_KIND_MAIN = 0,
  PHYLAX_KIND_KEY = 1,
  PHYLAX_KIND_STORAGE = 2,
} PhylaxKind;

// What this firmware does.
typedef enum {
  PHYLAX_ROLE_STOCK = 0,  // plain Trezor One behaviour, bus unused
  PHYLAX_ROLE_MAIN = 1,   // top block: USB to the PC, bus master
  PHYLAX_ROLE_KEY = 2,    // key block: answers Trezor messages over the bus
} PhylaxRole;

typedef struct {
  uint8_t dst;
  uint8_t src;
  uint8_t type;
  uint8_t seq;
  uint8_t flags;
  uint16_t len;
  uint8_t payload[PHYLAX_MAX_PAYLOAD];
} PhylaxFrame;

typedef struct {
  uint8_t state;
  uint16_t pos;
  uint16_t crc;
  PhylaxFrame frame;
} PhylaxDecoder;

// A block's identity, as sent in HELLO (19 bytes on the wire).
typedef struct {
  uint8_t uid[PHYLAX_UID_SIZE];
  uint8_t kind;
  uint8_t flags;
  uint32_t fingerprint;  // valid if flags & PHYLAX_HELLO_HAS_FP
  uint8_t version[3];
} PhylaxHello;
#define PHYLAX_HELLO_SIZE (PHYLAX_UID_SIZE + 2 + 4 + 3)

// --- frame layer (pure functions, no I/O) ---
uint16_t phylax_crc16(const uint8_t *data, size_t len, uint16_t crc);
size_t phylax_frame_encode(const PhylaxFrame *frame, uint8_t *out,
                           size_t out_size);
void phylax_decoder_reset(PhylaxDecoder *dec);
// Feed one received byte. Returns true when a complete, CRC-valid frame is
// available in dec->frame (valid until the next call).
bool phylax_decoder_feed(PhylaxDecoder *dec, uint8_t byte);
size_t phylax_hello_encode(const PhylaxHello *h, uint8_t *out);
bool phylax_hello_decode(const uint8_t *in, size_t len, PhylaxHello *h);

// --- this block ---
PhylaxRole phylax_role(void);
uint8_t phylax_my_addr(void);
// This block's own identity (uid, kind, seed fingerprint if available).
void phylax_my_hello(PhylaxHello *h);

// --- bus I/O ---
// Called often from the transport loop (timers: HELLO slots, discovery,
// key block timeouts).
void phylax_poll(void);
// Raw bytes received from the transport (UDP datagram or UART bytes).
void phylax_bus_rx(const uint8_t *data, size_t len);
bool phylax_bus_send(uint8_t dst, uint8_t type, const uint8_t *payload,
                     uint16_t len);
// Send a whole Trezor protobuf message (id + encoded body) to a block,
// cut into 64-byte Trezor USB packets, one packet per frame.
bool phylax_bus_send_trezor_msg(uint8_t dst, uint16_t msg_id,
                                const uint8_t *data, uint32_t len);

// --- key block: route its replies back over the bus ---
bool phylax_reply_via_bus(void);
void phylax_note_host_input(void);
void phylax_bus_send_reply_packet(const uint8_t *packet);  // 64 bytes

// --- provided by the transport (firmware/udp.c, firmware/usb.c) ---
// Deliver one 64-byte Trezor packet as if it came from the USB host.
void usbDeliverPacket(const uint8_t *packet);
// Put raw frame bytes on the shared bus. False if there is no bus.
bool phylax_transport_send(const uint8_t *data, size_t len);

#if EMULATOR
// Emulator only: confirm button prompts that come over the bus (tests).
bool phylax_emu_autoconfirm(void);
#endif

#endif

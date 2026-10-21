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

#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#define TREZOR_UDP_PORT 21324

// PHYLAX: the port can be changed with the TREZOR_UDP_PORT environment
// variable, so several emulated blocks can run side by side. DebugLink uses
// the next port up, as before.
static int udp_port(void) {
  const char *env = getenv("TREZOR_UDP_PORT");
  if (env != NULL && *env != '\0') {
    int port = atoi(env);
    if (port > 0 && port < 65535) {
      return port;
    }
    fprintf(stderr, "Ignoring invalid TREZOR_UDP_PORT=%s\n", env);
  }
  return TREZOR_UDP_PORT;
}

struct usb_socket {
  int fd;
  struct sockaddr_in from;
  socklen_t fromlen;
};

static struct usb_socket usb_main;
static struct usb_socket usb_debug;

// PHYLAX: emulated block-to-block bus, a shared medium like RS485. Every
// emulator is a "node" listening on UDP port PHYLAX_BUS_PORT_BASE + node, and
// every frame is sent to all other nodes; the bus address inside the frame
// decides who acts on it, exactly as on the real wire.
#define PHYLAX_BUS_PORT_BASE 21500
#define PHYLAX_BUS_NODES 8
static struct usb_socket bus_sock;
static int bus_enabled = 0;
static int bus_port_base = PHYLAX_BUS_PORT_BASE;
static int bus_node = 0;

static struct pollfd usb_fds[3];

static int socket_setup(int port) {
  int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (fd < 0) {
    perror("Failed to create socket");
    exit(1);
  }

  struct sockaddr_in addr = {0};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

  if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
    perror("Failed to bind socket");
    exit(1);
  }

  return fd;
}

static size_t socket_write(struct usb_socket *sock, const void *buffer,
                           size_t size) {
  if (sock->fromlen > 0) {
    ssize_t n = sendto(sock->fd, buffer, size, MSG_DONTWAIT,
                       (const struct sockaddr *)&sock->from, sock->fromlen);
    if (n < 0 || ((size_t)n) != size) {
      perror("Failed to write socket");
      return 0;
    }
  }

  return size;
}

static size_t socket_read(struct usb_socket *sock, void *buffer, size_t size) {
  sock->fromlen = sizeof(sock->from);
  ssize_t n = recvfrom(sock->fd, buffer, size, MSG_DONTWAIT,
                       (struct sockaddr *)&sock->from, &sock->fromlen);

  if (n < 0) {
    if (errno != EAGAIN && errno != EWOULDBLOCK) {
      perror("Failed to read socket");
    }
    return 0;
  }

  static const char msg_ping[] = {'P', 'I', 'N', 'G', 'P', 'I', 'N', 'G'};
  static const char msg_pong[] = {'P', 'O', 'N', 'G', 'P', 'O', 'N', 'G'};

  if (n == sizeof(msg_ping) &&
      memcmp(buffer, msg_ping, sizeof(msg_ping)) == 0) {
    socket_write(sock, msg_pong, sizeof(msg_pong));
    return 0;
  }

  return n;
}

void emulatorSocketInit(void) {
  int port = udp_port();
  usb_main.fd = socket_setup(port);
  usb_main.fromlen = 0;
  usb_fds[0].fd = usb_main.fd;
  usb_fds[0].events = POLLIN;

  usb_debug.fd = socket_setup(port + 1);
  usb_debug.fromlen = 0;
  usb_fds[1].fd = usb_debug.fd;
  usb_fds[1].events = POLLIN;

  const char *role = getenv("PHYLAX_ROLE");
  if (role != NULL && *role != '\0') {
    const char *base = getenv("PHYLAX_BUS_PORT_BASE");
    if (base != NULL && *base != '\0') bus_port_base = atoi(base);
    const char *node = getenv("PHYLAX_BUS_NODE");
    bus_node = (node != NULL && *node != '\0') ? atoi(node)
               : (strcmp(role, "main") == 0)    ? 0
                                                : 1;
    if (bus_node < 0 || bus_node >= PHYLAX_BUS_NODES) {
      fprintf(stderr, "PHYLAX_BUS_NODE must be 0..%d\n", PHYLAX_BUS_NODES - 1);
      exit(1);
    }
    bus_sock.fd = socket_setup(bus_port_base + bus_node);
    bus_sock.fromlen = 0;
    usb_fds[2].fd = bus_sock.fd;
    usb_fds[2].events = POLLIN;
    bus_enabled = 1;
    fprintf(stderr, "PHYLAX bus: node %d on UDP port %d\n", bus_node,
            bus_port_base + bus_node);
  }
}

size_t emulatorBusBroadcast(const void *buffer, size_t size) {
  if (!bus_enabled) return 0;
  for (int node = 0; node < PHYLAX_BUS_NODES; node++) {
    if (node == bus_node) continue;
    struct sockaddr_in to = {0};
    to.sin_family = AF_INET;
    to.sin_port = htons(bus_port_base + node);
    to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    // Nodes that are not running simply drop it, like an empty bus slot.
    sendto(bus_sock.fd, buffer, size, MSG_DONTWAIT,
           (const struct sockaddr *)&to, sizeof(to));
  }
  return size;
}

size_t emulatorSocketRead(int *iface, void *buffer, size_t size,
                          int timeout_ms) {
  if (poll(usb_fds, bus_enabled ? 3 : 2, timeout_ms) > 0) {
    if (usb_fds[0].revents & POLLIN) {
      *iface = 0;
      return socket_read(&usb_main, buffer, size);
    } else if (usb_fds[1].revents & POLLIN) {
      *iface = 1;
      return socket_read(&usb_debug, buffer, size);
    } else if (bus_enabled && (usb_fds[2].revents & POLLIN)) {
      // Bus frames are raw bytes: no PING/PONG handling here.
      *iface = 2;
      ssize_t n = recv(bus_sock.fd, buffer, size, MSG_DONTWAIT);
      return n > 0 ? (size_t)n : 0;
    }
  }
  return 0;
}

size_t emulatorSocketWrite(int iface, const void *buffer, size_t size) {
  if (iface == 0) {
    return socket_write(&usb_main, buffer, size);
  }
  if (iface == 1) {
    return socket_write(&usb_debug, buffer, size);
  }
  return 0;
}

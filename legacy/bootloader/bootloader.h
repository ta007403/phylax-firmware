/*
 * This file is part of the Trezor project, https://trezor.io/
 *
 * Copyright (C) 2014 Pavol Rusnak <stick@satoshilabs.com>
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

#ifndef __BOOTLOADER_H__
#define __BOOTLOADER_H__

#include "version.h"

#define STR(X) #X
#define VERSTR(X) STR(X)

#include <stdbool.h>
#include <stdint.h>

void show_halt(const char *line1, const char *line2);
void show_unplug(const char *line1, const char *line2);
void layoutFirmwareFingerprint(const uint8_t *hash);
bool get_button_response(void);

// PHYLAX Blocks: the PHYLAX bootloader's wording (PHYLAX_SIGNING builds).
#if PHYLAX_SIGNING
#define BOOTLOADER_BRAND "PHYLAX"
#define DEVICE_UNPLUG_LINE "unplug the device"
#define DEVICE_YOUR "Your device"
#else
#define BOOTLOADER_BRAND "Trezor"
#define DEVICE_UNPLUG_LINE "unplug your Trezor"
#define DEVICE_YOUR "Your Trezor"
#endif

#endif

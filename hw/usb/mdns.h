/*
 * Multicast DNS service advertising.
 *
 * Copyright (c) 2023-2026 Visual Ehrmanntraut (VisualEhrmanntraut).
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include "qemu/osdep.h"
#include "qapi/error.h"

typedef struct MDNSService MDNSService;

/* mDNSResponder on macOS, the DNS Client service on Windows, avahi-daemon on Linux. */
MDNSService* mdns_service_register(const char* name, const char* type, uint16_t port, const char* txt, Error** errp);

void mdns_service_unregister(MDNSService* svc);

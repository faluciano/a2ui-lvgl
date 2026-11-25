// Copyright 2026 The a2ui-lvgl Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>

/**
 * Joins the Wi-Fi network from menuconfig as a station and keeps rejoining
 * it after every disconnect. Waits up to `wait_ms` for an IP address and
 * returns whether one arrived; the connection attempts go on either way.
 */
bool example_wifi_start(uint32_t wait_ms);

// SPDX-FileCopyrightText: © 2026 Tenstorrent Inc.
//
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>

#include "tt-umd/coordinates/att/att_map.hpp"

namespace tt::umd::att {

/**
 * Two whole Horizon 2x3 devices behind linked crossbars (aether horizon_x2): each device's flat
 * addresses, transcribed from aether's verification/att_maps/horizon/two_devices.yaml, which the
 * testbench programs into every bridge and tile of a device (AETHER_ATT_CONFIG). Device d owns the
 * d-th slot of each region: L1 at 0x4000_0000 + d * 256 MB, Tensix CSR at 0x8000_0000 + d * 512 MB,
 * DRAM at 0x1_0000_0000 + d * 2 GB. Its own regions are rebased onto its tiles; the other device's
 * cross the link. tests/att_maps/horizon_x2_two_devices.yaml is the snapshot the maps are checked
 * against.
 *
 * Endpoint words are (y << 6) | x in the device's frame, which is the whole 2x3 grid: Tensix (0,0),
 * (1,0); Dispatch (0,1), (1,1); NOC2AXI (0,2), (1,2), which front DRAM banks 0 and 1. Words are per
 * selector, in the device table's row order (table_offset + selector).
 */

// L1 unicast, 1 MB per selector: Dispatch (0,1), (1,1), Tensix (0,0), (1,0), then the table's
// remaining rows. Only the first 1 MB of each 3 MB L1 is reachable through it; the Tensix CSR
// window reaches the rest.
inline constexpr Window HORIZON_X2_DEV0_L1_WINDOW{
    .compare = 0x4000'0000,
    .mask_bits = 26,
    .endpoint_shift = 20,
    .endpoint_size = 6,
    .endpoint_table_offset = 1,
    .translate_address = true,
};
inline constexpr uint16_t HORIZON_X2_DEV0_L1_WORDS[] = {0x040, 0x041, 0x000, 0x001, 0x081, 0x080};

// Tensix CSR, 128 MB per selector: each tile's whole map, L1 included.
inline constexpr Window HORIZON_X2_DEV0_CSR_WINDOW{
    .compare = 0x8000'0000,
    .mask_bits = 29,
    .endpoint_shift = 27,
    .endpoint_size = 2,
    .endpoint_table_offset = 1,
    .translate_address = true,
};

// DRAM, 1 GB per bank, behind NOC2AXI (0,2) and (1,2). The tables hold the two banks as two
// pass-through windows, 0x1_0000_0000 and 0x1_4000_0000; one 2 GB window with a one-bit selector
// gives the same addresses.
inline constexpr Window HORIZON_X2_DEV0_DRAM_WINDOW{
    .compare = 0x1'0000'0000,
    .mask_bits = 31,
    .endpoint_shift = 30,
    .endpoint_size = 1,
    .endpoint_table_offset = 6,
    .translate_address = false,
};

inline constexpr Window HORIZON_X2_DEV1_L1_WINDOW{
    .compare = 0x5000'0000,
    .mask_bits = 26,
    .endpoint_shift = 20,
    .endpoint_size = 6,
    .endpoint_table_offset = 2,
    .translate_address = true,
};
inline constexpr uint16_t HORIZON_X2_DEV1_L1_WORDS[] = {0x040, 0x041, 0x000, 0x001, 0x081};

inline constexpr Window HORIZON_X2_DEV1_CSR_WINDOW{
    .compare = 0xA000'0000,
    .mask_bits = 29,
    .endpoint_shift = 27,
    .endpoint_size = 2,
    .endpoint_table_offset = 2,
    .translate_address = true,
};

inline constexpr Window HORIZON_X2_DEV1_DRAM_WINDOW{
    .compare = 0x1'8000'0000,
    .mask_bits = 31,
    .endpoint_shift = 30,
    .endpoint_size = 1,
    .endpoint_table_offset = 1,
    .translate_address = false,
};

// Both devices' CSR and DRAM selectors reach the same tiles in their own frame.
inline constexpr uint16_t HORIZON_X2_CSR_WORDS[] = {0x040, 0x041, 0x000, 0x001};
inline constexpr uint16_t HORIZON_X2_DRAM_WORDS[] = {0x080, 0x081};

inline constexpr MapData HORIZON_X2_DEV0_MAP{
    .windows = {{HORIZON_X2_DEV0_L1_WINDOW, HORIZON_X2_DEV0_DRAM_WINDOW, HORIZON_X2_DEV0_CSR_WINDOW}},
    .endpoint_words = {{HORIZON_X2_DEV0_L1_WORDS, HORIZON_X2_DRAM_WORDS, HORIZON_X2_CSR_WORDS}},
    .package_offset_x = 0,
    .package_offset_y = 0,
};

inline constexpr MapData HORIZON_X2_DEV1_MAP{
    .windows = {{HORIZON_X2_DEV1_L1_WINDOW, HORIZON_X2_DEV1_DRAM_WINDOW, HORIZON_X2_DEV1_CSR_WINDOW}},
    .endpoint_words = {{HORIZON_X2_DEV1_L1_WORDS, HORIZON_X2_DRAM_WORDS, HORIZON_X2_CSR_WORDS}},
    .package_offset_x = 0,
    .package_offset_y = 0,
};

}  // namespace tt::umd::att

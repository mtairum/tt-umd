// SPDX-FileCopyrightText: © 2026 Tenstorrent Inc.
//
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>
#include <yaml-cpp/yaml.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "tests/test_utils/fetch_local_files.hpp"
#include "tt-umd/coordinates/att/att_map.hpp"
#include "tt-umd/coordinates/att/att_resolver.hpp"
#include "tt-umd/coordinates/att/att_window.hpp"
#include "tt-umd/coordinates/att/configs/horizon_x2_att_map.hpp"
#include "tt-umd/soc_arch_descriptor.hpp"
#include "tt-umd/soc_descriptor.hpp"
#include "tt-umd/types/core_coordinates.hpp"
#include "tt-umd/types/io_window_config.hpp"
#include "tt-umd/types/xy_pair.hpp"

using namespace tt;
using namespace tt::umd;

namespace {

const att::MapData* const MAPS[] = {&att::HORIZON_X2_DEV0_MAP, &att::HORIZON_X2_DEV1_MAP};

// Device @p device's tables in the snapshot of aether's two_devices.yaml.
YAML::Node device_tables(int device) {
    const YAML::Node tables = YAML::LoadFile(test_utils::GetAbsPath("att_maps/horizon_x2_two_devices.yaml"));
    for (const auto& entry : tables["devices"]) {
        if (entry["id"].as<int>() == device) {
            return entry["defaults"];
        }
    }
    ADD_FAILURE() << "the snapshot has no device " << device;
    return {};
}

uint64_t compare_of(const YAML::Node& mask_entry) {
    return std::stoull(mask_entry["compare"].as<std::string>(), nullptr, 16);
}

// The mask entry whose window starts at @p compare.
YAML::Node mask_entry_at(const YAML::Node& tables, uint64_t compare) {
    for (const auto& entry : tables["mask_table"]) {
        if (compare_of(entry) == compare) {
            return entry;
        }
    }
    return {};
}

// Endpoint table row @p row, packed as the map packs it: (y << 6) | x.
uint16_t row_word(const YAML::Node& tables, uint32_t row) {
    for (const auto& entry : tables["noc_endpoint_table"]) {
        if (entry["index"].as<uint32_t>() == row) {
            return static_cast<uint16_t>((entry["y"].as<uint32_t>() << 6) | entry["x"].as<uint32_t>());
        }
    }
    ADD_FAILURE() << "the snapshot has no endpoint row " << row;
    return att::ENDPOINT_UNPOPULATED;
}

}  // namespace

// The maps are transcriptions, so they are checked against the tables they were transcribed from:
// tests/att_maps/horizon_x2_two_devices.yaml, a copy of the tables aether's testbench programs.
TEST(HorizonX2AttMap, L1AndCsrWindowsMatchTheTables) {
    for (int device = 0; device < 2; device++) {
        const YAML::Node tables = device_tables(device);
        const att::MapData& map = *MAPS[device];
        for (const att::WindowClass window_class : {att::WindowClass::WORKER, att::WindowClass::FULL_TILE}) {
            const auto index = static_cast<size_t>(window_class);
            const att::Window& window = map.windows[index];
            SCOPED_TRACE("device " + std::to_string(device) + ", window class " + std::to_string(index));

            const YAML::Node entry = mask_entry_at(tables, window.compare);
            ASSERT_TRUE(entry.IsDefined()) << "no mask entry at 0x" << std::hex << window.compare;
            EXPECT_EQ(window.mask_bits, entry["mask"].as<uint32_t>());
            EXPECT_EQ(window.endpoint_shift, entry["ep_idx"].as<uint32_t>());
            EXPECT_EQ(window.endpoint_size, entry["ep_id_size"].as<uint32_t>());
            EXPECT_EQ(window.endpoint_table_offset, entry["table_offset"].as<uint32_t>());
            EXPECT_EQ(window.translate_address, entry["translate_addr"].as<int>() == 1);

            const att::Table<uint16_t>& words = map.endpoint_words[index];
            for (uint32_t selector = 0; selector < words.size(); selector++) {
                EXPECT_EQ(words[selector], row_word(tables, window.endpoint_table_offset + selector))
                    << "selector " << selector;
            }
        }
    }
}

// The tables hold each DRAM bank as its own pass-through window; the map folds the two into one
// window with a one-bit selector, which must give the same addresses and reach the same bridges.
TEST(HorizonX2AttMap, DramBanksMatchTheTables) {
    for (int device = 0; device < 2; device++) {
        const YAML::Node tables = device_tables(device);
        const att::MapData& map = *MAPS[device];
        const auto index = static_cast<size_t>(att::WindowClass::DRAM);
        const att::Window& window = map.windows[index];
        for (uint32_t bank = 0; bank < 2; bank++) {
            SCOPED_TRACE("device " + std::to_string(device) + ", bank " + std::to_string(bank));
            const YAML::Node entry = mask_entry_at(tables, window.make_address(bank, 0));
            ASSERT_TRUE(entry.IsDefined());
            EXPECT_EQ(entry["mask"].as<uint32_t>(), window.endpoint_shift);
            EXPECT_EQ(entry["ep_id_size"].as<uint32_t>(), 0u);
            EXPECT_EQ(window.translate_address, entry["translate_addr"].as<int>() == 1);
            EXPECT_EQ(map.endpoint_words[index][bank], row_word(tables, entry["table_offset"].as<uint32_t>()));
        }
    }
}

// The per-tile addresses aether's README_multi_device.md lists for these tables.
TEST(HorizonX2AttMap, ResolvesTheAddressesAetherDocuments) {
    struct Case {
        tt_xy_pair core;
        CoreType type;
        uint64_t offset;
        uint64_t dev0;
        uint64_t dev1;
    };

    const Case cases[] = {
        {{0, 1}, CoreType::DISPATCH, 0, 0x8000'0000, 0xA000'0000},
        {{1, 1}, CoreType::DISPATCH, 0, 0x8800'0000, 0xA800'0000},
        {{0, 0}, CoreType::TENSIX, 0, 0x4020'0000, 0x5020'0000},
        {{1, 0}, CoreType::TENSIX, 0, 0x4030'0000, 0x5030'0000},
        // The README's example: the TSC reset register of Tensix (0,0), past L1, so through its CSR.
        {{0, 0}, CoreType::TENSIX, 0x0303'3088, 0x9303'3088, 0xB303'3088},
        {{1, 0}, CoreType::TENSIX, 0x0010'0000, 0x9810'0000, 0xB810'0000},
        {{0, 2}, CoreType::DRAM, 0x100, 0x1'0000'0100, 0x1'8000'0100},
        {{1, 2}, CoreType::DRAM, 0x100, 0x1'4000'0100, 0x1'C000'0100},
    };
    const att::EndpointResolver dev0(att::HORIZON_X2_DEV0_MAP);
    const att::EndpointResolver dev1(att::HORIZON_X2_DEV1_MAP);
    for (const Case& c : cases) {
        SCOPED_TRACE(c.core.str() + " offset 0x" + std::to_string(c.offset));
        EXPECT_EQ(dev0.resolve(c.core, c.type, c.offset, 4), c.dev0);
        EXPECT_EQ(dev1.resolve(c.core, c.type, c.offset, 4), c.dev1);
    }
}

// The fabric query: an IoWindow's flags and ordering pick the window. These maps have unicast only.
TEST(HorizonX2AttMap, IoWindowFlagsSelectTheUnicastWindowOnly) {
    // Literal, typed coordinates skip the descriptor's frame and type lookup, so any descriptor will do.
    const SocDescriptor soc_descriptor(
        std::make_shared<SocArchDescriptor>(test_utils::GetSocDescAbsPath("quasar_32_arch.yaml")),
        {.noc_translation_enabled = false});
    const att::EndpointResolver dev1(att::HORIZON_X2_DEV1_MAP);
    const CoreCoord tensix(0, 0, CoreType::TENSIX, CoordSystem::LITERAL);

    for (const WindowFlags direction :
         {WindowFlags::Bidirectional, WindowFlags::UnicastWrite, WindowFlags::UnicastRead}) {
        for (const IoOrdering ordering : {IoOrdering::Relaxed, IoOrdering::Strict, IoOrdering::Posted}) {
            EXPECT_EQ(
                att::resolve_io_window(dev1, soc_descriptor, tensix, direction, ordering, 0x100),
                std::optional<uint64_t>(0x5020'0100));
        }
    }
    EXPECT_EQ(
        att::resolve_io_window(dev1, soc_descriptor, tensix, WindowFlags::MulticastWrite, IoOrdering::Strict, 0x100),
        std::nullopt);
    EXPECT_EQ(
        att::resolve_io_window(
            dev1, soc_descriptor, tensix, WindowFlags::UnicastWrite | WindowFlags::Atomic, IoOrdering::Posted, 0),
        std::nullopt);
    EXPECT_EQ(
        att::resolve_io_window(
            dev1, soc_descriptor, tensix, WindowFlags::UnicastRead | WindowFlags::Snoop, IoOrdering::Strict, 0),
        std::nullopt);
}

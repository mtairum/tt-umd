// SPDX-FileCopyrightText: © 2026 Tenstorrent Inc.
//
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "tt-umd/types/arch.hpp"
#include "tt-umd/types/register_access_width.hpp"
#include "utils.hpp"

using namespace tt;
using namespace tt::umd;

namespace {

using Accesses = std::vector<std::pair<uint64_t, size_t>>;

Accesses accesses(uint64_t addr, size_t size, RegisterAccessWidth width) {
    Accesses result;
    for_each_register_access(
        addr, size, width, [&](uint64_t reg_addr, size_t offset) { result.emplace_back(reg_addr, offset); });
    return result;
}

struct SplitCase {
    RegisterAccessWidth width;
    size_t size;
    Accesses expected;
};

struct MisalignedCase {
    RegisterAccessWidth width;
    uint64_t addr;
    size_t size;
};

}  // namespace

class RegisterAccessSplit : public ::testing::TestWithParam<SplitCase> {};

TEST_P(RegisterAccessSplit, IssuesOneAccessPerRegister) {
    EXPECT_EQ(accesses(0xFFB20100, GetParam().size, GetParam().width), GetParam().expected);
}

INSTANTIATE_TEST_SUITE_P(
    Widths,
    RegisterAccessSplit,
    ::testing::Values(
        SplitCase{RegisterAccessWidth::Bits32, 12, {{0xFFB20100, 0}, {0xFFB20104, 4}, {0xFFB20108, 8}}},
        SplitCase{RegisterAccessWidth::Bits64, 16, {{0xFFB20100, 0}, {0xFFB20108, 8}}}));

class RegisterAccessMisaligned : public ::testing::TestWithParam<MisalignedCase> {};

TEST_P(RegisterAccessMisaligned, Throws) {
    EXPECT_ANY_THROW(accesses(GetParam().addr, GetParam().size, GetParam().width));
}

INSTANTIATE_TEST_SUITE_P(
    AddressAndSize,
    RegisterAccessMisaligned,
    ::testing::Values(
        MisalignedCase{RegisterAccessWidth::Bits32, 0xFFB20102, 4},
        MisalignedCase{RegisterAccessWidth::Bits32, 0xFFB20100, 6},
        MisalignedCase{RegisterAccessWidth::Bits64, 0xFFB20104, 8},
        MisalignedCase{RegisterAccessWidth::Bits64, 0xFFB20100, 12}));

class RegisterAccess64BitOutsideQuasar : public ::testing::TestWithParam<tt::ARCH> {};

TEST_P(RegisterAccess64BitOutsideQuasar, Throws) {
    EXPECT_ANY_THROW(validate_register_access_width(GetParam(), RegisterAccessWidth::Bits64));
}

INSTANTIATE_TEST_SUITE_P(
    Architectures, RegisterAccess64BitOutsideQuasar, ::testing::Values(tt::ARCH::WORMHOLE_B0, tt::ARCH::BLACKHOLE));

TEST(RegisterAccess, Allows64BitAccessOnQuasar) {
    EXPECT_NO_THROW(validate_register_access_width(tt::ARCH::QUASAR, RegisterAccessWidth::Bits64));
}

// SPDX-FileCopyrightText: © 2026 Tenstorrent Inc.
//
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>
#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#if defined(__x86_64__)
#include <asm/perf_regs.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include "tt-umd/cluster.hpp"

using namespace tt;
using namespace tt::umd;

// Profilers that record call chains (Tracy, perf -g) have the kernel walk frame pointers from the sampled rbp, reading
// memory at rbp. If rbp holds a pointer into a BAR mapping, that read goes to the device and can wedge it
// (tt-umd#3594). Sample rbp alone, which the kernel only copies, while UMD does register I/O, and check it never
// points into device memory.
TEST(FramePointer, RbpNeverPointsIntoDeviceMemory) {
#if !defined(__x86_64__)
    GTEST_SKIP() << "Samples the x86-64 frame pointer register.";
#else
    perf_event_attr attr{.type = PERF_TYPE_SOFTWARE, .size = sizeof(attr), .config = PERF_COUNT_SW_CPU_CLOCK};
    attr.sample_period = 100'000;  // ns of CPU time.
    attr.sample_type = PERF_SAMPLE_REGS_USER;
    attr.sample_regs_user = 1ULL << PERF_REG_X86_BP;
    attr.exclude_kernel = 1;
    const int fd = static_cast<int>(syscall(SYS_perf_event_open, &attr, 0, -1, -1, PERF_FLAG_FD_CLOEXEC));
    if (fd < 0) {
        GTEST_SKIP() << "perf_event_open not permitted.";
    }
    const size_t page = sysconf(_SC_PAGESIZE);
    const size_t data_size = 64 * page;
    // A writable mapping puts the ring in non-overwrite mode: the kernel drops new samples once it is full rather than
    // running over records the test has not read yet.
    auto* ring =
        static_cast<perf_event_mmap_page*>(mmap(nullptr, page + data_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
    ASSERT_NE(ring, MAP_FAILED);
    const uint8_t* data = reinterpret_cast<const uint8_t*>(ring) + page;

    Cluster cluster;
    cluster.start_device({});

    std::vector<std::pair<uint64_t, uint64_t> > device_maps;
    std::ifstream maps("/proc/self/maps");
    for (std::string line; std::getline(maps, line);) {
        if (line.find("/dev/tenstorrent") != std::string::npos) {
            const size_t dash = line.find('-');
            device_maps.emplace_back(std::stoull(line, nullptr, 16), std::stoull(line.substr(dash + 1), nullptr, 16));
        }
    }
    ASSERT_FALSE(device_maps.empty()) << "No BAR mapping to compare against, so the check below would pass vacuously.";

    size_t samples = 0;
    size_t hits = 0;
    // data_head and data_tail are free-running byte counters into a ring, so a record can straddle the end of the
    // buffer: copy each one out before reading it. Publishing data_tail is what frees space for the rest of the run;
    // without it the ring fills up and the kernel drops every later sample.
    auto drain = [&]() {
        const uint64_t head = __atomic_load_n(&ring->data_head, __ATOMIC_ACQUIRE);
        auto copy_out = [&](uint64_t from, void* dest, size_t size) {
            const size_t start = from % data_size;
            const size_t until_end = std::min(size, data_size - start);
            std::memcpy(dest, data + start, until_end);
            std::memcpy(static_cast<uint8_t*>(dest) + until_end, data, size - until_end);
        };
        uint64_t offset = ring->data_tail;
        while (offset + sizeof(perf_event_header) <= head) {
            perf_event_header header;
            copy_out(offset, &header, sizeof(header));
            if (header.size < sizeof(header) || offset + header.size > head) {
                break;  // Truncated or malformed; the rest of the ring cannot be parsed.
            }
            if (header.type == PERF_RECORD_SAMPLE && header.size == sizeof(header) + 2 * sizeof(uint64_t)) {
                uint64_t sample[2];  // abi, rbp
                copy_out(offset + sizeof(header), sample, sizeof(sample));
                samples++;
                for (auto [lo, hi] : device_maps) {
                    hits += sample[1] >= lo && sample[1] < hi;
                }
            }
            offset += header.size;
        }
        __atomic_store_n(&ring->data_tail, offset, __ATOMIC_RELEASE);
    };

    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (std::chrono::steady_clock::now() < end) {
        for (const CoreCoord& core : cluster.get_soc_descriptor(0).get_cores(CoreType::TENSIX)) {
            cluster.assert_risc_reset(0, core, RiscType::ALL);  // Register read-modify-write through a TLB window.
        }
        drain();
    }
    ioctl(fd, PERF_EVENT_IOC_DISABLE, 0);  // Stop sampling so data_head stays put while the ring is drained.
    drain();

    ASSERT_GT(samples, 0u);
    EXPECT_EQ(hits, 0u) << hits << " of " << samples << " samples caught rbp pointing into a BAR mapping.";
    munmap(ring, page + data_size);
    close(fd);
#endif
}

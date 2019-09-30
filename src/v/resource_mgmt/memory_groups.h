#pragma once

#include <seastar/core/memory.hh>

// centralized unit for memory management
struct memory_groups {
    static size_t kafka_total_memory() {
        // 30%
        return memory::stats().total_memory() * .30;
    }
    /// \brief includes raft & all services
    static size_t rpc_total_memory() {
        // 30%
        return memory::stats().total_memory() * .30;
    }
    static size_t reserved_unused_total_memory() {
        return memory::stats().total_memory() * .40;
    }
};
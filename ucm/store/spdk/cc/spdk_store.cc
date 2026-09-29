/**
 * MIT License
 *
 * Copyright (c) 2025 Huawei Technologies Co., Ltd. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE DEALINGS IN THE SOFTWARE.
 * */

/*
 * spdk_store.cc — SPDK-backed UCM StoreV1 implementation.
 *
 * Wraps the prototype store (testSPDK/ucm_spdk_store.c) into a pipeline-
 * loadable .so. The underlying engine runs a dedicated reactor thread that
 * owns the NVMe qpair; all StoreV1 calls from UCM worker threads enqueue
 * ops through a lock-free MPSC ring and are woken on completion.
 *
 * Semantics mapping (Posix store → this store):
 *   file per block     → LBA slot (superblock + fixed index region)
 *   .tmp + rename      → commit record after last shard (epoch-based)
 *   unlink / GC        → DELETED record + exact free-list push
 *   access() lookup    → shared-memory hash (rebuilt at init)
 *   multi-process      → SHM index + nvmf loopback for data path
 *
 * Configuration keys (all under ucm_connector_config):
 *   spdk_trid            "trtype:PCIe traddr:0000:84:00.0" or
 *                         "trtype:RDMA adrfam:IPv4 traddr:IP trsvcid:4420"
 *   spdk_capacity_gb     active capacity in GiB (default 500; fixes the
 *                        slot count only at first format)
 *   spdk_format          1 = format on first use (default 0)
 *   spdk_offset_gib      LBA offset of the pool region (default 0)
 *   shard_size/block_size  runtime model geometry (the same keys the
 *                        Cache stage consumes). The store serves whatever
 *                        the model declares: a geometry change (model
 *                        switch) invalidates all existing blocks and
 *                        reformats the region in place, preserving the
 *                        byte capacity.
 *
 * Metrics: emits posix_* names so ucm_store_stats.sh works unchanged.
 */

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <thread>
#include <unordered_map>

#include <cerrno>

#include "ucm_spdk_engine.h"

/*
 * The engine functions are resolved at runtime: either the engine static
 * library is linked into this .so (preferred, via build_so.sh with
 * ENGINE_LINK=1), or this .so runs with stubs (interface-only mode for
 * pipeline registration testing). The Makefile in cc/ builds the engine.
 */

#include "logger/logger.h"
#include "metrics_api.h"
#include "type/types.h"
#include "ucmstore_v1.h"

namespace UC::SpdkStore {

using Detail::BlockId;
using Detail::Dictionary;
using Detail::Shard;
using Detail::TaskDesc;
using Detail::TaskHandle;

static constexpr const char *kStoreName = "SpdkStore";

/* ---- handle registry: TaskHandle → completion state ---- */

struct TaskEntry {
    std::atomic<bool> done{false};
    std::atomic<int> rc{0};
    std::atomic<size_t> remaining{0};     /* shards not yet completed */
    std::mutex waitMu;
    std::condition_variable waitCv;
    bool isDump{false};
    std::chrono::steady_clock::time_point submitTp;
};

class HandleRegistry {
    std::mutex mu_;
    std::unordered_map<TaskHandle, std::shared_ptr<TaskEntry>> map_;
    TaskHandle next_{1};

public:
    std::pair<TaskHandle, std::shared_ptr<TaskEntry>> alloc(bool isDump, size_t nshards)
    {
        std::lock_guard<std::mutex> lock(mu_);
        auto handle = next_++;
        auto entry = std::make_shared<TaskEntry>();
        entry->isDump = isDump;
        entry->remaining.store(nshards);
        entry->submitTp = std::chrono::steady_clock::now();
        map_[handle] = entry;
        return {handle, std::move(entry)};
    }

    std::shared_ptr<TaskEntry> get(TaskHandle handle)
    {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = map_.find(handle);
        return (it != map_.end()) ? it->second : nullptr;
    }

    void release(TaskHandle handle)
    {
        std::lock_guard<std::mutex> lock(mu_);
        map_.erase(handle);
    }

};

/* ---- per-shard completion thunk (bridges C callback → TaskEntry) ---- */

struct ShardCbCtx {
    std::shared_ptr<TaskEntry> entry;
};

static void
complete_entry(const std::shared_ptr<TaskEntry> &entry, int status)
{
    if (status != 0) {
        int expected = 0;
        entry->rc.compare_exchange_strong(expected, status);
    }
    if (entry->remaining.fetch_sub(1) == 1) {
        {
            std::lock_guard<std::mutex> lock(entry->waitMu);
            entry->done.store(true, std::memory_order_release);
        }
        entry->waitCv.notify_all();
    }
}

static void
shard_done_cb(void *arg, int status)
{
    auto ctx = static_cast<ShardCbCtx *>(arg);
    auto entry = std::move(ctx->entry);

    complete_entry(entry, status);
    delete ctx;
}

/* ---- the store ---- */

class SpdkStore : public StoreV1 {
    bool initialized_{false};
    bool lookupOnly_{false};
    uint32_t blockSize_{0};       /* bytes per block */
    uint32_t shardSize_{0};       /* bytes per shard */
    uint32_t nshards_{0};         /* shards per block */
    HandleRegistry registry_;
    void* registeredHost_{nullptr};
    size_t registeredHostSize_{0};

    /* metric helpers (posix_* names for ucm_store_stats.sh compatibility) */
    static void Metric(const char *name, double value)
    {
        UC::Metrics::UpdateStats(name, value);
    }
    static void MetricCount(const char *name, double delta = 1.0)
    {
        UC::Metrics::UpdateStats(name, delta);
    }

public:
    ~SpdkStore() override
    {
        if (initialized_) {
            if (registeredHost_) {
                spdk_store_unregister_memory(registeredHost_, registeredHostSize_);
                registeredHost_ = nullptr;
                registeredHostSize_ = 0;
            }
            if (lookupOnly_) {
                spdk_store_lookup_fini();
            } else {
                spdk_store_reactor_stop();
                spdk_store_fini();
            }
        }
    }

    std::string Readme() const override
    {
        return kStoreName;
    }

    Status Setup(const Dictionary &config) override
    {
        if (initialized_) { return Status::OK(); }

        std::string trid = "trtype:PCIe traddr:0000:84:00.0";
        config.Get("spdk_trid", trid);

        uint64_t offsetGib = 0;
        config.GetNumber("spdk_offset_gib", offsetGib);

        uint64_t blockSize = 0;
        config.GetNumber("block_size", blockSize);
        uint64_t shardSize = 0;
        config.GetNumber("shard_size", shardSize);

        /* No model geometry in the config = scheduler role: the Cache
         * stage exempts deviceId == -1 from all size checks and the
         * vLLM connector injects tensor_size_list/shard_size/block_size
         * only on workers. Attach the shared index without ever probing
         * the device — the segment is region-scoped (trid + offset), so
         * no geometry is needed to find it. The worker may still be
         * creating it (the Cache|Posix watcher waits the same way), so
         * poll briefly instead of failing instantly. */
        if (shardSize == 0 && blockSize == 0) {
            int rc = -ENOENT;
            for (int i = 0; i < 100; i++) {
                rc = spdk_store_lookup_init_region(trid.c_str(), offsetGib);
                if (rc == 0) { break; }
                if (rc != -ENOENT && rc != -EBUSY) { break; }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            if (rc != 0) {
                return Status::Error(
                    fmt::format("lookup-only SHM attach failed: {}", rc));
            }
            lookupOnly_ = true;
            initialized_ = true;
            UC_INFO("{}: no model geometry (scheduler role), attached shared "
                    "index in lookup-only mode",
                    kStoreName);
            return Status::OK();
        }

        uint64_t shardKb = shardSize / 1024;
        if (shardSize == 0 || blockSize == 0 || blockSize % shardSize != 0 ||
            shardSize % 1024 != 0) {
            return Status::InvalidParam(
                "block_size must be divisible by a nonzero, KiB-aligned shard_size");
        }
        uint64_t nshards = blockSize / shardSize;
        if (nshards > std::numeric_limits<uint32_t>::max() ||
            shardSize > std::numeric_limits<uint32_t>::max()) {
            return Status::InvalidParam("SPDK block geometry exceeds 32-bit limits");
        }

        uint64_t capacityGb = 500;
        config.GetNumber("spdk_capacity_gb", capacityGb);
        if (capacityGb == 0) { capacityGb = 500; }

        int format = 0;
        config.GetNumber("spdk_format", format);

        /* capacity in slots */
        uint64_t slotBytes = nshards * shardKb * 1024;
        uint64_t nslots = (capacityGb << 30) / slotBytes;
        if (nslots == 0) {
            return Status::Error("invalid capacity: 0 slots");
        }

        blockSize_ = static_cast<uint32_t>(slotBytes);
        shardSize_ = static_cast<uint32_t>(shardSize);
        nshards_ = static_cast<uint32_t>(nshards);

        UC_INFO("{}: trid='{}' capacity={}GB ({} slots) shard={}KB x {}",
                kStoreName, trid, capacityGb, nslots, shardKb, nshards);

        int rc = spdk_store_init(trid.c_str(), static_cast<uint32_t>(nslots),
                                 static_cast<uint32_t>(nshards),
                                 static_cast<uint32_t>(shardKb), offsetGib, format);
        if (rc != 0) {
            if (rc == -ENODEV) {
                /* device busy or absent: degrade to lookup-only (no
                 * device access needed) */
                rc = spdk_store_lookup_init_region(trid.c_str(), offsetGib);
                if (rc != 0) {
                    return Status::Error(
                        fmt::format("lookup-only SHM attach failed: {}", rc));
                }
                lookupOnly_ = true;
                UC_INFO("{}: device unavailable, running in lookup-only mode",
                        kStoreName);
                initialized_ = true;
                return Status::OK();
            }
            return Status::Error(fmt::format("spdk_store_init failed: {}", rc));
        }

        rc = spdk_store_reactor_start();
        if (rc != 0) {
            spdk_store_fini();
            return Status::Error(fmt::format("reactor start failed: {}", rc));
        }

        initialized_ = true;
        UC_INFO("{}: initialized ({} slots, {} shards/block)", kStoreName,
                nslots, nshards);
        return Status::OK();
    }

    Expected<std::vector<uint8_t>> Lookup(const BlockId *blocks, size_t num) override
    {
        if (num == 0) { return std::vector<uint8_t>{}; }

        auto results = std::vector<uint8_t>(num, 0);
        size_t hits = 0;
        for (size_t i = 0; i < num; i++) {
            const auto *id = reinterpret_cast<const uint8_t *>(blocks[i].data());
            if (spdk_store_lookup(id)) {
                results[i] = 1;
                hits++;
            }
        }
        MetricCount("posix_lookup_query_blocks_total", static_cast<double>(num));
        MetricCount("posix_lookup_hit_blocks_total", static_cast<double>(hits));
        return results;
    }

    Expected<ssize_t> LookupOnPrefix(const BlockId *blocks, size_t num) override
    {
        if (num == 0) { return static_cast<ssize_t>(-1); }

        /* build contiguous id array for the C API */
        auto ids = std::make_unique<uint8_t[][16]>(num);
        for (size_t i = 0; i < num; i++) {
            memcpy(ids[i], blocks[i].data(), 16);
        }
        auto hit = spdk_store_lookup_prefix(const_cast<const uint8_t (*)[16]>(ids.get()),
                                            static_cast<uint32_t>(num));
        MetricCount("posix_lookup_query_blocks_total", static_cast<double>(num));
        MetricCount("posix_lookup_hit_blocks_total",
                    static_cast<double>(hit > 0 ? hit : 0));
        return static_cast<ssize_t>(hit) - 1;
    }

    Expected<ssize_t> LookupOnReverse(const BlockId *blocks, size_t num) override
    {
        for (ssize_t i = static_cast<ssize_t>(num) - 1; i >= 0; i--) {
            const auto *id = reinterpret_cast<const uint8_t *>(blocks[i].data());
            if (spdk_store_lookup(id)) {
                return i;
            }
        }
        return static_cast<ssize_t>(-1);
    }

    void Prefetch(const BlockId *blocks, size_t num) override
    {
        /* no-op: hotness is tracked in the shared-memory array */
        (void)blocks;
        (void)num;
    }

    Status CheckHealth() override
    {
        if (lookupOnly_) { return Status::OK(); }
        return spdk_store_healthy() ? Status::OK()
                                    : Status::Error("spdk store unhealthy");
    }

    Status RegisterHostMemory(void* addr, size_t size) override
    {
        if (lookupOnly_) { return Status::OK(); }
        if (!initialized_ || addr == nullptr || size == 0) {
            return Status::InvalidParam("invalid SPDK host-memory registration");
        }
        auto rc = spdk_store_register_memory(addr, size);
        if (rc != 0) {
            return Status::Error(fmt::format("spdk_mem_register failed: {}", rc));
        }
        registeredHost_ = addr;
        registeredHostSize_ = size;
        UC_INFO("{}: registered Cache SHM [{}, {}) {} bytes for direct DMA",
                kStoreName, addr, static_cast<void*>(static_cast<char*>(addr) + size), size);
        return Status::OK();
    }

    void UnregisterHostMemory(void* addr, size_t size) override
    {
        if (lookupOnly_ || addr == nullptr || size == 0) { return; }
        auto rc = spdk_store_unregister_memory(addr, size);
        if (rc != 0) {
            UC_ERROR("{}: spdk_mem_unregister({}, {}) failed: {}",
                     kStoreName, addr, size, rc);
            return;
        }
        if (registeredHost_ == addr) {
            registeredHost_ = nullptr;
            registeredHostSize_ = 0;
        }
    }

    Expected<TaskHandle> Load(TaskDesc task) override
    {
        if (lookupOnly_) { return Status::Error("lookup-only mode"); }
        if (task.empty()) { return Status::Error("empty task"); }

        for (const auto &shard : task) {
            if (shard.index >= nshards_ || shard.addrs.size() != 1 || shard.addrs.front() == nullptr) {
                return Status::InvalidParam("SPDK tasks require one valid address and an in-range shard index");
            }
        }

        auto [handle, entry] = registry_.alloc(false, task.size());
        size_t nSubmitted = 0;
        int lastRc = 0;

        for (const auto &shard : task) {
            const auto &idBytes = shard.owner;
            const auto *id = reinterpret_cast<const uint8_t *>(idBytes.data());
            auto *ctx = new (std::nothrow) ShardCbCtx{entry};
            if (ctx == nullptr) {
                if (lastRc == 0) { lastRc = -ENOMEM; }
                complete_entry(entry, -ENOMEM);
                continue;
            }
            int rc = spdk_store_load_submit(id, static_cast<uint32_t>(shard.index), 1,
                                            shard.addrs.front(),
                                            static_cast<uint32_t>(shardSize_),
                                            shard_done_cb, ctx);
            if (rc != 0 && lastRc == 0) { lastRc = rc; }
            if (rc == 0) {
                nSubmitted++;
            } else {
                shard_done_cb(ctx, rc);
            }
        }

        if (nSubmitted == 0) {
            registry_.release(handle);
            return Status::Error(fmt::format("all load submits failed: {}", lastRc));
        }
        return std::move(handle);
    }

    Expected<TaskHandle> Dump(TaskDesc task) override
    {
        if (lookupOnly_) { return Status::Error("lookup-only mode"); }
        if (task.empty()) { return Status::Error("empty task"); }

        for (const auto &shard : task) {
            if (shard.index >= nshards_ || shard.addrs.size() != 1 || shard.addrs.front() == nullptr) {
                return Status::InvalidParam("SPDK tasks require one valid address and an in-range shard index");
            }
        }

        auto [handle, entry] = registry_.alloc(true, task.size());
        size_t nSubmitted = 0;
        int lastRc = 0;

        for (const auto &shard : task) {
            const auto &idBytes = shard.owner;
            const auto *id = reinterpret_cast<const uint8_t *>(idBytes.data());
            auto *ctx = new (std::nothrow) ShardCbCtx{entry};
            if (ctx == nullptr) {
                if (lastRc == 0) { lastRc = -ENOMEM; }
                complete_entry(entry, -ENOMEM);
                continue;
            }
            /* shard.addrs[0] is the source data (SHM buffer, direct DMA) */
            int rc = spdk_store_dump_submit(id,
                                            static_cast<uint32_t>(shard.index),
                                            1, /* one shard per call */
                                             shard.addrs.front(),
                                             shard_done_cb, ctx);
            if (rc != 0 && lastRc == 0) { lastRc = rc; }
            if (rc == 0) {
                nSubmitted++;
            } else {
                shard_done_cb(ctx, rc);
            }
        }

        MetricCount("posix_dump_shards_total", static_cast<double>(nSubmitted));
        if (nSubmitted == 0) {
            registry_.release(handle);
            return Status::Error(fmt::format("all dump submits failed: {}", lastRc));
        }
        return std::move(handle);
    }

    Expected<bool> Check(TaskHandle taskId) override
    {
        auto entry = registry_.get(taskId);
        if (entry == nullptr) { return Status::Error("invalid task handle"); }
        return entry->done.load(std::memory_order_acquire);
    }

    Status Wait(TaskHandle taskId) override
    {
        auto entry = registry_.get(taskId);
        if (entry == nullptr) { return Status::Error("invalid task handle"); }

        std::unique_lock<std::mutex> lock(entry->waitMu);
        entry->waitCv.wait(lock, [&entry] {
            return entry->done.load(std::memory_order_acquire);
        });
        lock.unlock();

        auto rc = entry->rc.load();
        if (rc != 0) {
            registry_.release(taskId);
            if (rc == -ENOENT) { return Status::NotFound(); }
            return Status::Error(fmt::format("task failed: {}", rc));
        }
        registry_.release(taskId);
        return Status::OK();
    }
};

}  // namespace UC::SpdkStore

extern "C" UC::StoreV1 *MakeSpdkStore() { return new UC::SpdkStore::SpdkStore(); }

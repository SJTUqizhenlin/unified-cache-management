#include <gtest/gtest.h>
#include <set>

#include "spdk/cc/spdk_store.cc"

namespace {

struct EngineState {
    int lookupInitCalls = 0;
    int lookupFiniCalls = 0;
    int initCalls = 0;
    int finiCalls = 0;
    int reactorStartCalls = 0;
    int reactorStopCalls = 0;
    int gcStartCalls = 0;
    int gcStopCalls = 0;
    int gcResult = 0;
    int reactorResult = 0;
    double gcThreshold = 0;
    double gcRecycle = 0;
    uint32_t gcInterval = 0;
    std::vector<std::string> lifecycle;
    int lookupResult = 0;
    int initResult = 0;
    int sessionInitCalls = 0;
    std::string session;
    std::string trid;
    uint64_t offset = 0;
    uint32_t shards = 0;
    uint32_t shardKb = 0;
    int createCalls = 0;
    int destroyCalls = 0;
    int lookupCalls = 0;
    int loadCalls = 0;
    int dumpCalls = 0;
    int healthCalls = 0;
    int registerCalls = 0;
    int unregisterCalls = 0;
    uint32_t slots = 0;
    uint32_t firstShard = 0;
    uint32_t loadLength = 0;
    int healthy = 1;
    std::vector<int> cores;
    uint8_t dataByte = 0;
    std::set<UC::Detail::BlockId> hits;
} engine;

}  // namespace

struct spdk_store_engine {
    EngineState state;
    bool destroyed = false;
};

namespace {

std::vector<std::unique_ptr<spdk_store_engine>> contexts;
thread_local spdk_store_engine* selected = nullptr;
std::mutex mockMutex;
bool synchronizeLookups = false;
std::atomic<int> lookupArrivals{0};

EngineState& Current()
{
    EXPECT_NE(selected, nullptr);
    if (!selected) { return engine; }
    EXPECT_FALSE(selected->destroyed);
    return selected->state;
}

// Keep the original aggregate assertions while recording each instance separately.
template <typename F>
void Record(F update)
{
    std::lock_guard<std::mutex> lock(mockMutex);
    auto& state = Current();
    update(engine);
    if (&state != &engine) { update(state); }
}

class SpdkStoreSetupTest : public testing::Test {
protected:
    void SetUp() override
    {
        selected = nullptr;
        contexts.clear();
        engine = {};
        synchronizeLookups = false;
        lookupArrivals = 0;
    }

    void TearDown() override
    {
        EXPECT_EQ(selected, nullptr);
        EXPECT_EQ(engine.createCalls, engine.destroyCalls);
        for (const auto& context : contexts) { EXPECT_TRUE(context->destroyed); }
    }

    UC::Detail::Dictionary Config()
    {
        UC::Detail::Dictionary config;
        config.Set("spdk_trid", std::string("trtype:RDMA traddr:192.0.2.1 trsvcid:4420"));
        config.SetNumber("spdk_offset_gib", 8);
        return config;
    }
};

TEST_F(SpdkStoreSetupTest, Dp0SchedulerWithBlockSizeUsesLookupOnly)
{
    auto config = Config();
    // Mirrors UCMConnector._create_store() on the DP0 scheduler.
    config.SetNumber("block_size", 32768);
    config.Set("posix_gc_enable", true);
    {
        UC::SpdkStore::SpdkStore store;
        ASSERT_TRUE(store.Setup(config).Success());
        EXPECT_EQ(engine.lookupInitCalls, 1);
        EXPECT_EQ(engine.trid, "trtype:RDMA traddr:192.0.2.1 trsvcid:4420");
        EXPECT_EQ(engine.offset, 8);
        EXPECT_EQ(store.Load({}).Error().ToString(), "-1, lookup-only mode");
        EXPECT_EQ(store.Dump({}).Error().ToString(), "-1, lookup-only mode");
    }
    EXPECT_EQ(engine.lookupFiniCalls, 1);
    EXPECT_EQ(engine.initCalls, 0);
    EXPECT_EQ(engine.reactorStartCalls, 0);
    EXPECT_EQ(engine.finiCalls, 0);
    EXPECT_EQ(engine.reactorStopCalls, 0);
    EXPECT_EQ(engine.gcStartCalls, 0);
    EXPECT_EQ(engine.gcStopCalls, 0);
}

TEST_F(SpdkStoreSetupTest, SchedulerWithoutGeometryUsesLookupOnly)
{
    UC::SpdkStore::SpdkStore store;
    EXPECT_TRUE(store.Setup(Config()).Success());
    EXPECT_EQ(engine.lookupInitCalls, 1);
    EXPECT_EQ(engine.initCalls, 0);
}

TEST_F(SpdkStoreSetupTest, ExplicitNegativeDeviceIdUsesLookupOnly)
{
    auto config = Config();
    config.SetNumber("device_id", -1);
    config.SetNumber("block_size", 32768);
    UC::SpdkStore::SpdkStore store;
    EXPECT_TRUE(store.Setup(config).Success());
    EXPECT_EQ(engine.lookupInitCalls, 1);
    EXPECT_EQ(engine.initCalls, 0);
}

TEST_F(SpdkStoreSetupTest, SchedulerAttachFailureIsReported)
{
    engine.lookupResult = -EACCES;
    {
        UC::SpdkStore::SpdkStore store;
        auto config = Config();
        config.SetNumber("block_size", 32768);
        EXPECT_TRUE(store.Setup(config).Failure());
    }
    EXPECT_EQ(engine.lookupInitCalls, 1);
    EXPECT_EQ(engine.lookupFiniCalls, 0);
    EXPECT_EQ(engine.initCalls, 0);
}

TEST_F(SpdkStoreSetupTest, WorkerWithoutGeometryDoesNotBecomeScheduler)
{
    auto config = Config();
    config.SetNumber("device_id", 0);
    UC::SpdkStore::SpdkStore store;
    EXPECT_EQ(store.Setup(config), UC::Status::InvalidParam());
    EXPECT_EQ(engine.lookupInitCalls, 0);
    EXPECT_EQ(engine.initCalls, 0);
}

TEST_F(SpdkStoreSetupTest, WorkerWithOnlyBlockSizeRejectsMissingShardSize)
{
    auto config = Config();
    config.SetNumber("device_id", 0);
    config.SetNumber("block_size", 32768);
    UC::SpdkStore::SpdkStore store;
    EXPECT_EQ(store.Setup(config), UC::Status::InvalidParam());
    EXPECT_EQ(engine.lookupInitCalls, 0);
    EXPECT_EQ(engine.initCalls, 0);
}

TEST_F(SpdkStoreSetupTest, WorkerWithoutDeviceFailsInsteadOfAttachingSharedIndex)
{
    engine.initResult = -ENODEV;
    auto config = Config();
    config.SetNumber("device_id", 0);
    config.SetNumber("shard_size", 4096);
    config.SetNumber("block_size", 32768);
    {
        UC::SpdkStore::SpdkStore store;
        const auto status = store.Setup(config);
        EXPECT_TRUE(status.Failure());
        EXPECT_EQ(status.ToString(),
                  UC::Status::Error(fmt::format("spdk_store_init failed: {}", -ENODEV)).ToString());
    }
    EXPECT_EQ(engine.initCalls, 1);
    EXPECT_EQ(engine.lookupInitCalls, 0);
    EXPECT_EQ(engine.reactorStartCalls, 0);
    // A failed engine init owns its cleanup; the adapter must not finalize it again.
    EXPECT_EQ(engine.finiCalls, 0);
    EXPECT_EQ(engine.reactorStopCalls, 0);
    EXPECT_EQ(engine.lookupFiniCalls, 0);
}

TEST_F(SpdkStoreSetupTest, FormatRequiresSharedServiceId)
{
    auto config = Config();
    config.SetNumber("device_id", 0);
    config.SetNumber("shard_size", 4096);
    config.SetNumber("block_size", 32768);
    config.SetNumber("spdk_format", 1);
    UC::SpdkStore::SpdkStore store;
    EXPECT_EQ(store.Setup(config), UC::Status::InvalidParam());
    EXPECT_EQ(engine.initCalls, 0);
    EXPECT_EQ(engine.sessionInitCalls, 0);
}

TEST_F(SpdkStoreSetupTest, FormatPassesSharedServiceIdToEngine)
{
    auto config = Config();
    config.SetNumber("device_id", 0);
    config.SetNumber("shard_size", 4096);
    config.SetNumber("block_size", 32768);
    config.SetNumber("spdk_format", 1);
    config.Set("unique_id", std::string("service-uuid"));
    UC::SpdkStore::SpdkStore store;
    ASSERT_TRUE(store.Setup(config).Success());
    EXPECT_EQ(engine.sessionInitCalls, 1);
    EXPECT_EQ(engine.session, "service-uuid");
    EXPECT_EQ(engine.lookupInitCalls, 0);
}

TEST_F(SpdkStoreSetupTest, SchedulerIgnoresFormatAndNeverInitializesEngine)
{
    auto config = Config();
    config.SetNumber("spdk_format", 1);
    config.SetNumber("block_size", 32768);
    UC::SpdkStore::SpdkStore store;
    ASSERT_TRUE(store.Setup(config).Success());
    EXPECT_EQ(engine.lookupInitCalls, 1);
    EXPECT_EQ(engine.sessionInitCalls, 0);
    EXPECT_EQ(engine.initCalls, 0);
}

TEST_F(SpdkStoreSetupTest, WorkerWithGeometryStartsEngine)
{
    auto config = Config();
    config.SetNumber("device_id", 1);
    config.SetNumber("shard_size", 4096);
    config.SetNumber("block_size", 32768);
    {
        UC::SpdkStore::SpdkStore store;
        ASSERT_TRUE(store.Setup(config).Success());
        EXPECT_EQ(engine.initCalls, 1);
        EXPECT_EQ(engine.shards, 8);
        EXPECT_EQ(engine.shardKb, 4);
        EXPECT_EQ(engine.reactorStartCalls, 1);
        EXPECT_EQ(engine.lookupInitCalls, 0);
        EXPECT_EQ(engine.gcStartCalls, 1);
        EXPECT_DOUBLE_EQ(engine.gcThreshold, 0.7);
        EXPECT_DOUBLE_EQ(engine.gcRecycle, 0.1);
        EXPECT_EQ(engine.gcInterval, 30);
    }
    EXPECT_EQ(engine.finiCalls, 1);
    EXPECT_EQ(engine.reactorStopCalls, 1);
    EXPECT_EQ(engine.lookupFiniCalls, 0);
    EXPECT_EQ(engine.lifecycle, (std::vector<std::string>{"reactor_start", "gc_start",
                                                        "gc_stop", "reactor_stop", "fini"}));
}

TEST_F(SpdkStoreSetupTest, GcConfigDelegatedAndStoppedBeforeUnregister)
{
    auto config = Config();
    config.SetNumber("device_id", 0);
    config.SetNumber("shard_size", 4096);
    config.SetNumber("block_size", 32768);
    config.Set("spdk_gc_trigger_threshold_ratio", 0.8);
    config.Set("spdk_gc_recycle_percent", 0.2);
    config.SetNumber("spdk_gc_check_interval_sec", 7);
    UC::SpdkStore::SpdkStore store;
    ASSERT_TRUE(store.Setup(config).Success());
    EXPECT_DOUBLE_EQ(engine.gcThreshold, 0.8);
    EXPECT_DOUBLE_EQ(engine.gcRecycle, 0.2);
    EXPECT_EQ(engine.gcInterval, 7);
    store.UnregisterHostMemory(reinterpret_cast<void*>(4096), 4096);
    EXPECT_EQ(engine.lifecycle[2], "gc_stop");
    EXPECT_EQ(engine.lifecycle[3], "unregister");
}

TEST_F(SpdkStoreSetupTest, GcCanBeDisabled)
{
    auto config = Config();
    config.SetNumber("device_id", 0);
    config.SetNumber("shard_size", 4096);
    config.SetNumber("block_size", 32768);
    config.Set("spdk_gc_enable", false);
    UC::SpdkStore::SpdkStore store;
    ASSERT_TRUE(store.Setup(config).Success());
    EXPECT_EQ(engine.gcStartCalls, 0);
}

TEST_F(SpdkStoreSetupTest, InvalidGcConfigRejectedBeforeEngineInit)
{
    for (double value : {0.0, -0.1, 1.1, std::numeric_limits<double>::infinity(),
                         std::numeric_limits<double>::quiet_NaN()}) {
        for (const char* key : {"spdk_gc_trigger_threshold_ratio", "spdk_gc_recycle_percent"}) {
            auto config = Config();
            config.SetNumber("device_id", 0);
            config.SetNumber("shard_size", 4096);
            config.SetNumber("block_size", 32768);
            config.Set(key, value);
            UC::SpdkStore::SpdkStore store;
            EXPECT_EQ(store.Setup(config), UC::Status::InvalidParam());
        }
    }
    for (int64_t value : {int64_t{0}, int64_t{-1}, int64_t{1} << 32}) {
        auto config = Config();
        config.SetNumber("device_id", 0);
        config.SetNumber("shard_size", 4096);
        config.SetNumber("block_size", 32768);
        config.SetNumber("spdk_gc_check_interval_sec", value);
        UC::SpdkStore::SpdkStore store;
        EXPECT_EQ(store.Setup(config), UC::Status::InvalidParam());
    }
    EXPECT_EQ(engine.initCalls, 0);
}

TEST_F(SpdkStoreSetupTest, GcStartFailureCleansUpOnce)
{
    engine.gcResult = -EAGAIN;
    auto config = Config();
    config.SetNumber("device_id", 0);
    config.SetNumber("shard_size", 4096);
    config.SetNumber("block_size", 32768);
    {
        UC::SpdkStore::SpdkStore store;
        EXPECT_TRUE(store.Setup(config).Failure());
    }
    EXPECT_EQ(engine.lifecycle, (std::vector<std::string>{"reactor_start", "gc_start",
                                                        "gc_stop", "reactor_stop", "fini"}));
}

TEST_F(SpdkStoreSetupTest, ReactorFailureDoesNotStartGc)
{
    engine.reactorResult = -EAGAIN;
    auto config = Config();
    config.SetNumber("device_id", 0);
    config.SetNumber("shard_size", 4096);
    config.SetNumber("block_size", 32768);
    UC::SpdkStore::SpdkStore store;
    EXPECT_TRUE(store.Setup(config).Failure());
    EXPECT_EQ(engine.gcStartCalls, 0);
    EXPECT_EQ(engine.finiCalls, 1);
}

TEST_F(SpdkStoreSetupTest, SchedulerStoresUseIndependentIndexesAndRestoreSelection)
{
    UC::Detail::BlockId id{};
    auto first = std::make_unique<UC::SpdkStore::SpdkStore>();
    UC::SpdkStore::SpdkStore second;
    auto config = Config();
    ASSERT_TRUE(first->Setup(config).Success());
    config.SetNumber("spdk_offset_gib", 24);
    ASSERT_TRUE(second.Setup(config).Success());
    ASSERT_EQ(contexts.size(), 2);
    auto& a = contexts[0]->state;
    auto& b = contexts[1]->state;
    a.hits.insert(id);
    EXPECT_EQ(a.offset, 8);
    EXPECT_EQ(b.offset, 24);
    EXPECT_EQ(first->Lookup(&id, 1).Value(), std::vector<uint8_t>{1});
    EXPECT_EQ(second.Lookup(&id, 1).Value(), std::vector<uint8_t>{0});
    EXPECT_EQ(first->LookupOnPrefix(&id, 1).Value(), 0);
    EXPECT_EQ(second.LookupOnPrefix(&id, 1).Value(), -1);
    EXPECT_EQ(first->LookupOnReverse(&id, 1).Value(), 0);
    EXPECT_EQ(second.LookupOnReverse(&id, 1).Value(), -1);
    EXPECT_EQ(selected, nullptr);
    first.reset();
    EXPECT_EQ(a.lookupFiniCalls, 1);
    EXPECT_EQ(a.destroyCalls, 1);
    EXPECT_EQ(b.lookupFiniCalls, 0);
    EXPECT_EQ(b.destroyCalls, 0);
    b.hits.insert(id);
    EXPECT_EQ(second.Lookup(&id, 1).Value(), std::vector<uint8_t>{1});
    EXPECT_TRUE(second.CheckHealth().Success());
}

TEST_F(SpdkStoreSetupTest, ConcurrentStoresKeepThreadLocalSelection)
{
    UC::SpdkStore::SpdkStore first;
    UC::SpdkStore::SpdkStore second;
    ASSERT_TRUE(first.Setup(Config()).Success());
    auto config = Config();
    config.SetNumber("spdk_offset_gib", 24);
    ASSERT_TRUE(second.Setup(config).Success());
    UC::Detail::BlockId id{};
    contexts[0]->state.hits.insert(id);
    synchronizeLookups = true;
    auto run = [&](UC::SpdkStore::SpdkStore& store, uint8_t hit) {
        spdk_store_engine caller;
        EXPECT_EQ(spdk_store_engine_select(&caller), nullptr);
        for (int i = 0; i < 100; ++i) {
            EXPECT_EQ(store.Lookup(&id, 1).Value(), std::vector<uint8_t>{hit});
            EXPECT_EQ(selected, &caller);
            EXPECT_EQ(store.LookupOnPrefix(&id, 1).Value(), hit ? 0 : -1);
            EXPECT_EQ(selected, &caller);
            EXPECT_EQ(store.LookupOnReverse(&id, 1).Value(), hit ? 0 : -1);
            EXPECT_EQ(selected, &caller);
        }
        EXPECT_EQ(spdk_store_engine_select(nullptr), &caller);
    };
    std::thread a(run, std::ref(first), 1);
    std::thread b(run, std::ref(second), 0);
    a.join();
    b.join();
    EXPECT_EQ(selected, nullptr);
    EXPECT_EQ(contexts[0]->state.lookupCalls, 300);
    EXPECT_EQ(contexts[1]->state.lookupCalls, 300);
}

TEST_F(SpdkStoreSetupTest, WorkersKeepGeometryDispatchAndCleanupIndependent)
{
    auto config = Config();
    config.SetNumber("device_id", 0);
    config.SetNumber("shard_size", 4096);
    config.SetNumber("block_size", 32768);
    config.SetNumber("spdk_capacity_gb", 1);
    auto first = std::make_unique<UC::SpdkStore::SpdkStore>();
    auto second = std::make_unique<UC::SpdkStore::SpdkStore>();
    ASSERT_TRUE(first->Setup(config).Success());
    config.SetNumber("device_id", 1);
    config.SetNumber("spdk_offset_gib", 24);
    config.SetNumber("shard_size", 8192);
    config.SetNumber("block_size", 16384);
    ASSERT_TRUE(second->Setup(config).Success());
    auto& a = contexts[0]->state;
    auto& b = contexts[1]->state;
    EXPECT_EQ(a.shards, 8);
    EXPECT_EQ(a.shardKb, 4);
    EXPECT_EQ(a.offset, 8);
    EXPECT_EQ(a.slots, 32768);
    EXPECT_EQ(b.shards, 2);
    EXPECT_EQ(b.shardKb, 8);
    EXPECT_EQ(b.offset, 24);
    EXPECT_EQ(b.slots, 65536);
    a.dataByte = 0x31;
    b.dataByte = 0x72;
    std::array<uint8_t, 8192> buffer{};
    UC::Detail::BlockId id{};
    ASSERT_TRUE(first->RegisterHostMemory(buffer.data(), buffer.size()).Success());
    ASSERT_TRUE(second->RegisterHostMemory(buffer.data(), buffer.size()).Success());
    auto load = first->Load({{id, 7, {buffer.data()}}});
    ASSERT_TRUE(load);
    EXPECT_TRUE(first->Check(load.Value()).Value());
    EXPECT_TRUE(first->Wait(load.Value()).Success());
    EXPECT_EQ(buffer[4095], a.dataByte);
    EXPECT_EQ(buffer[4096], 0);
    EXPECT_EQ(a.firstShard, 7);
    EXPECT_EQ(a.loadLength, 4096);
    auto dump = second->Dump({{id, 1, {buffer.data()}}});
    ASSERT_TRUE(dump);
    EXPECT_TRUE(second->Wait(dump.Value()).Success());
    EXPECT_EQ(b.firstShard, 1);
    EXPECT_EQ(a.dumpCalls, 0);
    EXPECT_EQ(b.dumpCalls, 1);
    EXPECT_FALSE(second->Load({{id, 2, {buffer.data()}}}));
    EXPECT_EQ(b.loadCalls, 0);
    a.healthy = 0;
    EXPECT_TRUE(first->CheckHealth().Failure());
    EXPECT_TRUE(second->CheckHealth().Success());
    first.reset();
    EXPECT_EQ(a.lifecycle, (std::vector<std::string>{"reactor_start", "gc_start", "gc_stop",
                                                   "unregister", "reactor_stop", "fini", "destroy"}));
    EXPECT_EQ(b.gcStopCalls, 0);
    EXPECT_EQ(b.unregisterCalls, 0);
    EXPECT_EQ(b.finiCalls, 0);
    EXPECT_EQ(b.destroyCalls, 0);
    load = second->Load({{id, 1, {buffer.data()}}});
    ASSERT_TRUE(load);
    EXPECT_TRUE(second->Wait(load.Value()).Success());
    EXPECT_EQ(buffer.front(), b.dataByte);
    EXPECT_EQ(buffer.back(), b.dataByte);
    EXPECT_EQ(b.loadLength, 8192);
    second.reset();
    EXPECT_EQ(a.destroyCalls, 1);
    EXPECT_EQ(b.lifecycle, a.lifecycle);
    EXPECT_EQ(b.destroyCalls, 1);
}

TEST_F(SpdkStoreSetupTest, FailedSecondWorkerInitLeavesFirstUsable)
{
    auto config = Config();
    config.SetNumber("device_id", 0);
    config.SetNumber("shard_size", 4096);
    config.SetNumber("block_size", 32768);
    UC::SpdkStore::SpdkStore first;
    ASSERT_TRUE(first.Setup(config).Success());
    engine.initResult = -ENODEV;
    config.SetNumber("spdk_offset_gib", 24);
    {
        UC::SpdkStore::SpdkStore second;
        EXPECT_TRUE(second.Setup(config).Failure());
    }
    ASSERT_EQ(contexts.size(), 2);
    auto& a = contexts[0]->state;
    auto& b = contexts[1]->state;
    EXPECT_EQ(b.initCalls, 1);
    EXPECT_EQ(b.reactorStartCalls, 0);
    EXPECT_EQ(b.gcStopCalls, 0);
    EXPECT_EQ(b.finiCalls, 0);
    EXPECT_EQ(b.destroyCalls, 1);
    EXPECT_EQ(a.finiCalls, 0);
    EXPECT_EQ(a.destroyCalls, 0);
    UC::Detail::BlockId id{};
    a.hits.insert(id);
    EXPECT_EQ(first.Lookup(&id, 1).Value(), std::vector<uint8_t>{1});
    EXPECT_TRUE(first.CheckHealth().Success());
    std::array<uint8_t, 4096> buffer{};
    auto dump = first.Dump({{id, 0, {buffer.data()}}});
    ASSERT_TRUE(dump);
    EXPECT_TRUE(first.Wait(dump.Value()).Success());
    EXPECT_EQ(a.dumpCalls, 1);
    EXPECT_EQ(b.dumpCalls, 0);
}

TEST_F(SpdkStoreSetupTest, ReactorUsesStoreAffinityAndRejectsInvalidCore)
{
    auto config = Config();
    config.SetNumber("device_id", 0);
    config.SetNumber("shard_size", 4096);
    config.SetNumber("block_size", 32768);
    config.Set("cpu_affinity_cores", std::vector<ssize_t>{2, 4});
    UC::SpdkStore::SpdkStore store;
    ASSERT_TRUE(store.Setup(config).Success());
    EXPECT_EQ(contexts.back()->state.cores, (std::vector<int>{2, 4}));
    config.Set("cpu_affinity_cores", std::vector<ssize_t>{-1});
    UC::SpdkStore::SpdkStore invalid;
    EXPECT_TRUE(invalid.Setup(config).Failure());
    EXPECT_EQ(contexts.back()->state.initCalls, 0);
}

}  // namespace

extern "C" {

int spdk_store_set_cpu_affinity(const int* cores, uint32_t count)
{
    Record([&](EngineState& state) {
        state.cores.clear();
        if (count) { state.cores.assign(cores, cores + count); }
    });
    return 0;
}

int spdk_store_set_direct_only(int enabled)
{
    EXPECT_EQ(enabled, 1);
    EXPECT_NE(selected, nullptr);
    EXPECT_EQ(Current().initCalls, 0);
    return 0;
}

spdk_store_engine* spdk_store_engine_create()
{
    auto context = std::make_unique<spdk_store_engine>();
    context->state.lookupResult = engine.lookupResult;
    context->state.initResult = engine.initResult;
    context->state.reactorResult = engine.reactorResult;
    context->state.gcResult = engine.gcResult;
    ++engine.createCalls;
    contexts.push_back(std::move(context));
    return contexts.back().get();
}

spdk_store_engine* spdk_store_engine_select(spdk_store_engine* context)
{
    auto previous = selected;
    selected = context;
    return previous;
}

void spdk_store_engine_destroy(spdk_store_engine* context)
{
    ASSERT_NE(context, nullptr);
    EXPECT_FALSE(context->destroyed);
    EXPECT_EQ(selected, nullptr);
    ++engine.destroyCalls;
    ++context->state.destroyCalls;
    context->state.lifecycle.push_back("destroy");
    context->destroyed = true;
}

int spdk_store_lookup_init_region(const char* trid, uint64_t offset)
{
    Record([&](EngineState& state) {
        ++state.lookupInitCalls;
        state.trid = trid;
        state.offset = offset;
    });
    return Current().lookupResult;
}

void spdk_store_lookup_fini() { Record([](EngineState& state) { ++state.lookupFiniCalls; }); }

int spdk_store_init(const char* trid, uint32_t slots, uint32_t shards, uint32_t shardKb,
                    uint64_t offset, int)
{
    Record([&](EngineState& state) {
        ++state.initCalls;
        state.trid = trid;
        state.slots = slots;
        state.shards = shards;
        state.shardKb = shardKb;
        state.offset = offset;
    });
    return Current().initResult;
}

void spdk_store_fini()
{
    Record([](EngineState& state) { ++state.finiCalls; state.lifecycle.push_back("fini"); });
}
int spdk_store_init_for_session(const char* trid, uint32_t slots, uint32_t shards,
                               uint32_t shardKb, uint64_t offset, int format, const char* session)
{
    Record([&](EngineState& state) { ++state.sessionInitCalls; state.session = session; });
    return spdk_store_init(trid, slots, shards, shardKb, offset, format);
}
int spdk_store_reactor_start()
{
    Record([](EngineState& state) {
        ++state.reactorStartCalls;
        state.lifecycle.push_back("reactor_start");
    });
    return Current().reactorResult;
}
void spdk_store_reactor_stop()
{
    Record([](EngineState& state) {
        ++state.reactorStopCalls;
        state.lifecycle.push_back("reactor_stop");
    });
}
int spdk_store_gc_start(double threshold, double recycle, uint32_t interval)
{
    Record([&](EngineState& state) {
        ++state.gcStartCalls;
        state.gcThreshold = threshold;
        state.gcRecycle = recycle;
        state.gcInterval = interval;
        state.lifecycle.push_back("gc_start");
    });
    return Current().gcResult;
}
void spdk_store_gc_stop()
{
    Record([](EngineState& state) {
        ++state.gcStopCalls;
        state.lifecycle.push_back("gc_stop");
    });
}
int spdk_store_healthy()
{
    Record([](EngineState& state) { ++state.healthCalls; });
    return Current().healthy;
}
int spdk_store_register_memory(void*, uint64_t)
{
    Record([](EngineState& state) { ++state.registerCalls; });
    return 0;
}
int spdk_store_unregister_memory(void*, uint64_t)
{
    Record([](EngineState& state) {
        ++state.unregisterCalls;
        state.lifecycle.push_back("unregister");
    });
    return 0;
}
int spdk_store_lookup(const uint8_t id[16])
{
    if (synchronizeLookups && lookupArrivals.fetch_add(1) < 2) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (lookupArrivals.load() < 2 && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
        EXPECT_GE(lookupArrivals.load(), 2);
    }
    Record([](EngineState& state) { ++state.lookupCalls; });
    UC::Detail::BlockId block;
    std::memcpy(block.data(), id, block.size());
    return Current().hits.count(block) != 0;
}
uint32_t spdk_store_lookup_prefix(const uint8_t ids[][16], uint32_t count)
{
    uint32_t hits = 0;
    while (hits < count && spdk_store_lookup(ids[hits])) { ++hits; }
    return hits;
}
int spdk_store_load_submit(const uint8_t[16], uint32_t first, uint32_t count, void* dst,
                           uint32_t length, spdk_store_cb cb, void* arg)
{
    EXPECT_EQ(count, 1);
    Record([&](EngineState& state) {
        ++state.loadCalls;
        state.firstShard = first;
        state.loadLength = length;
    });
    std::memset(dst, Current().dataByte, length);
    cb(arg, 0);
    return 0;
}
int spdk_store_dump_submit(const uint8_t[16], uint32_t first, uint32_t count, const void*,
                           spdk_store_cb cb, void* arg)
{
    EXPECT_EQ(count, 1);
    Record([&](EngineState& state) { ++state.dumpCalls; state.firstShard = first; });
    cb(arg, 0);
    return 0;
}

}  // extern "C"

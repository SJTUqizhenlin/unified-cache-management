#include <gtest/gtest.h>

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
} engine;

class SpdkStoreSetupTest : public testing::Test {
protected:
    void SetUp() override { engine = {}; }

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

}  // namespace

extern "C" {

int spdk_store_lookup_init_region(const char* trid, uint64_t offset)
{
    ++engine.lookupInitCalls;
    engine.trid = trid;
    engine.offset = offset;
    return engine.lookupResult;
}

void spdk_store_lookup_fini() { ++engine.lookupFiniCalls; }

int spdk_store_init(const char*, uint32_t, uint32_t shards, uint32_t shardKb, uint64_t, int)
{
    ++engine.initCalls;
    engine.shards = shards;
    engine.shardKb = shardKb;
    return engine.initResult;
}

void spdk_store_fini() { ++engine.finiCalls; engine.lifecycle.push_back("fini"); }
int spdk_store_init_for_session(const char* trid, uint32_t slots, uint32_t shards,
                               uint32_t shardKb, uint64_t offset, int format, const char* session)
{
    ++engine.sessionInitCalls;
    engine.session = session;
    return spdk_store_init(trid, slots, shards, shardKb, offset, format);
}
int spdk_store_reactor_start()
{
    ++engine.reactorStartCalls;
    engine.lifecycle.push_back("reactor_start");
    return engine.reactorResult;
}
void spdk_store_reactor_stop()
{
    ++engine.reactorStopCalls;
    engine.lifecycle.push_back("reactor_stop");
}
int spdk_store_gc_start(double threshold, double recycle, uint32_t interval)
{
    ++engine.gcStartCalls;
    engine.gcThreshold = threshold;
    engine.gcRecycle = recycle;
    engine.gcInterval = interval;
    engine.lifecycle.push_back("gc_start");
    return engine.gcResult;
}
void spdk_store_gc_stop()
{
    ++engine.gcStopCalls;
    engine.lifecycle.push_back("gc_stop");
}
int spdk_store_healthy() { return 1; }
int spdk_store_register_memory(void*, uint64_t) { return -ENOTSUP; }
int spdk_store_unregister_memory(void*, uint64_t)
{
    engine.lifecycle.push_back("unregister");
    return -ENOTSUP;
}
int spdk_store_lookup(const uint8_t[16]) { return 0; }
uint32_t spdk_store_lookup_prefix(const uint8_t[][16], uint32_t) { return 0; }
int spdk_store_load_submit(const uint8_t[16], uint32_t, uint32_t, void*, uint32_t, spdk_store_cb,
                           void*)
{
    return -ENOTSUP;
}
int spdk_store_dump_submit(const uint8_t[16], uint32_t, uint32_t, const void*, spdk_store_cb, void*)
{
    return -ENOTSUP;
}

}  // extern "C"

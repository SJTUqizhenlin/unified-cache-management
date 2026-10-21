#include <gtest/gtest.h>
#include <sys/wait.h>
#include <unistd.h>
#include <limits>
#include <string>

#include "testSPDK/ucm_spdk_store/gc_policy.h"

namespace {

TEST(SpdkGcPolicyTest, RepeatsLiveFractionRoundsUntilBelowThreshold)
{
    uint32_t live = 100;
    int rounds = 0;
    while (auto count = store_gc_round_count(live, 100, 0.7, 0.1)) {
        live -= count;
        ++rounds;
    }
    EXPECT_EQ(rounds, 4);
    EXPECT_EQ(live, 66);
    EXPECT_EQ(store_gc_round_count(70, 100, 0.7, 0.1), 7);
    EXPECT_EQ(store_gc_round_count(69, 100, 0.7, 0.1), 0);
    EXPECT_EQ(store_gc_round_count(1, 1, 0.7, 0.1), 0);
    EXPECT_EQ(store_gc_round_count(100, 100, 1.0, 1.0), 100);
}

TEST(SpdkGcPolicyTest, ValidatesTypedParameters)
{
    EXPECT_TRUE(store_gc_config_valid(0.7, 0.1, 30));
    EXPECT_FALSE(store_gc_config_valid(0, 0.1, 30));
    EXPECT_FALSE(store_gc_config_valid(0.7, 1.1, 30));
    EXPECT_FALSE(store_gc_config_valid(0.7, 0.1, 0));
    EXPECT_FALSE(store_gc_config_valid(std::numeric_limits<double>::quiet_NaN(), 0.1, 30));
}

TEST(SpdkGcPolicyTest, ColdestUnpinnedSelectionIsBounded)
{
    gc_victim victims[2]{};
    int count = 0;
    const uint8_t id[16] = {1};
    store_gc_consider(victims, &count, 2, id, 1, 0, 100);
    store_gc_consider(victims, &count, 2, id, 2, 1, 1);
    store_gc_consider(victims, &count, 2, id, 3, 0, 50);
    store_gc_consider(victims, &count, 2, id, 4, 0, 75);
    EXPECT_EQ(count, 2);
    EXPECT_EQ(victims[0].slot, 3);
    EXPECT_EQ(victims[1].slot, 4);
    count = 0;
    store_gc_consider(victims, &count, 0, id, 1, 0, 1);
    EXPECT_EQ(count, 0);
}

TEST(SpdkGcPolicyTest, GateExcludesPeerAndRecoversOnProcessExit)
{
    const auto name = "/ucm_spdk_gc_test_" + std::to_string(getpid());
    int parent = store_gc_gate_open(name.c_str());
    ASSERT_GE(parent, 0);
    ASSERT_EQ(flock(parent, LOCK_EX | LOCK_NB), 0);
    pid_t child = fork();
    ASSERT_GE(child, 0);
    if (child == 0) {
        close(parent);
        int peer = store_gc_gate_open(name.c_str());
        if (peer < 0) { _exit(1); }
        if (flock(peer, LOCK_EX | LOCK_NB) == 0 || errno != EWOULDBLOCK) { _exit(2); }
        _exit(0);
    }
    int status = 0;
    ASSERT_EQ(waitpid(child, &status, 0), child);
    EXPECT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0);
    ASSERT_EQ(flock(parent, LOCK_UN), 0);
    child = fork();
    ASSERT_GE(child, 0);
    if (child == 0) {
        close(parent);
        int peer = store_gc_gate_open(name.c_str());
        if (peer < 0 || flock(peer, LOCK_EX | LOCK_NB) != 0) { _exit(3); }
        _exit(0); // kernel releases the lease without an explicit unlock
    }
    ASSERT_EQ(waitpid(child, &status, 0), child);
    EXPECT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0);
    EXPECT_EQ(flock(parent, LOCK_EX | LOCK_NB), 0);
    close(parent);
    shm_unlink(name.c_str());
}

TEST(SpdkGcPolicyTest, FourProcessesRecheckThresholdUnderCycleGate)
{
    const auto name = "/ucm_spdk_gc_tp4_test_" + std::to_string(getpid());
    auto* live = static_cast<uint32_t*>(mmap(nullptr, sizeof(uint32_t), PROT_READ | PROT_WRITE,
                                            MAP_SHARED | MAP_ANONYMOUS, -1, 0));
    ASSERT_NE(live, MAP_FAILED);
    *live = 100;
    pid_t children[4];
    for (auto& child : children) {
        child = fork();
        ASSERT_GE(child, 0);
        if (child == 0) {
            int gate = store_gc_gate_open(name.c_str());
            if (gate < 0) { _exit(1); }
            if (flock(gate, LOCK_EX | LOCK_NB) == 0) {
                while (auto count = store_gc_round_count(*live, 100, 0.7, 0.1)) {
                    *live -= count;
                    usleep(1000); // keep the cycle gate across all rounds
                }
            } else if (errno != EWOULDBLOCK) {
                _exit(2);
            }
            close(gate);
            _exit(0);
        }
    }
    for (auto child : children) {
        int status = 0;
        ASSERT_EQ(waitpid(child, &status, 0), child);
        EXPECT_TRUE(WIFEXITED(status));
        EXPECT_EQ(WEXITSTATUS(status), 0);
    }
    EXPECT_EQ(*live, 66);
    munmap(live, sizeof(uint32_t));
    shm_unlink(name.c_str());
}

}  // namespace

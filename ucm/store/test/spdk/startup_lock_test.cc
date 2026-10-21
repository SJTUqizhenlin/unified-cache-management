#include <gtest/gtest.h>
#include <poll.h>
#include <sys/wait.h>
#include <string>

#include "testSPDK/ucm_spdk_store/startup_lock.h"

namespace {

TEST(SpdkStartupLockTest, ConcurrentWorkerWaitsWithoutBlockingMaintenanceUpgrade)
{
    const auto name = "/ucm_spdk_startup_test_" + std::to_string(getpid());
    const auto maintenanceName = name + "_maintenance";
    int gate = -1;
    ASSERT_EQ(store_startup_open(name.c_str(), &gate), 0);
    int maintenance = shm_open(maintenanceName.c_str(), O_CREAT | O_RDWR, 0600);
    ASSERT_GE(maintenance, 0);
    ASSERT_EQ(flock(maintenance, LOCK_SH), 0);
    int ready[2];
    ASSERT_EQ(pipe(ready), 0);
    const pid_t child = fork();
    ASSERT_GE(child, 0);
    if (child == 0) {
        close(ready[0]);
        // Do not inherit the parent's flock ownership.
        close(gate);
        close(maintenance);
        const char marker = 'R';
        if (write(ready[1], &marker, 1) != 1) { _exit(1); }
        int childGate = -1;
        if (store_startup_open(name.c_str(), &childGate) != 0) { _exit(2); }
        int childMaintenance = shm_open(maintenanceName.c_str(), O_RDWR, 0600);
        if (childMaintenance < 0 || flock(childMaintenance, LOCK_SH) != 0) { _exit(3); }
        if (write(ready[1], &marker, 1) != 1) { _exit(4); }
        close(childMaintenance);
        store_startup_close(&childGate);
        _exit(0);
    }
    close(ready[1]);
    pollfd event{ready[0], POLLIN, 0};
    EXPECT_EQ(poll(&event, 1, 2000), 1);
    char marker;
    EXPECT_EQ(read(ready[0], &marker, 1), 1);
    // The follower cannot take a maintenance SH lock while waiting for startup.
    EXPECT_EQ(poll(&event, 1, 100), 0);
    EXPECT_EQ(flock(maintenance, LOCK_UN), 0);
    EXPECT_EQ(flock(maintenance, LOCK_EX | LOCK_NB), 0);
    EXPECT_EQ(flock(maintenance, LOCK_SH), 0);
    store_startup_close(&gate);
    const int completed = poll(&event, 1, 2000);
    EXPECT_EQ(completed, 1);
    if (completed == 1) {
        EXPECT_EQ(read(ready[0], &marker, 1), 1);
    } else {
        kill(child, SIGKILL);
    }
    int status = 0;
    EXPECT_EQ(waitpid(child, &status, 0), child);
    EXPECT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0);
    close(ready[0]);
    close(maintenance);
    shm_unlink(name.c_str());
    shm_unlink(maintenanceName.c_str());
}

TEST(SpdkStartupLockTest, ExistingWorkerStillPreventsExclusiveMaintenance)
{
    const auto name = "/ucm_spdk_existing_test_" + std::to_string(getpid());
    int existing = shm_open(name.c_str(), O_CREAT | O_RDWR, 0600);
    ASSERT_GE(existing, 0);
    ASSERT_EQ(flock(existing, LOCK_SH), 0);
    int joining = shm_open(name.c_str(), O_RDWR, 0600);
    ASSERT_GE(joining, 0);
    EXPECT_EQ(flock(joining, LOCK_EX | LOCK_NB), -1);
    EXPECT_EQ(errno, EWOULDBLOCK);
    close(existing);
    EXPECT_EQ(flock(joining, LOCK_EX | LOCK_NB), 0);
    close(joining);
    shm_unlink(name.c_str());
}

TEST(SpdkStartupLockTest, ServiceSessionPublishedOnceAcrossFourProcesses)
{
    const auto name = "/ucm_spdk_session_test_" + std::to_string(getpid());
    const auto leaseName = name + "_lease";
    int lease = shm_open(leaseName.c_str(), O_CREAT | O_RDWR, 0600);
    ASSERT_GE(lease, 0);
    pid_t children[4];
    for (auto& child : children) {
        child = fork();
        ASSERT_GE(child, 0);
        if (child == 0) {
            close(lease);
            int gate = -1;
            if (store_startup_open(name.c_str(), &gate) != 0) { _exit(2); }
            int fd = shm_open(leaseName.c_str(), O_RDWR, 0600);
            if (fd < 0) { _exit(3); }
            int match = store_session_matches(fd, "first-service");
            if (match < 0) { _exit(4); }
            if (!match && store_session_publish(fd, "first-service") != 0) { _exit(5); }
            close(fd);
            store_startup_close(&gate);
            _exit(match ? 0 : 1);
        }
    }
    int formats = 0;
    for (auto child : children) {
        int status = 0;
        ASSERT_EQ(waitpid(child, &status, 0), child);
        ASSERT_TRUE(WIFEXITED(status));
        ASSERT_LE(WEXITSTATUS(status), 1);
        formats += WEXITSTATUS(status);
    }
    EXPECT_EQ(formats, 1);
    EXPECT_EQ(store_session_matches(lease, "first-service"), 1);
    EXPECT_EQ(store_session_matches(lease, "next-service"), 0);
    EXPECT_EQ(store_session_publish(lease, "next-service"), 0);
    EXPECT_EQ(store_session_matches(lease, "next-service"), 1);
    EXPECT_EQ(store_session_publish(lease, nullptr), 0);
    EXPECT_EQ(store_session_matches(lease, "next-service"), 0);
    EXPECT_EQ(store_session_publish(lease, ""), -EINVAL);
    EXPECT_EQ(store_session_publish(lease, std::string(STORE_SESSION_SIZE, 'x').c_str()), -EINVAL);
    close(lease);
    shm_unlink(name.c_str());
    shm_unlink(leaseName.c_str());
}

}  // namespace

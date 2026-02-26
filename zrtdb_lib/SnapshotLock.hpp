// ZRTDB（Zero-copy Real-Time Data Bus）
// Copyright (c) 2026 邹德虎 （Zou Dehu）
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <pthread.h>
#include <string>
#include <thread>

namespace mmdb {

// 进程间快照锁：
// - 写者持读锁（一个写入批次外层）
// - 快照保存/下装持写锁（短暂停写入）
class SnapshotLock {
public:
    static SnapshotLock& instance();

    bool ensureOpenForCurrentApp();

    void rdlock();
    void wrlock();
    void unlock();

private:
    SnapshotLock() = default;
    ~SnapshotLock();

    SnapshotLock(const SnapshotLock&) = delete;
    SnapshotLock& operator=(const SnapshotLock&) = delete;

    // Lock file layout (v2): add owner pid + heartbeat to enable crash recovery.
    // NOTE: Consistency requirements are intentionally loose; this is for engineering robustness.
    static constexpr std::uint32_t kMaxReaders = 64;
    struct ReaderSlot {
        std::int32_t pid; // 0 = empty
        std::uint64_t heartbeatMs; // CLOCK_MONOTONIC ms
    };

    struct LockFileLayout {
        std::uint32_t magic;
        std::uint32_t version;
        std::uint32_t generation;
        std::uint32_t reserved0;
        pthread_rwlock_t rwlock;

        // owner metadata
        std::int32_t writerPid; // 0 = none
        std::uint32_t reserved1;
        std::uint64_t writerHbMs; // CLOCK_MONOTONIC ms

        ReaderSlot readers[kMaxReaders];
    };

    bool openOrCreate_(const std::filesystem::path& appRoot);

    // crash-recovery helpers
    bool tryRecoverFromStaleOwner_();
    bool forceRebuildLockFile_();
    void cleanupDeadOwners_();
    void touchHeartbeat_();
    void touchReadSlot_();
    void touchWriteOwner_();
    static std::uint64_t nowMonotonicMs_();
    static bool pidAlive_(std::int32_t pid);
    void startHeartbeatThreadIfNeeded_();
    void stopHeartbeatThread_();

    std::filesystem::path openedAppRoot_;
    int fd_ = -1;
    LockFileLayout* mapped_ = nullptr;

    // local state (per-process)
    int readerSlotIndex_ = -1;
    std::atomic<int> rdHoldCount_ { 0 };
    std::atomic<int> wrHoldCount_ { 0 };
    std::atomic<bool> hbThreadRunning_ { false };
    std::thread hbThread_;
};

class SnapshotReadGuard {
public:
    SnapshotReadGuard()
    {
        SnapshotLock::instance().ensureOpenForCurrentApp();
        SnapshotLock::instance().rdlock();
    }
    ~SnapshotReadGuard() { SnapshotLock::instance().unlock(); }
};

class SnapshotWriteGuard {
public:
    SnapshotWriteGuard()
    {
        SnapshotLock::instance().ensureOpenForCurrentApp();
        SnapshotLock::instance().wrlock();
    }
    ~SnapshotWriteGuard() { SnapshotLock::instance().unlock(); }
};

} // namespace mmdb

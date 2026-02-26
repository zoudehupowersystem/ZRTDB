// ZRTDB（Zero-copy Real-Time Data Bus）
// Copyright (c) 2026 邹德虎 （Zou Dehu）
// SPDX-License-Identifier: Apache-2.0

#include "SnapshotLock.hpp"
#include "MmdbManager.hpp"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <thread>
#include <time.h>
#include <unistd.h>

namespace mmdb {

static constexpr std::uint32_t kMagic = 0x5A52544Bu; // 'ZRTB'
static constexpr std::uint32_t kVersion = 2;

// Tuning knobs (engineering robustness > strict consistency)
static constexpr std::uint64_t kTryLockTimeoutMs = 100; // timed lock granularity
static constexpr std::uint64_t kHeartbeatIntervalMs = 200; // while holding lock
static constexpr std::uint64_t kHeartbeatStaleMs = 1500; // stale if no hb within this window
static constexpr std::uint64_t kForceRebuildAfterMs = 5000; // if no owners recorded but still blocked

// per-thread lock mode tracking (needed for correct local counters)
static thread_local int tls_lock_mode = 0; // 0 none, 1 read, 2 write

static timespec add_ms_realtime(std::uint64_t ms)
{
    timespec ts;
    ::clock_gettime(CLOCK_REALTIME, &ts);
    std::uint64_t nsec = static_cast<std::uint64_t>(ts.tv_nsec) + (ms % 1000) * 1000ULL * 1000ULL;
    ts.tv_sec += static_cast<time_t>(ms / 1000) + static_cast<time_t>(nsec / (1000ULL * 1000ULL * 1000ULL));
    ts.tv_nsec = static_cast<long>(nsec % (1000ULL * 1000ULL * 1000ULL));
    return ts;
}

SnapshotLock& SnapshotLock::instance()
{
    static SnapshotLock inst;
    return inst;
}

SnapshotLock::~SnapshotLock()
{
    stopHeartbeatThread_();
    if (mapped_) {
        ::munmap(mapped_, sizeof(LockFileLayout));
        mapped_ = nullptr;
    }
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

std::uint64_t SnapshotLock::nowMonotonicMs_()
{
    timespec ts;
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000ULL + static_cast<std::uint64_t>(ts.tv_nsec) / 1000000ULL;
}

bool SnapshotLock::pidAlive_(std::int32_t pid)
{
    if (pid <= 0)
        return false;
    int rc = ::kill(static_cast<pid_t>(pid), 0);
    if (rc == 0)
        return true;
    if (errno == EPERM)
        return true; // exists but no permission
    return false;
}

bool SnapshotLock::ensureOpenForCurrentApp()
{
    auto appRoot = MmdbManager::instance().getAppRootPath();
    if (appRoot.empty())
        return false;

    if (mapped_ && openedAppRoot_ == appRoot)
        return true;

    // close old
    stopHeartbeatThread_();
    if (mapped_) {
        ::munmap(mapped_, sizeof(LockFileLayout));
        mapped_ = nullptr;
    }
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    openedAppRoot_.clear();
    readerSlotIndex_ = -1;
    rdHoldCount_.store(0);
    wrHoldCount_.store(0);

    return openOrCreate_(appRoot);
}

bool SnapshotLock::openOrCreate_(const std::filesystem::path& appRoot)
{
    openedAppRoot_ = appRoot;
    try {
        std::filesystem::create_directories(appRoot);
    } catch (...) {
    }

    auto lockPath = appRoot / ".snaplock";

    fd_ = ::open(lockPath.c_str(), O_RDWR | O_CREAT, 0666);
    if (fd_ < 0) {
        std::cerr << "[snapshot] open lock failed: " << lockPath << " err=" << std::strerror(errno) << "\n";
        return false;
    }

    // serialize init
    if (::flock(fd_, LOCK_EX) != 0) {
        std::cerr << "[snapshot] flock init failed: " << lockPath << "\n";
        return false;
    }

    // ensure file size
    struct stat st {};
    if (::fstat(fd_, &st) != 0) {
        ::flock(fd_, LOCK_UN);
        return false;
    }
    if ((size_t)st.st_size < sizeof(LockFileLayout)) {
        if (::ftruncate(fd_, (off_t)sizeof(LockFileLayout)) != 0) {
            std::cerr << "[snapshot] ftruncate lock failed\n";
            ::flock(fd_, LOCK_UN);
            return false;
        }
    }

    void* p = ::mmap(nullptr, sizeof(LockFileLayout), PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
    if (p == MAP_FAILED) {
        std::cerr << "[snapshot] mmap lock failed: " << std::strerror(errno) << "\n";
        ::flock(fd_, LOCK_UN);
        return false;
    }
    mapped_ = reinterpret_cast<LockFileLayout*>(p);

    // init if needed (or upgrade)
    if (mapped_->magic != kMagic || mapped_->version != kVersion) {
        std::uint32_t nextGen = mapped_->generation + 1;
        std::memset(mapped_, 0, sizeof(LockFileLayout));

        pthread_rwlockattr_t attr;
        ::pthread_rwlockattr_init(&attr);
        ::pthread_rwlockattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);

#ifdef PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP
        // glibc extension: reduce writer starvation risk
        ::pthread_rwlockattr_setkind_np(&attr, PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP);
#endif

        if (::pthread_rwlock_init(&mapped_->rwlock, &attr) != 0) {
            std::cerr << "[snapshot] pthread_rwlock_init failed\n";
            ::pthread_rwlockattr_destroy(&attr);
            ::flock(fd_, LOCK_UN);
            return false;
        }
        ::pthread_rwlockattr_destroy(&attr);

        mapped_->magic = kMagic;
        mapped_->version = kVersion;
        mapped_->generation = nextGen;
        mapped_->writerPid = 0;
        mapped_->writerHbMs = 0;
        for (std::uint32_t i = 0; i < kMaxReaders; ++i) {
            mapped_->readers[i].pid = 0;
            mapped_->readers[i].heartbeatMs = 0;
        }

        ::msync(mapped_, sizeof(LockFileLayout), MS_SYNC);
    }

    ::flock(fd_, LOCK_UN);
    return true;
}

void SnapshotLock::startHeartbeatThreadIfNeeded_()
{
    bool expected = false;
    if (!hbThreadRunning_.compare_exchange_strong(expected, true))
        return;

    hbThread_ = std::thread([this]() {
        while (hbThreadRunning_.load()) {
            if (!mapped_) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }
            if (wrHoldCount_.load() > 0 || rdHoldCount_.load() > 0) {
                touchHeartbeat_();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(kHeartbeatIntervalMs));
        }
    });
}

void SnapshotLock::stopHeartbeatThread_()
{
    if (!hbThreadRunning_.exchange(false))
        return;
    if (hbThread_.joinable()) {
        hbThread_.join();
    }
}

void SnapshotLock::touchWriteOwner_()
{
    if (!mapped_)
        return;
    mapped_->writerPid = static_cast<std::int32_t>(::getpid());
    mapped_->writerHbMs = nowMonotonicMs_();
}

void SnapshotLock::touchReadSlot_()
{
    if (!mapped_)
        return;
    const std::int32_t pid = static_cast<std::int32_t>(::getpid());
    const std::uint64_t now = nowMonotonicMs_();

    // reuse existing slot if valid
    if (readerSlotIndex_ >= 0) {
        auto& s = mapped_->readers[static_cast<std::uint32_t>(readerSlotIndex_)];
        if (s.pid == pid) {
            s.heartbeatMs = now;
            return;
        }
    }

    // find an existing slot for this pid
    int found = -1;
    int empty = -1;
    int reclaim = -1;
    std::uint64_t minHb = UINT64_MAX;

    for (std::uint32_t i = 0; i < kMaxReaders; ++i) {
        auto& s = mapped_->readers[i];
        if (s.pid == pid) {
            found = static_cast<int>(i);
            break;
        }
        if (s.pid == 0 && empty < 0) {
            empty = static_cast<int>(i);
            continue;
        }
        // candidate for reclaim: dead or stale, otherwise choose oldest
        bool dead = (s.pid != 0) && (!pidAlive_(s.pid));
        bool stale = (s.pid != 0) && (now > s.heartbeatMs) && (now - s.heartbeatMs > kHeartbeatStaleMs);
        if (dead || stale) {
            reclaim = static_cast<int>(i);
            minHb = s.heartbeatMs;
        } else if (s.pid != 0 && s.heartbeatMs < minHb) {
            // fallback oldest
            reclaim = static_cast<int>(i);
            minHb = s.heartbeatMs;
        }
    }

    int idx = (found >= 0) ? found : ((empty >= 0) ? empty : reclaim);
    if (idx < 0)
        idx = 0;

    readerSlotIndex_ = idx;
    mapped_->readers[static_cast<std::uint32_t>(idx)].pid = pid;
    mapped_->readers[static_cast<std::uint32_t>(idx)].heartbeatMs = now;
}

void SnapshotLock::touchHeartbeat_()
{
    if (!mapped_)
        return;
    if (wrHoldCount_.load() > 0) {
        touchWriteOwner_();
        return;
    }
    if (rdHoldCount_.load() > 0) {
        touchReadSlot_();
        return;
    }
}

void SnapshotLock::cleanupDeadOwners_()
{
    if (!mapped_)
        return;
    const std::uint64_t now = nowMonotonicMs_();

    // writer
    if (mapped_->writerPid != 0) {
        bool dead = !pidAlive_(mapped_->writerPid);
        bool stale = (now > mapped_->writerHbMs) && (now - mapped_->writerHbMs > kHeartbeatStaleMs);
        if (dead && stale) {
            mapped_->writerPid = 0;
            mapped_->writerHbMs = 0;
        }
    }

    // readers
    for (std::uint32_t i = 0; i < kMaxReaders; ++i) {
        auto& s = mapped_->readers[i];
        if (s.pid == 0)
            continue;
        bool dead = !pidAlive_(s.pid);
        bool stale = (now > s.heartbeatMs) && (now - s.heartbeatMs > kHeartbeatStaleMs);
        if (dead && stale) {
            s.pid = 0;
            s.heartbeatMs = 0;
        }
    }
}

bool SnapshotLock::forceRebuildLockFile_()
{
    if (!mapped_)
        return false;

    std::uint32_t nextGen = mapped_->generation + 1;
    std::memset(mapped_, 0, sizeof(LockFileLayout));

    pthread_rwlockattr_t attr;
    ::pthread_rwlockattr_init(&attr);
    ::pthread_rwlockattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);

#ifdef PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP
    ::pthread_rwlockattr_setkind_np(&attr, PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP);
#endif

    if (::pthread_rwlock_init(&mapped_->rwlock, &attr) != 0) {
        ::pthread_rwlockattr_destroy(&attr);
        return false;
    }
    ::pthread_rwlockattr_destroy(&attr);

    mapped_->magic = kMagic;
    mapped_->version = kVersion;
    mapped_->generation = nextGen;

    ::msync(mapped_, sizeof(LockFileLayout), MS_SYNC);
    return true;
}

bool SnapshotLock::tryRecoverFromStaleOwner_()
{
    if (!mapped_ || fd_ < 0)
        return false;

    // Serialize recovery with flock. Use non-blocking to reduce interference.
    if (::flock(fd_, LOCK_EX | LOCK_NB) != 0)
        return false;

    bool rebuilt = false;

    // First, cleanup dead/stale owners.
    std::uint64_t now = nowMonotonicMs_();
    bool staleFound = false;

    if (mapped_->writerPid != 0) {
        bool dead = !pidAlive_(mapped_->writerPid);
        bool stale = (now > mapped_->writerHbMs) && (now - mapped_->writerHbMs > kHeartbeatStaleMs);
        if (dead && stale)
            staleFound = true;
    }
    for (std::uint32_t i = 0; i < kMaxReaders && !staleFound; ++i) {
        auto& s = mapped_->readers[i];
        if (s.pid == 0)
            continue;
        bool dead = !pidAlive_(s.pid);
        bool stale = (now > s.heartbeatMs) && (now - s.heartbeatMs > kHeartbeatStaleMs);
        if (dead && stale)
            staleFound = true;
    }

    cleanupDeadOwners_();

    // If stale owner existed, we rebuild the rwlock in-place.
    if (staleFound) {
        rebuilt = forceRebuildLockFile_();
    }

    ::flock(fd_, LOCK_UN);
    return rebuilt;
}

void SnapshotLock::rdlock()
{
    if (!mapped_)
        return;

    std::uint64_t waitedMs = 0;
    while (true) {
        timespec ts = add_ms_realtime(kTryLockTimeoutMs);
        int rc = ::pthread_rwlock_timedrdlock(&mapped_->rwlock, &ts);
        if (rc == 0) {
            tls_lock_mode = 1;
            rdHoldCount_.fetch_add(1);
            touchReadSlot_();
            startHeartbeatThreadIfNeeded_();
            return;
        }
        if (rc == ETIMEDOUT) {
            waitedMs += kTryLockTimeoutMs;
            // try recovery if we suspect a dead owner
            (void)tryRecoverFromStaleOwner_();

            // if no owners are recorded but lock still blocks too long, force rebuild
            if (waitedMs >= kForceRebuildAfterMs) {
                if (::flock(fd_, LOCK_EX | LOCK_NB) == 0) {
                    bool anyOwner = (mapped_->writerPid != 0);
                    if (!anyOwner) {
                        for (std::uint32_t i = 0; i < kMaxReaders; ++i) {
                            if (mapped_->readers[i].pid != 0) {
                                anyOwner = true;
                                break;
                            }
                        }
                    }
                    if (!anyOwner) {
                        (void)forceRebuildLockFile_();
                        waitedMs = 0;
                    }
                    ::flock(fd_, LOCK_UN);
                }
            }
            continue;
        }

        std::cerr << "[snapshot] rdlock failed rc=" << rc << "\n";
        // conservative fallback: block (keeps prior semantics) if timed lock not supported
        ::pthread_rwlock_rdlock(&mapped_->rwlock);
        tls_lock_mode = 1;
        rdHoldCount_.fetch_add(1);
        touchReadSlot_();
        startHeartbeatThreadIfNeeded_();
        return;
    }
}

void SnapshotLock::wrlock()
{
    if (!mapped_)
        return;

    std::uint64_t waitedMs = 0;
    while (true) {
        timespec ts = add_ms_realtime(kTryLockTimeoutMs);
        int rc = ::pthread_rwlock_timedwrlock(&mapped_->rwlock, &ts);
        if (rc == 0) {
            tls_lock_mode = 2;
            wrHoldCount_.fetch_add(1);
            touchWriteOwner_();
            startHeartbeatThreadIfNeeded_();
            return;
        }
        if (rc == ETIMEDOUT) {
            waitedMs += kTryLockTimeoutMs;
            (void)tryRecoverFromStaleOwner_();

            if (waitedMs >= kForceRebuildAfterMs) {
                if (::flock(fd_, LOCK_EX | LOCK_NB) == 0) {
                    bool anyOwner = (mapped_->writerPid != 0);
                    if (!anyOwner) {
                        for (std::uint32_t i = 0; i < kMaxReaders; ++i) {
                            if (mapped_->readers[i].pid != 0) {
                                anyOwner = true;
                                break;
                            }
                        }
                    }
                    if (!anyOwner) {
                        (void)forceRebuildLockFile_();
                        waitedMs = 0;
                    }
                    ::flock(fd_, LOCK_UN);
                }
            }
            continue;
        }

        std::cerr << "[snapshot] wrlock failed rc=" << rc << "\n";
        ::pthread_rwlock_wrlock(&mapped_->rwlock);
        tls_lock_mode = 2;
        wrHoldCount_.fetch_add(1);
        touchWriteOwner_();
        startHeartbeatThreadIfNeeded_();
        return;
    }
}

void SnapshotLock::unlock()
{
    if (!mapped_)
        return;

    (void)::pthread_rwlock_unlock(&mapped_->rwlock);

    const std::int32_t pid = static_cast<std::int32_t>(::getpid());
    if (tls_lock_mode == 2) {
        int left = wrHoldCount_.fetch_sub(1) - 1;
        if (left <= 0) {
            // clear write owner marker
            mapped_->writerPid = 0;
            mapped_->writerHbMs = 0;
        }
    } else if (tls_lock_mode == 1) {
        int left = rdHoldCount_.fetch_sub(1) - 1;
        if (left <= 0) {
            // clear our reader slot (best-effort)
            if (readerSlotIndex_ >= 0) {
                auto& s = mapped_->readers[static_cast<std::uint32_t>(readerSlotIndex_)];
                if (s.pid == pid) {
                    s.pid = 0;
                    s.heartbeatMs = 0;
                }
            } else {
                // scan and clear any slot for this pid
                for (std::uint32_t i = 0; i < kMaxReaders; ++i) {
                    auto& s = mapped_->readers[i];
                    if (s.pid == pid) {
                        s.pid = 0;
                        s.heartbeatMs = 0;
                        break;
                    }
                }
            }
        }
    }

    tls_lock_mode = 0;

    // Stop heartbeat thread when no lock is held in this process.
    if (rdHoldCount_.load() <= 0 && wrHoldCount_.load() <= 0) {
        stopHeartbeatThread_();
    }
}

} // namespace mmdb

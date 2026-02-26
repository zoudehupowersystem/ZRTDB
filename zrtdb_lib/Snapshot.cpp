// ZRTDB（Zero-copy Real-Time Data Bus）
// Copyright (c) 2026 邹德虎 （Zou Dehu）
// SPDX-License-Identifier: Apache-2.0

#include "Snapshot.hpp"
#include "MmdbManager.hpp"
#include "SnapshotLock.hpp"
#include "StringUtils.h"
#include "zrtdb_const.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>

#include <fcntl.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {

// format local time with ms
static mmdb::snapshot::SnapshotInfo make_info(const fs::path& appRoot)
{
    using namespace std::chrono;

    auto now = system_clock::now();
    auto epochMs = duration_cast<milliseconds>(now.time_since_epoch()).count();
    auto msPart = (int)(epochMs % 1000);

    std::time_t t = system_clock::to_time_t(now);
    std::tm tm {};
    localtime_r(&t, &tm);

    std::ostringstream tsDir;
    tsDir << std::put_time(&tm, "%Y%m%d-%H%M%S") << "." << std::setw(3) << std::setfill('0') << msPart;

    std::ostringstream tsLocal;
    tsLocal << std::put_time(&tm, "%F %T") << "." << std::setw(3) << std::setfill('0') << msPart;

    mmdb::snapshot::SnapshotInfo info;
    info.appUpper = appRoot.filename().string(); // /var/ZRTDB/<APP>
    info.createdLocal = tsLocal.str();
    info.epochMs = (std::uint64_t)epochMs;

    // folder name: APP + "_" + time
    std::string folder = info.appUpper + "_" + tsDir.str();
    info.dir = appRoot / folder;
    return info;
}

static bool fdatasync_quiet(int fd)
{
    if (fd < 0)
        return false;
    // fdatasync on O_RDONLY is permitted on Linux; ignore errors for robustness.
    ::fdatasync(fd);
    return true;
}

static std::uintmax_t file_size_quiet(const fs::path& p)
{
    try {
        return fs::file_size(p);
    } catch (...) {
        return 0;
    }
}

// try reflink clone: dest must exist and be empty/truncated
static bool try_reflink(int srcFd, int dstFd)
{
#ifdef FICLONE
    if (srcFd < 0 || dstFd < 0)
        return false;
    if (::ioctl(dstFd, FICLONE, srcFd) == 0)
        return true;
#endif
    return false;
}

// sparse copy using SEEK_DATA/SEEK_HOLE when available; fallback to normal copy
static std::string copy_sparse_or_full(int srcFd, int dstFd, std::uintmax_t size)
{
    if (srcFd < 0 || dstFd < 0)
        return "copy";

    // best effort: set dest size
    ::ftruncate(dstFd, (off_t)size);

#if defined(SEEK_DATA) && defined(SEEK_HOLE)
    off_t end = (off_t)size;
    off_t pos = 0;
    const size_t kBuf = 1 << 20;
    std::string method = "sparse";

    while (pos < end) {
        off_t data = ::lseek(srcFd, pos, SEEK_DATA);
        if (data < 0) {
            // no more data; keep hole to end
            break;
        }
        off_t hole = ::lseek(srcFd, data, SEEK_HOLE);
        if (hole < 0) {
            method = "copy";
            break;
        }
        // copy [data, hole)
        off_t cur = data;
        std::vector<char> buf(kBuf);
        while (cur < hole) {
            size_t toRead = (size_t)std::min<off_t>((off_t)buf.size(), hole - cur);
            ssize_t r = ::pread(srcFd, buf.data(), toRead, cur);
            if (r <= 0) {
                method = "copy";
                break;
            }
            ssize_t w = ::pwrite(dstFd, buf.data(), (size_t)r, cur);
            if (w != r) {
                method = "copy";
                break;
            }
            cur += r;
        }
        if (method == "copy")
            break;
        pos = hole;
    }

    if (method == "sparse") {
        ::fdatasync(dstFd);
        return method;
    }
#endif

    // fallback: full copy
    {
        const size_t kBuf = 1 << 20;
        std::vector<char> buf(kBuf);
        off_t cur = 0;
        while ((std::uintmax_t)cur < size) {
            size_t toRead = (size_t)std::min<std::uintmax_t>(buf.size(), size - (std::uintmax_t)cur);
            ssize_t r = ::pread(srcFd, buf.data(), toRead, cur);
            if (r <= 0)
                break;
            ssize_t w = ::pwrite(dstFd, buf.data(), (size_t)r, cur);
            if (w != r)
                break;
            cur += r;
        }
        ::fdatasync(dstFd);
    }
    return "copy";
}

static mmdb::snapshot::FileItem copy_one_file(const fs::path& src, const fs::path& dst)
{
    mmdb::snapshot::FileItem item;
    item.rel = dst;
    item.size = file_size_quiet(src);

    fs::create_directories(dst.parent_path());

    int sfd = ::open(src.c_str(), O_RDONLY);
    if (sfd < 0) {
        item.method = "missing";
        return item;
    }

    // Flush dirty pages (best effort). If writers cooperate, this makes snapshot stable.
    fdatasync_quiet(sfd);

    int dfd = ::open(dst.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0666);
    if (dfd < 0) {
        ::close(sfd);
        item.method = "dst_open_fail";
        return item;
    }

    if (try_reflink(sfd, dfd)) {
        item.method = "reflink";
        ::fdatasync(dfd);
        ::close(dfd);
        ::close(sfd);
        return item;
    }

    item.method = copy_sparse_or_full(sfd, dfd, item.size);
    ::close(dfd);
    ::close(sfd);
    return item;
}

static void write_manifest(const mmdb::snapshot::SnapshotInfo& info, const std::string& note)
{
    fs::path mf = info.dir / "manifest.json";
    std::ofstream ofs(mf, std::ios::trunc);
    if (!ofs)
        return;

    ofs << "{\n";
    ofs << "  \"app\": \"" << info.appUpper << "\",\n";
    ofs << "  \"created_local\": \"" << info.createdLocal << "\",\n";
    ofs << "  \"epoch_ms\": " << info.epochMs << ",\n";
    ofs << "  \"note\": \"" << note << "\",\n";
    ofs << "  \"files\": [\n";
    for (size_t i = 0; i < info.files.size(); ++i) {
        const auto& f = info.files[i];
        // store relative path from snapshot dir
        fs::path rel = fs::relative(f.rel, info.dir);
        ofs << "    {\"rel\": \"" << rel.string() << "\", \"size\": " << f.size << ", \"method\": \"" << f.method << "\"}";
        if (i + 1 < info.files.size())
            ofs << ",";
        ofs << "\n";
    }
    ofs << "  ]\n";
    ofs << "}\n";
    ofs.close();
}

static std::string trim_zero_padded(std::string_view s)
{
    // Preserve legacy semantics:
    // 1) stop at first NUL (historical fixed-size char[] padding)
    // 2) trim leading/trailing whitespace
    std::string out(s);
    auto z = out.find('\0');
    if (z != std::string::npos)
        out.erase(z);
    // whitespace trim (same as old implementation)
    while (!out.empty() && std::isspace((unsigned char)out.back()))
        out.pop_back();
    while (!out.empty() && std::isspace((unsigned char)out.front()))
        out.erase(out.begin());
    return out;
}

} // namespace

namespace mmdb::snapshot {

SnapshotInfo save(std::string note)
{
    // Stop writers (cooperative)
    SnapshotWriteGuard guard;

    auto appRoot = MmdbManager::instance().getAppRootPath();
    if (appRoot.empty()) {
        throw std::runtime_error("[snapshot] app context not set");
    }

    SnapshotInfo info = make_info(appRoot);

    // create snapshot dir
    fs::create_directories(info.dir);

    // copy meta/apps
    {
        fs::path srcMeta = appRoot / "meta" / "apps";
        fs::path dstMeta = info.dir / "meta" / "apps";

        std::string appUpper = appRoot.filename().string();
        fs::path src1 = srcMeta / (appUpper + ".sec");
        fs::path src2 = srcMeta / (appUpper + "_NEW.sec");

        info.files.push_back(copy_one_file(src1, dstMeta / (appUpper + ".sec")));
        info.files.push_back(copy_one_file(src2, dstMeta / (appUpper + "_NEW.sec")));
    }

    // copy zrtdb_data based on g_runtime_app partition list
    {
        fs::path srcSecs = appRoot / "zrtdb_data";
        fs::path dstSecs = info.dir / "zrtdb_data";

        // Build db name table (1-based in partition_db_1based)
        std::vector<std::string> dbs;
        for (int i = 0; i < g_runtime_app.db_count; ++i) {
            dbs.push_back(mmdb::utils::toUpper(trim_zero_padded(g_runtime_app.db_ids[i])));
        }

        for (int p = 0; p < g_runtime_app.partition_count; ++p) {
            std::string partUpper = mmdb::utils::toUpper(trim_zero_padded(g_runtime_app.partition_ids[p]));
            if (partUpper.empty())
                continue;

            int dbi1 = g_runtime_app.partition_db_1based[p]; // 1-based
            std::string dbUpper = partUpper;
            if (dbi1 > 0 && (dbi1 - 1) < (int)dbs.size())
                dbUpper = dbs[dbi1 - 1];

            fs::path src = srcSecs / dbUpper / (partUpper + ".sec");
            fs::path dst = dstSecs / dbUpper / (partUpper + ".sec");

            info.files.push_back(copy_one_file(src, dst));
        }
    }

    write_manifest(info, note);
    return info;
}

static fs::path normalize_snapshot_dir(const fs::path& input)
{
    if (input.is_absolute())
        return input;

    auto appRoot = MmdbManager::instance().getAppRootPath();
    if (appRoot.empty())
        throw std::runtime_error("[snapshot] app context not set");

    return appRoot / input;
}

void load(const fs::path& snapshotNameOrPath)
{
    // Stop writers (cooperative)
    SnapshotWriteGuard guard;

    auto appRoot = MmdbManager::instance().getAppRootPath();
    if (appRoot.empty())
        throw std::runtime_error("[snapshot] app context not set");

    fs::path snapDir = normalize_snapshot_dir(snapshotNameOrPath);
    if (!fs::exists(snapDir) || !fs::is_directory(snapDir))
        throw std::runtime_error("[snapshot] snapshot dir not found: " + snapDir.string());

    // restore meta/apps
    {
        fs::path srcMeta = snapDir / "meta" / "apps";
        fs::path dstMeta = appRoot / "meta" / "apps";
        fs::create_directories(dstMeta);

        std::string appUpper = appRoot.filename().string();
        copy_one_file(srcMeta / (appUpper + ".sec"), dstMeta / (appUpper + ".sec"));
        copy_one_file(srcMeta / (appUpper + "_NEW.sec"), dstMeta / (appUpper + "_NEW.sec"));
    }

    // restore zrtdb_data: overwrite in place (do NOT rename, to keep existing mmaps on same inode meaningful)
    {
        fs::path srcSecs = snapDir / "zrtdb_data";
        fs::path dstSecs = appRoot / "zrtdb_data";
        if (!fs::exists(srcSecs))
            throw std::runtime_error("[snapshot] snapshot missing zrtdb_data/: " + srcSecs.string());

        for (auto& e : fs::recursive_directory_iterator(srcSecs)) {
            if (!e.is_regular_file())
                continue;
            if (e.path().extension() != ".sec")
                continue;

            fs::path rel = fs::relative(e.path(), srcSecs); // <DB>/<PART>.sec
            fs::path dst = dstSecs / rel;
            fs::create_directories(dst.parent_path());

            // overwrite target content in-place
            // (copy_one_file uses O_TRUNC; that keeps inode but truncates—OK for mmap coherence,
            //  but requires that file is the same inode; O_TRUNC does not change inode.)
            copy_one_file(e.path(), dst);
        }
    }
}

std::vector<fs::path> list()
{
    auto appRoot = MmdbManager::instance().getAppRootPath();
    if (appRoot.empty())
        return {};

    std::string appUpper = appRoot.filename().string();
    std::vector<fs::path> dirs;

    for (auto& e : fs::directory_iterator(appRoot)) {
        if (!e.is_directory())
            continue;
        auto name = e.path().filename().string();
        // prefix match: APP_
        if (name.rfind(appUpper + "_", 0) == 0) {
            dirs.push_back(e.path());
        }
    }

    // sort by name desc (timestamp lex order works with YYYYMMDD-HHMMSS.mmm)
    std::sort(dirs.begin(), dirs.end(), [](const fs::path& a, const fs::path& b) {
        return a.filename().string() > b.filename().string();
    });

    return dirs;
}

} // namespace mmdb::snapshot

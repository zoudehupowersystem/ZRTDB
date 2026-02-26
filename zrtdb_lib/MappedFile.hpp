// ZRTDB（Zero-copy Real-Time Data Bus）
// Copyright (c) 2026 邹德虎 （Zou Dehu）
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <fcntl.h>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace mmdb {

class MappedFile {
public:
    MappedFile(const std::filesystem::path& path, bool readOnly)
        : path_(path)
    {
        int flags = readOnly ? O_RDONLY : O_RDWR;
        fd_ = ::open(path.c_str(), flags, 0666);
        if (fd_ == -1) {
            throw std::runtime_error("Failed to open file: " + path.string());
        }

        struct stat sb;
        if (fstat(fd_, &sb) == -1) {
            ::close(fd_);
            throw std::runtime_error("Failed to stat file: " + path.string());
        }
        size_ = static_cast<size_t>(sb.st_size);

        int prot = readOnly ? PROT_READ : (PROT_READ | PROT_WRITE);
        addr_ = ::mmap(nullptr, size_, prot, MAP_SHARED, fd_, 0);
        if (addr_ == MAP_FAILED) {
            ::close(fd_);
            throw std::runtime_error("mmap failed for: " + path.string());
        }
    }

    ~MappedFile()
    {
        if (addr_ != MAP_FAILED) {
            ::munmap(addr_, size_);
        }
        if (fd_ != -1) {
            ::close(fd_);
        }
    }

    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;

    MappedFile(MappedFile&& other) noexcept
        : path_(std::move(other.path_))
        , fd_(other.fd_)
        , addr_(other.addr_)
        , size_(other.size_)
    {
        other.fd_ = -1;
        other.addr_ = MAP_FAILED;
        other.size_ = 0;
    }

    void* data() const { return addr_; }
    size_t size() const { return size_; }
    int fd() const { return fd_; }

private:
    std::filesystem::path path_;
    int fd_ = -1;
    void* addr_ = MAP_FAILED;
    size_t size_ = 0;
};

} // namespace mmdb

#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace dedup {
using Bytes = std::vector<std::uint8_t>;
namespace fs = std::filesystem;
std::string sha256(const Bytes& bytes);
bool valid_hash(const std::string& hash);
void ensure_directory(const fs::path& directory);
void sync_directory(const fs::path& directory);
Bytes read_file(const fs::path& path, std::uint64_t limit);
class FileLock {
public:
    explicit FileLock(const fs::path& path);
    ~FileLock();
    FileLock(const FileLock&) = delete; FileLock& operator=(const FileLock&) = delete;
private: int fd_ = -1;
};
class AtomicFile {
public:
    explicit AtomicFile(fs::path target);
    ~AtomicFile();
    void append(const Bytes& bytes);
    void publish(bool replace);
    AtomicFile(const AtomicFile&) = delete; AtomicFile& operator=(const AtomicFile&) = delete;
private: fs::path target_, temporary_; int fd_ = -1;
};
void atomic_write(const fs::path& path, const Bytes& bytes, bool replace);
}

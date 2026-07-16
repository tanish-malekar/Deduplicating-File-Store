#include "dedup/io.hpp"
#include <openssl/evp.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <utility>
#include <limits>
#include <stdexcept>
#include <system_error>

namespace dedup {
namespace {
[[noreturn]] void fail(const char* operation) { throw std::system_error(errno, std::generic_category(), operation); }
struct Descriptor { int value; ~Descriptor() { if (value >= 0) ::close(value); } };
}
std::string sha256(const Bytes& bytes) {
    unsigned char digest[EVP_MAX_MD_SIZE]; unsigned int length = 0;
    if (EVP_Digest(bytes.data(), bytes.size(), digest, &length, EVP_sha256(), nullptr) != 1 || length != 32)
        throw std::runtime_error("SHA-256 failed");
    static constexpr char hex[] = "0123456789abcdef"; std::string result; result.reserve(64);
    for (unsigned int i = 0; i < length; ++i) { result += hex[digest[i] >> 4]; result += hex[digest[i] & 15]; }
    return result;
}
bool valid_hash(const std::string& hash) {
    return hash.size() == 64 && hash.find_first_not_of("0123456789abcdef") == std::string::npos;
}
void sync_directory(const fs::path& path) {
    Descriptor fd{::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC)};
    if (fd.value < 0) fail("open directory"); if (::fsync(fd.value) != 0) fail("fsync directory");
}
void ensure_directory(const fs::path& path) {
    if (fs::exists(path)) { if (!fs::is_directory(path)) throw std::runtime_error("Expected directory"); return; }
    if (!fs::create_directory(path)) throw std::runtime_error("Could not create directory");
    sync_directory(path); sync_directory(path.parent_path().empty() ? fs::path(".") : path.parent_path());
}
Bytes read_file(const fs::path& path, std::uint64_t limit) {
    Descriptor fd{::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW)};
    if (fd.value < 0) fail("open file");
    struct stat info{}; if (::fstat(fd.value, &info) != 0) fail("stat file");
    if (!S_ISREG(info.st_mode) || info.st_size < 0 || static_cast<std::uint64_t>(info.st_size) > limit)
        throw std::runtime_error("Invalid file size or type");
    Bytes bytes(static_cast<std::size_t>(info.st_size)); std::size_t at = 0;
    while (at < bytes.size()) {
        ssize_t count = ::read(fd.value, bytes.data() + at, bytes.size() - at);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) throw std::runtime_error("Truncated file"); at += static_cast<std::size_t>(count);
    }
    return bytes;
}
FileLock::FileLock(const fs::path& path) {
    fd_ = ::open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd_ < 0) fail("open lock");
    while (::flock(fd_, LOCK_EX) != 0) {
        if (errno == EINTR) continue; int saved = errno; ::close(fd_); fd_ = -1; errno = saved; fail("flock");
    }
}
FileLock::~FileLock() { if (fd_ >= 0) { ::flock(fd_, LOCK_UN); ::close(fd_); } }
AtomicFile::AtomicFile(fs::path target) : target_(std::move(target)) {
    fs::path parent = target_.parent_path().empty() ? fs::path(".") : target_.parent_path();
    std::string pattern = (parent / ".tmp-XXXXXX").string(); std::vector<char> writable(pattern.begin(), pattern.end()); writable.push_back(0);
    fd_ = ::mkstemp(writable.data()); if (fd_ < 0) fail("mkstemp"); temporary_ = writable.data();
}
AtomicFile::~AtomicFile() { if (fd_ >= 0) ::close(fd_); if (!temporary_.empty()) ::unlink(temporary_.c_str()); }
void AtomicFile::append(const Bytes& bytes) {
    std::size_t at = 0;
    while (at < bytes.size()) {
        ssize_t n = ::write(fd_, bytes.data() + at, bytes.size() - at);
        if (n < 0 && errno == EINTR) continue; if (n <= 0) fail("write"); at += static_cast<std::size_t>(n);
    }
}
void AtomicFile::publish(bool replace) {
    if (::fsync(fd_) != 0) fail("fsync file");
    if (replace) { if (::rename(temporary_.c_str(), target_.c_str()) != 0) fail("rename"); }
    else {
        // Same-directory hard-link publication is atomic and refuses overwrite.
        if (::link(temporary_.c_str(), target_.c_str()) != 0) fail("publish file");
        if (::unlink(temporary_.c_str()) != 0) fail("unlink temporary");
    }
    temporary_.clear(); sync_directory(target_.parent_path().empty() ? fs::path(".") : target_.parent_path());
}
void atomic_write(const fs::path& path, const Bytes& bytes, bool replace) {
    AtomicFile file(path); file.append(bytes); file.publish(replace);
}
}

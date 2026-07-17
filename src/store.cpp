#include "dedup/store.hpp"
#include <algorithm>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace dedup {
namespace {
constexpr std::uint64_t manifest_limit = 256ULL * 1024 * 1024;
void number(Bytes& out, std::uint64_t value, int bytes) {
    for (int i = 0; i < bytes; ++i) out.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
}
std::uint64_t take(const Bytes& in, std::size_t& at, int bytes) {
    if (at > in.size() || in.size() - at < static_cast<std::size_t>(bytes)) throw std::runtime_error("Truncated manifest");
    std::uint64_t value = 0; for (int i = 0; i < bytes; ++i) value |= std::uint64_t(in[at++]) << (8 * i); return value;
}
std::string name_hash(const std::string& name) {
    if (name.empty() || name.size() > 256 || name.find('\0') != std::string::npos) throw std::invalid_argument("Snapshot name must contain 1..256 non-NUL bytes");
    return sha256(Bytes(name.begin(), name.end()));
}
Bytes encode(const Manifest& manifest) {
    Bytes out{'D','F','S','0','0','0','0','1'};
    number(out, manifest.name.size(), 4); out.insert(out.end(), manifest.name.begin(), manifest.name.end());
    number(out, manifest.bytes, 8); number(out, manifest.chunks.size(), 8);
    for (const auto& chunk : manifest.chunks) { out.insert(out.end(), chunk.hash.begin(), chunk.hash.end()); number(out, chunk.bytes, 4); }
    if (out.size() > manifest_limit) throw std::runtime_error("Manifest exceeds 256 MiB"); return out;
}
Manifest decode(const Bytes& in) {
    const std::string magic = "DFS00001";
    if (in.size() < 28 || !std::equal(magic.begin(), magic.end(), in.begin())) throw std::runtime_error("Invalid manifest header");
    std::size_t at = 8; auto size = take(in, at, 4);
    if (!size || size > 256 || size > in.size() - at) throw std::runtime_error("Invalid manifest name");
    Manifest m; m.name.assign(in.begin() + at, in.begin() + at + size); at += size; name_hash(m.name);
    m.bytes = take(in, at, 8); auto count = take(in, at, 8);
    if (count > (in.size() - at) / 68 || count * 68 != in.size() - at) throw std::runtime_error("Invalid chunk reference count");
    std::uint64_t total = 0;
    for (std::uint64_t i = 0; i < count; ++i) {
        std::string hash(in.begin() + at, in.begin() + at + 64); at += 64; auto length = take(in, at, 4);
        if (!valid_hash(hash) || length == 0 || length > 16 * 1024 * 1024 || total > std::numeric_limits<std::uint64_t>::max() - length)
            throw std::runtime_error("Invalid chunk reference");
        total += length; m.chunks.push_back({hash, static_cast<std::uint32_t>(length)});
    }
    if (total != m.bytes) throw std::runtime_error("Manifest length mismatch"); return m;
}
bool temporary(const fs::path& path) { return path.filename().string().rfind(".tmp-", 0) == 0; }
std::vector<fs::path> objects(const fs::path& root) {
    std::vector<fs::path> paths;
    for (const auto& shard : fs::directory_iterator(root / "objects")) {
        const auto prefix = shard.path().filename().string();
        if (prefix.size() != 2 || prefix.find_first_not_of("0123456789abcdef") != std::string::npos ||
            shard.is_symlink() || !shard.is_directory()) throw std::runtime_error("Unexpected object shard");
        for (const auto& object : fs::directory_iterator(shard)) {
            if (object.is_symlink() || !object.is_regular_file()) throw std::runtime_error("Unexpected object entry");
            if (!temporary(object.path()) && !valid_hash(prefix + object.path().filename().string()))
                throw std::runtime_error("Unexpected object filename");
            paths.push_back(object.path());
        }
    }
    return paths;
}
}
Store::Store(fs::path root, Chunking options) : root_(fs::absolute(std::move(root)).lexically_normal()), chunker_(options) {
    ensure_directory(root_); FileLock lock(root_ / "LOCK");
    const std::string format = "DEDUP_STORE_V1\n";
    if (fs::exists(root_ / "FORMAT")) {
        auto existing = read_file(root_ / "FORMAT", 128);
        if (std::string(existing.begin(), existing.end()) != format) throw std::runtime_error("Unknown store format");
    } else {
        for (const auto& entry : fs::directory_iterator(root_))
            if (entry.path().filename() != "LOCK") throw std::runtime_error("Refusing to initialize a nonempty directory");
        atomic_write(root_ / "FORMAT", Bytes(format.begin(), format.end()), false);
    }
    ensure_directory(root_ / "objects"); ensure_directory(root_ / "manifests");
    save_index(references(manifests())); // The index is derived; committed manifests are authoritative.
}
fs::path Store::manifest_path(const std::string& name) const { return root_ / "manifests" / (name_hash(name) + ".mf"); }
fs::path Store::object_path(const std::string& hash) const {
    if (!valid_hash(hash)) throw std::invalid_argument("Invalid content digest");
    return root_ / "objects" / hash.substr(0, 2) / hash.substr(2);
}
std::vector<Manifest> Store::manifests() const {
    std::vector<Manifest> result;
    for (const auto& entry : fs::directory_iterator(root_ / "manifests")) {
        if (temporary(entry.path())) continue;
        if (entry.is_symlink() || !entry.is_regular_file() || entry.path().extension() != ".mf") throw std::runtime_error("Unexpected manifest entry");
        Manifest m = decode(read_file(entry.path(), manifest_limit));
        if (entry.path().stem() != name_hash(m.name)) throw std::runtime_error("Manifest name digest mismatch");
        result.push_back(std::move(m));
    }
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) { return a.name < b.name; }); return result;
}
std::map<std::string, Reference> Store::references(const std::vector<Manifest>& manifests) const {
    std::map<std::string, Reference> refs;
    for (const auto& manifest : manifests) for (const auto& chunk : manifest.chunks) {
        auto& ref = refs[chunk.hash];
        if (ref.count && ref.bytes != chunk.bytes) throw std::runtime_error("Inconsistent chunk length");
        if (ref.count == std::numeric_limits<std::uint64_t>::max()) throw std::runtime_error("Reference count overflow");
        ++ref.count; ref.bytes = chunk.bytes;
    }
    return refs;
}
void Store::save_index(const std::map<std::string, Reference>& refs) const {
    std::ostringstream out; out << "DEDUP_INDEX_V1\n";
    for (const auto& [hash, ref] : refs) out << hash << ' ' << ref.bytes << ' ' << ref.count << '\n';
    auto text = out.str(); atomic_write(root_ / "index", Bytes(text.begin(), text.end()), true);
}
Bytes Store::checked_chunk(const ChunkRef& chunk) const {
    auto data = read_file(object_path(chunk.hash), 16 * 1024 * 1024);
    if (data.size() != chunk.bytes || sha256(data) != chunk.hash) throw std::runtime_error("Chunk integrity verification failed: " + chunk.hash);
    return data;
}
Manifest Store::put(const fs::path& source, const std::string& name) {
    FileLock lock(root_ / "LOCK"); const auto path = manifest_path(name);
    if (fs::exists(path)) throw std::runtime_error("Snapshot already exists");
    std::ifstream input(source, std::ios::binary); if (!input) throw std::runtime_error("Could not open source");
    Manifest manifest; manifest.name = name;
    chunker_.split(input, [&](const Bytes& bytes) {
        auto hash = sha256(bytes); auto object = object_path(hash); ensure_directory(object.parent_path());
        if (fs::exists(object)) {
            if (read_file(object, 16 * 1024 * 1024) != bytes) throw std::runtime_error("Digest collision or corrupt existing object");
        } else atomic_write(object, bytes, false);
        if (manifest.bytes > std::numeric_limits<std::uint64_t>::max() - bytes.size()) throw std::runtime_error("File too large");
        manifest.bytes += bytes.size(); manifest.chunks.push_back({hash, static_cast<std::uint32_t>(bytes.size())});
        if (manifest.chunks.size() > (manifest_limit - 1024) / 68) throw std::runtime_error("Manifest capacity exceeded");
    });
    atomic_write(path, encode(manifest), false); // All chunk contents are durable before publication.
    save_index(references(manifests())); return manifest;
}
void Store::restore(const std::string& name, const fs::path& output) {
    FileLock lock(root_ / "LOCK"); auto destination = fs::weakly_canonical(fs::absolute(output));
    auto root = fs::weakly_canonical(root_); auto a = root.begin(), b = destination.begin();
    while (a != root.end() && b != destination.end() && *a == *b) { ++a; ++b; }
    if (a == root.end()) throw std::runtime_error("Restore destination must be outside the store");
    if (fs::exists(destination)) throw std::runtime_error("Restore destination already exists");
    Manifest manifest = decode(read_file(manifest_path(name), manifest_limit));
    if (manifest.name != name) throw std::runtime_error("Manifest identity mismatch");
    AtomicFile target(destination); for (const auto& chunk : manifest.chunks) target.append(checked_chunk(chunk));
    target.publish(false);
}
void Store::remove(const std::string& name) {
    FileLock lock(root_ / "LOCK");
    if (!fs::remove(manifest_path(name))) throw std::runtime_error("Unknown snapshot");
    sync_directory(root_ / "manifests"); save_index(references(manifests()));
}
std::vector<Manifest> Store::list() { FileLock lock(root_ / "LOCK"); return manifests(); }
Statistics Store::statistics() {
    FileLock lock(root_ / "LOCK"); Statistics stats;
    for (const auto& manifest : manifests()) {
        if (stats.logical_bytes > std::numeric_limits<std::uint64_t>::max() - manifest.bytes) throw std::runtime_error("Statistics overflow");
        stats.logical_bytes += manifest.bytes; ++stats.files;
    }
    for (const auto& path : objects(root_)) if (!temporary(path)) { stats.chunk_bytes += fs::file_size(path); ++stats.chunks; }
    return stats;
}
Collection Store::collect(bool dry_run) {
    FileLock lock(root_ / "LOCK"); auto refs = references(manifests());
    // Finish marking and verify every live object before deleting any candidate.
    for (const auto& [hash, ref] : refs) checked_chunk({hash, static_cast<std::uint32_t>(ref.bytes)});
    std::vector<fs::path> garbage; Collection result;
    for (const auto& path : objects(root_)) {
        auto hash = path.parent_path().filename().string() + path.filename().string();
        if (temporary(path) || !refs.count(hash)) { garbage.push_back(path); ++result.objects; result.bytes += fs::file_size(path); }
    }
    if (!dry_run) {
        for (const auto& path : garbage) { fs::remove(path); sync_directory(path.parent_path()); }
        for (const auto& entry : fs::directory_iterator(root_ / "manifests")) if (temporary(entry.path())) fs::remove(entry.path());
        sync_directory(root_ / "manifests"); save_index(refs);
    }
    return result;
}
void Store::verify() {
    FileLock lock(root_ / "LOCK");
    for (const auto& [hash, ref] : references(manifests())) checked_chunk({hash, static_cast<std::uint32_t>(ref.bytes)});
}
}

#pragma once
#include "dedup/io.hpp"
#include "dedup/chunker.hpp"
#include <map>

namespace dedup {
struct ChunkRef { std::string hash; std::uint32_t bytes; };
struct Manifest { std::string name; std::uint64_t bytes = 0; std::vector<ChunkRef> chunks; };
struct Reference { std::uint64_t count = 0, bytes = 0; };
struct Statistics { std::uint64_t logical_bytes = 0, chunk_bytes = 0, files = 0, chunks = 0; };
struct Collection { std::uint64_t objects = 0, bytes = 0; };
class Store {
public:
    explicit Store(fs::path root, Chunking options = {});
    Manifest put(const fs::path& source, const std::string& name);
    void restore(const std::string& name, const fs::path& output);
    void remove(const std::string& name);
    std::vector<Manifest> list();
    Statistics statistics();
    Collection collect(bool dry_run = false);
    void verify();
private:
    fs::path root_;
    RabinChunker chunker_;
    fs::path manifest_path(const std::string& name) const;
    fs::path object_path(const std::string& hash) const;
    std::vector<Manifest> manifests() const;
    std::map<std::string, Reference> references(const std::vector<Manifest>& manifests) const;
    void save_index(const std::map<std::string, Reference>& index) const;
    Bytes checked_chunk(const ChunkRef& chunk) const;
};
}

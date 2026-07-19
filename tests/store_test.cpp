#include "dedup/store.hpp"
#include <gtest/gtest.h>
#include <fstream>
#include <cstdlib>
#include <random>
#include <sstream>
#include <unistd.h>

namespace {
struct StoreTest : testing::Test {
    dedup::fs::path root;
    void SetUp() override {
        std::string pattern = (dedup::fs::temp_directory_path() / "dedup-test-XXXXXX").string();
        std::vector<char> name(pattern.begin(), pattern.end()); name.push_back(0);
        auto path = ::mkdtemp(name.data()); ASSERT_NE(path, nullptr); root = path;
    }
    void TearDown() override { dedup::fs::remove_all(root); }
    void write(const dedup::fs::path& path, const std::string& bytes) {
        std::ofstream out(path, std::ios::binary); out.write(bytes.data(), bytes.size());
    }
};
TEST_F(StoreTest, DuplicateSnapshotsShareObjectsAndCollectionPreservesLiveData) {
    std::string input(200000, 'x'); write(root / "input", input);
    dedup::Store store(root / "store");
    store.put(root / "input", "first"); auto initial = store.statistics();
    store.put(root / "input", "second"); auto duplicate = store.statistics();
    EXPECT_EQ(initial.chunk_bytes, duplicate.chunk_bytes); EXPECT_EQ(duplicate.logical_bytes, input.size() * 2);
    store.remove("first"); store.collect(); store.restore("second", root / "restored");
    auto data = dedup::read_file(root / "restored", input.size());
    EXPECT_EQ(std::string(data.begin(), data.end()), input);
    store.remove("second"); EXPECT_GT(store.collect().objects, 0); EXPECT_EQ(store.statistics().chunks, 0);
}
TEST_F(StoreTest, EmptyFilesAndDestinationProtection) {
    write(root / "empty", ""); dedup::Store store(root / "store");
    EXPECT_EQ(store.put(root / "empty", "empty").chunks.size(), 0);
    store.restore("empty", root / "output"); EXPECT_EQ(dedup::fs::file_size(root / "output"), 0);
    EXPECT_THROW(store.restore("empty", root / "output"), std::exception);
    EXPECT_THROW(store.restore("empty", root / "store" / "other"), std::exception);
}
TEST_F(StoreTest, CollectorRefusesCorruptLiveChunksBeforeRemovingGarbage) {
    write(root / "input", std::string(8192, 'a')); dedup::Store store(root / "store");
    auto m = store.put(root / "input", "live"); const auto& hash = m.chunks.front().hash;
    auto object = root / "store" / "objects" / hash.substr(0, 2) / hash.substr(2);
    write(object, "corrupt"); EXPECT_THROW(store.collect(), std::exception); EXPECT_THROW(store.verify(), std::exception);
}
TEST_F(StoreTest, RefusesUnrelatedNonemptyDirectory) {
    write(root / "precious", "keep"); EXPECT_THROW(dedup::Store(root), std::exception);
    EXPECT_TRUE(dedup::fs::exists(root / "precious"));
}
TEST(Rabin, BoundsAndReassembly) {
    std::mt19937 random(42); std::string input(200000, 0);
    for (auto& c : input) c = static_cast<char>(random());
    std::istringstream source(input); dedup::RabinChunker chunker({128, 256, 1024});
    std::string result; std::vector<std::size_t> sizes;
    chunker.split(source, [&](const dedup::Bytes& chunk) {
        sizes.push_back(chunk.size()); result.append(chunk.begin(), chunk.end());
    });
    EXPECT_EQ(result, input); ASSERT_GT(sizes.size(), 1);
    for (std::size_t i = 0; i < sizes.size(); ++i) { EXPECT_LE(sizes[i], 1024); if (i + 1 < sizes.size()) EXPECT_GE(sizes[i], 128); }
}
TEST(Hash, StandardVector) {
    EXPECT_EQ(dedup::sha256(dedup::Bytes{'a','b','c'}), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}
}

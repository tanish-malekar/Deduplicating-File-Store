#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <istream>
#include <vector>

namespace dedup {
struct Chunking {
    std::size_t minimum = 2048, average = 8192, maximum = 65536;
};
class RabinChunker {
public:
    explicit RabinChunker(Chunking options = {});
    void split(std::istream& source, const std::function<void(const std::vector<std::uint8_t>&)>& emit) const;
private:
    Chunking options_;
    std::array<std::uint64_t, 256> outgoing_{};
    static std::uint64_t append(std::uint64_t fingerprint, std::uint8_t byte);
};
}

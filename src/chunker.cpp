#include "dedup/chunker.hpp"
#include <stdexcept>

namespace dedup {
namespace { constexpr std::uint64_t polynomial = 0x3DA3358B4DC173ULL; constexpr std::size_t window = 64; }
std::uint64_t RabinChunker::append(std::uint64_t value, std::uint8_t byte) {
    // Polynomial arithmetic over GF(2), reduced modulo a degree-53 polynomial.
    for (int bit = 7; bit >= 0; --bit) {
        value = (value << 1) | ((byte >> bit) & 1U);
        if (value & (1ULL << 53)) value ^= polynomial;
    }
    return value;
}
RabinChunker::RabinChunker(Chunking options) : options_(options) {
    if (!options.minimum || options.minimum > options.average || options.average > options.maximum ||
        (options.average & (options.average - 1)) || options.maximum > 16 * 1024 * 1024)
        throw std::invalid_argument("Require min <= power-of-two average <= max <= 16 MiB");
    for (std::size_t i = 0; i < outgoing_.size(); ++i) {
        std::uint64_t value = append(0, static_cast<std::uint8_t>(i));
        for (std::size_t j = 1; j < window; ++j) value = append(value, 0);
        outgoing_[i] = value;
    }
}
void RabinChunker::split(std::istream& source, const std::function<void(const std::vector<std::uint8_t>&)>& emit) const {
    std::array<std::uint8_t, window> ring{};
    std::size_t position = 0, filled = 0; std::uint64_t fingerprint = 0;
    std::vector<std::uint8_t> chunk; chunk.reserve(options_.maximum);
    std::array<char, 64 * 1024> input{};
    while (source) {
        source.read(input.data(), static_cast<std::streamsize>(input.size()));
        for (std::streamsize i = 0; i < source.gcount(); ++i) {
            auto byte = static_cast<std::uint8_t>(input[static_cast<std::size_t>(i)]);
            if (filled == window) fingerprint ^= outgoing_[ring[position]]; else ++filled;
            ring[position] = byte; position = (position + 1) % window;
            fingerprint = append(fingerprint, byte); chunk.push_back(byte);
            if (chunk.size() >= options_.minimum &&
                ((fingerprint & (options_.average - 1)) == 0 || chunk.size() >= options_.maximum)) {
                emit(chunk); chunk.clear(); // Preserve the rolling window across chunk boundaries.
            }
        }
    }
    if (source.bad() || (!source.eof() && source.fail())) throw std::runtime_error("Input read failed");
    if (!chunk.empty()) emit(chunk);
}
}

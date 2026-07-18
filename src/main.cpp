#include "dedup/store.hpp"
#include <iostream>
#include <iomanip>
#include <stdexcept>

int main(int argc, char** argv) {
    try {
        if (argc < 3) throw std::invalid_argument("usage: dedup-store STORE put SOURCE NAME | restore NAME OUTPUT | remove NAME | list | stats | verify | gc [--dry-run]");
        dedup::Store store(argv[1]); std::string command = argv[2];
        if (command == "put" && argc == 5) {
            auto result = store.put(argv[3], argv[4]); std::cout << result.name << " bytes=" << result.bytes << " chunk_references=" << result.chunks.size() << '\n';
        } else if (command == "restore" && argc == 5) store.restore(argv[3], argv[4]);
        else if (command == "remove" && argc == 4) store.remove(argv[3]);
        else if (command == "list" && argc == 3) for (const auto& m : store.list()) std::cout << m.name << '\t' << m.bytes << '\t' << m.chunks.size() << '\n';
        else if (command == "verify" && argc == 3) { store.verify(); std::cout << "All referenced chunks verified\n"; }
        else if (command == "stats" && argc == 3) {
            auto stats = store.statistics(); std::cout << "snapshots=" << stats.files << " logical_bytes=" << stats.logical_bytes
                << " chunk_bytes=" << stats.chunk_bytes << " objects=" << stats.chunks;
            if (stats.chunk_bytes) std::cout << " logical_to_chunk_ratio=" << std::fixed << std::setprecision(3) << double(stats.logical_bytes) / stats.chunk_bytes;
            std::cout << '\n';
        } else if (command == "gc" && (argc == 3 || (argc == 4 && std::string(argv[3]) == "--dry-run"))) {
            auto result = store.collect(argc == 4); std::cout << "objects=" << result.objects << " bytes=" << result.bytes << '\n';
        } else throw std::invalid_argument("Invalid command or argument count");
    } catch (const std::exception& ex) { std::cerr << "error: " << ex.what() << '\n'; return 1; }
}

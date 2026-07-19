# Deduplicating File Store

A C++17 content-addressed backup store for Linux. Files are split into variable-
size chunks using a rolling Rabin fingerprint, addressed by SHA-256, and recorded
in immutable named manifests. Duplicate chunks share one stored object, while a
reference-counted index and mark-and-sweep collector manage retained data.

## Data path

```text
source stream -> Rabin chunker -> SHA-256 -> immutable chunk objects
                                      -> named manifest -> derived reference index
restore       <- verified chunk objects <- manifest
GC            <- mark all committed manifests, verify live objects, sweep orphans
```

The rolling fingerprint uses a 64-byte window and polynomial reduction over
GF(2) with a degree-53 polynomial. Default boundaries use a 2 KiB minimum, 8 KiB
average mask, and 64 KiB maximum. The rolling window continues across chunk
boundaries, helping boundaries resynchronize after insertions. SHA-256 uses
OpenSSL's EVP API; the fingerprint chooses boundaries and is not the content ID.

Objects are sharded by the first two digest characters. Manifests contain the
logical name, original byte length, ordered chunk hashes, and chunk lengths.
Snapshot names are hashed into filenames, rather than interpreted as paths.
The reference index counts every retained chunk occurrence, including repeats
within one file. Committed manifests are authoritative, and the index is rebuilt
from them when opening the store and after mutations.

## Durability and collection

Each object is written to a same-directory temporary file, fsynced, and published
without overwrite. Its directory is fsynced before the manifest can reference it.
Manifest publication follows the same sequence; the reference index is replaced
atomically afterward. A crash before manifest publication leaves collectible
orphan objects. A crash after publication can leave a stale index, repaired from
manifests on the next open.

Every operation holds an exclusive Linux `flock`, including restores and garbage
collection. This serializes store operations across processes and prevents a
collector from racing a writer or reader. The store directory and its parent
must be on a trusted local filesystem honoring fsync and atomic hard links.
The format marker prevents initialization over an unrelated nonempty directory.

The collector marks references from every committed manifest, validates all live
chunk contents and lengths, and only then removes unmarked objects. A malformed
manifest or a missing/corrupt live object aborts collection before any deletion.
Interrupted object staging files are eligible for collection. Removal of a
snapshot first durably removes its manifest; chunks remain until collection.

Restore verifies SHA-256 and length for every chunk and publishes the completed
output atomically. Existing destinations are refused, and restore paths must be
outside the store. The output parent directory must already exist.

## Build and commands

Requires Linux, CMake 3.20+, a C++17 compiler, OpenSSL 3 development headers, and
pthread support. GoogleTest is fetched by CMake when tests are enabled.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

./build/dedup-store store-data put backup.tar backup-001
./build/dedup-store store-data put next-backup.tar backup-002
./build/dedup-store store-data list
./build/dedup-store store-data stats
./build/dedup-store store-data verify
./build/dedup-store store-data restore backup-001 restored.tar
./build/dedup-store store-data remove backup-001
./build/dedup-store store-data gc --dry-run
./build/dedup-store store-data gc
```

Snapshot names are immutable and must contain 1–256 non-NUL bytes. Input files
must remain stable while being ingested; the store captures the byte stream it
reads rather than a filesystem-level snapshot. Empty files are supported. There
is no compression, encryption, remote object service, directory traversal backup,
or multi-writer parallel ingestion inside one store.

```sh
ctest --test-dir build --output-on-failure
cmake -S . -B build-core -DDEDUP_BUILD_TESTS=OFF
```

GoogleTest specifications exercise content round trips, empty files, duplicated
snapshots, deletion and collection, corruption protection, SHA-256 vectors, and
chunk-size bounds.

## Statistics and scaling

`stats` reports retained logical bytes, physical chunk payload bytes, snapshot
count, and object count. `logical_to_chunk_ratio` divides retained logical bytes
by all stored chunk payload bytes, including orphans until collection. It excludes
manifests, the index, filesystem allocation overhead, and compression effects.
Measure filesystem usage separately when evaluating total disk consumption.

Record input corpus size and similarity, chunk settings, snapshot count, unique
chunk count, physical disk usage, and restore verification alongside space ratios.
The ingestion buffer is bounded by maximum chunk size, while manifests and the
reference map grow with chunk count. A manifest is limited to 256 MiB; large backup
corpora can be represented as multiple named files. This implementation rebuilds
the full reference map and performs a full mark pass for collection.

## Layout

- `chunker.cpp` — rolling Rabin boundaries over a streaming input
- `io.cpp` — SHA-256, process locks, bounded reads, durable atomic publication
- `store.cpp` — manifests, chunk references, restore, statistics, collection
- `main.cpp` — command-line interface
- `tests/store_test.cpp` — GoogleTest storage specifications

MIT licensed.

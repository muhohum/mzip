# mzip

[![CI](https://github.com/muhohum/mzip/actions/workflows/ci.yml/badge.svg)](https://github.com/muhohum/mzip/actions/workflows/ci.yml)

A block-sorting lossless compressor in C++20. Multi-block inputs first pass through a
stream-wide LZP stage that collapses long repeats between distant blocks. Each block then
goes through an optional LZP pass and a Burrows-Wheeler transform (linear-time SA-IS suffix
array), and whichever of two coders yields fewer bytes: a context-mixing arithmetic coder
over the BWT output, or move-to-front with run coding under an adaptive range coder. Blocks
are compressed and decompressed in parallel, incompressible blocks are stored raw, the
container format is documented, and the decoder validates everything it reads.

See:
[ALGORITHM.md](ALGORITHM.md) for how it works and [BENCHMARKS.md](BENCHMARKS.md) for
measurements.

On the Canterbury corpora (13 MB, 9 files) mzip compresses 13.9% smaller than bzip2 -9 and
10.5% smaller than xz -9; on the Silesia corpus (212 MB) and on enwik9 (1 GB of Wikipedia)
with `--profile ratio` it comes out ahead of bzip3 and bsc, the strongest block-sorting
compressors in the comparison:

| Codec    | Canterbury | Silesia | enwik9 |
|----------|-----------:|--------:|-------:|
| mzip     |     0.1902 |  0.2178 | 0.1637 |
| bzip3    |          - |  0.2191 | 0.1700 |
| bsc      |          - |  0.2222 | 0.1706 |
| xz -9e   |     0.2127 |  0.2286 | 0.2118 |
| zstd -22 |          - |  0.2478 | 0.2140 |
| bzip2 -9 |     0.2211 |  0.2572 |      - |
| gzip -9  |     0.2749 |       - |      - |

Per-file tables, enwik8, and a versioned-data benchmark are in
[BENCHMARKS.md](BENCHMARKS.md).

## Build

Requires CMake 3.20+ and a C++20 compiler: GCC 11+, Clang 14+, MSVC from Visual Studio 2019
16.11, or AppleClang 14+. There are no third-party dependencies — only the standard library
and the system thread library. CI builds every configuration with MSVC, GCC, Clang, and
AppleClang.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

`cmake --install build --prefix <dir>` installs the `mzip` binary, the static library, the
header, and a CMake package config. Tagged releases also ship prebuilt archives for Windows,
Linux, and macOS on the Releases page, built by CI from the tag.

## Usage

```sh
mzip compress input.txt output.mz
mzip compress my-folder folder.mz
mzip compress big.bin big.mz --profile ratio
mzip compress big.bin big.mz --block-size 16777216 --threads 8
mzip decompress output.mz restored.txt
mzip decompress folder.mz restored-folder --threads 4
```

Directories are archived as a tar stream generated on the fly, so a folder of any size
compresses without a temporary file; extraction validates every path and restores the tree
atomically. Inputs up to 512 MiB are buffered whole for the deduplication stage; larger
inputs stream through fixed-size blocks with bounded memory.

By default the block size is picked from the input: 4 MiB for small inputs, one sixteenth of
the input for larger ones, capped at 16 MiB so big files always split into enough blocks to
keep every core busy. `--profile ratio` puts the whole input in one block (up to 1 GiB)
instead — the best compression at the cost of parallelism and memory — and `--block-size`
(1 KiB to 1 GiB) sets anything else; a block costs roughly 15x its size in memory while it
is being encoded. Compression and decompression both run blocks on all cores by default
(`--threads` overrides); the output is byte-identical for any thread count. It is written to
a temporary file and renamed only after the whole operation succeeds.

## Using as a library

The API lives in `include/mzip/mzip.hpp`: `compress_file`, `decompress_file`,
`CompressionOptions`, `DecompressionOptions`, `CompressionStats`, and `FormatError`. The target is `mzip::mzip`, and
only the library builds when mzip is not the top-level project.

Through FetchContent (or a plain `add_subdirectory`):

```cmake
include(FetchContent)
FetchContent_Declare(mzip GIT_REPOSITORY https://github.com/muhohum/mzip.git GIT_TAG v2.0.0)
FetchContent_MakeAvailable(mzip)
target_link_libraries(app PRIVATE mzip::mzip)
```

Or against an installed copy:

```cmake
find_package(mzip 2.0 REQUIRED CONFIG)
target_link_libraries(app PRIVATE mzip::mzip)
```

```cpp
#include <mzip/mzip.hpp>

const mzip::CompressionStats stats = mzip::compress_file("data.bin", "data.mz");
mzip::decompress_file("data.mz", "restored.bin");
```

CI verifies the install-and-`find_package` flow on every push.

## Tests

`ctest` runs round-trip tests for every stage (including randomized and known-answer BWT
cases), whole-file tests over text/binary/random/multi-block inputs, determinism checks
across thread counts, and a corruption suite that bit-flips and truncates archives to verify
the decoder always fails cleanly. CI builds Debug and Release on Windows and Linux, plus an
AddressSanitizer/UBSan run.

## License

MIT, see [LICENSE](LICENSE).

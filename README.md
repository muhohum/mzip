# mzip

[![CI](https://github.com/muhohum/mzip/actions/workflows/ci.yml/badge.svg)](https://github.com/muhohum/mzip/actions/workflows/ci.yml)

A block-sorting lossless compressor in C++20. Multi-block inputs first pass through a
stream-wide LZP stage that collapses long repeats between distant blocks, and blocks are cut
where their content changes (code to data tables, one embedded file to the next). Each block
then goes through an optional LZP pass, an x86 branch-target filter when it looks like machine
code, a record filter when it holds fixed-length records or rows of 16-bit samples, and a
Burrows-Wheeler transform (linear-time SA-IS suffix array), and whichever of two
coders yields fewer bytes: a context-mixing arithmetic coder over the BWT output (a repeat
decision, then seven models per bit blended by gated logistic mixing with two secondary
estimation stages), or
move-to-front with run coding under an adaptive range coder. Blocks are compressed and
decompressed in parallel, incompressible blocks are stored raw, the container format is
documented and versioned, and the decoder validates everything it reads.

See:
[ALGORITHM.md](ALGORITHM.md) for how it works and [BENCHMARKS.md](BENCHMARKS.md) for
measurements.

On the Silesia corpus (212 MB) with `--profile ratio` mzip compresses 5.9% smaller than
bzip3 and 6.1% smaller than bsc, the strongest block-sorting compressors in the comparison,
and 9.8% smaller than xz -9e; the default profile, which codes 16 MiB blocks in parallel,
lands within 0.6% of that. All figures were measured on one 4-core machine:

| Codec                | Silesia | enwik8 | Compress | Decompress |
|----------------------|--------:|-------:|---------:|-----------:|
| mzip --profile ratio |  0.2062 | 0.2061 | 1.3 MB/s |   2.8 MB/s |
| mzip (defaults)      |  0.2074 | 0.2249 | 2.0 MB/s |   4.4 MB/s |
| bzip3 -b 511         |  0.2191 | 0.2075 | 7.2 MB/s |   6.5 MB/s |
| BCM -9               |  0.2194 | 0.2079 | 7.0 MB/s |   4.1 MB/s |
| bsc -b1000 -e2       |  0.2196 | 0.2080 |  17 MB/s |    34 MB/s |
| xz -9e               |  0.2286 | 0.2483 | 1.0 MB/s |    57 MB/s |
| zstd -22 --long      |  0.2471 |      - | 1.0 MB/s |   351 MB/s |
| bzip2 -9             |  0.2572 |      - |  10 MB/s |    20 MB/s |

Speeds are on Silesia. kanzi's context-mixing level 9 goes further on most data (0.1924 on
Silesia) at about the same compression speed, half mzip's decompression speed and four times
the memory. Tables of structured data, executables, and source gain the most from this
release, through the record and x86 filters and blocks cut where their content changes.
Per-file tables, memory, and the other data sets are in [BENCHMARKS.md](BENCHMARKS.md).

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

By default blocks are 16 MiB: inputs up to that size are coded as one block, since splitting
them costs several percent, and bigger files split into enough blocks to keep every core
busy. Wherever a block's statistics change part-way, as between the code and data sections
of an executable, the encoder cuts it there and codes the pieces as blocks of their own.
`--profile ratio` puts the whole input in one block (up to 1 GiB, still cut where its
content changes) and takes no shortcut in choosing its coding — the best compression at the
cost of speed, parallelism and memory — and `--block-size` (1 KiB to 1 GiB) sets anything
else; a block costs roughly 15x its size in memory while it is being encoded, plus about
26 MiB for the context-mixing model. Compression and
decompression both run blocks on all cores by default (`--threads` overrides); the output is
byte-identical for any thread count. It is written to a temporary file and renamed only after
the whole operation succeeds.

## Using as a library

The API lives in `include/mzip/mzip.hpp`: `compress_file`, `decompress_file`,
`CompressionOptions`, `DecompressionOptions`, `CompressionStats`, and `FormatError`. The target is `mzip::mzip`, and
only the library builds when mzip is not the top-level project.

Through FetchContent (or a plain `add_subdirectory`):

```cmake
include(FetchContent)
FetchContent_Declare(mzip GIT_REPOSITORY https://github.com/muhohum/mzip.git GIT_TAG v3.0.0)
FetchContent_MakeAvailable(mzip)
target_link_libraries(app PRIVATE mzip::mzip)
```

Or against an installed copy:

```cmake
find_package(mzip 3.0 REQUIRED CONFIG)
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

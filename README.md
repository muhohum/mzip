# mzip

A block-sorting lossless compressor in C++20. Each block goes through a Burrows-Wheeler
transform (linear-time SA-IS suffix array), move-to-front, and run coding, then an adaptive
binary range coder with contexts derived from the decoded state; blocks are compressed in
parallel and incompressible blocks are stored raw. The container format is documented and
the decoder validates everything it reads.

This is a compact personal project, not a replacement for zstd or bzip2. See
[ALGORITHM.md](ALGORITHM.md) for how it works and [BENCHMARKS.md](BENCHMARKS.md) for
measurements.

On the Canterbury corpora (13 MB, 9 files) mzip compresses 7.3% smaller than bzip2 -9 and
3.7% smaller than xz -9:

| Codec    |  Ratio |  Compress | Decompress |
|----------|-------:|----------:|-----------:|
| mzip     | 0.2048 | 12.5 MB/s |  17.8 MB/s |
| gzip -9  | 0.2749 |  4.1 MB/s | 521.0 MB/s |
| bzip2 -9 | 0.2211 | 24.6 MB/s |  63.9 MB/s |
| xz -9    | 0.2127 |  3.1 MB/s | 150.1 MB/s |

## Build

Requires CMake 3.20+ and a C++20 compiler (CI covers MSVC, GCC, and AppleClang).

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
mzip compress big.bin big.mz --block-size 16777216 --threads 8
mzip decompress output.mz restored.txt
```

By default the block size is picked from the input: 4 MiB for small inputs, one sixteenth of
the input for larger ones, capped at 16 MiB so big files always split into enough blocks to
keep every core busy. `--block-size` (1 KiB to 64 MiB) overrides the choice; blocks above
16 MiB compress better but give up thread-level parallelism and use more memory. `--threads 0`
(default) uses all cores; the archive is byte-identical for any thread count. Output is
written to a temporary file and renamed only after the whole operation succeeds.

## Using as a library

The API lives in `include/mzip/mzip.hpp`: `compress_file`, `decompress_file`,
`CompressionOptions`, `CompressionStats`, and `FormatError`. The target is `mzip::mzip`, and
only the library builds when mzip is not the top-level project.

Through FetchContent (or a plain `add_subdirectory`):

```cmake
include(FetchContent)
FetchContent_Declare(mzip GIT_REPOSITORY https://github.com/muhohum/mzip.git GIT_TAG v1.0.0)
FetchContent_MakeAvailable(mzip)
target_link_libraries(app PRIVATE mzip::mzip)
```

Or against an installed copy:

```cmake
find_package(mzip 1.0 REQUIRED CONFIG)
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

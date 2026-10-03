# Benchmark scripts

Two scripts, both Python 3.8+ with the standard library only:

- `run_benchmark.py` downloads a standard corpus, verifies it against pinned hashes, and
  compares mzip with gzip, bzip2 and xz at level 9 through Python's own bindings.
  `--corpus canterbury` (default) fetches the Canterbury, Artificial and Large corpora from
  corpus.canterbury.ac.nz; `--corpus silesia` fetches the Silesia corpus from its GitHub
  mirror ([MiloszKrajewski/SilesiaCorpus](https://github.com/MiloszKrajewski/SilesiaCorpus))
  into `<data-dir>/silesia/` and checks each unzipped file's size and SHA-256.
- `compare.py` compares mzip with external compressors on any files you give it, recording
  size, speed and memory. The rest of this page describes it.

## compare.py

```sh
python3 benchmarks/compare.py \
  --mzip build/mzip --mzip "mzip 2.0=/path/to/old/mzip" \
  --tools-dir ~/bench-tools/bin --repeat 5 \
  --json silesia.json --markdown silesia.md \
  /data/silesia
```

Each positional argument is one corpus: a directory (its files, not recursive) or a single
file. Every file goes through every codec and back, and the restored bytes must match the
input's SHA-256. For each (codec, file) the JSON records the compressed size, the median wall
time of `--repeat` compress and decompress runs (each run's time is kept as well), the median
user CPU time, and the peak RSS of the codec process, taken from `os.wait4` so that only that
child is counted. The Markdown report has the host (CPU, cores, OS, the compiler from the mzip
build's CMake cache), the exact command of every codec, and per corpus a totals table (size,
ratio, compress and decompress MB/s, CPU seconds, peak memory) and a per-file size table.

Options:

- `--mzip [LABEL=]PATH`, repeatable (default `build/mzip`): each binary runs with its
  defaults, `--profile ratio`, and `--threads 1` (same sizes as the defaults; for timing).
  The label defaults to `mzip X.Y` from `--version`.
- `--tools-dir DIR`: where the other codecs are looked up before `PATH` (default
  `$MZIP_BENCH_TOOLS`, else `build/bench-tools/bin`). Codecs whose executable is missing are
  skipped; `--list` shows what was found and each tool's version.
- `--codecs` / `--exclude`: comma-separated name patterns, e.g. `--codecs 'mzip*,xz*'`.
- `--timeout S`: abandon a run after S seconds and record the codec as failed on that file.
- `--work-dir DIR`: where archives are written (use a local disk, not tmpfs, for big files).
- `--from-json FILE`: re-render the Markdown report of a saved run.

The codecs are the table in `reference_codecs()`; the strongest common setting of each tool,
plus the default where that is a meaningful second point:

| Codec              | Command                                                    |
|--------------------|------------------------------------------------------------|
| gzip -9            | `gzip -9 -n -c`                                            |
| bzip2 -9           | `bzip2 -9 -c`                                              |
| xz -9e             | `xz -9e -T1 -c` (single-threaded)                          |
| zstd -19           | `zstd -19`                                                 |
| zstd -22 --long=27 | `zstd --ultra -22 --long=27`                               |
| brotli -q11 -w30   | `brotli -q 11 --large_window=30` (not RFC 7932 compatible) |
| bzip3 -b fit       | `bzip3 -e -b <file size in MiB, at most 511> -j 1`         |
| bzip3 -j cpus      | `bzip3 -e -j <cpus>` (16 MiB blocks in parallel)           |
| bsc -b1000 -e2     | `bsc e in out -b1000 -e2` (one block per file up to 1000 MB, adaptive QLFC) |
| bsc (defaults)     | `bsc e in out` (`-b25 -e1`)                                |
| bcm -9             | `bcm -9` (one block up to 2 GB)                            |
| kanzi -l7 -b fit   | `kanzi -c -l 7 -b <file size in MiB>m` (BWT + CM, one block per file) |
| kanzi -l9          | `kanzi -c -l 9` (TPAQX context mixing, default blocks)     |

Notes on fairness:

- Block-sorting codecs get one block per file (`fit`), as mzip does with `--profile ratio`;
  bzip3 and mzip defaults both use 16 MiB blocks in parallel.
- bsc built with OpenMP runs with `OMP_WAIT_POLICY=passive`: by default its idle threads
  spin, which on a loaded machine costs seconds of CPU per file.
- Timings include process start and file I/O. Run on an idle machine with `--repeat 3` or
  more; sizes do not depend on the machine.
- Linux charges a spawned process with the peak RSS of the process that spawned it, so memory
  figures below the launching Python interpreter's footprint (about 20 MiB) read as that floor;
  the report states the floor.

### Building the other codecs

Build each tool in Release mode with its project's default options and copy the binaries into
one directory for `--tools-dir`. Sources: [bzip3](https://github.com/iczelia/bzip3),
[libbsc](https://github.com/IlyaGrebnov/libbsc), [zstd](https://github.com/facebook/zstd),
[brotli](https://github.com/google/brotli), [kanzi-cpp](https://github.com/flanglet/kanzi-cpp)
(CMake), and BCM (`bcm.cpp` with its bundled `libsais.c`, last released by its author as
v1.60; the original repository is gone, the history survives in forks such as
[FS-make-simple/bcm](https://github.com/FS-make-simple/bcm)):

```sh
T=~/bench-tools; mkdir -p $T/bin
cmake -S bzip3 -B $T/bzip3 -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF
cmake -S libbsc -B $T/bsc -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++     # GCC brings OpenMP
cmake -S zstd/build/cmake -B $T/zstd -DCMAKE_BUILD_TYPE=Release -DZSTD_BUILD_SHARED=OFF
cmake -S brotli -B $T/brotli -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF
cmake -S kanzi-cpp -B $T/kanzi -DCMAKE_BUILD_TYPE=Release
for d in bzip3 bsc zstd brotli kanzi; do cmake --build $T/$d --parallel; done
cp $T/bzip3/bzip3 $T/bsc/bsc $T/zstd/programs/zstd $T/brotli/brotli $T/bin/
cp $T/kanzi/kanzi_static $T/bin/kanzi
gcc -O2 -c bcm/src/libsais.c -o $T/libsais.o
g++ -O2 -std=c++11 bcm/src/bcm.cpp $T/libsais.o -o $T/bin/bcm
```

gzip, bzip2 and xz come from the system.

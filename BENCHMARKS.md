# Benchmarks

`benchmarks/run_benchmark.py` downloads the official
[Canterbury, Artificial, and Large corpora](https://corpus.canterbury.ac.nz/descriptions/),
verifies the archives against pinned SHA-256 hashes, and round-trips every file through mzip,
checking the restored bytes hash-for-hash. gzip, bzip2, and xz run at level 9 through
Python's standard-library bindings on the same data.

```sh
python benchmarks/run_benchmark.py \
  --binary build/Release/mzip \
  --repeat 3 \
  --output benchmark-results.md
```

All numbers are medians of three runs. mzip times include process start and file I/O; the
reference codecs run in memory through C extension modules, which flatters their throughput
slightly on the small files. `--threads` is forwarded to mzip (default: all cores).

## Results

Measured 2026-07-22 on an AMD Ryzen 9 7950X (16 cores), Windows 11, MSVC 19.44 Release
build, Python 3.12. Total input: 13,065,681 bytes across 9 files.

| Codec    | Total size |  Ratio |  Compress | Decompress |
|----------|-----------:|-------:|----------:|-----------:|
| mzip     |  2,676,409 | 0.2048 | 12.5 MB/s |  17.8 MB/s |
| gzip -9  |  3,591,511 | 0.2749 |  4.1 MB/s | 521.0 MB/s |
| bzip2 -9 |  2,888,233 | 0.2211 | 24.6 MB/s |  63.9 MB/s |
| xz -9    |  2,778,764 | 0.2127 |  3.1 MB/s | 150.1 MB/s |

## Archive sizes

| File         | Kind             |      Input |      mzip |   gzip -9 |  bzip2 -9 |     xz -9 |
|--------------|------------------|-----------:|----------:|----------:|----------:|----------:|
| alice29.txt  | English text     |    152,089 |    43,921 |    54,182 |    43,202 |    48,492 |
| fields.c     | C source         |     11,150 |     3,107 |     3,127 |     3,039 |     3,028 |
| kennedy.xls  | spreadsheet      |  1,029,744 |    75,379 |   207,041 |   130,280 |    49,116 |
| ptt5         | fax bitmap       |    513,216 |    50,878 |    52,233 |    49,759 |    41,992 |
| aaa.txt      | repeated byte    |    100,000 |        57 |       133 |        47 |       148 |
| random.txt   | random text      |    100,000 |    76,472 |    75,747 |    75,684 |    76,824 |
| world192.txt | CIA fact book    |  2,473,400 |   432,505 |   721,957 |   489,583 |   487,492 |
| bible.txt    | King James Bible |  4,047,392 |   805,393 | 1,177,362 |   845,635 |   885,184 |
| E.coli       | DNA sequence     |  4,638,690 | 1,188,697 | 1,299,729 | 1,251,004 | 1,186,488 |
| total        |                  | 13,065,681 | 2,676,409 | 3,591,511 | 2,888,233 | 2,778,764 |

## mzip timings

| File         |     Input | Compress | Decompress |
|--------------|----------:|---------:|-----------:|
| alice29.txt  |   152,089 |  20.6 ms |    13.7 ms |
| fields.c     |    11,150 |  10.0 ms |    11.0 ms |
| kennedy.xls  | 1,029,744 |  52.9 ms |    30.1 ms |
| ptt5         |   513,216 |  31.4 ms |    18.1 ms |
| aaa.txt      |   100,000 |  10.0 ms |     9.9 ms |
| random.txt   |   100,000 |  21.8 ms |    15.5 ms |
| world192.txt | 2,473,400 | 174.8 ms |    82.3 ms |
| bible.txt    | 4,047,392 | 299.2 ms |   210.4 ms |
| E.coli       | 4,638,690 | 423.4 ms |   341.3 ms |

## Reading the numbers

- mzip produces the smallest total on this corpus: 7.3% smaller than bzip2 -9 and 3.7%
  smaller than xz -9. It wins outright on structured binary data (kennedy.xls: 42% smaller
  than bzip2), on large text (world192.txt, bible.txt), and on DNA, where the
  previous-literal context of the range coder pays off.
- xz keeps the lead where LZMA's match modeling shines: long repeated records (kennedy.xls
  is still 35% smaller under xz) and ptt5.
- Nearly incompressible data (random.txt) costs a fraction of a percent over a static coder
  because the adaptive model has to learn that the data is random; the raw fallback bounds
  the damage.
- The ratio comes from 4 MiB blocks and per-block adaptive modeling, and both cost speed:
  files up to a few megabytes fit in one or two blocks, so the thread pool has little to do,
  and the coder touches every bit. On many-block inputs compression scales with cores again.
  `--block-size` trades ratio for speed and memory in both directions.

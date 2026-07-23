# Benchmarks

`benchmarks/run_benchmark.py` downloads the official
[Canterbury, Artificial, and Large corpora](https://corpus.canterbury.ac.nz/descriptions/),
verifies the archives against pinned SHA-256 hashes, and round-trips every file through mzip,
checking the restored bytes hash-for-hash. gzip, bzip2, and xz run at level 9 through
Python's standard-library bindings on the same data.

```sh
python benchmarks/run_benchmark.py \
  --binary build/Release/mzip \
  --repeat 5 \
  --output benchmark-results.md
```

## Canterbury results

Measured 2026-07-23 on an AMD Ryzen 9 7950X (16 cores), Windows 11, MSVC 19.44 Release
build, medians of five runs on an idle machine. Total input: 13,065,681 bytes across 9
files; mzip runs with default settings.

| Codec    | Total size |  Ratio |  Compress | Decompress |
|----------|-----------:|-------:|----------:|-----------:|
| mzip     |  2,484,577 | 0.1902 |  4.4 MB/s |  11.7 MB/s |
| xz -9    |  2,778,764 | 0.2127 |  3.4 MB/s | 154.7 MB/s |
| bzip2 -9 |  2,888,233 | 0.2211 | 24.8 MB/s |  64.7 MB/s |
| gzip -9  |  3,591,511 | 0.2749 |  4.2 MB/s | 522.5 MB/s |

| File         | Kind             |      Input |      mzip |   gzip -9 |  bzip2 -9 |     xz -9 |
|--------------|------------------|-----------:|----------:|----------:|----------:|----------:|
| alice29.txt  | English text     |    152,089 |    40,510 |    54,182 |    43,202 |    48,492 |
| fields.c     | C source         |     11,150 |     3,107 |     3,127 |     3,039 |     3,028 |
| kennedy.xls  | spreadsheet      |  1,029,744 |    75,379 |   207,041 |   130,280 |    49,116 |
| ptt5         | fax bitmap       |    513,216 |    45,177 |    52,233 |    49,759 |    41,992 |
| aaa.txt      | repeated byte    |    100,000 |        57 |       133 |        47 |       148 |
| random.txt   | random text      |    100,000 |    75,712 |    75,747 |    75,684 |    76,824 |
| world192.txt | CIA fact book    |  2,473,400 |   398,719 |   721,957 |   489,583 |   487,492 |
| bible.txt    | King James Bible |  4,047,392 |   727,898 | 1,177,362 |   845,635 |   885,184 |
| E.coli       | DNA sequence     |  4,638,690 | 1,118,018 | 1,299,729 | 1,251,004 | 1,186,488 |
| total        |                  | 13,065,681 | 2,484,577 | 3,591,511 | 2,888,233 | 2,778,764 |

| File         |     Input |  Compress | Decompress |
|--------------|----------:|----------:|-----------:|
| alice29.txt  |   152,089 |   33.6 ms |    16.6 ms |
| fields.c     |    11,150 |    8.2 ms |     5.3 ms |
| kennedy.xls  | 1,029,744 |   85.9 ms |    23.6 ms |
| ptt5         |   513,216 |   54.1 ms |    38.0 ms |
| aaa.txt      |   100,000 |   10.1 ms |     5.6 ms |
| random.txt   |   100,000 |   23.7 ms |    14.4 ms |
| world192.txt | 2,473,400 |  424.3 ms |   189.8 ms |
| bible.txt    | 4,047,392 |  767.4 ms |   354.8 ms |
| E.coli       | 4,638,690 | 1530.4 ms |   472.8 ms |

## Silesia results

The [Silesia corpus](https://sun.aei.polsl.pl/~sdeor/index.php?page=silesia) is the standard
mixed benchmark for modern compressors: 211,938,580 bytes across 12 files of text, databases,
executables, and images. mzip runs with `--profile ratio`; the references are the strongest
settings of each tool (bzip2 1.0.8 `-9`, xz `-9e`, zstd 1.5.5 `--ultra -22 --long=27`,
bzip3 1.4.0 `-b 64`, bsc 3.3 `-b64`), all on the same machine. Every Silesia file is under
52 MB, so `-b 64` already gives bzip3 and bsc a single block per file — the same result
their maximum block settings produce.

| Codec     | Total size |  Ratio |
|-----------|-----------:|-------:|
| mzip      | 46,158,710 | 0.2178 |
| bzip3     | 46,426,615 | 0.2191 |
| bsc       | 47,088,148 | 0.2222 |
| xz -9e    | 48,456,004 | 0.2286 |
| zstd -22  | 52,522,343 | 0.2478 |
| bzip2 -9  | 54,506,769 | 0.2572 |

| File    | Kind            |       Input |       mzip |      bzip3 |        bsc |     xz -9e |
|---------|-----------------|------------:|-----------:|-----------:|-----------:|-----------:|
| dickens | English text    |  10,192,446 |  2,234,610 |  2,233,954 |  2,274,312 |  2,831,212 |
| mozilla | executables tar |  51,220,480 | 15,814,911 | 15,832,992 | 16,107,622 | 13,376,240 |
| mr      | MRI image       |   9,970,564 |  2,117,695 |  2,119,990 |  2,231,072 |  2,751,892 |
| nci     | chemical db     |  33,553,445 |  1,196,019 |  1,366,363 |  1,225,348 |  1,449,272 |
| ooffice | executable      |   6,152,192 |  2,529,406 |  2,526,882 |  2,587,452 |  2,427,224 |
| osdb    | database        |  10,085,684 |  2,249,329 |  2,261,872 |  2,276,416 |  2,844,556 |
| reymont | Polish text     |   6,627,202 |    982,269 |    980,475 |    993,980 |  1,315,592 |
| samba   | source tar      |  21,606,400 |  3,905,962 |  3,918,956 |  3,968,420 |  3,739,524 |
| sao     | star catalog    |   7,251,944 |  4,673,329 |  4,673,313 |  4,724,796 |  4,425,664 |
| webster | dictionary      |  41,458,703 |  6,410,594 |  6,448,851 |  6,492,662 |  8,368,672 |
| x-ray   | X-ray image     |   8,474,240 |  3,657,105 |  3,657,090 |  3,806,844 |  4,491,264 |
| xml     | XML             |   5,345,280 |    387,481 |    405,877 |    399,224 |    434,892 |
| total   |                 | 211,938,580 | 46,158,710 | 46,426,615 | 47,088,148 | 48,456,004 |

## Versioned data

Cross-block deduplication shows on versioned data. The test set is the source tree of 19
consecutive zstd releases (v1.0.0 to v1.5.7), each `git archive` tar concatenated into one
126,873,600-byte stream, so identical files recur up to 118 MB apart:

| Codec                | Total size |  Ratio |
|----------------------|-----------:|-------:|
| xz -9e               |  3,388,044 | 0.0267 |
| zstd -22 --long=30   |  3,466,573 | 0.0273 |
| mzip --profile ratio |  3,514,113 | 0.0277 |
| mzip (defaults)      |  3,694,471 | 0.0291 |
| bzip3 -b 511         |  3,875,045 | 0.0305 |
| bsc -b1000           |  4,777,094 | 0.0377 |

## enwik8 and enwik9

The [Large Text Compression Benchmark](https://mattmahoney.net/dc/text.html) datasets: the
first 100 MB and 1 GB of English Wikipedia XML. `--profile ratio` covers enwik9 with a
single 953 MiB block; bzip3 and bsc run `-b 100` / `-b 511`, and 511 MB is the largest
block bzip3 supports:

| Codec                |     enwik8 |  Ratio |      enwik9 |  Ratio |
|----------------------|-----------:|-------:|------------:|-------:|
| mzip --profile ratio | 20,752,546 | 0.2075 | 163,730,107 | 0.1637 |
| bzip3                | 20,749,632 | 0.2075 | 169,990,721 | 0.1700 |
| bsc                  | 20,944,930 | 0.2094 | 170,615,942 | 0.1706 |
| mzip (defaults)      | 23,891,573 | 0.2389 | 198,171,513 | 0.1982 |
| xz -9e               | 24,831,648 | 0.2483 | 211,776,220 | 0.2118 |
| zstd -22 --long      | 25,333,695 | 0.2533 | 213,968,104 | 0.2140 |

## Timings

Single runs on an idle machine (same hardware, Linux/GCC 13 builds of every tool). mzip
uses all cores; reference tools run as configured above. The native Windows build is timed
in the Canterbury section.

| Set                 | Codec           |          Compress |        Decompress |
|---------------------|-----------------|------------------:|------------------:|
| Silesia (212 MB)    | mzip (defaults) |  18 s (11.8 MB/s) |   7 s (30.3 MB/s) |
|                     | bzip3 -b 64     |  16 s (13.2 MB/s) |  19 s (11.2 MB/s) |
|                     | bsc -b64        |   7 s (30.3 MB/s) |   6 s (35.3 MB/s) |
|                     | zstd -22 --long |  79 s (2.7 MB/s)  |    <1 s           |
|                     | xz -9e          |  78 s (2.7 MB/s)  |     1 s           |
| enwik9 (1 GB)       | mzip (defaults) |  57 s (17.5 MB/s) |  20 s (50.0 MB/s) |
|                     | mzip --profile ratio | 469 s (2.1 MB/s) | 269 s (3.7 MB/s) |
|                     | bzip3 -b 511    |  82 s (12.2 MB/s) | 117 s (8.5 MB/s)  |
|                     | bsc -b511       |  50 s (20.0 MB/s) |  27 s (37.0 MB/s) |
| versioned (127 MB)  | mzip (defaults) |   3 s (42.3 MB/s) |   2 s (63.4 MB/s) |
|                     | mzip --profile ratio | 21 s (6.0 MB/s) |  3 s (42.3 MB/s) |

## Reading the numbers

- mzip produces the smallest Canterbury total: 13.9% below bzip2 -9 and 10.5% below xz -9.
  Text, DNA, and the fax bitmap go to the context mixer; kennedy.xls and fields.c go to the
  move-to-front path.
- On Silesia mzip posts the smallest total of the six codecs. The largest wins are
  repetitive data (nci 12% below bzip3, xml, webster, samba); dickens, reymont, sao, and
  x-ray land within a tenth of a percent of bzip3 either way.
- xz keeps the lead where its LZMA machinery shines: x86 executables (mozilla, ooffice —
  mzip applies no branch-target filter) and long fixed-stride records (kennedy.xls, sao).
- Nearly incompressible data costs almost nothing: the mixer models random text at a few
  hundredths of a percent over its entropy, and the raw fallback bounds pathological cases.
- Ratio still trades against speed through `--block-size`: the default keeps every core busy
  on multi-block files, `--profile ratio` maximizes context instead. The context mixer works
  bit by bit, so it is slower than the v1-style run coder in both directions; parallel
  blocks absorb most of that on multi-core machines.

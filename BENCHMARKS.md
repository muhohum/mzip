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

Compressed sizes do not depend on the machine, so the tables below mix measurements from two
hosts; every timing says where it was taken.

## Silesia results

The [Silesia corpus](https://sun.aei.polsl.pl/~sdeor/index.php?page=silesia) is the standard
mixed benchmark for modern compressors: 211,938,580 bytes across 12 files of text, databases,
executables, and images. mzip 3.0 and 2.0 were measured on a 4-core Intel Xeon 2.8 GHz Linux
host with a GCC 13 Release build; the references are the strongest settings of each tool
(bzip2 1.0.8 `-9`, xz `-9e`, zstd 1.5.5 `--ultra -22 --long=27`, bzip3 1.4.0 `-b 64`, bsc 3.3
`-b64`), measured earlier on the 2.0 benchmark machine. Every Silesia file is under 52 MB, so
`-b 64` already gives bzip3 and bsc a single block per file.

| Codec                    | Total size |  Ratio |
|--------------------------|-----------:|-------:|
| mzip 3.0 --profile ratio | 45,132,866 | 0.2130 |
| mzip 2.0 --profile ratio | 46,158,710 | 0.2178 |
| mzip 3.0 (defaults)      | 45,614,890 | 0.2152 |
| bzip3 -b 64              | 46,426,615 | 0.2191 |
| bsc -b64                 | 47,088,148 | 0.2222 |
| mzip 2.0 (defaults)      | 47,572,059 | 0.2245 |
| xz -9e                   | 48,456,004 | 0.2286 |
| zstd -22 --long          | 52,522,343 | 0.2478 |
| bzip2 -9                 | 54,506,769 | 0.2572 |

Per file with `--profile ratio` (one block per file), 3.0 against 2.0 and the references:

| File    | Kind                    |       Input |   mzip 3.0 |   mzip 2.0 |      bzip3 |        bsc |     xz -9e |
|---------|-------------------------|------------:|-----------:|-----------:|-----------:|-----------:|-----------:|
| dickens | English text            |  10,192,446 |  2,224,551 |  2,234,610 |  2,233,954 |  2,274,312 |  2,831,212 |
| mozilla | executables tar (Alpha) |  51,220,480 | 15,546,671 | 15,814,911 | 15,832,992 | 16,107,622 | 13,376,240 |
| mr      | MRI image               |   9,970,564 |  2,103,883 |  2,117,695 |  2,119,990 |  2,231,072 |  2,751,892 |
| nci     | chemical db             |  33,553,445 |  1,162,296 |  1,196,019 |  1,366,363 |  1,225,348 |  1,449,272 |
| ooffice | x86 executable          |   6,152,192 |  2,106,768 |  2,529,406 |  2,526,882 |  2,587,452 |  2,427,224 |
| osdb    | database                |  10,085,684 |  2,189,306 |  2,249,329 |  2,261,872 |  2,276,416 |  2,844,556 |
| reymont | Polish text             |   6,627,202 |    968,855 |    982,269 |    980,475 |    993,980 |  1,315,592 |
| samba   | source tar              |  21,606,400 |  3,791,650 |  3,905,962 |  3,918,956 |  3,968,420 |  3,739,524 |
| sao     | star catalog            |   7,251,944 |  4,652,659 |  4,673,329 |  4,673,313 |  4,724,796 |  4,425,664 |
| webster | dictionary              |  41,458,703 |  6,368,143 |  6,410,594 |  6,448,851 |  6,492,662 |  8,368,672 |
| x-ray   | X-ray image             |   8,474,240 |  3,639,625 |  3,657,105 |  3,657,090 |  3,806,844 |  4,491,264 |
| xml     | XML                     |   5,345,280 |    378,459 |    387,481 |    405,877 |    399,224 |    434,892 |
| total   |                         | 211,938,580 | 45,132,866 | 46,158,710 | 46,426,615 | 47,088,148 | 48,456,004 |

With default settings the input is coded in 16 MiB blocks for parallel coding (2.0 split
it into blocks of 4 to 16 MiB), which costs about 1% against the single-block profile on
this corpus; 3.0 against 2.0:

| File    |       Input |   mzip 3.0 |   mzip 2.0 |  Change |
|---------|------------:|-----------:|-----------:|--------:|
| dickens |  10,192,446 |  2,224,551 |  2,326,545 |  -4.38% |
| mozilla |  51,220,480 | 15,627,244 | 15,921,821 |  -1.85% |
| mr      |   9,970,564 |  2,103,883 |  2,141,080 |  -1.74% |
| nci     |  33,553,445 |  1,220,195 |  1,372,269 | -11.08% |
| ooffice |   6,152,192 |  2,106,768 |  2,527,403 | -16.64% |
| osdb    |  10,085,684 |  2,189,306 |  2,345,160 |  -6.65% |
| reymont |   6,627,202 |    969,661 |  1,023,904 |  -5.30% |
| samba   |  21,606,400 |  3,814,436 |  3,994,317 |  -4.50% |
| sao     |   7,251,944 |  4,652,659 |  4,667,934 |  -0.33% |
| webster |  41,458,703 |  6,688,103 |  7,189,834 |  -6.98% |
| x-ray   |   8,474,240 |  3,639,625 |  3,674,295 |  -0.94% |
| xml     |   5,345,280 |    378,459 |    387,497 |  -2.33% |
| total   | 211,938,580 | 45,614,890 | 47,572,059 |  -4.11% |

## Executables, source and structured data

A local set of 109,484,801 bytes drawn from a Linux installation: x86-64 programs and shared
libraries, Python and C sources, JSON, HTML, a font, and a tar of time-zone binaries. Default
settings, 3.0 against 2.0:

| File                  | Kind                  |      Input |  mzip 3.0 |  mzip 2.0 |  Change |
|-----------------------|-----------------------|-----------:|----------:|----------:|--------:|
| cheaders.txt          | C headers             | 12,000,000 | 1,360,893 | 1,420,232 |  -4.18% |
| dna.txt               | synthetic DNA         |  3,000,000 |   502,054 |   505,461 |  -0.67% |
| font.ttc              | TrueType collection   | 16,791,251 | 7,681,504 | 7,632,068 |  +0.65% |
| libgtk-3.so.0.2409.32 | x86-64 shared library |  8,156,632 | 2,139,523 | 2,380,550 | -10.12% |
| libpython3.11.so.1.0  | x86-64 shared library |  7,766,224 | 2,211,550 | 2,306,633 |  -4.12% |
| libpython3.13.so.1.0  | x86-64 shared library |  7,380,048 | 2,197,010 | 2,304,357 |  -4.66% |
| pysrc.txt             | Python source         | 11,372,568 | 1,702,133 | 1,794,108 |  -5.13% |
| python3.11.elf        | x86-64 executable     |  6,639,992 | 2,112,367 | 2,229,927 |  -5.27% |
| records.json          | JSON records          | 10,103,582 |   497,251 |   546,136 |  -8.95% |
| rust-copyright.html   | HTML                  | 15,287,199 |    82,764 |    85,135 |  -2.78% |
| vimdoc.txt            | plain text            |  9,932,585 | 1,763,406 | 1,848,958 |  -4.63% |
| zoneinfo.tar          | tar of binaries       |  1,054,720 |   119,680 |   126,649 |  -5.50% |
| total                 |                       | 109,484,801 | 22,370,135 | 23,180,214 |  -3.49% |

The x86 branch-target filter accounts for most of the gain on the programs and libraries;
the remaining files show the new context mixer alone.

## Timings

Single runs on the 4-core Intel Xeon 2.8 GHz Linux host (GCC 13 Release builds of both
versions, all cores, files one after another, decompression on the same core count):

| Set                      | Codec    |           Compress |         Decompress |
|--------------------------|----------|-------------------:|-------------------:|
| Silesia, defaults        | mzip 3.0 |   131 s (1.6 MB/s) |    71 s (3.0 MB/s) |
| Silesia, defaults        | mzip 2.0 |    46 s (4.6 MB/s) |    19 s (11.1 MB/s) |
| Silesia, --profile ratio | mzip 3.0 |   217 s (1.0 MB/s) |   109 s (1.9 MB/s) |
| Silesia, --profile ratio | mzip 2.0 |   111 s (1.9 MB/s) |    52 s (4.1 MB/s) |
| local set, defaults      | mzip 3.0 |   110 s (1.0 MB/s) |    47 s (2.3 MB/s) |
| local set, defaults      | mzip 2.0 |    29 s (3.8 MB/s) |    10 s (11.5 MB/s) |

The new mixer evaluates six models and two networks per bit, so it codes a block about twice
as slowly as the 2.0 mixer in both directions, which the `--profile ratio` rows show
directly. The default rows add the block policy: 3.0 codes 16 MiB blocks, so files up to
16 MiB run on one core where 2.0 split them four ways, and machine-code blocks encode twice
(filtered and not). On more cores the gap narrows for inputs with several blocks;
`--block-size 4194304` restores the 2.0 parallelism at about 2% of ratio.

## Versioned data (version 2.0)

Cross-block deduplication shows on versioned data. The test set is the source tree of 19
consecutive zstd releases (v1.0.0 to v1.5.7), each `git archive` tar concatenated into one
126,873,600-byte stream, so identical files recur up to 118 MB apart:

| Codec                    | Total size |  Ratio |
|--------------------------|-----------:|-------:|
| xz -9e                   |  3,388,044 | 0.0267 |
| zstd -22 --long=30       |  3,466,573 | 0.0273 |
| mzip 2.0 --profile ratio |  3,514,113 | 0.0277 |
| mzip 2.0 (defaults)      |  3,694,471 | 0.0291 |
| bzip3 -b 511             |  3,875,045 | 0.0305 |
| bsc -b1000               |  4,777,094 | 0.0377 |

## enwik8 and enwik9 (version 2.0)

The [Large Text Compression Benchmark](https://mattmahoney.net/dc/text.html) datasets: the
first 100 MB and 1 GB of English Wikipedia XML. `--profile ratio` covers enwik9 with a
single 953 MiB block; bzip3 and bsc run `-b 100` / `-b 511`, and 511 MB is the largest
block bzip3 supports:

| Codec                    |     enwik8 |  Ratio |      enwik9 |  Ratio |
|--------------------------|-----------:|-------:|------------:|-------:|
| mzip 2.0 --profile ratio | 20,752,546 | 0.2075 | 163,730,107 | 0.1637 |
| bzip3                    | 20,749,632 | 0.2075 | 169,990,721 | 0.1700 |
| bsc                      | 20,944,930 | 0.2094 | 170,615,942 | 0.1706 |
| mzip 2.0 (defaults)      | 23,891,573 | 0.2389 | 198,171,513 | 0.1982 |
| xz -9e                   | 24,831,648 | 0.2483 | 211,776,220 | 0.2118 |
| zstd -22 --long          | 25,333,695 | 0.2533 | 213,968,104 | 0.2140 |

## Canterbury results (version 2.0)

These figures were measured with mzip 2.0 (2026-07-23, AMD Ryzen 9 7950X, 16 cores, Windows 11,
MSVC 19.44 Release build, medians of five runs on an idle machine) and have not been
re-measured for 3.0. Total input: 13,065,681 bytes across 9
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

## Reading the numbers

- With `--profile ratio` mzip 3.0 posts the smallest Silesia total of the codecs compared,
  2.8% below bzip3 and 6.9% below xz -9e; the defaults stay 1.7% below bzip3 while coding
  16 MiB blocks in parallel.
- The largest wins over 2.0 are x86 code (ooffice -17%, the local programs and libraries
  -4% to -10%), structured data (records.json -9%, zoneinfo -5.5%, nci -2.8%, xml -2.3%),
  and source (pysrc -5%, samba -2.9%, C headers -4.2%). Plain text gains 0.5% to 4.6%
  depending on how much the 16 MiB blocks help it, and already-dense data (sao, x-ray, DNA)
  under 1%.
- Block size cuts both ways: text and source gain 2% to 4% from 16 MiB blocks, while
  structured binaries (the shared libraries, records.json, font.ttc, which ends 0.7% worse
  than 2.0) compress 2% to 3% better in 4 MiB blocks, with either version's coder. The
  default follows the larger set; `--block-size` picks the other trade.
- xz still leads on mozilla: Silesia's mozilla is a Tru64 (Alpha) build with almost no x86
  code, so the filter does not apply, and LZMA's long-range matching wins on its data
  sections.
- Nearly incompressible data costs almost nothing: the mixer models random text at a few
  hundredths of a percent over its entropy, and the raw fallback bounds pathological cases.
- Ratio still trades against speed through `--block-size`: the default keeps every core busy
  on multi-block files, `--profile ratio` maximizes context and tries every coding candidate
  instead. The context mixer evaluates six models per bit, so it is slower than the 2.0
  mixer in both directions; parallel blocks absorb most of that on multi-core machines.

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

`--corpus silesia` runs the same comparison on the Silesia corpus, fetched from its GitHub
mirror and checked file by file against pinned SHA-256 hashes. `benchmarks/compare.py`
compares mzip with external compressors (zstd, brotli, bzip3, bsc, BCM, kanzi and the system
gzip, bzip2 and xz) on any set of files, recording sizes, timings, CPU time and peak memory;
[benchmarks/README.md](benchmarks/README.md) describes it, the exact command of every codec,
and how to build the other tools.

## Setup

Every table below was produced by `compare.py` on one host: a 4-core Intel Xeon at 2.8 GHz,
Linux 6.18, mzip built with GCC 13.3 in Release mode. The other tools were built from source
at these versions: bzip3 1.5.4, bsc 3.3.12, BCM 1.60, kanzi 2.6.0, zstd 1.6.0-dev,
brotli 1.2.0; xz 5.4.5, bzip2 1.0.8 and gzip 1.12 are the system packages. Each codec runs at
its strongest common setting, plus its default where that is a useful second point:
`xz -9e` single-threaded, `zstd --ultra -22 --long=27`, `brotli -q 11 --large_window=30`,
bzip3 with one block per file (`-b` = file size, at most 511 MiB) and with its default 16 MiB
blocks on 4 threads, `bsc -b1000 -e2` (one block per file, adaptive coder) and bsc's
defaults, `bcm -9`, kanzi level 7 (its BWT pipeline, one block per file) and level 9 (its
context-mixing pipeline). mzip 2.0 is the previous release.

Timings are single runs on an otherwise idle machine, files one after another; MB/s is the
corpus size over the summed wall-clock time, process start and file I/O included, with every
tool free to use all four cores. Memory is the largest peak resident size of any one run.
Every round trip was verified against the SHA-256 of the input. Sizes do not depend on the
machine; the Canterbury, snappy and local-set sizes of the other tools come from the same
script run earlier on the same host.

## Silesia

The [Silesia corpus](https://sun.aei.polsl.pl/~sdeor/index.php?page=silesia) is the standard
mixed benchmark for modern compressors: 211,938,580 bytes across 12 files of text, databases,
executables, and images.

| Codec | Total size | Ratio | Compress | Decompress | Peak memory |
|---|---:|---:|---:|---:|---:|
| kanzi -l9 | 40,785,680 | 0.1924 | 1.30 MB/s | 1.32 MB/s | 1766 MiB |
| mzip 3.0 --profile ratio | 43,699,822 | 0.2062 | 1.29 MB/s | 2.81 MB/s | 412 MiB |
| mzip 3.0 | 43,962,198 | 0.2074 | 1.98 MB/s | 4.44 MB/s | 496 MiB |
| mzip 3.0 --threads 1 | 43,962,198 | 0.2074 | 1.19 MB/s | 2.83 MB/s | 280 MiB |
| mzip 2.0 --profile ratio | 46,158,710 | 0.2178 | 1.84 MB/s | 3.83 MB/s | 789 MiB |
| bzip3 -b fit | 46,426,530 | 0.2191 | 7.24 MB/s | 6.48 MB/s | 297 MiB |
| bcm -9 | 46,506,716 | 0.2194 | 7.01 MB/s | 4.11 MB/s | 246 MiB |
| bsc -b1000 -e2 | 46,536,916 | 0.2196 | 16.84 MB/s | 33.67 MB/s | 246 MiB |
| kanzi -l7 -b fit | 46,560,467 | 0.2197 | 8.70 MB/s | 13.00 MB/s | 276 MiB |
| bzip3 -j cpus | 46,929,960 | 0.2214 | 10.50 MB/s | 9.48 MB/s | 363 MiB |
| bsc (defaults) | 47,428,196 | 0.2238 | 19.74 MB/s | 47.19 MB/s | 246 MiB |
| mzip 2.0 | 47,572,059 | 0.2245 | 4.07 MB/s | 10.23 MB/s | 423 MiB |
| mzip 2.0 --threads 1 | 47,572,059 | 0.2245 | 1.83 MB/s | 4.81 MB/s | 209 MiB |
| xz -9e | 48,456,004 | 0.2286 | 1.01 MB/s | 56.82 MB/s | 506 MiB |
| brotli -q11 -w30 | 49,383,136 | 0.2330 | 0.38 MB/s | 165.82 MB/s | 519 MiB |
| zstd -22 --long=27 | 52,364,240 | 0.2471 | 1.00 MB/s | 350.80 MB/s | 740 MiB |
| zstd -19 | 52,895,350 | 0.2496 | 1.37 MB/s | 394.32 MB/s | 142 MiB |
| bzip2 -9 | 54,506,769 | 0.2572 | 10.16 MB/s | 19.74 MB/s | 27 MiB |
| gzip -9 | 67,631,918 | 0.3191 | 10.17 MB/s | 144.59 MB/s | 27 MiB |

Per file (smallest in bold):

| File | Kind | Input | mzip 3.0 --profile ratio | mzip 3.0 | mzip 2.0 --profile ratio | bzip3 -b fit | bsc -b1000 -e2 | bcm -9 | xz -9e | kanzi -l9 |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| dickens | English text | 10,192,446 | 2,230,038 | 2,230,038 | 2,234,610 | 2,233,934 | 2,259,134 | 2,243,761 | 2,831,212 | **2,140,182** |
| mozilla | executables tar | 51,220,480 | 15,492,954 | 15,386,882 | 15,814,911 | 15,832,992 | 15,907,194 | 15,977,036 | 13,376,240 | **12,315,592** |
| mr | MRI image | 9,970,564 | **1,886,554** | **1,886,554** | 2,117,695 | 2,119,990 | 2,206,012 | 2,117,667 | 2,751,892 | 2,148,534 |
| nci | chemical db | 33,553,445 | **1,159,120** | 1,222,611 | 1,196,019 | 1,366,363 | 1,210,336 | 1,227,055 | 1,449,272 | 1,396,030 |
| ooffice | x86 DLL | 6,152,192 | 2,058,457 | 2,058,457 | 2,529,406 | 2,526,882 | 2,559,108 | 2,543,053 | 2,427,224 | **1,726,413** |
| osdb | database | 10,085,684 | **2,178,391** | **2,178,391** | 2,249,329 | 2,261,854 | 2,224,780 | 2,249,301 | 2,844,556 | 2,240,942 |
| reymont | Polish text | 6,627,202 | **969,437** | 969,925 | 982,269 | 980,459 | 982,364 | 983,191 | 1,315,592 | 971,633 |
| samba | source tar | 21,606,400 | 3,714,004 | 3,692,731 | 3,905,962 | 3,918,956 | 3,909,928 | 4,019,306 | 3,739,524 | **3,189,022** |
| sao | star catalog | 7,251,944 | **3,648,419** | **3,648,419** | 4,673,329 | 4,673,307 | 4,679,764 | 4,673,301 | 4,425,664 | 4,405,890 |
| webster | dictionary | 41,458,703 | 6,373,398 | 6,699,140 | 6,410,594 | 6,448,833 | 6,448,532 | 6,421,326 | 8,368,672 | **6,180,031** |
| x-ray | X-ray image | 8,474,240 | **3,641,083** | **3,641,083** | 3,657,105 | 3,657,083 | 3,756,584 | 3,657,078 | 4,491,264 | 3,728,578 |
| xml | XML | 5,345,280 | 347,967 | 347,967 | 387,481 | 405,877 | 393,180 | 394,641 | 434,892 | **342,833** |
| total | | 211,938,580 | 43,699,822 | 43,962,198 | 46,158,710 | 46,426,530 | 46,536,916 | 46,506,716 | 48,456,004 | 40,785,680 |

## enwik8

The first 100,000,000 bytes of English Wikipedia XML, from the
[Large Text Compression Benchmark](https://mattmahoney.net/dc/text.html). BWT codecs run with
one block over the whole file where they can; brotli, zstd -22 and kanzi -l9 were left out of
this run for time.

| Codec | Total size | Ratio | Compress | Decompress | Peak memory |
|---|---:|---:|---:|---:|---:|
| mzip 3.0 --profile ratio | 20,610,829 | 0.2061 | 1.08 MB/s | 2.38 MB/s | 853 MiB |
| bzip3 -b fit | 20,749,611 | 0.2075 | 6.00 MB/s | 4.48 MB/s | 585 MiB |
| mzip 2.0 --profile ratio | 20,752,546 | 0.2075 | 1.20 MB/s | 2.62 MB/s | 1625 MiB |
| bcm -9 | 20,789,667 | 0.2079 | 6.21 MB/s | 2.84 MB/s | 479 MiB |
| bsc -b1000 -e2 | 20,803,016 | 0.2080 | 14.63 MB/s | 29.58 MB/s | 493 MiB |
| kanzi -l7 -b fit | 20,941,172 | 0.2094 | 7.23 MB/s | 13.89 MB/s | 495 MiB |
| mzip 3.0 | 22,490,634 | 0.2249 | 1.98 MB/s | 6.53 MB/s | 745 MiB |
| mzip 3.0 --threads 1 | 22,490,634 | 0.2249 | 0.74 MB/s | 2.56 MB/s | 387 MiB |
| mzip 2.0 | 23,891,573 | 0.2389 | 3.16 MB/s | 10.87 MB/s | 671 MiB |
| xz -9e | 24,831,648 | 0.2483 | 0.55 MB/s | 52.88 MB/s | 674 MiB |
| zstd -19 | 26,944,227 | 0.2694 | 0.82 MB/s | 248.34 MB/s | 187 MiB |

## Canterbury and small files

The [Canterbury, Large and Artificial corpora](https://corpus.canterbury.ac.nz/descriptions/)
(taken from a GitHub mirror whose files match a second independent mirror byte for byte) and
the eleven test files that ship with Google's snappy library: small text, HTML, source,
protocol buffers, a JPEG and a PDF. Total sizes and ratios:

| Codec | Canterbury | Large | Artificial | Snappy test data |
|---|---:|---:|---:|---:|
| mzip 3.0 --profile ratio | **398,069 (0.1416)** | 2,232,971 (0.2001) | 75,580 (0.2519) | 733,854 (0.2506) |
| mzip 3.0 | 398,152 (0.1417) | 2,232,971 (0.2001) | 75,580 (0.2519) | 733,937 (0.2506) |
| mzip 2.0 | 457,988 (0.1629) | 2,244,635 (0.2011) | 75,899 (0.2530) | 744,373 (0.2542) |
| kanzi -l9 | 412,514 (0.1468) | **2,162,555 (0.1938)** | 77,880 (0.2596) | **704,138 (0.2405)** |
| kanzi -l7 -b fit | 447,175 (0.1591) | 2,249,091 (0.2015) | 75,944 (0.2531) | 735,619 (0.2512) |
| bzip3 -b fit | 459,337 (0.1634) | 2,255,330 (0.2021) | 75,882 (0.2529) | 736,363 (0.2515) |
| bsc -b1000 -e2 | 457,620 (0.1628) | 2,258,742 (0.2024) | 75,935 (0.2531) | 740,352 (0.2528) |
| bcm -9 | 459,539 (0.1635) | 2,252,142 (0.2018) | 75,962 (0.2532) | 753,599 (0.2573) |
| brotli -q11 -w30 | 490,675 (0.1746) | 2,502,114 (0.2242) | **75,080 (0.2503)** | 778,735 (0.2659) |
| xz -9e | 493,080 (0.1754) | 2,556,712 (0.2291) | 77,208 (0.2574) | 793,776 (0.2711) |
| zstd -22 --long=27 | 516,291 (0.1837) | 2,547,398 (0.2283) | 75,207 (0.2507) | 812,220 (0.2774) |
| bzip2 -9 | 542,710 (0.1931) | 2,586,222 (0.2318) | 75,899 (0.2530) | 787,200 (0.2688) |
| gzip -9 | 730,625 (0.2599) | 3,197,094 (0.2865) | 76,134 (0.2538) | 985,229 (0.3364) |

Canterbury per file:

| File | Input | mzip 3.0 --profile ratio | mzip 2.0 | kanzi -l9 | bzip3 -b fit | bsc -b1000 -e2 | xz -9e |
|---|---:|---:|---:|---:|---:|---:|---:|
| alice29.txt | 152,089 | 39,983 | 40,510 | **38,012** | 40,496 | 40,240 | 48,528 |
| asyoulik.txt | 125,179 | 37,010 | 37,445 | **35,496** | 37,404 | 37,236 | 44,592 |
| cp.html | 24,603 | 7,192 | 7,474 | **6,948** | 7,323 | 7,328 | 7,652 |
| fields.c | 11,150 | 2,852 | 3,107 | **2,580** | 3,132 | 2,958 | 3,032 |
| grammar.lsp | 3,721 | 1,185 | 1,322 | **1,120** | 1,285 | 1,270 | 1,292 |
| kennedy.xls | 1,029,744 | **20,072** | 75,379 | 53,780 | 76,906 | 74,020 | 51,868 |
| lcet10.txt | 426,754 | 98,335 | 99,477 | **92,253** | 99,392 | 99,566 | 119,488 |
| plrabn12.txt | 481,861 | 134,084 | 134,643 | **130,053** | 134,614 | 134,882 | 165,456 |
| ptt5 | 513,216 | 44,534 | 45,177 | 40,888 | 45,450 | 46,642 | **39,860** |
| sum | 38,240 | 11,155 | 11,671 | 9,752 | 11,574 | 11,748 | **9,500** |
| xargs.1 | 4,227 | 1,667 | 1,783 | **1,632** | 1,761 | 1,730 | 1,812 |
| total | 2,810,784 | 398,069 | 457,988 | 412,514 | 459,337 | 457,620 | 493,080 |

## Executables, source and structured data

A local set of 109,484,801 bytes drawn from a Linux installation: x86-64 programs and shared
libraries, Python and C sources, JSON, HTML, a font collection, a tar of time-zone binaries,
and synthetic DNA. Default settings for mzip, the settings above for the others:

| File | Input | mzip 3.0 | mzip 2.0 | kanzi -l9 | bzip3 -b fit | bsc -b1000 -e2 | xz -9e | brotli -q11 -w30 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| cheaders.txt | 12,000,000 | 1,350,870 | 1,420,232 | **1,050,301** | 1,390,859 | 1,374,010 | 1,405,076 | 1,404,732 |
| dna.txt | 3,000,000 | **502,331** | 505,461 | 505,217 | 506,311 | 514,974 | 568,612 | 599,677 |
| font.ttc | 16,791,251 | 7,004,229 | 7,632,068 | **6,843,490** | 7,775,404 | 7,927,936 | 7,461,204 | 7,657,080 |
| libgtk-3.so.0.2409.32 | 8,156,632 | 1,933,270 | 2,380,550 | **1,705,741** | 2,428,539 | 2,445,758 | 2,256,468 | 2,298,797 |
| libpython3.11.so.1.0 | 7,766,224 | 1,910,530 | 2,306,633 | **1,574,469** | 2,364,609 | 2,412,014 | 1,992,804 | 2,063,430 |
| libpython3.13.so.1.0 | 7,380,048 | 1,957,496 | 2,304,357 | **1,662,589** | 2,358,177 | 2,398,796 | 2,121,900 | 2,163,352 |
| pysrc.txt | 11,372,568 | 1,696,222 | 1,794,108 | **1,441,937** | 1,708,177 | 1,714,924 | 1,868,456 | 1,865,190 |
| python3.11.elf | 6,639,992 | 1,874,287 | 2,229,927 | **1,541,609** | 2,270,880 | 2,313,424 | 1,960,940 | 2,016,719 |
| records.json | 10,103,582 | 486,127 | 546,136 | **374,769** | 594,420 | 518,272 | 673,424 | 614,493 |
| rust-copyright.html | 15,287,199 | 81,571 | 85,135 | **75,133** | 87,689 | 94,652 | 93,116 | 87,336 |
| vimdoc.txt | 9,932,585 | 1,758,892 | 1,848,958 | **1,541,045** | 1,776,601 | 1,780,530 | 2,114,284 | 2,117,929 |
| zoneinfo.tar | 1,054,720 | 116,593 | 126,649 | **97,037** | 121,231 | 125,698 | 108,040 | 111,285 |
| total | 109,484,801 | 20,672,418 | 23,180,214 | 18,413,337 | 23,382,897 | 23,620,988 | 22,624,324 | 23,000,020 |

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

## Reading the numbers

- With `--profile ratio` mzip 3.0 posts the smallest Silesia total of every block-sorting and
  dictionary codec compared: 5.9% below bzip3, 6.1% below bsc, 9.8% below xz -9e and 5.3%
  below mzip 2.0. Only kanzi -l9, a context-mixing compressor in the PAQ family, goes further,
  at about the same compression speed and two to four times the memory; mzip still beats it
  on half the Silesia files and on the Canterbury corpus.
- The largest wins come from the per-block filters. The record filter takes sao from
  4.67 MB to 3.65 MB (-22%), mr by 11%, and kennedy.xls from 75 KB to 20 KB; the x86 filter
  together with blocks cut at section boundaries takes 19% off ooffice and 15% to 19% off the
  local programs and libraries. The new context mixer accounts for 0.5% to 3% on everything
  else, text included.
- The defaults code 16 MiB blocks in parallel and give up 0.6% against `--profile ratio` on
  Silesia (mostly webster and nci, which have more context to gain from one big block) and 9%
  on enwik8, where a single 100 MB block matters most.
- mzip pays for this in time: it compresses at 1-2 MB/s and decompresses at 2.4-6.5 MB/s on
  four cores, against 6-17 MB/s and 3-34 MB/s for the other BWT codecs, and its context mixer
  decodes about as fast as it encodes. The LZ codecs decompress one to two orders of
  magnitude faster than any block-sorting coder.
- xz, brotli and kanzi -l9 still lead on mozilla (Silesia's mozilla is a Tru64 Alpha build
  with almost no x86 code, so the filter does not apply), on ptt5 and sum, and on tiny files
  where mzip's 48-byte headers dominate.

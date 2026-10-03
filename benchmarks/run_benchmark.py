#!/usr/bin/env python3
"""Download standard corpora and benchmark mzip against gzip, bzip2, and xz.

`--corpus canterbury` (default) fetches the Canterbury, Artificial, and Large corpora from
corpus.canterbury.ac.nz; `--corpus silesia` fetches the Silesia corpus from its GitHub mirror
(one zip per file). Every extracted file is checked against its pinned size and, where
pinned, its SHA-256.
"""

from __future__ import annotations

import argparse
import bz2
import datetime as dt
import gzip
import hashlib
import lzma
from pathlib import Path
import platform
import shutil
import statistics
import subprocess
import sys
import tempfile
import time
import urllib.request
import zipfile


ARCHIVES = {
    "cantrbry.zip": (
        "https://corpus.canterbury.ac.nz/resources/cantrbry.zip",
        "c44b686dfc137e74aba4db0540e5d6568cb09e270ba8f8411d2f9df24f39a1a6",
    ),
    "artificl.zip": (
        "https://corpus.canterbury.ac.nz/resources/artificl.zip",
        "6d02ab02183a4cdbc39afb812b9fd038b44c97a2276b6d7afa79516ee69645f3",
    ),
    "large.zip": (
        "https://corpus.canterbury.ac.nz/resources/large.zip",
        "767f3e07004af0dee6dda14b9653609b621fad051e7a80367fcdc4514ec1fc29",
    ),
}

FILES = (
    ("alice29.txt", "English text", 152_089, "cantrbry.zip"),
    ("fields.c", "C source", 11_150, "cantrbry.zip"),
    ("kennedy.xls", "spreadsheet", 1_029_744, "cantrbry.zip"),
    ("ptt5", "fax bitmap", 513_216, "cantrbry.zip"),
    ("aaa.txt", "repeated byte", 100_000, "artificl.zip"),
    ("random.txt", "random text", 100_000, "artificl.zip"),
    ("world192.txt", "CIA fact book", 2_473_400, "large.zip"),
    ("bible.txt", "King James Bible", 4_047_392, "large.zip"),
    ("E.coli", "DNA sequence", 4_638_690, "large.zip"),
)

# The Silesia corpus (https://sun.aei.polsl.pl/~sdeor/index.php?page=silesia) as mirrored on
# GitHub; the hashes are those of the unzipped files, so a re-zipped mirror still verifies.
SILESIA_URL = "https://raw.githubusercontent.com/MiloszKrajewski/SilesiaCorpus/master/{}.zip"
SILESIA_FILES = (
    ("dickens", "English text", 10_192_446,
     "b24c37886142e11d0ee687db6ab06f936207aa7f2ea1fd1d9a36763c7a507e6a"),
    ("mozilla", "executables tar (Alpha)", 51_220_480,
     "657fc3764b0c75ac9de9623125705831ebbfbe08fed248df73bc2dc66e2a963b"),
    ("mr", "MRI image", 9_970_564,
     "68637ed52e3e4860174ed2dc0840ac77d5f1a60abbcb13770d5754e3774d53e6"),
    ("nci", "chemical database", 33_553_445,
     "fc63a31770947b8c2062d3b19ca94c00485a232bb91b502021948fee983e1635"),
    ("ooffice", "x86 executable", 6_152_192,
     "e7ee013880d34dd5208283d0d3d91b07f442e067454276095ded14f322a656eb"),
    ("osdb", "database", 10_085_684,
     "60f027179302ca3ad87c58ac90b6be72ec23588aaa7a3b7fe8ecc0f11def3fa3"),
    ("reymont", "Polish text", 6_627_202,
     "0eac0114a3dfe6e2ee1f345a0f79d653cb26c3bc9f0ed79238af4933422b7578"),
    ("samba", "source tar", 21_606_400,
     "93ba07bc44d8267789c1d911992f40b089ffa2140b4a160fac11ccae9a40e7b2"),
    ("sao", "star catalog", 7_251_944,
     "c2d0ea2cc59d4c21b7fe43a71499342a00cbe530a1d5548770e91ecd6214adcc"),
    ("webster", "dictionary", 41_458_703,
     "6a68f69b26daf09f9dd84f7470368553194a0b294fcfa80f1604efb11143a383"),
    ("x-ray", "X-ray image", 8_474_240,
     "7de9fce1405dc44ae5e6813ed21cd5751e761bd4265655a005d39b9685d1c9ad"),
    ("xml", "XML", 5_345_280,
     "0e82e54e695c1938e4193448022543845b33020c8be6bf3bf3ead2224903e08c"),
)

# corpus -> (archives {name: (url, sha256 or None)}, files, pinned file hashes, description)
CORPORA = {
    "canterbury": (ARCHIVES, FILES, {}, "Canterbury, Artificial, and Large corpora"),
    "silesia": (
        {f"{name}.zip": (SILESIA_URL.format(name), None) for name, *_ in SILESIA_FILES},
        tuple((name, kind, size, f"{name}.zip") for name, kind, size, _ in SILESIA_FILES),
        {name: digest for name, _, _, digest in SILESIA_FILES},
        "Silesia corpus",
    ),
}

REFERENCE_CODECS = (
    ("gzip -9", lambda d: gzip.compress(d, compresslevel=9, mtime=0), gzip.decompress),
    ("bzip2 -9", lambda d: bz2.compress(d, compresslevel=9), bz2.decompress),
    ("xz -9", lambda d: lzma.compress(d, preset=9), lzma.decompress),
)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def download(url: str, destination: Path, expected_hash: str | None) -> None:
    """Fetch url unless destination is already there (and matches expected_hash, if any)."""
    if destination.exists() and (expected_hash is None or sha256(destination) == expected_hash):
        return

    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = destination.with_suffix(destination.suffix + ".tmp")
    print(f"Downloading {url}", file=sys.stderr)
    try:
        with urllib.request.urlopen(url, timeout=60) as response, temporary.open("wb") as output:
            shutil.copyfileobj(response, output)
        actual_hash = sha256(temporary)
        if expected_hash is not None and actual_hash != expected_hash:
            raise RuntimeError(
                f"SHA-256 mismatch for {destination.name}: expected {expected_hash}, got {actual_hash}"
            )
        temporary.replace(destination)
    finally:
        temporary.unlink(missing_ok=True)


def prepare_corpus(data_directory: Path, corpus: str) -> dict[str, Path]:
    archives, files, file_hashes, _description = CORPORA[corpus]
    for archive_name, (url, expected_hash) in archives.items():
        download(url, data_directory / archive_name, expected_hash)

    extracted = data_directory / "files"
    extracted.mkdir(parents=True, exist_ok=True)
    paths: dict[str, Path] = {}
    for name, _kind, expected_size, archive_name in files:
        destination = extracted / name
        with zipfile.ZipFile(data_directory / archive_name) as archive:
            if name not in archive.namelist():
                raise RuntimeError(f"{name} is missing from {archive_name}")
            with archive.open(name) as source, destination.open("wb") as output:
                shutil.copyfileobj(source, output)
        if destination.stat().st_size != expected_size:
            raise RuntimeError(f"unexpected size for {name}")
        if name in file_hashes and sha256(destination) != file_hashes[name]:
            raise RuntimeError(f"SHA-256 mismatch for {name}")
        paths[name] = destination
    return paths


def run_command(arguments: list[str]) -> float:
    started = time.perf_counter()
    completed = subprocess.run(arguments, capture_output=True, text=True, check=False)
    elapsed = time.perf_counter() - started
    if completed.returncode != 0:
        raise RuntimeError(
            f"command failed ({completed.returncode}): {' '.join(arguments)}\n"
            f"stdout:\n{completed.stdout}\nstderr:\n{completed.stderr}"
        )
    return elapsed


def time_callable(action, repeat: int):
    timings = []
    result = None
    for _ in range(repeat):
        started = time.perf_counter()
        result = action()
        timings.append(time.perf_counter() - started)
    return statistics.median(timings), result


def benchmark(binary: Path, data_directory: Path, repeat: int, threads: int | None,
              corpus_name: str):
    corpus = prepare_corpus(data_directory, corpus_name)
    rows: list[dict[str, object]] = []
    reference_totals = {
        name: {"size": 0, "compress": 0.0, "decompress": 0.0} for name, _c, _d in REFERENCE_CODECS
    }
    mzip_total = {"size": 0, "compress": 0.0, "decompress": 0.0}

    work_parent = data_directory.parent
    work_parent.mkdir(parents=True, exist_ok=True)
    thread_arguments = [] if threads is None else ["--threads", str(threads)]
    with tempfile.TemporaryDirectory(prefix="mzip-benchmark-", dir=work_parent) as temporary:
        work = Path(temporary)
        for name, kind, expected_size, _archive_name in CORPORA[corpus_name][1]:
            source = corpus[name]
            archive = work / f"{name}.mz"
            restored = work / f"{name}.restored"
            compress_times: list[float] = []
            decompress_times: list[float] = []
            for _ in range(repeat):
                compress_times.append(
                    run_command(
                        [str(binary), "compress", str(source), str(archive)] + thread_arguments
                    )
                )
                decompress_times.append(
                    run_command([str(binary), "decompress", str(archive), str(restored)])
                )
                if sha256(source) != sha256(restored):
                    raise RuntimeError(f"round-trip verification failed for {name}")

            data = source.read_bytes()
            archive_size = archive.stat().st_size
            row: dict[str, object] = {
                "name": name,
                "kind": kind,
                "input_size": expected_size,
                "archive_size": archive_size,
                "ratio": archive_size / expected_size,
                "compress_ms": statistics.median(compress_times) * 1000.0,
                "decompress_ms": statistics.median(decompress_times) * 1000.0,
            }
            mzip_total["size"] += archive_size
            mzip_total["compress"] += statistics.median(compress_times)
            mzip_total["decompress"] += statistics.median(decompress_times)

            for codec_name, compress, decompress in REFERENCE_CODECS:
                compress_seconds, packed = time_callable(lambda: compress(data), repeat)
                decompress_seconds, unpacked = time_callable(lambda: decompress(packed), repeat)
                if unpacked != data:
                    raise RuntimeError(f"{codec_name} round-trip failed for {name}")
                row[codec_name] = len(packed)
                reference_totals[codec_name]["size"] += len(packed)
                reference_totals[codec_name]["compress"] += compress_seconds
                reference_totals[codec_name]["decompress"] += decompress_seconds

            rows.append(row)
            print(f"Verified {name}", file=sys.stderr)

    return rows, mzip_total, reference_totals


def render_table(headers: list[str], rows: list[list[str]], numeric_from: int) -> list[str]:
    """Markdown table with padded cells so the source stays readable as plain text."""
    widths = [max(len(headers[i]), max(len(row[i]) for row in rows)) for i in range(len(headers))]

    def render_row(cells: list[str]) -> str:
        padded = [
            cells[i].rjust(widths[i]) if i >= numeric_from else cells[i].ljust(widths[i])
            for i in range(len(cells))
        ]
        return "| " + " | ".join(padded) + " |"

    separator = "|" + "|".join(
        ("-" * (widths[i] + 1) + ":") if i >= numeric_from else ("-" * (widths[i] + 2))
        for i in range(len(headers))
    ) + "|"
    return [render_row(headers), separator] + [render_row(row) for row in rows]


def format_report(rows, mzip_total, reference_totals, repeat, binary, threads, corpus) -> str:
    total_input = sum(int(row["input_size"]) for row in rows)
    thread_note = "all cores" if threads is None else str(threads)

    def throughput(seconds: float) -> str:
        return f"{total_input / seconds / 1_000_000:.1f} MB/s"

    summary_rows = [
        [
            "mzip",
            f"{mzip_total['size']:,}",
            f"{mzip_total['size'] / total_input:.4f}",
            throughput(mzip_total["compress"]),
            throughput(mzip_total["decompress"]),
        ]
    ]
    for codec_name, _c, _d in REFERENCE_CODECS:
        totals = reference_totals[codec_name]
        summary_rows.append(
            [
                codec_name,
                f"{totals['size']:,}",
                f"{totals['size'] / total_input:.4f}",
                throughput(totals["compress"]),
                throughput(totals["decompress"]),
            ]
        )

    size_rows = [
        [
            str(row["name"]),
            str(row["kind"]),
            f"{row['input_size']:,}",
            f"{row['archive_size']:,}",
            f"{row['gzip -9']:,}",
            f"{row['bzip2 -9']:,}",
            f"{row['xz -9']:,}",
        ]
        for row in rows
    ]
    size_rows.append(
        [
            "total",
            "",
            f"{total_input:,}",
            f"{mzip_total['size']:,}",
            f"{reference_totals['gzip -9']['size']:,}",
            f"{reference_totals['bzip2 -9']['size']:,}",
            f"{reference_totals['xz -9']['size']:,}",
        ]
    )

    timing_rows = [
        [
            str(row["name"]),
            f"{row['input_size']:,}",
            f"{row['compress_ms']:.1f} ms",
            f"{row['decompress_ms']:.1f} ms",
        ]
        for row in rows
    ]

    lines = [
        "# mzip benchmark",
        "",
        f"- Date (UTC): {dt.datetime.now(dt.timezone.utc).strftime('%Y-%m-%d %H:%M')}",
        f"- Platform: {platform.platform()}",
        f"- Binary: `{binary}`",
        f"- Timing: median of {repeat} run(s); mzip numbers include process start and file I/O,",
        "  reference codecs run in memory through Python's C modules",
        f"- Compression threads: {thread_note}",
        f"- Corpus: {CORPORA[corpus][3]}",
        "",
        "## Summary",
        "",
        f"Total input: {total_input:,} bytes across {len(rows)} files.",
        "",
    ]
    lines += render_table(
        ["Codec", "Total size", "Ratio", "Compress", "Decompress"], summary_rows, 1
    )
    lines += ["", "## Archive sizes", ""]
    lines += render_table(
        ["File", "Kind", "Input", "mzip", "gzip -9", "bzip2 -9", "xz -9"], size_rows, 2
    )
    lines += ["", "## mzip timings", ""]
    lines += render_table(["File", "Input", "Compress", "Decompress"], timing_rows, 1)
    return "\n".join(lines) + "\n"


def parse_arguments() -> argparse.Namespace:
    project_root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True, type=Path, help="path to the mzip executable")
    parser.add_argument(
        "--data-dir",
        type=Path,
        default=project_root / "build" / "benchmark-data",
        help="download/cache directory (Silesia goes to its silesia/ subdirectory)",
    )
    parser.add_argument(
        "--corpus", choices=sorted(CORPORA), default="canterbury", help="corpus to benchmark"
    )
    parser.add_argument("--repeat", type=int, default=3, help="number of timed round-trips")
    parser.add_argument(
        "--threads",
        type=int,
        help="worker threads passed to mzip (default: let the binary decide)",
    )
    parser.add_argument("--output", type=Path, help="optional Markdown output file")
    return parser.parse_args()


def main() -> int:
    arguments = parse_arguments()
    if arguments.repeat < 1:
        raise ValueError("--repeat must be at least 1")
    binary = arguments.binary.resolve()
    if not binary.is_file():
        raise FileNotFoundError(f"mzip executable not found: {binary}")

    data_directory = arguments.data_dir.resolve()
    if arguments.corpus != "canterbury":
        data_directory /= arguments.corpus
    rows, mzip_total, reference_totals = benchmark(
        binary, data_directory, arguments.repeat, arguments.threads, arguments.corpus
    )
    report = format_report(
        rows, mzip_total, reference_totals, arguments.repeat, binary, arguments.threads,
        arguments.corpus,
    )
    if arguments.output:
        arguments.output.parent.mkdir(parents=True, exist_ok=True)
        arguments.output.write_text(report, encoding="utf-8")
    print(report, end="")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, ValueError) as error:
        print(f"benchmark error: {error}", file=sys.stderr)
        raise SystemExit(1) from error

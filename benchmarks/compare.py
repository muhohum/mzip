#!/usr/bin/env python3
"""Compare mzip with other compressors: size, speed and memory on any set of files.

Every file of every corpus goes through every available codec and back; the restored bytes
must match the original SHA-256. For each (codec, file) the script records the compressed
size, the median wall time of --repeat compress and decompress runs, the user CPU time,
and the peak resident set size of the codec process itself (rusage from os.wait4, so only
that child is counted). Results go to a JSON file and a Markdown report.

Each positional argument is one corpus: a directory (its regular files, not recursive) or
a single file. Codecs are command templates (see reference_codecs); only those whose
executable is found in --tools-dir or on PATH run. POSIX only (posix_spawn, wait4).

    python3 benchmarks/compare.py --mzip build/mzip --tools-dir ~/bench-tools/bin \\
        --repeat 3 --json silesia.json --markdown silesia.md data/silesia
"""

from __future__ import annotations

import argparse
import dataclasses
import datetime as dt
import fnmatch
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import re
import shutil
import signal
import statistics
import subprocess
import sys
import tempfile
import time

MIB = 1 << 20
CPUS = os.cpu_count() or 1
PROJECT_ROOT = Path(__file__).resolve().parents[1]


@dataclasses.dataclass
class Codec:
    name: str
    tool: str  # executable: a path, or a name looked up in --tools-dir and then PATH
    compress: list  # arguments after the executable, see expand()
    decompress: list
    stdout: bool = False  # the codec writes its result to stdout instead of {out}
    max_mib: int = 1024  # cap for {mib}
    env: dict = dataclasses.field(default_factory=dict)
    version_args: tuple = ("--version",)
    path: str = ""  # resolved executable
    version: str = ""

    def expand(self, template: list, source: Path, target: Path, size: int) -> list:
        """{in}/{out}: source and target paths, {mib}: input size in MiB rounded up (at
        least 1, at most max_mib), i.e. the smallest block that holds the whole file;
        {cpus}: the number of logical CPUs."""
        mib = min(self.max_mib, max(1, math.ceil(size / MIB)))
        fields = {"in": str(source), "out": str(target), "mib": str(mib), "cpus": str(CPUS)}
        return [self.path] + [arg.format(**fields) for arg in template]


# OpenMP builds of bsc spin in their thread pool by default; on a busy host that burns
# seconds of CPU per file. Passive waiting costs nothing on an idle machine.
OMP_PASSIVE = {"OMP_WAIT_POLICY": "passive"}


def reference_codecs() -> list:
    """The strongest common settings of each tool, plus defaults where they differ."""

    def kanzi(level: str, block: list) -> Codec:
        common = ["-j", "{cpus}", "-f", "-v", "0", "-i", "{in}", "-o", "{out}"]
        return Codec(f"kanzi -l{level}" + (" -b fit" if block else ""), "kanzi",
                     ["-c", "-l", level] + block + common, ["-d"] + common,
                     version_args=("--help",))

    return [
        Codec("gzip -9", "gzip", ["-9", "-n", "-c", "{in}"], ["-d", "-c", "{in}"], stdout=True),
        Codec("bzip2 -9", "bzip2", ["-9", "-c", "{in}"], ["-d", "-c", "{in}"], stdout=True,
              version_args=("--help",)),
        Codec("xz -9e", "xz", ["-9e", "-T1", "-c", "{in}"], ["-d", "-T1", "-c", "{in}"],
              stdout=True),
        Codec("zstd -19", "zstd", ["-19", "-q", "-f", "{in}", "-o", "{out}"],
              ["-d", "-q", "-f", "{in}", "-o", "{out}"]),
        Codec("zstd -22 --long=27", "zstd",
              ["--ultra", "-22", "--long=27", "-q", "-f", "{in}", "-o", "{out}"],
              ["-d", "--long=27", "-q", "-f", "{in}", "-o", "{out}"]),
        Codec("brotli -q11 -w30", "brotli",
              ["-q", "11", "--large_window=30", "-f", "-o", "{out}", "{in}"],
              ["-d", "--large_window=30", "-f", "-o", "{out}", "{in}"]),
        # One block per file (bzip3 caps blocks at 511 MiB), single thread.
        Codec("bzip3 -b fit", "bzip3", ["-e", "-f", "-b", "{mib}", "-j", "1", "{in}", "{out}"],
              ["-d", "-f", "-j", "1", "{in}", "{out}"], max_mib=511),
        # Default 16 MiB blocks coded in parallel, like mzip's defaults.
        Codec("bzip3 -j cpus", "bzip3", ["-e", "-f", "-j", "{cpus}", "{in}", "{out}"],
              ["-d", "-f", "-j", "{cpus}", "{in}", "{out}"]),
        Codec("bsc -b1000 -e2", "bsc", ["e", "{in}", "{out}", "-b1000", "-e2"],
              ["d", "{in}", "{out}"], env=OMP_PASSIVE, version_args=()),
        Codec("bsc (defaults)", "bsc", ["e", "{in}", "{out}"], ["d", "{in}", "{out}"],
              env=OMP_PASSIVE, version_args=()),
        Codec("bcm -9", "bcm", ["-9", "-f", "{in}", "{out}"], ["-d", "-f", "{in}", "{out}"],
              version_args=()),
        kanzi("7", ["-b", "{mib}m"]),  # LZP+TEXT+UTF+BWT+LZP & CM, one block per file
        kanzi("9", []),  # EXE+RLT+TEXT+UTF+DNA & TPAQX, default blocks
    ]


def mzip_codecs(spec: str) -> list:
    """`[LABEL=]PATH` -> defaults, --profile ratio and --threads 1 variants."""
    label, _, path = spec.rpartition("=")
    if not label:
        label = "mzip " + ".".join(tool_version(path, ("--version",)).split(".")[:2])
    base = Codec(label, path, ["compress", "{in}", "{out}"], ["decompress", "{in}", "{out}"])
    return [base,
            dataclasses.replace(base, name=label + " --profile ratio",
                                compress=base.compress + ["--profile", "ratio"]),
            dataclasses.replace(base, name=label + " --threads 1",
                                compress=base.compress + ["--threads", "1"],
                                decompress=base.decompress + ["--threads", "1"])]


def tool_version(path: str, args: tuple) -> str:
    try:
        result = subprocess.run([path, *args], capture_output=True, text=True, timeout=10,
                                errors="replace")
    except (OSError, subprocess.TimeoutExpired):
        return "unknown"
    version = re.search(r"\d+(\.\d+)+", result.stdout + result.stderr)
    return version.group(0) if version else "unknown"


def resolve(codecs: list, tools_dir: Path) -> list:
    """Find each executable; drop the codecs whose tool is missing."""
    found = []
    for codec in codecs:
        if os.sep in codec.tool:
            path = Path(codec.tool)
            candidate = str(path.resolve()) if path.is_file() else None
        else:
            local = tools_dir / codec.tool
            candidate = str(local) if os.access(local, os.X_OK) else shutil.which(codec.tool)
        if not candidate:
            print(f"skipping {codec.name}: {codec.tool} not found", file=sys.stderr)
            continue
        codec.path = candidate
        codec.version = tool_version(candidate, codec.version_args)
        found.append(codec)
    return found


class Timeout(Exception):
    pass


def _alarm(_signum, _frame):
    raise Timeout()


def run(argv: list, env: dict, stdout: Path | None, timeout: float | None) -> dict:
    """Run one codec process; return wall time, user CPU time and peak RSS of that process."""
    with tempfile.NamedTemporaryFile(prefix="stderr-") as errors:
        actions = [(os.POSIX_SPAWN_OPEN, 0, os.devnull, os.O_RDONLY, 0),
                   (os.POSIX_SPAWN_OPEN, 2, errors.name, os.O_WRONLY | os.O_TRUNC, 0)]
        if stdout:
            actions.append((os.POSIX_SPAWN_OPEN, 1, str(stdout),
                            os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o644))
        started = time.perf_counter()
        pid = os.posix_spawn(argv[0], argv, env, file_actions=actions)
        try:
            if timeout:
                signal.setitimer(signal.ITIMER_REAL, timeout)
            _, status, usage = os.wait4(pid, 0)
        except Timeout:
            os.kill(pid, signal.SIGKILL)
            os.wait4(pid, 0)
            raise RuntimeError(f"timed out after {timeout:g} s: {' '.join(argv)}")
        finally:
            signal.setitimer(signal.ITIMER_REAL, 0)
        wall = time.perf_counter() - started
        if not os.WIFEXITED(status) or os.WEXITSTATUS(status) != 0:
            message = Path(errors.name).read_text(errors="replace").strip()[-500:]
            raise RuntimeError(f"exit status {status}: {' '.join(argv)}\n{message}")
    rss_unit = 1 if sys.platform == "darwin" else 1024  # ru_maxrss is in KiB on Linux
    return {"wall": wall, "user": usage.ru_utime, "rss": usage.ru_maxrss * rss_unit}


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(MIB), b""):
            digest.update(chunk)
    return digest.hexdigest()


def measure(codec: Codec, source: Path, size: int, digest: str, work: Path, args) -> dict:
    archive, restored = work / "archive", work / "restored"
    env = dict(os.environ, **codec.env)
    compress, decompress = [], []
    for _ in range(args.repeat):
        for path in (archive, restored):
            if path.exists():
                path.unlink()
        compress.append(run(codec.expand(codec.compress, source, archive, size), env,
                            archive if codec.stdout else None, args.timeout))
        decompress.append(run(codec.expand(codec.decompress, archive, restored, size), env,
                              restored if codec.stdout else None, args.timeout))
        if sha256(restored) != digest:
            raise RuntimeError("restored bytes differ from the input")
    return {
        "output": archive.stat().st_size,
        "compress_s": statistics.median(r["wall"] for r in compress),
        "decompress_s": statistics.median(r["wall"] for r in decompress),
        "compress_user_s": statistics.median(r["user"] for r in compress),
        "decompress_user_s": statistics.median(r["user"] for r in decompress),
        "compress_rss": max(r["rss"] for r in compress),
        "decompress_rss": max(r["rss"] for r in decompress),
        "compress_runs_s": [r["wall"] for r in compress],
        "decompress_runs_s": [r["wall"] for r in decompress],
    }


def corpus_files(item: Path) -> list:
    if item.is_dir():
        return sorted(p for p in item.iterdir() if p.is_file() and not p.name.startswith("."))
    return [item]


def benchmark(corpus: Path, codecs: list, work: Path, args) -> dict:
    files = []
    results = []
    for source in corpus_files(corpus):
        size = source.stat().st_size
        digest = sha256(source)
        files.append({"file": source.name, "input": size, "sha256": digest})
        for codec in codecs:
            row = {"codec": codec.name, "file": source.name, "input": size}
            try:
                row.update(measure(codec, source, size, digest, work, args))
                note = f"{row['output']:>12,d} {row['compress_s']:8.2f}s {row['decompress_s']:8.2f}s"
            except (RuntimeError, OSError) as error:  # failed, timed out, or no output
                row["error"] = str(error)
                note = "FAILED " + str(error).splitlines()[0]
            results.append(row)
            print(f"{corpus.name}/{source.name:20s} {codec.name:28s} {note}", file=sys.stderr,
                  flush=True)
    return {"name": corpus.name, "path": str(corpus), "files": files, "results": results}


def host_info(codecs: list) -> dict:
    cpu = platform.processor() or platform.machine()
    try:
        cpu = next(line.split(":", 1)[1].strip()
                   for line in Path("/proc/cpuinfo").read_text().splitlines()
                   if line.startswith("model name"))
    except (OSError, StopIteration):
        pass
    compiler = "unknown"
    mzip = next((c.path for c in codecs if c.name.startswith("mzip")), None)
    if mzip:  # the CMake cache of the first mzip build names its compiler
        for cache in (Path(mzip).parent / "CMakeCache.txt", Path(mzip).parent.parent / "CMakeCache.txt"):
            if cache.is_file():
                entries = dict(re.findall(r"^(CMAKE_CXX_COMPILER|CMAKE_BUILD_TYPE):\w+=(.*)$",
                                          cache.read_text(errors="replace"), re.M))
                cxx = entries.get("CMAKE_CXX_COMPILER", "")
                compiler = f"{tool_version(cxx, ('--version',)) if cxx else 'unknown'}, " \
                           f"{entries.get('CMAKE_BUILD_TYPE') or 'default'} build"
                break
    # Linux charges an exec'd child with the peak RSS of the process that spawned it, so
    # this interpreter's footprint is the smallest memory figure the report can show.
    floor = run([shutil.which("true") or "/bin/true"], dict(os.environ), None, None)["rss"]
    return {"cpu": cpu, "logical_cpus": CPUS, "os": platform.platform(), "rss_floor": floor,
            "mzip_compiler": compiler, "python": platform.python_version(),
            "date_utc": dt.datetime.now(dt.timezone.utc).strftime("%Y-%m-%d %H:%M")}


def table(headers: list, rows: list) -> list:
    """Markdown table; the first column is left-aligned, the others right-aligned."""
    widths = [max(len(str(r[i])) for r in [headers] + rows) for i in range(len(headers))]

    def line(cells):
        return "| " + " | ".join(str(c).ljust(w) if i == 0 else str(c).rjust(w)
                                 for i, (c, w) in enumerate(zip(cells, widths))) + " |"

    rule = "|" + "|".join("-" * (w + 2) if i == 0 else "-" * (w + 1) + ":"
                          for i, w in enumerate(widths)) + "|"
    return [line(headers), rule] + [line(r) for r in rows]


def report(data: dict) -> str:
    host = data["host"]
    lines = [
        "# Compression comparison", "",
        f"- Date (UTC): {host['date_utc']}",
        f"- Host: {host['cpu']}, {host['logical_cpus']} logical CPUs, {host['os']}",
        f"- mzip compiler: {host['mzip_compiler']}",
        f"- Timing: median of {data['repeat']} run(s) per file, wall clock including process "
        "start and file I/O; CPU is user time; memory is the peak RSS of the codec process.",
        f"- Memory below {host['rss_floor'] / MIB:.0f} MiB is not resolved: Linux counts the "
        "launching Python process's peak RSS in every child it spawns.",
        "- Every round trip is verified against the SHA-256 of the input.", "",
        "## Codecs", "",
    ]
    lines += table(["Codec", "Version", "Compress", "Decompress"],
                   [[c["name"], c["version"], "`" + " ".join(c["compress"]) + "`",
                     "`" + " ".join(c["decompress"]) + "`"] for c in data["codecs"]])
    lines += ["", "`{mib}` is the input size in MiB rounded up (the smallest block that holds "
              "the whole file), capped at the codec's maximum block; `{cpus}` is the number of "
              "logical CPUs."]
    for corpus in data["corpora"]:
        total_in = sum(f["input"] for f in corpus["files"])
        by_codec = {}
        for row in corpus["results"]:
            by_codec.setdefault(row["codec"], []).append(row)
        complete = {name: rows for name, rows in by_codec.items()
                    if not any("error" in r for r in rows)}
        order = sorted(complete, key=lambda name: sum(r["output"] for r in complete[name]))
        lines += ["", f"## {corpus['name']}", "",
                  f"{len(corpus['files'])} files, {total_in:,} bytes (`{corpus['path']}`).", ""]
        totals = []
        for name in order:
            rows = complete[name]
            size = sum(r["output"] for r in rows)
            ctime = sum(r["compress_s"] for r in rows)
            dtime = sum(r["decompress_s"] for r in rows)
            totals.append([name, f"{size:,}", f"{size / total_in:.4f}",
                           f"{total_in / ctime / 1e6:.2f}", f"{total_in / dtime / 1e6:.2f}",
                           f"{sum(r['compress_user_s'] for r in rows):.1f}",
                           f"{sum(r['decompress_user_s'] for r in rows):.1f}",
                           f"{max(r['compress_rss'] for r in rows) / MIB:.0f}",
                           f"{max(r['decompress_rss'] for r in rows) / MIB:.0f}"])
        lines += table(["Codec", "Total size", "Ratio", "Comp. MB/s", "Dec. MB/s",
                        "Comp. CPU s", "Dec. CPU s", "Comp. peak MiB", "Dec. peak MiB"], totals)
        for name, rows in by_codec.items():
            for r in rows:
                if "error" in r:
                    lines += ["", f"- {name} failed on {r['file']}: "
                              + r["error"].splitlines()[0]]
        # Per-file sizes; a codec whose sizes repeat an earlier column (a thread-count
        # variant, say) is folded into that column's header.
        sizes = {name: [r["output"] for r in complete[name]] for name in order}
        columns = {}
        for name in order:
            same = next((c for c in columns if sizes[c] == sizes[name]), None)
            columns.setdefault(same or name, []).append(name)
        size_rows = []
        for i, f in enumerate(corpus["files"]):
            best = min(order, key=lambda name: sizes[name][i]) if order else ""
            size_rows.append([f["file"], f"{f['input']:,}"]
                             + [f"{sizes[c][i]:,}" for c in columns] + [best])
        lines += ["", "Compressed size per file:", ""]
        lines += table(["File", "Input"] + [" = ".join(n) for n in columns.values()]
                       + ["Smallest"], size_rows)
    return "\n".join(lines) + "\n"


def parse_arguments():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("inputs", nargs="*", type=Path, help="corpus directories or files")
    parser.add_argument("--mzip", action="append", metavar="[LABEL=]PATH",
                        help="mzip binary, repeatable (default: build/mzip); the label "
                             "defaults to 'mzip X.Y' from --version")
    parser.add_argument("--tools-dir", type=Path,
                        default=Path(os.environ.get("MZIP_BENCH_TOOLS",
                                                    PROJECT_ROOT / "build" / "bench-tools" / "bin")),
                        help="directory searched before PATH for the other codecs "
                             "(default: $MZIP_BENCH_TOOLS or build/bench-tools/bin)")
    parser.add_argument("--codecs", help="comma-separated name patterns to run (fnmatch)")
    parser.add_argument("--exclude", help="comma-separated name patterns to skip")
    parser.add_argument("--repeat", type=int, default=3, help="timed round trips per file")
    parser.add_argument("--timeout", type=float, help="seconds before one run is abandoned")
    parser.add_argument("--work-dir", type=Path, help="scratch directory for archives")
    parser.add_argument("--json", type=Path, help="write all measurements here")
    parser.add_argument("--markdown", type=Path, help="write the report here")
    parser.add_argument("--list", action="store_true", help="list the available codecs and exit")
    parser.add_argument("--from-json", type=Path, help="only render the report of a saved run")
    return parser.parse_args()


def main() -> int:
    args = parse_arguments()
    if args.repeat < 1:
        raise SystemExit("--repeat must be at least 1")
    if args.from_json:
        text = report(json.loads(args.from_json.read_text()))
        if args.markdown:
            args.markdown.write_text(text)
        print(text, end="")
        return 0
    codecs = []
    for spec in args.mzip or [str(PROJECT_ROOT / "build" / "mzip")]:
        added = mzip_codecs(spec)
        taken = {codec.name for codec in codecs}
        if added[0].name in taken:
            # Two binaries of the same version: tell them apart by order on the command line.
            suffix = 2
            while f"{added[0].name} #{suffix}" in taken:
                suffix += 1
            added = [dataclasses.replace(c, name=c.name.replace(added[0].name,
                                                                f"{added[0].name} #{suffix}", 1))
                     for c in added]
        codecs += added
    codecs += reference_codecs()

    def matches(name, patterns):
        return any(fnmatch.fnmatchcase(name, p.strip()) for p in patterns.split(","))

    codecs = resolve(codecs, args.tools_dir)
    codecs = [c for c in codecs if (not args.codecs or matches(c.name, args.codecs))
              and not (args.exclude and matches(c.name, args.exclude))]
    if args.list or not args.inputs:
        for c in codecs:
            print(f"{c.name:28s} {c.version:60.60s} {c.path}")
        return 0

    signal.signal(signal.SIGALRM, _alarm)
    data = {"host": host_info(codecs), "repeat": args.repeat,
            "codecs": [{"name": c.name, "path": c.path, "version": c.version,
                        "compress": [Path(c.path).name] + c.compress,
                        "decompress": [Path(c.path).name] + c.decompress, "env": c.env}
                       for c in codecs],
            "corpora": []}
    if args.work_dir:
        Path(args.work_dir).mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="mzip-compare-", dir=args.work_dir) as work:
        for corpus in args.inputs:
            data["corpora"].append(benchmark(corpus, codecs, Path(work), args))
    if args.json:
        args.json.write_text(json.dumps(data, indent=1) + "\n")
    text = report(data)
    if args.markdown:
        args.markdown.write_text(text)
    print(text, end="")
    failed = sum("error" in r for c in data["corpora"] for r in c["results"])
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())

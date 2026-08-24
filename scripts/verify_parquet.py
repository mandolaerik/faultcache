#!/usr/bin/env python3
# © 2026 Erik Carstensen
# SPDX-License-Identifier: MPL-2.0

"""Check examples/parquet_column against files written by pyarrow.

The example reads each row group through pyarrow and copies the arrow
values buffer into a faultcache region, so what needs proving is that the
region really is that column: every writer setting has to give pyarrow's
own values back exactly, and anything that cannot be represented as a
dense int array has to be refused when the column is opened rather than
during a fill (an exception raised inside the fault handler cannot reach
the caller, so a late check would hand out zeros instead).

Also checks that the array really is lazy: read through numpy under a
budget smaller than the column, and row groups have to fault in and get
evicted again while the values stay correct.

Only registered as a test when the build is configured with
-Dpyarrow=true, so a missing pyarrow here is a failure, not a skip.
"""
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy
import pyarrow as pa
import pyarrow.parquet as pq

ROOT = Path(__file__).resolve().parent.parent
EXAMPLE = ROOT / "examples" / "parquet_column"
sys.path.insert(0, str(EXAMPLE))

import faultcache  # noqa: E402  (needs the path above)
from lazy_column import LazyIntColumn  # noqa: E402

N = 25_000
COLUMNS = {
    "i64": (pa.int64(), [i * 7919 % 1_000_003 for i in range(N)]),
    "i32": (pa.int32(), [-(i % 1000) for i in range(N)]),
    "i8": (pa.int8(), [i % 127 - 64 for i in range(N)]),
    "u16": (pa.uint16(), [i % 65521 for i in range(N)]),
    "few": (pa.int64(), [i % 3 for i in range(N)]),  # dictionary-friendly
}

# Every one of these has to read back exactly as pyarrow reads it.
SUPPORTED = [
    ("gzip + dictionary (pyarrow's default)", dict(compression="gzip")),
    ("uncompressed + dictionary", dict(compression="none")),
    ("snappy", dict(compression="snappy")),
    ("zstd", dict(compression="zstd")),
    ("gzip + plain", dict(compression="gzip", use_dictionary=False)),
    ("v2 data pages", dict(compression="gzip", data_page_version="2.0")),
    ("delta encoding",
     dict(compression="gzip", use_dictionary=False,
          column_encoding="DELTA_BINARY_PACKED")),
    ("uneven row groups", dict(compression="gzip", rows_per_group=997)),
    # A tiny dictionary budget makes the writer give up mid-chunk, so one
    # column chunk mixes RLE_DICTIONARY and PLAIN pages.
    ("dictionary falling back to plain",
     dict(compression="gzip", dictionary_pagesize_limit=1024,
          data_page_size=4096)),
    # Nothing needs proving about nulls when the schema rules them out, so
    # this is the one shape that works without statistics.
    ("required (non-nullable), no statistics",
     dict(compression="gzip", nullable=False, write_statistics=False)),
]

# Each has to be refused by LazyIntColumn(), with that text in the error.
UNSUPPORTED = [
    ("nulls", dict(compression="gzip", nulls=True), "contains nulls"),
    ("an optional column without statistics",
     dict(compression="gzip", write_statistics=False), "rule them out"),
]

failures = []


def report(label, ok, detail=""):
    print(f"{'ok  ' if ok else 'FAIL'} {label}{': ' + detail if detail else ''}",
          flush=True)
    if not ok:
        failures.append(label)


def write(path, *, nullable=True, nulls=False, rows_per_group=4000, **kw):
    data = {name: list(values) for name, (_, values) in COLUMNS.items()}
    if nulls:
        data["i64"][7] = None
    schema = pa.schema([pa.field(name, ptype, nullable=nullable)
                        for name, (ptype, _) in COLUMNS.items()])
    pq.write_table(pa.table(data, schema=schema), path,
                   row_group_size=rows_per_group, **kw)


def truth(path, name):
    return pq.read_table(path, columns=[name])[name].to_pylist()


def check_supported(label, path):
    with faultcache.Pool(maxsize=64 << 10) as pool:
        for name in COLUMNS:
            try:
                with LazyIntColumn(pool, path, name) as column:
                    got = list(column[:])
            except Exception as exc:
                report(f"{label} / {name}", False, repr(exc))
                continue
            want = truth(path, name)
            if got == want:
                report(f"{label} / {name}", True, f"{len(got)} values")
            else:
                i = next(i for i, (a, b) in enumerate(zip(got, want)) if a != b)
                report(f"{label} / {name}", False,
                       f"differs at {i}: {got[i]} != {want[i]}")


def check_rejected(label, path, name, expected):
    with faultcache.Pool() as pool:
        try:
            LazyIntColumn(pool, path, name).close()
        except Exception as exc:
            report(f"rejects {label}", expected in str(exc), str(exc))
        else:
            report(f"rejects {label}", False, "opened without complaint")


def check_lazy(path, name):
    """A budget below the column size must force real eviction, and the
    values must survive the churn."""
    itemsize = 8
    budget = 3 * 4000 * itemsize
    with faultcache.Pool(maxsize=budget) as pool:
        with LazyIntColumn(pool, path, name) as column:
            array = column.numpy()
            stats = column._region._debug_lru_stats()
            if column.fills or stats.resident_bytes:
                report("lazy / nothing read up front", False,
                       f"fills={column.fills} resident={stats.resident_bytes}")
                return

            first = int(array[0])
            after_one = column.fills
            if not 1 <= after_one <= 2:  # 2 when a page straddles two chunks
                report("lazy / one read touches one row group", False,
                       f"fills={after_one}")
                return

            got = array.tolist()
            after_scan = column.fills
            stats = column._region._debug_lru_stats()
            # A forward scan never revisits, so it is going back to the
            # front that proves the early row groups really went away.
            reread = int(array[0])
            refilled = column.fills > after_scan

    report("lazy / nothing read up front", True)
    report("lazy / one read touches one row group", True,
           f"fills={after_one} of {column.row_groups} row groups")
    report("lazy / scanning evicts as it goes",
           stats.resident_bytes <= budget and refilled,
           f"{stats.resident_bytes}B resident after a full scan, "
           f"first row group re-read: {refilled}")
    report("lazy / values survive eviction",
           got == truth(path, name) and got[0] == first == reread)


def check_example_runs(path):
    """The example's own __main__, so its demo code is covered too -- with
    -v, which is the only caller of its residency report."""
    proc = subprocess.run([sys.executable, str(EXAMPLE / "lazy_column.py"),
                           "-v", str(path), "i64"],
                          capture_output=True, text=True)
    report("example runs standalone", proc.returncode == 0,
           (proc.stdout + proc.stderr).strip().splitlines()[-1])


def main():
    print(f"pyarrow {pa.__version__}, numpy {numpy.__version__}", flush=True)
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "case.parquet"

        for label, kwargs in SUPPORTED:
            write(path, **kwargs)
            check_supported(label, path)

        for label, kwargs, expected in UNSUPPORTED:
            write(path, **kwargs)
            check_rejected(label, path, "i64", expected)

        other = Path(tmp) / "other.parquet"
        pq.write_table(pa.table({"f": [1.5] * 100, "s": ["x"] * 100}), other)
        for name in ("f", "s"):
            check_rejected(f"a non-integer column ({name})", other, name,
                           "not an integer column")

        write(path, compression="gzip")
        check_lazy(path, "i64")
        check_example_runs(path)

    print(f"\n{len(failures)} failure(s)")
    for label in failures:
        print(f"  {label}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())

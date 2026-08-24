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

import pyarrow as pa
import pyarrow.parquet as pq
import pytest

ROOT = Path(__file__).resolve().parent.parent
EXAMPLE = ROOT / "examples" / "parquet_column"
sys.path.insert(0, str(EXAMPLE))

import faultcache  # noqa: E402  (needs the path above)
from lazy_column import LazyIntColumn  # noqa: E402

N = 25_000
ROWS_PER_GROUP = 4000
COLUMNS = {
    "i64": (pa.int64(), [i * 7919 % 1_000_003 for i in range(N)]),
    "i32": (pa.int32(), [-(i % 1000) for i in range(N)]),
    "i8": (pa.int8(), [i % 127 - 64 for i in range(N)]),
    "u16": (pa.uint16(), [i % 65521 for i in range(N)]),
    "few": (pa.int64(), [i % 3 for i in range(N)]),  # dictionary-friendly
}

# Every one of these has to read back exactly as pyarrow reads it.
SUPPORTED = {
    "gzip + dictionary (pyarrow's default)": dict(compression="gzip"),
    "uncompressed + dictionary": dict(compression="none"),
    "snappy": dict(compression="snappy"),
    "zstd": dict(compression="zstd"),
    "gzip + plain": dict(compression="gzip", use_dictionary=False),
    "v2 data pages": dict(compression="gzip", data_page_version="2.0"),
    "delta encoding": dict(compression="gzip", use_dictionary=False,
                           column_encoding="DELTA_BINARY_PACKED"),
    "uneven row groups": dict(compression="gzip", rows_per_group=997),
    # A tiny dictionary budget makes the writer give up mid-chunk, so one
    # column chunk mixes RLE_DICTIONARY and PLAIN pages.
    "dictionary falling back to plain": dict(compression="gzip",
                                             dictionary_pagesize_limit=1024,
                                             data_page_size=4096),
    # Nothing needs proving about nulls when the schema rules them out, so
    # this is the one shape that works without statistics.
    "required (non-nullable), no statistics": dict(compression="gzip",
                                                   nullable=False,
                                                   write_statistics=False),
}

# Each has to be refused by LazyIntColumn(), with that text in the error.
UNSUPPORTED = {
    "nulls": (dict(compression="gzip", nulls=True), "contains nulls"),
    "an optional column without statistics":
        (dict(compression="gzip", write_statistics=False), "rule them out"),
}


def write(path, *, nullable=True, nulls=False, rows_per_group=ROWS_PER_GROUP,
          **kw):
    data = {name: list(values) for name, (_, values) in COLUMNS.items()}
    if nulls:
        data["i64"][7] = None
    schema = pa.schema([pa.field(name, ptype, nullable=nullable)
                        for name, (ptype, _) in COLUMNS.items()])
    pq.write_table(pa.table(data, schema=schema), path,
                   row_group_size=rows_per_group, **kw)


def truth(path, name):
    return pq.read_table(path, columns=[name])[name].to_pylist()


@pytest.fixture(scope="module")
def tmpdir():
    with tempfile.TemporaryDirectory() as tmp:
        yield Path(tmp)


@pytest.fixture(scope="module", params=list(SUPPORTED), ids=list(SUPPORTED))
def supported_file(request, tmpdir):
    """One written file per writer setting, shared by that setting's
    column cases -- writing 25k rows once per case would dominate."""
    path = tmpdir / f"{abs(hash(request.param))}.parquet"
    write(path, **SUPPORTED[request.param])
    return path


@pytest.fixture(scope="module")
def plain_file(tmpdir):
    path = tmpdir / "plain.parquet"
    write(path, compression="gzip")
    return path


@pytest.mark.parametrize("name", list(COLUMNS))
def test_column_matches_pyarrow(supported_file, name):
    with faultcache.Pool(maxsize=64 << 10) as pool:
        with LazyIntColumn(pool, supported_file, name) as column:
            assert list(column[:]) == truth(supported_file, name)


@pytest.mark.parametrize("label", list(UNSUPPORTED))
def test_rejects_at_open(tmpdir, label):
    kwargs, expected = UNSUPPORTED[label]
    path = tmpdir / f"unsupported-{abs(hash(label))}.parquet"
    write(path, **kwargs)
    with faultcache.Pool() as pool:
        with pytest.raises(ValueError, match=expected):
            LazyIntColumn(pool, path, "i64").close()


@pytest.mark.parametrize("name", ["f", "s"])
def test_rejects_non_integer_column(tmpdir, name):
    path = tmpdir / "other.parquet"
    pq.write_table(pa.table({"f": [1.5] * 100, "s": ["x"] * 100}), path)
    with faultcache.Pool() as pool:
        with pytest.raises(ValueError, match="not an integer column"):
            LazyIntColumn(pool, path, name).close()


def test_column_is_lazy_and_survives_eviction(plain_file):
    """A budget below the column size must force real eviction, and the
    values must survive the churn."""
    budget = 3 * ROWS_PER_GROUP * 8
    with faultcache.Pool(maxsize=budget) as pool:
        with LazyIntColumn(pool, plain_file, "i64") as column:
            array = column.numpy()
            assert column.fills == 0
            assert column._region._debug_lru_stats().resident_bytes == 0

            first = int(array[0])
            # 2 when a page straddles two chunks.
            assert 1 <= column.fills <= 2

            got = array.tolist()
            after_scan = column.fills
            resident = column._region._debug_lru_stats().resident_bytes
            # A forward scan never revisits, so it is going back to the
            # front that proves the early row groups really went away.
            reread = int(array[0])

            assert resident <= budget
            assert column.fills > after_scan, "first row group was not evicted"
            assert got == truth(plain_file, "i64")
            assert got[0] == first == reread


def test_example_runs_standalone(plain_file):
    """The example's own __main__, so its demo code is covered too -- with
    -v, which is the only caller of its residency report."""
    proc = subprocess.run([sys.executable, str(EXAMPLE / "lazy_column.py"),
                           "-v", str(plain_file), "i64"],
                          capture_output=True, text=True)
    assert proc.returncode == 0, proc.stdout + proc.stderr

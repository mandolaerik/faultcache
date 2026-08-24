#!/usr/bin/env python3
# © 2026 Erik Carstensen
# SPDX-License-Identifier: MPL-2.0

"""One parquet integer column as a single, directly-indexable lazy array.

Parquet stores a column as one independently-compressed chunk per row
group, which is exactly the shape faultcache wants: a region whose chunk
`i` is row group `i`, decoded on first touch. pyarrow does the decoding,
so encodings, compression and page versions are its problem rather than
ours; what this adds is that the decoding happens per row group on
demand, and that the result is one flat array instead of a chunked one.

An arrow integer array is a dense little-endian values buffer, so the
fill is a straight copy of that buffer into the region -- and the
region's bytes then *are* the array. `memoryview.cast()` (or
`numpy.frombuffer`) hands the whole column out with no further copy:
`column[12345]` is a plain index, but only the row groups actually
touched are ever read off disk.

The one real restriction is nulls, which a dense int array has nowhere to
put, so a column is accepted only if the row group statistics prove it
has none. That check has to happen when the column is opened, never
during a fill: a fill callback runs inside the fault handler, where an
exception has nowhere to go and would leave the caller holding zeros.

    python3 lazy_column.py [-v] FILE.parquet COLUMN
"""
from __future__ import annotations

import argparse
import sys

import numpy
import pyarrow as pa
import pyarrow.parquet as pq

import faultcache

# Bytes per value -> the signed memoryview/struct format character.
FORMATS = {1: "b", 2: "h", 4: "i", 8: "q"}


class LazyIntColumn:
    """A parquet integer column, materialised one row group at a time."""

    def __init__(self, pool: faultcache.Pool, path: str, name: str):
        self._pool = pool
        self._path = path
        self._name = name
        self._region = None
        self._bytes = None
        self.values = None
        with pq.ParquetFile(path) as file:
            metadata = file.metadata
            field = file.schema_arrow.field(self._name)
            if not pa.types.is_integer(field.type):
                raise ValueError(
                    f"column {self._name!r} is not an integer column")

            self.itemsize = field.type.bit_width // 8
            signed = pa.types.is_signed_integer(field.type)
            self._fmt = FORMATS[self.itemsize]
            if not signed:
                self._fmt = self._fmt.upper()
            self.dtype = f"<{'i' if signed else 'u'}{self.itemsize}"

            index = metadata.schema.names.index(self._name)
            for i in range(metadata.num_row_groups):
                statistics = metadata.row_group(i).column(index).statistics
                if field.nullable and (statistics is None
                                    or statistics.null_count != 0):
                    raise ValueError(
                        f"column {self._name!r} contains nulls, or lacks "
                        "the statistics to rule them out")

            self.fills = 0
            self.row_groups = metadata.num_row_groups
            self._sizes = [metadata.row_group(i).num_rows * self.itemsize
                           for i in range(self.row_groups)]

    def __enter__(self) -> "LazyIntColumn":
        assert self._region is None, "LazyIntColumn cannot be re-entered"
        self._region = self._pool.create_region(
            self._sizes, self._fill_chunk)
        self._bytes = self._region.view()
        self.values = self._bytes.cast(self._fmt)
        return self

    def __exit__(self, *exc_info) -> None:
        # Dropping the last reference is what releases the region's view;
        # memoryview.release() would not, and Region.close() refuses while
        # a view is outstanding. A numpy array from numpy() counts as one
        # too -- it keeps the memoryview alive as its .base.
        self.values = None
        self._bytes = None
        if self._region is not None:
            self._region.close()
            self._region = None

    def _fill_chunk(self, chunk: int, buf: memoryview) -> None:
        file = pq.ParquetFile(self._path)
        try:
            table = file.read_row_group(chunk, columns=[self._name],
                                        use_threads=False)
            array = pa.concat_arrays(table.column(0).chunks)
        finally:
            file.close()
        # An arrow buffer views as signed bytes; the region's view is
        # unsigned, and memoryview assignment insists the two agree.
        values = memoryview(array.buffers()[1]).cast("B")
        start = array.offset * self.itemsize
        buf[:] = values[start:start + len(buf)]
        self.fills += 1

    def numpy(self):
        """The whole column as a zero-copy read-only numpy array."""
        return numpy.frombuffer(self._bytes, dtype=self.dtype)

    def residency(self) -> str:
        """What the pool is currently holding, one character per row group:
        `.` never read, `#` resident, `~` resident but demoted past the
        pool's midpoint, so its pages are unmapped again and the next touch
        of it faults without re-reading. Uses faultcache's debug-only API,
        hence the underscores."""
        stats = self._region._debug_lru_stats()
        state = "".join(
            "~" if e.cold else "#" if e.resident else "."
            for e in self._region._debug_lru_history())
        return (f"{self.fills} fill(s), {stats.resident_bytes} B resident, "
                f"{stats.fault_events_total} fault(s)  [{state}]")

    def __len__(self) -> int:
        return len(self.values)

    def __getitem__(self, key):
        return self.values[key]


def demo_numpy(column: LazyIntColumn, note) -> None:
    """Kept in its own function so the array -- and with it the region's
    view -- is gone again by the time the column is closed."""
    array = column.numpy()
    print(f"sum of the first 1000 rows: {array[:1000].sum()}  "
          f"({column.fills} row group(s) read)")
    note(column)
    print(f"max of the whole column: {numpy.max(array)}  "
          f"({column.fills} row group(s) read)")
    note(column)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.partition("\n")[0])
    parser.add_argument("file", metavar="FILE.parquet")
    parser.add_argument("column", metavar="COLUMN")
    parser.add_argument("-v", "--verbose", action="store_true",
                        help="after every step, print which row groups the "
                             "pool is holding")
    args = parser.parse_args()

    def note(column: LazyIntColumn) -> None:
        if args.verbose:
            print(f"    {column.residency()}")

    # A budget of a few row groups: enough to work in, small enough that a
    # full scan of a much larger column never holds all of it at once.
    with faultcache.Pool(maxsize=3 << 20) as pool:
        with LazyIntColumn(pool, args.file, args.column) as column:
            print(f"{args.column}: {len(column)} rows, {column.row_groups} "
                  f"row groups, {column.itemsize * 8}-bit")
            note(column)

            print(f"column[0] = {column[0]}, "
                  f"column[-1] = {column[-1]}  "
                  f"({column.fills} row group(s) read)")
            note(column)

            middle = len(column) // 2
            print(f"column[{middle}] = {column[middle]}  "
                  f"({column.fills} row group(s) read)")
            note(column)

            demo_numpy(column, note)
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
# © 2026 Erik Carstensen
# SPDX-License-Identifier: MPL-2.0
"""Guards the library's ABI surface against accidental change.

Everything without FC_API is hidden (src/meson.build sets
gnu_symbol_visibility), so an unexpected name here means something
internal escaped -- most likely a new source file whose helpers are not
static, or a declaration that picked up the marker by accident. A
missing name is an ABI break. Both are fine to do on purpose, and doing
them on purpose means editing EXPORTED below.

    test_exported_symbols.py NM LIBRARY
"""
import subprocess
import sys

EXPORTED = {
    "fc_init",
    "fc_rearm_handler",
    "fc_pool_create",
    "fc_pool_destroy",
    "fc_region_create",
    "fc_region_destroy",
    "fc_region_size",
    "fc_region_base",
    "fc_client_pool_create",
    "fc_client_pool_destroy",
    "fc_client_region_create",
    "fc_client_region_destroy",
    "fc_client_region_size",
    "fc_client_region_base",
    "fc_server_create",
    "fc_server_run",
    "fc_server_stop",
    "fc_server_destroy",
    "fc_region_debug_stats",
    "fc_region_debug_lru_stats",
    "fc_region_debug_lru_history",
    "fc_pool_debug_lru_queue",
    "fc_debug_set_misuse_hook",
}


def main():
    if len(sys.argv) != 3:
        return __doc__.strip().splitlines()[-1].strip()
    nm, library = sys.argv[1], sys.argv[2]

    result = subprocess.run(
        [nm, "--dynamic", "--defined-only", "--format=posix", library],
        capture_output=True, text=True)
    if result.returncode != 0:
        return f"{nm} failed on {library}: {result.stderr.strip()}"

    # posix format is "name type value size", one symbol per line.
    found = {line.split()[0]
             for line in result.stdout.splitlines() if line.strip()}

    leaked = sorted(found - EXPORTED)
    missing = sorted(EXPORTED - found)
    for name in leaked:
        print(f"unexpected: {name}")
    for name in missing:
        print(f"missing:    {name}")
    if leaked or missing:
        return (f"{len(leaked)} unexpected and {len(missing)} missing "
                "symbol(s) -- fix the code, or edit EXPORTED if the change "
                "was intended")

    print(f"ok: {len(found)} exported symbols, exactly as listed")
    return None


if __name__ == "__main__":
    sys.exit(main())

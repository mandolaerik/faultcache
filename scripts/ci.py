#!/usr/bin/env python3
# © 2026 Erik Carstensen
# SPDX-License-Identifier: MPL-2.0

"""Run one CI lane locally, exactly as GitHub Actions runs it.

    scripts/ci.py default
    scripts/ci.py asan coverage
    scripts/ci.py python:3.10
    scripts/ci.py python:/opt/py314t/bin/python3
    scripts/ci.py all -L4
    scripts/ci.py clean

The workflow in .github/workflows/ci.yml only does checkout + toolchain
setup and then calls this, so build dirs, configure flags, sanitizer
options, the coverage gate and the list of supported interpreters have
exactly one definition.

`all` runs everything it can: the three C lanes plus one python lane per
entry in PYTHONS. Interpreters that aren't installed are reported and
skipped, so `all` still works on a bare dev machine; naming one explicitly
(python:3.10) makes a missing interpreter an error instead.

Two lanes that resolve to the same interpreter are one job - `all
python:/my/venv/bin/python3.13` builds 3.13 once, from the venv, because a
later lane on the command line wins.
"""
import argparse
import concurrent.futures
import io
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# abort_on_error=1: some tests deliberately trigger a real SIGSEGV and assert
# the process dies via signal (WIFSIGNALED); ASan's default of exiting
# normally after reporting would fail that check.
SANITIZER_ENV = {
    "ASAN_OPTIONS": "detect_leaks=1,abort_on_error=1",
    "UBSAN_OPTIONS": "print_stacktrace=1,halt_on_error=1",
}

# src/ is gated at 100% line coverage; genuinely untestable lines are marked
# GCOVR_EXCL. examples/ and test/ aren't held to the same bar.
COVERAGE_GATE = [
    "gcovr", "--root", ".", "--filter", r"src/.*\.c$",
    "--fail-under-line", "100", "--print-summary", "build-coverage",
]

LANES = {
    "default": {
        "build_dir": "build",
        "setup_args": [],
    },
    "asan": {
        "build_dir": "build-asan",
        "setup_args": ["-Db_sanitize=address,undefined"],
        "env": SANITIZER_ENV,
    },
    "coverage": {
        "build_dir": "build-coverage",
        "setup_args": ["-Db_coverage=true"],
        "after": [COVERAGE_GATE],
    },
}

# Interpreters the bindings are supported on; the workflow matrix mirrors
# this list. 3.10 is the Py_LIMITED_API level the shim targets, so CI proves
# the abi3 floor we claim. No free-threaded entry: CPython rejects the
# limited API there outright.
PYTHONS = ["3.10", "3.13"]

ALL = [*LANES, *(f"python:{v}" for v in PYTHONS)]

# A python lane rebuilds only the shim against another interpreter; the C
# side is already covered by the lanes above.
PYTHON_TEST = "test_python_bindings"


def run(cmd, env=None, out=None):
    """Run cmd; stream to the terminal, or into `out` if lanes are parallel."""
    merged = {**os.environ, **(env or {})}
    echo = "+ " + " ".join(str(c) for c in cmd)
    if out is None:
        print(echo, flush=True)
        return subprocess.run(cmd, cwd=ROOT, env=merged).returncode
    proc = subprocess.run(cmd, cwd=ROOT, env=merged, text=True,
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    out.write(f"{echo}\n{proc.stdout}")
    return proc.returncode


def interpreter_tag(exe):
    """Return e.g. "3.12" for exe, or None if it won't run.
    """
    probe = ("import json,sys,sysconfig;"
             "print(json.dumps([sys.version_info[0], sys.version_info[1],"
             " bool(sysconfig.get_config_var('Py_GIL_DISABLED'))]))")
    try:
        out = subprocess.run([exe, "-c", probe], capture_output=True,
                             text=True, check=True).stdout
    except (OSError, subprocess.SubprocessError):
        return None
    major, minor, freethreaded = json.loads(out)
    # free-threaded not yet supported -- as of 3.14, incompatible with Python's
    # stable ABI
    assert not freethreaded
    return f"{major}.{minor}"


def resolve_interpreter(spec):
    """Map a python lane's spec to (executable, tag), or (None, None).

    A spec that looks like a path is used as-is; otherwise it's a version.
    """
    if os.sep in spec:
        exe = os.path.abspath(spec)
        tag = interpreter_tag(exe)
        return (exe, tag) if tag else (None, None)

    # python3.14t if it exists, else python3.14, else whatever python3 is -
    # which is what actions/setup-python repoints. The tag check decides.
    seen = dict.fromkeys([f"python{spec}", f"python{spec}",
                          "python3"])
    for candidate in seen:
        exe = shutil.which(candidate)
        if exe and interpreter_tag(exe) == spec:
            return exe, spec
    return None, None


def python_lane(spec):
    exe, tag = resolve_interpreter(spec)
    if exe is None:
        return None
    return {
        "build_dir": f"build-py{tag}",
        "setup_args": [f"-Dpython={exe}"],
        "test_args": [PYTHON_TEST],
    }


def run_lane(lane, jobs=None, out=None):
    build_dir = Path(lane["build_dir"])
    env = lane.get("env")
    j = [] if jobs is None else [str(jobs)]

    setup = ["meson", "setup", str(build_dir), *lane["setup_args"]]
    if (ROOT / build_dir / "meson-info").is_dir():
        setup.insert(2, "--reconfigure")

    steps = [
        setup,
        ["ninja", "-C", str(build_dir), *(["-j", *j] if j else [])],
        ["meson", "test", "-C", str(build_dir), *lane.get("test_args", []),
         "--print-errorlogs", *(["--num-processes", *j] if j else [])],
        *lane.get("after", []),
    ]
    for step in steps:
        rc = run(step, env, out)
        if rc != 0:
            return rc
    return 0


def clean():
    for path in sorted(ROOT.glob("build*")):
        # Only ever delete something meson actually configured.
        if (path / "meson-info").is_dir():
            print(f"removing {path.name}/", flush=True)
            shutil.rmtree(path)


def expand(names):
    """Requested lane names, with `all` expanded, in order, deduplicated."""
    out = []
    for name in names:
        for expanded in (ALL if name == "all" else [name]):
            if expanded not in out:
                out.append(expanded)
    return out


def plan(names, explicit):
    """Resolve every lane before anything runs.

    Returns (do_clean, jobs, skipped, missing). Jobs are (name, lane) pairs
    keyed on the build dir, so two lanes naming the same interpreter collapse
    into one and the later one on the command line wins. `missing` are
    explicitly named interpreters that aren't installed, `skipped` are ones
    only `all` asked for.
    """
    do_clean = False
    jobs, skipped, missing = {}, [], []
    for name in names:
        if name == "clean":
            do_clean = True
        elif name in LANES:
            jobs[LANES[name]["build_dir"]] = (name, LANES[name])
        elif (lane := python_lane(name.split(":", 1)[1])) is not None:
            jobs[lane["build_dir"]] = (name, lane)
        elif name in explicit:
            missing.append(name)
        else:
            skipped.append(name)
    return do_clean, list(jobs.values()), skipped, missing


def execute(jobs, lane_jobs, inner_jobs):
    """Run the planned jobs, each under its own banner. Returns failed names."""
    def banner(name):
        return f"\n{'=' * 70}\n== {name}\n{'=' * 70}"

    if lane_jobs == 1:
        failed = []
        for name, lane in jobs:
            print(banner(name), flush=True)
            if run_lane(lane, inner_jobs) != 0:
                failed.append(name)
        return failed

    # Parallel lanes would interleave unreadably, so each buffers its own
    # output and gets printed whole, still in plan order.
    buffers = [io.StringIO() for _ in jobs]
    with concurrent.futures.ThreadPoolExecutor(max_workers=lane_jobs) as pool:
        futures = [pool.submit(run_lane, lane, inner_jobs, buf)
                   for (_, lane), buf in zip(jobs, buffers)]
        failed = []
        for (name, _), future, buf in zip(jobs, futures, buffers):
            rc = future.result()
            print(banner(name))
            print(buf.getvalue(), end="", flush=True)
            if rc != 0:
                failed.append(name)
    return failed


def main():
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("lanes", nargs="+", metavar="LANE",
                        help=f"one or more of: {', '.join(LANES)}, "
                             "python:VERSION, python:PATH, all, clean")
    parser.add_argument("-j", "--jobs", type=int, metavar="N",
                        help="compile/test jobs within a lane (default: all "
                             "cores, split across lanes when -L is used)")
    parser.add_argument("-L", "--lanes-at-once", type=int, default=1,
                        metavar="N",
                        help="lanes to run concurrently (default 1); their "
                             "output is buffered and printed per lane")
    args = parser.parse_args()

    names = expand(args.lanes)
    for name in names:
        if name not in LANES and name != "clean" \
                and not name.startswith("python:"):
            parser.error(f"unknown lane {name!r}")
    if args.lanes_at_once < 1 or (args.jobs is not None and args.jobs < 1):
        parser.error("-j and -L must be at least 1")

    do_clean, jobs, skipped, missing = plan(names, set(args.lanes))
    if missing:
        parser.error(
            "no interpreter found for " + ", ".join(missing)
            + "; install it, give a full path (python:/path/to/python3), "
              "or drop it and let `all` skip it")

    lane_jobs = min(args.lanes_at_once, len(jobs)) or 1
    # Keep the total near one job per core rather than lanes x cores.
    inner_jobs = args.jobs
    if inner_jobs is None and lane_jobs > 1:
        inner_jobs = max(1, (os.cpu_count() or 1) // lane_jobs)

    lines = ["plan:"]
    if do_clean:
        lines.append("  clean (first)")
    for name, lane in jobs:
        lines.append(f"  {name} -> {lane['build_dir']}")
    lines += [f"  {n}: skipped, interpreter not installed" for n in skipped]
    if jobs:
        lines.append(f"parallelism: {lane_jobs} lane(s) at a time, "
                     f"{inner_jobs or 'default'} job(s) each")
    print("\n".join(lines), flush=True)

    if do_clean:
        print(f"\n{'=' * 70}\n== clean\n{'=' * 70}", flush=True)
        clean()

    # Like the workflow's fail-fast: false -- run everything, report at the end.
    failed = execute(jobs, lane_jobs, inner_jobs)

    print(f"\n{'=' * 70}\nsummary: {len(jobs) - len(failed)}/{len(jobs)} ok"
          + (f", {len(skipped)} skipped" if skipped else ""))
    for name, _ in jobs:
        print(f"  {'FAIL' if name in failed else 'ok  '}  {name}")
    for name in skipped:
        print(f"  skip  {name}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())

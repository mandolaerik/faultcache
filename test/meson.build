# © 2026 Erik Carstensen
# SPDX-License-Identifier: MPL-2.0

test_names = ['basic']

if is_windows
  test_names += ['multi-chunk', 'lru']
else
  test_names += ['multi-chunk', 'lru', 'boundary', 'readonly', 'unmap', 'misuse',
                 'client-server', 'client-server-shared',
                 'client-server-destroy', 'client-server-reject',
                 'client-server-boundary', 'client-server-shared-boundary']
endif

foreach t : test_names
  exe = executable(t, t + '.c',
    dependencies: [faultcache_dep, thread_dep])
  test(t, exe, timeout: 30)
endforeach

# The Python tests run under pytest, and it is not optional: without it
# they would silently not run at all, which is the failure mode the
# suite exists to prevent. Checked here rather than skipped over.
system_py = find_program('python3', required: false)
if not is_windows
  if not system_py.found()
    error('the Python tests need python3')
  endif
  if run_command(system_py, '-c', 'import pytest',
                 check: false).returncode() != 0
    error('the Python tests need pytest in ' + system_py.full_path())
  endif
endif
pytest_args = ['-m', 'pytest', '-q', '-p', 'no:cacheprovider']
python_path_separator = ':'
if is_windows
  python_path_separator = ';'
endif

# nm ships with the binutils that provide the assembler and linker, so a
# build that got this far has it.
#
# Not under b_coverage: gcov links its runtime into the library and that
# runtime exports its own __gcov_* symbols, so the dynamic symbol table
# there is not the one we ship. The ABI surface is a property of an
# ordinary build, and every lane builds one.
if not is_windows and not get_option('b_coverage')
  test('test_exported_symbols', system_py,
    args: pytest_args + [files('test_exported_symbols.py')],
    env: {
      'NM': find_program('nm').full_path(),
      'FAULTCACHE_LIBRARY': faultcache_lib.full_path(),
    },
    depends: faultcache_lib,
    timeout: 30)
endif

# ctypes-loading a sanitizer-instrumented libfaultcache.so into the plain
# (non-instrumented) python3 interpreter via dlopen doesn't work without
# LD_PRELOAD-ing libasan first, so this test is skipped under b_sanitize.
# The bindings require the compiled callback shim (see python/meson.build),
# so this also skips when Python development headers are unavailable.
if py.found() and faultcache_shim.found() and get_option('b_sanitize') == 'none'
  if run_command(py, '-c', 'import pytest', check: false).returncode() != 0
    error('the Python bindings test needs pytest in ' + py.full_path())
  endif
  test('test_python_bindings', py,
    args: pytest_args + [files('test_python_bindings.py')],
    env: {
      'FAULTCACHE_LIBRARY': faultcache_lib.full_path(),
      'PYTHONPATH': meson.project_source_root() / 'python' +
            python_path_separator + shim_build_dir,
    },
    depends: faultcache_shim,
    timeout: 30)
else
  message('Skipping Python bindings test: no Python/headers, or a sanitizer build')
endif

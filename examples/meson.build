# © 2026 Erik Carstensen
# SPDX-License-Identifier: MPL-2.0

zlib_dep = dependency('zlib', required: false)
python3 = find_program('python3', required: false)

if zlib_dep.found() and python3.found()
  fixture = custom_target('compressed_fixture',
    output: 'fixture.bin',
    command: [python3, files('../scripts/gen_compressed_fixture.py'), '@OUTPUT@'],
    build_by_default: true)

  gzip_chunks_example = executable('gzip_chunks_example',
    'compressed_chunks/main.c',
    dependencies: [faultcache_dep, zlib_dep])

  test('gzip_chunks_example', gzip_chunks_example, args: [fixture], timeout: 30)
else
  message('Skipping compressed-file example: zlib or python3 not found')
endif

# Checks the parquet example against files written by pyarrow. Opt-in, so
# that whether it runs is decided by whoever configures the build rather
# than by what happens to be installed -- see scripts/ci.py's parquet
# lane. A missing pyarrow is then a failure, not a silent skip.
if get_option('pyarrow')
  if not (py.found() and faultcache_shim.found())
    error('-Dpyarrow=true needs the Python bindings, which are not enabled')
  endif
  if run_command(py, '-c', 'import pytest', check: false).returncode() != 0
    error('-Dpyarrow=true needs pytest in ' + py.full_path())
  endif
  # Lives in test/ so it is an ordinary pytest module; only the decision
  # to run it belongs to the example.
  test('parquet_column', py,
    args: pytest_args + [files('../test/test_parquet_example.py')],
    env: {
      'FAULTCACHE_LIBRARY': faultcache_lib.full_path(),
      'PYTHONPATH': meson.project_source_root() / 'python' + ':' + shim_build_dir,
    },
    depends: faultcache_shim,
    timeout: 120)
endif

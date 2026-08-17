# © 2026 Erik Carstensen
# SPDX-License-Identifier: MPL-2.0

test_names = ['test-basic', 'test-multi-chunk', 'test-boundary',
              'test-readonly', 'test-unmap', 'test-client-server',
              'test-client-server-shared', 'test-client-server-destroy',
              'test-client-server-reject']

foreach t : test_names
  exe = executable(t, t + '.c',
    dependencies: [faultcache_dep, thread_dep])
  test(t, exe, timeout: 30)
endforeach

python3 = find_program('python3', required: false)
# ctypes-loading a sanitizer-instrumented libfaultcache.so into the plain
# (non-instrumented) python3 interpreter via dlopen doesn't work without
# LD_PRELOAD-ing libasan first, so this test is skipped under b_sanitize.
if python3.found() and get_option('b_sanitize') == 'none'
  test('test_python_bindings', python3,
    args: [files('test_python_bindings.py')],
    env: {
      'FAULTCACHE_LIBRARY': faultcache_lib.full_path(),
      'PYTHONPATH': meson.project_source_root() / 'python',
    },
    timeout: 30)
else
  message('Skipping Python bindings test: python3 not found, or a sanitizer build')
endif

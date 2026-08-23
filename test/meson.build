# © 2026 Erik Carstensen
# SPDX-License-Identifier: MPL-2.0

test_names = ['basic', 'multi-chunk', 'boundary',
              'readonly', 'unmap', 'lru', 'client-server',
              'client-server-shared', 'client-server-destroy',
              'client-server-reject', 'client-server-boundary',
              'client-server-shared-boundary', 'misuse']

foreach t : test_names
  exe = executable(t, t + '.c',
    dependencies: [faultcache_dep, thread_dep])
  test(t, exe, timeout: 30)
endforeach

# ctypes-loading a sanitizer-instrumented libfaultcache.so into the plain
# (non-instrumented) python3 interpreter via dlopen doesn't work without
# LD_PRELOAD-ing libasan first, so this test is skipped under b_sanitize.
# The bindings require the compiled callback shim (see python/meson.build),
# so this also skips when Python development headers are unavailable.
if py.found() and faultcache_shim.found() and get_option('b_sanitize') == 'none'
  test('test_python_bindings', py,
    args: [files('test_python_bindings.py')],
    env: {
      'FAULTCACHE_LIBRARY': faultcache_lib.full_path(),
      'PYTHONPATH': meson.project_source_root() / 'python' + ':' + shim_build_dir,
    },
    depends: faultcache_shim,
    timeout: 30)
else
  message('Skipping Python bindings test: no Python/headers, or a sanitizer build')
endif

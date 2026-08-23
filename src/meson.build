# © 2026 Erik Carstensen
# SPDX-License-Identifier: MPL-2.0

lib_c_args = []
if get_option('b_coverage')
  # Lets internal.h call __gcov_dump() to flush counters right
  # before a signal-terminated death (gcov otherwise only writes .gcda at
  # normal exit) -- see fc_flush_coverage_before_death(). Compiled out
  # entirely otherwise, so non-coverage builds never reference libgcov.
  lib_c_args += ['-DFC_COVERAGE_BUILD']
endif

faultcache_lib = library('faultcache', 'client.c', 'server.c',
  'sigsegv.c', 'misuse.c',
  include_directories: inc,
  dependencies: [thread_dep],
  c_args: lib_c_args,
  install: true)

faultcache_dep = declare_dependency(
  link_with: faultcache_lib,
  include_directories: inc)

install_headers('../include/faultcache/faultcache.h',
  '../include/faultcache/faultcache-client.h',
  '../include/faultcache/faultcache-server.h',
  subdir: 'faultcache')

pkg = import('pkgconfig')
pkg.generate(faultcache_lib,
  name: 'faultcache',
  filebase: 'faultcache',
  description: 'Transparent lazy-loading memory cache for cheaply-derived content')

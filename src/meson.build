# © 2026 Erik Carstensen
# SPDX-License-Identifier: MPL-2.0

faultcache_lib = library('faultcache', 'faultcache.c', 'faultcache-server.c',
  'faultcache-sigsegv.c', 'faultcache-misuse.c',
  include_directories: inc,
  dependencies: [thread_dep],
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

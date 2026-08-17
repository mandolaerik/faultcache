# © 2026 Erik Carstensen
# SPDX-License-Identifier: MPL-2.0

faultcache_lib = library('faultcache', 'faultcache.c', 'faultcache-server.c',
  include_directories: inc,
  dependencies: [thread_dep],
  install: true)

faultcache_dep = declare_dependency(
  link_with: faultcache_lib,
  include_directories: inc)

install_headers('../include/faultcache/faultcache.h',
  '../include/faultcache/faultcache-client-server.h',
  subdir: 'faultcache')

pkg = import('pkgconfig')
pkg.generate(faultcache_lib,
  name: 'faultcache',
  filebase: 'faultcache',
  description: 'Userfaultfd-backed transparent cache for cheaply-derived memory content')

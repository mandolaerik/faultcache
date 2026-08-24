# © 2026 Erik Carstensen
# SPDX-License-Identifier: MPL-2.0

lib_c_args = ['-DFC_BUILDING_LIB']
if get_option('b_coverage')
  # Lets internal.h call __gcov_dump() to flush counters right
  # before a signal-terminated death (gcov otherwise only writes .gcda at
  # normal exit) -- see fc_flush_coverage_before_death(). Compiled out
  # entirely otherwise, so non-coverage builds never reference libgcov.
  lib_c_args += ['-DFC_COVERAGE_BUILD']
endif

lib_sources = ['inproc.c', 'misuse.c']
lib_deps = [thread_dep]

if is_windows
  lib_sources += ['vm-win32.c', 'fault-win32.c']
  # mingw declares VirtualAlloc2/MapViewOfFile3 (memoryapi.h, gated on
  # NTDDI_WIN10_RS4) but ships no import library defining them, so
  # linking fails without one we generate ourselves.
  kernelbase_import = custom_target('kernelbase-vm',
    input: 'kernelbase-vm.def',
    output: 'libkernelbase-vm.a',
    command: [find_program('dlltool'), '--def', '@INPUT@',
              '--dllname', 'kernelbase.dll', '--output-lib', '@OUTPUT@'])
  lib_deps += declare_dependency(sources: kernelbase_import,
    link_args: ['-L' + meson.current_build_dir(), '-lkernelbase-vm'])
else
  lib_sources += ['vm-posix.c', 'fault-posix.c', 'client.c', 'server.c']
endif

faultcache_lib = library('faultcache', lib_sources,
  include_directories: inc,
  dependencies: lib_deps,
  c_args: lib_c_args,
  # Only FC_API-marked declarations get out; see faultcache.h.
  gnu_symbol_visibility: 'hidden',
  install: true)

faultcache_dep = declare_dependency(
  link_with: faultcache_lib,
  include_directories: inc)

install_headers('../include/faultcache/faultcache.h',
  '../include/faultcache/client.h',
  '../include/faultcache/server.h',
  subdir: 'faultcache')

pkg = import('pkgconfig')
pkg.generate(faultcache_lib,
  name: 'faultcache',
  filebase: 'faultcache',
  description: 'Transparent lazy-loading memory cache for cheaply-derived content')

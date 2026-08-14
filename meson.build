# © 2026 Erik Carstensen
# SPDX-License-Identifier: MPL-2.0

project('faultcache', 'c',
  version: '0.1.0',
  license: 'MPL-2.0',
  default_options: ['c_std=gnu11', 'warning_level=3'])

if host_machine.system() != 'linux'
  error('faultcache relies on Linux userfaultfd(2) and only builds on Linux')
endif

thread_dep = dependency('threads')
inc = include_directories('include')

subdir('src')
subdir('test')
subdir('examples')

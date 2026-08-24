# © 2026 Erik Carstensen
# SPDX-License-Identifier: MPL-2.0

project('faultcache', 'c',
  version: '0.1.0',
  license: 'MPL-2.0',
  default_options: ['c_std=gnu2x', 'warning_level=3', 'werror=true'])

# The in-process backend runs everywhere; the client/server one is
# Linux-only, since userfaultfd(2) + SCM_RIGHTS has no Windows analogue
# (a server process cannot resolve faults in a client's address space).
is_windows = host_machine.system() == 'windows'
if not (is_windows or host_machine.system() == 'linux')
  error('faultcache has no fault-delivery backend for '
        + host_machine.system() + ' -- see src/fault.h')
endif

thread_dep = dependency('threads')
inc = include_directories('include')

subdir('src')
subdir('python')
subdir('test')
subdir('examples')

# © 2026 Erik Carstensen
# SPDX-License-Identifier: MPL-2.0

py_mod = import('python')
py = py_mod.find_installation('python3', required: false)

faultcache_shim = disabler()
shim_build_dir = meson.current_build_dir()

if py.found()
  py_dep = py.dependency(required: false)
  if py_dep.found()
    # abi3 at 3.10: everything the shim uses is in the stable ABI at that
    # level, so one binary covers every supported interpreter.
    faultcache_shim = py.extension_module('_faultcache',
      '_faultcache_shim.c',
      limited_api: '3.10',
      install: false)
  else
    message('Skipping Python callback shim: Python development headers not found')
  endif
else
  message('Skipping Python callback shim: no Python installation found')
endif

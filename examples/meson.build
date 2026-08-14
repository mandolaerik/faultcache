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

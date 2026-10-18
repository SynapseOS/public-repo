#!/usr/bin/env python3
"""Opt-in SQLite runtime/concurrency checks (QGA_SOCKET must select a test VM)."""
from pathlib import Path
import os, subprocess, sys, time
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tests'))
from guest_runtime import Guest
work = ROOT / 'pkgs/.work/sqlite'
subprocess.run([str(ROOT / 'apps/apps/common/musl-cc'), '-O2', '-I' + str(work / 'payload/usr/include'), str(ROOT / 'pkgs/sqlite/runtime-test.c'), str(work / 'payload/usr/lib/libsqlite3.a'), '-lm', '-o', str(work / 'runtime-test')], check=True)
g = Guest()
try:
    g.write('/tmp/sqlite-test', (work / 'runtime-test').read_bytes())
    g.write('/tmp/sqlite3', (work / 'payload/bin/sqlite3').read_bytes())
    g.run('/bin/chmod', ['755', '/tmp/sqlite-test', '/tmp/sqlite3'])
    print(g.run('/tmp/sqlite3', ['--version']).decode().strip())
    g.run('/bin/rm', ['-rf', '/tmp/sqlite-ready', '/var/lib/sqlite-runtime.db', '/var/lib/sqlite-runtime.db.lock', '/var/lib/sqlite-runtime.db-journal'])
    g.run('/tmp/sqlite-test', ['init', '/var/lib/sqlite-runtime.db'])
    print(g.run('/tmp/sqlite-test', ['verify', '/var/lib/sqlite-runtime.db']).decode().strip())
    pid = g.start('/tmp/sqlite-test', ['hold', '/var/lib/sqlite-runtime.db'])
    for i in range(40):
        status, out, err = g.result(g.start('/bin/cat', ['/tmp/sqlite-ready']))
        if status.get('exitcode') == 0 and out == b'ready':
            break
        time.sleep(.1)
    else:
        raise AssertionError('holder did not acquire the lock')
    print(g.run('/tmp/sqlite-test', ['busy', '/var/lib/sqlite-runtime.db']).decode().strip())
    status, out, err = g.result(pid)
    assert status.get('exitcode') == 0, (status, out, err)
    print(g.run('/tmp/sqlite-test', ['verify', '/var/lib/sqlite-runtime.db']).decode().strip())
    path = '/var/lib/sqlite-runtime.db'
    before = g.read(path)
    status, out, err = g.result(g.start('/tmp/sqlite-test', ['crash', path]))
    assert status.get('exitcode') == 71, (status, out, err)
    journal = g.read(path + '-journal')
    assert journal[:8] == bytes.fromhex('d9d505f920a163d7'), journal[:8]
    assert len(journal) > 512, len(journal)
    assert g.read(path) != before, 'dirty pages did not spill to the database'
    print(g.run('/tmp/sqlite-test', ['busy', path]).decode().strip())
    # This test has no surviving database users. Remove only its stale lock.
    g.run('/bin/rmdir', [path + '.lock'])
    print(g.run('/tmp/sqlite-test', ['verify', path]).decode().strip())
    after = g.read(path)
    differences = [(i, a, b) for i, (a, b) in enumerate(zip(before, after)) if a != b]
    assert after == before, ('hot-journal rollback did not restore database bytes',
                             len(before), len(after), differences[:16])
    print('Unclean process exit: hot journal, stale lock and recovery OK (not a power-cut test)')
finally:
    g.close()

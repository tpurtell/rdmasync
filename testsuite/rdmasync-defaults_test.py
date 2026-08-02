#!/usr/bin/env python3
"""Verify rdmasync's product name, remote command, and checksum defaults."""

import os
import re

from rsyncfns import (
    FROMDIR, SRCDIR, TODIR, TOOLDIR,
    assert_same, run_rsync, test_fail,
)


version = run_rsync('--version', capture_output=True).stdout
if not version.startswith('rdmasync  version '):
    test_fail(f"unexpected program name in --version output: {version.splitlines()[0]!r}")

FROMDIR.mkdir(parents=True, exist_ok=True)
TODIR.mkdir(parents=True, exist_ok=True)
source = FROMDIR / 'payload.bin'
source.write_bytes((b'rdmasync-defaults\0' * 8192) + b'end')

# Let lsh resolve the default remote program by name, just as ssh would.
os.environ['PATH'] = f'{TOOLDIR}{os.pathsep}{os.environ.get("PATH", "")}'
os.environ['RSYNC_RSH'] = str(SRCDIR / 'support' / 'lsh.sh')

remote_dest = TODIR / 'default-remote.bin'
proc = run_rsync('-a', '--no-rdma', '--debug=CMD2,NSTR', str(source),
                 f'lh:{remote_dest}', capture_output=True)
assert_same(source, remote_dest, label='default rdmasync remote command')
if not re.search(r'cmd\[\d+\]=rdmasync(?:\s|$)', proc.stdout):
    test_fail(f'default remote command was not rdmasync:\n{proc.stdout}')
if not re.search(r'checksum: none\b', proc.stdout):
    test_fail(f'default checksum was not none:\n{proc.stdout}')


def expect_automatic(label, *options):
    dest = TODIR / f'{label}.bin'
    proc = run_rsync('-a', '--no-rdma', '--debug=NSTR', *options,
                     str(source), str(dest), capture_output=True)
    assert_same(source, dest, label=label)
    match = re.search(r'checksum: (\S+)', proc.stdout)
    if not match or match.group(1) == 'none':
        test_fail(f'{label} did not restore checksum negotiation:\n{proc.stdout}')


expect_automatic('explicit-auto', '--checksum-choice=auto')
expect_automatic('explicit-checksum', '-c')
expect_automatic('delta-request', '--no-whole-file')

old_list = os.environ.get('RSYNC_CHECKSUM_LIST')
os.environ['RSYNC_CHECKSUM_LIST'] = 'md5 md4'
try:
    expect_automatic('environment-list')
finally:
    if old_list is None:
        os.environ.pop('RSYNC_CHECKSUM_LIST', None)
    else:
        os.environ['RSYNC_CHECKSUM_LIST'] = old_list

print('rdmasync-defaults: name, remote lookup, and checksum policy verified')

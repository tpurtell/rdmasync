#!/usr/bin/env python3
"""RDMA bulk-option, fallback, and source-I/O regression coverage.

The portable suite cannot assume that its runner owns RDMA hardware.  It
therefore forces a no-candidate negotiation to exercise automatic fallback
and required-mode failure deterministically.  Live verbs/QP tests belong in
the fabric integration harness.
"""

import os

from rsyncfns import (
    FROMDIR, RSYNC_PEER, SRCDIR, TODIR,
    assert_same, rmtree, run_rsync, test_fail,
)


def output(proc):
    return (proc.stdout or '') + (proc.stderr or '')


def expect_failure(needle, *args):
    proc = run_rsync(*args, check=False, capture_output=True)
    text = output(proc)
    if proc.returncode == 0:
        test_fail(f"expected failure for {' '.join(args)}")
    needles = (needle,) if isinstance(needle, str) else needle
    if not any(item.lower() in text.lower() for item in needles):
        test_fail(f"expected one of {needles!r} in failure output for {' '.join(args)}"
                  f"\n{text}")


rmtree(FROMDIR)
rmtree(TODIR)
FROMDIR.mkdir(parents=True)
TODIR.mkdir(parents=True)

# An odd size crosses several source windows and exercises an unaligned EOF.
source = FROMDIR / 'source.bin'
source.write_bytes(bytes((i * 17 + 3) & 0xff
                         for i in range(2 * 1024 * 1024 + 37)))

# Cached mode and its explicit read-window control.
cached = TODIR / 'cached.bin'
run_rsync('-a', '--cached', '--disk-read-size=64K', str(source), str(cached))
assert_same(source, cached, label='cached source I/O')

# mmap may be absent on a niche target.  When present, it must copy exactly.
mapped = TODIR / 'mapped.bin'
proc = run_rsync('-a', '--mapped', str(source), str(mapped),
                 check=False, capture_output=True)
if proc.returncode == 0:
    assert_same(source, mapped, label='mapped source I/O')
elif 'not supported by this build' in output(proc):
    print('rdma-bulk: mmap source mode unavailable on this build')
else:
    test_fail(f"mapped source-I/O copy failed unexpectedly\n{output(proc)}")

# O_DIRECT is likewise platform/filesystem-dependent.  Unsupported builds and
# filesystems must reject it explicitly; a successful copy must be exact.
uncached = TODIR / 'uncached.bin'
proc = run_rsync('-a', '--uncached', '--disk-read-size=1M',
                 str(source), str(uncached), check=False, capture_output=True)
if proc.returncode == 0:
    assert_same(source, uncached, label='uncached source I/O')
else:
    direct_error = output(proc)
    allowed = ('not supported by this build', 'invalid argument',
               'operation not supported', 'not supported')
    if not any(msg in direct_error.lower() for msg in allowed):
        test_fail(f"uncached source-I/O copy failed unexpectedly\n{direct_error}")
    print('rdma-bulk: O_DIRECT source mode unavailable on this filesystem/build')

# Synthetic input accepts archive mode for one regular-file placeholder and
# produces the documented deterministic counter stream without reading it.
placeholder = FROMDIR / 'placeholder'
placeholder.write_bytes(b'not the synthetic payload')
synthetic_size = 1024 * 1024 + 13
synthetic = TODIR / 'synthetic.bin'
run_rsync('-a', f'--synthetic-file-data={synthetic_size}',
          str(placeholder), str(synthetic))
synthetic_data = synthetic.read_bytes()
expected = bytes(i & 0xff for i in range(synthetic_size))
if synthetic_data != expected:
    test_fail('synthetic counter stream has the wrong size or content')

directory = FROMDIR / 'directory'
directory.mkdir()
expect_failure('not a regular file', '-a', '--synthetic-file-data=1M',
               str(directory), str(TODIR / 'bad-directory'))
expect_failure('conflicts', '--synthetic-file-data=1M', '--mapped',
               str(placeholder), str(TODIR / 'bad-combination'))
expect_failure('requires --synthetic-file-data', '--rdma-discard',
               str(placeholder), str(TODIR / 'bad-discard'))

# Bounds and enums are rejected during option parsing, before negotiation.
expect_failure('--rdma mode must be', '--rdma=maybe', str(source),
               str(TODIR / 'bad-rdma-mode'))
expect_failure('--rdma-rails must be', '--rdma-rails=3', str(source),
               str(TODIR / 'bad-rails'))
expect_failure('rdma-chunk-size', '--rdma-chunk-size=1K', str(source),
               str(TODIR / 'bad-chunk'))
expect_failure('between 2 and 4096', '--rdma-queue-depth=1', str(source),
               str(TODIR / 'bad-depth'))
expect_failure('between 0 and 65535', '--rdma-port=65536', str(source),
               str(TODIR / 'bad-port'))
expect_failure('mutually exclusive', '--cached', '--mapped', str(source),
               str(TODIR / 'bad-source-modes'))

# Force an empty client-side candidate set even on an RDMA-equipped runner.
# Auto mode must warn and finish over the ordinary rsync/remote-shell stream;
# required mode must stop instead.  lsh is the suite's SSH stand-in.
os.environ['RSYNC_RSH'] = str(SRCDIR / 'support' / 'lsh.sh')
fallback = TODIR / 'fallback.bin'
proc = run_rsync('-a', '--rdma=auto',
                 '--rdma-device=rdmasync-no-such-device',
                 f'--rsync-path={RSYNC_PEER}', str(source), f'lh:{fallback}',
                 capture_output=True)
assert_same(source, fallback, label='automatic SSH fallback')
fallback_output = output(proc)
if ('RDMA unavailable' not in fallback_output
        or 'using rsync-over-SSH' not in fallback_output):
    test_fail(f"automatic fallback did not emit its warning\n{fallback_output}")

expect_failure(('RDMA required but unavailable',
                '--rdma=required was specified'),
               '-a', '--rdma=required',
               '--rdma-device=rdmasync-no-such-device',
               f'--rsync-path={RSYNC_PEER}', str(source),
               f'lh:{TODIR / "required.bin"}')

# Discard is propagated to whichever side receives.  Neither a push nor a
# pull may create or replace the destination named on that receiver.
discard_push = TODIR / 'discard-push.bin'
discard_push.write_bytes(b'push sentinel')
run_rsync('-a', '--no-rdma', '--synthetic-file-data=1M', '--rdma-discard',
          f'--rsync-path={RSYNC_PEER}', str(placeholder),
          f'lh:{discard_push}')
if discard_push.read_bytes() != b'push sentinel':
    test_fail('--rdma-discard push modified its named destination')

discard_pull = TODIR / 'discard-pull.bin'
discard_pull.write_bytes(b'pull sentinel')
run_rsync('-a', '--no-rdma', '--synthetic-file-data=1M', '--rdma-discard',
          f'--rsync-path={RSYNC_PEER}', f'lh:{placeholder}',
          str(discard_pull))
if discard_pull.read_bytes() != b'pull sentinel':
    test_fail('--rdma-discard pull modified its named destination')

print('rdma-bulk: source modes, synthetic/discard, validation, and fallback verified')

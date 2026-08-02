# RDMA failure and cleanup verification

## Purpose

This verification proves the transport boundary: setup failures before the
first diverted literal byte may fall back to SSH, while any failure after data
activation terminates the transfer.  It also checks that signals and SSH death
do not leave remote processes, benchmark destinations, or registered memory.

## Reproducible harness

`support/rdma-integration.sh` accepts a host and the new remote rsync path.  It
uses unique files under `/tmp`, verifies successful copies with SHA-256, and
removes its exact paths on exit.  The live commands were:

```sh
RDMASYNC_EXPECT_RAILS=2 support/rdma-integration.sh \
  ostrich /tmp/rdmasync-dev-20260802/rsync ./rsync

ssh -A ostrich 'cd /tmp/rdmasync-dev-20260802 && \
  RDMASYNC_EXPECT_RAILS=2 support/rdma-integration.sh \
  dodo /tmp/rdmasync-dev-20260802/rsync ./rsync'
```

Both invocations passed on 2026-08-02 at revision `6c7177bc` plus the test seam
and harness being verified.  The first uses the amd64 raptor binary as command
side and an arm64 ostrich peer; the second uses native arm64 binaries on
ostrich and dodo.

## Covered failures

| Stage | Injection or trigger | Required outcome | Result |
| --- | --- | --- | --- |
| enumeration | nonexistent command-side device filter | auto warns/falls back; required fails | pass |
| listener | test-only environment on remote rsync path | auto warns, exact SSH copy | pass |
| TCP connect | test-only command-side failure | auto warns, exact SSH copy | pass |
| QP transition | fail after bootstrap identities | auto warns, exact SSH copy | pass |
| final ready | fail after the verbs probe | auto warns, exact SSH copy | pass |
| old peer | remote `/usr/bin/rsync` without capability | auto warns, exact SSH copy | pass |
| post-data | fail sender after 4 MiB diverted | nonzero; no SSH fallback; no destination | pass |
| SSH death | terminate the transfer's SSH child after activation | nonzero; remote exits; no destination | pass |
| SIGINT | interrupt a 1 TiB synthetic/discard run after activation | nonzero; remote exits; no destination | pass |

The environment variables `RSYNC_TEST_RDMA_FAIL` and
`RSYNC_TEST_RDMA_FAIL_AFTER_BYTES` are internal test seams, not command-line
features and not sent in normal remote arguments.  They make bootstrap and
post-activation errors deterministic without changing an interface users can
accidentally select.

Compressed transfers are separately covered by the portable regression:
automatic mode reports its reason and uses SSH, while required mode fails
before transfer.  `--rdma-no-config` is also verified not to suppress fallback
warnings.  An unmodified local `/usr/bin/rsync` successfully drives the new
remote binary, proving the reverse old/new direction does not activate RDMA or
require an unknown option.

## Cleanup evidence

After both live harness runs, all three hosts reported:

```text
ps -C rsync -o args= | grep rdmasync-integration   # no output
rdma resource show mr                              # no output
```

Only the hosts' pre-existing kernel GSI QPs and CQs remained.  No integration
destination existed.  The harness waits up to five seconds for the remote
process after post-data failure, SSH death, and SIGINT; a survivor is a hard
test failure.

Normal bounded-memory evidence is recorded in the transport and source-I/O
reports: 16 MiB registered for one rail, 32 MiB for two, approximately 37–40
MiB process RSS at the selected configuration, and no allocation proportional
to file size.  Odd-chunk exact-content tests also exercise deferred receive
slot reposting at wraparound.

## Outcome

The tested implementation falls back only during setup.  Once literal data is
diverted, errors are visible and fatal.  Process exit lets the kernel reclaim
verbs resources even when cleanup is entered from a signal handler, and the
peer notices SSH/RDMA teardown and exits within the harness deadline.  This is
the intended fail-closed behavior: no plausible partial destination is
reported as a successful rsync transfer.

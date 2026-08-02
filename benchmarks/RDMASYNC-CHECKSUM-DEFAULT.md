# Rdmasync checksum default

## Purpose

Choose the application-level checksum default for high-bandwidth rdmasync
copies.  This is separate from the RDMA framing protocol: the RDMA data plane
never adds a digest or encryption layer.

## Evidence

The end-to-end measurements in `RDMA-END-TO-END.md` used identical 32 GiB
raptor-to-ostrich transfers:

| Rsync transfer checksum | Median throughput | Sender user CPU |
| --- | ---: | ---: |
| MD5 | 4.61 Gb/s | 53.76 s |
| none | 38.22 Gb/s | 1.16 s |

The MD5 run was CPU-bound well below the available RDMA and storage ceilings.
The `none` run remained bounded by the real source/destination path rather than
an implicit digest.

## Selected behavior

An ordinary rdmasync invocation now behaves as if
`--checksum-choice=none` were specified.  This disables transfer verification
and forces whole-file transfer, while the usual size/time quick check still
decides whether a file needs updating.

User intent overrides the default:

- `-c` enables pre-transfer checksum comparison and normal automatic checksum
  negotiation.
- `--checksum-choice=auto` restores upstream automatic negotiation.
- `--checksum-choice=ALGORITHM` selects that algorithm explicitly.
- `--no-whole-file`, batch mode, and a non-empty `RSYNC_CHECKSUM_LIST` retain
  checksum negotiation because their operation depends on or explicitly asks
  for it.
- `--append` and `--append-verify` retain checksum negotiation because append
  mode is incompatible with a forced whole-file transfer.

This is a rdmasync application default, not a wire-protocol change.  The
selected checksum choice is passed to the peer, and an explicit
`--rsync-path=rsync` remains available when interoperating with a standard
rsync installation.

## Verification

The renamed amd64 build passed the complete protocol 32, 30, and 29 suites:
106 passed with 9 expected platform skips in each configuration.  A native
arm64 build on ostrich reported `RDMA-bulk` and passed 103 tests with 12
expected platform skips.  The install/uninstall smoke test installed
`rdmasync` and `rdmasync.1`, installed no `rsync` executable, and left no files
after uninstall.

A 64 MiB raptor-to-ostrich transfer was then run with no checksum option.  Its
session output reported `Client checksum: none`, activated two RDMA rails, and
the independently calculated SHA-256 values matched.  The temporary source
and destination were removed after verification.

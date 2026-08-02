# RDMA end-to-end rsync behavior

## Purpose

This project measures complete rsync operations after the isolated fabric,
transport, source, and destination studies.  It covers a new ordinary file,
an unchanged large file, a partially matching file, and a representative build
tree.  These results retain normal rsync file selection, delta matching, temp
files, metadata, and requested transfer checksums.

## Test state and method

- Date: 2026-08-02 (Asia/Taipei)
- Revision: `eb31b47c`
- Raw results: `raw/2026-08-02-end-to-end.csv`
- Binaries: native amd64 raptor and native arm64 ostrich/dodo builds
- Transport defaults: automatic two rails, 2 MiB payload, depth 8, cached
  2 MiB source window
- Samples: five per scenario
- Normal checksum in these builds: rsync MD5 (the builds have neither OpenSSL
  crypto nor xxhash development support)
- Durable new-file cases: ext4 destination plus `--fsync`
- Delta and tree cases: tmpfs, to separate rsync semantics from NVMe

Independent `cmp` checks verified every 32 GiB raptor destination and every
8 GiB Spark destination.  The random partial-file source and destination were
verified with an independent SHA-256 after the final update.  Each tree was
checked with a checksum/itemized dry run that reported no differences.

## New whole files

The ordinary-file command used the measured source defaults and normal rsync
MD5 transfer verification:

```sh
./rsync -aW --rdma=required --rdma-rails=2 --rdma-show-config \
  --fsync --rsync-path=/tmp/rdmasync-dev-20260802/rsync \
  /home/tj/rdmasync-benchmark-32g.bin \
  ostrich:/home/tj/rdmasync-e2e-32g.bin
```

| Topology | Size | Checksum | Data median (range) Gb/s | Wall median (range) s | Sender user median s |
| --- | ---: | --- | ---: | ---: | ---: |
| raptor→ostrich | 32 GiB | MD5 | 4.61 (4.57–4.66) | 59.93 (59.37–60.46) | 53.76 |
| ostrich→dodo | 8 GiB | MD5 | 4.27 (4.22–4.27) | 16.57 (16.52–16.74) | 13.54 |
| raptor→ostrich | 32 GiB | explicit none | 38.22 (37.46–38.85) | 7.54 (7.42–7.71) | 1.16 |

The default-checksum result is CPU-bound in rsync's existing MD5 calculation,
not in RDMA: the raptor sender spends almost 54 CPU seconds hashing a transfer
that takes about 60 seconds.  Explicit `--checksum-choice=none` is 8.3 times
faster at the data median, after which cold source reads and durable destination
writes become the limit.  rdmasync does not silently make that choice: it adds
no checksum of its own and preserves the checksum policy selected by rsync and
the user.

These hosts have runtime `libcrypto` and `libxxhash` libraries but lack their
development headers on the Sparks, so the native arm64 build cannot negotiate
a faster implementation from this environment.  Installing matching build
dependencies and retesting the normal checksum is the clearest production
opportunity; changing file-integrity semantics inside the RDMA transport is
not.

## Unchanged and partially matching files

An unchanged 8 GiB file completes in a 0.47 s median (0.46–0.50 s) and sends
zero literal bytes.  RDMA setup still succeeds, but rsync's quick check avoids
source reads, destination writes, and bulk messages exactly as before.

For the delta case, a nonrepeating random 2 GiB source was copied to dodo and a
64 MiB region in the destination was independently overwritten before every
run.  Whole-file mode was disabled.  Rsync sent 67,187,200 literal bytes—3.13%
of the file—and represented the rest with existing match tokens on SSH.  Wall
time was 14.52 s median (14.41–14.68 s).  Its displayed 0.08 Gb/s RDMA rate is
not a fabric ceiling: the timer spans 2,049 small literal runs interleaved with
the single-process rolling/strong checksum scan.  The important outcome is
that the delta algorithm remains effective and only literals enter RDMA.

An all-zero file was deliberately rejected as a delta benchmark: rsync could
correct a changed region by reusing identical zero blocks from elsewhere and
therefore sent no literals.  That behavior was correct, but it did not test the
bulk data path.

## Representative tree

The arm64 build tree contained 571 files and 35.48 MB of literal content after
excluding `.git` and test scratch.  A new recursive archive transfer completed
in a 0.58 s wall median (0.56–0.60 s).  Its literal-data median was 1.74 Gb/s
across 572 RDMA messages.  At this size, file-list, SSH control, per-file
metadata, and checksum costs dominate; the result demonstrates correct normal
tree semantics rather than fabric saturation.

## Outcome

RDMA accelerates literal bytes without weakening rsync's quick check, delta
algorithm, checksum, metadata, or atomic destination behavior.  The tuned data
plane is capable of 167.71 Gb/s with explicit synthetic/discard isolation, but
complete transfers are bounded by whichever ordinary rsync component is
slowest: MD5 here, then source/destination storage, and for small trees or
deltas, control and per-file checksum work.  Users who knowingly do not need
the transfer digest can request `--checksum-choice=none`; rdmasync never does
so automatically.

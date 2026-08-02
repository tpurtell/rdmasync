# RDMA destination-I/O tuning

## Purpose

This project measures the receiver copy/write path and persistent destination
storage.  Synthetic input removes source reads, while a real destination file
and `--fsync` make the durable tests include the receiver's page-cache write
and final storage flush.  A separate tmpfs target exposes receiver CPU and
memory behavior without NVMe latency.

## Test state and method

- Date: 2026-08-02 (Asia/Taipei)
- Baseline revision: `996f8e5b`
- Payload: 16 GiB for NVMe, 8 GiB for tmpfs
- Transport: two rails, 2 MiB chunks, depth 8, synthetic source, explicit
  `--checksum-choice=none`
- Raw copy comparison: `raw/2026-08-02-destination-copy.csv`
- Raw write-buffer comparison: `raw/2026-08-02-destination-buffer.csv`
- Raw host/storage comparison: `raw/2026-08-02-destination-storage.csv`
- Raw direct-write baseline: `raw/2026-08-02-nvme-write.csv`
- Samples: five interleaved runs per comparison point

The persisted command shape was:

```sh
./rsync -aW --checksum-choice=none --rdma=required --rdma-rails=2 \
  --rdma-chunk-size=2M --rdma-queue-depth=8 \
  --synthetic-file-data=16G --fsync \
  --rsync-path=/tmp/rdmasync-dev-20260802/rsync \
  README.md ostrich:/home/tj/rdmasync-destination-16g.bin
```

Every result used a newly absent destination, checked its exact 16 GiB size,
and removed that exact benchmark path after the run.  Independent odd-size,
odd-chunk synthetic tests verified every counter byte after transfer.  Direct
write baselines used the same ext4 filesystems with synchronous 2 MiB
`O_DIRECT` writes.

## Receiver-copy result

The baseline token decoder copied each 32 KiB slice from its registered receive
slot into a static token buffer, after which rsync copied it again into its
write buffer.  The selected implementation returns a pointer into the receive
slot and defers reposting that slot until the next token request.  The pointer
therefore remains valid for rsync's digest and write-buffer copy, while receive
credits, ordering, and registered-memory bounds remain unchanged.

| Target | Baseline median (range) Gb/s | Direct-pointer median (range) Gb/s | Change |
| --- | ---: | ---: | ---: |
| ostrich tmpfs | 29.99 (24.57–31.04) | 30.43 (26.81–31.65) | +1.5% |
| ostrich NVMe + fsync | 12.16 (11.90–30.19) | 12.58 (12.31–28.31) | +3.5% |

On tmpfs the receiver is 97–98% of one CPU.  Median receiver user time falls
from 0.41 to 0.30 seconds (27%) because the explicit ring-to-token memcpy is
gone; system time remains dominated by tmpfs writes.  Peak RSS remains about
36–38 MiB.  Exact-content tests cover one and two rails, push and pull, and an
odd 4,160-byte RDMA chunk whose receive slot is reused at changing counter
phase.

The wide NVMe range is a measured property of the ostrich SSD under this run:
initial burst-cache results near 30 Gb/s fall to a stable 12–13 Gb/s after
repeated durable 16 GiB writes.  Runs were interleaved, and both variants see
the cliff.  The steady last-three medians are 11.93 and 12.45 Gb/s, consistent
with the smaller full-sample improvement; the burst result is not presented as
sustained storage performance.

## Rejected write-buffer change

The existing rsync destination buffer is 256 KiB.  Increasing only the RDMA
case to 2 MiB looked attractive because it matches a registered payload, but
the paired tmpfs sweep rejected it:

| Write buffer | Median Gb/s | Min–max Gb/s | Peak RSS range |
| --- | ---: | ---: | ---: |
| **256 KiB** | **34.29** | 31.65–34.57 | 36,824–36,928 KiB |
| 2 MiB | 27.92 | 24.56–34.53 | 38,648–38,688 KiB |

The larger buffer is 18.6% slower at the median and adds about 1.75 MiB RSS.
It was removed; RDMA retains rsync's historical 256 KiB write coalescing.

## Persistent-storage ceiling

Five direct 16 GiB writes establish these device/filesystem ceilings:

| Destination | Direct-write median | Min–max | rdmasync + fsync median | Efficiency |
| --- | ---: | ---: | ---: | ---: |
| raptor Samsung 9100 PRO | 84.32 Gb/s | 83.80–84.84 | 35.99 Gb/s | 42.7% |
| dodo ESL04TBTLCZ | 35.33 Gb/s | 34.79–40.19 | 29.43 Gb/s | 83.3% |

Raptor's non-fsync receiver reaches a 61.04 Gb/s median and runs at 97% of one
CPU; requiring persistence lowers the median to 35.99 Gb/s while the receiver
waits for dirty pages to flush.  Its NVMe is faster than the single receiver
process and buffered-write path.  Dodo's durable 29.43 Gb/s is instead close
to its 35.33 Gb/s direct-write ceiling, so destination storage is the primary
bottleneck there.  Both are far below the 160–196 Gb/s fabric ceilings, as a
real persisted single-file transfer must be on these devices.

## Outcome

The direct registered-slot pointer is retained because it removes one full
literal-data copy, measurably reduces receiver user CPU, preserves bounded
memory, and improves both memory-backed and sustained NVMe medians.  The
destination write buffer stays at 256 KiB.  No receiver direct-I/O mode is
selected: normal rsync temp-file, rename, checksum, sparse, and partial-file
semantics continue to own destination I/O, with `--fsync` remaining the
user-controlled durability boundary.

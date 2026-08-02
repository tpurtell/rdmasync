# RDMA queue and chunk tuning

## Purpose

This project chooses the default registered-slot size and queue depth for the
literal-data transport.  It isolates the data plane from source and destination
storage using deterministic synthetic input plus explicit receiver discard.
No extra digest or encryption is present.  `--checksum-choice=none` was chosen
by the benchmark command so rsync's optional transfer digest did not become the
CPU bottleneck; rdmasync never selects that checksum policy automatically.

## Test state and method

- Date: 2026-08-02 (Asia/Taipei)
- Revision: `e8647a6f`
- Raw parameter data: `raw/2026-08-02-transport.csv`
- Raw one/two-path comparison: `raw/2026-08-02-rail-scaling.csv`
- Fabric comparison: `RDMA-FABRIC-BASELINE.md`
- Binaries: native x86_64 build on raptor; native aarch64 builds on ostrich
  and dodo, all reporting `RDMA-bulk`
- Payload: 8 GiB deterministic counter stream
- Repetitions: five per point; tables report medians and full observed range
- Timer: starts at preparation of the first RDMA payload and ends after the
  final send completion, excluding SSH startup, QP setup, and destination I/O

Command shape on raptor was:

```sh
./rsync -a --checksum-choice=none --rdma=required --rdma-rails=2 \
  --rdma-chunk-size=2M --rdma-queue-depth=8 \
  --synthetic-file-data=8589934592 --rdma-discard --rdma-show-config \
  --rsync-path=/tmp/rdmasync-dev-20260802/rsync placeholder \
  ostrich:/tmp/rdmasync-discard-sentinel
```

The Spark-to-Spark command was launched on ostrich with the same binary path
on dodo.  The sentinel was checked independently; discard left it unchanged.
The counter stream was also sent to a real file and independently verified,
including an odd 4,160-byte chunk size that rotates the counter phase as ring
slots are reused.

Three implementation costs were removed before the final sweep:

1. New whole-file literals are accumulated to the negotiated RDMA chunk size
   instead of rsync's historical 32 KiB stream-I/O chunk.
2. Explicit discard consumes completions and reposts receive slots without a
   destination write, receiver copy, or an ignored receiver digest.
3. Synthetic registered slots are filled with the counter pattern once and
   reused when their counter phase repeats.  This preserves exact content and
   makes the test comparable to a perftest MR rather than timing a byte loop.

## Constant-32-MiB sweep

All rows below register 32 MiB of payload memory across two paths.  Headers add
less than 1 KiB at the selected depth.

| Chunk × depth/path | Raptor→ostrich median (range) Gb/s | Ostrich→dodo median (range) Gb/s |
| --- | ---: | ---: |
| 256 KiB × 64 | 150.28 (95.39–167.08) | 166.47 (166.33–169.45) |
| 1 MiB × 16 | 156.31 (57.06–161.50) | 166.69 (163.30–173.94) |
| **2 MiB × 8** | **163.31 (139.88–168.85)** | **167.71 (165.15–173.78)** |
| 4 MiB × 4 | 131.24 (98.01–174.15) | 160.61 (157.29–172.46) |
| 8 MiB × 2 | 151.16 (142.19–158.31) | 161.97 (157.87–171.78) |

The selected 2 MiB × 8 point has the best median on both topologies.  At this
revision it used the provisional 8 MiB source window; the separate source-I/O
sweep later selected a 2 MiB default.  The same 2 MiB chunk at depth 4 halves
registered memory to 16 MiB, but the Spark
median drops from 167.71 to 117.04 Gb/s because its control/credit window is
too short.  Additional depth beyond 8 did not justify more memory.

## Rail scaling and efficiency

At the selected point, ostrich→dodo produced:

| Active paths | Median Gb/s | Min–max Gb/s |
| ---: | ---: | ---: |
| 1 | 94.49 | 93.15–101.25 |
| 2 | 167.71 | 165.15–173.78 |

Dual paths improve the rdmasync median by 77.5%, closely tracking the raw
fabric's 79.4% one-to-two-path gain.  The dual transport reaches 85.5% of the
196.06 Gb/s Spark fabric median.  The remaining gap is dominated by the
single rsync sender/receiver process: a representative selected-point run used
0.56 s of sender CPU and 0.54 s of receiver CPU while each process lived for
about 0.6–1.2 s.  Both sides are effectively limited by one busy core, whereas
the raw dual-path baseline uses one perftest process per path.

The selected configuration reports 32 MiB registered and measured peak RSS of
37,532 KiB on the sender and 36,608 KiB on the receiver.  This matches the
bounded formula and leaves roughly 5 MiB for rsync, verbs objects, and normal
process state.  One path registers 16 MiB and is a first-class configuration,
not an error or degraded negotiation mode.

## Outcome

The defaults are now:

- RDMA chunk size: **2 MiB**
- queue depth: **8 slots per path**
- rails: **auto**, accepting one and selecting two when topology/rates justify
  it
- registered payload: **16 MiB for one path, 32 MiB for two**

These replace the provisional 256 KiB × 64 defaults without increasing memory.
On this fabric they substantially reduce message count while preserving enough
receive credit.  Further progress toward the raw 196 Gb/s Spark ceiling needs
parallel per-path CPU work or a lower-copy design; increasing the ring alone is
not supported by these measurements.

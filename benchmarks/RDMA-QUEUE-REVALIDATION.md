# RDMA queue revalidation after rdmapipe

## Purpose

Rdmapipe reached its throughput plateau with a shallower queue than rdmasync.
This follow-up asks whether that result justifies changing rdmasync's 2 MiB,
depth-8 default.  It reruns a compact chunk/depth sweep after all competing
rdmapipe traffic was stopped, then confirms the plausible alternatives on
both Spark-to-Spark and amd64-to-Spark paths.

The tools have different flow control.  Rdmapipe carries a pure byte stream
after bootstrap.  Rdmasync retains rsync's ordered literal protocol: every
RDMA message has a corresponding length integer on the SSH control stream,
and the receiver cannot consume the message until it has read that integer.
The sender flushes those control records at half the configured queue depth.
Consequently, rdmapipe's queue-depth result is useful evidence to recheck the
ring, but it is not directly transferable to rdmasync.

## Test state and method

- Date: 2026-08-02 (Asia/Taipei)
- Revision: `f987b63e`
- Raw samples: `raw/2026-08-02-queue-revalidation.csv`
- Binaries: native amd64 baseline on raptor and native arm64 baseline builds
  on ostrich and dodo
- Raptor: Linux 7.0.0-28, one 400 Gb/s `mlx5_0`, `10.55.0.12`
- Ostrich: Linux 6.17.0-1026-nvidia, two 200 Gb/s ports,
  `10.55.0.1` and `10.55.0.5`
- Dodo: same arm64/Spark topology, `10.55.0.2` and `10.55.0.6`
- Workload: deterministic synthetic input, receiver discard, explicit
  `--checksum-choice=none`, and two explicitly selected paths
- Payload: 4 GiB for exploration and 8 GiB for seven-run confirmation
- Timer: RDMA data interval only; it excludes SSH and QP setup

Raptor's single 400 Gb/s endpoint creates two independent RC QPs to the two
200 Gb/s Spark endpoints.  Both paths share raptor's local device and address;
this is intentional fan-out, not a requirement for two local adapters.

The command shape was:

```sh
rdmasync -aW --checksum-choice=none --rdma=required --rdma-rails=2 \
  --rdma-chunk-size=2M --rdma-queue-depth=8 \
  --synthetic-file-data=8G --rdma-discard --rdma-show-config \
  --rsync-path=/tmp/rdmasync-tune/rdmasync-baseline \
  placeholder ostrich:/tmp/rdmasync-discard-sentinel
```

The Spark-to-Spark runs used the same options with ostrich as the command
host and dodo as the peer.  Runs that overlapped other fabric testing were
discarded before this data set was started.

## Exploratory sweep

The clean 4 GiB Spark sweep first located the queue plateau:

| Chunk × depth/path | Registered payload, two paths | Median (range) Gb/s |
| --- | ---: | ---: |
| 1 MiB × 4 | 8 MiB | 52.53 (50.22–54.62) |
| 1 MiB × 8 | 16 MiB | 115.89 (104.53–121.56) |
| 2 MiB × 4 | 16 MiB | 91.83 (88.48–105.89) |
| 2 MiB × 6 | 24 MiB | 137.92 (127.30–142.24) |
| **2 MiB × 8** | **32 MiB** | **156.70 (154.68–157.88)** |
| 2 MiB × 10 | 40 MiB | 155.81 (155.50–155.83) |

Depth 8 is necessary for rdmasync: depth 4 loses 41%, depth 6 loses 12%, and
depth 10 adds memory without improving the median.  A follow-up 4 MiB sweep
put 4 MiB × depth 4 and × depth 6 near the same short-run plateau, so both
advanced to the longer confirmation.

## Cross-topology confirmation

Seven 8 GiB repetitions per point produced:

| Topology | Chunk × depth/path | Memory | Median (range) Gb/s |
| --- | ---: | ---: | ---: |
| Ostrich→dodo | **2 MiB × 8** | **32 MiB** | **166.85 (153.99–173.34)** |
| Ostrich→dodo | 4 MiB × 4 | 32 MiB | 165.33 (155.21–172.71) |
| Ostrich→dodo | 4 MiB × 6 | 48 MiB | 172.33 (165.89–173.15) |
| Raptor→ostrich | **2 MiB × 8** | **32 MiB** | **130.59 (65.43–157.36)** |
| Raptor→ostrich | 4 MiB × 4 | 32 MiB | 142.11 (106.84–170.98) |
| Raptor→ostrich | 4 MiB × 6 | 48 MiB | 102.14 (73.01–160.52) |

The amd64-to-Spark samples have substantial run-to-run variance, so their
small difference between the two 32 MiB configurations is not a basis for a
default change.  They do reject 4 MiB × depth 6 as a cross-topology setting:
it is 21.8% below the current default's median while registering 50% more
payload memory.  On Spark-to-Spark, that extra memory bought only 3.3%.

At equal memory, 4 MiB × depth 4 halves the message count but is 0.9% slower
than the current default on the stable Spark confirmation.  The original
five-run tuning also favored 2 MiB × depth 8 on both topologies.  There is no
repeatable cross-topology gain that warrants changing the message size.

## Final-frame slot audit

Rdmapipe also exposed a FIN/data slot-reuse race during its own testing, so
rdmasync's end-of-stream path was audited separately.  Rdmasync does not post
an RDMA FIN frame: EOF and final status remain on the ordered SSH protocol.
Its ring index advances only in `post_data_send()` after a nonempty literal
exists, every DATA WR is signaled, and RC send completions are ordered.  When
the ring is full, the completion therefore releases the exact slot selected
by `next_slot % depth`.  Cleanup drains all outstanding sends before freeing
registered memory.

A live stress check sent both 250 MiB exactly and 250 MiB + 37 bytes from
raptor to ostrich with queue depths 2 and 4, using both one and two paths.
SHA-256 verification passed all 22 transfers.  The live integration harness
now keeps an unaligned final frame and forces depth 2 so slot wrap followed by
EOF remains regression-covered.

## Outcome

The defaults remain:

- RDMA chunk size: **2 MiB**
- queue depth: **8 slots per path**
- registered payload: **16 MiB for one path, 32 MiB for two paths**
- rails: **auto**, with one configured rail fully supported

This is the smallest tested ring that consistently reaches rdmasync's
cross-topology plateau.  The gap to rdmapipe is not evidence of an undersized
ring: rdmasync additionally serializes literal lengths and other rsync state
over SSH, preserves delta-transfer semantics, and is limited by a single
rsync sender/receiver process.  Improvements there require changing the
control/data coupling or parallelizing work, not simply adopting rdmapipe's
queue depth.

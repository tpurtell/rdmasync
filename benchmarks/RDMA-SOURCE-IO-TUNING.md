# RDMA source-I/O tuning

## Purpose

This project chooses the ordinary-file source access mode and staging-window
default.  Unlike the synthetic transport sweep, every payload byte here is
read from a real, fully allocated 32 GiB file on the sender's NVMe filesystem.
The explicit discard receiver consumes the token stream without opening the
named destination, so the measurements isolate source storage, sender copies,
and the already-tuned RDMA transport.

## Test state and controls

- Date: 2026-08-02 (Asia/Taipei)
- Revision under test: `3044da5d`; all window sizes were explicit, and the
  selected default was applied after the sweep
- Raw mode data: `raw/2026-08-02-source-io.csv`
- Raw window data: `raw/2026-08-02-source-window.csv`
- Raw direct-read baseline: `raw/2026-08-02-nvme-read.csv`
- File: 32 GiB written with 8 MiB direct I/O, not sparse or tmpfs-backed
- Raptor source: Samsung SSD 9100 PRO 4TB, ext4 on `/dev/nvme0n1p2`
- Ostrich source: ESL04TBTLCZ-27J4-TYN 4TB, ext4 on `/dev/nvme0n1p2`
- Transport: two rails, 2 MiB RDMA payloads, depth 8 per rail
- Transfer: whole-file literal stream, explicit `--checksum-choice=none`,
  explicit `--rdma-discard`, five repetitions for access-mode comparisons

Before every cold run, `POSIX_FADV_DONTNEED` was applied only to the benchmark
file and zero resident pages were verified on raptor.  No system-wide cache
drop was used.  Warm runs first read the whole file into the page cache.
`--uncached` uses `O_DIRECT` and therefore has no warm state.  The access-mode
comparison used the then-provisional 8 MiB source window; a separate sweep
tested 1, 4, 8, 16, and 32 MiB with three cold repetitions, followed by five
confirming repetitions at 2 MiB.

The transfer command shape was:

```sh
./rsync -aW --checksum-choice=none --rdma=required --rdma-discard \
  --rdma-rails=2 --rdma-chunk-size=2M --rdma-queue-depth=8 \
  --cached --disk-read-size=2M \
  --rsync-path=/tmp/rdmasync-dev-20260802/rsync \
  /home/tj/rdmasync-benchmark-32g.bin \
  ostrich:/tmp/rdmasync-source-discard
```

The receiver path was confirmed absent before and after ordinary-source push
and pull tests.  Discard requires one regular-file source, rejects directories
and `--remove-source-files`, and is propagated to either remote role so those
restrictions are checked at the sender.

## Access-mode results

These medians and ranges use the provisional 8 MiB read window:

| Source state and mode | Raptor→ostrich Gb/s | Ostrich→dodo Gb/s |
| --- | ---: | ---: |
| warm cached | 110.28 (99.33–139.69) | 116.68 (112.14–139.02) |
| warm mapped | 164.18 (138.48–168.15) | 129.52 (117.55–131.13) |
| cold cached | 65.08 (62.91–66.14) | 46.00 (38.22–48.69) |
| cold mapped | 12.40 (12.39–12.41) | 50.91 (45.25–51.94) |
| direct uncached | 59.77 (59.61–62.52) | 43.18 (42.73–45.03) |

Mapped access has the highest warm result, but its cold behavior is not
portable: synchronous page faults collapse raptor to 12.40 Gb/s while the same
mode leads cold cached access on the Spark.  `O_DIRECT` is stable but loses to
normal cached access on both machines at the same window size.  Cached access
is therefore the safe default; mapped and uncached remain explicit controls
for known workloads and filesystems.

## Cached window sweep

| Read window | Raptor→ostrich median (range) Gb/s | Ostrich→dodo median (range) Gb/s |
| --- | ---: | ---: |
| 1 MiB | 70.65 (69.49–72.19) | 45.38 (45.08–47.36) |
| **2 MiB** | **70.20 (69.15–70.61)** | **47.85 (40.45–49.71)** |
| 4 MiB | 67.00 (64.55–67.42) | 47.36 (44.22–49.37) |
| 8 MiB | 64.74 (62.96–64.95) | 45.94 (45.65–46.00) |
| 16 MiB | 62.38 (56.43–64.60) | 44.49 (44.05–47.77) |
| 32 MiB | 59.72 (57.99–59.93) | 44.22 (44.20–44.52) |

One MiB narrowly leads on raptor, while 2 MiB leads on the Spark and is within
0.7% of raptor's peak.  The 2 MiB window also matches one RDMA payload.  Against
the provisional 8 MiB value, it improves the medians by 8.4% on raptor and
4.2% on the Spark.  Larger speculative reads add latency and memory traffic
without improving sequential throughput.

For comparison, uncached medians are nearly flat on raptor (59.98–61.35 Gb/s)
and rise with window size on the Spark (39.15 Gb/s at 1 MiB to 44.23 Gb/s at
32 MiB).  None exceeds cached 2 MiB on its host, and selecting 32 MiB globally
would waste staging memory for the common cached path.

## Storage ceiling, CPU, and memory

A five-run synchronous `O_DIRECT` read of the same file with 2 MiB blocks took
a 3.37 s median on raptor and 5.35 s on ostrich: 81.57 and 51.38 Gb/s in decimal
network units.  The selected rdmasync medians reach 86.1% and 93.1% of those
host-specific read ceilings.  This identifies source storage plus sender copy
work—not the 160–196 Gb/s raw fabric—as the cold-file bottleneck.

Representative selected-point sender measurements were:

| Sender | Data Gb/s | Wall s | User s | System s | CPU | Peak RSS |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| raptor | 68.76 | 4.36 | 0.90 | 2.72 | 83% | 39,488 KiB |
| ostrich | 42.10 | 7.39 | 0.94 | 3.18 | 55% | 39,388 KiB |

The roughly 39 MiB RSS includes the 32 MiB registered two-rail ring and a
bounded 2 MiB cached staging window.  Windowed mmap promptly unmaps its old
window; uncached mode reuses one aligned staging allocation.  None maps or
allocates in proportion to the 32 GiB file size.

## Outcome

The ordinary source defaults are now `--cached --disk-read-size=2M` on both
amd64 and arm64.  The explicit `--mapped` and `--uncached` modes remain useful
for deliberately warm data or host-specific direct-I/O policies, but the data
does not support silently choosing either one.  Cold end-to-end file transfer
will be storage-limited on these hosts well before the dual-rail network limit;
warm-cache transfer can approach the transport ceiling.

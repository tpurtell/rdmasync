# RDMA fabric baseline

## Purpose

This measurement establishes the network ceiling independently of rsync and
the new bulk protocol.  It also tests the premise that a dual-rail Spark must
use both configured 200 Gb/s paths.  The results are the comparison point for
transport tuning, not an rdmasync performance claim.

## Test state

- Date: 2026-08-02 (Asia/Taipei)
- Repository revision used to record the run: `e8647a6f`
- Raw results: `raw/2026-08-02-fabric.csv`
- Tool: distro `ib_write_bw`, RC, one QP per path, 1 MiB messages,
  128-deep transmit queue, duration mode, five measured runs
- Raptor: x86_64, Linux 7.0.0-28-generic, `mlx5_0`, firmware 28.43.4100,
  one active Ethernet/RoCE port at 400 Gb/s, IPv4 RoCE-v2 GID index 4
- Ostrich/dodo: aarch64, Linux 6.17.0-1026-nvidia, mlx5 firmware 28.45.4028,
  two active Ethernet/RoCE devices at 200 Gb/s each, IPv4 RoCE-v2 GID index 3

The single-path command shape was:

```sh
# dodo server
ib_write_bw -d rocep1s0f0 -x 3 -p 19315 -D 3 -s 1048576 \
  -t 128 -q 1 -F --report_gbits --cpu_util

# ostrich client
ib_write_bw 10.55.0.2 -d rocep1s0f0 -x 3 -p 19315 -D 3 \
  -s 1048576 -t 128 -q 1 -F --report_gbits --cpu_util \
  --bind_source_ip=10.55.0.1
```

Dual mode ran the equivalent command concurrently on `rocep1s0f0`
(`10.55.0.1` to `.2`) and `roceP2p1s0f0` (`10.55.0.5` to `.6`).  The raptor
test ran two client processes on `mlx5_0` toward the two ostrich devices.

## Results

| Topology | Paths | Median Gb/s | Min–max Gb/s | Client CPU reported by perftest |
| --- | ---: | ---: | ---: | ---: |
| ostrich → dodo | 1 | 109.28 | 109.27–109.30 | 5.03–5.10% |
| ostrich → dodo | 2 | 196.06 | 196.06–196.07 | 10.01–10.04% per process |
| raptor → ostrich | 2 | 159.74 | 151.85–162.98 | 3.33–3.56% per process |

The Spark pair is exceptionally stable.  Its two-path result is a 79.4%
increase over one path and reaches the expected approximately 194–200 Gb/s
aggregate range.  A one-rail Spark is still fully usable; it simply has the
measured approximately 109 Gb/s raw ceiling.

Raptor's two flows were consistently imbalanced and its median aggregate
ceiling was 159.74 Gb/s despite the local port advertising 400 Gb/s.  Because
this is reproduced by the independent verbs benchmark, a 200 Gb/s raptor
rdmasync claim would not be honest on this run.  The remaining issue is below
rdmasync—likely fabric path, switch, or NIC flow allocation—and should be
investigated separately from the file-transfer protocol.

## Outcome

Automatic mode must retain dual-path selection whenever both Spark paths are
available, while accepting one candidate without error.  Performance results
must be judged against the topology-specific ceilings above: 196.06 Gb/s for
Spark-to-Spark and 159.74 Gb/s for this raptor-to-Spark run.

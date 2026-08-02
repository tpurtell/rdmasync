# RDMA benchmark and verification index

All measurements were recorded on 2026-08-02.  Reports contain commands,
topology, sample counts, medians/ranges, CPU and memory evidence, rejected
alternatives, and links to their raw CSV data.

1. `RDMA-FABRIC-BASELINE.md` — independent one/two-rail `ib_write_bw`
   ceilings; validates a one-rail Spark and dual-rail aggregation.
2. `RDMA-QUEUE-AND-CHUNK-TUNING.md` — synthetic/discard chunk, depth, memory,
   CPU, and rail sweep; selects 2 MiB × depth 8.
3. `RDMA-SOURCE-IO-TUNING.md` — real NVMe cached, mmap, and `O_DIRECT` reads;
   selects cached 2 MiB windows on amd64 and arm64.
4. `RDMA-DESTINATION-IO-TUNING.md` — receiver-copy and durable NVMe results;
   removes one receive copy and rejects a larger write buffer.
5. `RDMA-END-TO-END.md` — new, unchanged, partially matching, and tree
   transfers with normal rsync semantics and checksum bottleneck analysis.
6. `RDMA-FAILURE-AND-CLEANUP.md` — setup fallback, post-data failure, SSH
   death, SIGINT, process, MR, one-rail, and old-peer evidence.

Headline ceilings and selected defaults:

| Measurement | Result |
| --- | ---: |
| Spark raw dual-rail fabric | 196.06 Gb/s median |
| Spark rdmasync synthetic/discard | 167.71 Gb/s median |
| Spark rdmasync one rail | 94.49 Gb/s median |
| Raptor raw two-flow fabric | 159.74 Gb/s median |
| registered payload memory | 16 MiB one rail; 32 MiB two rails |
| defaults | auto rails, 2 MiB chunk, depth 8, cached 2 MiB reads |

Synthetic/discard and source/destination isolation use the benchmark command's
explicit `--checksum-choice=none`; rdmasync never selects it automatically.
The end-to-end report separately measures the normal rsync MD5 checksum and
shows its much lower CPU-bound rate on the available builds.

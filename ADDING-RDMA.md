# Adding RDMA to rsync

## Purpose

`rdmasync` keeps rsync's command line, file-selection rules, delta algorithm,
metadata handling, checksums, and SSH control connection.  When both ends of
an SSH transfer can reach each other over an RDMA fabric, literal file data is
automatically moved on an unencrypted libibverbs data plane.  If negotiation
or setup fails before any literal data is diverted, the transfer continues on
the ordinary rsync-over-SSH path and emits a warning.

The target topology is:

| Host class | Architecture | Active RoCE paths | Addressing |
| --- | --- | --- | --- |
| `raptor.200gb` (development host) | amd64 | one 400 Gb/s adapter | `10.55.0.12` |
| `ostrich`, `dodo`, `emu`, `kiwi` | arm64 | two 200 Gb/s adapters | rail 0: `10.55.0.1`-`.4`; rail 1: `10.55.0.5`-`.8` |

The initial representative test pair is `ostrich` and `dodo`.  Testing covers
raptor in both directions with one spark, and spark-to-spark in both
directions.  Native builds are required on amd64 raptor and arm64 sparks.

## Compatibility and safety invariants

1. RDMA is an optional build feature.  A build without libibverbs remains a
   normal rsync build and accepts the user-facing options far enough to report
   that RDMA is unavailable.
2. Automatic RDMA is attempted only for a remote-shell transfer.  Local,
   rsync-daemon, batch, and remote-to-remote transfers retain existing paths.
3. A new client must interoperate with an unmodified remote rsync.  It must not
   pass an unknown command-line option to an old server or bump the public
   rsync protocol in a way that makes the old server reject the connection.
4. SSH remains the authenticated control plane.  Detection, option exchange,
   endpoint exchange, and failure reporting happen before rsync multiplexing.
5. No encryption, digest, checksum, or copy is added to the bulk data plane.
   Rsync-requested checksums remain unchanged.
6. Fallback is allowed only before the first diverted literal byte.  A data
   plane failure after that point is a transfer error; silently changing paths
   mid-token could corrupt protocol framing.
7. RDMA memory is bounded by `rails * queue-depth * chunk-size` plus small
   descriptors and control messages.  Buffers are registered once and reused.
8. The receiver consumes literal chunks in rsync token order even when rails
   complete out of order.  Existing matched-block tokens stay on the SSH
   control path and require no RDMA traffic.
9. No benchmark mode can silently create a plausible but incorrect user file.
   Synthetic generation and discard behavior must be explicit in command
   output and rejected in incompatible normal-transfer modes.

## Negotiation design

The client appends `R` to rsync's existing internal `-e` capability string.
That string already exists to describe implementation behavior, is ignored by
old servers when they do not recognize a letter, and is independent of the
program named by `--rsync-path`.  A new remote that sees `R` advertises an RDMA
bit in rsync's existing server-to-client variable-length compatibility flags.
A new remote does not advertise the bit to an old client because `R` is
absent.  This preserves protocol-32 interoperability without adding a remote
command-line option that old rsync would reject.

When the capability bit is present, both processes enter a bounded RDMA setup
exchange immediately after `setup_protocol()` and before either side enables
rsync multiplexing:

1. The client sends its requested policy and tuning values over SSH.
2. Each side enumerates active Ethernet-link-layer verbs ports and associates
   them with usable IPv4 netdevices and RoCE-v2 GIDs.
3. The remote side opens one TCP bootstrap listener per candidate path and
   returns endpoint records over SSH.  A random per-process connection cookie
   sent only on the SSH control plane associates incoming bootstrap sockets
   with this rsync process; it is not a bulk-data checksum or encryption layer.
4. Rsync's receiving side subsequently forks its generator and data-receiver
   processes.  The generator closes its inherited listeners without creating
   verbs state.  Only the actual data sender and data receiver continue setup,
   so registered memory and verbs objects are never inherited across this
   fork.
5. The command-side process connects to every selected listener, binding the
   intended source netdevice address.  Each bootstrap socket exchanges QP
   number, PSN, GID, MTU, and negotiated limits, transitions an RC QP through
   INIT/RTR/RTS, exchanges a final ready record, and is then closed.
6. Until every selected QP is ready, any failure tears down partial verbs state
   and selects ordinary SSH transport.  A successful RC transition, rather
   than a matching subnet or hostname, is the proof of RDMA connectivity.

Path selection prefers distinct active verbs devices/netdevices and considers
their advertised link rates.  `auto` accepts one path, selects two when both
ends offer two useful paths, and also selects two QPs when one 400 Gb/s device
faces two 200 Gb/s devices.  Thus raptor can use its one device against both
spark rails, while a spark with only one configured 200 Gb/s rail remains a
fully supported one-path peer.  Spark-to-spark maps distinct devices to each
other.  Explicit path options exist for diagnosis, but no hostname-specific
logic belongs in the transport.

## Bulk protocol

Only literal bytes are diverted.  File lists, generator requests, block-match
tokens, literal lengths, file checksums, messages, statistics, and final
goodbyes remain on the SSH rsync stream.  This keeps rsync as the source of
truth for semantics and makes the RDMA layer a byte transport rather than a
second file-transfer protocol.

Each rail uses a fixed receive ring with `queue-depth` equal-sized registered
slots.  The receiver preposts all slots.  The sender stripes monotonically
numbered chunks across rails and posts `IBV_WR_SEND` work requests.  A compact
header contains the stream sequence and payload length.  The header is
transport framing, not a digest.  The receiver reorders only the bounded set of
completed slots and exposes exactly the literal length requested by the token
decoder.  Returning a slot reposts its receive WR, providing natural credit
flow without a second allocation or unbounded queue.

The first implementation will support uncompressed literal tokens.  Before it
is considered complete, compressed rsync transfers must either divert their
post-compression byte stream correctly or make a clearly documented,
automatically reported fallback to SSH.  Correctness and option semantics are
never changed merely to force the RDMA path.

## User-visible behavior and options

RDMA policy defaults to `auto` for SSH transfers:

- `--rdma=auto` (default): negotiate when possible; warn and fall back before
  transfer if unavailable.
- `--rdma=required`: fail rather than use SSH for literal data.
- `--no-rdma`: do not advertise or negotiate RDMA.
- `--rdma-rails=auto|1|2`: automatic or explicit data-path count.
- `--rdma-chunk-size=SIZE`: registered slot payload size.
- `--rdma-queue-depth=N`: slots per rail.
- `--rdma-port=PORT`: bootstrap listener base port; `0` requests ephemeral
  ports and is the default.
- `--rdma-device=LIST`: optional comma-separated local device/netdevice
  preference for reproducible tests.
- `--rdma-show-config`: show one concise negotiated/fallback configuration
  line even when normal output would hide it.
- `--rdma-no-config`: suppress that line even in non-quiet output.
- `--rdma-discard`: benchmark-only receiver discard; accepts exactly one
  regular-file source and leaves the named destination untouched.

By default, a non-quiet invocation prints one line resembling:

```text
rdmasync: RDMA active: 2 rails, 2 MiB chunks, depth 8, 32 MiB registered; rocep1s0f0/10.55.0.1 + roceP2p1s0f0/10.55.0.5
```

Fallback is a warning with the concrete reason, for example:

```text
rdmasync: warning: RDMA unavailable on peer; using rsync-over-SSH
```

The config line follows rsync's quiet level unless explicitly overridden by
`--rdma-show-config` or `--rdma-no-config`.  Forced display also reports final
RDMA data bytes, elapsed time, throughput, and message count.  Warnings are not
suppressed by the config-display options.

The source-file access policy is independent of whether RDMA activates:

- `--cached`: `pread`/`read` through the page cache.
- `--uncached`: aligned `O_DIRECT` reads with a bounded reusable staging ring;
  reject or explicitly fall back for unsupported file/device/alignment cases.
- `--mapped`: windowed `mmap`, with bounded mappings and prompt `munmap` so
  cancellation and very large files do not pin an unbounded address range.
- `--disk-read-size=SIZE`: controls cached and direct read-ahead/staging size;
  it is accepted but does not change mapped-window semantics.

The modes are mutually exclusive.  The measured default is
`--cached --disk-read-size=2M`: it has predictable cancellation, matches the
kernel's normal page-cache path, and was the safest cross-platform result in
`benchmarks/RDMA-SOURCE-IO-TUNING.md`.

`--synthetic-file-data=SIZE` is a benchmark-only source that produces a
deterministic counter byte stream without reading a source device.  Its final
CLI contract accepts exactly one named regular-file placeholder, including
when archive mode is selected, states the logical size, and forces a whole-file
literal stream.  A directory or other non-regular placeholder is rejected.
Pair it with a verified temporary destination so benchmark results can exclude
source reads and independently characterize destination writes, or add
`--rdma-discard` to omit destination I/O entirely.  Discard can also consume
one ordinary regular-file source when measuring cached, mapped, or direct
source access.  It is explicit in the configuration line, never creates or
replaces the named destination, rejects directory sources and source removal,
and may be paired with the user's explicit `--checksum-choice=none` when
isolating raw transport CPU.  Synthetic data uses no extra per-byte hash and
is never a substitute for the actual content of a named ordinary file.

All size options accept rsync's normal size suffixes.  Invalid zero, overflow,
alignment, unreasonable-memory, and unsupported combinations fail during
option parsing before opening an RDMA endpoint.

## Evaluated defaults

The transport sweep recorded in
`benchmarks/RDMA-QUEUE-AND-CHUNK-TUNING.md` selected:

| Parameter | Starting value | Bounded memory at two rails |
| --- | ---: | ---: |
| RDMA chunk size | 2 MiB | |
| queue depth | 8 per rail | 32 MiB payload plus headers |
| rails | auto (maximum 2) | |
| disk read size | 2 MiB | one reusable staging region |
| source I/O | cached | best safe cross-platform result |

The tuning goal is the smallest queue and chunk combination that reaches the
throughput plateau.  Increasing registered memory after throughput is within
2% of the plateau is considered waste unless it measurably improves tail
behavior or CPU use.

## Implementation sequence

1. Add configure detection for libibverbs and native amd64/arm64 builds.
2. Add strict option parsing, remote option propagation where needed, help,
   manpage text, and unit tests.
3. Add topology enumeration and a diagnostic mode/test seam.
4. Add backward-compatible capability signaling and pre-multiplex setup with
   tested fallback against an unmodified rsync.
5. Implement one-rail RC ring transport and literal-token diversion.
6. Add multi-rail striping, bounded reorder, teardown, timeout, and signal
   handling.
7. Add cached, direct, and windowed-mapped source readers.
8. Add synthetic source/discard benchmark support.
9. Run correctness, compatibility, architecture, failure-injection, memory,
   and throughput gates; tune from measured results.

## Correctness and compatibility test plan

- Run the upstream test suite on an RDMA-enabled amd64 build and an arm64
  build, then on a `--without-rdma` build.
- Compare recursive transfers containing empty, sparse, small, large,
  changing, hard-linked, ACL, xattr, and partially matching files over SSH and
  RDMA.  Exercise both sender directions and rsync checksumming options.
- Verify exact content with an independent test checksum after transfer.  This
  verification is outside the timed bulk path.
- Test old-new and new-old SSH pairs.  Automatic mode must fall back and still
  complete; required mode must fail before data transfer.
- Inject setup failures at enumeration, listener, connect, QP transition, and
  final-ready stages; prove cleanup and pre-data fallback.
- Inject post-data QP failure; prove a nonzero transfer result and no silent
  fallback.
- Exercise `SIGINT`, SSH death, timeout, disk read/write errors, receiver
  rejection, and early file-list termination without leaked processes,
  sockets, MRs, QPs, CQs, or mappings.
- Validate that quiet/config overrides and warnings match the documented
  behavior.
- Record maximum RSS while varying rails, depth, and chunk size and compare it
  with the documented memory formula.

## Performance and tuning plan

Every tuning project gets a report under `benchmarks/`, named for the decision
(for example `benchmarks/RDMA-QUEUE-AND-CHUNK-TUNING.md`).  Reports contain the
git revision, binaries and architectures, host/kernel/NIC topology, exact
commands, sample count, raw-result location, CPU and memory data, median and
spread, bottleneck analysis, selected default, and rejected alternatives.

The sequence is:

1. **Fabric ceiling:** run `ib_write_bw`/equivalent per rail and concurrently
   on both rails for raptor-to-spark and spark-to-spark.  This validates the
   topology independently of rdmasync.
2. **Transport ceiling:** run synthetic counter data to a discard receiver.
   Sweep chunk sizes (64 KiB through 2 MiB), depths (8 through 128), and one vs
   two rails.  Measure wire rate, CPU per side, context switches, and RSS.
3. **Source path:** synthetic source to a real destination only if destination
   storage is under study; otherwise compare cached, uncached, and mapped reads
   to discard after cold/warm-cache controls.  Sweep disk-read sizes around
   1, 2, 4, 8, 16, and 32 MiB.
4. **Destination/storage ceiling:** use large files that do not fit trivially
   in cache, record NVMe model and direct sequential baselines, and test both
   directions.  Distinguish network, source NVMe, destination NVMe, and rsync
   CPU bottlenecks.
5. **End-to-end rsync:** unchanged large files, new whole files, partially
   matching files, and representative trees.  Report both user-observed rate
   and literal-data wire rate.

Each throughput point gets warm-up plus at least five measured runs.  Report
median, min/max or percentile spread, and confidence limitations.  The target
is approximately 194 Gb/s aggregate for dual-rail spark-to-spark and near 200
Gb/s for raptor-to-spark when the fabric-only baseline proves that rate is
available.  Disk-backed results are judged against measured NVMe ceilings,
not against a network rate storage cannot sustain.

## Current inventory (2026-08-02)

- The starting checkout is clean upstream rsync master at `3b84610c` and has
  no RDMA code.
- Raptor is x86_64 with active `mlx5_0`, Ethernet link layer, netdevice
  `enp1s0np0`, and address `10.55.0.12/24`.  libibverbs, librdmacm,
  `ibv_devinfo`, `rdma`, and `ib_write_bw` are installed.
- All four spark hosts are reachable by noninteractive SSH, are aarch64, have
  GCC 13.3 and libibverbs 1.14.50.0, and expose two active RoCE devices.  For
  `ostrich`, the active addresses are `10.55.0.1` and `10.55.0.5`; for `dodo`,
  `10.55.0.2` and `10.55.0.6`.
- The ordinary rsync protocol version is 32.  The planned capability marker
  and compatibility-flag handshake avoids changing that public version.

## Completion gates

This project is complete only when all of the following evidence exists in the
current tree or linked benchmark artifacts:

- documented and tested CLI behavior for every option above;
- clean amd64 and arm64 builds, plus an RDMA-disabled build;
- upstream suite and new focused tests passing;
- correct automatic activation and old-peer/setup-failure fallback;
- correct one- and two-rail transfers in both directions on the selected hosts;
- no added encryption or checksum in the bulk path and no change to requested
  rsync checksums;
- bounded-memory evidence and cancellation/failure cleanup evidence;
- fabric, synthetic, file-I/O, and end-to-end benchmark reports supporting the
  final defaults;
- measured near-200-Gb/s behavior where the independent fabric ceiling and CPU
  allow it, or a precise, reproducible bottleneck report if the hardware does
  not expose that ceiling.

WHAT IS RDMASYNC?
-----------------

Rdmasync is an rsync-compatible fork that builds and installs as
`rdmasync`.  For remote-shell transfers it looks for `rdmasync` on the peer by
default and automatically uses the negotiated RDMA bulk-data path when the
fabric is available.  Install it on both endpoints, or use `--rsync-path` to
name a non-standard installation path.

Stable Linux ARM64 (including DGX Spark) and AMD64 releases are available
through the [Local AI Homebrew tap](https://github.com/tpurtell/local-ai-tap#readme):

```sh
brew tap tpurtell/local-ai https://github.com/tpurtell/local-ai-tap.git
if brew commands | grep -qx trust; then brew trust --tap tpurtell/local-ai; fi
brew install tpurtell/local-ai/rdmasync
```

The tap enables RDMA and builds natively on each architecture. Install it on
both endpoints; use `--rsync-path=/home/linuxbrew/.linuxbrew/bin/rdmasync` if
Homebrew is absent from the remote non-interactive PATH.

Rdmasync defaults the transfer checksum to `none` so high-speed whole-file
copies are not CPU-limited by an implicit digest.  Use `-c`,
`--checksum-choice=auto`, or an explicit checksum algorithm when checksum or
delta-transfer behavior is wanted.

WHAT IS RSYNC?
--------------

Rsync is a fast and extraordinarily versatile file copying tool for
both remote and local files.

Rsync uses a delta-transfer algorithm which provides a very fast method
for bringing remote files into sync.  It does this by sending just the
differences in the files across the link, without requiring that both
sets of files are present at one of the ends of the link beforehand.  At
first glance this may seem impossible because the calculation of diffs
between two files normally requires local access to both files.

A technical report describing the rsync algorithm is included with this
package.


USAGE
-----

Rdmasync keeps rsync's command line.  A normal archive copy needs no RDMA
options:

```sh
rdmasync -a source/ spark:/data/source/
rdmasync -a spark:/data/results/ results/
```

For remote-shell transfers, the default `--rdma=auto` attempts RDMA before
literal file data starts.  SSH still authenticates the peer, launches the
remote process, and carries file lists, metadata, matched-block tokens, and
status.  If the peer or fabric cannot use RDMA, the transfer continues over
SSH and always prints a warning explaining why.  Use `--rdma=required` when
fallback is not acceptable:

```sh
rdmasync -a --rdma=required checkpoint/ spark:/checkpoints/checkpoint/
```

Successful RDMA negotiation is quiet by default.  To inspect the selected
devices, addresses, rail count, registered ring, source-I/O mode, byte count,
and measured data rate, add `--rdma-show-config`:

```sh
rdmasync -a --rdma-show-config image.tar spark:/images/image.tar
```

`--rdma-no-config` is retained as an explicit compatibility spelling for the
default quiet-success behavior.  Neither it nor `--quiet` suppresses the
warning when RDMA was attempted but not used.

### Rdmasync defaults

- The remote program is `rdmasync`, not `rsync`.  Install it on both endpoints
  or provide `--rsync-path=PROGRAM`.
- The normal transfer checksum defaults to `none` for high-speed whole-file
  copies.  Use `-c`, `--checksum-choice=auto`, or a named algorithm when
  content-based selection or an explicit transfer digest is required.
- RDMA rail selection defaults to `auto`.  One configured rail is sufficient;
  two are selected only when both endpoint topologies make them useful.
- RDMA data is reliable-connected but is not encrypted and has no additional
  payload checksum.  Use it only on a trusted fabric.
- Compression remains on the SSH data path and therefore produces the normal
  RDMA-fallback warning.  On a 200/400-Gb/s fabric, compression can also become
  the bottleneck.

### RDMA options

| Option | Purpose |
| --- | --- |
| `--rdma=auto` | Attempt RDMA, warn and use SSH if setup cannot complete. This is the default. |
| `--rdma=required` | Fail before literal data rather than fall back to SSH. |
| `--no-rdma` | Disable RDMA capability advertisement and negotiation. |
| `--rdma-rails=auto\|1\|2` | Select automatic, forced one-path, or forced two-path operation. |
| `--rdma-chunk-size=SIZE` | Set each registered payload slot; accepts 4 KiB–8 MiB in 64-byte multiples. |
| `--rdma-queue-depth=N` | Set registered slots per rail; accepts 2–4096. |
| `--rdma-port=PORT` | Set the first TCP bootstrap port; `0` uses ephemeral ports. Bulk data never uses TCP. |
| `--rdma-device=LIST` | Restrict local selection by verbs device or network-device name. |
| `--rdma-show-config` | Opt in to successful negotiation topology and final RDMA counters. |
| `--rdma-no-config` | Explicitly retain the default quiet-success behavior. |

The chunk and queue-depth controls are for measured tuning, not routine use.
Registered payload memory is approximately `rails × chunk-size × queue-depth`
per endpoint.  Forcing two rails creates two QPs but does not require two
physical adapters; automatic mode remains the appropriate choice for a Spark
with only one configured rail.

### Source-I/O and benchmark options

| Option | Purpose |
| --- | --- |
| `--cached` | Read regular source files through the page cache; this is the default. |
| `--uncached` | Use aligned `O_DIRECT` source reads. |
| `--mapped` | Use bounded windowed `mmap` source access. |
| `--disk-read-size=SIZE` | Set the cached/direct source window; the measured default is 2 MiB. |
| `--synthetic-file-data=SIZE` | Generate deterministic bytes for one regular-file placeholder without source reads. |
| `--rdma-discard` | Consume and validate one file without modifying the named destination. |

The final two options are benchmark controls.  `--rdma-discard` deliberately
does not create or update the destination and must not be used for a real
copy.  A transport-only measurement that excludes both disks looks like:

```sh
rdmasync -aW --rdma=required --rdma-show-config \
  --synthetic-file-data=8G --rdma-discard \
  placeholder spark:/tmp/unused-destination
```

### Finding the remote binary

SSH starts the remote side non-interactively, so its PATH can differ from an
interactive login shell.  Check it directly:

```sh
ssh spark 'command -v rdmasync'
```

If a per-user install is not on that PATH, specify it explicitly while
preserving the tilde for the remote shell:

```sh
rdmasync -a --rsync-path='~/.local/bin/rdmasync' source/ spark:/data/source/
```

For the complete inherited rsync option set and detailed rdmasync additions,
use:

    rdmasync --help

See the [rdmasync manpage][0] for full semantics and restrictions.

[0]: rsync.1.md

BUILDING AND INSTALLING
-----------------------

Rdmasync's fast path requires Linux, a C toolchain, and the libibverbs headers
and library.  On Debian or Ubuntu, including arm64 Spark systems, install the
minimal build dependencies with:

```sh
sudo apt update
sudo apt install build-essential libibverbs-dev
```

Then configure a lean RDMA build from the checkout:

```sh
./configure --enable-rdma --disable-md2man \
  --disable-openssl --disable-xxhash --disable-zstd --disable-lz4
make -j"$(nproc)"
make check
```

`--enable-rdma` is intentional: configuration fails instead of silently
building without the RDMA transport when libibverbs is unavailable.  The
disabled options above are optional rsync checksum, compression, and manpage
generation integrations; none is required by rdmasync's RDMA data path.

For a system-wide installation:

```sh
sudo make install
```

For the per-user installation commonly used on development hosts and Sparks:

```sh
install -Dm755 rdmasync "$HOME/.local/bin/rdmasync"
```

Build and install the native binary on both endpoints.  The same commands
build amd64 on raptor and arm64 on a Spark.  Ensure non-interactive SSH can
find the remote binary, or use the `--rsync-path` form shown above.

See [INSTALL.md][1] for the full inherited rsync configuration choices,
optional libraries, platform notes, and prefix-controlled installation.

[1]: INSTALL.md


COPYRIGHT
---------

Rsync was originally written by Andrew Tridgell and Paul Mackerras.  Many
people from around the world have helped to maintain and improve it.

Rsync may be used, modified and redistributed only under the terms of
the GNU General Public License, found in the file [COPYING][9] in this
distribution, or at [the Free Software Foundation][10].

[9]: https://github.com/RsyncProject/rsync/blob/master/COPYING
[10]: https://www.fsf.org/licenses/gpl.html

# Releases

## 0.1.0

First stable release of the separately named rdmasync fork, based on the
rsync 3.5.0 development tree. The fork has its own release version; this is
not an upstream rsync release. The rsync wire protocol remains compatible.

Supports native Linux ARM64 and AMD64 builds with libibverbs. RDMA is enabled
explicitly in the Homebrew package. Compression, xxHash, ACLs, extended
attributes, and ordinary rsync transports remain available.

The release archive includes generated configure files and manual pages.
Builds do not fetch generated files from upstream or the network.

Maintainers create the archive with `packaging/release-source v0.1.0 OUTPUT_DIR`.
Archive generation requires GNU tar, gzip, autoconf, automake, a C compiler,
and Python with cmarkgfm. Consumers do not need these documentation dependencies.

The source and modifications retain GPL-3.0-or-later licensing (see COPYING
and the notices in the source files).

/*
 * File IO utilities used in rsync.
 *
 * Copyright (C) 1998 Andrew Tridgell
 * Copyright (C) 2002 Martin Pool
 * Copyright (C) 2004-2023 Wayne Davison
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, visit the http://fsf.org website.
 */

#include "rsync.h"
#include "inums.h"
#include "rdma.h"

#if defined HAVE_SYS_MMAN_H
#include <sys/mman.h>
#endif

#ifndef ENODATA
#define ENODATA EAGAIN
#endif

/* We want all reads to be aligned on 1K boundaries. */
#define ALIGN_BOUNDARY 1024
/* How far past the boundary is an offset? */
#define ALIGNED_OVERSHOOT(oft) ((oft) & (ALIGN_BOUNDARY-1))
/* Round up a length to the next boundary */
#define ALIGNED_LENGTH(len) ((((len) - 1) | (ALIGN_BOUNDARY-1)) + 1)
#define DIRECT_ALIGNMENT 4096
#define DIRECT_ALIGNED_LENGTH(len) ((((len) - 1) | (DIRECT_ALIGNMENT-1)) + 1)
#define MAPPED_WINDOW_SIZE (8 * 1024 * 1024)

extern int sparse_files;
extern int am_sender;

OFF_T preallocated_len = 0;

static OFF_T sparse_seek = 0;
static OFF_T sparse_past_write = 0;

int sparse_end(int f, OFF_T size, int updating_basis_or_equiv)
{
	int ret = 0;

	if (updating_basis_or_equiv) {
		if (sparse_seek && do_punch_hole(f, sparse_past_write, sparse_seek) < 0)
			ret = -1;
#ifdef HAVE_FTRUNCATE /* A compilation formality -- in-place requires ftruncate() */
		else /* Just in case the original file was longer */
			ret = do_ftruncate(f, size);
#endif
	} else if (sparse_seek) {
#ifdef HAVE_FTRUNCATE
		ret = do_ftruncate(f, size);
#else
		if (do_lseek(f, sparse_seek-1, SEEK_CUR) != size-1)
			ret = -1;
		else {
			do {
				ret = write(f, "", 1);
			} while (ret < 0 && errno == EINTR);

			ret = ret <= 0 ? -1 : 0;
		}
#endif
	}

	sparse_past_write = sparse_seek = 0;

	return ret;
}

/* Note that the offset is just the caller letting us know where
 * the current file position is in the file. The use_seek arg tells
 * us that we should seek over matching data instead of writing it. */
static int write_sparse(int f, int use_seek, OFF_T offset, const char *buf, int len)
{
	int l1 = 0, l2 = 0;
	int ret;

	for (l1 = 0; l1 < len && buf[l1] == 0; l1++) {}
	for (l2 = 0; l2 < len-l1 && buf[len-(l2+1)] == 0; l2++) {}

	sparse_seek += l1;

	if (l1 == len)
		return len;

	if (sparse_seek) {
		if (sparse_past_write >= preallocated_len) {
			if (do_lseek(f, sparse_seek, SEEK_CUR) < 0)
				return -1;
		} else if (do_punch_hole(f, sparse_past_write, sparse_seek) < 0) {
			sparse_seek = 0;
			return -1;
		}
	}
	sparse_seek = l2;
	sparse_past_write = offset + len - l2;

	if (use_seek) {
		/* The in-place data already matches. */
		if (do_lseek(f, len - (l1+l2), SEEK_CUR) < 0)
			return -1;
		return len;
	}

	while ((ret = write(f, buf + l1, len - (l1+l2))) <= 0) {
		if (ret < 0 && errno == EINTR)
			continue;
		sparse_seek = 0;
		return ret;
	}

	if (ret != (int)(len - (l1+l2))) {
		sparse_seek = 0;
		return l1+ret;
	}

	return len;
}

static char *wf_writeBuf;
static size_t wf_writeBufSize;
static size_t wf_writeBufCnt;

int flush_write_file(int f)
{
	int ret = 0;
	char *bp = wf_writeBuf;

	while (wf_writeBufCnt > 0) {
		if ((ret = write(f, bp, wf_writeBufCnt)) < 0) {
			if (errno == EINTR)
				continue;
			return ret;
		}
		wf_writeBufCnt -= ret;
		bp += ret;
	}
	return ret;
}

/* write_file does not allow incomplete writes.  It loops internally
 * until len bytes are written or errno is set.  Note that use_seek and
 * offset are only used in sparse processing (see write_sparse()). */
int write_file(int f, int use_seek, OFF_T offset, const char *buf, int len)
{
	int ret = 0;

	while (len > 0) {
		int r1;
		if (sparse_files > 0) {
			int len1 = MIN(len, SPARSE_WRITE_SIZE);
			r1 = write_sparse(f, use_seek, offset, buf, len1);
			offset += r1;
		} else {
			if (!wf_writeBuf) {
				wf_writeBufSize = WRITE_SIZE * 8;
				wf_writeBufCnt  = 0;
				wf_writeBuf = new_array(char, wf_writeBufSize);
			}
			r1 = (int)MIN((size_t)len, wf_writeBufSize - wf_writeBufCnt);
			if (r1) {
				memcpy(wf_writeBuf + wf_writeBufCnt, buf, r1);
				wf_writeBufCnt += r1;
			}
			if (wf_writeBufCnt == wf_writeBufSize) {
				if (flush_write_file(f) < 0)
					return -1;
				if (!r1 && len)
					continue;
			}
		}
		if (r1 <= 0) {
			if (ret > 0)
				return ret;
			return r1;
		}
		len -= r1;
		buf += r1;
		ret += r1;
	}
	return ret;
}

/* An in-place update found identical data at an identical location. We either
 * just seek past it, or (for an in-place sparse update), we give the data to
 * the sparse processor with the use_seek flag set. */
int skip_matched(int fd, OFF_T offset, const char *buf, int len)
{
	OFF_T pos;

	if (sparse_files > 0) {
		if (write_file(fd, 1, offset, buf, len) != len)
			return -1;
		return 0;
	}

	if (flush_write_file(fd) < 0)
		return -1;

	if ((pos = do_lseek(fd, len, SEEK_CUR)) != offset + len) {
		rsyserr(FERROR_XFER, errno, "lseek returned %s, not %s",
			big_num(pos), big_num(offset));
		return -1;
	}

	return 0;
}

/* This provides functionality somewhat similar to mmap() but using read().
 * It gives sliding window access to a file.  mmap() is not used because of
 * the possibility of another program (such as a mailer) truncating the
 * file thus giving us a SIGBUS. */
struct map_struct *map_file(int fd, OFF_T len, int32 read_size, int32 blk_size)
{
	struct map_struct *map;

	map = new0(struct map_struct);
	map->io_mode = am_sender ? source_io_mode : SOURCE_IO_CACHED;
	map->synthetic = am_sender && synthetic_file_size >= 0;
	if (am_sender && !map->synthetic) {
		if (map->io_mode == SOURCE_IO_MAPPED)
			read_size = MAPPED_WINDOW_SIZE;
		else if (read_size < disk_read_size)
			read_size = disk_read_size;
	}

	if (blk_size && (read_size % blk_size))
		read_size += blk_size - (read_size % blk_size);

	map->fd = fd;
	map->file_size = len;
	map->def_window_size = ALIGNED_LENGTH(read_size);

	return map;
}


/* slide the read window in the file */
char *map_ptr(struct map_struct *map, OFF_T offset, int32 len)
{
	OFF_T window_start, read_start;
	int32 window_size, read_size, read_offset, align_fudge;

	if (len == 0)
		return NULL;
	if (len < 0) {
		rprintf(FERROR, "invalid len passed to map_ptr: %ld\n",
			(long)len);
		exit_cleanup(RERR_FILEIO);
	}

	/* In every mode, reuse a window that already covers the request. */
	if (offset >= map->p_offset && offset+len <= map->p_offset+map->p_len)
		return map->p + (offset - map->p_offset);

	if (map->synthetic) {
		static char counter_pattern[256];
		static int counter_pattern_ready;
		int32 j;
		window_start = offset - ALIGNED_OVERSHOOT(offset);
		window_size = map->def_window_size;
		if (window_start + window_size > map->file_size)
			window_size = (int32)(map->file_size - window_start);
		if (window_size < len + (offset - window_start))
			window_size = ALIGNED_LENGTH(len + (offset - window_start));
		if (window_size > map->p_size) {
			map->p = realloc_array(map->p, char, window_size);
			map->p_size = window_size;
		}
		if (!counter_pattern_ready) {
			for (j = 0; j < (int32)sizeof counter_pattern; j++)
				counter_pattern[j] = (char)j;
			counter_pattern_ready = 1;
		}
		for (j = 0; j < window_size; ) {
			int32 pattern_offset = (int32)((window_start + j) & 0xff);
			int32 amount = MIN(window_size - j,
				(int32)sizeof counter_pattern - pattern_offset);
			memcpy(map->p + j, counter_pattern + pattern_offset, amount);
			j += amount;
		}
		map->p_offset = window_start;
		map->p_len = window_size;
		return map->p + (offset - window_start);
	}

	if (map->io_mode == SOURCE_IO_MAPPED) {
#if defined HAVE_MMAP && defined HAVE_SYS_MMAN_H
		long page_size = sysconf(_SC_PAGESIZE);
		OFF_T page_fudge;
		if (page_size <= 0)
			page_size = 4096;
		page_fudge = offset % page_size;
		window_start = offset - page_fudge;
		window_size = map->def_window_size;
		if (window_start + window_size > map->file_size)
			window_size = (int32)(map->file_size - window_start);
		if (window_size < len + page_fudge)
			window_size = len + page_fudge;
		if (map->mmap_base)
			munmap(map->mmap_base, map->mmap_len);
		map->mmap_len = window_size;
		map->mmap_base = mmap(NULL, map->mmap_len, PROT_READ, MAP_PRIVATE,
			map->fd, window_start);
		if (map->mmap_base == MAP_FAILED) {
			map->mmap_base = NULL;
			rsyserr(FERROR, errno, "mmap failed at %s for %ld bytes",
				big_num(window_start), (long)window_size);
			exit_cleanup(RERR_FILEIO);
		}
		map->p = map->mmap_base;
		map->p_offset = window_start;
		map->p_len = window_size;
		return map->p + page_fudge;
#else
		rprintf(FERROR, "--mapped is not supported by this build\n");
		exit_cleanup(RERR_UNSUPPORTED);
#endif
	}

	/* nope, we are going to have to do a read. Work out our desired window */
	align_fudge = (int32)ALIGNED_OVERSHOOT(offset);
	window_start = offset - align_fudge;
	window_size = map->def_window_size;
	if (window_start + window_size > map->file_size)
		window_size = (int32)(map->file_size - window_start);
	if (window_size < len + align_fudge)
		window_size = ALIGNED_LENGTH(len + align_fudge);

	if (map->io_mode == SOURCE_IO_UNCACHED) {
		ssize_t nread;
		size_t direct_size;
		align_fudge = offset & (DIRECT_ALIGNMENT - 1);
		window_start = offset - align_fudge;
		window_size = map->def_window_size;
		if (window_start + window_size > map->file_size)
			window_size = (int32)(map->file_size - window_start);
		if (window_size < len + align_fudge)
			window_size = len + align_fudge;
		direct_size = DIRECT_ALIGNED_LENGTH((size_t)window_size);
		if ((int32)direct_size > map->p_size) {
			char *newbuf;
			if (posix_memalign((void **)&newbuf, DIRECT_ALIGNMENT, direct_size) != 0)
				out_of_memory("direct-read window");
			free(map->p);
			map->p = newbuf;
			map->p_size = direct_size;
		}
		do {
			nread = pread(map->fd, map->p, direct_size, window_start);
		} while (nread < 0 && errno == EINTR);
		if (nread < window_size) {
			if (!map->status)
				map->status = nread < 0 ? errno : ENODATA;
			if (nread < 0)
				nread = 0;
			memset(map->p + nread, 0, window_size - nread);
		}
		map->p_offset = window_start;
		map->p_len = window_size;
		return map->p + align_fudge;
	}

	/* make sure we have allocated enough memory for the window */
	if (window_size > map->p_size) {
		map->p = realloc_array(map->p, char, window_size);
		map->p_size = window_size;
	}

	/* Now try to avoid re-reading any bytes by reusing any bytes from the previous buffer. */
	if (window_start >= map->p_offset && window_start < map->p_offset + map->p_len
	 && window_start + window_size >= map->p_offset + map->p_len) {
		read_start = map->p_offset + map->p_len;
		read_offset = (int32)(read_start - window_start);
		read_size = window_size - read_offset;
		memmove(map->p, map->p + (map->p_len - read_offset), read_offset);
	} else {
		read_start = window_start;
		read_size = window_size;
		read_offset = 0;
	}

	if (read_size <= 0) {
		rprintf(FERROR, "invalid read_size of %ld in map_ptr\n",
			(long)read_size);
		exit_cleanup(RERR_FILEIO);
	}

	if (map->p_fd_offset != read_start) {
		OFF_T ret = do_lseek(map->fd, read_start, SEEK_SET);
		if (ret != read_start) {
			rsyserr(FERROR, errno, "lseek returned %s, not %s",
				big_num(ret), big_num(read_start));
			exit_cleanup(RERR_FILEIO);
		}
		map->p_fd_offset = read_start;
	}
	map->p_offset = window_start;
	map->p_len = window_size;

	while (read_size > 0) {
		int32 nread = read(map->fd, map->p + read_offset, read_size);
		if (nread <= 0) {
			if (!map->status)
				map->status = nread ? errno : ENODATA;
			/* The best we can do is zero the buffer -- the file
			 * has changed mid transfer! */
			memset(map->p + read_offset, 0, read_size);
			break;
		}
		map->p_fd_offset += nread;
		read_offset += nread;
		read_size -= nread;
	}

	return map->p + align_fudge;
}

int unmap_file(struct map_struct *map)
{
	int	ret;

	if (map->mmap_base) {
#if defined HAVE_MMAP && defined HAVE_SYS_MMAN_H
		munmap(map->mmap_base, map->mmap_len);
#endif
		map->mmap_base = map->p = NULL;
	} else if (map->p) {
		free(map->p);
		map->p = NULL;
	}
	ret = map->status;
#if 0 /* I don't think we really need this. */
	force_memzero(map, sizeof map[0]);
#endif
	free(map);

	return ret;
}

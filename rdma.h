#ifndef RDMASYNC_RDMA_H
#define RDMASYNC_RDMA_H

#include <stddef.h>

enum rdma_policy {
	RDMA_POLICY_AUTO = 0,
	RDMA_POLICY_REQUIRED = 1,
	RDMA_POLICY_OFF = 2
};

enum source_io_mode {
	SOURCE_IO_CACHED = 0,
	SOURCE_IO_UNCACHED = 1,
	SOURCE_IO_MAPPED = 2
};

#define RDMA_DEFAULT_CHUNK_SIZE (2 * 1024 * 1024)
#define RDMA_DEFAULT_QUEUE_DEPTH 8
#define RDMA_DEFAULT_DISK_READ_SIZE (2 * 1024 * 1024)

extern int rdma_policy;
extern int rdma_requested_rails;
extern int rdma_chunk_size;
extern int rdma_queue_depth;
extern int rdma_bootstrap_port;
extern int rdma_config_mode;
extern char *rdma_device_filter;

extern int source_io_mode;
extern int source_io_mode_explicit;
extern int disk_read_size;
extern int disk_read_size_explicit;
extern OFF_T synthetic_file_size;
extern int rdma_discard;

void rdma_set_peer_capable(int capable);
int rdma_peer_is_capable(void);
int rdma_preflight(int f_in, int f_out);
int rdma_activate(void);
int rdma_is_active(void);
int rdma_literal_chunk_size(void);
int rdma_control_flush_interval(void);
void rdma_send_data(const char *buf, size_t len);
void rdma_send_synthetic(OFF_T offset, size_t len);
void rdma_recv_data(char *buf, size_t len);
void rdma_after_receiver_fork(int keep_transport);
void rdma_cleanup(void);

#endif

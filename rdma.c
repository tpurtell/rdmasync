/*
 * Optional RDMA bulk-data transport for remote-shell rsync transfers.
 *
 * SSH remains authoritative for all rsync protocol control.  This module
 * negotiates unencrypted RC queue pairs and transports only literal bytes
 * selected by token.c.  No checksum or encryption is added here.
 */

#include "rsync.h"
#include "rdma.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>

#ifdef SUPPORT_RDMA
#include <infiniband/verbs.h>
#endif

extern int am_server;
extern int am_daemon;
extern int am_sender;
extern int local_server;
extern int quiet;
extern int do_compression;
extern int called_from_signal_handler;
extern struct stats stats;

int rdma_policy = RDMA_POLICY_AUTO;
int rdma_requested_rails = 0;
int rdma_chunk_size = RDMA_DEFAULT_CHUNK_SIZE;
int rdma_queue_depth = RDMA_DEFAULT_QUEUE_DEPTH;
int rdma_bootstrap_port = 0;
int rdma_config_mode = -1;
char *rdma_device_filter = NULL;

int source_io_mode = SOURCE_IO_CACHED;
int source_io_mode_explicit = 0;
int disk_read_size = RDMA_DEFAULT_DISK_READ_SIZE;
int disk_read_size_explicit = 0;
OFF_T synthetic_file_size = -1;

static int peer_capable;
static int transport_active;

void rdma_set_peer_capable(int capable)
{
	peer_capable = capable;
}

int rdma_peer_is_capable(void)
{
	return peer_capable;
}

static int fallback(const char *reason)
{
	transport_active = 0;
	if (!am_server && rdma_policy == RDMA_POLICY_REQUIRED) {
		rprintf(FERROR, "rdmasync: RDMA required but unavailable: %s\n", reason);
		exit_cleanup(RERR_UNSUPPORTED);
	}
	if (!am_server && rdma_policy != RDMA_POLICY_OFF)
		rprintf(FWARNING, "rdmasync: warning: RDMA unavailable (%s); using rsync-over-SSH\n", reason);
	return 0;
}

#ifndef SUPPORT_RDMA

int rdma_preflight(UNUSED(int f_in), UNUSED(int f_out))
{
	if (rdma_policy == RDMA_POLICY_OFF || local_server || am_daemon)
		return 0;
	if (!peer_capable)
		return fallback("peer does not advertise RDMA support");
	if (do_compression)
		return fallback("rsync compression is enabled");
	return fallback("this build has no libibverbs support");
}

int rdma_activate(void)
{
	return 0;
}

void rdma_after_receiver_fork(UNUSED(int keep_transport))
{
}

void rdma_cleanup(void)
{
	transport_active = 0;
}

#else /* SUPPORT_RDMA */

#define RDMA_CTL_MAGIC 0x52444d41u /* RDMA */
#define RDMA_CTL_VERSION 1
#define RDMA_DATA_MAGIC 0x52444244u /* RDBD */
#define RDMA_PROBE_MAGIC 0x52445042u /* RDPB */
#define RDMA_MAX_CANDIDATES 8
#define RDMA_MAX_RAILS 2
#define RDMA_NAME_LEN 64
#define RDMA_REASON_LEN 240
#define RDMA_COOKIE_LEN 16
#define RDMA_BOOTSTRAP_TIMEOUT_MS 5000

struct rdma_candidate {
	char ibdev[RDMA_NAME_LEN];
	char netdev[IFNAMSIZ];
	char address[INET_ADDRSTRLEN];
	struct sockaddr_in sin;
	union ibv_gid gid;
	int port_num;
	int gid_index;
	int rate_gbps;
	int listen_fd;
	int listen_port;
};

struct rdma_endpoint {
	char ibdev[RDMA_NAME_LEN];
	char netdev[IFNAMSIZ];
	char address[INET_ADDRSTRLEN];
	int port;
	int rate_gbps;
};

struct rdma_selection {
	int local_index;
	int remote_index;
};

struct rdma_data_header {
	uint32 magic;
	uint32 length;
	uint32 seq_hi;
	uint32 seq_lo;
};

struct rdma_path {
	struct rdma_candidate candidate;
	struct ibv_context *context;
	struct ibv_pd *pd;
	struct ibv_cq *cq;
	struct ibv_qp *qp;
	struct ibv_mr *mr;
	char *ring;
	size_t stride;
	int outstanding;
	unsigned int next_slot;
	int current_recv_slot;
	size_t current_recv_length;
	size_t current_recv_offset;
};

static struct {
	int owner;
	int preflight_ok;
	int rail_count;
	int candidate_count;
	int endpoint_count;
	uint64_t send_seq;
	uint64_t recv_seq;
	uchar cookie[RDMA_COOKIE_LEN];
	struct rdma_candidate candidates[RDMA_MAX_CANDIDATES];
	struct rdma_endpoint endpoints[RDMA_MAX_CANDIDATES];
	struct rdma_selection selection[RDMA_MAX_RAILS];
	struct rdma_path paths[RDMA_MAX_RAILS];
} transport;

static char last_error[RDMA_REASON_LEN];

static void set_error(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(last_error, sizeof last_error, fmt, ap);
	va_end(ap);
}

static int read_text_file(const char *path, char *buf, size_t size)
{
	int fd;
	ssize_t len;

	if (size < 2 || (fd = open(path, O_RDONLY)) < 0)
		return -1;
	do {
		len = read(fd, buf, size - 1);
	} while (len < 0 && errno == EINTR);
	close(fd);
	if (len <= 0)
		return -1;
	buf[len] = '\0';
	while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r' || buf[len-1] == ' ' || buf[len-1] == '\t'))
		buf[--len] = '\0';
	return 0;
}

static int filter_matches(const char *ibdev, const char *netdev)
{
	const char *p, *end;
	size_t len;

	if (!rdma_device_filter || !*rdma_device_filter)
		return 1;
	for (p = rdma_device_filter; *p; p = *end ? end + 1 : end) {
		end = strchr(p, ',');
		if (!end)
			end = p + strlen(p);
		len = end - p;
		if ((strlen(ibdev) == len && strncmp(p, ibdev, len) == 0)
		 || (strlen(netdev) == len && strncmp(p, netdev, len) == 0))
			return 1;
		if (!*end)
			break;
	}
	return 0;
}

static int find_ipv4(const char *netdev, struct sockaddr_in *sin, char *text, size_t text_size)
{
	struct ifaddrs *ifas, *ifa;
	int found = 0;

	if (getifaddrs(&ifas) < 0)
		return 0;
	for (ifa = ifas; ifa; ifa = ifa->ifa_next) {
		if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET
		 || strcmp(ifa->ifa_name, netdev) != 0
		 || !(ifa->ifa_flags & IFF_UP))
			continue;
		memcpy(sin, ifa->ifa_addr, sizeof *sin);
		if (!inet_ntop(AF_INET, &sin->sin_addr, text, text_size))
			continue;
		found = 1;
		break;
	}
	freeifaddrs(ifas);
	return found;
}

static int gid_matches_ipv4(const union ibv_gid *gid, const struct in_addr *addr)
{
	static const uchar prefix[12] = { 0,0,0,0,0,0,0,0,0,0,0xff,0xff };
	return memcmp(gid->raw, prefix, sizeof prefix) == 0
	    && memcmp(gid->raw + 12, addr, sizeof *addr) == 0;
}

static int find_gid(const char *ibdev, const char *netdev, const struct in_addr *addr,
		    int port_num, int *gid_index, union ibv_gid *gid)
{
	char dirpath[PATH_MAX], path[PATH_MAX], value[128], type[64];
	DIR *dir;
	struct dirent *de;
	int found = 0;

	snprintf(dirpath, sizeof dirpath,
		 "/sys/class/infiniband/%s/ports/%d/gid_attrs/ndevs", ibdev, port_num);
	if (!(dir = opendir(dirpath)))
		return 0;
	while ((de = readdir(dir)) != NULL) {
		char *end;
		long index;
		if (!isdigit((unsigned char)de->d_name[0]))
			continue;
		index = strtol(de->d_name, &end, 10);
		if (*end || index < 0 || index > INT_MAX)
			continue;
		pathjoin(path, sizeof path, dirpath, de->d_name);
		if (read_text_file(path, value, sizeof value) < 0 || strcmp(value, netdev) != 0)
			continue;
		snprintf(path, sizeof path,
			 "/sys/class/infiniband/%s/ports/%d/gid_attrs/types/%s",
			 ibdev, port_num, de->d_name);
		if (read_text_file(path, type, sizeof type) < 0 || strcmp(type, "RoCE v2") != 0)
			continue;
		snprintf(path, sizeof path, "/sys/class/infiniband/%s/ports/%d/gids/%s",
			 ibdev, port_num, de->d_name);
		if (read_text_file(path, value, sizeof value) < 0
		 || inet_pton(AF_INET6, value, gid->raw) != 1
		 || !gid_matches_ipv4(gid, addr))
			continue;
		*gid_index = (int)index;
		found = 1;
		break;
	}
	closedir(dir);
	return found;
}

static int candidate_compare(const void *a, const void *b)
{
	const struct rdma_candidate *ca = a, *cb = b;
	int ret = strcmp(ca->ibdev, cb->ibdev);
	return ret ? ret : strcmp(ca->netdev, cb->netdev);
}

static int enumerate_candidates(struct rdma_candidate *out, int apply_filter)
{
	DIR *ibdir, *netdir;
	struct dirent *ibde, *netde;
	char path[PATH_MAX], value[128];
	int count = 0;

	if (!(ibdir = opendir("/sys/class/infiniband"))) {
		set_error("cannot enumerate /sys/class/infiniband: %s", strerror(errno));
		return 0;
	}
	while (count < RDMA_MAX_CANDIDATES && (ibde = readdir(ibdir)) != NULL) {
		struct rdma_candidate *candidate;
		if (ibde->d_name[0] == '.')
			continue;
		snprintf(path, sizeof path, "/sys/class/infiniband/%s/ports/1/state", ibde->d_name);
		if (read_text_file(path, value, sizeof value) < 0 || strncmp(value, "4:", 2) != 0)
			continue;
		snprintf(path, sizeof path, "/sys/class/infiniband/%s/ports/1/link_layer", ibde->d_name);
		if (read_text_file(path, value, sizeof value) < 0 || strcmp(value, "Ethernet") != 0)
			continue;
		snprintf(path, sizeof path, "/sys/class/infiniband/%s/device/net", ibde->d_name);
		if (!(netdir = opendir(path)))
			continue;
		while (count < RDMA_MAX_CANDIDATES && (netde = readdir(netdir)) != NULL) {
			if (netde->d_name[0] == '.'
			 || (apply_filter && !filter_matches(ibde->d_name, netde->d_name)))
				continue;
			candidate = &out[count];
			memset(candidate, 0, sizeof *candidate);
			candidate->listen_fd = -1;
			candidate->port_num = 1;
			strlcpy(candidate->ibdev, ibde->d_name, sizeof candidate->ibdev);
			strlcpy(candidate->netdev, netde->d_name, sizeof candidate->netdev);
			if (!find_ipv4(candidate->netdev, &candidate->sin,
				       candidate->address, sizeof candidate->address)
			 || !find_gid(candidate->ibdev, candidate->netdev,
				      &candidate->sin.sin_addr, candidate->port_num,
				      &candidate->gid_index, &candidate->gid))
				continue;
			snprintf(path, sizeof path, "/sys/class/infiniband/%s/ports/1/rate", ibde->d_name);
			if (read_text_file(path, value, sizeof value) == 0)
				candidate->rate_gbps = atoi(value);
			count++;
		}
		closedir(netdir);
	}
	closedir(ibdir);
	qsort(out, count, sizeof *out, candidate_compare);
	if (!count && !last_error[0])
		set_error("no active IPv4 RoCE v2 device was found");
	return count;
}

static int make_listener(struct rdma_candidate *candidate, int ordinal)
{
	struct sockaddr_in addr;
	socklen_t addrlen = sizeof addr;
	int fd, one = 1, port = rdma_bootstrap_port ? rdma_bootstrap_port + ordinal : 0;

	if (port > 65535) {
		set_error("RDMA bootstrap port range exceeds 65535");
		return -1;
	}
	if ((fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
		set_error("cannot create RDMA bootstrap socket: %s", strerror(errno));
		return -1;
	}
	fcntl(fd, F_SETFD, FD_CLOEXEC);
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
	addr = candidate->sin;
	addr.sin_port = htons(port);
	if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0 || listen(fd, RDMA_MAX_RAILS) < 0
	 || getsockname(fd, (struct sockaddr *)&addr, &addrlen) < 0) {
		set_error("cannot listen on RDMA bootstrap address %s: %s",
			  candidate->address, strerror(errno));
		close(fd);
		return -1;
	}
	candidate->listen_fd = fd;
	candidate->listen_port = ntohs(addr.sin_port);
	return 0;
}

static void close_listeners(void)
{
	int i;
	for (i = 0; i < transport.candidate_count; i++) {
		if (transport.candidates[i].listen_fd >= 0) {
			close(transport.candidates[i].listen_fd);
			transport.candidates[i].listen_fd = -1;
		}
	}
}

static void send_preflight_status(int f_out, int ok, const char *reason)
{
	write_int(f_out, ok);
	write_vstring(f_out, reason ? reason : "", reason ? strlen(reason) : 0);
}

static int receive_reason(int f_in, char *buf, size_t size)
{
	int len = read_vstring(f_in, buf, size);
	if (len < 0)
		len = 0;
	if ((size_t)len >= size)
		len = size - 1;
	buf[len] = '\0';
	return len;
}

static void make_cookie(uchar cookie[RDMA_COOKIE_LEN])
{
	int fd = open("/dev/urandom", O_RDONLY);
	int done = 0;
	while (fd >= 0 && done < RDMA_COOKIE_LEN) {
		ssize_t len = read(fd, cookie + done, RDMA_COOKIE_LEN - done);
		if (len > 0)
			done += len;
		else if (len < 0 && errno == EINTR)
			continue;
		else
			break;
	}
	if (fd >= 0)
		close(fd);
	if (done < RDMA_COOKIE_LEN) {
		uint64_t seed = ((uint64_t)time(NULL) << 32) ^ (uint64_t)getpid() ^ (uintptr_t)cookie;
		while (done < RDMA_COOKIE_LEN) {
			seed = seed * 6364136223846793005ULL + 1;
			cookie[done++] = (uchar)(seed >> 32);
		}
	}
}

static int choose_rails(void)
{
	int desired, i;
	int local_count = transport.candidate_count;
	int remote_count = transport.endpoint_count;

	if (!local_count || !remote_count)
		return 0;
	if (rdma_requested_rails)
		desired = rdma_requested_rails;
	else if (local_count >= 2 && remote_count >= 2)
		desired = 2;
	else if (local_count == 1 && remote_count >= 2
	      && transport.candidates[0].rate_gbps * 2
		 >= transport.endpoints[0].rate_gbps * 3)
		desired = 2;
	else if (local_count >= 2 && remote_count == 1
	      && transport.endpoints[0].rate_gbps * 2
		 >= transport.candidates[0].rate_gbps * 3)
		desired = 2;
	else
		desired = 1;

	for (i = 0; i < desired; i++) {
		transport.selection[i].local_index = i % local_count;
		transport.selection[i].remote_index = i % remote_count;
	}
	return desired;
}

static int server_preflight(int f_in, int f_out)
{
	char reason[RDMA_REASON_LEN];
	int magic, version, requested_policy, i, usable = 0;

	magic = read_int(f_in);
	version = read_int(f_in);
	requested_policy = read_int(f_in);
	rdma_requested_rails = read_int(f_in);
	rdma_chunk_size = read_int(f_in);
	rdma_queue_depth = read_int(f_in);
	rdma_bootstrap_port = read_int(f_in);
	read_buf(f_in, (char *)transport.cookie, RDMA_COOKIE_LEN);
	if ((unsigned)magic != RDMA_CTL_MAGIC || version != RDMA_CTL_VERSION
	 || (requested_policy != RDMA_POLICY_AUTO && requested_policy != RDMA_POLICY_REQUIRED)
	 || rdma_requested_rails < 0 || rdma_requested_rails > RDMA_MAX_RAILS
	 || rdma_chunk_size < 4096 || rdma_chunk_size > 8 * 1024 * 1024
	 || (rdma_chunk_size & 63) || rdma_queue_depth < 2 || rdma_queue_depth > 4096
	 || rdma_bootstrap_port < 0 || rdma_bootstrap_port > 65535) {
		strlcpy(reason, "invalid RDMA preflight request", sizeof reason);
		send_preflight_status(f_out, 0, reason);
		io_flush(FULL_FLUSH);
		return fallback(reason);
	}

	last_error[0] = '\0';
	transport.candidate_count = enumerate_candidates(transport.candidates, 0);
	for (i = 0; i < transport.candidate_count; i++) {
		if (make_listener(&transport.candidates[i], i) == 0) {
			if (usable != i)
				transport.candidates[usable] = transport.candidates[i];
			usable++;
		}
	}
	transport.candidate_count = usable;
	if (!usable) {
		strlcpy(reason, last_error[0] ? last_error : "no usable RDMA bootstrap endpoint", sizeof reason);
		send_preflight_status(f_out, 0, reason);
		io_flush(FULL_FLUSH);
		return fallback(reason);
	}

	send_preflight_status(f_out, 1, "");
	write_int(f_out, usable);
	for (i = 0; i < usable; i++) {
		struct rdma_candidate *candidate = &transport.candidates[i];
		write_vstring(f_out, candidate->ibdev, strlen(candidate->ibdev));
		write_vstring(f_out, candidate->netdev, strlen(candidate->netdev));
		write_vstring(f_out, candidate->address, strlen(candidate->address));
		write_int(f_out, candidate->listen_port);
		write_int(f_out, candidate->rate_gbps);
	}
	io_flush(FULL_FLUSH);

	transport.rail_count = read_int(f_in);
	if (transport.rail_count < 1 || transport.rail_count > RDMA_MAX_RAILS) {
		close_listeners();
		return fallback("command side found no compatible RDMA path");
	}
	for (i = 0; i < transport.rail_count; i++) {
		transport.selection[i].local_index = read_int(f_in);
		transport.selection[i].remote_index = read_int(f_in);
		if (transport.selection[i].remote_index < 0
		 || transport.selection[i].remote_index >= transport.candidate_count) {
			close_listeners();
			return fallback("command side selected an invalid RDMA endpoint");
		}
	}
	transport.owner = transport.preflight_ok = 1;
	return 1;
}

static int client_preflight(int f_in, int f_out)
{
	char reason[RDMA_REASON_LEN];
	int status, i;

	make_cookie(transport.cookie);
	write_int(f_out, RDMA_CTL_MAGIC);
	write_int(f_out, RDMA_CTL_VERSION);
	write_int(f_out, rdma_policy);
	write_int(f_out, rdma_requested_rails);
	write_int(f_out, rdma_chunk_size);
	write_int(f_out, rdma_queue_depth);
	write_int(f_out, rdma_bootstrap_port);
	write_buf(f_out, (char *)transport.cookie, RDMA_COOKIE_LEN);
	io_flush(FULL_FLUSH);

	status = read_int(f_in);
	receive_reason(f_in, reason, sizeof reason);
	if (!status)
		return fallback(*reason ? reason : "remote endpoint rejected RDMA setup");
	transport.endpoint_count = read_int(f_in);
	if (transport.endpoint_count < 1 || transport.endpoint_count > RDMA_MAX_CANDIDATES)
		return fallback("remote endpoint returned an invalid RDMA path count");
	for (i = 0; i < transport.endpoint_count; i++) {
		struct rdma_endpoint *endpoint = &transport.endpoints[i];
		read_vstring(f_in, endpoint->ibdev, sizeof endpoint->ibdev);
		read_vstring(f_in, endpoint->netdev, sizeof endpoint->netdev);
		read_vstring(f_in, endpoint->address, sizeof endpoint->address);
		endpoint->port = read_int(f_in);
		endpoint->rate_gbps = read_int(f_in);
		if (endpoint->port < 1 || endpoint->port > 65535)
			return fallback("remote endpoint returned an invalid bootstrap port");
	}

	last_error[0] = '\0';
	transport.candidate_count = enumerate_candidates(transport.candidates, 1);
	transport.rail_count = choose_rails();
	write_int(f_out, transport.rail_count);
	for (i = 0; i < transport.rail_count; i++) {
		write_int(f_out, transport.selection[i].local_index);
		write_int(f_out, transport.selection[i].remote_index);
	}
	io_flush(FULL_FLUSH);
	if (!transport.rail_count)
		return fallback(last_error[0] ? last_error : "no compatible local RDMA path");
	transport.owner = transport.preflight_ok = 1;
	return 1;
}

int rdma_preflight(int f_in, int f_out)
{
	if (rdma_policy == RDMA_POLICY_OFF || local_server || am_daemon)
		return 0;
	if (!peer_capable)
		return fallback("peer does not advertise RDMA support");
	if (do_compression)
		return fallback("rsync compression is enabled");
	return am_server ? server_preflight(f_in, f_out) : client_preflight(f_in, f_out);
}

static int wait_fd(int fd, short events, int timeout_ms)
{
	struct pollfd pfd;
	int ret;
	pfd.fd = fd;
	pfd.events = events;
	pfd.revents = 0;
	do {
		ret = poll(&pfd, 1, timeout_ms);
	} while (ret < 0 && errno == EINTR);
	return ret > 0 && (pfd.revents & events);
}

static int socket_write_all(int fd, const void *data, size_t len)
{
	const char *p = data;
	while (len) {
		ssize_t ret;
		if (!wait_fd(fd, POLLOUT, RDMA_BOOTSTRAP_TIMEOUT_MS))
			return -1;
		ret = write(fd, p, len);
		if (ret < 0 && errno == EINTR)
			continue;
		if (ret <= 0)
			return -1;
		p += ret;
		len -= ret;
	}
	return 0;
}

static int socket_read_all(int fd, void *data, size_t len)
{
	char *p = data;
	while (len) {
		ssize_t ret;
		if (!wait_fd(fd, POLLIN, RDMA_BOOTSTRAP_TIMEOUT_MS))
			return -1;
		ret = read(fd, p, len);
		if (ret < 0 && errno == EINTR)
			continue;
		if (ret <= 0)
			return -1;
		p += ret;
		len -= ret;
	}
	return 0;
}

static int connect_bootstrap(const struct rdma_candidate *local,
			     const struct rdma_endpoint *remote)
{
	struct sockaddr_in remote_addr, local_addr;
	int fd, flags, ret, error = 0;
	socklen_t error_len = sizeof error;

	if ((fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
		set_error("cannot create bootstrap socket: %s", strerror(errno));
		return -1;
	}
	fcntl(fd, F_SETFD, FD_CLOEXEC);
	local_addr = local->sin;
	local_addr.sin_port = 0;
	if (bind(fd, (struct sockaddr *)&local_addr, sizeof local_addr) < 0) {
		set_error("cannot bind bootstrap source %s: %s", local->address, strerror(errno));
		close(fd);
		return -1;
	}
	memset(&remote_addr, 0, sizeof remote_addr);
	remote_addr.sin_family = AF_INET;
	remote_addr.sin_port = htons(remote->port);
	if (inet_pton(AF_INET, remote->address, &remote_addr.sin_addr) != 1) {
		set_error("invalid bootstrap address %s", remote->address);
		close(fd);
		return -1;
	}
	flags = fcntl(fd, F_GETFL, 0);
	fcntl(fd, F_SETFL, flags | O_NONBLOCK);
	ret = connect(fd, (struct sockaddr *)&remote_addr, sizeof remote_addr);
	if (ret < 0 && errno != EINPROGRESS) {
		set_error("cannot connect RDMA bootstrap %s:%d: %s",
			  remote->address, remote->port, strerror(errno));
		close(fd);
		return -1;
	}
	if (ret < 0 && (!wait_fd(fd, POLLOUT, RDMA_BOOTSTRAP_TIMEOUT_MS)
	 || getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &error_len) < 0 || error)) {
		set_error("RDMA bootstrap connection to %s:%d failed: %s",
			  remote->address, remote->port, strerror(error ? error : errno));
		close(fd);
		return -1;
	}
	fcntl(fd, F_SETFL, flags);
	return fd;
}

static int accept_bootstrap(const struct rdma_candidate *candidate)
{
	int fd;
	if (!wait_fd(candidate->listen_fd, POLLIN, RDMA_BOOTSTRAP_TIMEOUT_MS)) {
		set_error("timed out waiting for RDMA bootstrap on %s:%d",
			  candidate->address, candidate->listen_port);
		return -1;
	}
	do {
		fd = accept(candidate->listen_fd, NULL, NULL);
	} while (fd < 0 && errno == EINTR);
	if (fd < 0) {
		set_error("cannot accept RDMA bootstrap on %s:%d: %s",
			  candidate->address, candidate->listen_port, strerror(errno));
		return -1;
	}
	fcntl(fd, F_SETFD, FD_CLOEXEC);
	return fd;
}

static struct ibv_context *open_verbs_device(const char *name)
{
	struct ibv_device **devices;
	struct ibv_context *context = NULL;
	int count, i;

	devices = ibv_get_device_list(&count);
	if (!devices)
		return NULL;
	for (i = 0; i < count; i++) {
		if (strcmp(ibv_get_device_name(devices[i]), name) == 0) {
			context = ibv_open_device(devices[i]);
			break;
		}
	}
	ibv_free_device_list(devices);
	return context;
}

static int poll_completion(struct rdma_path *path, struct ibv_wc *wc, int timeout_ms)
{
	struct timespec start, now;
	long elapsed;
	int ret;

	clock_gettime(CLOCK_MONOTONIC, &start);
	for (;;) {
		ret = ibv_poll_cq(path->cq, 1, wc);
		if (ret > 0)
			return wc->status == IBV_WC_SUCCESS ? 0 : -1;
		if (ret < 0)
			return -1;
		clock_gettime(CLOCK_MONOTONIC, &now);
		elapsed = (now.tv_sec - start.tv_sec) * 1000
			+ (now.tv_nsec - start.tv_nsec) / 1000000;
		if (elapsed >= timeout_ms)
			return -1;
	}
}

static void encode_header(struct rdma_data_header *header, uint32 magic,
			  uint64_t seq, uint32 length)
{
	header->magic = htonl(magic);
	header->length = htonl(length);
	header->seq_hi = htonl((uint32)(seq >> 32));
	header->seq_lo = htonl((uint32)seq);
}

static uint64_t decode_seq(const struct rdma_data_header *header)
{
	return ((uint64_t)ntohl(header->seq_hi) << 32) | ntohl(header->seq_lo);
}

static int post_receive_slot(struct rdma_path *path, int slot)
{
	struct ibv_sge sge;
	struct ibv_recv_wr wr, *bad;

	memset(&sge, 0, sizeof sge);
	sge.addr = (uintptr_t)(path->ring + path->stride * slot);
	sge.length = path->stride;
	sge.lkey = path->mr->lkey;
	memset(&wr, 0, sizeof wr);
	wr.wr_id = slot;
	wr.sg_list = &sge;
	wr.num_sge = 1;
	return ibv_post_recv(path->qp, &wr, &bad);
}

static int modify_qp_init(struct rdma_path *path)
{
	struct ibv_qp_attr attr;
	memset(&attr, 0, sizeof attr);
	attr.qp_state = IBV_QPS_INIT;
	attr.pkey_index = 0;
	attr.port_num = path->candidate.port_num;
	attr.qp_access_flags = 0;
	return ibv_modify_qp(path->qp, &attr,
		IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS);
}

static int modify_qp_rtr_rts(struct rdma_path *path, uint32 remote_qpn,
			     uint32 remote_psn, int remote_mtu,
			     const union ibv_gid *remote_gid, uint32 local_psn)
{
	struct ibv_qp_attr attr;
	struct ibv_port_attr port_attr;
	int mtu;

	if (ibv_query_port(path->context, path->candidate.port_num, &port_attr))
		return -1;
	mtu = MIN((int)port_attr.active_mtu, remote_mtu);
	memset(&attr, 0, sizeof attr);
	attr.qp_state = IBV_QPS_RTR;
	attr.path_mtu = mtu;
	attr.dest_qp_num = remote_qpn;
	attr.rq_psn = remote_psn;
	attr.max_dest_rd_atomic = 1;
	attr.min_rnr_timer = 12;
	attr.ah_attr.is_global = 1;
	attr.ah_attr.port_num = path->candidate.port_num;
	attr.ah_attr.grh.dgid = *remote_gid;
	attr.ah_attr.grh.sgid_index = path->candidate.gid_index;
	attr.ah_attr.grh.hop_limit = 64;
	if (ibv_modify_qp(path->qp, &attr, IBV_QP_STATE | IBV_QP_AV
		| IBV_QP_PATH_MTU | IBV_QP_DEST_QPN | IBV_QP_RQ_PSN
		| IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER))
		return -1;

	memset(&attr, 0, sizeof attr);
	attr.qp_state = IBV_QPS_RTS;
	attr.timeout = 14;
	attr.retry_cnt = 7;
	attr.rnr_retry = 7;
	attr.sq_psn = local_psn;
	attr.max_rd_atomic = 1;
	return ibv_modify_qp(path->qp, &attr, IBV_QP_STATE | IBV_QP_TIMEOUT
		| IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN
		| IBV_QP_MAX_QP_RD_ATOMIC);
}

static void destroy_path(struct rdma_path *path)
{
	if (path->qp)
		ibv_destroy_qp(path->qp);
	if (path->mr)
		ibv_dereg_mr(path->mr);
	if (path->cq)
		ibv_destroy_cq(path->cq);
	if (path->pd)
		ibv_dealloc_pd(path->pd);
	if (path->context)
		ibv_close_device(path->context);
	free(path->ring);
	memset(path, 0, sizeof *path);
	path->current_recv_slot = -1;
}

static int setup_path_resources(struct rdma_path *path,
				const struct rdma_candidate *candidate)
{
	struct ibv_qp_init_attr init;
	struct ibv_device_attr device_attr;
	size_t total;
	int i;

	memset(path, 0, sizeof *path);
	path->candidate = *candidate;
	path->candidate.listen_fd = -1;
	path->current_recv_slot = -1;
	path->stride = (sizeof(struct rdma_data_header) + rdma_chunk_size + 63) & ~(size_t)63;
	if ((size_t)rdma_queue_depth > SIZE_MAX / path->stride) {
		set_error("RDMA ring size overflows size_t");
		return -1;
	}
	total = path->stride * rdma_queue_depth;
	if (posix_memalign((void **)&path->ring, 4096, total) != 0) {
		set_error("cannot allocate %s RDMA ring", do_big_num((int64)total, 0, NULL));
		return -1;
	}
	memset(path->ring, 0, total);
	if (!(path->context = open_verbs_device(candidate->ibdev))
	 || ibv_query_device(path->context, &device_attr)
	 || rdma_queue_depth > (int)device_attr.max_qp_wr
	 || !(path->pd = ibv_alloc_pd(path->context))
	 || !(path->cq = ibv_create_cq(path->context, rdma_queue_depth * 2, NULL, NULL, 0))) {
		set_error("cannot allocate verbs resources on %s", candidate->ibdev);
		return -1;
	}
	memset(&init, 0, sizeof init);
	init.send_cq = init.recv_cq = path->cq;
	init.qp_type = IBV_QPT_RC;
	init.cap.max_send_wr = rdma_queue_depth;
	init.cap.max_recv_wr = rdma_queue_depth;
	init.cap.max_send_sge = init.cap.max_recv_sge = 1;
	if (!(path->qp = ibv_create_qp(path->pd, &init))
	 || !(path->mr = ibv_reg_mr(path->pd, path->ring, total, IBV_ACCESS_LOCAL_WRITE))
	 || modify_qp_init(path)) {
		set_error("cannot create/register RC queue on %s", candidate->ibdev);
		return -1;
	}
	if (!am_sender) {
		for (i = 0; i < rdma_queue_depth; i++) {
			if (post_receive_slot(path, i)) {
				set_error("cannot post receive ring on %s", candidate->ibdev);
				return -1;
			}
		}
	}
	return 0;
}

static void put_u32(uchar **p, uint32 value)
{
	value = htonl(value);
	memcpy(*p, &value, sizeof value);
	*p += sizeof value;
}

static uint32 get_u32(const uchar **p)
{
	uint32 value;
	memcpy(&value, *p, sizeof value);
	*p += sizeof value;
	return ntohl(value);
}

static int post_probe_send(struct rdma_path *path, int path_index)
{
	struct rdma_data_header *header = (struct rdma_data_header *)path->ring;
	struct ibv_sge sge;
	struct ibv_send_wr wr, *bad;
	struct ibv_wc wc;

	encode_header(header, RDMA_PROBE_MAGIC, path_index, 0);
	memset(&sge, 0, sizeof sge);
	sge.addr = (uintptr_t)header;
	sge.length = sizeof *header;
	sge.lkey = path->mr->lkey;
	memset(&wr, 0, sizeof wr);
	wr.wr_id = 0;
	wr.sg_list = &sge;
	wr.num_sge = 1;
	wr.opcode = IBV_WR_SEND;
	wr.send_flags = IBV_SEND_SIGNALED;
	if (ibv_post_send(path->qp, &wr, &bad) || poll_completion(path, &wc, RDMA_BOOTSTRAP_TIMEOUT_MS))
		return -1;
	return wc.opcode == IBV_WC_SEND ? 0 : -1;
}

static int receive_probe(struct rdma_path *path, int path_index)
{
	struct ibv_wc wc;
	struct rdma_data_header *header;
	if (poll_completion(path, &wc, RDMA_BOOTSTRAP_TIMEOUT_MS)
	 || wc.opcode != IBV_WC_RECV || wc.wr_id >= (uint64_t)rdma_queue_depth
	 || wc.byte_len < sizeof *header)
		return -1;
	header = (struct rdma_data_header *)(path->ring + path->stride * wc.wr_id);
	if (ntohl(header->magic) != RDMA_PROBE_MAGIC || decode_seq(header) != (uint64_t)path_index
	 || ntohl(header->length) != 0 || post_receive_slot(path, (int)wc.wr_id))
		return -1;
	return 0;
}

static int exchange_qp(struct rdma_path *path, int bootstrap_fd, int path_index)
{
	uchar local[64], remote[64], *put = local;
	const uchar *get = remote;
	union ibv_gid remote_gid;
	struct ibv_port_attr port_attr;
	uint32 local_psn, remote_qpn, remote_psn, remote_mtu;
	uchar ready = 1, peer_ready = 0;

	local_psn = ((uint32)transport.cookie[path_index * 4] << 16)
		| ((uint32)transport.cookie[path_index * 4 + 1] << 8)
		| transport.cookie[path_index * 4 + 2];
	if (ibv_query_port(path->context, path->candidate.port_num, &port_attr)) {
		set_error("cannot query verbs port on %s", path->candidate.ibdev);
		return -1;
	}
	put_u32(&put, RDMA_CTL_MAGIC);
	put_u32(&put, RDMA_CTL_VERSION);
	put_u32(&put, path_index);
	put_u32(&put, path->qp->qp_num);
	put_u32(&put, local_psn);
	put_u32(&put, port_attr.active_mtu);
	put_u32(&put, rdma_chunk_size);
	put_u32(&put, rdma_queue_depth);
	memcpy(put, path->candidate.gid.raw, 16); put += 16;
	memcpy(put, transport.cookie, RDMA_COOKIE_LEN); put += RDMA_COOKIE_LEN;
	if (put - local != sizeof local
	 || socket_write_all(bootstrap_fd, local, sizeof local)
	 || socket_read_all(bootstrap_fd, remote, sizeof remote)) {
		set_error("RDMA QP bootstrap exchange failed on path %d", path_index + 1);
		return -1;
	}
	if (get_u32(&get) != RDMA_CTL_MAGIC || get_u32(&get) != RDMA_CTL_VERSION
	 || get_u32(&get) != (uint32)path_index) {
		set_error("RDMA QP bootstrap identity mismatch on path %d", path_index + 1);
		return -1;
	}
	remote_qpn = get_u32(&get);
	remote_psn = get_u32(&get);
	remote_mtu = get_u32(&get);
	if (get_u32(&get) != (uint32)rdma_chunk_size
	 || get_u32(&get) != (uint32)rdma_queue_depth) {
		set_error("RDMA QP tuning mismatch on path %d", path_index + 1);
		return -1;
	}
	memcpy(remote_gid.raw, get, 16); get += 16;
	if (memcmp(get, transport.cookie, RDMA_COOKIE_LEN) != 0) {
		set_error("RDMA QP bootstrap cookie mismatch on path %d", path_index + 1);
		return -1;
	}
	if (modify_qp_rtr_rts(path, remote_qpn, remote_psn, remote_mtu, &remote_gid, local_psn)) {
		set_error("cannot transition RC QP on %s to ready", path->candidate.ibdev);
		return -1;
	}
	if ((am_sender ? post_probe_send(path, path_index) : receive_probe(path, path_index)) < 0) {
		set_error("RDMA fabric probe failed on %s", path->candidate.ibdev);
		return -1;
	}
	if (socket_write_all(bootstrap_fd, &ready, 1)
	 || socket_read_all(bootstrap_fd, &peer_ready, 1) || peer_ready != 1) {
		set_error("RDMA ready exchange failed on path %d", path_index + 1);
		return -1;
	}
	return 0;
}

static void destroy_all_paths(void)
{
	int i;
	for (i = 0; i < RDMA_MAX_RAILS; i++)
		destroy_path(&transport.paths[i]);
	transport_active = 0;
}

static void show_active_config(void)
{
	size_t memory = (size_t)transport.rail_count * rdma_queue_depth
		* transport.paths[0].stride;
	int i;

	if (am_server || rdma_config_mode == 0 || (rdma_config_mode < 0 && quiet))
		return;
	rprintf(FINFO, "rdmasync: RDMA active: %d rail%s, %s chunks, depth %d, %s registered; ",
		transport.rail_count, transport.rail_count == 1 ? "" : "s",
		do_big_num(rdma_chunk_size, 3, NULL), rdma_queue_depth,
		do_big_num((int64)memory, 3, NULL));
	for (i = 0; i < transport.rail_count; i++) {
		struct rdma_candidate *local = &transport.candidates[transport.selection[i].local_index];
		struct rdma_endpoint *remote = &transport.endpoints[transport.selection[i].remote_index];
		rprintf(FINFO, "%s%s/%s->%s/%s", i ? " + " : "",
			local->ibdev, local->address, remote->ibdev, remote->address);
	}
	rprintf(FINFO, "; source=%s%s", source_io_mode == SOURCE_IO_UNCACHED ? "uncached"
		: source_io_mode == SOURCE_IO_MAPPED ? "mapped" : "cached",
		synthetic_file_size >= 0 ? ",synthetic" : "");
	rprintf(FINFO, "\n");
}

int rdma_activate(void)
{
	int i, fd = -1;

	if (!transport.preflight_ok || !transport.owner || transport_active)
		return transport_active;
	last_error[0] = '\0';
	for (i = 0; i < transport.rail_count; i++) {
		struct rdma_candidate *candidate;
		if (am_server) {
			candidate = &transport.candidates[transport.selection[i].remote_index];
			fd = accept_bootstrap(candidate);
		} else {
			candidate = &transport.candidates[transport.selection[i].local_index];
			fd = connect_bootstrap(candidate,
				&transport.endpoints[transport.selection[i].remote_index]);
		}
		if (fd < 0 || setup_path_resources(&transport.paths[i], candidate) < 0
		 || exchange_qp(&transport.paths[i], fd, i) < 0) {
			if (fd >= 0)
				close(fd);
			if (am_server && last_error[0])
				rprintf(FWARNING, "rdmasync: warning: remote RDMA activation failed: %s\n",
					last_error);
			destroy_all_paths();
			close_listeners();
			transport.preflight_ok = 0;
			return fallback(last_error[0] ? last_error : "RDMA activation failed");
		}
		close(fd);
		fd = -1;
	}
	close_listeners();
	transport_active = 1;
	show_active_config();
	return 1;
}

static int post_data_send(struct rdma_path *path, const char *buf, size_t len, uint64_t seq)
{
	struct rdma_data_header *header;
	struct ibv_send_wr wr, *bad;
	struct ibv_sge sge;
	struct ibv_wc wc;
	unsigned int slot;

	memset(&wc, 0, sizeof wc);
	if (path->outstanding >= rdma_queue_depth) {
		if (poll_completion(path, &wc, RDMA_BOOTSTRAP_TIMEOUT_MS) || wc.opcode != IBV_WC_SEND) {
			set_error("RDMA send completion failed on %s: %s",
				  path->candidate.ibdev, ibv_wc_status_str(wc.status));
			return -1;
		}
		path->outstanding--;
	}
	slot = path->next_slot++ % rdma_queue_depth;
	header = (struct rdma_data_header *)(path->ring + path->stride * slot);
	encode_header(header, RDMA_DATA_MAGIC, seq, len);
	memcpy(header + 1, buf, len);
	memset(&sge, 0, sizeof sge);
	sge.addr = (uintptr_t)header;
	sge.length = sizeof *header + len;
	sge.lkey = path->mr->lkey;
	memset(&wr, 0, sizeof wr);
	wr.wr_id = slot;
	wr.sg_list = &sge;
	wr.num_sge = 1;
	wr.opcode = IBV_WR_SEND;
	wr.send_flags = IBV_SEND_SIGNALED;
	if (ibv_post_send(path->qp, &wr, &bad)) {
		set_error("cannot post RDMA send on %s: %s",
			  path->candidate.ibdev, strerror(errno));
		return -1;
	}
	path->outstanding++;
	return 0;
}

void rdma_send_data(const char *buf, size_t len)
{
	while (len) {
		size_t amount = MIN(len, (size_t)rdma_chunk_size);
		struct rdma_path *path = &transport.paths[transport.send_seq % transport.rail_count];
		if (post_data_send(path, buf, amount, transport.send_seq) < 0) {
			rprintf(FERROR, "rdmasync: RDMA data send failed after activation: %s\n",
				last_error[0] ? last_error : "unknown verbs error");
			exit_cleanup(RERR_STREAMIO);
		}
		transport.send_seq++;
		stats.total_written += amount;
		buf += amount;
		len -= amount;
	}
}

static int begin_receive_message(struct rdma_path *path, uint64_t expected_seq)
{
	struct ibv_wc wc;
	struct rdma_data_header *header;
	uint32 length;

	if (poll_completion(path, &wc, RDMA_BOOTSTRAP_TIMEOUT_MS)
	 || wc.opcode != IBV_WC_RECV || wc.wr_id >= (uint64_t)rdma_queue_depth
	 || wc.byte_len < sizeof *header)
		return -1;
	header = (struct rdma_data_header *)(path->ring + path->stride * wc.wr_id);
	length = ntohl(header->length);
	if (ntohl(header->magic) != RDMA_DATA_MAGIC || decode_seq(header) != expected_seq
	 || length > (uint32)rdma_chunk_size || wc.byte_len != sizeof *header + length)
		return -1;
	path->current_recv_slot = (int)wc.wr_id;
	path->current_recv_length = length;
	path->current_recv_offset = 0;
	return 0;
}

void rdma_recv_data(char *buf, size_t len)
{
	while (len) {
		struct rdma_path *path = &transport.paths[transport.recv_seq % transport.rail_count];
		size_t available, amount;
		struct rdma_data_header *header;
		if (path->current_recv_slot < 0
		 && begin_receive_message(path, transport.recv_seq) < 0) {
			rprintf(FERROR, "rdmasync: RDMA data receive failed after activation\n");
			exit_cleanup(RERR_STREAMIO);
		}
		header = (struct rdma_data_header *)(path->ring
			+ path->stride * path->current_recv_slot);
		available = path->current_recv_length - path->current_recv_offset;
		amount = MIN(len, available);
		memcpy(buf, (char *)(header + 1) + path->current_recv_offset, amount);
		path->current_recv_offset += amount;
		stats.total_read += amount;
		buf += amount;
		len -= amount;
		if (path->current_recv_offset == path->current_recv_length) {
			if (post_receive_slot(path, path->current_recv_slot)) {
				rprintf(FERROR, "rdmasync: failed to repost RDMA receive slot\n");
				exit_cleanup(RERR_STREAMIO);
			}
			path->current_recv_slot = -1;
			transport.recv_seq++;
		}
	}
}

void rdma_after_receiver_fork(int keep_transport)
{
	if (!transport.preflight_ok)
		return;
	if (keep_transport) {
		transport.owner = 1;
		return;
	}
	close_listeners();
	transport.owner = transport.preflight_ok = 0;
}

void rdma_cleanup(void)
{
	int i;
	struct ibv_wc wc;
	if (!transport.owner || called_from_signal_handler)
		return;
	if (transport_active && am_sender) {
		for (i = 0; i < transport.rail_count; i++) {
			while (transport.paths[i].outstanding > 0) {
				if (poll_completion(&transport.paths[i], &wc, RDMA_BOOTSTRAP_TIMEOUT_MS))
					break;
				transport.paths[i].outstanding--;
			}
		}
	}
	destroy_all_paths();
	close_listeners();
	transport.owner = transport.preflight_ok = 0;
}

#endif /* SUPPORT_RDMA */

int rdma_is_active(void)
{
	return transport_active;
}

int rdma_literal_chunk_size(void)
{
	return transport_active ? rdma_chunk_size : CHUNK_SIZE;
}

#ifndef SUPPORT_RDMA
void rdma_send_data(UNUSED(const char *buf), UNUSED(size_t len))
{
	rprintf(FERROR, "rdmasync: internal error: RDMA send on inactive transport\n");
	exit_cleanup(RERR_STREAMIO);
}

void rdma_recv_data(UNUSED(char *buf), UNUSED(size_t len))
{
	rprintf(FERROR, "rdmasync: internal error: RDMA receive on inactive transport\n");
	exit_cleanup(RERR_STREAMIO);
}
#endif

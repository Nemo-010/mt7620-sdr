/*
 * mt7620agent - device side of the MT7620 toy SDR.
 *
 * Reads one capture record at a time from the capture file (the debugfs node
 * the kernel module exposes) and forwards it unchanged to a TCP client. The
 * record already is the wire format from docs/04, so this program never needs
 * to understand I/Q: the host (host/mt7620cap.py) does that.
 *
 * Every read() reopens the source, which is what triggers a fresh capture.
 *
 * Usage:
 *   mt7620agent [-s SOURCE] [-l PORT] [-c HOST:PORT] [-n COUNT] [-d MS]
 *
 *   -s SOURCE   capture file (default /sys/kernel/debug/mt7620sdr/capture)
 *   -l PORT     listen on PORT and accept a client (default 5555)
 *   -c ADDR     connect out to ADDR instead of listening
 *   -n COUNT    send COUNT records then exit (0 = forever, default 0)
 *   -d MS       sleep MS between captures
 *   -o FILE     write records to FILE instead of a socket ('-' = stdout)
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define DEF_SOURCE "/sys/kernel/debug/mt7620sdr/capture"
#define DEF_PORT   5555
#define REC_MAX    (1024 * 1024)

static volatile sig_atomic_t stop;

static void on_signal(int sig)
{
	(void)sig;
	stop = 1;
}

static int read_record(int fd, unsigned char *buf, size_t cap)
{
	size_t off = 0;

	while (off < cap) {
		ssize_t n = read(fd, buf + off, cap - off);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (n == 0)
			break;
		off += (size_t)n;
	}
	return (int)off;
}

static int write_all(int fd, const unsigned char *buf, size_t len)
{
	size_t off = 0;

	while (off < len) {
		ssize_t n = write(fd, buf + off, len - off);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		off += (size_t)n;
	}
	return 0;
}

static void log_record(const unsigned char *buf, int len)
{
	uint32_t nsamples, chains, rate;
	uint64_t seq, lo;

	if (len < 44 || memcmp(buf, "M7620SDR", 8) != 0) {
		fprintf(stderr, "record: %d bytes (no header)\n", len);
		return;
	}
	memcpy(&seq, buf + 16, 8);
	memcpy(&nsamples, buf + 24, 4);
	memcpy(&chains, buf + 28, 4);
	memcpy(&rate, buf + 32, 4);
	memcpy(&lo, buf + 36, 8);
	fprintf(stderr, "seq %llu: %u samples x %u chains @ %u Hz, lo %llu Hz\n",
		(unsigned long long)seq, nsamples, chains, rate,
		(unsigned long long)lo);
}

static int io_fd;

static void cleanup(void)
{
	if (io_fd >= 0)
		close(io_fd);
}

int main(int argc, char **argv)
{
	const char *source = DEF_SOURCE;
	const char *connect_to = NULL;
	const char *out_path = NULL;
	int port = DEF_PORT;
	int file_out = 0;
	long count = 0;
	long delay_ms = 0;
	int srv = -1, cli = -1, opt;
	unsigned char *buf;

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	signal(SIGPIPE, SIG_IGN);

	while ((opt = getopt(argc, argv, "s:l:c:n:d:o:h")) != -1) {
		switch (opt) {
		case 's': source = optarg; break;
		case 'l': port = atoi(optarg); break;
		case 'c': connect_to = optarg; break;
		case 'n': count = atol(optarg); break;
		case 'd': delay_ms = atol(optarg); break;
		case 'o': out_path = optarg; break;
		default:
			fprintf(stderr,
				"usage: %s [-s SOURCE] [-l PORT] [-c HOST:PORT]"
				" [-n COUNT] [-d MS] [-o FILE]\n", argv[0]);
			return 2;
		}
	}

	buf = malloc(REC_MAX);
	if (!buf) {
		perror("malloc");
		return 1;
	}

	if (out_path) {
		file_out = 1;
		io_fd = (strcmp(out_path, "-") == 0)
			? STDOUT_FILENO
			: open(out_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (io_fd < 0) {
			perror(out_path);
			return 1;
		}
	} else if (connect_to) {
		char host[256];
		const char *colon = strrchr(connect_to, ':');
		struct addrinfo hints, *res;

		if (!colon) {
			fprintf(stderr, "bad -c address\n");
			return 2;
		}
		snprintf(host, sizeof(host), "%.*s",
			 (int)(colon - connect_to), connect_to);
		memset(&hints, 0, sizeof(hints));
		hints.ai_family = AF_INET;
		hints.ai_socktype = SOCK_STREAM;
		if (getaddrinfo(host, colon + 1, &hints, &res) != 0) {
			perror("getaddrinfo");
			return 1;
		}
		cli = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
		if (cli < 0 || connect(cli, res->ai_addr, res->ai_addrlen) < 0) {
			perror("connect");
			return 1;
		}
		freeaddrinfo(res);
	} else {
		struct sockaddr_in sa;
		int one = 1;

		srv = socket(AF_INET, SOCK_STREAM, 0);
		if (srv < 0) {
			perror("socket");
			return 1;
		}
		setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
		memset(&sa, 0, sizeof(sa));
		sa.sin_family = AF_INET;
		sa.sin_addr.s_addr = htonl(INADDR_ANY);
		sa.sin_port = htons((uint16_t)port);
		if (bind(srv, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
			perror("bind");
			return 1;
		}
		if (listen(srv, 4) < 0) {
			perror("listen");
			return 1;
		}
		fprintf(stderr, "listening on :%d\n", port);
	}

	if (!file_out)
		io_fd = cli;

	while (!stop) {
		int fd, len;
		long sent = 0;

		if (!connect_to && !file_out) {
			cli = accept(srv, NULL, NULL);
			if (cli < 0) {
				if (errno == EINTR)
					continue;
				perror("accept");
				break;
			}
			io_fd = cli;
			setsockopt(cli, IPPROTO_TCP, TCP_NODELAY,
				   &(int){ 1 }, sizeof(int));
			fprintf(stderr, "client connected\n");
		}

		while (!stop && (count == 0 || sent < count)) {
			fd = open(source, O_RDONLY);
			if (fd < 0) {
				perror(source);
				if (!connect_to || file_out)
					break;
				return 1;
			}
			len = read_record(fd, buf, REC_MAX);
			close(fd);
			if (len <= 0) {
				fprintf(stderr, "empty capture\n");
				if (!connect_to || file_out)
					break;
				return 1;
			}
			log_record(buf, len);
			if (write_all(io_fd, buf, (size_t)len) < 0) {
				fprintf(stderr, "client went away\n");
				break;
			}
			sent++;
			if (delay_ms > 0) {
				struct timespec ts = {
					.tv_sec = delay_ms / 1000,
					.tv_nsec = (delay_ms % 1000) * 1000000L,
				};
				nanosleep(&ts, NULL);
			}
		}

		if (connect_to || file_out)
			break;
		close(cli);
		cli = -1;
		io_fd = -1;
	}

	cleanup();
	if (srv >= 0)
		close(srv);
	if (file_out && io_fd > STDERR_FILENO)
		close(io_fd);
	free(buf);
	return 0;
}

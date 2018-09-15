/* SPDX-License-Identifier: GPL-2.0
 * Copyright (c) 2018 Jesper Dangaard Brouer
 */
static const char *__doc__ =
 "XDP debug program storing XDP level frame into tcpdump-pcap file";

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <locale.h>
#include <sys/resource.h>
#include <sys/sysinfo.h>
#include <getopt.h>
#include <net/if.h>
#include <assert.h>

#include <linux/if_link.h>

#include <linux/perf_event.h>
#include "perf-sys.h"
//#include "trace_helpers.h" // MISSING

#include "bpf_util.h"

/* These inc are located in tools/lib/ */
#include <bpf/bpf.h>
#include <bpf/libbpf.h>

static int ifindex = -1;
static char ifname_buf[IF_NAMESIZE];
static char *ifname;

#define MAX_CPUS 128
static int pmu_fds[MAX_CPUS];
static struct perf_event_mmap_page *headers[MAX_CPUS];

static __u32 xdp_flags;

static const struct option long_options[] = {
	{"help",	no_argument,		NULL, 'h' },
	{"dev",		required_argument,	NULL, 'd' },
	{"skb-mode",	no_argument,		NULL, 'S' },
	{0, 0, NULL,  0 }
};

/* Extra exit(3) return codes, besides EXIT_SUCCESS and EXIT_FAILURE */
#define EXIT_FAIL_OPTION	2
#define EXIT_FAIL_XDP		3
#define EXIT_FAIL_BPF		4

static void exit_sig_handler(int sig)
{
	fprintf(stderr,
		"Interrupted: Removing XDP program on ifindex:%d device:%s\n",
		ifindex, ifname);
	if (ifindex > -1)
		bpf_set_link_xdp_fd(ifindex, -1, xdp_flags);
	exit(EXIT_SUCCESS);
}

static void usage(char *argv[])
{
	int i;

	printf("\nDOCUMENTATION:\n%s\n", __doc__);
	printf(" Usage: %s (options-see-below)\n", argv[0]);
	printf(" Listing options:\n");
	for (i = 0; long_options[i].name != 0; i++) {
		printf(" --%-12s", long_options[i].name);
		if (long_options[i].flag != NULL)
			printf(" flag (internal value:%d)",
				*long_options[i].flag);
		else
			printf(" short-option: -%c",
				long_options[i].val);
		printf("\n");
	}
	printf("\n");
}

/* Header for perf event (meta data place before pkt data) */
struct my_perf_hdr {
	__u16 cookie;
	__u16 pkt_len;
} __packed;
#define COOKIE	0x9ca9

static int handle_perf_event(void *data, int size)
{
	struct {
		struct my_perf_hdr hdr;
		__u8  pkt_data[];
	} *e = data;
	int i;

	if (e->hdr.cookie != COOKIE) {
		printf("BUG cookie %x sized %d\n", e->hdr.cookie, size);
		return LIBBPF_PERF_EVENT_ERROR;
	}

	printf("Pkt len: %-5d bytes. Ethernet hdr: ", e->hdr.pkt_len);
	for (i = 0; i < 14 && i < e->hdr.pkt_len; i++)
		printf("%02x ", e->pkt_data[i]);
	printf("\n");

	return LIBBPF_PERF_EVENT_CONT;
}

static void setup_bpf_perf_event(int map_fd, int num)
{
	struct perf_event_attr attr = {
		.sample_type	= PERF_SAMPLE_RAW,
		.type		= PERF_TYPE_SOFTWARE,
		.config		= PERF_COUNT_SW_BPF_OUTPUT,
		.wakeup_events	= 1,/* get an fd notification for every event */
	};
	int i;

	for (i = 0; i < num; i++) {
		int key = i;

		pmu_fds[i] = sys_perf_event_open(&attr, -1/*pid*/, i/*cpu*/,
						 -1/*group_fd*/, 0);

		assert(pmu_fds[i] >= 0);
		assert(bpf_map_update_elem(map_fd, &key,
					   &pmu_fds[i], BPF_ANY) == 0);
		ioctl(pmu_fds[i], PERF_EVENT_IOC_ENABLE, 0);
	}
}

int main(int argc, char **argv)
{
	struct rlimit r = {100 * 1024 * 1024, RLIM_INFINITY};
	struct bpf_prog_load_attr prog_load_attr = {
		.prog_type	= BPF_PROG_TYPE_XDP,
	};
	struct bpf_map *perf_ring_map;
	struct bpf_object *obj;
	char filename[256];
	int longindex = 0;
	int prog_fd, opt;
	int map_fd, i;
	int numcpus;
	int err;

	numcpus = get_nprocs();
	if (numcpus > MAX_CPUS) {
		fprintf(stderr, "Cannot handle above %d CPUs\n", MAX_CPUS);
		return EXIT_FAIL_BPF; /* bpf map limitation */
	}

	snprintf(filename, sizeof(filename), "%s_kern.o", argv[0]);
	prog_load_attr.file = filename;

	if (setrlimit(RLIMIT_MEMLOCK, &r)) {
		perror("setrlimit(RLIMIT_MEMLOCK)");
		return EXIT_FAILURE;
	}

	/* Parse commands line args */
	while ((opt = getopt_long(argc, argv, "hSd:",
				  long_options, &longindex)) != -1) {
		switch (opt) {
		case 'd':
			if (strlen(optarg) >= IF_NAMESIZE) {
				fprintf(stderr, "ERR: --dev name too long\n");
				goto error;
			}
			ifname = (char *)&ifname_buf;
			strncpy(ifname, optarg, IF_NAMESIZE);
			ifindex = if_nametoindex(ifname);
			if (ifindex == 0) {
				fprintf(stderr,
					"ERR: --dev name unknown err(%d):%s\n",
					errno, strerror(errno));
				goto error;
			}
			break;
		case 'S':
			xdp_flags |= XDP_FLAGS_SKB_MODE;
			break;
		case 'h':
		error:
		default:
			usage(argv);
			return EXIT_FAIL_OPTION;
		}
	}
	/* Required option */
	if (ifindex == -1) {
		fprintf(stderr, "ERR: required option --dev missing\n");
		usage(argv);
		return EXIT_FAIL_OPTION;
	}

	if (bpf_prog_load_xattr(&prog_load_attr, &obj, &prog_fd))
		return EXIT_FAIL_BPF;

	if (!prog_fd) {
		fprintf(stderr, "ERR: load_bpf_file: %s\n", strerror(errno));
		return EXIT_FAIL_BPF;
	}

	perf_ring_map = bpf_map__next(NULL, obj); // TODO find by name
	if (!perf_ring_map) {
		fprintf(stderr, "Failed loading map in obj file\n");
		return EXIT_FAIL_BPF;
	}
	map_fd = bpf_map__fd(perf_ring_map);

	if (bpf_set_link_xdp_fd(ifindex, prog_fd, xdp_flags) < 0) {
		fprintf(stderr, "link set xdp fd failed\n");
		return EXIT_FAIL_XDP;
	}

	/* Remove XDP program when program is interrupted or killed */
	signal(SIGINT,  exit_sig_handler);
	signal(SIGTERM, exit_sig_handler);

	setup_bpf_perf_event(map_fd, numcpus);

	for (i = 0; i < numcpus; i++)
		if (perf_event_mmap_header(pmu_fds[i], &headers[i]) < 0)
			return 1;

	err = perf_event_poller_multi(pmu_fds, headers, numcpus,
				      handle_perf_event);
	if (err)
		return EXIT_FAIL_XDP;

	return EXIT_SUCCESS;
}
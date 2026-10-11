/*   SPDX-License-Identifier: BSD-3-Clause
 *   persist_check: write known pattern to LBA range, or verify read-back.
 *   For SE_Review scenD: cross-restart data persistence check.
 */

#include "spdk/stdinc.h"

#include "spdk/nvme.h"
#include "spdk/env.h"
#include "spdk/string.h"
#include "spdk/log.h"

#define PATTERN_MUL 131U
#define PATTERN_ADD 17U

struct ctrlr_entry {
	struct spdk_nvme_ctrlr		*ctrlr;
	TAILQ_ENTRY(ctrlr_entry)	link;
	char				name[1024];
};

struct ns_entry {
	struct spdk_nvme_ctrlr	*ctrlr;
	struct spdk_nvme_ns	*ns;
	TAILQ_ENTRY(ns_entry)	link;
	struct spdk_nvme_qpair	*qpair;
};

static TAILQ_HEAD(, ctrlr_entry) g_controllers = TAILQ_HEAD_INITIALIZER(g_controllers);
static TAILQ_HEAD(, ns_entry) g_namespaces = TAILQ_HEAD_INITIALIZER(g_namespaces);
static struct spdk_nvme_transport_id g_trid = {};

static bool g_do_write = false;      /* true = write, false = verify(read) */
static uint64_t g_start_lba = 0;
static uint64_t g_io_count = 64;     /* number of 4096-byte I/Os */
static uint32_t g_io_size = 4096;
static int g_errors = 0;

struct io_seq {
	struct ns_entry	*ns_entry;
	void		*buf;
	uint64_t	 lba;
	uint32_t	 blocks;
	bool		 is_write;
	int		 is_completed;
};

static void
fill_pattern(uint8_t *buf, uint64_t lba, uint32_t len)
{
	uint32_t i;
	uint64_t seed = lba * 31 + 7;

	for (i = 0; i < len; i++) {
		buf[i] = (uint8_t)((seed + i * PATTERN_MUL + PATTERN_ADD) & 0xff);
	}
}

static int
check_pattern(uint8_t *buf, uint64_t lba, uint32_t len)
{
	uint32_t i;
	uint64_t seed = lba * 31 + 7;

	for (i = 0; i < len; i++) {
		if (buf[i] != (uint8_t)((seed + i * PATTERN_MUL + PATTERN_ADD) & 0xff)) {
			return 1;
		}
	}
	return 0;
}

static void
io_complete(void *arg, const struct spdk_nvme_cpl *completion)
{
	struct io_seq *seq = arg;

	if (spdk_nvme_cpl_is_error(completion)) {
		fprintf(stderr, "I/O error at LBA %" PRIu64 ": sct=%u sc=%u\n",
			seq->lba, completion->status.sct, completion->status.sc);
		g_errors++;
		seq->is_completed = 2;
		return;
	}

	if (!seq->is_write) {
		if (check_pattern(seq->buf, seq->lba, g_io_size) != 0) {
			fprintf(stderr, "PATTERN MISMATCH at LBA %" PRIu64 "\n", seq->lba);
			g_errors++;
			seq->is_completed = 3;
			return;
		}
	}

	seq->is_completed = 1;
}

static int
run_one_io(struct ns_entry *entry, uint64_t lba, bool is_write)
{
	struct io_seq seq;
	int rc;
	uint32_t sector_size = spdk_nvme_ns_get_sector_size(entry->ns);

	seq.ns_entry = entry;
	seq.buf = spdk_zmalloc(g_io_size, 4096, NULL, SPDK_ENV_NUMA_ID_ANY, SPDK_MALLOC_DMA);
	if (seq.buf == NULL) {
		fprintf(stderr, "buffer alloc failed\n");
		return -ENOMEM;
	}
	seq.lba = lba;
	seq.blocks = g_io_size / sector_size;
	seq.is_write = is_write;
	seq.is_completed = 0;

	if (is_write) {
		fill_pattern(seq.buf, lba, g_io_size);
		rc = spdk_nvme_ns_cmd_write(entry->ns, entry->qpair, seq.buf, lba, seq.blocks,
					    io_complete, &seq, 0);
	} else {
		rc = spdk_nvme_ns_cmd_read(entry->ns, entry->qpair, seq.buf, lba, seq.blocks,
					   io_complete, &seq, 0);
	}
	if (rc != 0) {
		fprintf(stderr, "submit failed rc=%d at LBA %" PRIu64 "\n", rc, lba);
		spdk_free(seq.buf);
		return rc;
	}

	while (seq.is_completed == 0) {
		spdk_nvme_qpair_process_completions(entry->qpair, 0);
	}

	rc = (seq.is_completed == 1) ? 0 : -EIO;
	spdk_free(seq.buf);
	return rc;
}

static void
run_persist_check(void)
{
	struct ns_entry *ns_entry;
	uint64_t i, lba;

	TAILQ_FOREACH(ns_entry, &g_namespaces, link) {
		ns_entry->qpair = spdk_nvme_ctrlr_alloc_io_qpair(ns_entry->ctrlr, NULL, 0);
		if (ns_entry->qpair == NULL) {
			printf("ERROR: spdk_nvme_ctrlr_alloc_io_qpair() failed\n");
			return;
		}

		printf("%s %" PRIu64 " I/Os of %u bytes starting LBA %" PRIu64 "\n",
		       g_do_write ? "Writing" : "Verifying", g_io_count, g_io_size, g_start_lba);

		for (i = 0; i < g_io_count; i++) {
			lba = g_start_lba + i * (g_io_size / spdk_nvme_ns_get_sector_size(ns_entry->ns));
			if (run_one_io(ns_entry, lba, g_do_write) != 0) {
				printf("FAILED at offset %" PRIu64 " (LBA %" PRIu64 ")\n", i, lba);
				break;
			}
		}

		if (g_errors == 0 && i == g_io_count) {
			printf("%s: %s OK (%" PRIu64 " I/Os)\n",
			       g_do_write ? "WRITE" : "VERIFY",
			       g_do_write ? "wrote" : "read-back matched", g_io_count);
		} else {
			printf("%s: FAILED, errors=%d done=%" PRIu64 "\n",
			       g_do_write ? "WRITE" : "VERIFY", g_errors, i);
		}

		spdk_nvme_ctrlr_free_io_qpair(ns_entry->qpair);
		ns_entry->qpair = NULL;
	}
}

static bool
probe_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	 struct spdk_nvme_ctrlr_opts *opts)
{
	return true;
}

static void
attach_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	  struct spdk_nvme_ctrlr *ctrlr, const struct spdk_nvme_ctrlr_opts *opts)
{
	int nsid;
	struct ctrlr_entry *entry;
	struct spdk_nvme_ns *ns;
	const struct spdk_nvme_ctrlr_data *cdata;

	entry = malloc(sizeof(struct ctrlr_entry));
	if (entry == NULL) {
		perror("ctrlr_entry malloc");
		exit(1);
	}

	printf("Attached to %s\n", trid->traddr);
	cdata = spdk_nvme_ctrlr_get_data(ctrlr);
	snprintf(entry->name, sizeof(entry->name), "%-20.20s (%-20.20s)", cdata->mn, cdata->sn);
	entry->ctrlr = ctrlr;
	TAILQ_INSERT_TAIL(&g_controllers, entry, link);

	for (nsid = spdk_nvme_ctrlr_get_first_active_ns(ctrlr); nsid != 0;
	     nsid = spdk_nvme_ctrlr_get_next_active_ns(ctrlr, nsid)) {
		ns = spdk_nvme_ctrlr_get_ns(ctrlr, nsid);
		if (ns == NULL) {
			continue;
		}
		struct ns_entry *ne = malloc(sizeof(struct ns_entry));
		if (ne == NULL) {
			perror("ns_entry malloc");
			exit(1);
		}
		ne->ctrlr = ctrlr;
		ne->ns = ns;
		ne->qpair = NULL;
		TAILQ_INSERT_TAIL(&g_namespaces, ne, link);
		printf("  Namespace ID: %d size: %juGB\n", spdk_nvme_ns_get_id(ns),
		       spdk_nvme_ns_get_size(ns) / 1000000000);
	}
}

static void
cleanup(void)
{
	struct ns_entry *ns_entry, *tmp_ns_entry;
	struct ctrlr_entry *ctrlr_entry, *tmp_ctrlr_entry;
	struct spdk_nvme_detach_ctx *detach_ctx = NULL;

	TAILQ_FOREACH_SAFE(ns_entry, &g_namespaces, link, tmp_ns_entry) {
		TAILQ_REMOVE(&g_namespaces, ns_entry, link);
		free(ns_entry);
	}

	TAILQ_FOREACH_SAFE(ctrlr_entry, &g_controllers, link, tmp_ctrlr_entry) {
		TAILQ_REMOVE(&g_controllers, ctrlr_entry, link);
		spdk_nvme_detach_async(ctrlr_entry->ctrlr, &detach_ctx);
		free(ctrlr_entry);
	}

	if (detach_ctx) {
		spdk_nvme_detach_poll(detach_ctx);
	}
}

static void
usage(const char *program_name)
{
	printf("%s options:\n", program_name);
	printf("  -r, --transport <trid>  required, URMA trid\n");
	printf("  -w, --workload <op>     write or verify (default: write)\n");
	printf("  -o, --io-size <bytes>   I/O size (default: 4096)\n");
	printf("  -l, --start-lba <lba>   first LBA (default: 0)\n");
	printf("  -c, --count <n>         number of I/Os (default: 64)\n");
	printf("  -h, --help              show this help\n");
}

#define GETOPT_SHORT "r:w:o:l:c:h"

static const struct option g_cmdline_opts[] = {
	{"transport", required_argument, NULL, 'r'},
	{"workload", required_argument, NULL, 'w'},
	{"io-size", required_argument, NULL, 'o'},
	{"start-lba", required_argument, NULL, 'l'},
	{"count", required_argument, NULL, 'c'},
	{"help", no_argument, NULL, 'h'},
	{0, 0, 0, 0}
};

static int
parse_args(int argc, char **argv)
{
	int op, long_idx;
	long int val;

	spdk_nvme_trid_populate_transport(&g_trid, SPDK_NVME_TRANSPORT_URMA);

	while ((op = getopt_long(argc, argv, GETOPT_SHORT, g_cmdline_opts, &long_idx)) != -1) {
		switch (op) {
		case 'r':
			if (spdk_nvme_transport_id_parse(&g_trid, optarg) != 0) {
				fprintf(stderr, "Error parsing transport address\n");
				return 1;
			}
			break;
		case 'w':
			if (strcmp(optarg, "write") == 0) {
				g_do_write = true;
			} else if (strcmp(optarg, "verify") == 0) {
				g_do_write = false;
			} else {
				return 1;
			}
			break;
		case 'o':
			val = spdk_strtol(optarg, 10);
			if (val <= 0) {
				return 1;
			}
			g_io_size = (uint32_t)val;
			break;
		case 'l':
			val = spdk_strtol(optarg, 10);
			if (val < 0) {
				return 1;
			}
			g_start_lba = (uint64_t)val;
			break;
		case 'c':
			val = spdk_strtol(optarg, 10);
			if (val <= 0) {
				return 1;
			}
			g_io_count = (uint64_t)val;
			break;
		case 'h':
			usage(argv[0]);
			exit(0);
		default:
			usage(argv[0]);
			return 1;
		}
	}
	return 0;
}

int
main(int argc, char **argv)
{
	int rc;
	struct spdk_env_opts opts;

	opts.opts_size = sizeof(opts);
	spdk_env_opts_init(&opts);
	rc = parse_args(argc, argv);
	if (rc != 0) {
		return rc;
	}

	opts.name = "persist_check";
	if (spdk_env_init(&opts) < 0) {
		fprintf(stderr, "Unable to initialize SPDK env\n");
		return 1;
	}

	printf("Initializing NVMe Controllers\n");
	rc = spdk_nvme_probe(&g_trid, NULL, probe_cb, attach_cb, NULL);
	if (rc != 0) {
		fprintf(stderr, "spdk_nvme_probe() failed\n");
		rc = 1;
		goto exit;
	}

	if (TAILQ_EMPTY(&g_controllers)) {
		fprintf(stderr, "no NVMe controllers found\n");
		rc = 1;
		goto exit;
	}

	printf("Initialization complete.\n");
	run_persist_check();

	if (g_errors != 0) {
		rc = 1;
	}

exit:
	fflush(stdout);
	cleanup();
	spdk_env_fini();
	return rc;
}

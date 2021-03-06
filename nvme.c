/*
 * nvme.c -- NVM-Express command line utility.
 * 
 * Copyright (c) 2014, Intel Corporation.
 *
 * Written by Keith Busch <keith.busch@intel.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * This program uses NVMe IOCTLs to run native nvme commands to a device.
 */

#include <errno.h>
#include <getopt.h>
#include <fcntl.h>
#include <inttypes.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef LIBUDEV_EXISTS
#include <libudev.h>
#endif

#include <linux/fs.h>

#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "linux/nvme.h"

static int fd;
static struct stat nvme_stat;
static const char *devicename;

#define COMMAND_LIST \
	ENTRY(LIST, "list", "List all NVMe devices and namespaces on machine", list) \
	ENTRY(ID_CTRL, "id-ctrl", "Send NVMe Identify Controller", id_ctrl) \
	ENTRY(ID_NS, "id-ns", "Send NVMe Identify Namespace, display structure", id_ns) \
	ENTRY(LIST_NS, "list-ns", "Send NVMe Identify List, display structure", list_ns) \
	ENTRY(GET_NS_ID, "get-ns-id", "Retrieve the namespace ID of opened block device", get_ns_id) \
	ENTRY(GET_LOG, "get-log", "Generic NVMe get log, returns log in raw format", get_log) \
	ENTRY(GET_FW_LOG, "fw-log", "Retrieve FW Log, show it", get_fw_log) \
	ENTRY(GET_SMART_LOG, "smart-log", "Retrieve SMART Log, show it", get_smart_log) \
	ENTRY(GET_ERR_LOG, "error-log", "Retrieve Error Log, show it", get_error_log) \
	ENTRY(GET_FEATURE, "get-feature", "Get feature and show the resulting value", get_feature) \
	ENTRY(SET_FEATURE, "set-feature", "Set a feature and show the resulting value", set_feature) \
	ENTRY(FORMAT, "format", "Format namespace with new block format", format) \
	ENTRY(FW_ACTIVATE, "fw-activate", "Activate new firmware slot", fw_activate) \
	ENTRY(FW_DOWNLOAD, "fw-download", "Download new firmware", fw_download) \
	ENTRY(ADMIN_PASSTHRU, "admin-passthru", "Submit arbitrary admin command, return results", admin_passthru) \
	ENTRY(IO_PASSTHRU, "io-passthru", "Submit an arbitrary IO command, return results", io_passthru) \
	ENTRY(SECURITY_SEND, "security-send", "Submit a Security Send command, return results", sec_send) \
	ENTRY(SECURITY_RECV, "security-recv", "Submit a Security Receive command, return results", sec_recv) \
	ENTRY(RESV_ACQUIRE, "resv-acquire", "Submit a Reservation Acquire, return results", resv_acquire) \
	ENTRY(RESV_REGISTER, "resv-register", "Submit a Reservation Register, return results", resv_register) \
	ENTRY(RESV_RELEASE, "resv-release", "Submit a Reservation Release, return results", resv_release) \
	ENTRY(RESV_REPORT, "resv-report", "Submit a Reservation Report, return results", resv_report) \
	ENTRY(FLUSH, "flush", "Submit a Flush command, return results", flush) \
	ENTRY(COMPARE, "compare", "Submit a Comapre command, return results", compare) \
	ENTRY(READ_CMD, "read", "Submit a read command, return results", read_cmd) \
	ENTRY(WRITE_CMD, "write", "Submit a write command, return results", write_cmd) \
	ENTRY(REGISTERS, "show-regs", "Shows the controller registers. Requires admin character device", show_registers) \
	ENTRY(HELP, "help", "Display this help", help)

#define ENTRY(i, n, h, f) \
static int f(int argc, char **argv);
COMMAND_LIST
#undef ENTRY

enum {
	#define ENTRY(i, n, h, f) i,
	COMMAND_LIST
	#undef ENTRY
	NUM_COMMANDS
};

struct command {
	char *name;
	char *help;
	char *path;
	char *man;
	int (*fn)(int argc, char **argv);
};

struct command commands[] = {
	#define ENTRY(i, n, h, f)\
	{ \
		.name = n, \
		.help = h, \
		.fn = f, \
		.path = "Documentation/nvme-"n".1", \
		.man = "nvme-"n, \
	},
	COMMAND_LIST
	#undef ENTRY
};

static void open_dev(const char *dev)
{
	int err;
	devicename = dev;
	fd = open(dev, O_RDONLY);
	if (fd < 0)
		goto perror;

	err = fstat(fd, &nvme_stat);
	if (err < 0)
		goto perror;
	if (!S_ISCHR(nvme_stat.st_mode) && !S_ISBLK(nvme_stat.st_mode)) {
		fprintf(stderr, "%s is not a block or character device\n", dev);
		exit(ENODEV);
	}
	return;
 perror:
	perror(dev);
	exit(errno);
}

static void get_dev(int optind, int argc, char **argv)
{
	if (optind >= argc) {
		errno = EINVAL;
		perror(argv[0]);
		exit(errno);
	}
	open_dev((const char *)argv[optind]);
}

static void get_long(char *optarg, __u64 *val)
{
	if (sscanf(optarg, "%lli", val) == 1)
		return;
	fprintf(stderr, "bad param for command value:%s\n", optarg);
	exit(EINVAL);
}

static void get_int(char *optarg, __u32 *val)
{
	if (sscanf(optarg, "%i", val) == 1)
		return;
	fprintf(stderr, "bad param for command value:%s\n", optarg);
	exit(EINVAL);
}

static void get_short(char *optarg, __u16 *val)
{
	if (sscanf(optarg, "%hi", val) == 1)
		return;
	fprintf(stderr, "bad param for command value:%s\n", optarg);
	exit(EINVAL);
}

static void get_byte(char *optarg, __u8 *val)
{
	if (sscanf(optarg, "%hhi", val) == 1)
		return;
	fprintf(stderr, "bad param for command value:%s\n", optarg);
	exit(EINVAL);
}

static void show_error_log(struct nvme_error_log_page *err_log, int entries)
{
	int i;
	
	printf("Error Log Entries for device:%s entries:%d\n", devicename,
								entries);
	printf(".................\n");
	for (i = 0; i < entries; i++) {
		printf(" Entry[%2d]   \n", i);
		printf(".................\n");
		printf("error_count  : %lld\n", err_log[i].error_count);
		printf("sqid         : %d\n", err_log[i].sqid);
		printf("cmdid        : %#x\n", err_log[i].cmdid);
		printf("status_field : %#x\n", err_log[i].status_field);
		printf("parm_err_loc : %#x\n", err_log[i].parm_error_location);
		printf("lba          : %#llx\n", err_log[i].lba);
		printf("nsid         : %d\n", err_log[i].nsid);
		printf("vs           : %d\n", err_log[i].vs);
		printf(".................\n");
	}
}

static void show_nvme_resv_report(struct nvme_reservation_status *status)
{
	int i, regctl;

	regctl = status->regctl[0] | (status->regctl[1] << 8);

	printf("\nNVME Reservatation status:\n\n");
	printf("gen       : %d\n", le32toh(status->gen));
	printf("regctl    : %d\n", regctl);
	printf("rtype     : %d\n", status->rtype);
	printf("ptpls     : %d\n", status->ptpls);

	for (i = 0; i < regctl; i++) {
		printf("regctl[%d] :\n", i);
		printf("  cntlid  : %x\n", le16toh(status->regctl_ds[i].cntlid));
		printf("  rcsts   : %x\n", status->regctl_ds[i].rcsts);
		printf("  hostid  : %llx\n", le64toh(status->regctl_ds[i].hostid));
		printf("  rkey    : %llx\n", le64toh(status->regctl_ds[i].rkey));
	}
	printf("\n");
}

static char *fw_to_string(__u64 fw)
{
	static char ret[9];
	char *c = (char *)&fw;
	int i;

	for (i = 0; i < 8; i++)
		ret[i] = c[i] >= '!' && c[i] <= '~' ? c[i] : '.';
	ret[i] = '\0';
	return ret;
}

static void show_fw_log(struct nvme_firmware_log_page *fw_log)
{
	int i;

	printf("Firmware Log for device:%s\n", devicename);
	printf("afi  : %#x\n", fw_log->afi);
	for (i = 0; i < 8; i++)
		if (fw_log->frs[i])
			printf("frs%d : %#016llx (%s)\n", i + 1, fw_log->frs[i],
						fw_to_string(fw_log->frs[i]));
}

static long double int128_to_double(__u8 *data)
{
	int i;
	long double result = 0;

	for (i = 0; i < 16; i++) {
		result *= 256;
		result += data[15 - i];
	}
	return result;
}

static void show_smart_log(struct nvme_smart_log *smart, unsigned int nsid)
{
	/* convert temperature from Kelvin to Celsius */
	unsigned int temperature = ((smart->temperature[1] << 8) |
		smart->temperature[0]) - 273;

	printf("Smart Log for NVME device:%s namespace-id:%x\n", devicename, nsid);
	printf("critical_warning          : %#x\n", smart->critical_warning);
	printf("temperature               : %u C\n", temperature);
	printf("available_spare           : %u%%\n", smart->avail_spare);
	printf("available_spare_threshold : %u%%\n", smart->spare_thresh);
	printf("percentage_used           : %u%%\n", smart->percent_used);
	printf("data_units_read           : %'.0Lf\n",
		int128_to_double(smart->data_units_read));
	printf("data_units_written        : %'.0Lf\n",
		int128_to_double(smart->data_units_written));
	printf("host_read_commands        : %'.0Lf\n",
		int128_to_double(smart->host_reads));
	printf("host_write_commands       : %'.0Lf\n",
		int128_to_double(smart->host_writes));
	printf("controller_busy_time      : %'.0Lf\n",
		int128_to_double(smart->ctrl_busy_time));
	printf("power_cycles              : %'.0Lf\n",
		int128_to_double(smart->power_cycles));
	printf("power_on_hours            : %'.0Lf\n",
		int128_to_double(smart->power_on_hours));
	printf("unsafe_shutdowns          : %'.0Lf\n",
		int128_to_double(smart->unsafe_shutdowns));
	printf("media_errors              : %'.0Lf\n",
		int128_to_double(smart->media_errors));
	printf("num_err_log_entries       : %'.0Lf\n",
		int128_to_double(smart->num_err_log_entries));
}

char* nvme_feature_to_string(int feature)
{
	switch (feature)
	{
	case NVME_FEAT_ARBITRATION:	return "Arbitration";
	case NVME_FEAT_POWER_MGMT:	return "Power Management";
	case NVME_FEAT_LBA_RANGE:	return "LBA Range";
	case NVME_FEAT_TEMP_THRESH:	return "Temperature Threshold";
	case NVME_FEAT_ERR_RECOVERY:	return "Error Recovery";
	case NVME_FEAT_VOLATILE_WC:	return "Volatile Write Cache";
	case NVME_FEAT_NUM_QUEUES:	return "Number of Queues";
	case NVME_FEAT_IRQ_COALESCE:	return "IRQ Coalescing";
	case NVME_FEAT_IRQ_CONFIG: 	return "IRQ Configuration";
	case NVME_FEAT_WRITE_ATOMIC:	return "Write Atomicity";
	case NVME_FEAT_ASYNC_EVENT:	return "Async Event";
	case NVME_FEAT_SW_PROGRESS:	return "Software Progress";
	default:			return "Unknown";
	}
}

static const char *nvme_status_to_string(__u32 status)
{
	switch (status & 0x3ff) {
	case NVME_SC_SUCCESS:		return "SUCCESS";
	case NVME_SC_INVALID_OPCODE:	return "INVALID_OPCODE";
	case NVME_SC_INVALID_FIELD:	return "INVALID_FIELD";
	case NVME_SC_CMDID_CONFLICT:	return "CMDID_CONFLICT";
	case NVME_SC_DATA_XFER_ERROR:	return "DATA_XFER_ERROR";
	case NVME_SC_POWER_LOSS:	return "POWER_LOSS";
	case NVME_SC_INTERNAL:		return "INTERNAL";
	case NVME_SC_ABORT_REQ:		return "ABORT_REQ";
	case NVME_SC_ABORT_QUEUE:	return "ABORT_QUEUE";
	case NVME_SC_FUSED_FAIL:	return "FUSED_FAIL";
	case NVME_SC_FUSED_MISSING:	return "FUSED_MISSING";
	case NVME_SC_INVALID_NS:	return "INVALID_NS";
	case NVME_SC_CMD_SEQ_ERROR:	return "CMD_SEQ_ERROR";
	case NVME_SC_LBA_RANGE:		return "LBA_RANGE";
	case NVME_SC_CAP_EXCEEDED:	return "CAP_EXCEEDED";
	case NVME_SC_NS_NOT_READY:	return "NS_NOT_READY";
	case NVME_SC_CQ_INVALID:	return "CQ_INVALID";
	case NVME_SC_QID_INVALID:	return "QID_INVALID";
	case NVME_SC_QUEUE_SIZE:	return "QUEUE_SIZE";
	case NVME_SC_ABORT_LIMIT:	return "ABORT_LIMIT";
	case NVME_SC_ABORT_MISSING:	return "ABORT_MISSING";
	case NVME_SC_ASYNC_LIMIT:	return "ASYNC_LIMIT";
	case NVME_SC_FIRMWARE_SLOT:	return "FIRMWARE_SLOT";
	case NVME_SC_FIRMWARE_IMAGE:	return "FIRMWARE_IMAGE";
	case NVME_SC_INVALID_VECTOR:	return "INVALID_VECTOR";
	case NVME_SC_INVALID_LOG_PAGE:	return "INVALID_LOG_PAGE";
	case NVME_SC_INVALID_FORMAT:	return "INVALID_FORMAT";
	case NVME_SC_BAD_ATTRIBUTES:	return "BAD_ATTRIBUTES";
	case NVME_SC_WRITE_FAULT:	return "WRITE_FAULT";
	case NVME_SC_READ_ERROR:	return "READ_ERROR";
	case NVME_SC_GUARD_CHECK:	return "GUARD_CHECK";
	case NVME_SC_APPTAG_CHECK:	return "APPTAG_CHECK";
	case NVME_SC_REFTAG_CHECK:	return "REFTAG_CHECK";
	case NVME_SC_COMPARE_FAILED:	return "COMPARE_FAILED";
	case NVME_SC_ACCESS_DENIED:	return "ACCESS_DENIED";
	default:			return "Unknown";
	}
}

static int identify(int namespace, void *ptr, int cns)
{
	struct nvme_admin_cmd cmd;

	memset(&cmd, 0, sizeof(cmd));
	cmd.opcode = nvme_admin_identify;
	cmd.nsid = namespace;
	cmd.addr = (unsigned long)ptr;
	cmd.data_len = 4096;
	cmd.cdw10 = cns;
	return ioctl(fd, NVME_IOCTL_ADMIN_CMD, &cmd);
}

static int nvme_get_log(void *log_addr, __u32 data_len, __u32 dw10, __u32 nsid)
{
	struct nvme_admin_cmd cmd;

	memset(&cmd, 0, sizeof(cmd));
	cmd.opcode = nvme_admin_get_log_page;
	cmd.addr = (unsigned long)log_addr;
	cmd.data_len = data_len;
	cmd.cdw10 = dw10;
	cmd.nsid = nsid;
	return ioctl(fd, NVME_IOCTL_ADMIN_CMD, &cmd);
}

static void d_raw(unsigned char *buf, unsigned len)
{
	unsigned i;
	for (i = 0; i < len; i++)
		putchar(*(buf+i));
}

static void d(unsigned char *buf, int len, int width, int group)
{
	int i, offset = 0, line_done = 0;
	char ascii[width + 1];

	printf("     ");
	for (i = 0; i <= 15; i++)
		printf("%3x", i);
	for (i = 0; i < len; i++) {
		line_done = 0;
		if (i % width == 0)
			fprintf(stdout, "\n%04x:", offset);
		if (i % group == 0)
			fprintf(stdout, " %02x", buf[i]);
		else
			fprintf(stdout, "%02x", buf[i]);
		ascii[i % width] = (buf[i] >= '!' && buf[i] <= '~') ? buf[i] : '.';
		if (((i + 1) % width) == 0) {
			ascii[i % width + 1] = '\0';
			fprintf(stdout, " \"%.*s\"", width, ascii);
			offset += width;
			line_done = 1;
		}
	}
	if (!line_done) {
		unsigned b = width - (i % width);
		ascii[i % width + 1] = '\0';
		fprintf(stdout, " %*s \"%.*s\"",
				2 * b + b / group + (b % group ? 1 : 0), "",
				width, ascii);
	}
	fprintf(stdout, "\n");
}

static void show_nvme_id_ctrl(struct nvme_id_ctrl *ctrl, int vs)
{
	int i;
	char sn[sizeof(ctrl->sn) + 1];
	char mn[sizeof(ctrl->mn) + 1];
	char fr[sizeof(ctrl->fr) + 1];

	memcpy(sn, ctrl->sn, sizeof(ctrl->sn));
	memcpy(mn, ctrl->mn, sizeof(ctrl->mn));
	memcpy(fr, ctrl->fr, sizeof(ctrl->fr));

	sn[sizeof(ctrl->sn)] = '\0';
	mn[sizeof(ctrl->mn)] = '\0';
	fr[sizeof(ctrl->fr)] = '\0';

	printf("NVME Identify Controller:\n");
	printf("vid     : %#x\n", ctrl->vid);
	printf("ssvid   : %#x\n", ctrl->ssvid);
	printf("sn      : %s\n", sn);
	printf("mn      : %s\n", mn);
	printf("fr      : %s\n", fr);
	printf("rab     : %d\n", ctrl->rab);
	printf("ieee    : %02x%02x%02x\n",
		ctrl->ieee[0], ctrl->ieee[1], ctrl->ieee[2]);
	printf("cmic    : %#x\n", ctrl->cmic);
	printf("mdts    : %d\n", ctrl->mdts);
	printf("cntlid  : %x\n", ctrl->cntlid);
	printf("ver     : %x\n", ctrl->ver);
	printf("rtd3r   : %x\n", ctrl->rtd3r);
	printf("rtd3e   : %x\n", ctrl->rtd3e);
	printf("oacs    : %#x\n", ctrl->oacs);
	printf("acl     : %d\n", ctrl->acl);
	printf("aerl    : %d\n", ctrl->aerl);
	printf("frmw    : %#x\n", ctrl->frmw);
	printf("lpa     : %#x\n", ctrl->lpa);
	printf("elpe    : %d\n", ctrl->elpe);
	printf("npss    : %d\n", ctrl->npss);
	printf("avscc   : %#x\n", ctrl->avscc);
	printf("apsta   : %#x\n", ctrl->apsta);
	printf("wctemp  : %d\n", ctrl->wctemp);
	printf("cctemp  : %d\n", ctrl->cctemp);
	printf("mtfa    : %d\n", ctrl->mtfa);
	printf("hmmin   : %d\n", ctrl->hmmin);
	printf("tnvmcap : %.0Lf\n", int128_to_double(ctrl->tnvmcap));
	printf("unvmcap : %.0Lf\n", int128_to_double(ctrl->unvmcap));
	printf("rpmbs   : %#x\n", ctrl->rpmbs);
	printf("sqes    : %#x\n", ctrl->sqes);
	printf("cqes    : %#x\n", ctrl->cqes);
	printf("nn      : %d\n", ctrl->nn);
	printf("oncs    : %#x\n", ctrl->oncs);
	printf("fuses   : %#x\n", ctrl->fuses);
	printf("fna     : %#x\n", ctrl->fna);
	printf("vwc     : %#x\n", ctrl->vwc);
	printf("awun    : %d\n", ctrl->awun);
	printf("awupf   : %d\n", ctrl->awupf);
	printf("nvscc   : %d\n", ctrl->nvscc);
	printf("acwu    : %d\n", ctrl->acwu);
	printf("sgls    : %d\n", ctrl->sgls);

	for (i = 0; i <= ctrl->npss; i++) {
		printf("ps %4d : mp:%d flags:%x enlat:%d exlat:%d rrt:%d rrl:%d\n"
			"          rwt:%d rwl:%d idlp:%d ips:%x actp:%x ap flags:%x\n",
			i, ctrl->psd[i].max_power, ctrl->psd[i].flags,
			ctrl->psd[i].entry_lat, ctrl->psd[i].exit_lat,
			ctrl->psd[i].read_tput, ctrl->psd[i].read_lat,
			ctrl->psd[i].write_tput, ctrl->psd[i].write_lat,
			ctrl->psd[i].idle_power, ctrl->psd[i].idle_scale,
			ctrl->psd[i].active_power, ctrl->psd[i].active_work_scale);
	}
	if (vs) {
		printf("vs[]:\n");
		d(ctrl->vs, sizeof(ctrl->vs), 16, 1);
	}
}

static void show_lba_range(struct nvme_lba_range_type *lbrt, int nr_ranges)
{
	int i, j;

	for (i = 0; i < nr_ranges; i++) {
		printf("type       : %#x\n", lbrt[i].type);
		printf("attributes : %#x\n", lbrt[i].attributes);
		printf("slba       : %#llx", lbrt[i].slba);
		printf("nlb        : %#llx", lbrt[i].nlb);
		printf("guid       : ");
		for (j = 0; j < 16; j++)
			printf("%02x", lbrt[i].guid[j]);
		printf("\n");
	}
}

static void show_nvme_id_ns(struct nvme_id_ns *ns, int id, int vs)
{
	int i;

	printf("NVME Identify Namespace %d:\n", id);
	printf("nsze    : %#llx\n", ns->nsze);
	printf("ncap    : %#llx\n", ns->ncap);
	printf("nuse    : %#llx\n", ns->nuse);
	printf("nsfeat  : %#x\n", ns->nsfeat);
	printf("nlbaf   : %d\n", ns->nlbaf);
	printf("flbas   : %#x\n", ns->flbas);
	printf("mc      : %#x\n", ns->mc);
	printf("dpc     : %#x\n", ns->dpc);
	printf("dps     : %#x\n", ns->dps);
	printf("nmic    : %#x\n", ns->nmic);
	printf("rescap  : %#x\n", ns->rescap);
	printf("fpi     : %#x\n", ns->fpi);
	printf("nawun   : %d\n", ns->nawun);
	printf("nawupf  : %d\n", ns->nawupf);
	printf("nacwu   : %d\n", ns->nacwu);
	printf("nabsn   : %d\n", ns->nabsn);
	printf("nabo    : %d\n", ns->nabo);
	printf("nabspf  : %d\n", ns->nabspf);
	printf("nvmcap  : %.0Lf\n", int128_to_double(ns->nvmcap));

	printf("nguid   : ");
	for (i = 0; i < 16; i++)
		printf("%02x", ns->nguid[i]);
	printf("\n");

	printf("eui64   : ");
	for (i = 0; i < 8; i++)
		printf("%02x", ns->eui64[i]);
	printf("\n");

	for (i = 0; i <= ns->nlbaf; i++) {
		printf("lbaf %2d : ms:%-3d ds:%-2d rp:%#x %s\n", i,
			ns->lbaf[i].ms, ns->lbaf[i].ds, ns->lbaf[i].rp,
			i == (ns->flbas & 0xf) ? "(in use)" : "");
	}
	if (vs) {
		printf("vs[]:");
		d(ns->vs, sizeof(ns->vs), 16, 1);
	}
}

static int get_smart_log(int argc, char **argv)
{
	struct nvme_smart_log smart_log;
	int long_index, opt, err;
	unsigned int raw = 0, nsid = 0xffffffff;
	static struct option opts[] = {
		{"namespace-id", required_argument, 0, 'n'},
		{"raw-binary", no_argument, 0, 'b'},
		{0, 0, 0, 0 }
	};

	while ((opt = getopt_long(argc, (char **)argv, "n:b", opts,
							&long_index)) != -1) {
		switch (opt) {
		case 'n':
			get_int(optarg, &nsid);
			break;
		case 'b':
			raw = 1;
			break;
		default:
			return EINVAL;
		}
	}
	get_dev(optind, argc, argv);
	err = nvme_get_log(&smart_log,
		sizeof(smart_log), 0x2 | (((sizeof(smart_log) / 4) - 1) << 16),
		nsid);
	if (!err) {
		if (!raw)
			show_smart_log(&smart_log, nsid);
		else
			d_raw((unsigned char *)&smart_log, sizeof(smart_log));
	}
	else if (err > 0)
		fprintf(stderr, "NVMe Status: %s\n", nvme_status_to_string(err));
	return err;
}

static int get_error_log(int argc, char **argv)
{
	int opt, err, long_index = 0;
	unsigned int raw = 0, log_entries = 64, nsid = 0xffffffff;
	static struct option opts[] = {
		{"namespace-id", required_argument, 0, 'n'},
		{"log-entries", required_argument, 0, 'e'},
		{"raw-binary", no_argument, 0, 'b'},
		{0, 0, 0, 0 }
	};

	while ((opt = getopt_long(argc, (char **)argv, "n:e:b", opts,
							&long_index)) != -1) {
		switch (opt) {
		case 'n':
			get_int(optarg, &nsid);
			break;
		case 'e':
			get_int(optarg, &log_entries);
			break;
		case 'b':
			raw = 1;
			break;
		default:
			return EINVAL;
		}
	}
	get_dev(optind, argc, argv);
	if (!log_entries) {
		fprintf(stderr, "non-zero log-entires is required param\n");
		return EINVAL;
	} else {
		struct nvme_error_log_page err_log[log_entries];
	
		err = nvme_get_log(err_log,
				sizeof(err_log), 0x1 | (((sizeof(err_log) / 4) - 1) << 16),
				nsid);
		if (!err) {
			if (!raw)
				show_error_log(err_log, log_entries);
			else
				d_raw((unsigned char *)err_log, sizeof(err_log));
		}
		else if (err > 0)
			fprintf(stderr, "NVMe Status: %s\n", nvme_status_to_string(err));
		return err;
	}
}

static int get_fw_log(int argc, char **argv)
{
	int err, opt, long_index = 0, raw = 0;
	struct nvme_firmware_log_page fw_log;
	static struct option opts[] = {
		{"raw-binary", no_argument, 0, 'b'},
		{0, 0, 0, 0 }
	};

	while ((opt = getopt_long(argc, (char **)argv, "b", opts,
							&long_index)) != -1) {
		switch (opt) {
		case 'b':
			raw = 1;
			break;
		}
	}
	get_dev(optind, argc, argv);
	err = nvme_get_log(&fw_log,
			sizeof(fw_log), 0x3 | (((sizeof(fw_log) / 4) - 1) << 16),
			0xffffffff);
	if (!err) {
		if (!raw)
			show_fw_log(&fw_log);
		else
			d_raw((unsigned char *)&fw_log, sizeof(fw_log));
	}
	else if (err > 0)
		fprintf(stderr, "NVMe Status: %s\n", nvme_status_to_string(err));
	else
		perror("fw log");
	return err;
}

static int get_log(int argc, char **argv)
{
	int opt, err, long_index = 0;
	unsigned int raw = 0, lid = 0, log_len = 0, nsid = 0xffffffff;
	static struct option opts[] = {
		{"namespace-id", required_argument, 0, 'n'},
		{"log-id", required_argument, 0, 'i'},
		{"log-len", required_argument, 0, 'l'},
		{"raw-binary", no_argument, 0, 'b'},
		{0, 0, 0, 0 }
	};

	while ((opt = getopt_long(argc, (char **)argv, "n:i:l:b", opts,
							&long_index)) != -1) {
		switch (opt) {
		case 'n':
			get_int(optarg, &nsid);
			break;
		case 'i':
			get_int(optarg, &lid);
			break;
		case 'l':
			get_int(optarg, &log_len);
			break;
		case 'b':
			raw = 1;
			break;
		default:
			return EINVAL;
		}
	}
	get_dev(optind, argc, argv);
	if (!log_len) {
		fprintf(stderr, "non-zero log-len is required param\n");
		return EINVAL;
	} else {
		unsigned char log[log_len];

		err = nvme_get_log(log, log_len, lid | (((log_len / 4) - 1) << 16), nsid);
		if (!err) {
			if (!raw) {
				printf("Device:%s log-id:%d namespace-id:%#x",
								devicename, lid, nsid);
				d(log, log_len, 16, 1);
			} else
				d_raw((unsigned char *)log, log_len);
		} else if (err > 0)
			fprintf(stderr, "NVMe Status: %s\n",
						nvme_status_to_string(err));
		return err;
	}
}

static int list_ns(int argc, char **argv)
{
	int opt, err, i, long_index = 0;
	unsigned int nsid = 0;
	static struct option opts[] = {
		{"namespace-id", required_argument, 0, 'n'},
		{0, 0, 0, 0 }
	};
	__u32 ns_list[1024];

	while ((opt = getopt_long(argc, (char **)argv, "n:", opts,
							&long_index)) != -1) {
		switch (opt) {
		case 'n':
			get_int(optarg, &nsid);
			break;
		default:
			return EINVAL;
		}
	}
	get_dev(optind, argc, argv);
	err = identify(nsid, ns_list, 2);
	if (!err) {
		for (i = 0; i < 1024; i++)
			if (ns_list[i])
				printf("[%4u]:%#x\n", i, ns_list[i]);
	}
	else if (err > 0)
		fprintf(stderr, "NVMe Status: %s NSID:%d\n",
				nvme_status_to_string(err), nsid);
	return err;
}

#ifndef LIBUDEV_EXISTS
static int list(int argc, char **argv)
{
  fprintf(stderr,"nvme-list: libudev not detected, install and rebuild.\n");
  return -1;
}
#endif

#ifdef LIBUDEV_EXISTS
static int list(int argc, char **argv)
{
  struct udev *udev;
  struct udev_enumerate *enumerate;
  struct udev_list_entry *devices, *dev_list_entry;
  struct udev_device *dev;
  
  udev = udev_new();
  if (!udev) {
    perror("nvme-list: Can not create udev context.");
    return errno;
  }
  
  enumerate = udev_enumerate_new(udev);
  udev_enumerate_add_match_subsystem(enumerate, "block");
  udev_enumerate_scan_devices(enumerate);
  devices = udev_enumerate_get_list_entry(enumerate);
  udev_list_entry_foreach(dev_list_entry, devices) {

    const char *path, *node;
    path = udev_list_entry_get_name(dev_list_entry);
    dev = udev_device_new_from_syspath(udev, path);
    node = udev_device_get_devnode(dev);
    if (strstr(node,"nvme")!=NULL){
      struct nvme_id_ctrl ctrl;

      open_dev(node);
      int err = identify(0, &ctrl, 1);
      if (err > 0)
	return err;
      printf("  %s\t: NVM Express - %#x - %s - %x\n", node, 
	     ctrl.vid, ctrl.mn, ctrl.ver);
    }

  }
  udev_enumerate_unref(enumerate);
  udev_unref(udev);

  return 0;
}
#endif

static int id_ctrl(int argc, char **argv)
{
	int opt, err, raw = 0, vs = 0, long_index = 0;
	struct nvme_id_ctrl ctrl;
	static struct option opts[] = {
		{"vendor-specific", no_argument, 0, 'v'},
		{"raw-binary", no_argument, 0, 'b'},
		{0, 0, 0, 0 }
	};

	while ((opt = getopt_long(argc, (char **)argv, "vb", opts,
							&long_index)) != -1) {
		switch (opt) {
		case 'v':
			vs = 1;
			break;
		case 'b':
			raw = 1;
			break;
		default:
			return EINVAL;
		}
	}
	get_dev(optind, argc, argv);
	err = identify(0, &ctrl, 1);
	if (!err) {
		if (raw)
			d_raw((unsigned char *)&ctrl, sizeof(ctrl));
		else
			show_nvme_id_ctrl(&ctrl, vs);
	}
	else if (err > 0)
		fprintf(stderr, "NVMe Status: %s\n", nvme_status_to_string(err));

	return err;
}

static int id_ns(int argc, char **argv)
{
	struct nvme_id_ns ns;
	int opt, err, long_index = 0;
	unsigned int nsid = 0, vs = 0, raw = 0;

	static struct option opts[] = {
		{"namespace-id", required_argument, 0, 'n'},
		{"vendor-specific", no_argument, 0, 'v'},
		{"raw-binary", no_argument, 0, 'b'},
		{0, 0, 0, 0 }
	};

	while ((opt = getopt_long(argc, (char **)argv, "n:vb", opts,
							&long_index)) != -1) {
		switch (opt) {
		case 'n':
			get_int(optarg, &nsid);
			break;
		case 'v':
			vs = 1;
			break;
		case 'b':
			raw = 1;
			break;
		default:
			return EINVAL;
		}
	}
	get_dev(optind, argc, argv);
	if (!nsid) {
		if (!S_ISBLK(nvme_stat.st_mode)) {
			fprintf(stderr,
				"%s: non-block device requires namespace-id param\n",
				devicename);
			exit(ENOTBLK);
		}
		nsid = ioctl(fd, NVME_IOCTL_ID);
		if (nsid <= 0) {
			perror(devicename);
			exit(errno);
		}
	}
	err = identify(nsid, &ns, 0);
	if (!err) {
		if (raw)
			d_raw((unsigned char *)&ns, sizeof(ns));
		else
			show_nvme_id_ns(&ns, nsid, vs);
	}
	else if (err > 0)
		fprintf(stderr, "NVMe Status: %s NSID:%d\n", nvme_status_to_string(err), nsid);
	return err;
}

static int get_ns_id(int argc, char **argv)
{
	int nsid;

	open_dev(argv[1]);
	if (!S_ISBLK(nvme_stat.st_mode)) {
		fprintf(stderr, "%s: requesting nsid from non-block device\n",
								devicename);
		exit(ENOTBLK);
	}
	nsid = ioctl(fd, NVME_IOCTL_ID);
	if (nsid <= 0) {
		perror(devicename);
		exit(errno);
	}
	printf("%s: namespace-id:%d\n", devicename, nsid);
	return 0;
}

static int nvme_feature(int opcode, void *buf, int data_len, __u32 fid,
					__u32 nsid, __u32 cdw11, __u32 *result)
{
	int err;
	struct nvme_admin_cmd cmd;

	memset(&cmd, 0, sizeof(cmd));
	cmd.opcode = opcode;
	cmd.nsid = nsid;
	cmd.cdw10 = fid;
	cmd.cdw11 = cdw11;
	cmd.addr = (__u64)buf;
	cmd.data_len = data_len;

	err = ioctl(fd, NVME_IOCTL_ADMIN_CMD, &cmd);
	if (err >= 0 && result)
			*result = cmd.result;
	return err;
}

static int get_feature(int argc, char **argv)
{
	int opt, err, long_index = 0;
	unsigned int f, result, raw = 0, cdw10, cdw11 = 0, nsid = 0, data_len = 0;
	unsigned char sel = 0;
	void *buf = NULL;
	static struct option opts[] = {
		{"namespace-id", required_argument, 0, 'n'},
		{"feature-id", required_argument, 0, 'f'},
		{"sel", required_argument, 0, 's'},
		{"cdw11", required_argument, 0, 0},
		{"data-len", required_argument, 0, 'l'},
		{"raw-binary", no_argument, 0, 'b'},
		{0, 0, 0, 0 }
	};

	while ((opt = getopt_long(argc, (char **)argv, "n:f:l:s:b", opts,
							&long_index)) != -1) {
		switch (opt) {
		case 0:
			get_int(optarg, &cdw11);
			break;
		case 'n':
			get_int(optarg, &nsid);
			break;
		case 'f':
			get_int(optarg, &f);
			break;
		case 'l':
			get_int(optarg, &data_len);
			break;
		case 's':
			get_byte(optarg, &sel);
			break;
		case 'b':
			raw = 1;
			break;
		default:
			return EINVAL;
		}
	}
	get_dev(optind, argc, argv);
	if (sel > 7) {
		fprintf(stderr, "invalid 'select' param:%d\n", sel);
		return EINVAL;
	}
	if (!f) {
		fprintf(stderr, "feature-id required param\n");
		return EINVAL;
	}
	if (f == NVME_FEAT_LBA_RANGE)
		data_len = 4096;
	if (data_len)
		buf = malloc(data_len);

	cdw10 = sel << 8 | f;
	err = nvme_feature(nvme_admin_get_features, buf, data_len, cdw10, nsid,
							cdw11, &result);
	if (!err) {
		printf("get-feature:%d(%s), value:%#08x\n", f,
			nvme_feature_to_string(f), result);
		if (buf) {
			if (!raw) {
				if (f == NVME_FEAT_LBA_RANGE)
					show_lba_range((struct nvme_lba_range_type *)buf,
									result);
				else
					d(buf, data_len, 16, 1);
			}
			else
				d_raw(buf, data_len);
		}
	}
	else if (err > 0)
		fprintf(stderr, "NVMe Status: %s\n", nvme_status_to_string(err));
	if (buf)
		free(buf);
	return err;
}

#define min(x, y) x > y ? y : x;
static int fw_download(int argc, char **argv)
{
	int opt, err, long_index = 0, fw_fd = -1;
	unsigned int fw_size, xfer_size = 4096, offset = 0;
	struct stat sb;
	struct nvme_admin_cmd cmd;
	void *fw_buf;
	static struct option opts[] = {
		{"fw", required_argument, 0, 'f'},
		{"xfer", required_argument, 0, 'x'},
		{"offset", required_argument, 0, 'o'},
		{ 0, 0, 0, 0}
	};

	while ((opt = getopt_long(argc, (char **)argv, "x:f:o:", opts,
							&long_index)) != -1) {
		switch (opt) {
		case 'f':
			fw_fd = open(optarg, O_RDONLY);
			break;
		case 'x':
			get_int(optarg, &xfer_size);
			break;
		case 'p':
			get_int(optarg, &offset);
			offset <<= 2;
			break;
		default:
			return EINVAL;
		}
	}
	get_dev(optind, argc, argv);

	if (fw_fd < 0) {
		fprintf(stderr, "no firmware file provided\n");
		return EINVAL;
	}
	err = fstat(fw_fd, &sb);
	if (err < 0) {
		perror("fstat");
		exit(errno);
	}

	fw_size = sb.st_size;
	if (fw_size & 0x3) {
		fprintf(stderr, "Invalid size:%d for f/w image\n", fw_size);
		return EINVAL;
	} 
	if (posix_memalign(&fw_buf, getpagesize(), fw_size)) {
		fprintf(stderr, "No memory for f/w size:%d\n", fw_size);
		return ENOMEM;
	}
	if (xfer_size % 4096)
		xfer_size = 4096;

	while (fw_size > 0) {
		xfer_size = min(xfer_size, fw_size);

		memset(&cmd, 0, sizeof(cmd));
		cmd.opcode = nvme_admin_download_fw;
		cmd.addr = (__u64)fw_buf;
		cmd.data_len = xfer_size;
		cmd.cdw10 = (xfer_size >> 2) - 1;
		cmd.cdw11 = offset >> 2; 

		err = ioctl(fd, NVME_IOCTL_ADMIN_CMD, &cmd);
		if (err < 0) {
			perror("ioctl");
			exit(errno);
		} else if (err != 0) {
			fprintf(stderr, "NVME Admin command error:%d\n", err);
			break;
		}
		fw_buf += xfer_size;
		fw_size -= xfer_size;
		offset += xfer_size;
	}
	if (!err)
		printf("Firmware download success\n");
	return err;
}

static int fw_activate(int argc, char **argv)
{
	int opt, err, long_index;
	unsigned char slot = 0, action = 1;
	struct nvme_admin_cmd cmd;
	static struct option opts[] = {
		{"slot", required_argument, 0, 's'},
		{"action", required_argument, 0, 'a'},
		{ 0, 0, 0, 0}
	};

	while ((opt = getopt_long(argc, (char **)argv, "s:a:", opts,
							&long_index)) != -1) {
		switch (opt) {
		case 's':
			get_byte(optarg, &slot);
			if (slot > 7) {
				fprintf(stderr, "invalid slot:%d\n", slot);
				return EINVAL;
			}
			break;
		case 'a':
			get_byte(optarg, &action);
			if (action > 3) {
				fprintf(stderr, "invalid action:%d\n", action);
				return EINVAL;
			}
			break;
		default:
			return EINVAL;
		}
	}
	get_dev(optind, argc, argv);

	memset(&cmd, 0, sizeof(cmd));
	cmd.opcode = nvme_admin_activate_fw;
	cmd.cdw10 = (action << 3) | slot;

	err = ioctl(fd, NVME_IOCTL_ADMIN_CMD, &cmd);
	if (err < 0)
		perror("ioctl");
	else if (err != 0)
		fprintf(stderr, "NVME Admin command error:%d\n", err);
	else
		printf("Success activating firmware action:%d slot:%d\n", action, slot);
	return err;
}

static int show_registers(int argc, char **argv)
{
	int opt, long_index, pci_fd;
	char *base, path[512];
	void *membase;
	struct nvme_bar *bar;
	static struct option opts[] = {};

	while ((opt = getopt_long(argc, (char **)argv, "", opts,
					&long_index)) != -1);
	get_dev(optind, argc, argv);

	if (!S_ISCHR(nvme_stat.st_mode)) {
		fprintf(stderr, "%s is not character device\n", devicename);
		exit(ENODEV);
	}

	base = basename(devicename);
	sprintf(path, "/sys/class/misc/%s/device/resource0", base);
	pci_fd = open(path, O_RDONLY);
	if (pci_fd < 0) {
		fprintf(stderr, "%s did not find a pci resource\n", devicename);
		exit(ENODEV);
	}

	membase = mmap(0, getpagesize(), PROT_READ, MAP_SHARED, pci_fd, 0);
	if (!membase) {
		fprintf(stderr, "%s failed to map\n", devicename);
		exit(ENODEV);
	}

	bar = membase;
	printf("cap     : %"PRIx64"\n", (uint64_t)bar->cap);
	printf("version : %x\n", bar->vs);
	printf("intms   : %x\n", bar->intms);
	printf("intmc   : %x\n", bar->intmc);
	printf("cc      : %x\n", bar->cc);
	printf("csts    : %x\n", bar->csts);
	printf("nssr    : %x\n", bar->nssr);
	printf("aqa     : %x\n", bar->aqa);
	printf("asq     : %"PRIx64"\n", (uint64_t)bar->asq);
	printf("acq     : %"PRIx64"\n", (uint64_t)bar->acq);
	printf("cmbloc  : %x\n", bar->cmbloc);
	printf("cmbsz   : %x\n", bar->cmbsz);

	return 0;
}

static int format(int argc, char **argv)
{
	int opt, err, long_index;
	unsigned int nsid = 0xffffffff;
	unsigned char lbaf = 0, ses = 0, pil = 0, pi = 0, ms = 0;
	struct nvme_admin_cmd cmd;
	static struct option opts[] = {
		{"namespace-id", required_argument, 0, 'n'},
		{"lbaf", required_argument, 0, 'l'},
		{"ses", required_argument, 0, 's'},
		{"pil", required_argument, 0, 'p'},
		{"pi", required_argument, 0, 'i'},
		{"ms", required_argument, 0, 'm'},
		{ 0, 0, 0, 0}
	};

	while ((opt = getopt_long(argc, (char **)argv, "n:l:s:p:i:m:", opts,
							&long_index)) != -1) {
		switch (opt) {
		case 'n': get_int(optarg, &nsid); break;
		case 'l': get_byte(optarg, &lbaf); break;
		case 's': get_byte(optarg, &ses); break;
		case 'p': get_byte(optarg, &pil); break;
		case 'i': get_byte(optarg, &pi); break;
		case 'm': get_byte(optarg, &ms); break;
		default:
			return EINVAL;
		}
	}
	get_dev(optind, argc, argv);

	if (ms > 1) {
		fprintf(stderr, "invalid pi:%d\n", ms);
		return EINVAL;
	}
	if (pil > 1) {
		fprintf(stderr, "invalid pi location:%d\n", pil);
		return EINVAL;
	}
	if (ses > 7) {
		fprintf(stderr, "invalid secure erase settings:%d\n", ses);
		return EINVAL;
	}
	if (lbaf > 15) {
		fprintf(stderr, "invalid lbaf:%d\n", lbaf);
		return EINVAL;
	}
	if (pi > 7) {
		fprintf(stderr, "invalid pi:%d\n", pi);
		return EINVAL;
	}
	if (S_ISBLK(nvme_stat.st_mode)) {
		nsid = ioctl(fd, NVME_IOCTL_ID);
		if (nsid <= 0) {
			fprintf(stderr,
				"%s: failed to return namespace id\n",
				devicename);
			return errno;
		}
	}

	memset(&cmd, 0, sizeof(cmd));
	cmd.opcode = nvme_admin_format_nvm;
	cmd.nsid = nsid;
	cmd.cdw10 = (lbaf << 0) | (ms << 4) | (pi << 5) | (pil << 8) | (ses << 9);
	
	err = ioctl(fd, NVME_IOCTL_ADMIN_CMD, &cmd);
	if (err < 0)
		perror("ioctl");
	else if (err != 0)
		fprintf(stderr, "NVME Admin command error:%s(%d)\n",
					nvme_status_to_string(err), err);
	else {
		printf("Success formatting namespace:%x\n", nsid);
		ioctl(fd, BLKRRPART);
	}
	return err;
}

/* FIXME: read a buffer from a file if the feature requires one */
static int set_feature(int argc, char **argv)
{
	int opt, err, long_index = 0, val = -1;
	unsigned int f, v, result, nsid = 0, data_len = 0;
	void *buf = NULL;
	static struct option opts[] = {
		{"namespace-id", required_argument, 0, 'n'},
		{"feature-id", required_argument, 0, 'f'},
		{"value", required_argument, 0, 'v'},
		{"data-len", required_argument, 0, 'l'},
		{0, 0, 0, 0 }
	};

	while ((opt = getopt_long(argc, (char **)argv, "n:f:l:v:", opts,
							&long_index)) != -1) {
		switch (opt) {
		case 'n':
			get_int(optarg, &nsid);
			break;
		case 'f':
			get_int(optarg, &f);
			break;
		case 'l':
			get_int(optarg, &data_len);
			break;
		case 'v':
			get_int(optarg, &v);
			val = (int)v;
			break;
		default:
			return EINVAL;
		}
	}
	get_dev(optind, argc, argv);
	if (val == -1) {
		fprintf(stderr, "feature value required param\n");
		return EINVAL;
	}
	if (!f) {
		fprintf(stderr, "feature-id required param\n");
		return EINVAL;
	}
	if (f == NVME_FEAT_LBA_RANGE)
		data_len = 4096;
	if (data_len)
		buf = malloc(data_len);

	err = nvme_feature(nvme_admin_set_features, buf, data_len, f, nsid, v, &result);
	if (!err) {
		printf("set-feature:%d(%s), value:%#08x\n", f,
			nvme_feature_to_string(f), result);
		if (buf) {
			if (f == NVME_FEAT_LBA_RANGE)
				show_lba_range((struct nvme_lba_range_type *)buf,
								result);
			else
				d(buf, data_len, 16, 1);
		}
	}
	else if (err > 0)
		fprintf(stderr, "NVMe Status: %s\n", nvme_status_to_string(err));
	if (buf)
		free(buf);
	return err;
}

static int sec_send(int argc, char **argv)
{
	struct stat sb;
	struct nvme_admin_cmd cmd;
        int err, sec_fd = -1, opt, long_index = 0;
	void *sec_buf;
	unsigned int tl = 0;
	unsigned short spsp = 0;
	unsigned char secp = 0;
	unsigned int sec_size;
	static struct option opts[] = {
		{"file", required_argument, 0, 'f'},
		{"secp", required_argument, 0, 'p'},
		{"spsp", required_argument, 0, 's'},
		{"tl", required_argument, 0, 't'},
		{ 0, 0, 0, 0}
	};

	while ((opt = getopt_long(argc, (char **)argv, "f:p:s:t:", opts,
							&long_index)) != -1) {
		switch(opt) {
		case 'f':
			sec_fd = open(optarg, O_RDONLY);
			break;
		case 'p':
			get_short(optarg, &spsp);
			break;
		case 's':
			get_byte(optarg, &secp);
			break;
		case 't':
			get_int(optarg, &tl);
			break;
		default:
			return EINVAL;
		}
	}
	get_dev(optind, argc, argv);

	if (sec_fd < 0) {
		fprintf(stderr, "no firmware file provided\n");
		return EINVAL;
	}
	err = fstat(sec_fd, &sb);
	if (err < 0) {
		perror("fstat");
		return errno;
	}
	sec_size = sb.st_size;
	if (posix_memalign(&sec_buf, getpagesize(), sec_size)) {
		fprintf(stderr, "No memory for security size:%d\n", sec_size);
		return ENOMEM;
	}

        memset(&cmd, 0, sizeof(cmd));
        cmd.opcode = nvme_admin_security_send;
        cmd.cdw10 = secp << 24 | spsp << 8;
        cmd.cdw11 = tl;
        cmd.data_len = sec_size;
        cmd.addr = (__u64)sec_buf;

        err = ioctl(fd, NVME_IOCTL_ADMIN_CMD, &cmd);
        if (err < 0)
                return errno;
        else if (err != 0)
                fprintf(stderr, "NVME Security Send Command Error:%d\n", err);
	else
                printf("NVME Security Send Command Success:%d\n", cmd.result);
        return err;
}

static int flush(int argc, char **argv)
{
	struct nvme_passthru_cmd cmd;
	unsigned int nsid = 0xffffffff;
        int err, opt, long_index = 0;

	static struct option opts[] = {
		{"namespace-id", required_argument, 0, 'n'},
		{ 0, 0, 0, 0}
	};

	while ((opt = getopt_long(argc, (char **)argv, "n:", opts,
							&long_index)) != -1) {
		switch(opt) {
		case 'n': get_int(optarg, &nsid); break;
		default:
			return EINVAL;
		}
	}
	get_dev(optind, argc, argv);

	memset(&cmd, 0, sizeof(cmd));
	cmd.opcode = nvme_cmd_flush;
	cmd.nsid = nsid;

	err = ioctl(fd, NVME_IOCTL_IO_CMD, &cmd);
	if (err < 0)
		return errno;
	else if (err != 0)
		fprintf(stderr, "NVME IO command error:%s(%d)\n",
				nvme_status_to_string(err), err);
	else
		printf("NVMe Flush: success\n");
	return 0;
}

static int resv_acquire(int argc, char **argv)
{
	struct nvme_passthru_cmd cmd;
        int err, opt, long_index = 0;
	unsigned int nsid = 0;
	unsigned char rtype = 0, racqa = 0, iekey = 0;
	static struct option opts[] = {
		{"namespace-id", required_argument, 0, 'n'},
		{"crkey", required_argument, 0, 'c'},
		{"prkey", required_argument, 0, 'p'},
		{"rtype", required_argument, 0, 't'},
		{"racqa", required_argument, 0, 'a'},
		{"iekey", no_argument, 0, 'i'},
		{ 0, 0, 0, 0}
	};
	__u64 prkey= 0, crkey = 0, payload[2];

	while ((opt = getopt_long(argc, (char **)argv, "n:c:p:t:a:i", opts,
							&long_index)) != -1) {
		switch(opt) {
		case 'n': get_int(optarg, &nsid); break;
		case 'c': get_long(optarg, &crkey); break;
		case 'p': get_long(optarg, &prkey); break;
		case 't': get_byte(optarg, &rtype); break;
		case 'a': get_byte(optarg, &racqa); break;
		case 'i': iekey = 1; break;
		default:
			return EINVAL;
		}
	}
	get_dev(optind, argc, argv);

	if (!nsid) {
		if (!S_ISBLK(nvme_stat.st_mode)) {
			fprintf(stderr,
				"%s: non-block device requires namespace-id param\n",
				devicename);
			exit(ENOTBLK);
		}
		nsid = ioctl(fd, NVME_IOCTL_ID);
		if (nsid <= 0) {
			fprintf(stderr,
				"%s: failed to return namespace id\n",
				devicename);
			return errno;
		}
	}
	if (racqa > 7) {
		fprintf(stderr, "invalid racqa:%d\n", racqa);
		return EINVAL;
	}

	payload[0] = crkey;
	payload[1] = prkey;

	memset(&cmd, 0, sizeof(cmd));
        cmd.opcode = nvme_cmd_resv_acquire;
        cmd.nsid = nsid;
        cmd.cdw10 = rtype << 8 | iekey << 3 | racqa;

        cmd.addr = (__u64)payload;
        cmd.data_len = sizeof(payload);

        err = ioctl(fd, NVME_IOCTL_IO_CMD, &cmd);
        if (err < 0)
                return errno;
        else if (err != 0)
                fprintf(stderr, "NVME IO command error:%04x\n", err);
        else
                printf("NVME Reservation Acquire success\n");
	return 0;
}

static int resv_register(int argc, char **argv)
{
	struct nvme_passthru_cmd cmd;
        int err, opt, long_index = 0;
	unsigned int nsid = 0;
	unsigned char rrega = 0, iekey = 0, cptpl = 0;
	static struct option opts[] = {
		{"namespace-id", required_argument, 0, 'n'},
		{"crkey", required_argument, 0, 'c'},
		{"nrkey", required_argument, 0, 'k'},
		{"rrega", required_argument, 0, 'r'},
		{"cptpl", required_argument, 0, 'p'},
		{"iekey", required_argument, 0, 'i'},
		{ 0, 0, 0, 0}
	};
	__u64 nrkey= 0, crkey = 0, payload[2];

	while ((opt = getopt_long(argc, (char **)argv, "n:c:p:t:i:k:", opts,
							&long_index)) != -1) {
		switch(opt) {
		case 'n': get_int(optarg, &nsid); break;
		case 'c': get_long(optarg, &crkey); break;
		case 'k': get_long(optarg, &nrkey); break;
		case 'r': get_byte(optarg, &rrega); break;
		case 'p': get_byte(optarg, &cptpl); break;
		case 'i': get_byte(optarg, &iekey); break;
		default:
			return EINVAL;
		}
	}
	get_dev(optind, argc, argv);

	if (!nsid) {
		if (!S_ISBLK(nvme_stat.st_mode)) {
			fprintf(stderr,
				"%s: non-block device requires namespace-id param\n",
				devicename);
			exit(ENOTBLK);
		}
		nsid = ioctl(fd, NVME_IOCTL_ID);
		if (nsid <= 0) {
			fprintf(stderr,
				"%s: failed to return namespace id\n",
				devicename);
			return errno;
		}
	}
	if (iekey > 1) {
		fprintf(stderr, "invalid iekey:%d\n", iekey);
		return EINVAL;
	}
	if (cptpl > 3) {
		fprintf(stderr, "invalid cptpl:%d\n", cptpl);
		return EINVAL;
	}

	payload[0] = crkey;
	payload[1] = nrkey;

	memset(&cmd, 0, sizeof(cmd));
        cmd.opcode = nvme_cmd_resv_register;
        cmd.nsid = nsid;
	cmd.cdw10 = cptpl << 30 | iekey << 3 | rrega;

        cmd.addr = (__u64)payload;
        cmd.data_len = sizeof(payload);

        err = ioctl(fd, NVME_IOCTL_IO_CMD, &cmd);
        if (err < 0)
                return errno;
        else if (err != 0)
                fprintf(stderr, "NVME IO command error:%04x\n", err);
        else
                printf("NVME Reservation  success\n");
	return 0;
}

static int resv_release(int argc, char **argv)
{
	struct nvme_passthru_cmd cmd;
        int err, opt, long_index = 0;
	unsigned int nsid = 0;
	unsigned char rtype = 0, rrela = 0, iekey = 0;
	static struct option opts[] = {
		{"namespace-id", required_argument, 0, 'n'},
		{"crkey", required_argument, 0, 'c'},
		{"rtype", required_argument, 0, 't'},
		{"rrela", required_argument, 0, 'a'},
		{"iekey", required_argument, 0, 'i'},
		{ 0, 0, 0, 0}
	};
	__u64 crkey = 0;

	while ((opt = getopt_long(argc, (char **)argv, "n:c:p:t:a:i:", opts,
							&long_index)) != -1) {
		switch(opt) {
		case 'n': get_int(optarg, &nsid); break;
		case 'c': get_long(optarg, &crkey); break;
		case 't': get_byte(optarg, &rtype); break;
		case 'a': get_byte(optarg, &rrela); break;
		case 'i': get_byte(optarg, &iekey); break;
		default:
			return EINVAL;
		}
	}
	get_dev(optind, argc, argv);

	if (!nsid) {
		if (!S_ISBLK(nvme_stat.st_mode)) {
			fprintf(stderr,
				"%s: non-block device requires namespace-id param\n",
				devicename);
			exit(ENOTBLK);
		}
		nsid = ioctl(fd, NVME_IOCTL_ID);
		if (nsid <= 0) {
			fprintf(stderr,
				"%s: failed to return namespace id\n",
				devicename);
			return errno;
		}
	}
	if (iekey > 1) {
		fprintf(stderr, "invalid iekey:%d\n", iekey);
		return EINVAL;
	}
	if (rrela > 7) {
		fprintf(stderr, "invalid rrela:%d\n", rrela);
		return EINVAL;
	}

	memset(&cmd, 0, sizeof(cmd));
        cmd.opcode = nvme_cmd_resv_release;
        cmd.nsid = nsid;
	cmd.cdw10 = rtype << 8 | iekey << 3 | rrela;

        cmd.addr = (__u64)&crkey;
        cmd.data_len = sizeof(crkey);

        err = ioctl(fd, NVME_IOCTL_IO_CMD, &cmd);
        if (err < 0)
                return errno;
        else if (err != 0)
                fprintf(stderr, "NVME IO command error:%04x\n", err);
        else
                printf("NVME Reservation Register success\n");
	return 0;
}

static int resv_report(int argc, char **argv)
{
	struct nvme_passthru_cmd cmd;
        int err, opt, long_index = 0, raw = 0;
	unsigned int nsid = 0, numd = 0x1000 > 2;
	struct nvme_reservation_status *status;
	static struct option opts[] = {
		{"namespace-id", required_argument, 0, 'n'},
		{"numd", required_argument, 0, 'd'},
		{"raw-binary", no_argument, 0, 'b'},
		{ 0, 0, 0, 0}
	};

	while ((opt = getopt_long(argc, (char **)argv, "n:d:r", opts,
							&long_index)) != -1) {
		switch(opt) {
		case 'n': get_int(optarg, &nsid); break;
		case 'd': get_int(optarg, &numd); break;
		case 'r': raw = 0; break;
		default:
			return EINVAL;
		}
	}
	get_dev(optind, argc, argv);

	if (!nsid) {
		if (!S_ISBLK(nvme_stat.st_mode)) {
			fprintf(stderr,
				"%s: non-block device requires namespace-id param\n",
				devicename);
			exit(ENOTBLK);
		}
		nsid = ioctl(fd, NVME_IOCTL_ID);
		if (nsid <= 0) {
			fprintf(stderr,
				"%s: failed to return namespace id\n",
				devicename);
			return errno;
		}
	}
	if (!numd || numd > (0x1000 >> 2))
		numd = 0x1000 >> 2;

	if (posix_memalign((void **)&status, getpagesize(), numd << 2)) {
		fprintf(stderr, "No memory for resv report:%d\n", numd << 2);
		return ENOMEM;
	}

	memset(&cmd, 0, sizeof(cmd));
        cmd.opcode = nvme_cmd_resv_report;
        cmd.nsid = nsid;
        cmd.cdw10 = numd;

        cmd.addr = (__u64)status;
        cmd.data_len = numd << 2;

        err = ioctl(fd, NVME_IOCTL_IO_CMD, &cmd);
        if (err < 0)
                return errno;
        else if (err != 0)
                fprintf(stderr, "NVME IO command error:%04x\n", err);
        else {
		if (!raw) {
                	printf("NVME Reservation Report success\n");
			show_nvme_resv_report(status);
		} else
			d_raw((unsigned char *)status, numd << 2);
	}
	return 0;
}

static int submit_io(int opcode, char *command, int argc, char **argv)
{
	struct nvme_user_io io;
	void *buffer;
        int err, opt, dfd = opcode & 1 ? STDIN_FILENO : STDOUT_FILENO,
	  show = 0, dry_run = 0, long_index = 0;
	unsigned int data_size = 0;
	__u8 prinfo = 0;

	/* XXX: metadata ? */
	static struct option opts[] = {
		{"start-block", required_argument, 0, 's'},
		{"block-count", required_argument, 0, 'c'},
		{"data-size", required_argument, 0, 'z'},
		{"ref-tag", required_argument, 0, 'r'},
		{"data", required_argument, 0, 'd'},
		{"prinfo", required_argument, 0, 'p'},
		{"app-tag-mask", required_argument, 0, 'm'},
		{"app-tag", required_argument, 0, 'a'},
		{"limited-retry", no_argument, 0, 'l'},
		{"force-unit-access", no_argument, 0, 'f'},
		{"show-command", no_argument, 0, 'v'},
		{"dry-run", no_argument, 0, 'w'},
		{ 0, 0, 0, 0}
	};

	memset(&io, 0, sizeof(io));
	while ((opt = getopt_long(argc, (char **)argv, "p:s:c:z:r:d:m:a:lfvw", opts,
							&long_index)) != -1) {
		switch(opt) {
		case 's': get_long(optarg, &io.slba); break;
		case 'c': get_short(optarg, &io.nblocks); break;
		case 'z': get_int(optarg, &data_size); break;
		case 'r': get_int(optarg, &io.reftag); break;
		case 'm': get_short(optarg, &io.appmask); break;
		case 'a': get_short(optarg, &io.apptag); break;
		case 'p':
			get_byte(optarg, &prinfo);
			if (prinfo > 0xf)
				return EINVAL;
			io.control |= (prinfo << 10);
			break;
		case 'l':
			io.control |= NVME_RW_LR;
			break;
		case 'f':
			io.control |= NVME_RW_FUA;
			break;
		case 'd': 
		        if (opcode & 1)
			  dfd = open(optarg, O_RDONLY);		  
			else
			  dfd = open(optarg, O_WRONLY | O_CREAT, 660);
			if (dfd < 0) {
				perror(optarg);
				return EINVAL;
			}
			break;
		case 'v': show = 1; break;
		case 'w': dry_run = 1; break;
		default:
			return EINVAL;
		}
	};
	get_dev(optind, argc, argv);

	if (!data_size)	{
		fprintf(stderr, "data size not provided\n");
		return EINVAL;
	}
	buffer = malloc(data_size);
	if ((opcode & 1) && read(dfd, (void *)buffer, data_size) < 0) {
		fprintf(stderr, "failed to read buffer from input file\n");
		return EINVAL;
	}

        io.opcode = opcode;
	io.addr = (__u64)buffer;

	if (show) {
		printf("opcode       : %02x\n" , io.opcode);
		printf("flags        : %02x\n" , io.flags);
		printf("control      : %04x\n" , io.control);
		printf("nblocks      : %04x\n" , io.nblocks);
		printf("rsvd         : %04x\n" , io.rsvd);
		printf("metadata     : %p\n"   , (void *)io.metadata);
		printf("addr         : %p\n"   , (void *)io.addr);
		printf("sbla         : %p\n"   , (void *)io.slba);
		printf("dsmgmt       : %08x\n" , io.dsmgmt);
		printf("reftag       : %08x\n" , io.reftag);
		printf("apptag       : %04x\n" , io.apptag);
		printf("appmask      : %04x\n" , io.appmask);
		if (dry_run)
		  goto free_and_return;
	}

	err = ioctl(fd, NVME_IOCTL_SUBMIT_IO, &io);
	if (err < 0)
		perror("ioctl");
	else if (err)
		printf("%s:%s(%04x)\n", command, nvme_status_to_string(err), err);
	else {
		if (!(opcode & 1) && write(dfd, (void *)buffer, data_size) < 0) {
			fprintf(stderr, "failed to write buffer to output file\n");
			return EINVAL;
		} else
			printf("%s: success\n", command);
	}
 free_and_return:
	free(buffer);
	return 0;
}

static int compare(int argc, char **argv)
{
	return submit_io(nvme_cmd_compare, "compare", argc, argv); 
}

static int read_cmd(int argc, char **argv)
{
	return submit_io(nvme_cmd_read, "read", argc, argv); 
}

static int write_cmd(int argc, char **argv)
{
	return submit_io(nvme_cmd_write, "write", argc, argv); 
}

static int sec_recv(int argc, char **argv)
{
	struct nvme_admin_cmd cmd;
        int err, opt, long_index = 0, raw = 0;
	void *sec_buf = NULL;
	unsigned int al = 0;
	unsigned short spsp = 0;
	unsigned char secp = 0;
	unsigned int sec_size = 0;
	static struct option opts[] = {
		{"size", required_argument, 0, 'x'},
		{"file", required_argument, 0, 'f'},
		{"secp", required_argument, 0, 'p'},
		{"spsp", required_argument, 0, 's'},
		{"al", required_argument, 0, 't'},
		{"raw-binary", no_argument, 0, 'b'},
		{ 0, 0, 0, 0}
	};

	while ((opt = getopt_long(argc, (char **)argv, "f:p:s:t:", opts,
							&long_index)) != -1) {
		switch(opt) {
		case 'x':
			get_int(optarg, &sec_size);
			break;
		case 'p':
			get_short(optarg, &spsp);
			break;
		case 's':
			get_byte(optarg, &secp);
			break;
		case 't':
			get_int(optarg, &al);
			break;
		case 'b':
			raw = 1;
			break;
		default:
			return EINVAL;
		}
	}
	get_dev(optind, argc, argv);

	if (sec_size) {
		if (posix_memalign(&sec_buf, getpagesize(), sec_size)) {
			fprintf(stderr, "No memory for security size:%d\n",
								sec_size);
			return ENOMEM;
		}
	}

        memset(&cmd, 0, sizeof(cmd));
        cmd.opcode = nvme_admin_security_recv;
        cmd.cdw10 = secp << 24 | spsp << 8;
        cmd.cdw11 = al;
        cmd.data_len = sec_size;
        cmd.addr = (__u64)sec_buf;

        err = ioctl(fd, NVME_IOCTL_ADMIN_CMD, &cmd);
        if (err < 0)
                return errno;
        else if (err != 0)
                fprintf(stderr, "NVME Security Receivce Command Error:%d\n",
									err);
	else {
		if (!raw) {
                	printf("NVME Security Receivce Command Success:%d\n",
							cmd.result);
			d(sec_buf, sec_size, 16, 1);
		} else if (sec_size)
			d_raw((unsigned char *)&sec_buf, sec_size);
	}
        return err;
}

static int nvme_passthru(int argc, char **argv, int ioctl_cmd)
{
	int r = 0, w = 0;
	int opt, err, raw = 0, show = 0, dry_run = 0, long_index = 0, wfd = STDIN_FILENO;
	struct nvme_passthru_cmd cmd;
	static struct option opts[] = {
		{"opcode", required_argument, 0, 'o'},
		{"flags", required_argument, 0, 'f'},
		{"rsvd", required_argument, 0, 'R'},
		{"namespace-id", required_argument, 0, 'n'},
		{"data-len", required_argument, 0, 'l'},
		{"metadata-len", required_argument, 0, 'm'},
		{"timeout", required_argument, 0, 't'},
		{"cdw2", required_argument, 0, '2'},
		{"cdw3", required_argument, 0, '3'},
		{"cdw10", required_argument, 0, '4'},
		{"cdw11", required_argument, 0, '5'},
		{"cdw12", required_argument, 0, '6'},
		{"cdw13", required_argument, 0, '7'},
		{"cdw14", required_argument, 0, '8'},
		{"cdw15", required_argument, 0, '9'},
		{"raw-binary", no_argument, 0, 'b'},
		{"show-command", no_argument, 0, 's'},
		{"dry-run", no_argument, 0, 'd'},
		{"read", no_argument, 0, 'r'},
		{"write", no_argument, 0, 'w'},
		{"input-file", no_argument, 0, 'i'},
		{0, 0, 0, 0}
	};

	memset(&cmd, 0, sizeof(cmd));
	while ((opt = getopt_long(argc, (char **)argv, "o:n:f:l:R:m:t:i:bsdrw", opts,
							&long_index)) != -1) {
		switch (opt) {
		case '2': get_int(optarg, &cmd.cdw2); break;
		case '3': get_int(optarg, &cmd.cdw3); break;
		case '4': get_int(optarg, &cmd.cdw10); break;
		case '5': get_int(optarg, &cmd.cdw11); break;
		case '6': get_int(optarg, &cmd.cdw12); break;
		case '7': get_int(optarg, &cmd.cdw13); break;
		case '8': get_int(optarg, &cmd.cdw14); break;
		case '9': get_int(optarg, &cmd.cdw15); break;
		case 'o': get_byte(optarg, &cmd.opcode); break;
		case 'f': get_byte(optarg, &cmd.flags); break;
		case 'R': get_short(optarg, &cmd.rsvd1); break;
		case 'n': get_int(optarg, &cmd.nsid); break;
		case 'l': get_int(optarg, &cmd.data_len); break;
		case 'm': get_int(optarg, &cmd.metadata_len); break;
		case 't': get_int(optarg, &cmd.timeout_ms); break;
		case 'b': raw = 1; break;
		case 's': show = 1; break;
		case 'd': dry_run = 1; break;
		case 'r': r = 1; break;
		case 'w': w = 1; break;
		case 'i':
			wfd = open(optarg, O_RDONLY);
			if (wfd < 0) {
				perror(optarg);
				return EINVAL;
			}
			break;
		default:
			return EINVAL;
		}
	}
	get_dev(optind, argc, argv);
	if (cmd.metadata_len)
		cmd.metadata = (__u64)malloc(cmd.metadata_len);
	if (cmd.data_len) {
		cmd.addr = (__u64)malloc(cmd.data_len);
		if (!r && !w) {
			fprintf(stderr, "data direction not given\n");
			return EINVAL;
		}
		if (r && w) {
			fprintf(stderr, "command can't be both read and write\n");
			return EINVAL;
		}
		if (w) {
			if (read(wfd, (void *)cmd.addr, cmd.data_len) < 0) {
				fprintf(stderr, "failed to read write buffer\n");
				return EINVAL;
			}
		}
	}
	if (show) {
		printf("opcode       : %02x\n", cmd.opcode);
		printf("flags        : %02x\n", cmd.flags);
		printf("rsvd1        : %04x\n", cmd.rsvd1);
		printf("nsid         : %08x\n", cmd.nsid);
		printf("cdw2         : %08x\n", cmd.cdw2);
		printf("cdw3         : %08x\n", cmd.cdw3);
		printf("data_len     : %08x\n", cmd.data_len);
		printf("metadata_len : %08x\n", cmd.metadata_len);
		printf("addr         : %p\n",   (void *)cmd.addr);
		printf("metadata     : %p\n",   (void *)cmd.metadata);
		printf("cdw10        : %08x\n", cmd.cdw10);
		printf("cdw11        : %08x\n", cmd.cdw11);
		printf("cdw12        : %08x\n", cmd.cdw12);
		printf("cdw13        : %08x\n", cmd.cdw13);
		printf("cdw14        : %08x\n", cmd.cdw14);
		printf("cdw15        : %08x\n", cmd.cdw15);
		printf("timeout_ms   : %08x\n", cmd.timeout_ms);
		if (dry_run)
		  return 0;
	}
	err = ioctl(fd, ioctl_cmd, &cmd);
	if (err >= 0) {
		if (!raw) {
			printf("NVMe Status:%s Command Result:%08x\n",
				nvme_status_to_string(err), cmd.result);
			if (cmd.addr && r && !err)
				d((unsigned char *)cmd.addr, cmd.data_len, 16, 1);
		} else if (!err && cmd.addr && r)
			d_raw((unsigned char *)cmd.addr, cmd.data_len);
	} else
		perror("ioctl");
	return err;
}

static int io_passthru(int argc, char **argv)
{
	return nvme_passthru(argc, argv, NVME_IOCTL_IO_CMD);
}

static int admin_passthru(int argc, char **argv)
{
	return nvme_passthru(argc, argv, NVME_IOCTL_ADMIN_CMD);
}

static void usage(char *cmd)
{
	fprintf(stdout, "usage: %s <command> [<device>] [<args>]\n", cmd);
}

static void command_help(const char *cmd)
{
	unsigned i;
	struct command *c;

	for (i = 0; i < NUM_COMMANDS; i++) {
		c = &commands[i];
		if (strcmp(c->name, cmd))
			continue;
		exit(execlp("man", "man", c->man, (char *)NULL));
	}
	fprintf(stderr, "No entry for nvme sub-command %s\n", cmd);
}

static void general_help()
{
	unsigned i;

	usage("nvme");
	printf("\n");
	printf("The '<device>' may be either an NVMe character device (ex: /dev/nvme0)\n"
	       "or an nvme block device (ex: /dev/nvme0n1)\n\n");
	printf("The following are all implemented sub-commands:\n");
	for (i = 0; i < NUM_COMMANDS; i++)
		printf("  %-*s %s\n", 15, commands[i].name, commands[i].help);
	printf("\n");
	printf("See 'nvme help <command>' for more information on a specific command.\n");
}

static int help(int argc, char **argv)
{
	if (argc == 1)
		general_help();
	else
		command_help(argv[1]);
	return 0;
}

static void handle_internal_command(int argc, char **argv)
{
	unsigned i;
	struct command *cmd;

	for (i = 0; i < NUM_COMMANDS; i++) {
		cmd = &commands[i];
		if (strcmp(argv[0], cmd->name))
			continue;
		exit(cmd->fn(argc, argv));
	}
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		usage(argv[0]);
		return 0;
	}
	setlocale(LC_ALL, "");
	handle_internal_command(argc - 1, &argv[1]);
	return 0;
}

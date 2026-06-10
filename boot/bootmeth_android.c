// SPDX-License-Identifier: GPL-2.0+
/*
 * Bootmeth for Android
 *
 * Copyright (C) 2024 BayLibre, SAS
 * Written by Mattijs Korpershoek <mkorpershoek@baylibre.com>
 */
#define LOG_CATEGORY UCLASS_BOOTSTD

#include <android_ab.h>
#include <android_image.h>
#if CONFIG_IS_ENABLED(AVB_VERIFY)
#include <avb_verify.h>
#endif
#include <bcb.h>
#include <blk.h>
#include <bootflow.h>
#include <bootm.h>
#include <bootmeth.h>
#include <dm.h>
#include <div64.h>
#include <dt_table.h>
#include <env.h>
#include <image.h>
#include <malloc.h>
#include <mapmem.h>
#include <part.h>
#include <version.h>
#include <asm/global_data.h>
#include <linux/libfdt.h>
#include "bootmeth_android.h"

DECLARE_GLOBAL_DATA_PTR;

#define BCB_FIELD_COMMAND_SZ 32
#define BCB_PART_NAME "misc"
#define BOOT_PART_NAME "boot"
#define DTBO_PART_NAME "dtbo"
#define VENDOR_BOOT_PART_NAME "vendor_boot"
#define SLOT_LEN 2
#define SLOT_SUFFIX_LEN 3
#define TOWED_ANDROID_DTBO_ADDR_ENV "towed_android_dtbo_addr"
#define TOWED_ANDROID_DTBO_SIZE_ENV "towed_android_dtbo_size"
#define TOWED_ANDROID_FDT_ADDR_ENV "towed_android_fdt_addr"
#define TOWED_ANDROID_KERNEL_ADDR_ENV "towed_android_kernel_addr"
#define TOWED_ANDROID_RAMDISK_ADDR_ENV "towed_android_ramdisk_addr"
#define TOWED_ANDROID_INITRD_HIGH_IN_PLACE "0xffffffffffffffff"

/**
 * struct android_priv - Private data
 *
 * This is read from the disk and recorded for use when the full Android
 * kernel must be loaded and booted
 *
 * @boot_mode: Requested boot mode (normal, recovery, bootloader)
 * @slot: Nul-terminated partition slot suffix read from BCB ("a\0" or "b\0")
 * @header_version: Android boot image header version
 */
struct android_priv {
	enum android_boot_mode boot_mode;
	char *slot;
	char detected_slot[SLOT_LEN];
	bool boot_part_slotted;
	char boot_part[PART_NAME_LEN];
	char dtbo_part[PART_NAME_LEN];
	char vendor_boot_part[PART_NAME_LEN];
	u32 header_version;
	u32 boot_img_offset;
	u32 boot_img_size;
	u32 dtbo_img_size;
	u32 vendor_boot_img_size;
};

struct towed_saved_env {
	const char *name;
	char *value;
	bool valid;
};

static int android_check(struct udevice *dev, struct bootflow_iter *iter)
{
	if (bootflow_iter_check_blk(iter))
		return log_msg_ret("blk", -EOPNOTSUPP);

	/*
	 * This only works on whole devices, as multiple
	 * partitions are needed to boot Android
	 */
	if (iter->part != 0)
		return log_msg_ret("mmc part", -ENOTSUPP);

	return 0;
}

static int android_part_name(char *partname, const char *base,
			     const char *slot)
{
	int ret;

	if (slot)
		ret = snprintf(partname, PART_NAME_LEN, "%s_%s", base, slot);
	else
		ret = snprintf(partname, PART_NAME_LEN, "%s", base);

	if (ret < 0 || ret >= PART_NAME_LEN)
		return -EINVAL;

	return 0;
}

static const char *android_other_slot(const char *slot)
{
	if (!slot)
		return NULL;

	if (slot[0] == 'a')
		return "b";
	if (slot[0] == 'b')
		return "a";

	return NULL;
}

static bool android_part_is_slotted(const char *partname, const char *base,
				    char slot[SLOT_LEN])
{
	size_t len = strlen(base);

	if (strlen(partname) != len + SLOT_SUFFIX_LEN)
		return false;

	if (strncmp(partname, base, len) || partname[len] != '_')
		return false;

	if (partname[len + 1] != 'a' && partname[len + 1] != 'b')
		return false;

	slot[0] = partname[len + 1];
	slot[1] = '\0';

	return true;
}

static int android_read_part_bytes(struct udevice *blk,
				   const struct disk_partition *partition,
				   u64 offset, void *buffer, u32 bytes)
{
	struct blk_desc *desc = dev_get_uclass_plat(blk);
	u64 partition_size = (u64)partition->size * desc->blksz;
	uint block_offset;
	lbaint_t start;
	lbaint_t blkcnt;
	void *bounce;
	int ret;

	if (offset > partition_size || bytes > partition_size - offset)
		return -EINVAL;

	block_offset = do_div(offset, desc->blksz);
	start = partition->start + offset;
	blkcnt = DIV_ROUND_UP(block_offset + bytes, desc->blksz);

	bounce = malloc(blkcnt * desc->blksz);
	if (!bounce)
		return -ENOMEM;

	ret = blk_read(blk, start, blkcnt, bounce);
	if (ret != blkcnt) {
		free(bounce);
		return -EIO;
	}

	memcpy(buffer, (u8 *)bounce + block_offset, bytes);
	free(bounce);

	return 0;
}

static int scan_towed_android_payload(struct udevice *blk,
				      struct android_priv *priv,
				      const struct disk_partition *partition,
				      u32 outer_size)
{
	struct blk_desc *desc = dev_get_uclass_plat(blk);
	struct andr_boot_img_hdr_v0 *payload_boot_hdr;
	u32 payload_offset, payload_size, payload_boot_img_size;
	u64 partition_size = (u64)partition->size * desc->blksz;
	char *payload_hdr;
	ulong num_blks, bufsz;
	int ret;

	payload_hdr = malloc(TOWED_BOOT_ANDROID_PAYLOAD_HEADER_SIZE);
	if (!payload_hdr)
		return -ENOMEM;

	ret = android_read_part_bytes(blk, partition, outer_size, payload_hdr,
				      TOWED_BOOT_ANDROID_PAYLOAD_HEADER_SIZE);
	if (ret ||
	    !towed_boot_android_payload_get(payload_hdr, &payload_offset,
					    &payload_size)) {
		free(payload_hdr);
		return -ENOENT;
	}

	free(payload_hdr);

	if (payload_offset % desc->blksz || payload_offset > partition_size ||
	    payload_size > partition_size - payload_offset)
		return -EINVAL;

	num_blks = DIV_ROUND_UP(sizeof(*payload_boot_hdr), desc->blksz);
	bufsz = num_blks * desc->blksz;
	payload_boot_hdr = malloc(bufsz);
	if (!payload_boot_hdr)
		return -ENOMEM;

	ret = android_read_part_bytes(blk, partition, payload_offset,
				      payload_boot_hdr, sizeof(*payload_boot_hdr));
	if (ret ||
	    !is_android_boot_image_header(payload_boot_hdr) ||
	    payload_boot_hdr->header_version > 2 ||
	    !android_image_get_bootimg_size(payload_boot_hdr,
					    &payload_boot_img_size) ||
	    payload_boot_img_size > payload_size) {
		free(payload_boot_hdr);
		return -EINVAL;
	}

	priv->header_version = payload_boot_hdr->header_version;
	priv->boot_img_offset = payload_offset;
	priv->boot_img_size = payload_boot_img_size;
	free(payload_boot_hdr);

	return 0;
}

static int scan_boot_part_name(struct udevice *blk, struct android_priv *priv,
			       const char *partname)
{
	struct blk_desc *desc = dev_get_uclass_plat(blk);
	struct disk_partition partition;
	const struct andr_boot_img_hdr_v0 *hdr;
	u32 header_version, kernel_size;
	ulong kernel_offset, kernel_block;
	uint kernel_block_offset;
	ulong num_blks, bufsz;
	char *kernel_buf;
	char *buf;
	int ret;

	ret = part_get_info_by_name(desc, partname, &partition);
	if (ret < 0)
		return log_msg_ret("part info", ret);

	num_blks = DIV_ROUND_UP(sizeof(struct andr_boot_img_hdr_v0), desc->blksz);
	bufsz = num_blks * desc->blksz;
	buf = malloc(bufsz);
	if (!buf)
		return log_msg_ret("buf", -ENOMEM);

	ret = blk_read(blk, partition.start, num_blks, buf);
	if (ret != num_blks) {
		free(buf);
		return log_msg_ret("part read", -EIO);
	}

	if (!is_android_boot_image_header(buf)) {
		free(buf);
		return log_msg_ret("header", -ENOENT);
	}

	if (!android_image_get_bootimg_size(buf, &priv->boot_img_size)) {
		free(buf);
		return log_msg_ret("get bootimg size", -EINVAL);
	}

	hdr = (const struct andr_boot_img_hdr_v0 *)buf;
	header_version = hdr->header_version;
	kernel_size = header_version <= 2 ? hdr->kernel_size :
		((const struct andr_boot_img_hdr_v3 *)buf)->kernel_size;
	kernel_offset = header_version <= 2 ? hdr->page_size :
		ANDR_GKI_PAGE_SIZE;

	if (IS_ENABLED(CONFIG_TOWED_BOOT_ANDROID) && kernel_size >= 0x60) {
		kernel_block = kernel_offset / desc->blksz;
		kernel_block_offset = kernel_offset % desc->blksz;
		num_blks = DIV_ROUND_UP(kernel_block_offset + 0x60,
					desc->blksz);
		bufsz = num_blks * desc->blksz;
		kernel_buf = malloc(bufsz);
		if (!kernel_buf) {
			free(buf);
			return log_msg_ret("kernel buf", -ENOMEM);
		}

		ret = blk_read(blk, partition.start + kernel_block, num_blks,
			       kernel_buf);
		if (ret != num_blks) {
			free(kernel_buf);
			free(buf);
			return log_msg_ret("kernel read", -EIO);
		}

		if (is_arm64_u_boot_image(kernel_buf + kernel_block_offset,
					  bufsz - kernel_block_offset)) {
			ret = scan_towed_android_payload(blk, priv, &partition,
							 priv->boot_img_size);
			free(kernel_buf);
			if (ret) {
				free(buf);
				return log_msg_ret("u-boot payload", ret);
			}

			goto found;
		}

		free(kernel_buf);
	}

	priv->header_version = header_version;
	priv->boot_img_offset = 0;
found:
	strlcpy(priv->boot_part, partname, sizeof(priv->boot_part));
	priv->boot_part_slotted = android_part_is_slotted(partname,
							  BOOT_PART_NAME,
							  priv->detected_slot);

	free(buf);

	return 0;
}

static int scan_android_part_candidates(struct udevice *blk,
					struct android_priv *priv,
					const char *base,
					int (*scan)(struct udevice *blk,
						    struct android_priv *priv,
						    const char *partname))
{
	const char *other_slot = android_other_slot(priv->slot);
	const char *slot_order[3];
	char partname[PART_NAME_LEN];
	int i, ret;

	if (priv->slot) {
		slot_order[0] = priv->slot;
		slot_order[1] = NULL;
		slot_order[2] = other_slot;
	} else if (priv->detected_slot[0]) {
		slot_order[0] = priv->detected_slot;
		slot_order[1] = NULL;
		slot_order[2] = android_other_slot(priv->detected_slot);
	} else {
		slot_order[0] = NULL;
		slot_order[1] = "a";
		slot_order[2] = "b";
	}

	for (i = 0; i < ARRAY_SIZE(slot_order); i++) {
		ret = android_part_name(partname, base, slot_order[i]);
		if (ret)
			return ret;

		ret = scan(blk, priv, partname);
		if (!ret)
			return 0;
	}

	return ret;
}

static int scan_boot_part(struct udevice *blk, struct android_priv *priv)
{
	return scan_android_part_candidates(blk, priv, BOOT_PART_NAME,
					    scan_boot_part_name);
}

static int scan_vendor_boot_part_name(struct udevice *blk,
				      struct android_priv *priv,
				      const char *partname)
{
	struct blk_desc *desc = dev_get_uclass_plat(blk);
	struct disk_partition partition;
	ulong num_blks, bufsz;
	char *buf;
	int ret;

	ret = part_get_info_by_name(desc, partname, &partition);
	if (ret < 0)
		return log_msg_ret("part info", ret);

	num_blks = DIV_ROUND_UP(sizeof(struct andr_vnd_boot_img_hdr), desc->blksz);
	bufsz = num_blks * desc->blksz;
	buf = malloc(bufsz);
	if (!buf)
		return log_msg_ret("buf", -ENOMEM);

	ret = blk_read(blk, partition.start, num_blks, buf);
	if (ret != num_blks) {
		free(buf);
		return log_msg_ret("part read", -EIO);
	}

	if (!is_android_vendor_boot_image_header(buf)) {
		free(buf);
		return log_msg_ret("header", -ENOENT);
	}

	if (!android_image_get_vendor_bootimg_size(buf, &priv->vendor_boot_img_size)) {
		free(buf);
		return log_msg_ret("get vendor bootimg size", -EINVAL);
	}

	strlcpy(priv->vendor_boot_part, partname, sizeof(priv->vendor_boot_part));
	free(buf);

	return 0;
}

static bool android_dt_image_get_size(const void *buf, u32 *image_size)
{
	const struct dt_table_header *hdr = buf;
	u32 total_size, header_size, entry_size, entry_count, entries_offset;
	u64 entries_end;

	if (fdt32_to_cpu(hdr->magic) != DT_TABLE_MAGIC)
		return false;

	total_size = fdt32_to_cpu(hdr->total_size);
	header_size = fdt32_to_cpu(hdr->header_size);
	entry_size = fdt32_to_cpu(hdr->dt_entry_size);
	entry_count = fdt32_to_cpu(hdr->dt_entry_count);
	entries_offset = fdt32_to_cpu(hdr->dt_entries_offset);

	entries_end = (u64)entries_offset + (u64)entry_size * entry_count;
	if (header_size < sizeof(*hdr) ||
	    entry_size < sizeof(struct dt_table_entry) ||
	    entries_offset < header_size || entries_end > total_size)
		return false;

	*image_size = total_size;

	return true;
}

static int scan_dtbo_part_name(struct udevice *blk, struct android_priv *priv,
			       const char *partname)
{
	struct blk_desc *desc = dev_get_uclass_plat(blk);
	struct disk_partition partition;
	u32 image_size;
	ulong num_blks, bufsz;
	char *buf;
	int ret;

	ret = part_get_info_by_name(desc, partname, &partition);
	if (ret < 0)
		return log_msg_ret("part info", ret);

	num_blks = DIV_ROUND_UP(sizeof(struct dt_table_header), desc->blksz);
	bufsz = num_blks * desc->blksz;
	buf = malloc(bufsz);
	if (!buf)
		return log_msg_ret("buf", -ENOMEM);

	ret = blk_read(blk, partition.start, num_blks, buf);
	if (ret != num_blks) {
		free(buf);
		return log_msg_ret("part read", -EIO);
	}

	if (!android_dt_image_get_size(buf, &image_size) ||
	    image_size > (u64)partition.size * desc->blksz) {
		free(buf);
		return log_msg_ret("dtbo image", -EINVAL);
	}

	strlcpy(priv->dtbo_part, partname, sizeof(priv->dtbo_part));
	priv->dtbo_img_size = image_size;
	free(buf);

	return 0;
}

static int scan_vendor_boot_part(struct udevice *blk, struct android_priv *priv)
{
	return scan_android_part_candidates(blk, priv, VENDOR_BOOT_PART_NAME,
					    scan_vendor_boot_part_name);
}

static int scan_dtbo_part(struct udevice *blk, struct android_priv *priv)
{
	return scan_android_part_candidates(blk, priv, DTBO_PART_NAME,
					    scan_dtbo_part_name);
}

static int android_read_slot_from_bcb(struct bootflow *bflow, bool decrement)
{
	struct blk_desc *desc = dev_get_uclass_plat(bflow->blk);
	struct android_priv *priv = bflow->bootmeth_priv;
	struct disk_partition misc;
	int ret;

	if (!CONFIG_IS_ENABLED(ANDROID_AB)) {
		priv->slot = NULL;
		return 0;
	}

	ret = part_get_info_by_name(desc, BCB_PART_NAME, &misc);
	if (ret < 0)
		return log_msg_ret("part", ret);

	ret = ab_select_slot(desc, &misc, decrement);
	if (ret < 0)
		return log_msg_ret("slot", ret);

	priv->slot = malloc(SLOT_LEN);
	priv->slot[0] = BOOT_SLOT_NAME(ret);
	priv->slot[1] = '\0';

	return 0;
}

static int configure_slot_suffix(struct bootflow *bflow)
{
	struct android_priv *priv = bflow->bootmeth_priv;
	char slot_suffix[SLOT_SUFFIX_LEN];
	const char *slot = NULL;

	if (priv->boot_part_slotted)
		slot = priv->detected_slot;

	if (!slot)
		return 0;

	snprintf(slot_suffix, sizeof(slot_suffix), "_%s", slot);

	return bootflow_cmdline_set_arg(bflow, "androidboot.slot_suffix",
					slot_suffix, false);
}

static int configure_serialno(struct bootflow *bflow)
{
	char *serialno = env_get("serial#");

	if (!serialno)
		return log_msg_ret("serial", -ENOENT);

	return bootflow_cmdline_set_arg(bflow, "androidboot.serialno", serialno, false);
}

static int configure_bootloader_version(struct bootflow *bflow)
{
	return bootflow_cmdline_set_arg(bflow, "androidboot.bootloader",
					PLAIN_VERSION, false);
}

static int towed_bootflow_cmdline_set_arg(struct bootflow *bflow,
					  const char *arg, const char *value)
{
	char *cmdline;
	size_t len;
	char *buf;
	int ret;

	len = strlen(bflow->cmdline ?: "") + strlen(arg) + 2;
	if (value && value != BOOTFLOWCL_EMPTY)
		len += strlen(value) + 2;

	buf = malloc(len);
	if (!buf)
		return -ENOMEM;

	ret = cmdline_set_arg(buf, len, bflow->cmdline, arg, value, NULL);
	if (ret < 0) {
		free(buf);
		return ret;
	}

	cmdline = strdup(buf);
	free(buf);
	if (!cmdline)
		return -ENOMEM;

	free(bflow->cmdline);
	bflow->cmdline = cmdline;

	return 0;
}

static const char *towed_android_prev_bootargs(void)
{
	const void *fdt = gd->fdt_blob;
	const char *bootargs;
	int chosen, len;

	if (!IS_ENABLED(CONFIG_TOWED_BOOT_ANDROID) || !fdt ||
	    fdt_check_header(fdt))
		return NULL;

	chosen = fdt_path_offset(fdt, "/chosen");
	if (chosen < 0)
		return NULL;

	bootargs = fdt_getprop(fdt, chosen, "bootargs", &len);
	if (!bootargs || len <= 0 || bootargs[len - 1] != '\0')
		return NULL;

	return bootargs;
}

static bool towed_android_should_import_prev_arg(const char *arg)
{
	return !strncmp(arg, "androidboot.", 12);
}

static int import_previous_android_bootargs(struct bootflow *bflow)
{
	const char *bootargs;
	char *arg, *args, *orig;
	int count = 0;
	int ret = 0;

	bootargs = towed_android_prev_bootargs();
	if (!bootargs || !*bootargs)
		return 0;

	args = strdup(bootargs);
	if (!args)
		return -ENOMEM;
	orig = args;

	arg = strsep(&args, " ");
	while (arg) {
		char *value;

		if (!*arg || !towed_android_should_import_prev_arg(arg)) {
			arg = strsep(&args, " ");
			continue;
		}

		value = strchr(arg, '=');
		if (value)
			*value++ = '\0';
		else
			value = BOOTFLOWCL_EMPTY;

		ret = towed_bootflow_cmdline_set_arg(bflow, arg, value);
		if (ret < 0)
			break;

		count++;
		arg = strsep(&args, " ");
	}

	free(orig);

	if (ret < 0)
		return ret;

	if (IS_ENABLED(CONFIG_TOWED_DEBUG) && count)
		printf("Towed-Boot: imported %d previous Android bootargs\n",
		       count);

	return 0;
}

static int android_read_bootflow(struct udevice *dev, struct bootflow *bflow)
{
	struct blk_desc *desc = dev_get_uclass_plat(bflow->blk);
	struct disk_partition misc;
	struct android_priv *priv;
	char command[BCB_FIELD_COMMAND_SZ];
	const char *bcb_iface;
	int ret;

	bflow->state = BOOTFLOWST_MEDIA;

	/*
	 * bcb_find_partition_and_load() will print errors to stdout
	 * if BCB_PART_NAME is not found. To avoid that, check if the
	 * partition exists first.
	 */
	ret = part_get_info_by_name(desc, BCB_PART_NAME, &misc);
	if (ret < 0)
		return log_msg_ret("part", ret);

	bcb_iface = blk_get_uclass_name(desc->uclass_id);
	if (!bcb_iface)
		return log_msg_ret("bcb iface", -EINVAL);

	ret = bcb_find_partition_and_load(bcb_iface, desc->devnum, BCB_PART_NAME);
	if (ret < 0)
		return log_msg_ret("bcb load", ret);

	ret = bcb_get(BCB_FIELD_COMMAND, command, sizeof(command));
	if (ret < 0)
		return log_msg_ret("bcb read", ret);

	priv = calloc(1, sizeof(struct android_priv));
	if (!priv)
		return log_msg_ret("buf", -ENOMEM);

	if (!strcmp("bootonce-bootloader", command)) {
		priv->boot_mode = ANDROID_BOOT_MODE_BOOTLOADER;
		bflow->os_name = strdup("Android (bootloader)");
	} else if (!strcmp("boot-fastboot", command)) {
		priv->boot_mode = ANDROID_BOOT_MODE_RECOVERY;
		bflow->os_name = strdup("Android (fastbootd)");
	} else if (!strcmp("boot-recovery", command)) {
		priv->boot_mode = ANDROID_BOOT_MODE_RECOVERY;
		bflow->os_name = strdup("Android (recovery)");
	} else {
		priv->boot_mode = ANDROID_BOOT_MODE_NORMAL;
		bflow->os_name = strdup("Android");
	}
	if (!bflow->os_name) {
		free(priv);
		return log_msg_ret("os", -ENOMEM);
	}

	if (priv->boot_mode == ANDROID_BOOT_MODE_BOOTLOADER) {
		/* Clear BCB */
		memset(command, 0, sizeof(command));
		ret = bcb_set(BCB_FIELD_COMMAND, command);
		if (ret < 0) {
			free(priv);
			return log_msg_ret("bcb set", ret);
		}
		ret = bcb_store();
		if (ret < 0) {
			free(priv);
			return log_msg_ret("bcb store", ret);
		}

		bflow->bootmeth_priv = priv;
		bflow->state = BOOTFLOWST_READY;
		return 0;
	}

	bflow->bootmeth_priv = priv;

	/* For recovery and normal boot, we need to scan the partitions */
	ret = android_read_slot_from_bcb(bflow, false);
	if (ret < 0) {
		log_err("read slot: %d", ret);
		goto free_priv;
	}

	ret = scan_boot_part(bflow->blk, priv);
	if (ret < 0) {
		log_debug("scan boot failed: err=%d\n", ret);
		goto free_priv;
	}

	ret = scan_dtbo_part(bflow->blk, priv);
	if (ret < 0)
		log_debug("scan dtbo skipped: err=%d\n", ret);

	if (priv->header_version >= 3) {
		ret = scan_vendor_boot_part(bflow->blk, priv);
		if (ret < 0) {
			log_debug("scan vendor_boot failed: err=%d\n", ret);
			goto free_priv;
		}
	}

	ret = import_previous_android_bootargs(bflow);
	if (ret < 0) {
		log_debug("previous bootargs %d", ret);
		goto free_priv;
	}

	ret = configure_slot_suffix(bflow);
	if (ret < 0) {
		log_debug("slot_suffix %d", ret);
		goto free_priv;
	}

	/*
	 * Ignoring return code for the following configurations:
	 * these are not mandatory for booting.
	 */
	configure_serialno(bflow);
	configure_bootloader_version(bflow);

	if (priv->boot_mode == ANDROID_BOOT_MODE_NORMAL && priv->slot) {
		ret = bootflow_cmdline_set_arg(bflow, "androidboot.force_normal_boot",
					       "1", false);
		if (ret < 0) {
			log_debug("normal_boot %d", ret);
			goto free_priv;
		}
	}

	bflow->state = BOOTFLOWST_READY;

	return 0;

 free_priv:
	free(priv);
	bflow->bootmeth_priv = NULL;
	return ret;
}

static int android_read_file(struct udevice *dev, struct bootflow *bflow,
			     const char *file_path, ulong addr,
			     enum bootflow_img_t type, ulong *sizep)
{
	/*
	 * Reading individual files is not supported since we only
	 * operate on whole block devices (because we require multiple partitions)
	 */
	return log_msg_ret("Unsupported", -ENOSYS);
}

/**
 * read_partition() - Read a probed Android partition
 *
 * @desc: Block device to read
 * @partname: Exact partition name to read
 * @image_size: Image size in bytes used when reading the partition
 * @addr: Address where the partition content is loaded into
 * Return: 0 if OK, negative errno on failure.
 */
static int read_partition(struct blk_desc *desc, const char *const partname,
			  ulong image_offset, ulong image_size, ulong addr)
{
	struct disk_partition partition;
	ulong num_blks = DIV_ROUND_UP(image_size, desc->blksz);
	int ret;
	u32 n;

	ret = part_get_info_by_name(desc, partname, &partition);
	if (ret < 0)
		return log_msg_ret("part", ret);

	if (image_offset % desc->blksz)
		return log_msg_ret("part offset", -EINVAL);

	n = blk_dread(desc, partition.start + image_offset / desc->blksz,
		      num_blks, map_sysmem(addr, 0));
	if (n < num_blks)
		return log_msg_ret("part read", -EIO);

	return 0;
}

static void clear_dtbo_env(void)
{
	env_set(TOWED_ANDROID_DTBO_ADDR_ENV, NULL);
	env_set(TOWED_ANDROID_DTBO_SIZE_ENV, NULL);
}

static void clear_staged_component_env(void)
{
	env_set(TOWED_ANDROID_KERNEL_ADDR_ENV, NULL);
	env_set(TOWED_ANDROID_RAMDISK_ADDR_ENV, NULL);
	env_set(TOWED_ANDROID_FDT_ADDR_ENV, NULL);
}

static int towed_env_save(struct towed_saved_env *save, const char *name)
{
	const char *value;

	save->name = name;
	save->value = NULL;
	save->valid = false;

	value = env_get(name);
	if (!value)
		return 0;

	save->value = strdup(value);
	if (!save->value)
		return -ENOMEM;
	save->valid = true;

	return 0;
}

static int towed_env_restore(struct towed_saved_env *save)
{
	int ret;

	if (!save->name)
		return 0;

	ret = env_set(save->name, save->valid ? save->value : NULL);
	free(save->value);
	save->value = NULL;
	save->valid = false;

	return ret;
}

static int towed_android_pin_initrd_high(struct towed_saved_env *save)
{
	int ret;

	if (!IS_ENABLED(CONFIG_TOWED_BOOT_ANDROID) ||
	    !IS_ENABLED(CONFIG_SYS_BOOT_RAMDISK_HIGH) ||
	    !env_get_hex(TOWED_ANDROID_RAMDISK_ADDR_ENV, 0))
		return 0;

	ret = towed_env_save(save, "initrd_high");
	if (ret < 0)
		return ret;

	ret = env_set("initrd_high", TOWED_ANDROID_INITRD_HIGH_IN_PLACE);
	if (ret) {
		towed_env_restore(save);
		return ret;
	}

	if (IS_ENABLED(CONFIG_TOWED_DEBUG))
		puts("Towed-Boot: keeping Android ramdisk in place\n");

	return 0;
}

static const char *android_boot_mode_name(enum android_boot_mode mode)
{
	switch (mode) {
	case ANDROID_BOOT_MODE_NORMAL:
		return "normal";
	case ANDROID_BOOT_MODE_RECOVERY:
		return "recovery";
	case ANDROID_BOOT_MODE_BOOTLOADER:
		return "bootloader";
	default:
		return "unknown";
	}
}

static void towed_debug_print_handoff(struct bootflow *bflow, ulong boot_addr,
				      ulong vendor_boot_addr)
{
	struct android_priv *priv = bflow->bootmeth_priv;
	const void *vendor_boot = NULL;
	struct andr_image_data data = {0};
	ulong kernel_addr, ramdisk_addr, fdt_addr, dtbo_addr, dtbo_size;
	ulong dtbo_index;
	u32 fdt_size = 0;
	void *fdt_blob;

	if (!IS_ENABLED(CONFIG_TOWED_DEBUG))
		return;

	if (vendor_boot_addr)
		vendor_boot = (const void *)vendor_boot_addr;
	if (!android_image_get_data((const void *)boot_addr, vendor_boot, &data))
		return;

	kernel_addr = env_get_hex(TOWED_ANDROID_KERNEL_ADDR_ENV,
				  data.kernel_addr);
	ramdisk_addr = env_get_hex(TOWED_ANDROID_RAMDISK_ADDR_ENV, 0);
	fdt_addr = env_get_hex(TOWED_ANDROID_FDT_ADDR_ENV, 0);
	dtbo_addr = env_get_hex(TOWED_ANDROID_DTBO_ADDR_ENV, 0);
	dtbo_size = env_get_hex(TOWED_ANDROID_DTBO_SIZE_ENV, 0);
	dtbo_index = env_get_ulong("adtbo_idx", 10,
				   CONFIG_IS_ENABLED(TOWED_BOOT_ANDROID,
						     (CONFIG_TOWED_BOOT_ANDROID_DTBO_INDEX),
						     (0)));

	if (fdt_addr) {
		fdt_blob = map_sysmem(fdt_addr, 0);
		if (!fdt_check_header(fdt_blob))
			fdt_size = fdt_totalsize(fdt_blob);
		unmap_sysmem(fdt_blob);
	}

	printf("Towed-Boot: handoff mode=%s boot=%s header=v%u\n",
	       android_boot_mode_name(priv->boot_mode), priv->boot_part,
	       data.header_version);
	printf("Towed-Boot: handoff kernel load=0x%lx source=0x%lx size=%u KiB\n",
	       kernel_addr, data.kernel_ptr,
	       DIV_ROUND_UP(data.kernel_size, 1024));
	if (ramdisk_addr)
		printf("Towed-Boot: handoff ramdisk=0x%lx-0x%lx size=%u KiB initrd_high=%s\n",
		       ramdisk_addr, ramdisk_addr + data.ramdisk_size,
		       DIV_ROUND_UP(data.ramdisk_size, 1024),
		       env_get("initrd_high") ?: "<unset>");
	if (fdt_addr)
		printf("Towed-Boot: handoff DTB=0x%lx size=%u KiB\n",
		       fdt_addr, DIV_ROUND_UP(fdt_size, 1024));
	if (dtbo_addr)
		printf("Towed-Boot: handoff DTBO table=0x%lx size=%lu KiB requested=%lu\n",
		       dtbo_addr, DIV_ROUND_UP(dtbo_size, 1024),
		       dtbo_index);
}

static int stage_android_components(ulong boot_addr, ulong vendor_boot_addr)
{
	const void *vendor_boot = NULL;
	struct andr_image_data data = {0};
	ulong kernel_addr, ramdisk_addr, fdt_addr, dtb_src;
	u32 dtb_index, dtb_size;

	clear_staged_component_env();

	if (vendor_boot_addr)
		vendor_boot = (const void *)vendor_boot_addr;

	if (!android_image_get_data((const void *)boot_addr, vendor_boot, &data))
		return -EINVAL;

	kernel_addr = env_get_hex("android_kernel_addr_r", 0);
	if (kernel_addr)
		env_set_hex(TOWED_ANDROID_KERNEL_ADDR_ENV, kernel_addr);

	/*
	 * The arm64 Image relocation can overlap the v0-v2 ramdisk and DTB
	 * stored after it. Get them the fuck out of the way before bootm moves
	 * the kernel.
	 */
	if (data.header_version <= 2 && data.ramdisk_size) {
		ramdisk_addr = env_get_hex("android_ramdisk_addr_r", 0);
		if (!ramdisk_addr)
			return -EINVAL;

		memmove((void *)ramdisk_addr, (const void *)data.ramdisk_ptr,
			data.ramdisk_size);
		env_set_hex(TOWED_ANDROID_RAMDISK_ADDR_ENV, ramdisk_addr);
	}

	fdt_addr = env_get_hex("android_fdt_addr_r", 0);
	dtb_index = env_get_ulong("adtb_idx", 10, 0);
	if (fdt_addr) {
		if (!android_image_get_dtb_by_index(boot_addr,
						    vendor_boot_addr ?
						    vendor_boot_addr : -1,
						    dtb_index, &dtb_src,
						    &dtb_size))
			return -EINVAL;

		memmove((void *)fdt_addr, (const void *)dtb_src, dtb_size);
		env_set_hex(TOWED_ANDROID_FDT_ADDR_ENV, fdt_addr);
	}

	printf("Towed-Boot: staged kernel at 0x%lx", kernel_addr);
	if (env_get_hex(TOWED_ANDROID_RAMDISK_ADDR_ENV, 0))
		printf(", ramdisk at 0x%lx",
		       env_get_hex(TOWED_ANDROID_RAMDISK_ADDR_ENV, 0));
	if (env_get_hex(TOWED_ANDROID_FDT_ADDR_ENV, 0))
		printf(", DTB at 0x%lx", fdt_addr);
	puts("\n");

	return 0;
}

static void load_dtbo_partition(struct bootflow *bflow)
{
	struct blk_desc *desc = dev_get_uclass_plat(bflow->blk);
	struct android_priv *priv = bflow->bootmeth_priv;
	ulong dtboaddr = env_get_hex("dtbo_addr_r", 0);
	int ret;

	clear_dtbo_env();

	if (!priv->dtbo_img_size)
		return;

	if (!dtboaddr) {
		puts("Towed-Boot: dtbo_addr_r is unset, skipping DTBO\n");
		return;
	}

	ret = read_partition(desc, priv->dtbo_part, 0, priv->dtbo_img_size,
			     dtboaddr);
	if (ret < 0) {
		printf("Towed-Boot: failed to read %s DTBO table (%d)\n",
		       priv->dtbo_part, ret);
		return;
	}

	env_set_hex(TOWED_ANDROID_DTBO_ADDR_ENV, dtboaddr);
	env_set_hex(TOWED_ANDROID_DTBO_SIZE_ENV, priv->dtbo_img_size);
	printf("Towed-Boot: loaded %s DTBO table at 0x%lx (%u KiB)\n",
	       priv->dtbo_part, dtboaddr,
	       DIV_ROUND_UP(priv->dtbo_img_size, 1024));
}

#if CONFIG_IS_ENABLED(AVB_VERIFY)
static int avb_append_commandline_arg(struct bootflow *bflow, char *arg)
{
	char *key = strsep(&arg, "=");
	char *value = arg;
	int ret;

	ret = bootflow_cmdline_set_arg(bflow, key, value, false);
	if (ret < 0)
		return log_msg_ret("avb cmdline", ret);

	return 0;
}

static int avb_append_commandline(struct bootflow *bflow, char *cmdline)
{
	char *arg = strsep(&cmdline, " ");
	int ret;

	while (arg) {
		ret = avb_append_commandline_arg(bflow, arg);
		if (ret < 0)
			return ret;

		arg = strsep(&cmdline, " ");
	}

	return 0;
}

static int run_avb_verification(struct bootflow *bflow)
{
	struct blk_desc *desc = dev_get_uclass_plat(bflow->blk);
	struct android_priv *priv = bflow->bootmeth_priv;
	const char * const requested_partitions[] = {"boot", "vendor_boot", NULL};
	struct AvbOps *avb_ops;
	AvbSlotVerifyResult result;
	AvbSlotVerifyData *out_data;
	enum avb_boot_state boot_state;
	char *extra_args;
	char slot_suffix[3] = "";
	bool unlocked = false;
	int ret;

	if (desc->uclass_id != UCLASS_MMC)
		return log_msg_ret("avb non-mmc", -EOPNOTSUPP);

	avb_ops = avb_ops_alloc(desc->devnum);
	if (!avb_ops)
		return log_msg_ret("avb ops", -ENOMEM);

	if (priv->slot)
		sprintf(slot_suffix, "_%s", priv->slot);

	ret = avb_ops->read_is_device_unlocked(avb_ops, &unlocked);
	if (ret != AVB_IO_RESULT_OK)
		return log_msg_ret("avb lock", -EIO);

	result = avb_slot_verify(avb_ops,
				 requested_partitions,
				 slot_suffix,
				 unlocked,
				 AVB_HASHTREE_ERROR_MODE_RESTART_AND_INVALIDATE,
				 &out_data);

	if (!unlocked) {
		/* When device is locked, we only accept AVB_SLOT_VERIFY_RESULT_OK */
		if (result != AVB_SLOT_VERIFY_RESULT_OK) {
			printf("Verification failed, reason: %s\n",
			       str_avb_slot_error(result));
			if (out_data)
				avb_slot_verify_data_free(out_data);
			return log_msg_ret("avb verify", -EIO);
		}
		boot_state = AVB_GREEN;
	} else {
		/* When device is unlocked, we also accept verification errors */
		if (result != AVB_SLOT_VERIFY_RESULT_OK &&
		    result != AVB_SLOT_VERIFY_RESULT_ERROR_VERIFICATION) {
			printf("Unlocked verification failed, reason: %s\n",
			       str_avb_slot_error(result));
			if (out_data)
				avb_slot_verify_data_free(out_data);
			return log_msg_ret("avb verify unlocked", -EIO);
		}
		boot_state = AVB_ORANGE;
	}

	extra_args = avb_set_state(avb_ops, boot_state);
	if (extra_args) {
		/* extra_args will be modified after this. This is fine */
		ret = avb_append_commandline_arg(bflow, extra_args);
		if (ret < 0)
			goto free_out_data;
	}

	if (result == AVB_SLOT_VERIFY_RESULT_OK) {
		ret = avb_append_commandline(bflow, out_data->cmdline);
		if (ret < 0)
			goto free_out_data;
	}

	return 0;

 free_out_data:
	if (out_data)
		avb_slot_verify_data_free(out_data);

	return log_msg_ret("avb cmdline", ret);
}
#else
static int run_avb_verification(struct bootflow *bflow)
{
	int ret;

	/* When AVB is unsupported, pass ORANGE state  */
	ret = bootflow_cmdline_set_arg(bflow,
				       "androidboot.verifiedbootstate",
				       "orange", false);
	if (ret < 0)
		return log_msg_ret("avb cmdline", ret);

	return 0;
}
#endif /* AVB_VERIFY */

static int append_bootargs_to_cmdline(struct bootflow *bflow)
{
	char *bootargs;
	int len = 0;

	/*
	 * Check any additionnal bootargs coming from U-Boot env. If any,
	 * merge them with the current cmdline
	 */
	bootargs = env_get("bootargs");
	if (bootargs) {
		len += strlen(bootargs) + 1; /* Extra space character needed */
		len += strlen(bflow->cmdline);

		char *newcmdline = malloc(len + 1); /* +1 for the '\0' */

		if (!newcmdline)
			return log_msg_ret("newcmdline malloc", -ENOMEM);

		strcpy(newcmdline, bootargs);
		strcat(newcmdline, " ");
		strcat(newcmdline, bflow->cmdline);

		/* Free the previous cmdline and replace it */
		free(bflow->cmdline);
		bflow->cmdline = newcmdline;
	}

	return 0;
}

static int boot_android_normal(struct bootflow *bflow)
{
	struct blk_desc *desc = dev_get_uclass_plat(bflow->blk);
	struct android_priv *priv = bflow->bootmeth_priv;
	struct towed_saved_env bootargs_save = {0};
	struct towed_saved_env initrd_high_save = {0};
	int restore_ret;
	int ret;
	ulong loadaddr = env_get_hex("android_boot_addr_r",
				     env_get_hex("loadaddr", 0));
	ulong vloadaddr = env_get_hex("vendor_boot_comp_addr_r", 0);

	ret = run_avb_verification(bflow);
	if (ret < 0)
		return log_msg_ret("avb", ret);

	/* Read slot once more to decrement counter from BCB */
	ret = android_read_slot_from_bcb(bflow, true);
	if (ret < 0)
		return log_msg_ret("read slot", ret);

	ret = read_partition(desc, priv->boot_part, priv->boot_img_offset,
			     priv->boot_img_size, loadaddr);
	if (ret < 0)
		return log_msg_ret("read boot", ret);

	if (priv->header_version >= 3) {
		ret = read_partition(desc, priv->vendor_boot_part, 0,
				     priv->vendor_boot_img_size, vloadaddr);
		if (ret < 0)
			return log_msg_ret("read vendor_boot", ret);
		set_avendor_bootimg_addr(vloadaddr);
	}
	set_abootimg_addr(loadaddr);
	load_dtbo_partition(bflow);

	ret = stage_android_components(loadaddr,
				       priv->header_version >= 3 ? vloadaddr : 0);
	if (ret < 0)
		return log_msg_ret("stage Android components", ret);

	if (priv->slot)
		free(priv->slot);

	ret = towed_env_save(&bootargs_save, "bootargs");
	if (ret < 0)
		return log_msg_ret("bootargs save", ret);

	ret = append_bootargs_to_cmdline(bflow);
	if (ret < 0)
		goto restore_env;

	ret = towed_android_pin_initrd_high(&initrd_high_save);
	if (ret < 0)
		goto restore_env;

	towed_debug_print_handoff(bflow, loadaddr,
				  priv->header_version >= 3 ? vloadaddr : 0);
	ret = bootm_boot_start(loadaddr, bflow->cmdline);

	if (initrd_high_save.name) {
		restore_ret = towed_env_restore(&initrd_high_save);
		if (!ret && restore_ret)
			ret = restore_ret;
	}
restore_env:
	restore_ret = towed_env_restore(&bootargs_save);
	if (!ret && restore_ret)
		ret = restore_ret;

	clear_staged_component_env();

	return log_msg_ret("boot", ret);
}

static int boot_android_recovery(struct bootflow *bflow)
{
	int ret;

	ret = boot_android_normal(bflow);

	return log_msg_ret("boot", ret);
}

static int boot_android_bootloader(struct bootflow *bflow)
{
	int ret;

	ret = run_command("fastboot usb 0", 0);
	do_reset(NULL, 0, 0, NULL);

	return log_msg_ret("boot", ret);
}

static int android_boot(struct udevice *dev, struct bootflow *bflow)
{
	struct android_priv *priv = bflow->bootmeth_priv;
	int ret;

	switch (priv->boot_mode) {
	case ANDROID_BOOT_MODE_NORMAL:
		ret = boot_android_normal(bflow);
		break;
	case ANDROID_BOOT_MODE_RECOVERY:
		ret = boot_android_recovery(bflow);
		break;
	case ANDROID_BOOT_MODE_BOOTLOADER:
		ret = boot_android_bootloader(bflow);
		break;
	default:
		printf("ANDROID: Unknown boot mode %d. Running fastboot...\n",
		       priv->boot_mode);
		boot_android_bootloader(bflow);
		/* Tell we failed to boot since boot mode is unknown */
		ret = -EFAULT;
	}

	return ret;
}

static int android_bootmeth_bind(struct udevice *dev)
{
	struct bootmeth_uc_plat *plat = dev_get_uclass_plat(dev);

	plat->desc = "Android boot";
	plat->flags = BOOTMETHF_ANY_PART;

	return 0;
}

static struct bootmeth_ops android_bootmeth_ops = {
	.check		= android_check,
	.read_bootflow	= android_read_bootflow,
	.read_file	= android_read_file,
	.boot		= android_boot,
};

static const struct udevice_id android_bootmeth_ids[] = {
	{ .compatible = "u-boot,android" },
	{ }
};

U_BOOT_DRIVER(bootmeth_android) = {
	.name		= "bootmeth_android",
	.id		= UCLASS_BOOTMETH,
	.of_match	= android_bootmeth_ids,
	.ops		= &android_bootmeth_ops,
	.bind		= android_bootmeth_bind,
};

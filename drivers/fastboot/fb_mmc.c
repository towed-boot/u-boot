// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright 2014 Broadcom Corporation.
 */

#include <config.h>
#include <blk.h>
#include <env.h>
#include <fastboot.h>
#include <fastboot-internal.h>
#include <fb_block.h>
#include <fb_mmc.h>
#include <image-sparse.h>
#include <image.h>
#include <log.h>
#include <malloc.h>
#include <part.h>
#include <mmc.h>
#include <div64.h>
#include <linux/compat.h>
#include <linux/string.h>
#include <android_image.h>

#define BOOT_PARTITION_NAME "boot"

static bool fastboot_towed_boot_is_boot_partition(const char *part_name)
{
	if (!strcmp(part_name, "boot"))
		return true;

	return (!strcmp(part_name, "boot_a") || !strcmp(part_name, "boot_b"));
}

static int fastboot_mmc_read_part(struct blk_desc *dev_desc,
				  struct disk_partition *info, u64 offset,
				  void *buffer, u32 bytes)
{
	u64 partition_size = (u64)info->size * info->blksz;
	uint block_offset;
	lbaint_t start;
	lbaint_t blkcnt;
	void *bounce;
	int ret;

	if (offset > partition_size || bytes > partition_size - offset)
		return -EINVAL;

	block_offset = do_div(offset, info->blksz);
	start = info->start + offset;
	blkcnt = DIV_ROUND_UP(block_offset + bytes, info->blksz);

	bounce = malloc(blkcnt * info->blksz);
	if (!bounce)
		return -ENOMEM;

	ret = blk_dread(dev_desc, start, blkcnt, bounce);
	if (ret != blkcnt) {
		free(bounce);
		return -EIO;
	}

	memcpy(buffer, (u8 *)bounce + block_offset, bytes);
	free(bounce);

	return 0;
}

static int fastboot_mmc_read_part_aligned(struct blk_desc *dev_desc,
					  struct disk_partition *info,
					  u64 offset, void *buffer,
					  u32 bytes)
{
	lbaint_t blkcnt;

	if (offset % info->blksz || bytes % info->blksz)
		return -EINVAL;

	blkcnt = bytes / info->blksz;
	if (blk_dread(dev_desc, info->start + offset / info->blksz, blkcnt,
		      buffer) != blkcnt)
		return -EIO;

	return 0;
}

static bool fastboot_towed_boot_android_v2(const void *buffer, u32 size,
					   u32 *boot_img_size)
{
	const struct andr_boot_img_hdr_v0 *hdr = buffer;

	if (!is_android_boot_image_header(buffer) || hdr->header_version > 2)
		return false;

	if (!android_image_get_bootimg_size(buffer, boot_img_size))
		return false;

	return !size || *boot_img_size <= size;
}

static bool fastboot_mmc_part_contains_towed_boot(struct blk_desc *dev_desc,
						  struct disk_partition *info,
						  const void *hdr)
{
	const struct andr_boot_img_hdr_v0 *boot_hdr = hdr;
	u8 kernel_header[0x60];

	if (!is_android_boot_image_header(hdr) || boot_hdr->header_version > 2)
		return false;

	if (fastboot_mmc_read_part(dev_desc, info, boot_hdr->page_size,
				   kernel_header, sizeof(kernel_header)))
		return false;

	return is_arm64_u_boot_image(kernel_header, sizeof(kernel_header));
}

static bool fastboot_mmc_try_wrap_towed_boot(struct blk_desc *dev_desc,
					     struct disk_partition *info,
					     const char *part_name,
					     void *download_buffer,
					     u32 download_bytes,
					     char *response)
{
	struct andr_boot_img_hdr_v0 current_hdr;
	u32 current_size, payload_size;
	u64 payload_offset, write_size, write_blks;
	u64 partition_size = (u64)info->size * info->blksz;
	void *payload_header;
	int ret;

	if (!IS_ENABLED(CONFIG_TOWED_BOOT_ANDROID))
		return false;

	if (!fastboot_towed_boot_is_boot_partition(part_name))
		return false;

	if (!fastboot_towed_boot_android_v2(download_buffer, download_bytes,
					    &payload_size))
		return false;

	if (android_boot_image_has_arm64_u_boot(download_buffer, download_bytes))
		return false;

	ret = fastboot_mmc_read_part(dev_desc, info, 0, &current_hdr,
				     sizeof(current_hdr));
	if (ret)
		return false;

	if (!fastboot_towed_boot_android_v2(&current_hdr, 0, &current_size))
		return false;

	if (!fastboot_mmc_part_contains_towed_boot(dev_desc, info, &current_hdr))
		return false;

	payload_offset = ALIGN((u64)current_size +
			       TOWED_BOOT_ANDROID_PAYLOAD_HEADER_SIZE,
			       current_hdr.page_size);
	write_size = payload_offset + payload_size;
	write_blks = DIV_ROUND_UP(write_size, info->blksz);

	if (payload_offset > U32_MAX || write_size > partition_size ||
	    write_size > U32_MAX) {
		fastboot_fail("hybrid boot image too large", response);
		return true;
	}

	if (write_blks * info->blksz > fastboot_buf_size) {
		fastboot_fail("fastboot buffer too small for hybrid boot image",
			      response);
		return true;
	}

	memmove((u8 *)download_buffer + payload_offset, download_buffer,
		payload_size);

	ret = fastboot_mmc_read_part_aligned(dev_desc, info, 0, download_buffer,
					     current_size);
	if (ret) {
		fastboot_fail("failed to read current Towed-Boot image",
			      response);
		return true;
	}

	payload_header = (u8 *)download_buffer + current_size;
	towed_boot_android_payload_init(payload_header, (u32)payload_offset,
					payload_size);

	if (payload_offset > current_size + TOWED_BOOT_ANDROID_PAYLOAD_HEADER_SIZE)
		memset((u8 *)download_buffer + current_size +
		       TOWED_BOOT_ANDROID_PAYLOAD_HEADER_SIZE, 0,
		       payload_offset - current_size -
		       TOWED_BOOT_ANDROID_PAYLOAD_HEADER_SIZE);

	if (write_blks * info->blksz > write_size)
		memset((u8 *)download_buffer + write_size, 0,
		       write_blks * info->blksz - write_size);

	printf("Towed-Boot: injecting into %s Android boot image\n",
	       part_name);
	fastboot_block_write_raw_image(dev_desc, info, part_name,
				       download_buffer, (u32)write_size,
				       response);

	return true;
}

static int raw_part_get_info_by_name(struct blk_desc *dev_desc,
				     const char *name,
				     struct disk_partition *info)
{
	/* strlen("fastboot_raw_partition_") + PART_NAME_LEN + 1 */
	char env_desc_name[23 + PART_NAME_LEN + 1];
	char *raw_part_desc;
	const char *argv[2];
	const char **parg = argv;

	/* check for raw partition descriptor */
	strcpy(env_desc_name, "fastboot_raw_partition_");
	strlcat(env_desc_name, name, sizeof(env_desc_name));
	raw_part_desc = strdup(env_get(env_desc_name));
	if (raw_part_desc == NULL)
		return -ENODEV;

	/*
	 * parse partition descriptor
	 *
	 * <lba_start> <lba_size> [mmcpart <num>]
	 */
	for (; parg < argv + sizeof(argv) / sizeof(*argv); ++parg) {
		*parg = strsep(&raw_part_desc, " ");
		if (*parg == NULL) {
			pr_err("Invalid number of arguments.\n");
			return -ENODEV;
		}
	}

	info->start = simple_strtoul(argv[0], NULL, 0);
	info->size = simple_strtoul(argv[1], NULL, 0);
	info->blksz = dev_desc->blksz;
	strlcpy((char *)info->name, name, PART_NAME_LEN);

	if (raw_part_desc) {
		if (strcmp(strsep(&raw_part_desc, " "), "mmcpart") == 0) {
			ulong mmcpart = simple_strtoul(raw_part_desc, NULL, 0);
			int ret = blk_dselect_hwpart(dev_desc, mmcpart);

			if (ret)
				return ret;
		}
	}

	return 0;
}

static int do_get_part_info(struct blk_desc **dev_desc, const char *name,
			    struct disk_partition *info)
{
	int ret;

	/* First try partition names on the default device */
	*dev_desc = blk_get_dev("mmc", CONFIG_FASTBOOT_FLASH_MMC_DEV);
	if (*dev_desc) {
		ret = part_get_info_by_name(*dev_desc, name, info);
		if (ret >= 0)
			return ret;

		/* Then try raw partitions */
		ret = raw_part_get_info_by_name(*dev_desc, name, info);
		if (ret >= 0)
			return ret;
	}

	/* Then try dev.hwpart:part */
	ret = part_get_info_by_dev_and_name_or_num("mmc", name, dev_desc,
						   info, true);
	return ret;
}

static int part_get_info_by_name_or_alias(struct blk_desc **dev_desc,
					  const char *name,
					  struct disk_partition *info)
{
	/* strlen("fastboot_partition_alias_") + PART_NAME_LEN + 1 */
	char env_alias_name[25 + PART_NAME_LEN + 1];
	char *aliased_part_name;

	/* check for alias */
	strlcpy(env_alias_name, "fastboot_partition_alias_", sizeof(env_alias_name));
	strlcat(env_alias_name, name, sizeof(env_alias_name));
	aliased_part_name = env_get(env_alias_name);
	if (aliased_part_name)
		name = aliased_part_name;

	return do_get_part_info(dev_desc, name, info);
}

#ifdef CONFIG_FASTBOOT_MMC_BOOT_SUPPORT
static void fb_mmc_boot_ops(struct blk_desc *dev_desc, void *buffer,
			    int hwpart, u32 buff_sz, char *response)
{
	// To operate on EMMC_BOOT1/2 (mmc0boot0/1) we first change the hwpart
	if (blk_dselect_hwpart(dev_desc, hwpart)) {
		pr_err("Failed to select hwpart\n");
		fastboot_fail("Failed to select hwpart", response);
		return;
	}

	if (buffer) /* flash */
		fastboot_block_write_raw_disk(dev_desc, "EMMC_BOOT",
					      buffer, buff_sz, response);
	else /* erase */
		fastboot_block_raw_erase_disk(dev_desc, "EMMC_BOOT", response);
}
#endif

#ifdef CONFIG_ANDROID_BOOT_IMAGE
/**
 * Read Android boot image header from boot partition.
 *
 * @param[in] dev_desc MMC device descriptor
 * @param[in] info Boot partition info
 * @param[out] hdr Where to store read boot image header
 *
 * Return: Boot image header sectors count or 0 on error
 */
static lbaint_t fb_mmc_get_boot_header(struct blk_desc *dev_desc,
				       struct disk_partition *info,
				       struct andr_boot_img_hdr_v0 *hdr,
				       char *response)
{
	ulong sector_size;		/* boot partition sector size */
	lbaint_t hdr_sectors;		/* boot image header sectors count */
	int res;

	/* Calculate boot image sectors count */
	sector_size = info->blksz;
	hdr_sectors = DIV_ROUND_UP(sizeof(struct andr_boot_img_hdr_v0), sector_size);
	if (hdr_sectors == 0) {
		pr_err("invalid number of boot sectors: 0\n");
		fastboot_fail("invalid number of boot sectors: 0", response);
		return 0;
	}

	/* Read the boot image header */
	res = blk_dread(dev_desc, info->start, hdr_sectors, (void *)hdr);
	if (res != hdr_sectors) {
		pr_err("cannot read header from boot partition\n");
		fastboot_fail("cannot read header from boot partition",
			      response);
		return 0;
	}

	/* Check boot header magic string */
	if (!is_android_boot_image_header(hdr)) {
		pr_err("bad boot image magic\n");
		fastboot_fail("boot partition not initialized", response);
		return 0;
	}

	return hdr_sectors;
}

/**
 * Write downloaded zImage to boot partition and repack it properly.
 *
 * @param dev_desc MMC device descriptor
 * @param download_buffer Address to fastboot buffer with zImage in it
 * @param download_bytes Size of fastboot buffer, in bytes
 *
 * Return: 0 on success or -1 on error
 */
static int fb_mmc_update_zimage(struct blk_desc *dev_desc,
				void *download_buffer,
				u32 download_bytes,
				char *response)
{
	uintptr_t hdr_addr;			/* boot image header address */
	struct andr_boot_img_hdr_v0 *hdr;		/* boot image header */
	lbaint_t hdr_sectors;			/* boot image header sectors */
	u8 *ramdisk_buffer;
	u32 ramdisk_sector_start;
	u32 ramdisk_sectors;
	u32 kernel_sector_start;
	u32 kernel_sectors;
	u32 sectors_per_page;
	struct disk_partition info;
	int res;

	puts("Flashing zImage\n");

	/* Get boot partition info */
	res = part_get_info_by_name(dev_desc, BOOT_PARTITION_NAME, &info);
	if (res < 0) {
		pr_err("cannot find boot partition\n");
		fastboot_fail("cannot find boot partition", response);
		return -1;
	}

	/* Put boot image header in fastboot buffer after downloaded zImage */
	hdr_addr = (uintptr_t)download_buffer + ALIGN(download_bytes, PAGE_SIZE);
	hdr = (struct andr_boot_img_hdr_v0 *)hdr_addr;

	/* Read boot image header */
	hdr_sectors = fb_mmc_get_boot_header(dev_desc, &info, hdr, response);
	if (hdr_sectors == 0) {
		pr_err("unable to read boot image header\n");
		fastboot_fail("unable to read boot image header", response);
		return -1;
	}

	/* Check if boot image header version is 2 or less */
	if (hdr->header_version > 2) {
		pr_err("zImage flashing supported only for boot images v2 and less\n");
		fastboot_fail("zImage flashing supported only for boot images v2 and less",
			      response);
		return -EOPNOTSUPP;
	}

	/* Check if boot image has second stage in it (we don't support it) */
	if (hdr->second_size > 0) {
		pr_err("moving second stage is not supported yet\n");
		fastboot_fail("moving second stage is not supported yet",
			      response);
		return -1;
	}

	/* Extract ramdisk location */
	sectors_per_page = hdr->page_size / info.blksz;
	ramdisk_sector_start = info.start + sectors_per_page;
	ramdisk_sector_start += DIV_ROUND_UP(hdr->kernel_size, hdr->page_size) *
					     sectors_per_page;
	ramdisk_sectors = DIV_ROUND_UP(hdr->ramdisk_size, hdr->page_size) *
				       sectors_per_page;

	/* Read ramdisk and put it in fastboot buffer after boot image header */
	ramdisk_buffer = (u8 *)hdr + (hdr_sectors * info.blksz);
	res = blk_dread(dev_desc, ramdisk_sector_start, ramdisk_sectors,
			ramdisk_buffer);
	if (res != ramdisk_sectors) {
		pr_err("cannot read ramdisk from boot partition\n");
		fastboot_fail("cannot read ramdisk from boot partition",
			      response);
		return -1;
	}

	/* Write new kernel size to boot image header */
	hdr->kernel_size = download_bytes;
	res = blk_dwrite(dev_desc, info.start, hdr_sectors, (void *)hdr);
	if (res == 0) {
		pr_err("cannot writeback boot image header\n");
		fastboot_fail("cannot write back boot image header", response);
		return -1;
	}

	/* Write the new downloaded kernel */
	kernel_sector_start = info.start + sectors_per_page;
	kernel_sectors = DIV_ROUND_UP(hdr->kernel_size, hdr->page_size) *
				      sectors_per_page;
	res = blk_dwrite(dev_desc, kernel_sector_start, kernel_sectors,
			 download_buffer);
	if (res == 0) {
		pr_err("cannot write new kernel\n");
		fastboot_fail("cannot write new kernel", response);
		return -1;
	}

	/* Write the saved ramdisk back */
	ramdisk_sector_start = info.start + sectors_per_page;
	ramdisk_sector_start += DIV_ROUND_UP(hdr->kernel_size, hdr->page_size) *
					     sectors_per_page;
	res = blk_dwrite(dev_desc, ramdisk_sector_start, ramdisk_sectors,
			 ramdisk_buffer);
	if (res == 0) {
		pr_err("cannot write back original ramdisk\n");
		fastboot_fail("cannot write back original ramdisk", response);
		return -1;
	}

	puts("........ zImage was updated in boot partition\n");
	fastboot_okay(NULL, response);
	return 0;
}
#endif

/**
 * fastboot_mmc_get_part_info() - Lookup eMMC partion by name
 *
 * @part_name: Named partition to lookup
 * @dev_desc: Pointer to returned blk_desc pointer
 * @part_info: Pointer to returned struct disk_partition
 * @response: Pointer to fastboot response buffer
 */
int fastboot_mmc_get_part_info(const char *part_name,
			       struct blk_desc **dev_desc,
			       struct disk_partition *part_info, char *response)
{
	int ret;

	if (!part_name || !strcmp(part_name, "")) {
		fastboot_fail("partition not given", response);
		return -ENOENT;
	}

	ret = part_get_info_by_name_or_alias(dev_desc, part_name, part_info);
	if (ret < 0) {
		switch (ret) {
		case -ENOSYS:
		case -EINVAL:
			fastboot_fail("invalid partition or device", response);
			break;
		case -ENODEV:
			fastboot_fail("no such device", response);
			break;
		case -ENOENT:
			fastboot_fail("no such partition", response);
			break;
		case -EPROTONOSUPPORT:
			fastboot_fail("unknown partition table type", response);
			break;
		default:
			fastboot_fail("unanticipated error", response);
			break;
		}
	}

	return ret;
}

static struct blk_desc *fastboot_mmc_get_dev(char *response)
{
	struct blk_desc *ret = blk_get_dev("mmc",
					   CONFIG_FASTBOOT_FLASH_MMC_DEV);

	if (!ret || ret->type == DEV_TYPE_UNKNOWN) {
		pr_err("invalid mmc device\n");
		fastboot_fail("invalid mmc device", response);
		return NULL;
	}
	return ret;
}

/**
 * fastboot_mmc_flash_write() - Write image to eMMC for fastboot
 *
 * @cmd: Named partition to write image to
 * @download_buffer: Pointer to image data
 * @download_bytes: Size of image data
 * @response: Pointer to fastboot response buffer
 */
void fastboot_mmc_flash_write(const char *cmd, void *download_buffer,
			      u32 download_bytes, char *response)
{
	struct blk_desc *dev_desc;
	struct disk_partition info = {0};

#ifdef CONFIG_FASTBOOT_MMC_BOOT_SUPPORT
	if (strcmp(cmd, CONFIG_FASTBOOT_MMC_BOOT1_NAME) == 0) {
		dev_desc = fastboot_mmc_get_dev(response);
		if (dev_desc)
			fb_mmc_boot_ops(dev_desc, download_buffer, 1,
					download_bytes, response);
		return;
	}
	if (strcmp(cmd, CONFIG_FASTBOOT_MMC_BOOT2_NAME) == 0) {
		dev_desc = fastboot_mmc_get_dev(response);
		if (dev_desc)
			fb_mmc_boot_ops(dev_desc, download_buffer, 2,
					download_bytes, response);
		return;
	}
#endif

#if CONFIG_IS_ENABLED(EFI_PARTITION)
	if (strcmp(cmd, CONFIG_FASTBOOT_GPT_NAME) == 0) {
		dev_desc = fastboot_mmc_get_dev(response);
		if (!dev_desc)
			return;

		printf("%s: updating MBR, Primary and Backup GPT(s)\n",
		       __func__);
		if (is_valid_gpt_buf(dev_desc, download_buffer)) {
			printf("%s: invalid GPT - refusing to write to flash\n",
			       __func__);
			fastboot_fail("invalid GPT partition", response);
			return;
		}
		if (write_mbr_and_gpt_partitions(dev_desc, download_buffer)) {
			printf("%s: writing GPT partitions failed\n", __func__);
			fastboot_fail("writing GPT partitions failed",
				      response);
			return;
		}
		part_init(dev_desc);
		printf("........ success\n");
		fastboot_okay(NULL, response);
		return;
	}
#endif

#if CONFIG_IS_ENABLED(DOS_PARTITION)
	if (strcmp(cmd, CONFIG_FASTBOOT_MBR_NAME) == 0) {
		dev_desc = fastboot_mmc_get_dev(response);
		if (!dev_desc)
			return;

		printf("%s: updating MBR\n", __func__);
		if (is_valid_dos_buf(download_buffer)) {
			printf("%s: invalid MBR - refusing to write to flash\n",
			       __func__);
			fastboot_fail("invalid MBR partition", response);
			return;
		}
		if (write_mbr_sector(dev_desc, download_buffer)) {
			printf("%s: writing MBR partition failed\n", __func__);
			fastboot_fail("writing MBR partition failed",
				      response);
			return;
		}
		part_init(dev_desc);
		printf("........ success\n");
		fastboot_okay(NULL, response);
		return;
	}
#endif

#ifdef CONFIG_ANDROID_BOOT_IMAGE
	if (strncasecmp(cmd, "zimage", 6) == 0) {
		dev_desc = fastboot_mmc_get_dev(response);
		if (dev_desc)
			fb_mmc_update_zimage(dev_desc, download_buffer,
					     download_bytes, response);
		return;
	}
#endif

#if IS_ENABLED(CONFIG_FASTBOOT_MMC_USER_SUPPORT)
	if (strcmp(cmd, CONFIG_FASTBOOT_MMC_USER_NAME) == 0) {
		dev_desc = fastboot_mmc_get_dev(response);
		if (!dev_desc)
			return;

		strlcpy((char *)&info.name, cmd, sizeof(info.name));
		info.size	= dev_desc->lba;
		info.blksz	= dev_desc->blksz;
	}
#endif

	if (!info.name[0] &&
	    fastboot_mmc_get_part_info(cmd, &dev_desc, &info, response) < 0)
		return;

	if (is_sparse_image(download_buffer)) {
		fastboot_block_write_sparse_image(dev_desc, &info, cmd,
						  download_buffer, response);
	} else {
		if (fastboot_mmc_try_wrap_towed_boot(dev_desc, &info, cmd,
						     download_buffer,
						     download_bytes,
						     response))
			return;

		fastboot_towed_boot_flash_probe(cmd, download_buffer,
						download_bytes);
		fastboot_block_write_raw_image(dev_desc, &info, cmd, download_buffer,
					       download_bytes, response);
	}
}

/**
 * fastboot_mmc_flash_erase() - Erase eMMC for fastboot
 *
 * @cmd: Named partition to erase
 * @response: Pointer to fastboot response buffer
 */
void fastboot_mmc_erase(const char *cmd, char *response)
{
	struct blk_desc *dev_desc;
	struct disk_partition info;
	struct mmc *mmc = find_mmc_device(CONFIG_FASTBOOT_FLASH_MMC_DEV);

#ifdef CONFIG_FASTBOOT_MMC_BOOT_SUPPORT
	if (strcmp(cmd, CONFIG_FASTBOOT_MMC_BOOT1_NAME) == 0) {
		/* erase EMMC boot1 */
		dev_desc = fastboot_mmc_get_dev(response);
		if (dev_desc)
			fb_mmc_boot_ops(dev_desc, NULL, 1, 0, response);
		return;
	}
	if (strcmp(cmd, CONFIG_FASTBOOT_MMC_BOOT2_NAME) == 0) {
		/* erase EMMC boot2 */
		dev_desc = fastboot_mmc_get_dev(response);
		if (dev_desc)
			fb_mmc_boot_ops(dev_desc, NULL, 2, 0, response);
		return;
	}
#endif

#ifdef CONFIG_FASTBOOT_MMC_USER_SUPPORT
	if (strcmp(cmd, CONFIG_FASTBOOT_MMC_USER_NAME) == 0) {
		/* erase EMMC userdata */
		dev_desc = fastboot_mmc_get_dev(response);
		if (!dev_desc)
			return;

		fastboot_block_raw_erase_disk(dev_desc, "EMMC_USER", response);
		return;
	}
#endif

	if (fastboot_mmc_get_part_info(cmd, &dev_desc, &info, response) < 0)
		return;

	/* Align blocks to erase group size to avoid erasing other partitions */
	fastboot_block_raw_erase(dev_desc, &info, cmd, mmc->erase_grp_size, response);
}

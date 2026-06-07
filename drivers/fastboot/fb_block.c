// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2024 The Android Open Source Project
 */

#include <blk.h>
#include <android_image.h>
#include <div64.h>
#include <fastboot.h>
#include <fastboot-internal.h>
#include <fb_block.h>
#include <image.h>
#include <image-sparse.h>
#include <malloc.h>
#include <part.h>
#include <linux/compat.h>
#include <linux/string.h>

/**
 * FASTBOOT_MAX_BLOCKS_ERASE - maximum blocks to erase per derase call
 *
 * in the ERASE case we can have much larger buffer size since
 * we're not transferring an actual buffer
 */
#define FASTBOOT_MAX_BLOCKS_ERASE 1048576
/**
 * FASTBOOT_MAX_BLOCKS_SOFT_ERASE - maximum blocks to software erase at once
 */
#define FASTBOOT_MAX_BLOCKS_SOFT_ERASE 4096
/**
 * FASTBOOT_MAX_BLOCKS_WRITE - maximum blocks to write per dwrite call
 */
#define FASTBOOT_MAX_BLOCKS_WRITE 65536

__weak lbaint_t fb_mmc_get_boot_offset(void)
{
	return 0;
}

struct fb_block_sparse {
	struct blk_desc	*dev_desc;
};

static bool fastboot_block_towed_boot_is_boot_partition(const char *part_name)
{
	if (!strcmp(part_name, "boot"))
		return true;

	return (!strcmp(part_name, "boot_a") || !strcmp(part_name, "boot_b"));
}

static int fastboot_block_read_part(struct blk_desc *dev_desc,
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

static int fastboot_block_read_part_aligned(struct blk_desc *dev_desc,
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

static bool fastboot_block_towed_boot_android_v2(const void *buffer, u32 size,
						 u32 *boot_img_size)
{
	const struct andr_boot_img_hdr_v0 *hdr = buffer;

	if (!is_android_boot_image_header(buffer) || hdr->header_version > 2)
		return false;

	if (!android_image_get_bootimg_size(buffer, boot_img_size))
		return false;

	return !size || *boot_img_size <= size;
}

static bool fastboot_block_part_contains_towed_boot(struct blk_desc *dev_desc,
						    struct disk_partition *info,
						    const void *hdr)
{
	const struct andr_boot_img_hdr_v0 *boot_hdr = hdr;
	u8 kernel_header[0x60];

	if (!is_android_boot_image_header(hdr) || boot_hdr->header_version > 2)
		return false;

	if (fastboot_block_read_part(dev_desc, info, boot_hdr->page_size,
				     kernel_header, sizeof(kernel_header)))
		return false;

	return is_arm64_u_boot_image(kernel_header, sizeof(kernel_header));
}

static bool fastboot_block_try_wrap_towed_boot(struct blk_desc *dev_desc,
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

	if (!fastboot_block_towed_boot_is_boot_partition(part_name))
		return false;

	if (!fastboot_block_towed_boot_android_v2(download_buffer,
						  download_bytes,
						  &payload_size))
		return false;

	if (android_boot_image_has_arm64_u_boot(download_buffer, download_bytes))
		return false;

	ret = fastboot_block_read_part(dev_desc, info, 0, &current_hdr,
				       sizeof(current_hdr));
	if (ret)
		return false;

	if (!fastboot_block_towed_boot_android_v2(&current_hdr, 0,
						  &current_size))
		return false;

	if (!fastboot_block_part_contains_towed_boot(dev_desc, info,
						     &current_hdr))
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

	ret = fastboot_block_read_part_aligned(dev_desc, info, 0,
					       download_buffer, current_size);
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

/* Write 0s instead of using erase operation, inefficient but functional */
static lbaint_t fb_block_soft_erase(struct blk_desc *block_dev, lbaint_t blk,
				    lbaint_t cur_blkcnt, lbaint_t erase_buf_blks,
				    void *erase_buffer)
{
	lbaint_t blks_written = 0;
	lbaint_t j;

	memset(erase_buffer, 0, erase_buf_blks * block_dev->blksz);

	for (j = 0; j < cur_blkcnt; j += erase_buf_blks) {
		lbaint_t remain = min(cur_blkcnt - j, erase_buf_blks);

		blks_written += blk_dwrite(block_dev, blk + j,
					   remain, erase_buffer);
		printf(".");
	}

	return blks_written;
}

static lbaint_t fb_block_write(struct blk_desc *block_dev, lbaint_t start,
			       lbaint_t blkcnt, const void *buffer)
{
	lbaint_t blk = start;
	lbaint_t blks_written = 0;
	lbaint_t blks = 0;
	void *erase_buf = NULL;
	int erase_buf_blks = 0;
	lbaint_t step = buffer ? FASTBOOT_MAX_BLOCKS_WRITE : FASTBOOT_MAX_BLOCKS_ERASE;
	lbaint_t i;

	for (i = 0; i < blkcnt; i += step) {
		lbaint_t cur_blkcnt = min(blkcnt - i, step);

		if (buffer) {
			if (fastboot_progress_callback)
				fastboot_progress_callback("writing");
			blks_written = blk_dwrite(block_dev, blk, cur_blkcnt,
						  buffer + (i * block_dev->blksz));
		} else {
			if (fastboot_progress_callback)
				fastboot_progress_callback("erasing");

			if (!erase_buf) {
				blks_written = blk_derase(block_dev, blk, cur_blkcnt);

				/* Allocate erase buffer if erase is not implemented */
				if ((long)blks_written == -ENOSYS) {
					erase_buf_blks = min_t(long, blkcnt,
							       FASTBOOT_MAX_BLOCKS_SOFT_ERASE);
					erase_buf = malloc(erase_buf_blks * block_dev->blksz);

					printf("Slowly writing empty buffers due to missing erase operation\n");
				}
			}

			if (erase_buf)
				blks_written = fb_block_soft_erase(block_dev, blk, cur_blkcnt,
								   erase_buf_blks, erase_buf);
		}
		blk += blks_written;
		blks += blks_written;
	}

	if (erase_buf)
		free(erase_buf);

	return blks;
}

static lbaint_t fb_block_sparse_write(struct sparse_storage *info,
				      lbaint_t blk, lbaint_t blkcnt,
				      const void *buffer)
{
	struct fb_block_sparse *sparse = info->priv;
	struct blk_desc *dev_desc = sparse->dev_desc;

	return fb_block_write(dev_desc, blk, blkcnt, buffer);
}

static lbaint_t fb_block_sparse_reserve(struct sparse_storage *info,
					lbaint_t blk, lbaint_t blkcnt)
{
	return blkcnt;
}

int fastboot_block_get_part_info(const char *part_name,
				 struct blk_desc **dev_desc,
				 struct disk_partition *part_info,
				 char *response)
{
	int ret;
	const char *interface = config_opt_enabled(CONFIG_FASTBOOT_FLASH_BLOCK,
						   CONFIG_FASTBOOT_FLASH_BLOCK_INTERFACE_NAME,
						   NULL);
	const int device = config_opt_enabled(CONFIG_FASTBOOT_FLASH_BLOCK,
					      CONFIG_FASTBOOT_FLASH_BLOCK_DEVICE_ID, -1);

	if (!part_name || !strcmp(part_name, "")) {
		fastboot_fail("partition not given", response);
		return -ENOENT;
	}
	if (!interface || !strcmp(interface, "")) {
		fastboot_fail("block interface isn't provided", response);
		return -EINVAL;
	}

	*dev_desc = blk_get_dev(interface, device);
	if (!dev_desc) {
		fastboot_fail("no such device", response);
		return -ENODEV;
	}

	ret = part_get_info_by_name(*dev_desc, part_name, part_info);
	if (ret < 0)
		fastboot_fail("failed to get partition info", response);

	return ret;
}

void fastboot_block_raw_erase_disk(struct blk_desc *dev_desc, const char *disk_name,
				   char *response)
{
	lbaint_t written;

	debug("Start Erasing %s...\n", disk_name);

	written = fb_block_write(dev_desc, fb_mmc_get_boot_offset(),
				 dev_desc->lba, NULL);
	if (written != dev_desc->lba) {
		pr_err("Failed to erase %s\n", disk_name);
		fastboot_response("FAIL", response, "Failed to erase %s", disk_name);
		return;
	}

	printf("........ erased " LBAFU " bytes from '%s'\n",
	       dev_desc->lba * dev_desc->blksz, disk_name);
	fastboot_okay(NULL, response);
}

void fastboot_block_raw_erase(struct blk_desc *dev_desc, struct disk_partition *info,
			      const char *part_name, uint alignment, char *response)
{
	lbaint_t written, blks_start, blks_size;

	if (alignment) {
		blks_start = (info->start + alignment - 1) & ~(alignment - 1);
		if (info->size >= alignment)
			blks_size = (info->size - (blks_start - info->start)) &
				(~(alignment - 1));
		else
			blks_size = 0;

		printf("Erasing blocks " LBAFU " to " LBAFU " due to alignment\n",
		       blks_start, blks_start + blks_size);
	} else {
		blks_start = info->start;
		blks_size = info->size;
	}

	written = fb_block_write(dev_desc, blks_start, blks_size, NULL);
	if (written != blks_size) {
		fastboot_fail("failed to erase partition", response);
		return;
	}

	printf("........ erased " LBAFU " bytes from '%s'\n",
	       blks_size * info->blksz, part_name);
	fastboot_okay(NULL, response);
}

void fastboot_block_erase(const char *part_name, char *response)
{
	struct blk_desc *dev_desc;
	struct disk_partition part_info;

	if (fastboot_block_get_part_info(part_name, &dev_desc, &part_info, response) < 0)
		return;

	fastboot_block_raw_erase(dev_desc, &part_info, part_name,
				 fb_mmc_get_boot_offset(), response);
}

void fastboot_block_write_raw_disk(struct blk_desc *dev_desc, const char *disk_name,
				   void *buffer, u32 download_bytes, char *response)
{
	lbaint_t blkcnt;
	lbaint_t blks;

	/* determine number of blocks to write */
	blkcnt = ((download_bytes + (dev_desc->blksz - 1)) & ~(dev_desc->blksz - 1));
	blkcnt = lldiv(blkcnt, dev_desc->blksz);

	if ((blkcnt + fb_mmc_get_boot_offset()) > dev_desc->lba) {
		pr_err("too large for disk: '%s'\n", disk_name);
		fastboot_fail("too large for disk", response);
		return;
	}

	printf("Flashing Raw Image\n");

	blks = fb_block_write(dev_desc, fb_mmc_get_boot_offset(), blkcnt, buffer);

	if (blks != blkcnt) {
		pr_err("failed writing to %s\n", disk_name);
		fastboot_fail("failed writing to device", response);
		return;
	}

	printf("........ wrote " LBAFU " bytes to '%s'\n", blkcnt * dev_desc->blksz,
	       disk_name);
	fastboot_okay(NULL, response);
}

void fastboot_block_write_raw_image(struct blk_desc *dev_desc,
				    struct disk_partition *info, const char *part_name,
				    void *buffer, u32 download_bytes, char *response)
{
	lbaint_t blkcnt;
	lbaint_t blks;

	/* determine number of blocks to write */
	blkcnt = ((download_bytes + (info->blksz - 1)) & ~(info->blksz - 1));
	blkcnt = lldiv(blkcnt, info->blksz);

	if (blkcnt > info->size) {
		pr_err("too large for partition: '%s'\n", part_name);
		fastboot_fail("too large for partition", response);
		return;
	}

	printf("Flashing Raw Image\n");

	blks = fb_block_write(dev_desc, info->start, blkcnt, buffer);

	if (blks != blkcnt) {
		pr_err("failed writing to device %d\n", dev_desc->devnum);
		fastboot_fail("failed writing to device", response);
		return;
	}

	printf("........ wrote " LBAFU " bytes to '%s'\n", blkcnt * info->blksz,
	       part_name);
	fastboot_okay(NULL, response);
}

void fastboot_block_write_sparse_image(struct blk_desc *dev_desc, struct disk_partition *info,
				       const char *part_name, void *buffer, char *response)
{
	struct fb_block_sparse sparse_priv;
	struct sparse_storage sparse;
	int err;

	sparse_priv.dev_desc = dev_desc;

	sparse.blksz = info->blksz;
	sparse.start = info->start;
	sparse.size = info->size;
	sparse.write = fb_block_sparse_write;
	sparse.reserve = fb_block_sparse_reserve;
	sparse.mssg = fastboot_fail;

	printf("Flashing sparse image at offset " LBAFU "\n",
	       sparse.start);

	sparse.priv = &sparse_priv;
	err = write_sparse_image(&sparse, part_name, buffer,
				 response);
	if (!err)
		fastboot_okay(NULL, response);
}

void fastboot_block_flash_write(const char *part_name, void *download_buffer,
				u32 download_bytes, char *response)
{
	struct blk_desc *dev_desc;
	struct disk_partition part_info;

	if (fastboot_block_get_part_info(part_name, &dev_desc, &part_info, response) < 0)
		return;

	if (is_sparse_image(download_buffer)) {
		fastboot_block_write_sparse_image(dev_desc, &part_info, part_name,
						  download_buffer, response);
	} else {
		if (fastboot_block_try_wrap_towed_boot(dev_desc, &part_info,
						       part_name,
						       download_buffer,
						       download_bytes,
						       response))
			return;

		fastboot_towed_boot_flash_probe(part_name, download_buffer,
						download_bytes);
		fastboot_block_write_raw_image(dev_desc, &part_info, part_name,
					       download_buffer, download_bytes, response);
	}
}

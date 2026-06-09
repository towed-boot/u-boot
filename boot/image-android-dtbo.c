// SPDX-License-Identifier: GPL-2.0+
/*
 * Towed-Boot Android DTBO overlay glue
 */

#include <env.h>
#include <dt_table.h>
#include <fdt_support.h>
#include <image-android-dt.h>
#include <linux/errno.h>
#include <linux/libfdt.h>
#include <linux/sizes.h>
#include <malloc.h>
#include <mapmem.h>
#include <stdio.h>

#define TOWED_ANDROID_DTBO_ADDR_ENV "towed_android_dtbo_addr"

static int towed_boot_android_set_simplefb_reg(void *fdt_blob, int node,
					       u64 addr, u64 size)
{
	fdt32_t reg[4];

	reg[0] = cpu_to_fdt32(addr >> 32);
	reg[1] = cpu_to_fdt32(addr);
	reg[2] = cpu_to_fdt32(size >> 32);
	reg[3] = cpu_to_fdt32(size);

	return fdt_setprop(fdt_blob, node, "reg", reg, sizeof(reg));
}

static int towed_boot_android_add_simplefb(void *fdt_blob)
{
	const char *format;
	char node_name[32];
	u64 addr, size;
	u32 width, height, stride;
	int chosen, node, root, ret;

	if (!IS_ENABLED(CONFIG_TOWED_DEBUG_SIMPLEFB))
		return 0;

	addr = CONFIG_IS_ENABLED(TOWED_DEBUG_SIMPLEFB,
				 (CONFIG_TOWED_DEBUG_SIMPLEFB_ADDR), (0));
	size = CONFIG_IS_ENABLED(TOWED_DEBUG_SIMPLEFB,
				 (CONFIG_TOWED_DEBUG_SIMPLEFB_SIZE), (0));
	width = CONFIG_IS_ENABLED(TOWED_DEBUG_SIMPLEFB,
				  (CONFIG_TOWED_DEBUG_SIMPLEFB_WIDTH), (0));
	height = CONFIG_IS_ENABLED(TOWED_DEBUG_SIMPLEFB,
				   (CONFIG_TOWED_DEBUG_SIMPLEFB_HEIGHT), (0));
	stride = CONFIG_IS_ENABLED(TOWED_DEBUG_SIMPLEFB,
				   (CONFIG_TOWED_DEBUG_SIMPLEFB_STRIDE), (0));
	format = CONFIG_IS_ENABLED(TOWED_DEBUG_SIMPLEFB,
				   (CONFIG_TOWED_DEBUG_SIMPLEFB_FORMAT), (""));

	if (!addr || !size || !width || !height || !stride || !*format)
		return 0;

	ret = fdt_increase_size(fdt_blob, SZ_4K);
	if (ret) {
		printf("Towed-Boot: could not grow DTB for simplefb (%d)\n",
		       ret);
		return 0;
	}

	root = fdt_path_offset(fdt_blob, "/");
	if (root < 0)
		return 0;

	chosen = fdt_path_offset(fdt_blob, "/chosen");
	if (chosen < 0)
		chosen = fdt_add_subnode(fdt_blob, root, "chosen");
	if (chosen < 0)
		return 0;

	fdt_setprop_u32(fdt_blob, chosen, "#address-cells", 2);
	fdt_setprop_u32(fdt_blob, chosen, "#size-cells", 2);
	fdt_setprop(fdt_blob, chosen, "ranges", NULL, 0);

	snprintf(node_name, sizeof(node_name), "framebuffer@%llx", addr);
	node = fdt_subnode_offset(fdt_blob, chosen, node_name);
	if (node < 0)
		node = fdt_add_subnode(fdt_blob, chosen, node_name);
	if (node < 0)
		return 0;

	ret = fdt_setprop_string(fdt_blob, node, "compatible",
				 "simple-framebuffer");
	if (!ret)
		ret = towed_boot_android_set_simplefb_reg(fdt_blob, node,
							  addr, size);
	if (!ret)
		ret = fdt_setprop_u32(fdt_blob, node, "width", width);
	if (!ret)
		ret = fdt_setprop_u32(fdt_blob, node, "height", height);
	if (!ret)
		ret = fdt_setprop_u32(fdt_blob, node, "stride", stride);
	if (!ret)
		ret = fdt_setprop_string(fdt_blob, node, "format", format);
	if (!ret)
		ret = fdt_setprop_string(fdt_blob, node, "status", "okay");

	if (ret)
		printf("Towed-Boot: could not add simplefb (%d)\n", ret);
	else
		printf("Towed-Boot: added simplefb at 0x%llx\n", addr);

	return 0;
}

static bool towed_boot_android_dtbo_count(ulong dtbo_addr, u32 *count)
{
	const struct dt_table_header *hdr;
	u32 total_size, entry_size, entry_count, entries_offset;
	u64 entries_end;

	hdr = map_sysmem(dtbo_addr, sizeof(*hdr));
	total_size = fdt32_to_cpu(hdr->total_size);
	entry_size = fdt32_to_cpu(hdr->dt_entry_size);
	entry_count = fdt32_to_cpu(hdr->dt_entry_count);
	entries_offset = fdt32_to_cpu(hdr->dt_entries_offset);
	unmap_sysmem(hdr);

	entries_end = (u64)entries_offset + (u64)entry_size * entry_count;
	if (entry_size < sizeof(struct dt_table_entry) ||
	    entries_offset < sizeof(struct dt_table_header) ||
	    entries_end > total_size)
		return false;

	*count = entry_count;

	return true;
}

static int towed_boot_android_try_dtbo(void *fdt_blob, ulong overlay_addr,
				       u32 overlay_size, u32 dtbo_index)
{
	void *overlay, *overlay_copy, *work_fdt;
	size_t work_size;
	int ret;

	overlay = map_sysmem(overlay_addr, overlay_size);
	if (fdt_check_header(overlay)) {
		printf("Towed-Boot: DTBO index %u is not an overlay FDT\n",
		       dtbo_index);
		unmap_sysmem(overlay);
		return -EINVAL;
	}

	ret = fdt_increase_size(fdt_blob, overlay_size + SZ_8K);
	if (ret) {
		printf("Towed-Boot: could not grow DTB for DTBO index %u (%d)\n",
		       dtbo_index, ret);
		unmap_sysmem(overlay);
		return ret;
	}

	work_size = fdt_totalsize(fdt_blob);
	work_fdt = malloc(work_size);
	overlay_copy = malloc(overlay_size);
	if (!work_fdt || !overlay_copy) {
		free(work_fdt);
		free(overlay_copy);
		unmap_sysmem(overlay);
		return -ENOMEM;
	}

	ret = fdt_open_into(fdt_blob, work_fdt, work_size);
	if (ret) {
		printf("Towed-Boot: could not copy DTB for DTBO index %u (%d)\n",
		       dtbo_index, ret);
		free(work_fdt);
		free(overlay_copy);
		unmap_sysmem(overlay);
		return ret;
	}

	memcpy(overlay_copy, overlay, overlay_size);
	unmap_sysmem(overlay);

	ret = fdt_overlay_apply_verbose(work_fdt, overlay_copy);
	if (ret) {
		printf("Towed-Boot: DTBO index %u apply failed (%d)\n",
		       dtbo_index, ret);
		free(work_fdt);
		free(overlay_copy);
		return ret;
	}

	memcpy(fdt_blob, work_fdt, fdt_totalsize(work_fdt));
	free(work_fdt);
	free(overlay_copy);
	printf("Towed-Boot: applied DTBO index %u\n", dtbo_index);

	return 0;
}

int towed_boot_android_apply_dtbo_overlay(void *fdt_blob)
{
	ulong dtbo_addr, overlay_addr;
	u32 count, dtbo_index, overlay_size, i;
	int ret;

	dtbo_addr = env_get_hex(TOWED_ANDROID_DTBO_ADDR_ENV, 0);
	if (!dtbo_addr)
		return 0;

	if (!android_dt_check_header(dtbo_addr) ||
	    !towed_boot_android_dtbo_count(dtbo_addr, &count)) {
		puts("Towed-Boot: loaded DTBO table is invalid, skipping\n");
		return 0;
	}

	dtbo_index = env_get_ulong("adtbo_idx", 10,
				   CONFIG_TOWED_BOOT_ANDROID_DTBO_INDEX);
	if (dtbo_index < count &&
	    android_dt_get_fdt_by_index(dtbo_addr, dtbo_index,
					&overlay_addr, &overlay_size)) {
		ret = towed_boot_android_try_dtbo(fdt_blob, overlay_addr,
						  overlay_size, dtbo_index);
		if (!ret)
			return 0;
	} else {
		printf("Towed-Boot: DTBO index %u not found\n", dtbo_index);
	}

	printf("Towed-Boot: scanning %u DTBO entries for a usable overlay\n",
	       count);
	for (i = 0; i < count; i++) {
		if (i == dtbo_index)
			continue;
		if (!android_dt_get_fdt_by_index(dtbo_addr, i, &overlay_addr,
						 &overlay_size))
			continue;

		ret = towed_boot_android_try_dtbo(fdt_blob, overlay_addr,
						  overlay_size, i);
		if (!ret)
			return 0;
	}

	puts("Towed-Boot: no usable DTBO overlay found\n");

	return 0;
}

int towed_boot_android_debug_fdt(void *fdt_blob)
{
	return towed_boot_android_add_simplefb(fdt_blob);
}

// SPDX-License-Identifier: GPL-2.0+
/*
 * Towed-Boot Android DTBO overlay glue
 */

#include <env.h>
#include <fdt_support.h>
#include <image-android-dt.h>
#include <linux/libfdt.h>
#include <linux/sizes.h>
#include <mapmem.h>
#include <stdio.h>

#define TOWED_ANDROID_DTBO_ADDR_ENV "towed_android_dtbo_addr"

int towed_boot_android_apply_dtbo_overlay(void *fdt_blob)
{
	ulong dtbo_addr, overlay_addr;
	u32 dtbo_index, overlay_size;
	void *overlay;
	int ret;

	dtbo_addr = env_get_hex(TOWED_ANDROID_DTBO_ADDR_ENV, 0);
	if (!dtbo_addr)
		return 0;

	if (!android_dt_check_header(dtbo_addr)) {
		puts("Towed-Boot: loaded DTBO table is invalid, skipping\n");
		return 0;
	}

	dtbo_index = env_get_ulong("adtbo_idx", 10,
				   CONFIG_TOWED_BOOT_ANDROID_DTBO_INDEX);
	if (!android_dt_get_fdt_by_index(dtbo_addr, dtbo_index,
					 &overlay_addr, &overlay_size)) {
		printf("Towed-Boot: DTBO index %u not found, skipping\n",
		       dtbo_index);
		return 0;
	}

	overlay = map_sysmem(overlay_addr, overlay_size);
	if (fdt_check_header(overlay)) {
		printf("Towed-Boot: DTBO index %u is not an overlay FDT\n",
		       dtbo_index);
		unmap_sysmem(overlay);
		return 0;
	}

	ret = fdt_increase_size(fdt_blob, overlay_size + SZ_8K);
	if (ret) {
		printf("Towed-Boot: could not grow DTB for DTBO index %u (%d)\n",
		       dtbo_index, ret);
		unmap_sysmem(overlay);
		return 0;
	}

	ret = fdt_overlay_apply_verbose(fdt_blob, overlay);
	if (ret) {
		printf("Towed-Boot: DTBO index %u apply failed (%d)\n",
		       dtbo_index, ret);
		unmap_sysmem(overlay);
		return 0;
	}

	printf("Towed-Boot: applied DTBO index %u\n", dtbo_index);
	unmap_sysmem(overlay);

	return 0;
}

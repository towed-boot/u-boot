// SPDX-License-Identifier: GPL-2.0+
/*
 * (C) Copyright 2008 - 2009
 * Windriver, <www.windriver.com>
 * Tom Rix <Tom.Rix@windriver.com>
 *
 * Copyright 2011 Sebastian Andrzej Siewior <bigeasy@linutronix.de>
 *
 * Copyright 2014 Linaro, Ltd.
 * Rob Herring <robh@kernel.org>
 */

#include <bcb.h>
#include <command.h>
#include <env.h>
#include <fastboot.h>
#include <image.h>
#include <image-sparse.h>
#include <net.h>
#include <vsprintf.h>
#include <android_image.h>
#include <dt_table.h>
#include <linux/libfdt.h>

/**
 * fastboot_buf_addr - base address of the fastboot download buffer
 */
void *fastboot_buf_addr;

/**
 * fastboot_buf_size - size of the fastboot download buffer
 */
u32 fastboot_buf_size;

/**
 * fastboot_progress_callback - callback executed during long operations
 */
void (*fastboot_progress_callback)(const char *msg);

/**
 * fastboot_response() - Writes a response of the form "$tag$reason".
 *
 * @tag: The first part of the response
 * @response: Pointer to fastboot response buffer
 * @format: printf style format string
 */
void fastboot_response(const char *tag, char *response,
		       const char *format, ...)
{
	va_list args;

	strlcpy(response, tag, FASTBOOT_RESPONSE_LEN);
	if (format) {
		va_start(args, format);
		vsnprintf(response + strlen(response),
			  FASTBOOT_RESPONSE_LEN - strlen(response) - 1,
			  format, args);
		va_end(args);
	}
}

/**
 * fastboot_fail() - Write a FAIL response of the form "FAIL$reason".
 *
 * @reason: Pointer to returned reason string
 * @response: Pointer to fastboot response buffer
 */
void fastboot_fail(const char *reason, char *response)
{
	fastboot_response("FAIL", response, "%s", reason);
}

/**
 * fastboot_okay() - Write an OKAY response of the form "OKAY$reason".
 *
 * @reason: Pointer to returned reason string, or NULL to send a bare "OKAY"
 * @response: Pointer to fastboot response buffer
 */
void fastboot_okay(const char *reason, char *response)
{
	if (reason)
		fastboot_response("OKAY", response, "%s", reason);
	else
		fastboot_response("OKAY", response, NULL);
}

static bool is_linux_arm64_image(const void *buffer, u32 download_bytes)
{
	static const u8 arm64_magic[] = { 0x41, 0x52, 0x4d, 0x64 };

	if (download_bytes < 0x40)
		return false;

	return !memcmp((const u8 *)buffer + 0x38, arm64_magic,
		       sizeof(arm64_magic));
}

static bool is_android_dt_image(const void *buffer, u32 download_bytes)
{
	const struct dt_table_header *hdr = buffer;

	if (download_bytes < sizeof(*hdr))
		return false;

	return fdt32_to_cpu(hdr->magic) == DT_TABLE_MAGIC;
}

static bool is_android_vbmeta_image(const void *buffer, u32 download_bytes)
{
	if (download_bytes < 4)
		return false;

	return !memcmp(buffer, "AVB0", 4);
}

static bool fastboot_android_boot_has_u_boot(const void *buffer,
					     u32 download_bytes)
{
	const struct andr_boot_img_hdr_v0 *hdr = buffer;
	u32 kernel_offset;

	if (hdr->header_version <= 2)
		kernel_offset = hdr->page_size;
	else
		kernel_offset = ANDR_GKI_PAGE_SIZE;

	if (kernel_offset > download_bytes ||
	    download_bytes - kernel_offset < 0x60)
		return false;

	return is_arm64_u_boot_image((const u8 *)buffer + kernel_offset,
				     download_bytes - kernel_offset);
}

static void fastboot_print_android_boot(const char *part_name,
					const void *buffer,
					u32 download_bytes)
{
	const struct andr_boot_img_hdr_v0 *hdr_v0 = buffer;
	const struct andr_boot_img_hdr_v3 *hdr_v3 = buffer;
	u32 header_version = hdr_v0->header_version;
	u32 boot_img_size;

	if (fastboot_android_boot_has_u_boot(buffer, download_bytes)) {
		printf("Towed-Boot: %s contains a U-Boot payload; Android autoboot will ignore it\n",
		       part_name);
		return;
	}

	printf("Towed-Boot: %s is Android boot image v%u\n",
	       part_name, header_version);

	if (header_version <= 2) {
		printf("Towed-Boot: kernel %u KiB, ramdisk %u KiB, dtb %u KiB\n",
		       DIV_ROUND_UP(hdr_v0->kernel_size, 1024),
		       DIV_ROUND_UP(hdr_v0->ramdisk_size, 1024),
		       DIV_ROUND_UP(hdr_v0->dtb_size, 1024));
	} else {
		printf("Towed-Boot: kernel %u KiB, ramdisk %u KiB\n",
		       DIV_ROUND_UP(hdr_v3->kernel_size, 1024),
		       DIV_ROUND_UP(hdr_v3->ramdisk_size, 1024));
		puts("Towed-Boot: vendor_boot is needed for Android boot v3+\n");
		if (!hdr_v3->kernel_size && hdr_v3->ramdisk_size)
			puts("Towed-Boot: looks like init_boot style ramdisk data\n");
	}

	if (android_image_get_bootimg_size(buffer, &boot_img_size))
		printf("Towed-Boot: parsed boot image size %u KiB\n",
		       DIV_ROUND_UP(boot_img_size, 1024));
}

static void fastboot_print_android_vendor_boot(const char *part_name,
					       const void *buffer)
{
	const struct andr_vnd_boot_img_hdr *hdr = buffer;
	u32 vendor_boot_img_size;

	printf("Towed-Boot: %s is Android vendor_boot image v%u\n",
	       part_name, hdr->header_version);
	printf("Towed-Boot: vendor ramdisk %u KiB, dtb %u KiB, bootconfig %u KiB\n",
	       DIV_ROUND_UP(hdr->vendor_ramdisk_size, 1024),
	       DIV_ROUND_UP(hdr->dtb_size, 1024),
	       DIV_ROUND_UP(hdr->bootconfig_size, 1024));

	if (android_image_get_vendor_bootimg_size(buffer, &vendor_boot_img_size))
		printf("Towed-Boot: parsed vendor_boot image size %u KiB\n",
		       DIV_ROUND_UP(vendor_boot_img_size, 1024));
}

void fastboot_towed_boot_flash_probe(const char *part_name, const void *buffer,
				     u32 download_bytes)
{
	if (!IS_ENABLED(CONFIG_TOWED_BOOT_ANDROID))
		return;

	if (!part_name || !buffer || !download_bytes)
		return;

	if (is_sparse_image((void *)buffer)) {
		printf("Towed-Boot: %s is an Android sparse image\n", part_name);
		return;
	}

	if (is_android_boot_image_header(buffer)) {
		fastboot_print_android_boot(part_name, buffer, download_bytes);
		return;
	}

	if (is_android_vendor_boot_image_header(buffer)) {
		fastboot_print_android_vendor_boot(part_name, buffer);
		return;
	}

	if (is_android_dt_image(buffer, download_bytes)) {
		printf("Towed-Boot: %s is Android DTB/DTBO table data\n",
		       part_name);
		return;
	}

	if (is_android_vbmeta_image(buffer, download_bytes)) {
		printf("Towed-Boot: %s is Android vbmeta data\n", part_name);
		return;
	}

	switch (genimg_get_format(buffer)) {
	case IMAGE_FORMAT_FIT:
		printf("Towed-Boot: %s is a FIT image\n", part_name);
		return;
	case IMAGE_FORMAT_LEGACY:
		printf("Towed-Boot: %s is a legacy uImage\n", part_name);
		return;
	default:
		break;
	}

	if (is_linux_arm64_image(buffer, download_bytes))
		printf("Towed-Boot: %s is a Linux ARM64 Image\n", part_name);
}

/**
 * fastboot_set_reboot_flag() - Set flag to indicate reboot-bootloader
 *
 * Set flag which indicates that we should reboot into the bootloader
 * following the reboot that fastboot executes after this function.
 *
 * This function should be overridden in your board file with one
 * which sets whatever flag your board specific Android bootloader flow
 * requires in order to re-enter the bootloader.
 */
int __weak fastboot_set_reboot_flag(enum fastboot_reboot_reason reason)
{
	int ret;
	static const char * const boot_cmds[] = {
		[FASTBOOT_REBOOT_REASON_BOOTLOADER] = "bootonce-bootloader",
		[FASTBOOT_REBOOT_REASON_FASTBOOTD] = "boot-fastboot",
		[FASTBOOT_REBOOT_REASON_RECOVERY] = "boot-recovery"
	};

	int device = config_opt_enabled(CONFIG_FASTBOOT_FLASH_BLOCK,
					CONFIG_FASTBOOT_FLASH_BLOCK_DEVICE_ID, -1);
	if (device == -1) {
		device = config_opt_enabled(CONFIG_FASTBOOT_FLASH_MMC,
					    CONFIG_FASTBOOT_FLASH_MMC_DEV, -1);
	}
	const char *bcb_iface = config_opt_enabled(CONFIG_FASTBOOT_FLASH_BLOCK,
						   CONFIG_FASTBOOT_FLASH_BLOCK_INTERFACE_NAME,
						   "mmc");

	if (device == -1)
		return -EINVAL;

	if (reason >= FASTBOOT_REBOOT_REASONS_COUNT)
		return -EINVAL;

	ret = bcb_find_partition_and_load(bcb_iface, device, "misc");
	if (ret)
		goto out;

	ret = bcb_set(BCB_FIELD_COMMAND, boot_cmds[reason]);
	if (ret)
		goto out;

	ret = bcb_store();
out:
	bcb_reset();
	return ret;
}

/**
 * fastboot_get_progress_callback() - Return progress callback
 *
 * Return: Pointer to function called during long operations
 */
void (*fastboot_get_progress_callback(void))(const char *)
{
	return fastboot_progress_callback;
}

/**
 * fastboot_boot() - Execute fastboot boot command
 *
 * If ${fastboot_bootcmd} is set, run that command to execute the boot
 * process, if that returns, then exit the fastboot server and return
 * control to the caller.
 *
 * Otherwise execute "bootm <fastboot_buf_addr>", if that fails, reset
 * the board.
 */
void fastboot_boot(void)
{
	char *s;

	s = env_get("fastboot_bootcmd");
	if (s) {
		run_command(s, CMD_FLAG_ENV);
	} else if (IS_ENABLED(CONFIG_CMD_BOOTM)) {
		static char boot_addr_start[20];
		static char *const bootm_args[] = {
			"bootm", boot_addr_start, NULL
		};

		snprintf(boot_addr_start, sizeof(boot_addr_start) - 1,
			 "0x%p", fastboot_buf_addr);
		printf("Booting kernel at %s...\n\n\n", boot_addr_start);

		do_bootm(NULL, 0, 2, bootm_args);

		/*
		 * This only happens if image is somehow faulty so we start
		 * over. We deliberately leave this policy to the invocation
		 * of fastbootcmd if that's what's being run
		 */
		do_reset(NULL, 0, 0, NULL);
	}
}

/**
 * fastboot_handle_boot() - Shared implementation of system reaction to
 * fastboot commands
 *
 * Making desceisions about device boot state (stay in fastboot, reboot
 * to bootloader, reboot to OS, etc).
 */
void fastboot_handle_boot(int command, bool success)
{
	if (!success)
		return;

	switch (command) {
	case FASTBOOT_COMMAND_BOOT:
		fastboot_boot();
#if CONFIG_IS_ENABLED(NET_LEGACY)
		net_set_state(NETLOOP_SUCCESS);
#endif
		break;

	case FASTBOOT_COMMAND_CONTINUE:
#if CONFIG_IS_ENABLED(NET_LEGACY)
		net_set_state(NETLOOP_SUCCESS);
#endif
		break;

	case FASTBOOT_COMMAND_REBOOT:
	case FASTBOOT_COMMAND_REBOOT_BOOTLOADER:
	case FASTBOOT_COMMAND_REBOOT_FASTBOOTD:
	case FASTBOOT_COMMAND_REBOOT_RECOVERY:
		do_reset(NULL, 0, 0, NULL);
		break;
	}
}

/**
 * fastboot_set_progress_callback() - set progress callback
 *
 * @progress: Pointer to progress callback
 *
 * Set a callback which is invoked periodically during long running operations
 * (flash and erase). This can be used (for example) by the UDP transport to
 * send INFO responses to keep the client alive whilst those commands are
 * executing.
 */
void fastboot_set_progress_callback(void (*progress)(const char *msg))
{
	fastboot_progress_callback = progress;
}

/*
 * fastboot_init() - initialise new fastboot protocol session
 *
 * @buf_addr: Pointer to download buffer, or NULL for default
 * @buf_size: Size of download buffer, or zero for default
 */
void fastboot_init(void *buf_addr, u32 buf_size)
{
#if IS_ENABLED(CONFIG_FASTBOOT_FLASH_BLOCK)
	if (!strcmp(CONFIG_FASTBOOT_FLASH_BLOCK_INTERFACE_NAME, "mmc"))
		printf("Warning: the fastboot block backend features are limited, consider using the MMC backend\n");
#endif

	fastboot_buf_addr = buf_addr ? buf_addr :
				       (void *)CONFIG_FASTBOOT_BUF_ADDR;
	fastboot_buf_size = buf_size ? buf_size : CONFIG_FASTBOOT_BUF_SIZE;
	fastboot_set_progress_callback(NULL);

}

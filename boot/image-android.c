// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (c) 2011 Sebastian Andrzej Siewior <bigeasy@linutronix.de>
 */

#include <bootflow.h>
#include <env.h>
#include <image.h>
#include <image-android-dt.h>
#include <android_image.h>
#include <malloc.h>
#include <errno.h>
#include <asm/unaligned.h>
#include <mapmem.h>
#include <linux/libfdt.h>

#define ANDROID_IMAGE_DEFAULT_KERNEL_ADDR	0x10008000
#define ANDROID_IMAGE_DEFAULT_RAMDISK_ADDR	0x11000000
#define TOWED_BOOT_ANDROID_PAYLOAD_MAGIC	"TOWEDBOOTANDRV1"
#define TOWED_BOOT_ANDROID_PAYLOAD_VERSION	1
#define TOWED_BOOT_ANDROID_PAYLOAD_MAGIC_SIZE	16
#define TOWED_BOOT_ANDROID_PAYLOAD_VERSION_OFF	16
#define TOWED_BOOT_ANDROID_PAYLOAD_HDR_SIZE_OFF	20
#define TOWED_BOOT_ANDROID_PAYLOAD_OFFSET_OFF	24
#define TOWED_BOOT_ANDROID_PAYLOAD_SIZE_OFF	28
static char andr_tmp_str[ANDR_BOOT_ARGS_SIZE + 1];

static int towed_debug_set_bootarg(char **cmdline, const char *arg,
				   const char *value)
{
	char *buf, *new_cmdline;
	size_t len;
	int ret;

	len = strlen(*cmdline ?: "") + strlen(arg) + 2;
	if (value && value != BOOTFLOWCL_EMPTY)
		len += strlen(value) + 2;

	buf = malloc(len);
	if (!buf)
		return -ENOMEM;

	ret = cmdline_set_arg(buf, len, *cmdline, arg, value, NULL);
	if (ret < 0) {
		if (!value && ret == -ENOENT) {
			free(buf);
			return 0;
		}

		free(buf);
		return ret;
	}

	new_cmdline = strdup(buf);
	free(buf);
	if (!new_cmdline)
		return -ENOMEM;

	free(*cmdline);
	*cmdline = new_cmdline;

	return 0;
}

static int towed_debug_append_raw_bootarg(char **cmdline, const char *arg)
{
	const char *old_cmdline = *cmdline ?: "";
	char *new_cmdline;
	size_t old_len, arg_len;

	if (!arg || !*arg)
		return 0;

	old_len = strlen(old_cmdline);
	arg_len = strlen(arg);

	new_cmdline = malloc(old_len + !!old_len + arg_len + 1);
	if (!new_cmdline)
		return -ENOMEM;

	if (old_len) {
		strcpy(new_cmdline, old_cmdline);
		strcat(new_cmdline, " ");
		strcat(new_cmdline, arg);
	} else {
		strcpy(new_cmdline, arg);
	}

	free(*cmdline);
	*cmdline = new_cmdline;

	return 0;
}

static int towed_debug_apply_bootarg(char **cmdline, char *arg)
{
	char *value;

	if (!arg || !*arg)
		return 0;

	value = strchr(arg, '=');
	if (value) {
		*value++ = '\0';
		return towed_debug_set_bootarg(cmdline, arg, value);
	}

	return towed_debug_set_bootarg(cmdline, arg, BOOTFLOWCL_EMPTY);
}

static int towed_debug_apply_consoles(char **cmdline, const char *consoles)
{
	char *args, *console, *orig;
	int ret;

	if (!consoles || !*consoles)
		return 0;

	ret = towed_debug_set_bootarg(cmdline, "console", NULL);
	if (ret < 0)
		return ret;

	args = strdup(consoles);
	if (!args)
		return -ENOMEM;
	orig = args;

	console = strsep(&args, " ");
	while (console) {
		if (*console) {
			if (!strncmp(console, "console=", 8)) {
				ret = towed_debug_append_raw_bootarg(cmdline,
								     console);
			} else {
				char *console_arg;

				console_arg = malloc(strlen(console) + 9);
				if (!console_arg) {
					free(orig);
					return -ENOMEM;
				}

				sprintf(console_arg, "console=%s", console);
				ret = towed_debug_append_raw_bootarg(cmdline,
								     console_arg);
				free(console_arg);
			}
			if (ret < 0) {
				free(orig);
				return ret;
			}
		}

		console = strsep(&args, " ");
	}

	free(orig);

	return 0;
}

static int towed_debug_apply_bootargs(char **cmdline, const char *extra_args)
{
	char *args, *arg, *orig;
	int ret;

	if (!extra_args || !*extra_args)
		return 0;

	args = strdup(extra_args);
	if (!args)
		return -ENOMEM;
	orig = args;

	arg = strsep(&args, " ");
	while (arg) {
		ret = towed_debug_apply_bootarg(cmdline, arg);
		if (ret < 0) {
			free(orig);
			return ret;
		}

		arg = strsep(&args, " ");
	}

	free(orig);

	return 0;
}

static int towed_debug_update_android_bootargs(char **cmdline)
{
	const char *consoles, *extra_args;
	int ret;

	if (!IS_ENABLED(CONFIG_TOWED_DEBUG))
		return 0;

	consoles = env_get("towed_debug_consoles");
	if (!consoles || !*consoles)
		consoles = env_get("towed_debug_console");
	if (!consoles || !*consoles)
		consoles = CONFIG_IS_ENABLED(TOWED_DEBUG,
					     (CONFIG_TOWED_DEBUG_ANDROID_CONSOLES),
					     (""));
	if (!consoles || !*consoles)
		consoles = CONFIG_IS_ENABLED(TOWED_DEBUG,
					     (CONFIG_TOWED_DEBUG_ANDROID_CONSOLE),
					     (""));

	ret = towed_debug_apply_consoles(cmdline, consoles);
	if (ret < 0)
		return ret;

	ret = towed_debug_set_bootarg(cmdline, "quiet", NULL);
	if (ret < 0)
		return ret;

	extra_args = env_get("towed_debug_bootargs");
	if (!extra_args || !*extra_args)
		extra_args = CONFIG_IS_ENABLED(TOWED_DEBUG,
					       (CONFIG_TOWED_DEBUG_ANDROID_BOOTARGS),
					       (""));

	ret = towed_debug_apply_bootargs(cmdline, extra_args);
	if (ret < 0)
		return ret;

	puts("Towed-Boot: Android kernel debug logging enabled\n");

	return 0;
}

static bool android_boot_image_v0_v1_v2_page_size_valid(const struct andr_boot_img_hdr_v0 *hdr)
{
	return hdr->page_size >= sizeof(*hdr) &&
	       !(hdr->page_size & (hdr->page_size - 1));
}

static ulong checksum(const unsigned char *buffer, ulong size)
{
	ulong sum = 0;

	for (ulong i = 0; i < size; i++)
		sum += buffer[i];
	return sum;
}

static bool is_trailer_present(ulong bootconfig_end_addr)
{
	return !strncmp((char *)(bootconfig_end_addr - BOOTCONFIG_MAGIC_SIZE),
			BOOTCONFIG_MAGIC, BOOTCONFIG_MAGIC_SIZE);
}

static ulong add_trailer(ulong bootconfig_start_addr, ulong bootconfig_size)
{
	ulong end;
	ulong sum;

	if (!bootconfig_start_addr)
		return -1;
	if (!bootconfig_size)
		return 0;

	end = bootconfig_start_addr + bootconfig_size;
	if (is_trailer_present(end))
		return 0;

	memcpy((void *)(end), &bootconfig_size, BOOTCONFIG_SIZE_SIZE);
	sum = checksum((unsigned char *)bootconfig_start_addr, bootconfig_size);
	memcpy((void *)(end + BOOTCONFIG_SIZE_SIZE), &sum,
	       BOOTCONFIG_CHECKSUM_SIZE);
	memcpy((void *)(end + BOOTCONFIG_SIZE_SIZE + BOOTCONFIG_CHECKSUM_SIZE),
	       BOOTCONFIG_MAGIC, BOOTCONFIG_MAGIC_SIZE);

	return BOOTCONFIG_TRAILER_SIZE;
}

/*
 * Add a string of boot config parameters to memory appended by the trailer.
 * NOTE: This function expects bootconfig_start_addr to be already mapped.
 *       It works directly with the mapped pointer, not a physical address.
 */
static long add_bootconfig_parameters(char *params, long params_len,
				      ulong bootconfig_start_addr, u32 bootconfig_size)
{
	long applied_bytes = 0;
	long new_size = 0;
	ulong end;

	if (!params || !bootconfig_start_addr)
		return -EINVAL;

	if (params_len == 0)
		return 0;

	end = bootconfig_start_addr + bootconfig_size;

	if (is_trailer_present(end)) {
		end -= BOOTCONFIG_TRAILER_SIZE;
		applied_bytes -= BOOTCONFIG_TRAILER_SIZE;
		memcpy(&new_size, (void *)end, BOOTCONFIG_SIZE_SIZE);
	} else {
		/*
		 * When no trailer is present, the bootconfig_size includes the actual content.
		 * We should write new parameters right after the existing content.
		 */
		end = bootconfig_start_addr + bootconfig_size;
		new_size = bootconfig_size;
	}

	memcpy((void *)end, params, params_len);
	applied_bytes += params_len;
	applied_bytes += add_trailer(bootconfig_start_addr,
				     bootconfig_size + applied_bytes);
	return applied_bytes;
}

__weak ulong get_avendor_bootimg_addr(void)
{
	return -1;
}

static void android_boot_image_v3_v4_parse_hdr(const struct andr_boot_img_hdr_v3 *hdr,
					       struct andr_image_data *data)
{
	ulong end;

	data->kcmdline = hdr->cmdline;
	data->header_version = hdr->header_version;

	/*
	 * The header takes a full page, the remaining components are aligned
	 * on page boundary.
	 */
	end = map_to_sysmem(hdr);
	end += ANDR_GKI_PAGE_SIZE;
	data->kernel_ptr = end;
	data->kernel_size = hdr->kernel_size;
	end += ALIGN(hdr->kernel_size, ANDR_GKI_PAGE_SIZE);
	data->ramdisk_ptr = end;
	data->ramdisk_size = hdr->ramdisk_size;
	data->boot_ramdisk_size = hdr->ramdisk_size;
	end += ALIGN(hdr->ramdisk_size, ANDR_GKI_PAGE_SIZE);

	if (hdr->header_version > 3)
		end += ALIGN(hdr->signature_size, ANDR_GKI_PAGE_SIZE);

	data->boot_img_total_size = end - map_to_sysmem(hdr);
}

static void android_vendor_boot_image_v3_v4_parse_hdr(const struct andr_vnd_boot_img_hdr
						      *hdr, struct andr_image_data *data)
{
	ulong end;

	/*
	 * The header takes a full page, the remaining components are aligned
	 * on page boundary.
	 */
	data->kcmdline_extra = hdr->cmdline;
	data->tags_addr = hdr->tags_addr;
	data->image_name = hdr->name;
	data->kernel_addr = hdr->kernel_addr;
	data->ramdisk_addr = hdr->ramdisk_addr;
	data->dtb_load_addr = hdr->dtb_addr;
	data->bootconfig_size = hdr->bootconfig_size;
	end = map_to_sysmem(hdr);

	if (hdr->header_version > 3)
		end += ALIGN(ANDR_VENDOR_BOOT_V4_SIZE, hdr->page_size);
	else
		end += ALIGN(ANDR_VENDOR_BOOT_V3_SIZE, hdr->page_size);

	if (hdr->vendor_ramdisk_size) {
		data->vendor_ramdisk_ptr = end;
		data->vendor_ramdisk_size = hdr->vendor_ramdisk_size;
		data->ramdisk_size += hdr->vendor_ramdisk_size;
		end += ALIGN(hdr->vendor_ramdisk_size, hdr->page_size);
	}

	data->dtb_ptr = end;
	data->dtb_size = hdr->dtb_size;

	end += ALIGN(hdr->dtb_size, hdr->page_size);
	end += ALIGN(hdr->vendor_ramdisk_table_size, hdr->page_size);
	data->bootconfig_addr = end;
	if (hdr->bootconfig_size) {
		void *bootconfig_ptr = map_sysmem(data->bootconfig_addr,
						  data->bootconfig_size +
						  BOOTCONFIG_TRAILER_SIZE);
		data->bootconfig_size += add_trailer((ulong)bootconfig_ptr,
						     data->bootconfig_size);
		unmap_sysmem(bootconfig_ptr);
		data->ramdisk_size += data->bootconfig_size;
	}
	end += ALIGN(data->bootconfig_size, hdr->page_size);
	data->vendor_boot_img_total_size = end - map_to_sysmem(hdr);
}

static void android_boot_image_v0_v1_v2_parse_hdr(const struct andr_boot_img_hdr_v0 *hdr,
						  struct andr_image_data *data)
{
	ulong end;

	data->image_name = hdr->name;
	data->kcmdline = hdr->cmdline;
	data->kernel_addr = hdr->kernel_addr;
	data->ramdisk_addr = hdr->ramdisk_addr;
	data->header_version = hdr->header_version;
	data->dtb_load_addr = hdr->dtb_addr;

	end = map_to_sysmem(hdr);

	/*
	 * The header takes a full page, the remaining components are aligned
	 * on page boundary
	 */

	end += hdr->page_size;

	data->kernel_ptr = end;
	data->kernel_size = hdr->kernel_size;
	end += ALIGN(hdr->kernel_size, hdr->page_size);

	data->ramdisk_ptr = end;
	data->ramdisk_size = hdr->ramdisk_size;
	end += ALIGN(hdr->ramdisk_size, hdr->page_size);

	data->second_ptr = end;
	data->second_size = hdr->second_size;
	end += ALIGN(hdr->second_size, hdr->page_size);

	if (hdr->header_version >= 1) {
		data->recovery_dtbo_ptr = end;
		data->recovery_dtbo_size = hdr->recovery_dtbo_size;
		end += ALIGN(hdr->recovery_dtbo_size, hdr->page_size);
	}

	if (hdr->header_version >= 2) {
		data->dtb_ptr = end;
		data->dtb_size = hdr->dtb_size;
		end += ALIGN(hdr->dtb_size, hdr->page_size);
	}

	data->boot_img_total_size = end - map_to_sysmem(hdr);
}

bool android_image_get_bootimg_size(const void *hdr, u32 *boot_img_size)
{
	struct andr_image_data data;

	if (!hdr || !boot_img_size) {
		printf("hdr or boot_img_size can't be NULL\n");
		return false;
	}

	if (!is_android_boot_image_header(hdr)) {
		printf("Incorrect boot image header\n");
		return false;
	}

	if (((struct andr_boot_img_hdr_v0 *)hdr)->header_version <= 2) {
		if (!android_boot_image_v0_v1_v2_page_size_valid(hdr))
			return false;

		android_boot_image_v0_v1_v2_parse_hdr(hdr, &data);
	} else {
		android_boot_image_v3_v4_parse_hdr(hdr, &data);
	}

	*boot_img_size = data.boot_img_total_size;

	return true;
}

bool android_image_get_vendor_bootimg_size(const void *hdr, u32 *vendor_boot_img_size)
{
	struct andr_image_data data;

	if (!hdr || !vendor_boot_img_size) {
		printf("hdr or vendor_boot_img_size can't be NULL\n");
		return false;
	}

	if (!is_android_vendor_boot_image_header(hdr)) {
		printf("Incorrect vendor boot image header\n");
		return false;
	}

	android_vendor_boot_image_v3_v4_parse_hdr(hdr, &data);

	*vendor_boot_img_size = data.vendor_boot_img_total_size;

	return true;
}

bool android_image_get_data(const void *boot_hdr, const void *vendor_boot_hdr,
			    struct andr_image_data *data)
{
	const struct andr_boot_img_hdr_v0 *bhdr;
	const struct andr_vnd_boot_img_hdr *vhdr;

	if (!boot_hdr || !data) {
		printf("boot_hdr or data params can't be NULL\n");
		return false;
	}

	bhdr = map_sysmem((ulong)boot_hdr, sizeof(*bhdr));
	if (!is_android_boot_image_header(bhdr)) {
		printf("Incorrect boot image header\n");
		unmap_sysmem(bhdr);
		return false;
	}

	if (bhdr->header_version > 2) {
		if (!vendor_boot_hdr) {
			printf("For boot header v3+ vendor boot image has to be provided\n");
			unmap_sysmem(bhdr);
			return false;
		}
		vhdr = map_sysmem((ulong)vendor_boot_hdr, sizeof(*vhdr));
		if (!is_android_vendor_boot_image_header(vhdr)) {
			printf("Incorrect vendor boot image header\n");
			unmap_sysmem(vhdr);
			unmap_sysmem(bhdr);
			return false;
		}
		android_boot_image_v3_v4_parse_hdr((const struct andr_boot_img_hdr_v3 *)bhdr, data);
		android_vendor_boot_image_v3_v4_parse_hdr(vhdr, data);
		unmap_sysmem(vhdr);
	} else {
		android_boot_image_v0_v1_v2_parse_hdr(bhdr, data);
	}

	unmap_sysmem(bhdr);
	return true;
}

static ulong android_image_get_kernel_addr(struct andr_image_data *img_data,
					   ulong comp)
{
	ulong staged_addr;

	staged_addr = env_get_hex("towed_android_kernel_addr", 0);
	if (staged_addr)
		return staged_addr;

	/*
	 * All the Android tools that generate a boot.img use this
	 * address as the default.
	 *
	 * Even though it doesn't really make a lot of sense, and it
	 * might be valid on some platforms, we treat that adress as
	 * the default value for this field, and try to execute the
	 * kernel in place in such a case.
	 *
	 * Otherwise, we will return the actual value set by the user.
	 */
	if (img_data->kernel_addr  == ANDROID_IMAGE_DEFAULT_KERNEL_ADDR ||
	    IS_ENABLED(CONFIG_ANDROID_BOOT_IMAGE_IGNORE_BLOB_ADDR)) {
		if (comp == IH_COMP_NONE)
			return img_data->kernel_ptr;
		return env_get_ulong("kernel_addr_r", 16, 0);
	}

	/*
	 * abootimg creates images where all load addresses are 0
	 * and we need to fix them.
	 */
	if (img_data->kernel_addr == 0 && img_data->ramdisk_addr == 0)
		return env_get_ulong("kernel_addr_r", 16, 0);

	return img_data->kernel_addr;
}

/**
 * android_image_get_kernel() - processes kernel part of Android boot images
 * @hdr:	Pointer to boot image header, which is at the start
 *			of the image.
 * @vendor_boot_img:	Pointer to vendor boot image header, which is at the
 *				start of the image.
 * @verify:	Checksum verification flag. Currently unimplemented.
 * @os_data:	Pointer to a ulong variable, will hold os data start
 *			address.
 * @os_len:	Pointer to a ulong variable, will hold os data length.
 *
 * This function returns the os image's start address and length. Also,
 * it appends the kernel command line to the bootargs env variable.
 *
 * Return: Zero, os start address and length on success,
 *		otherwise on failure.
 */
int android_image_get_kernel(const void *hdr,
			     const void *vendor_boot_img, int verify,
			     ulong *os_data, ulong *os_len)
{
	struct andr_image_data img_data = {0};
	ulong kernel_addr;
	const struct legacy_img_hdr *ihdr;
	ulong comp;
	int ret;

	if (!android_image_get_data(hdr, vendor_boot_img, &img_data))
		return -EINVAL;

	comp = android_image_get_kcomp(hdr, vendor_boot_img);

	kernel_addr = android_image_get_kernel_addr(&img_data, comp);
	ihdr = (const struct legacy_img_hdr *)img_data.kernel_ptr;

	/*
	 * Not all Android tools use the id field for signing the image with
	 * sha1 (or anything) so we don't check it. It is not obvious that the
	 * string is null terminated so we take care of this.
	 */
	strlcpy(andr_tmp_str, img_data.image_name, ANDR_BOOT_NAME_SIZE);
	andr_tmp_str[ANDR_BOOT_NAME_SIZE] = '\0';
	if (strlen(andr_tmp_str))
		printf("Android's image name: %s\n", andr_tmp_str);

	printf("Kernel load addr 0x%08lx size %u KiB\n",
	       kernel_addr, DIV_ROUND_UP(img_data.kernel_size, 1024));

	int len = 0;
	char *bootargs = env_get("bootargs");

	if (bootargs)
		len += strlen(bootargs);

	if (img_data.kcmdline && *img_data.kcmdline) {
		printf("Kernel command line: %s\n", img_data.kcmdline);
		len += strlen(img_data.kcmdline) + (len ? 1 : 0); /* +1 for extra space */
	}

	if (img_data.kcmdline_extra && *img_data.kcmdline_extra) {
		printf("Kernel extra command line: %s\n", img_data.kcmdline_extra);
		len += strlen(img_data.kcmdline_extra) + (len ? 1 : 0); /* +1 for extra space */
	}

	char *newbootargs = malloc(len + 1); /* +1 for the '\0' */
	if (!newbootargs) {
		puts("Error: malloc in android_image_get_kernel failed!\n");
		return -ENOMEM;
	}
	*newbootargs = '\0'; /* set to Null in case no components below are present */

	if (bootargs)
		strcpy(newbootargs, bootargs);

	if (img_data.kcmdline && *img_data.kcmdline) {
		if (*newbootargs) /* If there is something in newbootargs, a space is needed */
			strcat(newbootargs, " ");
		strcat(newbootargs, img_data.kcmdline);
	}

	if (img_data.kcmdline_extra && *img_data.kcmdline_extra) {
		if (*newbootargs) /* If there is something in newbootargs, a space is needed */
			strcat(newbootargs, " ");
		strcat(newbootargs, img_data.kcmdline_extra);
	}

	ret = towed_debug_update_android_bootargs(&newbootargs);
	if (ret < 0) {
		free(newbootargs);
		return ret;
	}

	if (IS_ENABLED(CONFIG_TOWED_DEBUG))
		printf("Towed-Boot: final Android bootargs: %s\n", newbootargs);

	env_set("bootargs", newbootargs);
	free(newbootargs);

	if (os_data) {
		if (image_get_magic(ihdr) == IH_MAGIC) {
			*os_data = image_get_data(ihdr);
		} else {
			*os_data = img_data.kernel_ptr;
		}
	}
	if (os_len) {
		if (image_get_magic(ihdr) == IH_MAGIC)
			*os_len = image_get_data_size(ihdr);
		else
			*os_len = img_data.kernel_size;
	}
	return 0;
}

bool is_android_vendor_boot_image_header(const void *vendor_boot_img)
{
	return !memcmp(VENDOR_BOOT_MAGIC, vendor_boot_img, ANDR_VENDOR_BOOT_MAGIC_SIZE);
}

bool is_android_boot_image_header(const void *hdr)
{
	return !memcmp(ANDR_BOOT_MAGIC, hdr, ANDR_BOOT_MAGIC_SIZE);
}

bool is_arm64_u_boot_image(const void *buffer, size_t size)
{
	const u8 *data = buffer;
	u64 image_size, text_base, end_offset;
	u64 bss_start_offset, bss_end_offset;

	if (!buffer || size < 0x60)
		return false;

	if (get_unaligned_le32(data + 0x38) != 0x644d5241)
		return false;

	image_size = get_unaligned_le64(data + 0x10);
	text_base = get_unaligned_le64(data + 0x40);
	end_offset = get_unaligned_le64(data + 0x48);
	bss_start_offset = get_unaligned_le64(data + 0x50);
	bss_end_offset = get_unaligned_le64(data + 0x58);

	if (image_size < 0x60 || text_base < 0x100000 ||
	    (text_base & 0xfff))
		return false;

	if (end_offset < 0x60 || end_offset > image_size ||
	    bss_start_offset < 0x60 || bss_start_offset > image_size ||
	    bss_end_offset < bss_start_offset || bss_end_offset > image_size)
		return false;

	return true;
}

bool android_boot_image_has_arm64_u_boot(const void *buffer, size_t size)
{
	const struct andr_boot_img_hdr_v0 *hdr = buffer;
	u32 kernel_offset;

	if (!buffer || size < sizeof(*hdr) || !is_android_boot_image_header(buffer))
		return false;

	if (hdr->header_version <= 2) {
		if (!android_boot_image_v0_v1_v2_page_size_valid(hdr))
			return false;

		kernel_offset = hdr->page_size;
	} else {
		kernel_offset = ANDR_GKI_PAGE_SIZE;
	}

	if (kernel_offset > size || size - kernel_offset < 0x60)
		return false;

	return is_arm64_u_boot_image((const u8 *)buffer + kernel_offset,
				     size - kernel_offset);
}

void towed_boot_android_payload_init(void *header, u32 payload_offset,
				     u32 payload_size)
{
	u8 *data = header;

	memset(header, 0, TOWED_BOOT_ANDROID_PAYLOAD_HEADER_SIZE);
	memcpy(data, TOWED_BOOT_ANDROID_PAYLOAD_MAGIC,
	       TOWED_BOOT_ANDROID_PAYLOAD_MAGIC_SIZE);
	put_unaligned_le32(TOWED_BOOT_ANDROID_PAYLOAD_VERSION,
			   data + TOWED_BOOT_ANDROID_PAYLOAD_VERSION_OFF);
	put_unaligned_le32(TOWED_BOOT_ANDROID_PAYLOAD_HEADER_SIZE,
			   data + TOWED_BOOT_ANDROID_PAYLOAD_HDR_SIZE_OFF);
	put_unaligned_le32(payload_offset,
			   data + TOWED_BOOT_ANDROID_PAYLOAD_OFFSET_OFF);
	put_unaligned_le32(payload_size,
			   data + TOWED_BOOT_ANDROID_PAYLOAD_SIZE_OFF);
}

bool towed_boot_android_payload_get(const void *header, u32 *payload_offset,
				    u32 *payload_size)
{
	const u8 *data = header;
	u32 header_size, version;

	if (!header || !payload_offset || !payload_size)
		return false;

	if (memcmp(data, TOWED_BOOT_ANDROID_PAYLOAD_MAGIC,
		   TOWED_BOOT_ANDROID_PAYLOAD_MAGIC_SIZE))
		return false;

	version = get_unaligned_le32(data +
				     TOWED_BOOT_ANDROID_PAYLOAD_VERSION_OFF);
	header_size = get_unaligned_le32(data +
					 TOWED_BOOT_ANDROID_PAYLOAD_HDR_SIZE_OFF);
	if (version != TOWED_BOOT_ANDROID_PAYLOAD_VERSION ||
	    header_size != TOWED_BOOT_ANDROID_PAYLOAD_HEADER_SIZE)
		return false;

	*payload_offset = get_unaligned_le32(data +
					     TOWED_BOOT_ANDROID_PAYLOAD_OFFSET_OFF);
	*payload_size = get_unaligned_le32(data +
					   TOWED_BOOT_ANDROID_PAYLOAD_SIZE_OFF);

	return true;
}

ulong android_image_get_end(const struct andr_boot_img_hdr_v0 *hdr,
			    const void *vendor_boot_img)
{
	struct andr_image_data img_data;

	if (!android_image_get_data(hdr, vendor_boot_img, &img_data))
		return -EINVAL;

	if (img_data.header_version > 2)
		return 0;

	return img_data.boot_img_total_size;
}

ulong android_image_get_kload(const void *hdr,
			      const void *vendor_boot_img)
{
	struct andr_image_data img_data;
	ulong comp;

	if (!android_image_get_data(hdr, vendor_boot_img, &img_data))
		return -EINVAL;

	comp = android_image_get_kcomp(hdr, vendor_boot_img);

	return android_image_get_kernel_addr(&img_data, comp);
}

ulong android_image_get_kcomp(const void *hdr,
			      const void *vendor_boot_img)
{
	struct andr_image_data img_data;
	const void *p;

	if (!android_image_get_data(hdr, vendor_boot_img, &img_data))
		return -EINVAL;

	p = (const void *)img_data.kernel_ptr;
	if (image_get_magic((struct legacy_img_hdr *)p) == IH_MAGIC)
		return image_get_comp((struct legacy_img_hdr *)p);
	else if (get_unaligned_le32(p) == LZ4F_MAGIC)
		return IH_COMP_LZ4;
	else
		return image_decomp_type(p, sizeof(u32));
}

/**
 * android_boot_append_bootconfig() - Append bootconfig parameters to ramdisk
 * @img_data: Pointer to Android image data
 * @params: Pointer to boot config parameters to append
 * @params_len: Length of boot config parameters
 * @ramdisk_dest: Destination address for the merged ramdisk
 *
 * This function copies the vendor ramdisk, boot ramdisk, and bootconfig to
 * the destination. It then appends the provided bootconfig parameters.
 *
 * Return: Bytes added to the bootconfig on success, negative on error.
 */
static long android_boot_append_bootconfig(const struct andr_image_data *img_data,
					   char *params, long params_len,
					   void *ramdisk_dest)
{
	void *vendor_ramdisk_src;
	void *boot_ramdisk_src;
	void *bootconfig_src;
	long bytes_added = 0;

	/* Map sources */
	vendor_ramdisk_src = map_sysmem(img_data->vendor_ramdisk_ptr,
					img_data->vendor_ramdisk_size);
	boot_ramdisk_src = map_sysmem(img_data->ramdisk_ptr,
				      img_data->boot_ramdisk_size);

	/* Copy Vendor Ramdisk */
	memcpy(ramdisk_dest, vendor_ramdisk_src, img_data->vendor_ramdisk_size);

	/* Copy Boot Ramdisk */
	memcpy((char *)ramdisk_dest + img_data->vendor_ramdisk_size,
	       boot_ramdisk_src, img_data->boot_ramdisk_size);

	/* Copy Bootconfig and Append Params */
	if (img_data->bootconfig_size) {
		bootconfig_src = map_sysmem(img_data->bootconfig_addr,
					    img_data->bootconfig_size);
		memcpy((char *)ramdisk_dest + img_data->vendor_ramdisk_size +
		       img_data->boot_ramdisk_size,
		       bootconfig_src, img_data->bootconfig_size);
		unmap_sysmem(bootconfig_src);

		if (params && params_len > 1) {
			void *bootconfig_ptr = (char *)ramdisk_dest +
					       img_data->vendor_ramdisk_size +
					       img_data->boot_ramdisk_size;
			bytes_added = add_bootconfig_parameters(params, params_len,
								(ulong)bootconfig_ptr,
								img_data->bootconfig_size);
		}
	}

	unmap_sysmem(boot_ramdisk_src);
	unmap_sysmem(vendor_ramdisk_src);

	if (bytes_added < 0)
		return bytes_added;

	return bytes_added;
}

/**
 * android_image_set_bootconfig() - Extract androidboot.* args and append to bootconfig
 * @hdr: Pointer to boot image header
 * @vendor_boot_img: Pointer to vendor boot image header
 * @ramdisk_addr: Destination address for the merged ramdisk
 *
 * Return: Size of the bootconfig section (including new params) on success, negative on error.
 */
static long android_image_set_bootconfig(const void *hdr,
					 const void *vendor_boot_img,
					 ulong ramdisk_addr)
{
	const char *bootargs = env_get("bootargs");
	char *params = NULL;
	char *new_bootargs = NULL;
	long params_len = 0;
	struct andr_image_data img_data;
	long ret;
	size_t len;
	const char *src;
	char *bc_dst;
	char *args_dst;
	ulong total_size;
	void *ramdisk_dest;

	if (!android_image_get_data(hdr, vendor_boot_img, &img_data))
		return -EINVAL;

	/* Extract androidboot.* parameters from bootargs */
	if (bootargs && img_data.bootconfig_size) {
		len = strlen(bootargs);
		src = bootargs;

		params = malloc(len + 1);
		new_bootargs = malloc(len + 1);
		if (!params || !new_bootargs) {
			free(params);
			free(new_bootargs);
			printf("Error: malloc failed\n");
			return -ENOMEM;
		}

		bc_dst = params;
		args_dst = new_bootargs;

		/* Extract androidboot.* and build new bootargs in one pass */
		while (*src) {
			/* Skip leading spaces */
			while (*src == ' ')
				src++;
			if (!*src)
				break;

			/* Check if this param starts with androidboot. */
			if (strncmp(src, "androidboot.", 12) == 0) {
				/* Copy to bootconfig (add newline if not first) */
				if (bc_dst != params)
					*bc_dst++ = '\n';
				while (*src && *src != ' ')
					*bc_dst++ = *src++;
			} else {
				/* Copy to new bootargs (add space if not first) */
				if (args_dst != new_bootargs)
					*args_dst++ = ' ';
				while (*src && *src != ' ')
					*args_dst++ = *src++;
			}
		}

		*bc_dst++ = '\n'; /* Final newline for bootconfig */
		*bc_dst = '\0';
		*args_dst = '\0';
		params_len = bc_dst - params;

		/* Update bootargs if we extracted any androidboot params */
		if (params_len > 1)
			env_set("bootargs", new_bootargs);
	}

	/* Calculate total size for mapping */
	total_size = img_data.ramdisk_size + img_data.bootconfig_size;
	if (params_len > 1)
		total_size += params_len + BOOTCONFIG_TRAILER_SIZE;

	/* Map Dest */
	ramdisk_dest = map_sysmem(ramdisk_addr, total_size);

	/* Copy data */
	ret = android_boot_append_bootconfig(&img_data, params, params_len,
					     ramdisk_dest);

	unmap_sysmem(ramdisk_dest);
	free(params);
	free(new_bootargs);

	return ret;
}

int android_image_get_ramdisk(const void *hdr, const void *vendor_boot_img,
			      ulong *rd_data, ulong *rd_len)
{
	struct andr_image_data img_data = {0};
	ulong staged_addr;
	ulong ramdisk_ptr;

	if (!android_image_get_data(hdr, vendor_boot_img, &img_data))
		return -EINVAL;

	if (!img_data.ramdisk_size)
		return -ENOENT;

	staged_addr = env_get_hex("towed_android_ramdisk_addr", 0);
	if (staged_addr) {
		*rd_data = staged_addr;
		*rd_len = img_data.ramdisk_size;
		printf("RAM disk staged addr 0x%08lx size %u KiB\n",
		       *rd_data, DIV_ROUND_UP(img_data.ramdisk_size, 1024));
		return 0;
	}

	/*
	 * Android tools can generate a boot.img with default load address
	 * or 0, even though it doesn't really make a lot of sense, and it
	 * might be valid on some platforms, we treat that address as
	 * the default value for this field, and try to pass ramdisk
	 * in place if possible.
	 */
	if (img_data.header_version > 2) {
		/* Ramdisk can't be used in-place, copy it to ramdisk_addr_r */
		if (img_data.ramdisk_addr == ANDROID_IMAGE_DEFAULT_RAMDISK_ADDR ||
		    IS_ENABLED(CONFIG_ANDROID_BOOT_IMAGE_IGNORE_BLOB_ADDR)) {
			ramdisk_ptr = env_get_ulong("ramdisk_addr_r", 16, 0);
			if (!ramdisk_ptr) {
				printf("Invalid ramdisk_addr_r to copy ramdisk into\n");
				return -EINVAL;
			}
		} else {
			ramdisk_ptr = img_data.ramdisk_addr;
		}
		*rd_data = ramdisk_ptr;
		if (img_data.header_version > 3)
			img_data.ramdisk_size +=
				android_image_set_bootconfig(hdr, vendor_boot_img, ramdisk_ptr);
	} else {
		/* Ramdisk can be used in-place, use current ptr */
		if (img_data.ramdisk_addr == 0 ||
		    img_data.ramdisk_addr == ANDROID_IMAGE_DEFAULT_RAMDISK_ADDR ||
		    img_data.ramdisk_addr == img_data.kernel_addr ||
		    IS_ENABLED(CONFIG_ANDROID_BOOT_IMAGE_IGNORE_BLOB_ADDR)) {
			*rd_data = img_data.ramdisk_ptr;
		} else {
			ramdisk_ptr = img_data.ramdisk_addr;
			*rd_data = ramdisk_ptr;
			memcpy((void *)(ramdisk_ptr), (void *)img_data.ramdisk_ptr,
			       img_data.ramdisk_size);
		}
	}

	printf("RAM disk load addr 0x%08lx size %u KiB\n",
	       *rd_data, DIV_ROUND_UP(img_data.ramdisk_size, 1024));

	*rd_len = img_data.ramdisk_size;
	return 0;
}

int android_image_get_second(const void *hdr, ulong *second_data, ulong *second_len)
{
	struct andr_image_data img_data;

	if (!android_image_get_data(hdr, NULL, &img_data))
		return -EINVAL;

	if (img_data.header_version > 2) {
		printf("Second stage bootloader is only supported for boot image version <= 2\n");
		return -EOPNOTSUPP;
	}

	if (!img_data.second_size) {
		*second_data = *second_len = 0;
		return -1;
	}

	*second_data = img_data.second_ptr;

	printf("second address is 0x%lx\n",*second_data);

	*second_len = img_data.second_size;
	return 0;
}

/**
 * android_image_get_dtbo() - Get address and size of recovery DTBO image.
 * @hdr_addr: Boot image header address
 * @addr: If not NULL, will contain address of recovery DTBO image
 * @size: If not NULL, will contain size of recovery DTBO image
 *
 * Get the address and size of DTBO image in "Recovery DTBO" area of Android
 * Boot Image in RAM. The format of this image is Android DTBO (see
 * corresponding "DTB/DTBO Partitions" AOSP documentation for details). Once
 * the address is obtained from this function, one can use 'adtimg' U-Boot
 * command or android_dt_*() functions to extract desired DTBO blob.
 *
 * This DTBO (included in boot image) is only needed for non-A/B devices, and it
 * only can be found in recovery image. On A/B devices we can always rely on
 * "dtbo" partition. See "Including DTBO in Recovery for Non-A/B Devices" in
 * AOSP documentation for details.
 *
 * Return: true on success or false on error.
 */
bool android_image_get_dtbo(ulong hdr_addr, ulong *addr, u32 *size)
{
	const struct andr_boot_img_hdr_v0 *hdr;
	ulong dtbo_img_addr;
	bool ret = true;

	hdr = map_sysmem(hdr_addr, sizeof(*hdr));
	if (!is_android_boot_image_header(hdr)) {
		printf("Error: Boot Image header is incorrect\n");
		ret = false;
		goto exit;
	}

	if (hdr->header_version != 1 && hdr->header_version != 2) {
		printf("Error: header version must be >= 1 and <= 2 to get dtbo\n");
		ret = false;
		goto exit;
	}

	if (hdr->recovery_dtbo_size == 0) {
		printf("Error: recovery_dtbo_size is 0\n");
		ret = false;
		goto exit;
	}

	/* Calculate the address of DTB area in boot image */
	dtbo_img_addr = hdr_addr;
	dtbo_img_addr += hdr->page_size;
	dtbo_img_addr += ALIGN(hdr->kernel_size, hdr->page_size);
	dtbo_img_addr += ALIGN(hdr->ramdisk_size, hdr->page_size);
	dtbo_img_addr += ALIGN(hdr->second_size, hdr->page_size);

	if (addr)
		*addr = dtbo_img_addr;
	if (size)
		*size = hdr->recovery_dtbo_size;

exit:
	unmap_sysmem(hdr);
	return ret;
}

/**
 * android_image_get_dtb_img_addr() - Get the address of DTB area in boot image.
 * @hdr_addr: Boot image header address
 * @vhdr_addr: Vendor Boot image header address
 * @addr: Will contain the address of DTB area in boot image
 *
 * Return: true on success or false on fail.
 */
static bool android_image_get_dtb_img_addr(ulong hdr_addr, ulong vhdr_addr, ulong *addr)
{
	const struct andr_boot_img_hdr_v0 *hdr;
	const struct andr_vnd_boot_img_hdr *v_hdr;
	ulong dtb_img_addr;
	bool ret = true;

	hdr = map_sysmem(hdr_addr, sizeof(*hdr));
	if (!is_android_boot_image_header(hdr)) {
		printf("Error: Boot Image header is incorrect\n");
		ret = false;
		goto exit;
	}

	if (hdr->header_version < 2) {
		printf("Error: header_version must be >= 2 to get dtb\n");
		ret = false;
		goto exit;
	}

	if (hdr->header_version == 2) {
		if (!hdr->dtb_size) {
			printf("Error: dtb_size is 0\n");
			ret = false;
			goto exit;
		}
		/* Calculate the address of DTB area in boot image */
		dtb_img_addr = hdr_addr;
		dtb_img_addr += hdr->page_size;
		dtb_img_addr += ALIGN(hdr->kernel_size, hdr->page_size);
		dtb_img_addr += ALIGN(hdr->ramdisk_size, hdr->page_size);
		dtb_img_addr += ALIGN(hdr->second_size, hdr->page_size);
		dtb_img_addr += ALIGN(hdr->recovery_dtbo_size, hdr->page_size);

		*addr = dtb_img_addr;
	}

	if (hdr->header_version > 2) {
		v_hdr = map_sysmem(vhdr_addr, sizeof(*v_hdr));
		if (!v_hdr->dtb_size) {
			printf("Error: dtb_size is 0\n");
			ret = false;
			unmap_sysmem(v_hdr);
			goto exit;
		}
		/* Calculate the address of DTB area in boot image */
		dtb_img_addr = vhdr_addr;
		dtb_img_addr += v_hdr->page_size;
		if (v_hdr->vendor_ramdisk_size)
			dtb_img_addr += ALIGN(v_hdr->vendor_ramdisk_size, v_hdr->page_size);
		*addr = dtb_img_addr;
		unmap_sysmem(v_hdr);
		goto exit;
	}
exit:
	unmap_sysmem(hdr);
	return ret;
}

/**
 * android_image_get_dtb_by_index() - Get address and size of blob in DTB area.
 * @hdr_addr: Boot image header address
 * @vendor_boot_img: Pointer to vendor boot image header, which is at the start of the image.
 * @index: Index of desired DTB in DTB area (starting from 0)
 * @addr: If not NULL, will contain address to specified DTB
 * @size: If not NULL, will contain size of specified DTB
 *
 * Get the address and size of DTB blob by its index in DTB area of Android
 * Boot Image in RAM.
 *
 * Return: true on success or false on error.
 */
bool android_image_get_dtb_by_index(ulong hdr_addr, ulong vendor_boot_img,
				    u32 index, ulong *addr, u32 *size)
{
	struct andr_image_data img_data;
	const void *vendor_boot_hdr = NULL;

	if (vendor_boot_img != -1)
		vendor_boot_hdr = (const void *)vendor_boot_img;

	if (!android_image_get_data((const void *)hdr_addr, vendor_boot_hdr,
				    &img_data))
		return false;

	ulong dtb_img_addr;	/* address of DTB part in boot image */
	u32 dtb_img_size;	/* size of DTB payload in boot image */
	ulong dtb_addr;		/* address of DTB blob with specified index  */
	u32 i;			/* index iterator */

	if (!android_image_get_dtb_img_addr(hdr_addr, vendor_boot_img,
					    &dtb_img_addr))
		return false;

	/* Check if DTB area of boot image is in DTBO format */
	if (android_dt_check_header(dtb_img_addr)) {
		return android_dt_get_fdt_by_index(dtb_img_addr, index, addr,
						   size);
	}

	/* Find out the address of DTB with specified index in concat blobs */
	dtb_img_size = img_data.dtb_size;
	i = 0;
	dtb_addr = dtb_img_addr;
	while (dtb_addr < dtb_img_addr + dtb_img_size) {
		const struct fdt_header *fdt;
		struct fdt_header fdth __aligned(8);
		u32 dtb_size;

		fdt = map_sysmem(dtb_addr, sizeof(*fdt));
		memcpy(&fdth, fdt, sizeof(*fdt));
		unmap_sysmem(fdt);

		if (fdt_check_header(&fdth) != 0) {
			printf("Error: Invalid FDT header for index %u\n", i);
			return false;
		}

		dtb_size = fdt_totalsize(&fdth);

		if (i == index) {
			if (size)
				*size = dtb_size;
			if (addr)
				*addr = dtb_addr;
			return true;
		}

		dtb_addr += dtb_size;
		++i;
	}

	printf("Error: Index is out of bounds (%u/%u)\n", index, i);
	return false;
}

#if !defined(CONFIG_XPL_BUILD)
/**
 * android_print_contents - prints out the contents of the Android format image
 * @hdr: pointer to the Android format image header
 *
 * android_print_contents() formats a multi line Android image contents
 * description.
 * The routine prints out Android image properties
 *
 * returns:
 *     no returned results
 */
void android_print_contents(const struct andr_boot_img_hdr_v0 *hdr)
{
	if (hdr->header_version >= 3) {
		printf("Content print is not supported for boot image header version > 2");
		return;
	}
	const char * const p = IMAGE_INDENT_STRING;
	/* os_version = ver << 11 | lvl */
	u32 os_ver = hdr->os_version >> 11;
	u32 os_lvl = hdr->os_version & ((1U << 11) - 1);

	printf("%skernel size:          %x\n", p, hdr->kernel_size);
	printf("%skernel address:       %x\n", p, hdr->kernel_addr);
	printf("%sramdisk size:         %x\n", p, hdr->ramdisk_size);
	printf("%sramdisk address:      %x\n", p, hdr->ramdisk_addr);
	printf("%ssecond size:          %x\n", p, hdr->second_size);
	printf("%ssecond address:       %x\n", p, hdr->second_addr);
	printf("%stags address:         %x\n", p, hdr->tags_addr);
	printf("%spage size:            %x\n", p, hdr->page_size);
	/* ver = A << 14 | B << 7 | C         (7 bits for each of A, B, C)
	 * lvl = ((Y - 2000) & 127) << 4 | M  (7 bits for Y, 4 bits for M) */
	printf("%sos_version:           %x (ver: %u.%u.%u, level: %u.%u)\n",
	       p, hdr->os_version,
	       (os_ver >> 7) & 0x7F, (os_ver >> 14) & 0x7F, os_ver & 0x7F,
	       (os_lvl >> 4) + 2000, os_lvl & 0x0F);
	printf("%sname:                 %s\n", p, hdr->name);
	printf("%scmdline:              %s\n", p, hdr->cmdline);
	printf("%sheader_version:       %d\n", p, hdr->header_version);

	if (hdr->header_version >= 1) {
		printf("%srecovery dtbo size:   %x\n", p,
		       hdr->recovery_dtbo_size);
		printf("%srecovery dtbo offset: %llx\n", p,
		       hdr->recovery_dtbo_offset);
		printf("%sheader size:          %x\n", p,
		       hdr->header_size);
	}

	if (hdr->header_version == 2) {
		printf("%sdtb size:             %x\n", p, hdr->dtb_size);
		printf("%sdtb addr:             %llx\n", p, hdr->dtb_addr);
	}
}

/**
 * android_image_print_dtb_info - Print info for one DTB blob in DTB area.
 * @fdt: DTB header
 * @index: Number of DTB blob in DTB area.
 *
 * Return: true on success or false on error.
 */
static bool android_image_print_dtb_info(const struct fdt_header *fdt,
					 u32 index)
{
	int root_node_off;
	u32 fdt_size;
	const char *model;
	const char *compatible;

	root_node_off = fdt_path_offset(fdt, "/");
	if (root_node_off < 0) {
		printf("Error: Root node not found\n");
		return false;
	}

	fdt_size = fdt_totalsize(fdt);
	compatible = fdt_getprop(fdt, root_node_off, "compatible",
				 NULL);
	model = fdt_getprop(fdt, root_node_off, "model", NULL);

	printf(" - DTB #%u:\n", index);
	printf("           (DTB)size = %d\n", fdt_size);
	printf("          (DTB)model = %s\n", model ? model : "(unknown)");
	printf("     (DTB)compatible = %s\n",
	       compatible ? compatible : "(unknown)");

	return true;
}

/**
 * android_image_print_dtb_contents() - Print info for DTB blobs in DTB area.
 * @hdr_addr: Boot image header address
 *
 * DTB payload in Android Boot Image v2+ can be in one of following formats:
 *   1. Concatenated DTB blobs
 *   2. Android DTBO format (see CONFIG_CMD_ADTIMG for details)
 *
 * This function does next:
 *   1. Prints out the format used in DTB area
 *   2. Iterates over all DTB blobs in DTB area and prints out the info for
 *      each blob.
 *
 * Return: true on success or false on error.
 */
bool android_image_print_dtb_contents(ulong hdr_addr)
{
	const struct andr_boot_img_hdr_v0 *hdr;
	bool res;
	ulong dtb_img_addr;	/* address of DTB part in boot image */
	u32 dtb_img_size;	/* size of DTB payload in boot image */
	ulong dtb_addr;		/* address of DTB blob with specified index  */
	u32 i;			/* index iterator */

	res = android_image_get_dtb_img_addr(hdr_addr, 0, &dtb_img_addr);
	if (!res)
		return false;

	/* Check if DTB area of boot image is in DTBO format */
	if (android_dt_check_header(dtb_img_addr)) {
		printf("## DTB area contents (DTBO format):\n");
		android_dt_print_contents(dtb_img_addr);
		return true;
	}

	printf("## DTB area contents (concat format):\n");

	/* Iterate over concatenated DTB blobs */
	hdr = map_sysmem(hdr_addr, sizeof(*hdr));
	dtb_img_size = hdr->dtb_size;
	unmap_sysmem(hdr);
	i = 0;
	dtb_addr = dtb_img_addr;
	while (dtb_addr < dtb_img_addr + dtb_img_size) {
		const struct fdt_header *fdt;
		struct fdt_header *fulldt;
		struct fdt_header fdth __aligned(8);
		u32 dtb_size;

		fdt = map_sysmem(dtb_addr, sizeof(*fdt));
		memcpy(&fdth, fdt, sizeof(*fdt));
		unmap_sysmem(fdt);

		if (fdt_check_header(&fdth) != 0) {
			printf("Error: Invalid FDT header for index %u\n", i);
			return false;
		}

		dtb_size = fdt_totalsize(&fdth);

		/* The device tree must be at an 8-byte aligned address */
		if (!IS_ALIGNED((uintptr_t)fdt, 8)) {
			fulldt = memalign(8, dtb_size);
			if (!fulldt)
				return false;

			fdt = map_sysmem(dtb_addr, dtb_size);
			memcpy(fulldt, fdt, dtb_size);
			unmap_sysmem(fdt);
			res = android_image_print_dtb_info(fulldt, i);
			free(fulldt);
		} else {
			fulldt = map_sysmem(dtb_addr, dtb_size);
			res = android_image_print_dtb_info(fulldt, i);
			unmap_sysmem(fulldt);
		}

		if (!res)
			return false;

		dtb_addr += dtb_size;
		++i;
	}

	return true;
}
#endif

/* SPDX-License-Identifier: APSL-2.0 */
#pragma once

/*
 * Source-pinned x86_64 XNU boot-argument layout and preflight helpers.
 *
 * Layout is derived from pexpert/pexpert/i386/boot.h in
 * apple-oss-distributions/xnu xnu-12377.121.6
 * (ac9718fb1af618d5ce8678d0dc6e8a58f252216f). This is an isolated validation
 * object, not a complete EFI handoff implementation. It is not linked into
 * xnu.ko, and it must not be used to enter XNU until the matching kernel
 * artifact confirms its ABI and all required platform fields are supplied.
 */
#ifdef __KERNEL__
#include <linux/types.h>
typedef u8 xnu_boot_u8;
typedef u16 xnu_boot_u16;
typedef u32 xnu_boot_u32;
typedef u64 xnu_boot_u64;
#else
#include <stdint.h>
typedef uint8_t xnu_boot_u8;
typedef uint16_t xnu_boot_u16;
typedef uint32_t xnu_boot_u32;
typedef uint64_t xnu_boot_u64;
#endif

#define XNU_X86_BOOT_ARGS_VERSION 2U
#define XNU_X86_BOOT_ARGS_REVISION 0U
#define XNU_X86_BOOT_ARGS_MAX_REVISION 1U
#define XNU_X86_BOOT_ARGS_EFI_MODE_64 64U
#define XNU_X86_BOOT_ARGS_COMMAND_LINE_SIZE 1024U

struct xnu_x86_efi_memory_range {
	xnu_boot_u32 Type;
	xnu_boot_u32 Pad;
	xnu_boot_u64 PhysicalStart;
	xnu_boot_u64 VirtualStart;
	xnu_boot_u64 NumberOfPages;
	xnu_boot_u64 Attribute;
};

struct xnu_x86_boot_video_v1 {
	xnu_boot_u32 v_baseAddr;
	xnu_boot_u32 v_display;
	xnu_boot_u32 v_rowBytes;
	xnu_boot_u32 v_width;
	xnu_boot_u32 v_height;
	xnu_boot_u32 v_depth;
};

struct xnu_x86_boot_video {
	xnu_boot_u32 v_display;
	xnu_boot_u32 v_rowBytes;
	xnu_boot_u32 v_width;
	xnu_boot_u32 v_height;
	xnu_boot_u32 v_depth;
	xnu_boot_u8 v_rotate;
	xnu_boot_u8 v_resv_byte[3];
	xnu_boot_u32 v_resv[6];
	xnu_boot_u64 v_baseAddr;
};

/* Wire layout from XNU's public boot.h. Keep member order and widths exact. */
struct xnu_x86_boot_args {
	xnu_boot_u16 Revision;
	xnu_boot_u16 Version;
	xnu_boot_u8 efiMode;
	xnu_boot_u8 debugMode;
	xnu_boot_u16 flags;
	char CommandLine[XNU_X86_BOOT_ARGS_COMMAND_LINE_SIZE];
	xnu_boot_u32 MemoryMap;
	xnu_boot_u32 MemoryMapSize;
	xnu_boot_u32 MemoryMapDescriptorSize;
	xnu_boot_u32 MemoryMapDescriptorVersion;
	struct xnu_x86_boot_video_v1 VideoV1;
	xnu_boot_u32 deviceTreeP;
	xnu_boot_u32 deviceTreeLength;
	xnu_boot_u32 kaddr;
	xnu_boot_u32 ksize;
	xnu_boot_u32 efiRuntimeServicesPageStart;
	xnu_boot_u32 efiRuntimeServicesPageCount;
	xnu_boot_u64 efiRuntimeServicesVirtualPageStart;
	xnu_boot_u32 efiSystemTable;
	xnu_boot_u32 kslide;
	xnu_boot_u32 performanceDataStart;
	xnu_boot_u32 performanceDataSize;
	xnu_boot_u32 keyStoreDataStart;
	xnu_boot_u32 keyStoreDataSize;
	xnu_boot_u64 bootMemStart;
	xnu_boot_u64 bootMemSize;
	xnu_boot_u64 PhysicalMemorySize;
	xnu_boot_u64 FSBFrequency;
	xnu_boot_u64 pciConfigSpaceBaseAddress;
	xnu_boot_u32 pciConfigSpaceStartBusNumber;
	xnu_boot_u32 pciConfigSpaceEndBusNumber;
	xnu_boot_u32 csrActiveConfig;
	xnu_boot_u32 csrCapabilities;
	xnu_boot_u32 boot_SMC_plimit;
	xnu_boot_u16 bootProgressMeterStart;
	xnu_boot_u16 bootProgressMeterEnd;
	struct xnu_x86_boot_video Video;
	xnu_boot_u32 apfsDataStart;
	xnu_boot_u32 apfsDataSize;
	xnu_boot_u64 KC_hdrs_vaddr;
	xnu_boot_u64 arvRootHashStart;
	xnu_boot_u64 arvRootHashSize;
	xnu_boot_u64 arvManifestStart;
	xnu_boot_u64 arvManifestSize;
	xnu_boot_u64 bsARVRootHashStart;
	xnu_boot_u64 bsARVRootHashSize;
	xnu_boot_u64 bsARVManifestStart;
	xnu_boot_u64 bsARVManifestSize;
	xnu_boot_u32 __reserved4[692];
};

/*
 * Inputs use 64-bit physical values so callers cannot silently truncate the
 * 32-bit fields in XNU's boot_args. The function prepares only core fields;
 * fields required by EFI, firmware, video, security, or the actual kernel
 * entry protocol remain unset and this output is not bootable by itself.
 */
struct xnu_x86_boot_args_core {
	const char *command_line;
	xnu_boot_u32 command_line_length;
	xnu_boot_u64 memory_map_pa;
	xnu_boot_u64 memory_map_size;
	xnu_boot_u64 memory_map_descriptor_size;
	xnu_boot_u64 memory_map_descriptor_version;
	xnu_boot_u64 device_tree_pa;
	xnu_boot_u64 device_tree_size;
	xnu_boot_u64 kernel_pa;
	xnu_boot_u64 kernel_size;
	xnu_boot_u64 boot_mem_start;
	xnu_boot_u64 boot_mem_size;
	xnu_boot_u64 physical_memory_size;
	xnu_boot_u64 fsb_frequency;
};

/*
 * Build the bounded core-field fixture and validate its source-pinned layout.
 * Returns 0, -EINVAL for malformed input, -ERANGE for unrepresentable address
 * ranges, -ENOEXEC for malformed boot_args, or -EOPNOTSUPP for an ABI variant
 * outside the pinned source subset.
 */
int xnu_x86_boot_args_build_core(struct xnu_x86_boot_args *out,
				 const struct xnu_x86_boot_args_core *in);
int xnu_x86_boot_args_validate(const struct xnu_x86_boot_args *args);

/*
 * Inspect a supplied EFI map byte buffer, without dereferencing MemoryMap.
 * The buffer must match MemoryMapSize. Reject empty or unaligned descriptor
 * ranges and physical page indices that would truncate in the pinned XNU.
 * The pinned XNU skips page 0 and page UINT32_MAX even though their
 * indices fit. This check does not determine usable memory or ownership.
 */
int xnu_x86_boot_memory_map_validate(const struct xnu_x86_boot_args *args,
				     const void *map, unsigned long map_size);

/*
 * Inspect a supplied Apple Device Tree buffer without dereferencing deviceTreeP.
 * Size must match deviceTreeLength. Every node must have a bounded name, and
 * the tree is limited to 32 levels. The inspection buffer may be unaligned,
 * but the copy handed to XNU must start on a four-byte boundary because XNU
 * aligns properties using absolute addresses. Platform properties and memory
 * ownership require separate validation before entry.
 */
int xnu_x86_boot_device_tree_validate(const struct xnu_x86_boot_args *args,
				      const void *tree, unsigned long tree_size);

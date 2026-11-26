// SPDX-License-Identifier: APSL-2.0
/*
 * Fail-closed x86_64 module target.
 *
 * This allows the module packaging and CI target to build on x86_64 while
 * native XNU entry, return, fault delivery, and context isolation are being
 * ported. Do not replace this with a successful no-op: loading an XNU module
 * without those architecture boundaries would be unsafe.
 */
#include <linux/errno.h>
#include <linux/module.h>

static int __init xnu_x86_64_init(void)
{
	pr_err("xnu: native x86_64 world switch is not implemented; refusing to load\n");
	return -EOPNOTSUPP;
}

module_init(xnu_x86_64_init);

MODULE_LICENSE("Proprietary");
MODULE_DESCRIPTION("Fail-closed x86_64 XNU module target");

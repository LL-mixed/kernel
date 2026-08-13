/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_OBMM_H
#define _LINUX_OBMM_H

#include <linux/fs.h>
#include <linux/types.h>

bool obmm_file_matches_region(struct file *file, u64 mem_id);

#endif

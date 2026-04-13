/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2023-2025. All rights reserved.
 * Description：OBMM Framework's implementations.
 */
#ifndef OBMM_LOW_MEM_H
#define OBMM_LOW_MEM_H

#include <linux/mm.h>

#ifdef CONFIG_RECLAIM_NOTIFY
int lowmem_notify_init(void);
void lowmem_notify_exit(void);
#else
static inline int lowmem_notify_init(void)
{
	return 0;
}

static inline void lowmem_notify_exit(void)
{
}
#endif

#endif

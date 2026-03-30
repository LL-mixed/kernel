// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (c) HiSilicon Technologies Co., Ltd. 2025. All rights reserved.
 */

#define pr_fmt(fmt)	"ubus eid: " fmt

#include "ubus.h"
#include "eid.h"

static DEFINE_IDA(ub_eid_ida);

int ub_eid_request(guid_t *id, u32 *eid)
{
	int ida;

	if (!id || !eid)
		return -EINVAL;

	ida = ida_alloc_range(&ub_eid_ida, ubc_eid_start, ubc_eid_end, GFP_KERNEL);
	if (ida < 0)
		return ida;

	*eid = (u32)ida;

	return 0;
}

void ub_eid_release(u32 eid)
{
	ida_free(&ub_eid_ida, eid);
}

int ub_eid_alloc(struct ub_entity *uent)
{
	struct device *dev;
	u32 eid = 0;
	int ret;

	pr_info("ub_eid_alloc enter guid=%pUb entity_idx=%u type=%#x cluster=%d eid=%#x user_eid=%#x\n",
		uent ? uent->guid.dw : NULL, uent ? uent->entity_idx : 0,
		uent ? uent_type(uent) : 0, uent ? !!(is_ibus_controller(uent) && uent->ubc->cluster) : 0,
		uent ? uent->eid : 0, uent ? uent->user_eid : 0);

	if (is_p_device(uent))
		return 0;

	if (uent->eid) {
		ub_warn(uent, "uent eid not 0, eid=%#05x\n", uent->eid);
		return -EPERM;
	}

	if (is_ibus_controller(uent) && uent->ubc->cluster) {
		dev = &uent->ubc->dev;
		if (uent->ubc->cluster_bi && uent->ubc->cluster_bi->info.eid) {
			eid = uent->ubc->cluster_bi->info.eid;
			pr_info("ub_eid_alloc use cluster_bi eid=%#x\n", eid);
			dev_info(dev, "use cluster_bi eid=%#x\n", eid);
		} else if (uent->user_eid) {
			eid = uent->user_eid;
			pr_info("ub_eid_alloc use user_eid=%#x\n", eid);
			dev_info(dev, "use user_eid=%#x\n", eid);
		} else {
			pr_info("ub_eid_alloc fallback UB_EID_0 read start\n");
			ret = ub_cfg_read_dword(uent, UB_EID_0, &eid);
			if (ret) {
				pr_info("ub_eid_alloc fallback UB_EID_0 read failed ret=%d\n", ret);
				dev_err(dev, "query cluster ubc, ret=%d\n", ret);
				return ret;
			}

			eid &= UB_COMPACT_EID_MASK;
			pr_info("ub_eid_alloc fallback UB_EID_0 read done eid=%#x\n", eid);
			if (eid)
				dev_info(dev, "update cluster ubc eid, eid=%#x\n", eid);
		}

		uent->eid = eid;
		pr_info("ub_eid_alloc exit cluster eid=%#x\n", uent->eid);
		return 0;
	}

	ret = ub_eid_request(&uent->guid.id, &eid);
	if (!ret) {
		uent->eid = eid;
		pr_info("ub_eid_alloc exit local eid=%#x\n", uent->eid);
	} else {
		pr_info("ub_eid_alloc local request failed ret=%d\n", ret);
	}

	return ret;
}

void ub_eid_free(struct ub_entity *uent)
{
	u32 eid = uent->eid;

	if (is_p_device(uent))
		return;

	if (!eid) {
		ub_warn(uent, "eid free 0\n");
		return;
	}

	if (is_ibus_controller(uent) && uent->ubc->cluster)
		return;

	uent->eid = 0;
	ub_eid_release(eid);
}

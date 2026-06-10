// SPDX-License-Identifier: GPL-2.0
/*
 * UB-Attached SSD platform driver.
 *
 * Binds via FDT compatible "ub-sim,ssd-v1".
 * Provides /dev/ub_ssd0 with ioctl for SUBMIT/WAIT/QUERY.
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/miscdevice.h>
#include <linux/of.h>
#include <linux/io.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/delay.h>

#include <uapi/ub/ub_ssd.h>

#define DRIVER_NAME "ub-ssd"
#define DEV_NAME "ub_ssd0"

struct ub_ssd_priv {
	struct device *dev;
	void __iomem *mmio;
	struct miscdevice mdev;
	uint32_t cna;
};

static int ub_ssd_submit(struct ub_ssd_priv *priv,
			 struct ub_ssd_cmd_v1 __user *ucmd)
{
	struct ub_ssd_cmd_v1 kcmd;
	uint32_t status;

	if (copy_from_user(&kcmd, ucmd, sizeof(kcmd)))
		return -EFAULT;

	status = readl(priv->mmio + SSD_STATUS_OFF);
	if (status & SSD_STATUS_BUSY)
		return -EBUSY;

	if (status & SSD_STATUS_COMPLETION_VALID) {
		writel(1, priv->mmio + SSD_CLEAR_CPL_OFF);
		udelay(10);
	}

	/* memcpy_toio can't be used here: the packed command struct is 172
	 * bytes (not a multiple of 8), so the trailing bytes would be
	 * written as 1-byte stores.  QEMU's MMIO handler only accepts
	 * 4+ byte accesses, so byte stores trigger read-modify-write
	 * cycles that corrupt data because the command slot is not
	 * readable.  Use explicit 4-byte writes instead.
	 */
	{
		const uint32_t *src = (const uint32_t *)&kcmd;
		int i;
		for (i = 0; i < sizeof(kcmd) / 4; i++)
			writel(src[i], priv->mmio + SSD_CMD_SLOT_OFF + i * 4);
	}

	writel(1, priv->mmio + SSD_DOORBELL_OFF);

	return 0;
}

static int ub_ssd_wait(struct ub_ssd_priv *priv,
		       struct ub_ssd_cpl_v1 __user *ucpl)
{
	struct ub_ssd_cpl_v1 kcpl;
	uint32_t status;
	int retries;

	for (retries = 0; retries < 500000; retries++) {
		status = readl(priv->mmio + SSD_STATUS_OFF);
		if (status & SSD_STATUS_COMPLETION_VALID)
			break;
		udelay(10);
	}

	if (!(status & SSD_STATUS_COMPLETION_VALID))
		return -ETIMEDOUT;

	memcpy_fromio(&kcpl, priv->mmio + SSD_CPL_SLOT_OFF, sizeof(kcpl));

	writel(1, priv->mmio + SSD_CLEAR_CPL_OFF);

	if (copy_to_user(ucpl, &kcpl, sizeof(kcpl)))
		return -EFAULT;

	return 0;
}

static int ub_ssd_status_to_errno(uint32_t status)
{
	switch (status) {
	case SSD_OK:
		return 0;
	case SSD_ERR_BAD_VERSION:
	case SSD_ERR_BAD_OPCODE:
	case SSD_ERR_BAD_BLOCK:
	case SSD_ERR_BAD_SNAPSHOT:
		return -EINVAL;
	case SSD_ERR_BAD_DESCRIPTOR:
		return -EFAULT;
	case SSD_ERR_TOKEN_DENIED:
		return -EACCES;
	case SSD_ERR_STALE_EPOCH:
		return -EAGAIN;
	case SSD_ERR_SEGMENT_RETIRED:
		return -ENODEV;
	case SSD_ERR_COH_TIMEOUT:
		return -ETIMEDOUT;
	case SSD_ERR_DEVICE_BUSY:
		return -EBUSY;
	case SSD_ERR_CHECKSUM:
	case SSD_ERR_VERSION_CONFLICT:
	case SSD_ERR_SEALED:
	case SSD_ERR_TOMBSTONED:
	case SSD_ERR_BACKEND_IO:
	default:
		return -EIO;
	}
}

static int ub_ssd_submit_wait(struct ub_ssd_priv *priv,
			     struct ub_ssd_cmd_v1 *ucmd,
			     struct ub_ssd_cpl_v1 __user *ucpl)
{
	struct ub_ssd_cpl_v1 kcpl = {};
	int rc;

	rc = ub_ssd_submit(priv, ucmd);
	if (rc)
		return rc;

	rc = ub_ssd_wait(priv, &kcpl);
	if (rc)
		return rc;

	if (ucpl && copy_to_user(ucpl, &kcpl, sizeof(*ucpl)))
		return -EFAULT;

	return ub_ssd_status_to_errno(kcpl.status);
}

static int ub_ssd_submit_snapshot(struct ub_ssd_priv *priv,
				 struct ub_ssd_snapshot_v1 __user *usap,
				 uint32_t opcode)
{
	struct ub_ssd_snapshot_v1 snap = {};
	struct ub_ssd_cmd_v1 cmd = {};
	struct ub_ssd_cpl_v1 kcpl = {};
	int rc;

	if (copy_from_user(&snap, usap, sizeof(snap)))
		return -EFAULT;

	if (snap.version != 1)
		return -EINVAL;
	if (snap.snapshot_size == 0 && snap.buffer.bytes == 0)
		return -EINVAL;

	cmd.version = 1;
	cmd.opcode = opcode;
	cmd.buffer = snap.buffer;
	if (snap.snapshot_size)
		cmd.buffer.bytes = snap.snapshot_size;

	rc = ub_ssd_submit_wait(priv, &cmd, &kcpl);
	if (rc)
		return rc;

	switch (opcode) {
	case SSD_OP_EXPORT_SNAPSHOT:
		snap.snapshot_size = kcpl.bytes_written;
		break;
	case SSD_OP_IMPORT_SNAPSHOT:
		snap.snapshot_size = kcpl.bytes_read;
		break;
	default:
		break;
	}

	if (copy_to_user(usap, &snap, sizeof(snap)))
		return -EFAULT;

	return 0;
}

static int ub_ssd_query(struct ub_ssd_priv *priv,
			   struct ub_ssd_query_v1 __user *uq)
{
	struct ub_ssd_query_v1 kq = {};
	struct ub_ssd_cpl_v1 kcpl = {};
	uint32_t status;

	if (copy_from_user(&kq, uq, sizeof(kq)))
		return -EFAULT;

	kq.version = 1;

	if (kq.type == 0)
		kq.type = UB_QUERY_SSD_CAPS;

	switch (kq.type) {
	case UB_QUERY_SSD_CAPS:
		status = readl(priv->mmio + SSD_STATUS_OFF);
		kq.u.status.status_reg = status;
		kq.u.status.error_reg = readl(priv->mmio + SSD_ERROR_OFF);
		kq.u.status.last_req_id = readq(priv->mmio + SSD_LAST_REQ_ID_OFF);
		kq.u.status.backend_profile = readq(priv->mmio + SSD_BACKEND_PROFILE_OFF);
		kq.u.status.supported_commands =
			(1ULL << ((UB_SSD_SUBMIT >> _IOC_NRSHIFT))) |
			(1ULL << ((UB_SSD_WAIT >> _IOC_NRSHIFT))) |
			(1ULL << ((UB_SSD_QUERY >> _IOC_NRSHIFT))) |
			(1ULL << ((UB_SSD_EXPORT_SNAPSHOT >> _IOC_NRSHIFT))) |
			(1ULL << ((UB_SSD_IMPORT_SNAPSHOT >> _IOC_NRSHIFT)));
		if (status & SSD_STATUS_COMPLETION_VALID)
			memcpy_fromio(&kcpl, priv->mmio + SSD_CPL_SLOT_OFF, sizeof(kcpl));
		kq.u.status.completion = kcpl;
		return copy_to_user(uq, &kq, sizeof(kq)) ? -EFAULT : 0;
	default:
		return -EOPNOTSUPP;
	}
}

static long ub_ssd_ioctl(struct file *filp, unsigned int cmd,
			 unsigned long arg)
{
	struct ub_ssd_priv *priv =
		container_of(filp->private_data, struct ub_ssd_priv, mdev);

	switch (cmd) {
	case UB_SSD_SUBMIT:
		return ub_ssd_submit(priv, (struct ub_ssd_cmd_v1 __user *)arg);
	case UB_SSD_WAIT:
		return ub_ssd_wait(priv, (struct ub_ssd_cpl_v1 __user *)arg);
	case UB_SSD_QUERY:
		return ub_ssd_query(priv, (struct ub_ssd_query_v1 __user *)arg);
	case UB_SSD_EXPORT_SNAPSHOT:
		return ub_ssd_submit_snapshot(priv,
					      (struct ub_ssd_snapshot_v1 __user *)arg,
					      SSD_OP_EXPORT_SNAPSHOT);
	case UB_SSD_IMPORT_SNAPSHOT:
		return ub_ssd_submit_snapshot(priv,
					      (struct ub_ssd_snapshot_v1 __user *)arg,
					      SSD_OP_IMPORT_SNAPSHOT);
	default:
		return -ENOTTY;
	}
}

static const struct file_operations ub_ssd_fops = {
	.owner		= THIS_MODULE,
	.unlocked_ioctl = ub_ssd_ioctl,
};

static int ub_ssd_probe(struct platform_device *pdev)
{
	struct ub_ssd_priv *priv;
	struct resource *res;
	int err;

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->dev = &pdev->dev;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -ENODEV;

	priv->mmio = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(priv->mmio))
		return PTR_ERR(priv->mmio);

	of_property_read_u32(pdev->dev.of_node, "ub,cna", &priv->cna);

	priv->mdev.minor = MISC_DYNAMIC_MINOR;
	priv->mdev.name = DEV_NAME;
	priv->mdev.fops = &ub_ssd_fops;
	priv->mdev.parent = &pdev->dev;

	err = misc_register(&priv->mdev);
	if (err)
		return err;

	platform_set_drvdata(pdev, priv);
	dev_info(&pdev->dev, "ub-ssd probed cna=%#x mmio=%pR\n",
		 priv->cna, res);
	return 0;
}

static int ub_ssd_remove(struct platform_device *pdev)
{
	struct ub_ssd_priv *priv = platform_get_drvdata(pdev);

	if (priv)
		misc_deregister(&priv->mdev);
	return 0;
}

static const struct of_device_id ub_ssd_of_match[] = {
	{ .compatible = "ub-sim,ssd-v1" },
	{ }
};
MODULE_DEVICE_TABLE(of, ub_ssd_of_match);

static struct platform_driver ub_ssd_driver = {
	.probe	= ub_ssd_probe,
	.remove	= ub_ssd_remove,
	.driver	= {
		.name		= DRIVER_NAME,
		.of_match_table	= ub_ssd_of_match,
	},
};
module_platform_driver(ub_ssd_driver);

MODULE_DESCRIPTION("UB-Attached SSD platform driver");
MODULE_LICENSE("GPL");

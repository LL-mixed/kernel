// SPDX-License-Identifier: GPL-2.0
/*
 * UB-Attached NPU platform driver.
 *
 * Binds via FDT compatible "ub-sim,npu-v1".
 * Provides /dev/ub_npu0 with ioctl for SUBMIT/WAIT/QUERY.
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

#include <uapi/ub/ub_npu.h>

#define DRIVER_NAME "ub-npu"
#define DEV_NAME "ub_npu"

struct ub_npu_priv {
	struct device *dev;
	void __iomem *mmio;
	struct miscdevice mdev;
	uint32_t cna;
};

static int ub_npu_submit(struct ub_npu_priv *priv,
			 struct ub_npu_cmd_v1 __user *ucmd)
{
	struct ub_npu_cmd_v1 kcmd;
	uint32_t status;
	int retries;

	if (copy_from_user(&kcmd, ucmd, sizeof(kcmd)))
		return -EFAULT;

	status = readl(priv->mmio + NPU_STATUS_OFF);
	if (status & (NPU_STATUS_BUSY | NPU_STATUS_COMPLETION_VALID)) {
		/* Clear previous completion */
		writel(1, priv->mmio + NPU_CLEAR_CPL_OFF);
		udelay(10);
	}

	/* Write command slot */
	memcpy_toio(priv->mmio + NPU_CMD_SLOT_OFF, &kcmd, sizeof(kcmd));

	/* Ring doorbell */
	writel(1, priv->mmio + NPU_DOORBELL_OFF);

	/* Wait for completion */
	for (retries = 0; retries < 500000; retries++) {
		status = readl(priv->mmio + NPU_STATUS_OFF);
		if (status & NPU_STATUS_COMPLETION_VALID)
			break;
		udelay(10);
	}

	if (!(status & NPU_STATUS_COMPLETION_VALID))
		return -ETIMEDOUT;

	return 0;
}

static int ub_npu_wait(struct ub_npu_priv *priv,
		       struct ub_npu_cpl_v1 __user *ucpl)
{
	struct ub_npu_cpl_v1 kcpl;
	uint32_t status;

	status = readl(priv->mmio + NPU_STATUS_OFF);
	if (!(status & NPU_STATUS_COMPLETION_VALID))
		return -EAGAIN;

	memcpy_fromio(&kcpl, priv->mmio + NPU_CPL_SLOT_OFF, sizeof(kcpl));

	writel(1, priv->mmio + NPU_CLEAR_CPL_OFF);

	if (copy_to_user(ucpl, &kcpl, sizeof(kcpl)))
		return -EFAULT;

	return 0;
}

static long ub_npu_ioctl(struct file *filp, unsigned int cmd,
			 unsigned long arg)
{
	struct ub_npu_priv *priv =
		container_of(filp->private_data, struct ub_npu_priv, mdev);

	switch (cmd) {
	case UB_NPU_SUBMIT:
		return ub_npu_submit(priv, (struct ub_npu_cmd_v1 __user *)arg);
	case UB_NPU_WAIT:
		return ub_npu_wait(priv, (struct ub_npu_cpl_v1 __user *)arg);
	default:
		return -ENOTTY;
	}
}

static const struct file_operations ub_npu_fops = {
	.owner		= THIS_MODULE,
	.unlocked_ioctl = ub_npu_ioctl,
};

static int ub_npu_probe(struct platform_device *pdev)
{
	struct ub_npu_priv *priv;
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
	priv->mdev.fops = &ub_npu_fops;
	priv->mdev.parent = &pdev->dev;

	err = misc_register(&priv->mdev);
	if (err)
		return err;

	platform_set_drvdata(pdev, priv);
	dev_info(&pdev->dev, "ub-npu probed cna=%#x mmio=%pR\n",
		 priv->cna, res);
	return 0;
}

static int ub_npu_remove(struct platform_device *pdev)
{
	struct ub_npu_priv *priv = platform_get_drvdata(pdev);

	if (priv)
		misc_deregister(&priv->mdev);
	return 0;
}

static const struct of_device_id ub_npu_of_match[] = {
	{ .compatible = "ub-sim,npu-v1" },
	{ }
};
MODULE_DEVICE_TABLE(of, ub_npu_of_match);

static struct platform_driver ub_npu_driver = {
	.probe	= ub_npu_probe,
	.remove	= ub_npu_remove,
	.driver	= {
		.name		= DRIVER_NAME,
		.of_match_table	= ub_npu_of_match,
	},
};
module_platform_driver(ub_npu_driver);

MODULE_DESCRIPTION("UB-Attached NPU platform driver");
MODULE_LICENSE("GPL");

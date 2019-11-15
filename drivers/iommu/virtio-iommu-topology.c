// SPDX-License-Identifier: GPL-2.0
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/acpi.h>
#include <linux/acpi_iort.h>
#include <linux/dma-iommu.h>
#include <linux/iommu.h>
#include <linux/list.h>
#include <linux/pci.h>
#include <linux/printk.h>
#include <linux/virtio_config.h>
#include <linux/virtio_iommu.h>
#include <linux/virtio_pci.h>
#include <uapi/linux/virtio_iommu.h>

struct viommu_cap_config {
	u8 pos; /* PCI capability position */
	u8 bar;
	u32 length; /* structure size */
	u32 offset; /* structure offset within the bar */
};

union viommu_topo_cfg {
	__le16					type;
	struct virtio_iommu_topo_pci_range	pci;
	struct virtio_iommu_topo_endpoint	ep;
};

struct viommu_spec {
	struct device				*dev; /* transport device */
	struct fwnode_handle			*fwnode;
	struct iommu_ops			*ops;
	struct list_head			list;
	size_t					num_items;
	/* The config array of length num_items follows */
	union viommu_topo_cfg			cfg[];
};

struct viommu_pci_ctx {
	struct pci_dev				*pdev;
	int					cfg;
	struct viommu_cap_config		*cap;
};

typedef u32 (*viommu_readl_fn)(void *cookie, u32 offset);

static LIST_HEAD(viommus);
static DEFINE_MUTEX(viommus_lock);

#define VPCI_FIELD(field) offsetof(struct virtio_pci_cap, field)

static inline int viommu_pci_find_capability(struct pci_dev *dev, u8 cfg_type,
					     struct viommu_cap_config *cap)
{
	int pos;
	u8 bar;

	for (pos = pci_find_capability(dev, PCI_CAP_ID_VNDR);
	     pos > 0;
	     pos = pci_find_next_capability(dev, pos, PCI_CAP_ID_VNDR)) {
		u8 type;

		pci_read_config_byte(dev, pos + VPCI_FIELD(cfg_type), &type);
		if (type != cfg_type)
			continue;

		pci_read_config_byte(dev, pos + VPCI_FIELD(bar), &bar);

		/* Ignore structures with reserved BAR values */
		if (type != VIRTIO_PCI_CAP_PCI_CFG && bar > 0x5)
			continue;

		cap->bar = bar;
		cap->pos = pos;
		pci_read_config_dword(dev, pos + VPCI_FIELD(length),
				      &cap->length);
		pci_read_config_dword(dev, pos + VPCI_FIELD(offset),
				      &cap->offset);

		return pos;
	}
	return 0;
}

/*
 * Setup the special virtio PCI capability to read one of the config registers
 */
static int viommu_pci_switch_cfg(struct pci_dev *dev, int cfg,
				 struct viommu_cap_config *cap, u32 length,
				 u32 offset)
{
	offset += cap->offset;

	if (offset + length > cap->offset + cap->length) {
		dev_warn(&dev->dev,
			 "read of %d bytes at offset 0x%x overflows cap of size %d\n",
			 length, offset, cap->length);
		return -EOVERFLOW;
	}

	pci_write_config_byte(dev, cfg + VPCI_FIELD(bar), cap->bar);
	pci_write_config_dword(dev, cfg + VPCI_FIELD(length), length);
	pci_write_config_dword(dev, cfg + VPCI_FIELD(offset), offset);
	return 0;
}

static u32 viommu_pci_readl(void *cookie, u32 offset)
{
	u32 val;
	struct viommu_pci_ctx *ctx = cookie;
	int out = ctx->cfg + sizeof(struct virtio_pci_cap);

	if (viommu_pci_switch_cfg(ctx->pdev, ctx->cfg, ctx->cap, 4,
				  offset))
		return 0;

	pci_read_config_dword(ctx->pdev, out, &val);
	return val;
}

static void viommu_ccopy(viommu_readl_fn readl_fn, void *ctx,
			 u32 *dest, u32 length, u32 offset)
{
	size_t i;

	/* For the moment all our config structures align on 32b */
	if (WARN_ON(length % 4))
		return;

	for (i = 0; i < length / 4; i++, offset += 4)
		dest[i] = readl_fn(ctx, offset);
}

static int viommu_parse_topology(struct device *dev, viommu_readl_fn readl_fn,
				 void *ctx)
{
	size_t i;
	size_t spec_length;
	struct viommu_spec *viommu_spec;
	u32 offset, item_length, num_items;

	offset = readl_fn(ctx, offsetof(struct virtio_iommu_config,
					topo_config.offset));
	item_length = readl_fn(ctx, offsetof(struct virtio_iommu_config,
					     topo_config.item_length));
	num_items = readl_fn(ctx, offsetof(struct virtio_iommu_config,
					   topo_config.num_items));
	if (!offset || !num_items || !item_length)
		return 0;

	spec_length = sizeof(*viommu_spec) + num_items *
		                             sizeof(union viommu_topo_cfg);
	viommu_spec = kzalloc(spec_length, GFP_KERNEL);
	if (!viommu_spec)
		return -ENOMEM;

	viommu_spec->dev = dev;

	/* Copy in the whole array, sort it out later */
	for (i = 0; i < num_items; i++) {
		size_t read_length = min_t(size_t, item_length,
					   sizeof(union viommu_topo_cfg));

		viommu_ccopy(readl_fn, ctx, (void *)&viommu_spec->cfg[i],
			     read_length, offset);

		offset += item_length;
	}
	viommu_spec->num_items = num_items;

	/* TODO: handle device removal */
	mutex_lock(&viommus_lock);
	list_add(&viommu_spec->list, &viommus);
	mutex_unlock(&viommus_lock);

	return 0;
}

static void viommu_pci_parse_topology(struct pci_dev *dev)
{
	int pos;
	u32 features;
	struct viommu_cap_config common = {0};
	struct viommu_cap_config pci_cfg = {0};
	struct viommu_cap_config dev_cfg = {0};
	struct viommu_pci_ctx ctx = {
		.pdev = dev,
	};

	pos = viommu_pci_find_capability(dev, VIRTIO_PCI_CAP_COMMON_CFG, &common);
	if (!pos) {
		dev_warn(&dev->dev, "common capability not found\n");
		return;
	}
	pos = viommu_pci_find_capability(dev, VIRTIO_PCI_CAP_DEVICE_CFG, &dev_cfg);
	if (!pos) {
		dev_warn(&dev->dev, "device config capability not found\n");
		return;
	}
	pos = viommu_pci_find_capability(dev, VIRTIO_PCI_CAP_PCI_CFG, &pci_cfg);
	if (!pos) {
		dev_warn(&dev->dev, "PCI config capability not found\n");
		return;
	}

	/* Find out if the device supports topology description */
	if (viommu_pci_switch_cfg(dev, pos, &common, 4,
				  offsetof(struct virtio_pci_common_cfg,
					   device_feature_select)))
		return;

	/* Select features reg 0 */
	pci_write_config_dword(dev, pos + sizeof(struct virtio_pci_cap), 0);

	if (viommu_pci_switch_cfg(dev, pos, &common, 4,
				  offsetof(struct virtio_pci_common_cfg,
					   device_feature)))
		return;

	pci_read_config_dword(dev, pos + sizeof(struct virtio_pci_cap), &features);
	if (!(features & VIRTIO_IOMMU_F_TOPOLOGY)) {
		dev_dbg(&dev->dev, "device doesn't have topology description");
		return;
	}

	ctx.cfg = pos;
	ctx.cap = &dev_cfg;
	viommu_parse_topology(&dev->dev, viommu_pci_readl, &ctx);
}

DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_REDHAT_QUMRANET, 0x1014,
			viommu_pci_parse_topology);

/*
 * Return true if the device matches this topology structure. Write the endpoint
 * ID into epid if it's the case.
 */
static bool viommu_parse_pci(struct pci_dev *pdev, union viommu_topo_cfg *cfg,
			     u32 *epid)
{
	u16 devid = pci_dev_id(pdev);
	u16 type = le16_to_cpu(cfg->type);

	if (type == VIRTIO_IOMMU_TOPO_PCI_RANGE) {
		u32 start = le32_to_cpu(cfg->pci.requester_start);
		u32 end = le32_to_cpu(cfg->pci.requester_end);
		u32 domain = le32_to_cpu(cfg->pci.hierarchy);
		u32 endpoint_start = le32_to_cpu(cfg->pci.endpoint_start);

		if (pci_domain_nr(pdev->bus) == domain &&
		    devid >= start && devid <= end) {
			*epid = devid - start + endpoint_start;
			return true;
		}
	}
	return false;
}

static const struct iommu_ops *virt_iommu_setup(struct device *dev)
{
	struct iommu_fwspec *fwspec = dev_iommu_fwspec_get(dev);
	const struct iommu_ops *viommu_ops = NULL;
	struct fwnode_handle *viommu_fwnode;
	struct viommu_spec *viommu_spec;
	struct pci_dev *pci_dev = NULL;
	struct device *viommu_dev;
	bool found = false;
	size_t i;
	u32 epid;
	int ret;

	/* Already translated? */
	if (fwspec && fwspec->ops)
		return fwspec->ops;

	if (dev_is_pci(dev)) {
		pci_dev = to_pci_dev(dev);
	} else {
		return false;
	}

	mutex_lock(&viommus_lock);
	list_for_each_entry(viommu_spec, &viommus, list) {
		for (i = 0; i < viommu_spec->num_items; i++) {
			union viommu_topo_cfg *cfg = &viommu_spec->cfg[i];

			found = viommu_parse_pci(pci_dev, cfg, &epid);
			if (found)
				break;
		}
		if (found) {
			viommu_ops = viommu_spec->ops;
			viommu_fwnode = viommu_spec->fwnode;
			viommu_dev = viommu_spec->dev;
			break;
		}
	}
	mutex_unlock(&viommus_lock);
	if (!found)
		return NULL;

	/* We're not translating ourselves, that would be silly. */
	if (viommu_dev == dev)
		return NULL;

	if (!viommu_ops)
		return ERR_PTR(-EPROBE_DEFER);

	ret = iommu_fwspec_init(dev, viommu_fwnode, viommu_ops);
	if (ret)
		return ERR_PTR(ret);

	iommu_fwspec_add_ids(dev, &epid, 1);

	return viommu_ops;
}

/**
 * virt_dma_configure - Configure DMA of virtualized devices
 * @dev: the endpoint
 *
 * An alternative to the ACPI and DT methods to setup DMA and the IOMMU ops of a
 * virtual device.
 *
 * Return: -EPROBE_DEFER if the IOMMU hasn't been loaded yet, 0 otherwise
 */
int virt_dma_configure(struct device *dev)
{
	const struct iommu_ops *iommu_ops;

	/* TODO: do we need to mess about with the dma_mask as well? */
	WARN_ON(!dev->dma_mask);

	iommu_ops = virt_iommu_setup(dev);
	if (IS_ERR(iommu_ops)) {
		if (PTR_ERR(iommu_ops) == -EPROBE_DEFER)
			return -EPROBE_DEFER;
		iommu_ops = NULL;
	}

	/*
	 * If we have reason to believe the IOMMU driver missed the initial
	 * add_device callback for dev, replay it to get things in order.
	 */
	if (iommu_ops && dev->bus && !device_iommu_mapped(dev))
		iommu_probe_device(dev);

#ifdef CONFIG_ARCH_HAS_SETUP_DMA_OPS
	/* Assume coherent, as well as full 64-bit addresses. */
	arch_setup_dma_ops(dev, 0, ~0UL, iommu_ops, true);
#else
	if (iommu_ops)
		iommu_setup_dma_ops(dev, 0, ~0UL);
#endif
	return 0;
}

/**
 * virt_set_iommu_ops - Set the IOMMU ops of a virtual IOMMU device
 *
 * Setup the iommu_ops associated to a viommu_spec, once the driver is loaded
 * and the device probed.
 */
void virt_set_iommu_ops(struct device *dev, struct iommu_ops *ops)
{
	struct viommu_spec *viommu_spec;

	mutex_lock(&viommus_lock);
	list_for_each_entry(viommu_spec, &viommus, list) {
		if (viommu_spec->dev == dev) {
			viommu_spec->ops = ops;
			viommu_spec->fwnode = ops ? dev->fwnode : NULL;
			break;
		}
	}
	mutex_unlock(&viommus_lock);
}
EXPORT_SYMBOL_GPL(virt_set_iommu_ops);

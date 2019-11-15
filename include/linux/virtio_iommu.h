/* SPDX-License-Identifier: GPL-2.0 */
#ifndef VIRTIO_IOMMU_H_
#define VIRTIO_IOMMU_H_

#if IS_ENABLED(CONFIG_VIRTIO_IOMMU_TOPOLOGY)
int virt_dma_configure(struct device *dev);
void virt_set_iommu_ops(struct device *dev, struct iommu_ops *ops);
#else /* !CONFIG_VIRTIO_IOMMU_TOPOLOGY */
static inline int virt_dma_configure(struct device *dev)
{
	return -ENODEV;
}

static inline void virt_set_iommu_ops(struct device *dev, struct iommu_ops *ops)
{ }
#endif /* !CONFIG_VIRTIO_IOMMU_TOPOLOGY */

#endif /* VIRTIO_IOMMU_H_ */

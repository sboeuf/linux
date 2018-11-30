/*
 * Generic Event Device for ACPI.
 *
 * Copyright (c) 2016, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * Generic Event Device allows platforms to handle interrupts in ACPI
 * ASL statements. It follows very similar to  _EVT method approach
 * from GPIO events. All interrupts are listed in _CRS and the handler
 * is written in _EVT method. Here is an example.
 *
 * Device (GED0)
 * {
 *
 *     Name (_HID, "ACPI0013")
 *     Name (_UID, 0)
 *     Method (_CRS, 0x0, Serialized)
 *     {
 *		Name (RBUF, ResourceTemplate ()
 *		{
 *		Interrupt(ResourceConsumer, Edge, ActiveHigh, Shared, , , )
 *		{123}
 *		}
 *     })
 *
 *     Method (_EVT, 1) {
 *             if (Lequal(123, Arg0))
 *             {
 *             }
 *     }
 * }
 *
 */

#include <linux/err.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/list.h>
#include <linux/platform_device.h>
#include <linux/acpi.h>
#include <linux/msi.h>
#include <linux/irq.h>
#include <linux/delay.h>
#include <asm/apic.h>
#include <asm/msidef.h>

#define MODULE_NAME	"acpi-ged"

struct acpi_ged_device {
	struct device *dev;
	struct list_head event_list;
};

struct acpi_ged_event {
	struct list_head node;
	struct device *dev;
	unsigned int gsi;
	unsigned int irq;
	acpi_handle handle;
};

/*
 * MSI related functions
 */

void ged_msi_unmask(struct irq_data *data)
{
	pr_debug("%s\n", __func__);
}

void ged_msi_mask(struct irq_data *data)
{
	pr_debug("%s\n", __func__);
}

static void ged_msi_write_msg(struct irq_data *data, struct msi_msg *msg)
{
	struct irq_alloc_info *info;
	struct acpi_ged_event *ev;
	u64 msi_addr;
	struct {
		struct acpi_resource res;
		struct acpi_resource end;
	} *resource;
	struct acpi_buffer buffer = { 0, NULL };

	pr_debug("%s\n", __func__);

	if (!data || !data->chip_data) {
		pr_err("%s: missing chip_data", __func__);
		return;
	}
	info = (struct irq_alloc_info *) data->chip_data;

	if (!info->data) {
		pr_err("%s: missing data", __func__);
		return;
	}
	ev = (struct acpi_ged_event *) info->data;

	pr_debug("%s: address_lo = %x\taddress_hi = %x\tdata = %x\n",
		 __func__, msg->address_lo, msg->address_hi, msg->data);

	/* Build the resource */
	msi_addr = ((u64)msg->address_hi << 32) | ((u64)msg->address_lo & 0xffffffff);

	resource = kzalloc(sizeof(*resource) + 1, irqs_disabled() ? GFP_ATOMIC: GFP_KERNEL);
	if (!resource) {
		pr_err("%s: failed to allocate memory", __func__);
		return;
	}

	buffer.length = sizeof(*resource) + 1;
	buffer.pointer = resource;

	resource->res.type = ACPI_RESOURCE_TYPE_MSI_IRQ;
	resource->res.length = sizeof(struct acpi_resource);
	resource->res.data.msi_irq.addr_min = msi_addr;
	resource->res.data.msi_irq.addr_max = msi_addr;
	resource->res.data.msi_irq.data_min = msg->data;
	resource->res.data.msi_irq.data_max = msg->data;
	resource->res.data.msi_irq.tag = ev->gsi;

	resource->end.type = ACPI_RESOURCE_TYPE_END_TAG;
	resource->end.length = sizeof(struct acpi_resource);

	/* Set the resource by evaluating _SRS */
	if (ACPI_FAILURE(acpi_set_current_resources(ACPI_HANDLE(ev->dev), &buffer))) {
		pr_err("%s: failed to evaluate _SRS", __func__);
		goto end;
	}

      end:
	kfree(resource);
}

static void ged_msi_compose_msg(struct irq_data *data, struct msi_msg *msg)
{
	struct irq_cfg *cfg;

	pr_debug("%s\n", __func__);

	cfg = irqd_cfg(data);

	msg->address_hi = MSI_ADDR_BASE_HI;

	if (x2apic_enabled())
		msg->address_hi |= MSI_ADDR_EXT_DEST_ID(cfg->dest_apicid);

	msg->address_lo =
		MSI_ADDR_BASE_LO |
		((apic->irq_dest_mode == 0) ?
			MSI_ADDR_DEST_MODE_PHYSICAL :
			MSI_ADDR_DEST_MODE_LOGICAL) |
		MSI_ADDR_REDIRECTION_CPU |
		MSI_ADDR_DEST_ID(cfg->dest_apicid);

	msg->data =
		MSI_DATA_TRIGGER_EDGE |
		MSI_DATA_LEVEL_ASSERT |
		MSI_DATA_DELIVERY_FIXED |
		MSI_DATA_VECTOR(cfg->vector);
}

static struct irq_chip ged_msi_controller __ro_after_init = {
	.name			= "GED-MSI",
	.irq_unmask		= ged_msi_unmask,
	.irq_mask		= ged_msi_mask,
	.irq_ack		= irq_chip_ack_parent,
	.irq_set_affinity	= msi_domain_set_affinity,
	.irq_retrigger		= irq_chip_retrigger_hierarchy,
	.irq_compose_msi_msg	= ged_msi_compose_msg,
	.irq_write_msi_msg	= ged_msi_write_msg,
	.flags			= IRQCHIP_SKIP_SET_WAKE,
};

static irq_hw_number_t ged_msi_get_hwirq(struct msi_domain_info *info,
					 msi_alloc_info_t *arg)
{
	struct acpi_ged_event *ev;

	pr_debug("%s\n", __func__);

	ev = (struct acpi_ged_event *) arg->data;
	return ev->gsi;
}

static int ged_msi_init(struct irq_domain *domain,
			struct msi_domain_info *info, unsigned int virq,
			irq_hw_number_t hwirq, msi_alloc_info_t *arg)
{
	pr_debug("%s: virq %d\t hwirq %lu\n", __func__, virq, hwirq);
	irq_set_status_flags(virq, IRQ_TYPE_EDGE_BOTH | IRQ_LEVEL);
	irq_domain_set_info(domain, virq, hwirq, info->chip, arg,
			    handle_edge_irq, NULL, "edge");

	return 0;
}

static void ged_msi_free(struct irq_domain *domain,
			 struct msi_domain_info *info, unsigned int virq)
{
	pr_debug("%s\n", __func__);
	irq_clear_status_flags(virq, IRQ_TYPE_EDGE_BOTH | IRQ_LEVEL);
}

static struct msi_domain_ops ged_msi_domain_ops = {
	.get_hwirq	= ged_msi_get_hwirq,
	.msi_init	= ged_msi_init,
	.msi_free	= ged_msi_free,
};

static struct msi_domain_info ged_msi_domain_info = {
	.ops	= &ged_msi_domain_ops,
	.chip	= &ged_msi_controller,
};

static struct irq_domain *ged_create_msi_domain(uint64_t msi_id)
{
	struct fwnode_handle *fn;
	struct irq_domain *domain;

	pr_debug("%s\n", __func__);

	/* Create IRQ domain handler */
	fn = irq_domain_alloc_named_id_fwnode(ged_msi_controller.name,
					      msi_id);
	if (!fn)
		return NULL;

	/* Create platform MSI domain */
	domain = msi_create_irq_domain(fn,
				       &ged_msi_domain_info,
				       x86_vector_domain);
	irq_domain_free_fwnode(fn);
	return domain;
}

static irqreturn_t acpi_ged_irq_handler(int irq, void *data)
{
	struct acpi_ged_event *event = data;
	acpi_status acpi_ret;

	dev_dbg(event->dev, "%s: IRQ = %d\n", __func__, irq);

	acpi_ret = acpi_execute_simple_method(event->handle, NULL, event->gsi);
	if (ACPI_FAILURE(acpi_ret))
		dev_err_once(event->dev, "IRQ method execution failed\n");

	return IRQ_HANDLED;
}

static acpi_status acpi_ged_request_interrupt(struct acpi_resource *ares,
					      void *context)
{
	struct acpi_ged_event *event;
	unsigned int irq;
	int virq;
	unsigned int gsi = 0;
	unsigned int index = 0;
	unsigned int irqflags = IRQF_ONESHOT;
	struct acpi_ged_device *geddev = context;
	struct device *dev = geddev->dev;
	acpi_handle handle = ACPI_HANDLE(dev);
	acpi_handle evt_handle;
	struct resource r;
	struct irq_alloc_info info;

	dev_dbg(dev, "%s\n", __func__);

	if (ares->type == ACPI_RESOURCE_TYPE_END_TAG)
		return AE_OK;

	if (!acpi_dev_resource_interrupt(ares, index, &r)) {
		dev_err(dev, "unable to parse IRQ resource\n");
		return AE_ERROR;
	}

	switch (ares->type) {
	case ACPI_RESOURCE_TYPE_IRQ:
		gsi = ares->data.irq.interrupts[index];
		break;
	case ACPI_RESOURCE_TYPE_EXTENDED_IRQ:
		gsi = ares->data.extended_irq.interrupts[index];
		break;
	case ACPI_RESOURCE_TYPE_MSI_IRQ:
		/* Check if ACPI provides MSI support */
		if (!acpi_has_method(ACPI_HANDLE(dev), METHOD_NAME__SRS))
			return AE_ERROR;

		gsi = ares->data.msi_irq.tag;

		if (!dev->msi_domain) {
			/* Create MSI domain */
			dev->msi_domain = ged_create_msi_domain(gsi);
			if (!dev->msi_domain)
				return AE_ERROR;
		}

		break;
	}

	irq = r.start;

	if (ACPI_FAILURE(acpi_get_handle(handle, "_EVT", &evt_handle))) {
		dev_err(dev, "cannot locate _EVT method\n");
		return AE_ERROR;
	}

	event = devm_kzalloc(dev, sizeof(*event), GFP_KERNEL);
	if (!event)
		return AE_ERROR;

	event->gsi = gsi;
	event->dev = dev;
	event->handle = evt_handle;

	/* Use MSI if the MSI domain does exist */
	if (dev->msi_domain) {
		init_irq_alloc_info(&info, NULL);
		info.data = event;
		virq = irq_domain_alloc_irqs(dev->msi_domain, 1, NUMA_NO_NODE,
					     &info);
		if (virq < 0)
			return AE_ERROR;

		irq = (unsigned int) virq;
	}

	/* Update IRQ number */
	event->irq = irq;

	if (r.flags & IORESOURCE_IRQ_SHAREABLE)
		irqflags |= IRQF_SHARED;

	if (request_threaded_irq(irq, NULL, acpi_ged_irq_handler,
				 irqflags, "ACPI:Ged", event)) {
		dev_err(dev, "failed to setup event handler for irq %u\n", irq);
		return AE_ERROR;
	}

	dev_dbg(dev, "GED listening GSI %u @ IRQ %u\n", gsi, irq);
	list_add_tail(&event->node, &geddev->event_list);
	return AE_OK;
}

static int ged_probe(struct platform_device *pdev)
{
	struct acpi_ged_device *geddev;
	acpi_status acpi_ret;
	struct device *dev = &pdev->dev;

	geddev = devm_kzalloc(dev, sizeof(*geddev), GFP_KERNEL);
	if (!geddev)
		return -ENOMEM;

	geddev->dev = dev;
	INIT_LIST_HEAD(&geddev->event_list);

	/* Initialise IRQ for each Interrupt() resource listed from DSDT */
	acpi_ret = acpi_walk_resources(ACPI_HANDLE(dev), METHOD_NAME__CRS,
				       acpi_ged_request_interrupt, geddev);
	if (ACPI_FAILURE(acpi_ret)) {
		dev_err(dev, "unable to parse the %s record\n",
			METHOD_NAME__CRS);
		return -EINVAL;
	}
	platform_set_drvdata(pdev, geddev);

	return 0;
}

static void ged_shutdown(struct platform_device *pdev)
{
	struct acpi_ged_device *geddev = platform_get_drvdata(pdev);
	struct device *dev = &pdev->dev;
	struct acpi_ged_event *event, *next;

	if (dev->msi_domain)
		msi_domain_free_irqs(dev->msi_domain, dev);

	list_for_each_entry_safe(event, next, &geddev->event_list, node) {
		if (!dev->msi_domain)
			free_irq(event->irq, event);

		list_del(&event->node);
		dev_dbg(dev, "GED releasing GSI %u @ IRQ %u\n",
			 event->gsi, event->irq);
	}
}

static int ged_remove(struct platform_device *pdev)
{
	ged_shutdown(pdev);
	return 0;
}

static const struct acpi_device_id ged_acpi_ids[] = {
	{"ACPI0013"},
	{},
};

static struct platform_driver ged_driver = {
	.probe = ged_probe,
	.remove = ged_remove,
	.shutdown = ged_shutdown,
	.driver = {
		.name = MODULE_NAME,
		.acpi_match_table = ACPI_PTR(ged_acpi_ids),
	},
};
builtin_platform_driver(ged_driver);

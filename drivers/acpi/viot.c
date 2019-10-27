// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2019 Linaro
 *
 * Virtual IOMMU table
 */
#define pr_fmt(fmt)	"ACPI: VIOT: " fmt

#include <linux/acpi.h>
#include <linux/acpi_iort.h>
#include <linux/acpi_viot.h>

int __init acpi_viot_init(void)
{
	struct acpi_table_viot *viot;
	struct acpi_table_header *acpi_header;
	acpi_status status;

	status = acpi_get_table(ACPI_SIG_VIOT, 0, &acpi_header);
	if (ACPI_FAILURE(status)) {
		if (status != AE_NOT_FOUND) {
			const char *msg = acpi_format_exception(status);

			pr_err("Failed to get table, %s\n", msg);
			return -EINVAL;
		}

		return 0;
	}

	if (acpi_header->length < sizeof(*viot)) {
		pr_err("VIOT table overflow, bad table!\n");
		return -EINVAL;
	}

	viot = (struct acpi_table_viot *)acpi_header;
	if (ACPI_COMPARE_NAMESEG(viot->base_table.signature, ACPI_SIG_IORT)) {
		acpi_iort_register_table(&viot->base_table, IORT_SOURCE_VIOT);
		return 0;
	}

	pr_err("Unknown base table header\n");
	return -EINVAL;
}

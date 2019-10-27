/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2019 Linaro
 */

#ifndef __ACPI_VIOT_H__
#define __ACPI_VIOT_H__

#ifdef CONFIG_ACPI_VIOT

int acpi_viot_init(void);

#else /* !CONFIG_ACPI_VIOT */

static inline int acpi_viot_init(void)
{ return 0; }

#endif /* !CONFIG_ACPI_VIOT */

#endif /* __ACPI_VIOT_H__ */

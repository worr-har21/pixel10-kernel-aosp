// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2024 Google LLC
 */

#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/types.h>

#include <soc/google/goog-mba-gdmc-iface.h>
#include <soc/google/goog_gdmc_service_ids.h>
#include <soc/google/goog_mba_nq_xport.h>

struct gdmc_dhub {
	struct gdmc_iface *gdmc_iface;
};

struct uart_attribute {
	struct device_attribute dev_attr;
	u32 num;
};

static inline struct uart_attribute *to_uart_attr(struct device_attribute *attr)
{
	return container_of(attr, struct uart_attribute, dev_attr);
}

struct uart_id_name {
	u8 id;
	const char *name;
};

static const struct uart_id_name uart_list[] = {
	{ 0, "none" },
	{ 1, "cpm" },
	{ 2, "apc" },
	{ 3, "aoc" },
	{ 4, "bmsm" },
	{ 5, "gsa" },
	{ 6, "ispfe" },
	{ 7, "aurdsp" },
	{ 8, "tpu" },
	{ 9, "modem" },
	{ 10, "gsc" },
	{ 11, "gdmc" },
	{ 255, "virt" },
};

/*
 * Converts uart name into a uart id
 * Returns -EINVAL if the name is not valid.
 **/
static int google_uart_name_to_id(const char *name)
{
	for (int i = 0; i < ARRAY_SIZE(uart_list); i++) {
		if (sysfs_streq(name, uart_list[i].name))
			return uart_list[i].id;
	}

	return -EINVAL;
}

/*
 * Converts uart id into a uart name
 * Returns -EINVAL if the id is not valid.
 **/
static const char *google_uart_id_to_name(u32 id)
{
	for (int i = 0; i < ARRAY_SIZE(uart_list); i++) {
		if (id == uart_list[i].id)
			return uart_list[i].name;
	}

	return ERR_PTR(-EINVAL);
}

static ssize_t baudrate_show(struct device *dev,
			     struct device_attribute *attr,
			     char *buf)
{
	struct gdmc_dhub *gdmc_dhub = dev_get_drvdata(dev);
	struct uart_attribute *uart_attr = to_uart_attr(attr);
	u32 uart_num = uart_attr->num;
	int ret;
	u32 baudrate;

	ret = gdmc_dhub_uart_baudrate_get(gdmc_dhub->gdmc_iface, uart_num, &baudrate);
	if (ret < 0)
		return ret;

	return sysfs_emit(buf, "%u\n", baudrate);
}

static ssize_t baudrate_store(struct device *dev,
			      struct device_attribute *attr,
			      const char *buf,
			      size_t count)
{
	struct gdmc_dhub *gdmc_dhub = dev_get_drvdata(dev);
	struct uart_attribute *uart_attr = to_uart_attr(attr);
	u32 uart_num = uart_attr->num;
	int ret;
	u32 baudrate;

	ret = kstrtou32(buf, 0, &baudrate);
	if (ret != 0)
		return ret;

	ret = gdmc_dhub_uart_baudrate_set(gdmc_dhub->gdmc_iface, uart_num, baudrate);
	if (ret)
		return ret;

	return count;
}

static inline u32 uart_num_to_bit(u32 uart_num)
{
	if (uart_num < GDMC_MBA_DHUB_UART_ID_CPM ||
	    uart_num > GDMC_MBA_DHUB_UART_ID_GDMC)
		return 0;
	return BIT(uart_num);
}

static ssize_t virt_en_show(struct device *dev,
			    struct device_attribute *attr,
			    char *buf)
{
	struct gdmc_dhub *gdmc_dhub = dev_get_drvdata(dev);
	struct uart_attribute *uart_attr = to_uart_attr(attr);
	u32 uart_num = uart_attr->num;
	int ret;
	u32 mask;

	ret = gdmc_dhub_virt_en_get(gdmc_dhub->gdmc_iface, &mask);
	if (ret < 0)
		return ret;

	mask &= uart_num_to_bit(uart_num);

	return sysfs_emit(buf, "%u\n", !!mask);
}

static ssize_t virt_en_store(struct device *dev,
			     struct device_attribute *attr,
			     const char *buf,
			     size_t count)
{
	struct gdmc_dhub *gdmc_dhub = dev_get_drvdata(dev);
	struct uart_attribute *uart_attr = to_uart_attr(attr);
	u32 uart_num = uart_attr->num;
	int ret;
	bool enable;
	u32 mask, bit;

	ret = kstrtobool(buf, &enable);
	if (ret != 0)
		return ret;

	ret = gdmc_dhub_virt_en_get(gdmc_dhub->gdmc_iface, &mask);
	if (ret < 0)
		return ret;

	bit = uart_num_to_bit(uart_num);
	if (enable)
		mask |= bit;
	else
		mask &= ~bit;

	/* Enable bit must be clear for valid command */
	mask &= ~GDMC_MBA_DHUB_VIRT_MASK_EN;

	ret = gdmc_dhub_virt_en_set(gdmc_dhub->gdmc_iface, mask);
	if (ret)
		return ret;

	return count;
}

#define __UART_ATTR_RW(_name, _num) {					\
	.dev_attr = __ATTR_RW(_name),					\
	.num = _num,							\
}

#define UART_ATTR_RW(_name, _uart, _ID)					\
	struct uart_attribute uart_attr_##_uart##_##_name =		\
		__UART_ATTR_RW(_name, GDMC_MBA_DHUB_UART_ID_##_ID)

#define UART_ATTR_GROUP(_uart, _ID)					\
	UART_ATTR_RW(baudrate, _uart, _ID);				\
	UART_ATTR_RW(virt_en, _uart, _ID);				\
	static struct attribute *_uart##_attrs[] = {			\
		&uart_attr_##_uart##_baudrate.dev_attr.attr,		\
		&uart_attr_##_uart##_virt_en.dev_attr.attr,		\
		NULL,							\
	};								\
	static struct attribute_group _uart##_attr_group = {		\
		.attrs = _uart##_attrs,					\
		.name = #_uart,						\
	}

UART_ATTR_GROUP(cpm, CPM);
UART_ATTR_GROUP(apc, APC);
UART_ATTR_GROUP(aoc, AOC);
UART_ATTR_GROUP(bmsm, BMSM);
UART_ATTR_GROUP(gsa, GSA);
UART_ATTR_GROUP(ispfe, ISPFE);
UART_ATTR_GROUP(aurdsp, AURDSP);
UART_ATTR_GROUP(tpu, TPU);
UART_ATTR_GROUP(modem, MODEM);
UART_ATTR_GROUP(gsc, GSC);
UART_ATTR_GROUP(gdmc, GDMC);

static ssize_t uart_mux_show(struct device *dev,
			     struct device_attribute *attr,
			     char *buf)
{
	struct gdmc_dhub *gdmc_dhub = dev_get_drvdata(dev);
	int ret;
	u32 uart_id;
	const char *name;

	ret = gdmc_dhub_uart_mux_get(gdmc_dhub->gdmc_iface, &uart_id);
	if (ret < 0)
		return ret;

	name = google_uart_id_to_name(uart_id);
	if (IS_ERR(name))
		return PTR_ERR(name);

	return sysfs_emit(buf, "%s\n", name);
}

static ssize_t uart_mux_store(struct device *dev,
			      struct device_attribute *attr,
			      const char *buf,
			      size_t count)
{
	struct gdmc_dhub *gdmc_dhub = dev_get_drvdata(dev);
	int ret;
	int uart_id = google_uart_name_to_id(buf);

	if (uart_id < 0)
		return -EINVAL;

	ret = gdmc_dhub_uart_mux_set(gdmc_dhub->gdmc_iface, uart_id);
	if (ret)
		return ret;

	return count;
}

DEVICE_ATTR_RW(uart_mux);
UART_ATTR_RW(baudrate, sbu, VIRTUALIZED);

static struct attribute *dhub_attrs[] = {
	&dev_attr_uart_mux.attr,
	&uart_attr_sbu_baudrate.dev_attr.attr,
	NULL,
};

static const struct attribute_group root_attr_group = {
	.attrs = dhub_attrs,
};

static const struct attribute_group *attr_groups[] = {
	&root_attr_group,
	&cpm_attr_group,
	&apc_attr_group,
	&aoc_attr_group,
	&bmsm_attr_group,
	&gsa_attr_group,
	&ispfe_attr_group,
	&aurdsp_attr_group,
	&tpu_attr_group,
	&modem_attr_group,
	&gsc_attr_group,
	&gdmc_attr_group,
	NULL,
};

static int google_gdmc_dhub_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct gdmc_dhub *gdmc_dhub;
	int ret;

	gdmc_dhub = devm_kzalloc(dev, sizeof(*gdmc_dhub), GFP_KERNEL);
	if (!gdmc_dhub)
		return -ENOMEM;
	platform_set_drvdata(pdev, gdmc_dhub);

	gdmc_dhub->gdmc_iface = gdmc_iface_get(dev);
	if (IS_ERR(gdmc_dhub->gdmc_iface))
		return PTR_ERR(gdmc_dhub->gdmc_iface);

	ret = devm_device_add_groups(dev, attr_groups);
	if (ret)
		goto put_gdmc_iface;

	return 0;

put_gdmc_iface:
	gdmc_iface_put(gdmc_dhub->gdmc_iface);
	return ret;
}

static int google_gdmc_dhub_remove(struct platform_device *pdev)
{
	struct gdmc_dhub *gdmc_dhub = platform_get_drvdata(pdev);

	gdmc_iface_put(gdmc_dhub->gdmc_iface);

	return 0;
}

static const struct of_device_id google_gdmc_dhub_of_match_table[] = {
	{ .compatible = "google,gdmc-dhub" },
	{},
};
MODULE_DEVICE_TABLE(of, google_gdmc_dhub_of_match_table);

static struct platform_driver google_gdmc_dhub_driver = {
	.probe = google_gdmc_dhub_probe,
	.remove = google_gdmc_dhub_remove,
	.driver = {
		.name = "google-gdmc-dhub",
		.owner = THIS_MODULE,
		.of_match_table = of_match_ptr(google_gdmc_dhub_of_match_table),
	},
};
module_platform_driver(google_gdmc_dhub_driver);

MODULE_AUTHOR("Google LLC");
MODULE_DESCRIPTION("Google GDMC DHUB");
MODULE_LICENSE("GPL");

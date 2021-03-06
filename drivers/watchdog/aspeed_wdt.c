/*
 * Copyright 2016 IBM Corporation
 *
 * Joel Stanley <joel@jms.id.au>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */

#include <linux/delay.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/watchdog.h>

struct aspeed_wdt {
	struct watchdog_device	wdd;
	void __iomem		*base;
	u32			ctrl;
};

static const struct of_device_id aspeed_wdt_of_table[] = {
	{ .compatible = "aspeed,ast2400-wdt" },
	{ .compatible = "aspeed,ast2500-wdt" },
	{ },
};
MODULE_DEVICE_TABLE(of, aspeed_wdt_of_table);

#define WDT_STATUS		0x00
#define WDT_RELOAD_VALUE	0x04
#define WDT_RESTART		0x08
#define WDT_CTRL		0x0C
#define   WDT_CTRL_RESET_MODE_SOC	(0x00 << 5)
#define   WDT_CTRL_RESET_MODE_FULL_CHIP	(0x01 << 5)
#define   WDT_CTRL_RESET_MODE_ARM_CPU	(0x10 << 5)
#define   WDT_CTRL_1MHZ_CLK		BIT(4)
#define   WDT_CTRL_WDT_EXT		BIT(3)
#define   WDT_CTRL_WDT_INTR		BIT(2)
#define   WDT_CTRL_RESET_SYSTEM		BIT(1)
#define   WDT_CTRL_ENABLE		BIT(0)
#define WDT_RESET_WIDTH		0x18
#define   WDT_RESET_WIDTH_ACTIVE_HIGH	BIT(31)
#define     WDT_ACTIVE_HIGH_MAGIC	(0xA5 << 24)
#define     WDT_ACTIVE_LOW_MAGIC	(0x5A << 24)
#define   WDT_RESET_WIDTH_PUSH_PULL	BIT(30)
#define     WDT_PUSH_PULL_MAGIC		(0xA8 << 24)
#define     WDT_OPEN_DRAIN_MAGIC	(0x8A << 24)
#define   WDT_RESET_WIDTH_DURATION	0xFFF

#define WDT_RESTART_MAGIC	0x4755

/* 32 bits at 1MHz, in milliseconds */
#define WDT_MAX_TIMEOUT_MS	4294967
#define WDT_DEFAULT_TIMEOUT	30
#define WDT_RATE_1MHZ		1000000

static struct aspeed_wdt *to_aspeed_wdt(struct watchdog_device *wdd)
{
	return container_of(wdd, struct aspeed_wdt, wdd);
}

static void aspeed_wdt_enable(struct aspeed_wdt *wdt, int count)
{
	wdt->ctrl |= WDT_CTRL_ENABLE;

	writel(0, wdt->base + WDT_CTRL);
	writel(count, wdt->base + WDT_RELOAD_VALUE);
	writel(WDT_RESTART_MAGIC, wdt->base + WDT_RESTART);
	writel(wdt->ctrl, wdt->base + WDT_CTRL);
}

static int aspeed_wdt_start(struct watchdog_device *wdd)
{
	struct aspeed_wdt *wdt = to_aspeed_wdt(wdd);

	aspeed_wdt_enable(wdt, wdd->timeout * WDT_RATE_1MHZ);

	return 0;
}

static int aspeed_wdt_stop(struct watchdog_device *wdd)
{
	struct aspeed_wdt *wdt = to_aspeed_wdt(wdd);

	wdt->ctrl &= ~WDT_CTRL_ENABLE;
	writel(wdt->ctrl, wdt->base + WDT_CTRL);

	return 0;
}

static int aspeed_wdt_ping(struct watchdog_device *wdd)
{
	struct aspeed_wdt *wdt = to_aspeed_wdt(wdd);

	writel(WDT_RESTART_MAGIC, wdt->base + WDT_RESTART);

	return 0;
}

static int aspeed_wdt_set_timeout(struct watchdog_device *wdd,
				  unsigned int timeout)
{
	struct aspeed_wdt *wdt = to_aspeed_wdt(wdd);
	u32 actual;

	wdd->timeout = timeout;

	actual = min(timeout, wdd->max_hw_heartbeat_ms * 1000);

	writel(actual * WDT_RATE_1MHZ, wdt->base + WDT_RELOAD_VALUE);
	writel(WDT_RESTART_MAGIC, wdt->base + WDT_RESTART);

	return 0;
}

static int aspeed_wdt_restart(struct watchdog_device *wdd,
			      unsigned long action, void *data)
{
	struct aspeed_wdt *wdt = to_aspeed_wdt(wdd);

	aspeed_wdt_enable(wdt, 128 * WDT_RATE_1MHZ / 1000);

	mdelay(1000);

	return 0;
}

static const struct watchdog_ops aspeed_wdt_ops = {
	.start		= aspeed_wdt_start,
	.stop		= aspeed_wdt_stop,
	.ping		= aspeed_wdt_ping,
	.set_timeout	= aspeed_wdt_set_timeout,
	.restart	= aspeed_wdt_restart,
	.owner		= THIS_MODULE,
};

static const struct watchdog_info aspeed_wdt_info = {
	.options	= WDIOF_KEEPALIVEPING
			| WDIOF_MAGICCLOSE
			| WDIOF_SETTIMEOUT,
	.identity	= KBUILD_MODNAME,
};

static int aspeed_wdt_remove(struct platform_device *pdev)
{
	struct aspeed_wdt *wdt = platform_get_drvdata(pdev);

	watchdog_unregister_device(&wdt->wdd);

	return 0;
}

static int aspeed_wdt_probe(struct platform_device *pdev)
{
	struct aspeed_wdt *wdt;
	struct resource *res;
	struct device_node *np;
	const char *reset_type;
	u32 duration;
	int ret;

	wdt = devm_kzalloc(&pdev->dev, sizeof(*wdt), GFP_KERNEL);
	if (!wdt)
		return -ENOMEM;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	wdt->base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(wdt->base))
		return PTR_ERR(wdt->base);

	/*
	 * The ast2400 wdt can run at PCLK, or 1MHz. The ast2500 only
	 * runs at 1MHz. We chose to always run at 1MHz, as there's no
	 * good reason to have a faster watchdog counter.
	 */
	wdt->wdd.info = &aspeed_wdt_info;
	wdt->wdd.ops = &aspeed_wdt_ops;
	wdt->wdd.max_hw_heartbeat_ms = WDT_MAX_TIMEOUT_MS;
	wdt->wdd.parent = &pdev->dev;

	wdt->wdd.timeout = WDT_DEFAULT_TIMEOUT;
	watchdog_init_timeout(&wdt->wdd, 0, &pdev->dev);

	wdt->ctrl = WDT_CTRL_1MHZ_CLK;

	/*
	 * Control reset on a per-device basis to ensure the
	 * host is not affected by a BMC reboot
	 */
	np = pdev->dev.of_node;
	ret = of_property_read_string(np, "aspeed,reset-type", &reset_type);
	if (ret) {
		wdt->ctrl |= WDT_CTRL_RESET_SYSTEM;
	} else {
		if (!strcmp(reset_type, "cpu"))
			wdt->ctrl |= WDT_CTRL_RESET_MODE_ARM_CPU;
		else if (!strcmp(reset_type, "soc"))
			wdt->ctrl |= WDT_CTRL_RESET_MODE_SOC;
		else if (!strcmp(reset_type, "system"))
			wdt->ctrl |= WDT_CTRL_RESET_SYSTEM;
	}
	if (of_property_read_bool(np, "aspeed,external-signal"))
		wdt->ctrl |= WDT_CTRL_WDT_EXT;

	if (readl(wdt->base + WDT_CTRL) & WDT_CTRL_ENABLE)  {
		/*
		 * The watchdog is running, but invoke aspeed_wdt_start() to
		 * write wdt->ctrl to WDT_CTRL to ensure the watchdog's
		 * configuration conforms to the driver's expectations.
		 * Primarily, ensure we're using the 1MHz clock source.
		 */
		aspeed_wdt_start(&wdt->wdd);
		set_bit(WDOG_HW_RUNNING, &wdt->wdd.status);
	}

	if (of_device_is_compatible(np, "aspeed,ast2500-wdt")) {
		u32 reg = readl(wdt->base + WDT_RESET_WIDTH);

		reg &= WDT_RESET_WIDTH_DURATION;
		if (of_property_read_bool(np, "aspeed,ext-push-pull"))
			reg |= WDT_PUSH_PULL_MAGIC;
		else
			reg |= WDT_OPEN_DRAIN_MAGIC;

		writel(reg, wdt->base + WDT_RESET_WIDTH);

		reg &= WDT_RESET_WIDTH_DURATION;
		if (of_property_read_bool(np, "aspeed,ext-active-high"))
			reg |= WDT_ACTIVE_HIGH_MAGIC;
		else
			reg |= WDT_ACTIVE_LOW_MAGIC;

		writel(reg, wdt->base + WDT_RESET_WIDTH);
	}

	if (!of_property_read_u32(np, "aspeed,ext-pulse-duration", &duration)) {
		if (duration > WDT_RESET_WIDTH_DURATION) {
			dev_err(&pdev->dev, "Invalid reset width\n");
			return -EINVAL;
		}

		/*
		 * The watchdog is always configured with a 1MHz source, so
		 * there is no need to scale the microsecond value.
		 */
		writel(duration, wdt->base + WDT_RESET_WIDTH);
	}

	ret = watchdog_register_device(&wdt->wdd);
	if (ret) {
		dev_err(&pdev->dev, "failed to register\n");
		return ret;
	}

	platform_set_drvdata(pdev, wdt);

	return 0;
}

static struct platform_driver aspeed_watchdog_driver = {
	.probe = aspeed_wdt_probe,
	.remove = aspeed_wdt_remove,
	.driver = {
		.name = KBUILD_MODNAME,
		.of_match_table = of_match_ptr(aspeed_wdt_of_table),
	},
};

static int __init aspeed_wdt_init(void)
{
	return platform_driver_register(&aspeed_watchdog_driver);
}
arch_initcall(aspeed_wdt_init);

static void __exit aspeed_wdt_exit(void)
{
	platform_driver_unregister(&aspeed_watchdog_driver);
}
module_exit(aspeed_wdt_exit);

MODULE_DESCRIPTION("Aspeed Watchdog Driver");
MODULE_LICENSE("GPL");

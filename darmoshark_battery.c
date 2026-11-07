// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * darmoshark_battery — expose a Darmoshark (MOJI) mouse battery as a Linux
 * power supply.
 *
 * The mouse/receiver speaks a 64-byte vendor protocol on the configuration
 * collection (usage page 0xff0a, usage 0x0001). This module implements only
 * the power query from that protocol and registers the result as a peripheral
 * power_supply. UPower picks it up and desktop battery widgets (UPower ->
 * Solid -> KDE's "Power and Battery" applet) show it the same way they show a
 * Logitech mouse whose kernel driver registers hidpp_battery_N.
 *
 * Scope is deliberately minimal: battery percentage only. The protocol's
 * charging-state byte is not mapped, so the supply reports STATUS_UNKNOWN
 * instead of guessing. No DPI/profile/firmware writes happen here.
 */

#include <linux/atomic.h>
#include <linux/completion.h>
#include <linux/device.h>
#include <linux/hid.h>
#include <linux/init.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/power_supply.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/workqueue.h>

#include "darmoshark_protocol.h"

#define DS_VENDOR_ID 0x1915
#define DS_WIRED_PID 0x073c
#define DS_RECEIVER_PID 0x0723

#define DS_READ_TIMEOUT_MS 1500
#define DS_FIRST_POLL_MS 1000
#define DS_MIN_INTERVAL_MS 1000

static unsigned int poll_ms = 60000;
module_param(poll_ms, uint, 0644);
MODULE_PARM_DESC(poll_ms, "Battery refresh interval in milliseconds (default 60000)");

static unsigned int retry_ms = 10000;
module_param(retry_ms, uint, 0644);
MODULE_PARM_DESC(retry_ms, "Retry interval until the first reading (default 10000)");

/* Dedicated ordered queue: the vendor query is synchronous and may block for
 * the USB timeout, which must not stall the shared system workqueue. */
static struct workqueue_struct *ds_wq;

struct darmoshark_device {
	struct hid_device *hdev;
	int wireless;
	bool stopping;
	struct power_supply *psy;
	struct power_supply_desc desc;
	char ps_name[32];
	struct delayed_work work;
	struct mutex io_lock;
	spinlock_t state_lock;
	struct completion resp;
	u8 cmd_buf[DS_REPORT_LEN];
	u8 resp_buf[DS_REPORT_LEN];
	bool resp_pending;
	bool resp_valid;
	bool have_reading;
	u8 percent;
	int level;
};

static enum power_supply_property ds_props[] = {
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_CAPACITY_LEVEL,
	POWER_SUPPLY_PROP_SCOPE,
	POWER_SUPPLY_PROP_MODEL_NAME,
	POWER_SUPPLY_PROP_MANUFACTURER,
	POWER_SUPPLY_PROP_SERIAL_NUMBER,
};

static const char *ds_model(u32 product)
{
	switch (product) {
	case DS_WIRED_PID:
		return "Darmoshark M3S Pro";
	case DS_RECEIVER_PID:
		return "Darmoshark M3S Pro (2.4 GHz)";
	default:
		return "Darmoshark";
	}
}

static int ds_level(u8 percent)
{
	if (percent <= 5)
		return POWER_SUPPLY_CAPACITY_LEVEL_CRITICAL;
	if (percent <= 20)
		return POWER_SUPPLY_CAPACITY_LEVEL_LOW;
	if (percent <= 95)
		return POWER_SUPPLY_CAPACITY_LEVEL_NORMAL;
	return POWER_SUPPLY_CAPACITY_LEVEL_FULL;
}

static int ds_get_property(struct power_supply *psy,
			   enum power_supply_property psp,
			   union power_supply_propval *val)
{
	struct darmoshark_device *ds = power_supply_get_drvdata(psy);
	unsigned long flags;

	switch (psp) {
	case POWER_SUPPLY_PROP_PRESENT:
		spin_lock_irqsave(&ds->state_lock, flags);
		val->intval = ds->have_reading;
		spin_unlock_irqrestore(&ds->state_lock, flags);
		break;
	case POWER_SUPPLY_PROP_STATUS:
		/* The vendor state byte is not decoded yet. */
		val->intval = POWER_SUPPLY_STATUS_UNKNOWN;
		break;
	case POWER_SUPPLY_PROP_CAPACITY:
		spin_lock_irqsave(&ds->state_lock, flags);
		val->intval = ds->percent;
		spin_unlock_irqrestore(&ds->state_lock, flags);
		break;
	case POWER_SUPPLY_PROP_CAPACITY_LEVEL:
		spin_lock_irqsave(&ds->state_lock, flags);
		val->intval = ds->level;
		spin_unlock_irqrestore(&ds->state_lock, flags);
		break;
	case POWER_SUPPLY_PROP_SCOPE:
		val->intval = POWER_SUPPLY_SCOPE_DEVICE;
		break;
	case POWER_SUPPLY_PROP_MODEL_NAME:
		val->strval = ds_model(ds->hdev->product);
		break;
	case POWER_SUPPLY_PROP_MANUFACTURER:
		val->strval = "Darmoshark";
		break;
	case POWER_SUPPLY_PROP_SERIAL_NUMBER:
		val->strval = ds->hdev->uniq;
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static int ds_register_psy(struct darmoshark_device *ds)
{
	static atomic_t battery_no = ATOMIC_INIT(0);
	struct power_supply_config cfg = { .drv_data = ds };
	int index = atomic_inc_return(&battery_no) - 1;
	int ret;

	scnprintf(ds->ps_name, sizeof(ds->ps_name), "darmoshark_battery_%d",
		  index);
	ds->desc.name = ds->ps_name;
	ds->desc.type = POWER_SUPPLY_TYPE_BATTERY;
	ds->desc.properties = ds_props;
	ds->desc.num_properties = ARRAY_SIZE(ds_props);
	ds->desc.get_property = ds_get_property;
	ds->desc.use_for_apm = 0;

	ds->psy = devm_power_supply_register(&ds->hdev->dev, &ds->desc, &cfg);
	if (IS_ERR(ds->psy)) {
		ret = PTR_ERR(ds->psy);
		ds->psy = NULL;
		return ret;
	}
	power_supply_powers(ds->psy, &ds->hdev->dev);
	hid_info(ds->hdev, "registered %s (%s)\n", ds->ps_name,
		 ds_model(ds->hdev->product));
	return 0;
}

static void ds_update(struct darmoshark_device *ds, u8 percent)
{
	unsigned long flags;
	bool changed = false;
	int ret;

	spin_lock_irqsave(&ds->state_lock, flags);
	if (!ds->have_reading || ds->percent != percent) {
		ds->percent = percent;
		ds->level = ds_level(percent);
		ds->have_reading = true;
		changed = true;
	}
	spin_unlock_irqrestore(&ds->state_lock, flags);

	if (!ds->psy) {
		ret = ds_register_psy(ds);
		if (ret) {
			dev_warn_ratelimited(&ds->hdev->dev,
					     "power_supply registration failed: %d\n",
					     ret);
			return;
		}
		power_supply_changed(ds->psy);
		return;
	}

	if (changed)
		power_supply_changed(ds->psy);
}

/* Runs in the USB/input completion path: copy the report and wake the worker,
 * never sleep here. Returning 0 lets hidraw receive the report as well. */
static int ds_raw_event(struct hid_device *hdev, struct hid_report *report,
			u8 *data, int size)
{
	struct darmoshark_device *ds = hid_get_drvdata(hdev);
	unsigned long flags;

	(void)report;

	if (!ds || size < DS_REPORT_LEN)
		return 0;
	if (!darmoshark_is_power_response(data, (unsigned int)size))
		return 0;

	spin_lock_irqsave(&ds->state_lock, flags);
	if (ds->resp_pending) {
		memcpy(ds->resp_buf, data, DS_REPORT_LEN);
		ds->resp_valid = true;
		ds->resp_pending = false;
		spin_unlock_irqrestore(&ds->state_lock, flags);
		complete(&ds->resp);
		return 0;
	}
	spin_unlock_irqrestore(&ds->state_lock, flags);
	return 0;
}

static void ds_poll(struct work_struct *work)
{
	struct darmoshark_device *ds =
		container_of(work, struct darmoshark_device, work.work);
	u8 response[DS_REPORT_LEN];
	unsigned long flags;
	unsigned long delay;
	bool got = false;
	u8 percent = 0;
	int ret;

	if (READ_ONCE(ds->stopping))
		return;

	mutex_lock(&ds->io_lock);
	/*
	 * usbhid_output_report() hands this buffer to usb_interrupt_msg(),
	 * which maps it for DMA. A stack buffer is rejected by the HCD with
	 * "transfer buffer is on stack" (-EAGAIN), so keep it in the
	 * devm-allocated device state.
	 */
	darmoshark_build_power(ds->cmd_buf, ds->wireless);

	spin_lock_irqsave(&ds->state_lock, flags);
	ds->resp_pending = true;
	ds->resp_valid = false;
	spin_unlock_irqrestore(&ds->state_lock, flags);
	reinit_completion(&ds->resp);

	ret = hid_hw_output_report(ds->hdev, ds->cmd_buf, DS_REPORT_LEN);
	if (ret > 0 &&
	    wait_for_completion_timeout(&ds->resp,
					msecs_to_jiffies(DS_READ_TIMEOUT_MS))) {
		spin_lock_irqsave(&ds->state_lock, flags);
		if (ds->resp_valid) {
			memcpy(response, ds->resp_buf, DS_REPORT_LEN);
			got = true;
		}
		spin_unlock_irqrestore(&ds->state_lock, flags);
	} else {
		hid_dbg(ds->hdev, "battery query failed: %d\n", ret);
	}

	spin_lock_irqsave(&ds->state_lock, flags);
	ds->resp_pending = false;
	spin_unlock_irqrestore(&ds->state_lock, flags);
	mutex_unlock(&ds->io_lock);

	if (got && darmoshark_parse_power(response, DS_REPORT_LEN, &percent))
		ds_update(ds, percent);

	if (READ_ONCE(ds->stopping))
		return;

	spin_lock_irqsave(&ds->state_lock, flags);
	delay = ds->have_reading ? poll_ms : retry_ms;
	spin_unlock_irqrestore(&ds->state_lock, flags);
	if (delay < DS_MIN_INTERVAL_MS)
		delay = DS_MIN_INTERVAL_MS;

	queue_delayed_work(ds_wq, &ds->work, msecs_to_jiffies(delay));
}

/* Only the configuration collection (usage page 0xff0a) answers the vendor
 * protocol. The receiver shares its VID/PID across four HID interfaces, and
 * hid-generic declines every interface whose ID matches another driver, so
 * the non-configuration interfaces must be handled generically here. */
static bool ds_descriptor_is_config(const struct hid_device *hdev)
{
	unsigned int i;

	if (!hdev->dev_rdesc || hdev->dev_rsize < 3)
		return false;

	for (i = 0; i + 2 < hdev->dev_rsize; i++) {
		/* Usage Page 0xFF0A: 06 0a ff */
		if (hdev->dev_rdesc[i] == 0x06 &&
		    hdev->dev_rdesc[i + 1] == 0x0a &&
		    hdev->dev_rdesc[i + 2] == 0xff)
			return true;
	}
	return false;
}

static bool ds_has_vendor_collection(const struct hid_device *hdev)
{
	unsigned int i;

	for (i = 0; i < hdev->maxcollection; i++) {
		if (hdev->collection[i].usage == ((0xff0aU << 16) | 0x0001U))
			return true;
	}
	return false;
}

static int ds_probe(struct hid_device *hdev, const struct hid_device_id *id)
{
	struct darmoshark_device *ds;
	int ret;

	(void)id;

	if (!ds_descriptor_is_config(hdev)) {
		hdev->quirks |= HID_QUIRK_INPUT_PER_APP;
		ret = hid_parse(hdev);
		if (ret)
			return ret;
		return hid_hw_start(hdev, HID_CONNECT_DEFAULT);
	}

	ret = hid_parse(hdev);
	if (ret)
		return ret;
	if (!ds_has_vendor_collection(hdev)) {
		hid_warn(hdev,
			 "vendor usage page without a 0xff0a:0x0001 collection; using generic input\n");
		hdev->quirks |= HID_QUIRK_INPUT_PER_APP;
		return hid_hw_start(hdev, HID_CONNECT_DEFAULT);
	}

	ret = hid_hw_start(hdev, HID_CONNECT_HIDRAW);
	if (ret) {
		hid_err(hdev, "cannot start the vendor interface: %d\n", ret);
		return ret;
	}
	ret = hid_hw_open(hdev);
	if (ret) {
		hid_err(hdev, "cannot open the vendor interface: %d\n", ret);
		hid_hw_stop(hdev);
		return ret;
	}

	ds = devm_kzalloc(&hdev->dev, sizeof(*ds), GFP_KERNEL);
	if (!ds) {
		hid_hw_close(hdev);
		hid_hw_stop(hdev);
		return -ENOMEM;
	}
	ds->hdev = hdev;
	ds->wireless = hdev->product == DS_RECEIVER_PID;
	mutex_init(&ds->io_lock);
	spin_lock_init(&ds->state_lock);
	init_completion(&ds->resp);
	INIT_DELAYED_WORK(&ds->work, ds_poll);
	hid_set_drvdata(hdev, ds);

	queue_delayed_work(ds_wq, &ds->work, msecs_to_jiffies(DS_FIRST_POLL_MS));
	hid_info(hdev, "Darmoshark battery channel ready (%s)\n",
		 ds->wireless ? "2.4 GHz" : "wired");
	return 0;
}

static void ds_remove(struct hid_device *hdev)
{
	struct darmoshark_device *ds = hid_get_drvdata(hdev);

	if (ds) {
		WRITE_ONCE(ds->stopping, true);
		cancel_delayed_work_sync(&ds->work);
		hid_hw_close(hdev);
	}
	hid_hw_stop(hdev);
}

static const struct hid_device_id darmoshark_devices[] = {
	{ HID_USB_DEVICE(DS_VENDOR_ID, DS_WIRED_PID) },
	{ HID_USB_DEVICE(DS_VENDOR_ID, DS_RECEIVER_PID) },
	{ }
};
MODULE_DEVICE_TABLE(hid, darmoshark_devices);

static struct hid_driver darmoshark_driver = {
	.name = "darmoshark-battery",
	.id_table = darmoshark_devices,
	.probe = ds_probe,
	.remove = ds_remove,
	.raw_event = ds_raw_event,
};

static int ds_rebind_device(struct device *dev, void *data)
{
	struct hid_device *hdev = to_hid_device(dev);
	int ret;

	(void)data;

	if (hdev->bus != BUS_USB || hdev->vendor != DS_VENDOR_ID)
		return 0;
	if (hdev->product != DS_WIRED_PID && hdev->product != DS_RECEIVER_PID)
		return 0;
	if (hdev->driver == &darmoshark_driver)
		return 0;
	if (!ds_descriptor_is_config(hdev))
		return 0;

	ret = device_reprobe(dev);
	if (ret)
		hid_warn(hdev, "could not take over the vendor interface: %d\n",
			 ret);
	return 0;
}

static int __init ds_init(void)
{
	int ret;

	ds_wq = alloc_ordered_workqueue("darmoshark_battery", 0);
	if (!ds_wq)
		return -ENOMEM;

	ret = hid_register_driver(&darmoshark_driver);
	if (ret) {
		destroy_workqueue(ds_wq);
		ds_wq = NULL;
		return ret;
	}

	/* A module loaded after the device was enumerated has to take the
	 * configuration interface over from hid-generic. Only that interface is
	 * reprobed, so the input interfaces are left untouched. */
	bus_for_each_dev(&hid_bus_type, NULL, NULL, ds_rebind_device);

	pr_info("darmoshark_battery: loaded\n");
	return 0;
}

static void __exit ds_exit(void)
{
	hid_unregister_driver(&darmoshark_driver);
	destroy_workqueue(ds_wq);
	ds_wq = NULL;
}

module_init(ds_init);
module_exit(ds_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Darmoshark mouse battery as a Linux power supply");
MODULE_AUTHOR("akuma <exyexin@gmail.com>");

/*
 * bq27510 battery driver
 *
 * Copyright (C) 2008 Rodolfo Giometti <giometti@linux.it>
 * Copyright (C) 2008 Eurotech S.p.A. <info@eurotech.it>
 * Copyright (C) 2010 Konstantin Motov <kmotov@mm-sol.com>
 * Copyright (C) 2010 Dimitar Dimitrov <dddimitrov@mm-sol.com>
 *
 * Based on a previous work by Copyright (C) 2008 Texas Instruments, Inc.
 *
 * This package is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * THIS PACKAGE IS PROVIDED ``AS IS'' AND WITHOUT ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, WITHOUT LIMITATION, THE IMPLIED
 * WARRANTIES OF MERCHANTIBILITY AND FITNESS FOR A PARTICULAR PURPOSE.
 *
 */
#include <linux/module.h>
#include <linux/param.h>
#include <linux/jiffies.h>
#include <linux/workqueue.h>
#include <linux/delay.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/idr.h>
#include <linux/i2c.h>
#include <linux/i2c/twl4030-madc.h>
#include <asm/unaligned.h>
#include <linux/timer.h>
#include <linux/jiffies.h>
#include <linux/rwsem.h>
#include <mach/gpio.h>

#include <linux/bq27x00_battery.h>

#define DRIVER_VERSION			"1.1.0"

/*
 * Polling interval, in milliseconds.
 *
 * It is expected that each power supply will call power_supply_changed(),
 * thus alleviating user-space from the need to poll. But BQ27510 cannot
 * raise an interrupt so we're forced to issue change events regularly.
 */
#define T_POLL_MS			30000
/*
 * Because the battery may not report accurate status on the first
 * poll we check every 500ms for the first 5 seconds. 5 seconds
 * was empirically determined to be an ok interval for keeping
 * the charge status accurate-ish
 */
#define T_POLL_PLUG_MS        500 // ms
#define T_POLL_PLUG_MAX        10 // iterations

#define USB_CURRENT_LIMIT_LOW		100000   /* microAmp */
#define USB_CURRENT_LIMIT_HIGH		500000   /* microAmp */
#define AC_CURRENT_LIMIT		1500000  /* microAmp */

#define BQ27x00_REG_CONTROL     (0x00)
#define BQ27x00_CONTROL_STATUS		(0x0000)
#define BQ27x00_CONTROL_STATUS_INITCOMP	BIT(7)
#define BQ27x00_CONTROL_DEVICE_TYPE	(0x0001)
#define BQ27x00_CONTROL_FW_VERSION	(0x0002)
#define BQ27x00_CONTROL_HW_VERSION   (0x0003)
#define BQ27x00_CONTROL_DF_VERSION	(0x001F)



#define BQ27510_REG_ATRATE		0x02
#define BQ27510_REG_TEMP		0x06
#define BQ27510_REG_VOLT		0x08
#define BQ27510_REG_RSOC		0x2C /* Relative State-of-Charge */
#define BQ27510_REG_AI			0x14
#define BQ27510_REG_FLAGS		0x0A
#define BQ27510_REG_TTE			0x16
#define BQ27510_REG_TTF			0x18
#define BQ27510_REG_FCC			0x12
#if defined(CONFIG_BATTERY_BQ27520)
#define BQ27510_REG_SOH			0x28
#define BQ27510_REG_SOH_STATUS			0x29
#define BQ27510_REG_DATALOG_INDEX	0x32
#define BQ27510_REG_DATALOG_BUFFER	0x34
#define BQ27510_REG_NOMINAL_CAPACITY	0x0C
#endif /* CONFIG_BATTERY_BQ27520 */

#define BQ27510_REG_BUFFER_START	BQ27510_REG_ATRATE
#define BQ27510_REG_BUFFER_SIZE		0x36

#define BAT_DET_FLAG_BIT		3
#define OFFSET_KELVIN_CELSIUS		273
#define OFFSET_KELVIN_CELSIUS_DECI	2731
#define KELVIN_SCALE_RANGE		10
#define CURRENT_OVF_THRESHOLD		((1 << 15) - 1)

#define FLAG_BIT_DSG			0
#define FLAG_BIT_SOCF			1
#define FLAG_BIT_SOC1			2
#define FLAG_BIT_BAT_DET		3
#define FLAG_BIT_WAIT_ID		4
#define FLAG_BIT_OCV_GD			5
#define FLAG_BIT_CHG			8
#define FLAG_BIT_FC				9
#define FLAG_BIT_XCHG			10
#define FLAG_BIT_CHG_INH		11
#define FLAG_BIT_OTD			14
#define FLAG_BIT_OTC			15

#define to_bq27510_device_info(x) container_of((x), \
				struct bq27510_device_info, bat);
#define to_bq27510_device_usb_info(x) container_of((x), \
				struct bq27510_device_info, usb);
#define to_bq27510_device_wall_info(x) container_of((x), \
				struct bq27510_device_info, wall);
				
/* If the system has several batteries we need a different name for each
 * of them...
 */
static DEFINE_IDR(battery_id);
static DEFINE_MUTEX(battery_mutex);

struct bq27510_device_info;

struct bq27510_device_info {
	struct device 			*dev;
	int				id;
	int				voltage_uV;
	int				current_uA;
	int				temp_C;
	int				charge_rsoc;
	int                     	time_to_empty;
	int                     	time_to_full;
    int rapid_poll_cycle;
	struct power_supply		bat;
	struct power_supply		usb;
	struct power_supply		wall;

	struct i2c_client		*client;
	struct delayed_work		bat_work;
	struct rw_semaphore		reglock;

	/* Cached register values. Updated upon IRQ or poll timer activation */
	uint8_t				regbuf[BQ27510_REG_BUFFER_SIZE];

	/* Cached property registers - snatched upon device probe */
	int				sys_device_type;
	int				sys_fw_version;
	int				sys_hw_version;

	int gpio_ce;
	int gpio_soc_int;
	int gpio_bat_low;
	int gpio_bat_id;

};
static int bq27510_battery_supply_prop_present(struct bq27510_device_info *di);


#if defined(CONFIG_MACH_OMAP3621_GOSSAMER)
enum {
	BATTERY_MCNAIR= 0,
	BATTERY_LICO = 1,
	BATTERY_LISHEN = 2,
	BATTERY_ABSENT = 3,
	BATTERY_UNKNOWN = 4,
	BATTERY_NUM = 5
};

static const char * const manufacturer_name[BATTERY_NUM] = {
	"McNair",
	"Lico",
	"Lishen",
	"Absent",
	"Unknown"
};

/* ID voltages in uV */
static const int battery_id_min[BATTERY_NUM-1] = {
	290000,
	690000,
	1165000,
	1425000
};

static const int battery_id_max[BATTERY_NUM-1] = {
	400000,
	810000,
	1305000,
	1575000
};

static int manufacturer_id;
#endif

static enum power_supply_property bq27510_battery_props[] = {
	/* Battery status - see POWER_SUPPLY_STATUS_* */
	POWER_SUPPLY_PROP_STATUS,
	/* Battery health - see POWER_SUPPLY_HEALTH_* */
	POWER_SUPPLY_PROP_HEALTH,
	/* Battery technology - see POWER_SUPPLY_TECHNOLOGY_* */
	POWER_SUPPLY_PROP_TECHNOLOGY,
	/* Boolean 1 -> battery detected, 0 battery not inserted. */
	POWER_SUPPLY_PROP_PRESENT,
	/* Measured Voltage cell pack in mV. */
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	/* 
	 * Signed measured average current (1 sec) in mA.
	 * Negative means discharging, positive means charging.
	 */
#ifdef CONFIG_BATTERY_BQ27520
	POWER_SUPPLY_PROP_CURRENT_AVG,
	POWER_SUPPLY_PROP_STATE_OF_HEALTH,
	POWER_SUPPLY_PROP_DATALOG_INDEX,
	POWER_SUPPLY_PROP_DATALOG_BUFFER,
	POWER_SUPPLY_PROP_NOMINAL_CAPACITY,
#endif /* CONFIG_BATTERY_BQ27520 */
	POWER_SUPPLY_PROP_CURRENT_NOW,
	/*
	 * Predicted remaining battery capacity expressed 
	 * as a percentage 0 - 100%.
	 */
	POWER_SUPPLY_PROP_CAPACITY,
	/* Battery temperature converted in 0.1 Celsius. */
	POWER_SUPPLY_PROP_TEMP,
	/* 
	 * Time to discharge the battery in minutes based on the 
	 * average current. 65535 indicates charging cycle.
	 */
	POWER_SUPPLY_PROP_TIME_TO_EMPTY_NOW,
	/* Time to recharge the battery in minutes based on the average current. 65535 indicates discharging cycle. */
	POWER_SUPPLY_PROP_TIME_TO_FULL_NOW,
	/* Maximum battery charge */
	POWER_SUPPLY_PROP_CHARGE_FULL
#if defined(CONFIG_MACH_OMAP3621_GOSSAMER)
	,
	POWER_SUPPLY_PROP_MANUFACTURER
#endif
};

static enum power_supply_property bq27510_usb_props[] = {
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_CURRENT_AVG,
};

static enum power_supply_property bq27510_wall_props[] = {
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_CURRENT_AVG,
};

static int  bq27x10_type = POWER_SUPPLY_TYPE_BATTERY;
static struct bq27510_device_info *local_di = NULL;

void bq27x10_charger_type(int limit)
{
	bq27x10_type = POWER_SUPPLY_TYPE_BATTERY;
	if (limit == USB_CURRENT_LIMIT_HIGH)
		bq27x10_type = POWER_SUPPLY_TYPE_USB;
	if (limit == AC_CURRENT_LIMIT)
		bq27x10_type = POWER_SUPPLY_TYPE_MAINS;

	if (local_di) {
		cancel_delayed_work_sync(&local_di->bat_work);
		local_di->rapid_poll_cycle = 0;
		schedule_delayed_work(&local_di->bat_work,
					msecs_to_jiffies(T_POLL_PLUG_MS));
	}
}
EXPORT_SYMBOL(bq27x10_charger_type);

#if defined(CONFIG_MACH_OMAP3621_GOSSAMER)
static unsigned int g_pause_i2c = 0;
static ssize_t pause_i2c_show(struct device *dev, struct device_attribute *attr,
							  char *buf) {
	return sprintf(buf, "%u\n",  g_pause_i2c);
}

static ssize_t pause_i2c_store(struct device *dev,
							   struct device_attribute *attr,
							   const char *buf, size_t count) {
	if (count > 1) {
		if (buf[0] == '0')
			g_pause_i2c = 0;
		else
			g_pause_i2c = 1;
	}
	return count;
}

static struct device_attribute dev_attr_pause_i2c = {
	.attr	= {
		.name = "pause_i2c",
		.mode = 0660
		},
	.show = pause_i2c_show,
	.store = pause_i2c_store
};

static ssize_t bus_disable_store(struct device *dev,
							   struct device_attribute *attr,
							   const char *buf, size_t count) {
	int polling_interval = T_POLL_PLUG_MS;
	if (buf[0] == '0') {
		if (local_di) {
			schedule_delayed_work(&local_di->bat_work, msecs_to_jiffies(polling_interval));
		}
	}
	else {
		if (local_di) {
			cancel_delayed_work_sync(&local_di->bat_work);
		}
	}
	return count;
}
static struct device_attribute dev_attr_bus_disable = {
	.attr	= {
		.name = "bus_disable",
		.mode = 0220
		},
	.store = bus_disable_store
};
#endif

/*
 * Read all registers and save them to a local buffer. This minimizes
 * the overall I2C transfers, and prevents BQ lockup due to excessive
 * I2C communication.
 */
static int bq27x10_read_registers(struct bq27510_device_info *di)
{
	struct i2c_client *client = di->client;
	int stat;
	uint8_t regaddr_start = BQ27510_REG_BUFFER_START;
	struct i2c_msg msgs[] = {
		{
			.addr = client->addr,
			.flags = 0,
			.len = 1,
			.buf = &regaddr_start,
		},
		{
			.addr = client->addr,
			.flags = I2C_M_RD,
			.len = BQ27510_REG_BUFFER_SIZE - BQ27510_REG_BUFFER_START,
			.buf = di->regbuf + BQ27510_REG_BUFFER_START,
		}
	};


#if defined(CONFIG_MACH_OMAP3621_GOSSAMER)
	if (g_pause_i2c == 1)
		return -EBUSY;
#endif
	down_write(&di->reglock);
	stat = i2c_transfer(client->adapter, msgs, ARRAY_SIZE(msgs));
	up_write(&di->reglock);

	if (stat < 0)
		dev_err(di->dev, "I2C read error: %d\n", stat);
	else if (stat != ARRAY_SIZE(msgs)) {
		dev_err(di->dev, "I2C read N mismatch: %d\n", stat);
		stat = -EIO;
	} else
		stat = 0;

	return stat;
}

static void bq27x10_bat_work(struct work_struct *work)
{
	struct delayed_work *dwork = (void *)work;
	int polling_interval = T_POLL_MS;
	struct bq27510_device_info *di;

	di = container_of(dwork, struct bq27510_device_info, bat_work);

	if (local_di) {
		bq27x10_read_registers(di);
		power_supply_changed(&di->bat);
		power_supply_changed(&di->usb);
		power_supply_changed(&di->wall);

		if (di->rapid_poll_cycle < T_POLL_PLUG_MAX) {
			polling_interval = T_POLL_PLUG_MS;
			++di->rapid_poll_cycle;
		}
	}
	schedule_delayed_work(&di->bat_work,
				msecs_to_jiffies(polling_interval));
}

/*
 * Common code for bq27x10 devices - get cached register value.
 */
static int bq27x10_read(u8 reg, int *rt_value, int b_single,
			struct bq27510_device_info *di)
{
	struct i2c_client *client = di->client;

	if (!client->adapter)
		return -ENODEV;

	if (b_single)
		*rt_value = di->regbuf[reg];
	else {
		down_read(&di->reglock);
		*rt_value = di->regbuf[reg] + (di->regbuf[reg+1] << 8);
		up_read(&di->reglock);
	}

	return 0;
}

/*
 * Return the battery temperature in 0.1 Kelvin degrees
 * Or < 0 if something fails.
 */
static int bq27510_battery_temperature(struct bq27510_device_info *di)
{
	int ret;
	int temp = 0;

	ret = bq27x10_read(BQ27510_REG_TEMP, &temp, 0, di);
	if (ret) {
		dev_err(di->dev, "error reading temperature\n");
		return ret;
	}

	dev_dbg(di->dev, "temperature: %d [0.1K]\n", temp);
	return temp;
}

/*
 * Return the battery Voltage in milivolts
 * Or < 0 if something fails.
 */
static int bq27510_battery_voltage(struct bq27510_device_info *di)
{
	int ret;
	int volt = 0;
	ret = bq27x10_read(BQ27510_REG_VOLT, &volt, 0, di);
	if (ret) {
		dev_err(di->dev, "error reading voltage\n");
		return ret;
	}

	return volt;
}

/*
 * Return the battery average current
 * Note that current can be negative signed as well
 * Or 0 if something fails.
 */
static int bq27510_battery_current(struct bq27510_device_info *di)
{
	int ret;
	int curr = 0;
	/*int flags = 0;*/

	ret = bq27x10_read(BQ27510_REG_AI, &curr, 0, di);
	if (ret) {
		dev_err(di->dev, "error reading current\n");
		return 0;
	}

	/* In the BQ27510 convention, charging current is positive while discharging current is negative */
	return ((curr > CURRENT_OVF_THRESHOLD) ? (curr-((1 << 16) -1)):curr);
}

/*
 * Return the battery Relative State-of-Charge
 * Or < 0 if something fails.
 */
static int bq27510_battery_rsoc(struct bq27510_device_info *di)
{
	int ret;
	int rsoc = 0;

	ret = bq27x10_read(BQ27510_REG_RSOC, &rsoc, 0, di);
	if (ret) {
		dev_err(di->dev, "error reading relative State-of-Charge\n");
		printk(KERN_INFO "gauge: %s read RSOC error ret=%d  SOC=%d\n", __FUNCTION__,ret,rsoc);
		return 100;
	}

	if ((rsoc == 0) || (rsoc == 0xffff) ) {
		if (!bq27510_battery_supply_prop_present(di)) {
			rsoc=100;
		}
	}

	if (rsoc < 0){
		rsoc = 1;
	}
	else if (rsoc > 100)
		rsoc = 100;

	return rsoc;
}

/*
 * Battery detected.
 * Rerturn true when batery is present
 */
static int bq27510_battery_supply_prop_present(struct bq27510_device_info *di)
{
	int ret;
	int bat_det_flag = 0;

	ret = bq27x10_read(BQ27510_REG_FLAGS, &bat_det_flag, 0, di);
	if (ret) {
		dev_err(di->dev, "error reading voltage\n");
		return ret;
	} 
	return (bat_det_flag & (1 << FLAG_BIT_BAT_DET)) ? 1 : 0;
}

/*
 * Return predicted remaining battery life at the present rate of discharge,
 * in minutes.
 */
static int bq27510_battery_time_to_empty_now(struct bq27510_device_info *di)
{
	int ret;
	int tte = 0;

	ret = bq27x10_read(BQ27510_REG_TTE, &tte, 0, di);
	if (ret) {
		dev_err(di->dev, "error reading voltage\n");
		return ret;
	}
	return tte;
}

/*
 * Return predicted remaining time until the battery reaches full charge,
 * in minutes
  */
static int bq27510_battery_time_to_full_now(struct bq27510_device_info *di)
{
	int ret;
	int ttf = 0;

	ret = bq27x10_read(BQ27510_REG_TTF, &ttf, 0, di);
	if (ret) {
		dev_err(di->dev, "error reading voltage\n");
		return ret;
	}
	return ttf;
}

/*
 * Returns the compensated capacity of the battery when fully charged.
 * Units are mAh
  */
static int bq27510_battery_max_level(struct bq27510_device_info *di)
{
	int ret;
	int bml = 0;

	ret = bq27x10_read(BQ27510_REG_FCC, &bml, 0, di);
	if (ret) {
		dev_err(di->dev, "error reading voltage\n");
		return ret;
	}
	return bml;
}

/*
 * Returns the state of health for battery
 * Units are %
  */
#ifdef CONFIG_BATTERY_BQ27520
static int bq27510_battery_health_percent(struct bq27510_device_info *di)
{
	int ret;
	int bml = 0;

	ret = bq27x10_read(BQ27510_REG_SOH, &bml, 1, di);
	if (ret) {
		dev_err(di->dev, "error reading state of health\n");
		return ret;
	}

	return bml;
}

static int bq27510_battery_datalog_index(struct bq27510_device_info *di)
{
	int ret;
	int bml = 0;

	ret = bq27x10_read(BQ27510_REG_DATALOG_INDEX, &bml, 0, di);
	if (ret) {
		dev_err(di->dev, "error reading datalog index\n");
		return ret;
	}
	return bml;
}

static int bq27510_battery_datalog_buffer(struct bq27510_device_info *di)
{
	int ret;
	int bml = 0;

	ret = bq27x10_read(BQ27510_REG_DATALOG_BUFFER, &bml, 0, di);
	if (ret) {
		dev_err(di->dev, "error reading datalog buffer\n");
		return ret;
	}
	return bml;
}

static int bq27510_battery_nominal_capacity(struct bq27510_device_info *di)
{
	int ret;
	int bml = 0;

	ret = bq27x10_read(BQ27510_REG_NOMINAL_CAPACITY, &bml, 0, di);
	if (ret) {
		dev_err(di->dev, "error reading nominal capacity\n");
		return ret;
	}
	return bml;
}
#endif /* CONFIG_BATTERY_BQ27520 */

static int bq27510_battery_status(struct bq27510_device_info *di)
{
	int ret, curr;
	int flags = 0;

	ret = bq27x10_read(BQ27510_REG_FLAGS, &flags, 0, di);
	if (ret) {
		dev_err(di->dev, "error reading status flags (%d)\n", ret);
		return ret;
	}

	curr = bq27510_battery_current(di);

	dev_dbg(di->dev, "Flags=%04x\n", flags);

	if (flags & (1u << FLAG_BIT_FC))
		ret = POWER_SUPPLY_STATUS_FULL;
	else if ((flags & (1u << FLAG_BIT_DSG)) && (curr < 0))
		ret = POWER_SUPPLY_STATUS_DISCHARGING;
	else if ((flags & (1u << FLAG_BIT_CHG)) && (curr > 0))
		ret = POWER_SUPPLY_STATUS_CHARGING;
	else
		ret = POWER_SUPPLY_STATUS_NOT_CHARGING;

	return ret;
}

static int bq27510_battery_health(struct bq27510_device_info *di)
{
	int ret;
	int flags = 0;

	ret = bq27x10_read(BQ27510_REG_FLAGS, &flags, 0, di);
	if (ret) {
		dev_err(di->dev, "error reading health flags (%d)\n", ret);
		return ret;
	}

	if (flags & (1u << FLAG_BIT_OTC))
		ret = POWER_SUPPLY_HEALTH_OVERHEAT;
	else if (flags & (1u << FLAG_BIT_OTD))
		ret = POWER_SUPPLY_HEALTH_OVERHEAT;
	else if (flags & ((1u << FLAG_BIT_XCHG) | (1u << FLAG_BIT_CHG_INH))) {
		int t = bq27510_battery_temperature(di);
		if (t < OFFSET_KELVIN_CELSIUS_DECI)
			ret = POWER_SUPPLY_HEALTH_COLD;
		else
			ret = POWER_SUPPLY_HEALTH_OVERHEAT;
	} else
		ret = POWER_SUPPLY_HEALTH_GOOD;

	return ret;
}

/*
 * Return reuired battery property or error.
  */
static int bq27510_battery_get_property(struct power_supply *psy,
					enum power_supply_property psp,
					union power_supply_propval *val)
{
	int ret = -EINVAL;

	struct bq27510_device_info *di = to_bq27510_device_info(psy);
	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		val->intval = bq27510_battery_status(di);
		ret = 0;
		if (val->intval < 0)
			val->intval = POWER_SUPPLY_STATUS_UNKNOWN;
		break;
	case POWER_SUPPLY_PROP_HEALTH:
		val->intval = bq27510_battery_health(di);
		ret = 0;
		if (val->intval < 0)
			val->intval = POWER_SUPPLY_HEALTH_UNSPEC_FAILURE;
		break;
	case POWER_SUPPLY_PROP_TECHNOLOGY:
		ret = val->intval = POWER_SUPPLY_TECHNOLOGY_LION;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		/* return voltage in uV */
		ret = val->intval = bq27510_battery_voltage(di) * 1000;
		break;
#ifdef CONFIG_BATTERY_BQ27520
	case POWER_SUPPLY_PROP_DATALOG_INDEX:
		val->intval = bq27510_battery_datalog_index(di);
		ret = 0;
		break;
	case POWER_SUPPLY_PROP_DATALOG_BUFFER:
		val->intval = bq27510_battery_datalog_buffer(di);
		ret = 0;
		break;
	case POWER_SUPPLY_PROP_STATE_OF_HEALTH:
		val->intval = bq27510_battery_health_percent(di);
		ret = 0;
		break;
	case POWER_SUPPLY_PROP_NOMINAL_CAPACITY:
		val->intval = bq27510_battery_nominal_capacity(di);
		ret = 0;
		break;
	case POWER_SUPPLY_PROP_CURRENT_AVG:
#endif /* CONFIG_BATTERY_BQ27520 */
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		/* return positive current in uA */
		val->intval = bq27510_battery_current(di) * 1000;
		ret = 0;
		break;
	case POWER_SUPPLY_PROP_CAPACITY:
		ret = val->intval = bq27510_battery_rsoc(di);
		break;
	case POWER_SUPPLY_PROP_TEMP:
		ret = val->intval = bq27510_battery_temperature(di);
		/* convert from 0.1K to 0.1C */
		val->intval -= OFFSET_KELVIN_CELSIUS_DECI;
		break;
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = bq27510_battery_supply_prop_present(di);
		/* Report an absent battery if we can't reach the BQ chip. */
		if (val->intval < 0)
			val->intval = 0;
		ret = 0;
		break;
	case POWER_SUPPLY_PROP_TIME_TO_EMPTY_NOW:
		ret = val->intval = bq27510_battery_time_to_empty_now(di);
		break;
	case POWER_SUPPLY_PROP_TIME_TO_FULL_NOW:		
		ret = val->intval = bq27510_battery_time_to_full_now(di);
		break;	
	case POWER_SUPPLY_PROP_CHARGE_FULL:		
		/* present capacity in uAh */
		ret = val->intval = bq27510_battery_max_level(di) * 1000;
		break;	
#if defined(CONFIG_MACH_OMAP3621_GOSSAMER)
	case POWER_SUPPLY_PROP_MANUFACTURER:
		val->strval = manufacturer_name[manufacturer_id];
		ret = 0;
		break;
#endif
	default:
		return -EINVAL;
	}

	ret = (ret < 0) ? ret : 0;

	return ret;
}
/*
 * Return reuired battery property or error.
  */
static int bq27510_usb_get_property(struct power_supply *psy,
					enum power_supply_property psp,
					union power_supply_propval *val)
{
	int ret = -EINVAL;

	switch (psp) {
	case POWER_SUPPLY_PROP_ONLINE:
		val->intval = (bq27x10_type == POWER_SUPPLY_TYPE_USB);
		ret = 0;
		break;
    case POWER_SUPPLY_PROP_CURRENT_AVG:
		val->intval = USB_CURRENT_LIMIT_HIGH;
		ret = 0;
		break;
	default:
		return -EINVAL;
	}

	return ret;
}

/*
 * Return reuired battery property or error.
  */
static int bq27510_wall_get_property(struct power_supply *psy,
					enum power_supply_property psp,
					union power_supply_propval *val)
{
	int ret = -EINVAL;

	switch (psp) {
	case POWER_SUPPLY_PROP_ONLINE:
		val->intval = (bq27x10_type == POWER_SUPPLY_TYPE_MAINS);
		ret = 0;
		break;
    case POWER_SUPPLY_PROP_CURRENT_AVG:
		val->intval = AC_CURRENT_LIMIT;
		ret = 0;
		break;
	default:
		return -EINVAL;
	}

	return ret;
}

#if defined(CONFIG_MACH_OMAP3621_GOSSAMER)
static int get_gossamer_battery_manufacturer(int gpio_id)
{
	struct twl4030_madc_request req;
	int temp, i, type, ret;

	if (gpio_id > 0) {
		ret = gpio_request(gpio_id, "bq27510-id-control");
		if (ret) {
			printk(KERN_WARNING "couldn't request bq27510-id-control GPIO: %d\n", gpio_id);
			gpio_id = -1;
		} else {
			gpio_direction_output(gpio_id, 1);
		}
	}

	req.channels = 1;
	req.do_avg = 0;
	req.method = TWL4030_MADC_SW1;
	req.active = 0;
	req.func_cb = NULL;
	twl4030_madc_conversion(&req);
	temp = (u16)req.rbuf[0];
	temp = temp*1500000/1023; /* Accurate value in uV */

	type = BATTERY_UNKNOWN;
	for (i = 0; i < BATTERY_NUM-1; i++) {
		if ((temp > battery_id_min[i]) &&
		    (temp < battery_id_max[i])) {
			type = i;
			break;
		}
	}
	if (gpio_id > 0) {
		gpio_direction_output(gpio_id, 0);
		gpio_free(gpio_id);
	}

	return type;
}
#endif

static int bq27200_read_control(u16 reg, int *rt_value, struct bq27510_device_info *di)
{
	struct i2c_client *client = di->client;
	struct i2c_msg msg[1];
	unsigned char data[4];
	int err;

	if (!client->adapter)
		return -ENODEV;

	msg->addr = client->addr;
	msg->flags = 0;
	msg->len = 3;
	msg->buf = data;

	data[0] = BQ27x00_REG_CONTROL;
    data[1] = reg & 0xff;
    data[2] = (reg >> 8) & 0xff;
	err = i2c_transfer(client->adapter, msg, 1);

	if (err >= 0) {
        msleep(2);
        msg->addr = client->addr;
        msg->flags = 0;
        msg->len = 1;
        msg->buf = data;

        data[0] = BQ27x00_REG_CONTROL;
        err = i2c_transfer(client->adapter, msg, 1);

        if (err >= 0) {
            msg->len = 2;

            msg->flags = I2C_M_RD;
            err = i2c_transfer(client->adapter, msg, 1);
            if (err >= 0) {
                    *rt_value = data[0] | (data[1]<<8);
                return 0;
            }
        }
	}
	return err;
}

static void bq27x00_hw_reset(struct bq27510_device_info *di)
{
	if(di->gpio_ce == -1) {
		dev_warn(di->dev, "WARNING: bq27x00 cannot do hw_reset, not valid gpio_ce (value = %d)\n", di->gpio_ce);
		return;
	}
	dev_dbg(di->dev, "+bq27x00_hw_reset: gpio_ce:%d\n", di->gpio_ce);

	gpio_request(di->gpio_ce , "gpio_ce");
	gpio_direction_output(di->gpio_ce, 1);
	msleep(200);
	gpio_direction_output(di->gpio_ce, 0);
}

static ssize_t hw_reset_store(struct device *dev,
			       struct device_attribute *attr,
			       const char *buf, size_t count)
{
	if (count > 1) {
		if ('1' == buf[0]) {
			unsigned int previous_pause_i2c = g_pause_i2c;
			g_pause_i2c = 1;
			bq27x00_hw_reset(dev_get_drvdata(dev->parent));
			g_pause_i2c = previous_pause_i2c;
		}
	}
	return count;
}
static DEVICE_ATTR(hw_reset, S_IWUGO, NULL, hw_reset_store);

static ssize_t bq27520_get_device_type(struct device *dev, struct device_attribute *attr, char *buf)
{
    struct bq27510_device_info *di = dev_get_drvdata(dev->parent);
    return sprintf(buf, "0x%04x\n", di->sys_device_type);
}
static DEVICE_ATTR(device_type, S_IRUGO, bq27520_get_device_type, NULL);

static ssize_t bq27520_get_fw_version(struct device *dev, struct device_attribute *attr, char *buf)
{
    struct bq27510_device_info *di = dev_get_drvdata(dev->parent);
    return sprintf(buf, "0x%04x\n", di->sys_fw_version);
}
static DEVICE_ATTR(fw_version, S_IRUGO, bq27520_get_fw_version, NULL);

static ssize_t bq27520_get_state_of_health_status(struct device *dev, struct device_attribute *attr, char *buf)
{
    struct bq27510_device_info *di = dev_get_drvdata(dev->parent);
    int ret;
    int bml = 0;

    ret = bq27x10_read(BQ27510_REG_SOH_STATUS, &bml, 1, di);
    if (ret) {
	    dev_err(di->dev, "error reading state of health status\n");
	    return sprintf(buf, "error reading state of health status\n");
    }

    return sprintf(buf, "0x%02x\n", bml);
}

static DEVICE_ATTR(state_of_health_status, S_IRUGO, bq27520_get_state_of_health_status, NULL);

static ssize_t bq27520_get_hw_version(struct device *dev, struct device_attribute *attr, char *buf)
{
    struct bq27510_device_info *di = dev_get_drvdata(dev->parent);
    return sprintf(buf, "0x%04x\n", di->sys_hw_version);
}

static DEVICE_ATTR(hw_version, S_IRUGO, bq27520_get_hw_version, NULL);

static ssize_t bq27520_get_df_version(struct device *dev,
				       struct device_attribute *attr, char *buf)
{
	struct bq27510_device_info *di = dev_get_drvdata(dev->parent);
	int ret;

	if (unlikely(bq27200_read_control(BQ27x00_CONTROL_DF_VERSION,
				   &ret, di) < 0))
		dev_err(di->dev, "failed to read df_version\n");

	return sprintf(buf, "0x%04x\n", ret);
}

static DEVICE_ATTR(df_version, S_IRUGO, bq27520_get_df_version, NULL);

static int bq27x00_get_device_version(struct bq27510_device_info *di)
{
	int ret;

	int retry_counter = 3;

	do {
		ret = bq27200_read_control(BQ27x00_CONTROL_DEVICE_TYPE,
					   &di->sys_device_type, di);
		if (unlikely(ret < 0)) {
			dev_err(di->dev, "failed to read device_type: %d\n", ret);
			bq27x00_hw_reset(di);
		}
	} while (retry_counter-- > 0);


	ret = bq27200_read_control(BQ27x00_CONTROL_FW_VERSION,
				   &di->sys_fw_version, di);
	if (unlikely(ret < 0))
		dev_err(di->dev, "failed to read fw_version: %d\n", ret);

	return ret;
}


/*
 * init batery descriptor.
  */
static int bq27510_powersupply_init(struct bq27510_device_info *di)
{
	int ret = 0;

	di->bat.type = POWER_SUPPLY_TYPE_BATTERY;
	di->bat.properties = bq27510_battery_props;
	di->bat.num_properties = ARRAY_SIZE(bq27510_battery_props);
	di->bat.get_property = bq27510_battery_get_property;
	di->bat.external_power_changed = NULL;

	ret = bq27x00_get_device_version(di);
	if (ret) {
		dev_err(di->dev, "failed to get device version: %d\n", ret);
		return ret;
	}
	return ret;
}

/*
 * init usb descriptor.
  */
static void bq27510_powersupply_usb_init(struct bq27510_device_info *di)
{
	di->usb.type = POWER_SUPPLY_TYPE_USB;
	di->usb.properties = bq27510_usb_props;
	di->usb.num_properties = ARRAY_SIZE(bq27510_usb_props);
	di->usb.get_property = bq27510_usb_get_property;
	di->usb.external_power_changed = NULL;
}

/*
 * init wall descriptor.
  */
static void bq27510_powersupply_wall_init(struct bq27510_device_info *di)
{
	di->wall.type = POWER_SUPPLY_TYPE_MAINS;
	di->wall.properties = bq27510_wall_props;
	di->wall.num_properties = ARRAY_SIZE(bq27510_wall_props);
	di->wall.get_property = bq27510_wall_get_property;
	di->wall.external_power_changed = NULL;
}


/*
 *
 */
static int bq27510_battery_probe(struct i2c_client *client,
				 const struct i2c_device_id *id)
{
	char *name;
	struct bq27510_device_info *di;
	struct bq27x00_platform_data *pdata = NULL;
	int num;
	int retval = 0;

	printk("Probe bq27510.\n");
	pdata = client->dev.platform_data;
	/* Get new ID for the new battery device */
	retval = idr_pre_get(&battery_id, GFP_KERNEL);
	if (retval == 0)
		return -ENOMEM;
	mutex_lock(&battery_mutex);
	retval = idr_get_new(&battery_id, client, &num);
	mutex_unlock(&battery_mutex);
	if (retval < 0)
		return retval;
	name = kasprintf(GFP_KERNEL, "bq27510-%d", num);
	if (!name) {
		dev_err(&client->dev, "failed to allocate device name\n");
		retval = -ENOMEM;
		goto batt_failed_1;
	}

	di = kzalloc(sizeof(*di), GFP_KERNEL);
	if (!di) {
		dev_err(&client->dev, "failed to allocate device info data\n");
		retval = -ENOMEM;
		goto batt_failed_2;
	}
	di->id = num;

	i2c_set_clientdata(client, di);
	di->dev = &client->dev;

	di->bat.name = name;
	di->usb.name = "bq27510-usb";
	di->wall.name = "bq27510-wall";

	di->gpio_ce = pdata->gpio_ce;
	di->gpio_soc_int = pdata->gpio_soc_int;
	di->gpio_bat_low = pdata->gpio_bat_low;
	di->gpio_bat_id = pdata->gpio_bat_id;
	dev_dbg(di->dev, "ce:%d, soc:%d, low:%d\n",di->gpio_ce, di->gpio_soc_int, di->gpio_bat_low);

	init_rwsem(&di->reglock);

	di->client = client;
#if defined(CONFIG_MACH_OMAP3621_GOSSAMER)
	manufacturer_id = get_gossamer_battery_manufacturer(di->gpio_bat_id);
#endif
	bq27510_powersupply_init(di);
	bq27510_powersupply_usb_init(di);
	bq27510_powersupply_wall_init(di);

	retval = power_supply_register(&client->dev, &di->bat);
	if (retval) {
		dev_err(&client->dev, "failed to register battery\n");
		goto batt_failed_3;
	}
	retval = power_supply_register(&client->dev, &di->usb);
	if (retval) {
		dev_err(&client->dev, "failed to register battery(usb)\n");
		power_supply_unregister(&di->bat);
		goto batt_failed_3;
	}
	retval = power_supply_register(&client->dev, &di->wall);
	if (retval) {
		dev_err(&client->dev, "failed to register battery(wall)\n");
		power_supply_unregister(&di->bat);
		power_supply_unregister(&di->usb);
		goto batt_failed_3;
	}


	/* Cache static values */
	retval = bq27200_read_control(BQ27x00_CONTROL_DEVICE_TYPE,
					&di->sys_device_type, di);
	if (retval < 0)
		goto batt_failed_4;
	retval = bq27200_read_control(BQ27x00_CONTROL_HW_VERSION,
					&di->sys_hw_version, di);
	if (retval < 0)
		goto batt_failed_4;
	retval = bq27200_read_control(BQ27x00_CONTROL_FW_VERSION,
					&di->sys_fw_version, di);
	if (retval < 0)
		goto batt_failed_4;

	/* start with valid contents in register cache */
	retval = bq27x10_read_registers(di);
	if (retval < 0)
		goto batt_failed_4;

	INIT_DELAYED_WORK_DEFERRABLE(&di->bat_work, bq27x10_bat_work);
	schedule_delayed_work(&di->bat_work, msecs_to_jiffies(T_POLL_MS));

	local_di = di;
	dev_info(&client->dev, "support ver. %s enabled\n", DRIVER_VERSION);
#if defined(CONFIG_MACH_OMAP3621_GOSSAMER)
	retval = device_create_file(&client->dev, &dev_attr_pause_i2c);
	if (retval)
		printk(KERN_ERR "Failed to create pause_i2c sysfs entry\n");
	retval = device_create_file(&client->dev, &dev_attr_bus_disable);
	if (retval)
		printk(KERN_ERR "Failed to create bus_disable sysfs entry\n");
#endif
    retval = device_create_file(di->bat.dev, &dev_attr_device_type);
    if (retval)
        printk(KERN_ERR "Failed to create evice_type sysfs entry\n");
    retval = device_create_file(di->bat.dev, &dev_attr_hw_version);
    if (retval)
        printk(KERN_ERR "Failed to create hw_version sysfs entry\n");
    retval = device_create_file(di->bat.dev, &dev_attr_fw_version);
    if (retval)
        printk(KERN_ERR "Failed to create fw_version sysfs entry\n");

	retval = device_create_file(di->bat.dev, &dev_attr_hw_reset);
	if (unlikely(retval))
		dev_err(di->dev, "Failed to create hw_reset sysfs: %d\n", retval);

	retval = device_create_file(di->bat.dev, &dev_attr_df_version);
	if (unlikely(retval))
		dev_err(di->dev, "Failed to create df_version sysfs: %d\n",
			retval);
	retval = device_create_file(di->bat.dev, &dev_attr_state_of_health_status);
	if (retval)
		dev_err(di->dev, "Failed to create state_of_health_status sysfs: %d\n",
			retval);

    return 0;

batt_failed_4:
	power_supply_unregister(&di->bat);
	power_supply_unregister(&di->wall);
	power_supply_unregister(&di->usb);
batt_failed_3:
	kfree(di);
batt_failed_2:
	kfree(name);
batt_failed_1:
	mutex_lock(&battery_mutex);
	idr_remove(&battery_id, num);
	mutex_unlock(&battery_mutex);

	return retval;
}

static void bq27510_battery_shutdown(struct i2c_client *client)
{
	struct bq27510_device_info *di = i2c_get_clientdata(client);

	dev_dbg(&client->dev, "shutting down");
	cancel_delayed_work_sync(&di->bat_work);
}

/*
 *
 */
static int bq27510_battery_remove(struct i2c_client *client)
{
	struct bq27510_device_info *di = i2c_get_clientdata(client);

	printk("Remove bq27510.\n");
	bq27510_battery_shutdown(client);

	device_remove_file(di->bat.dev, &dev_attr_hw_reset);
	device_remove_file(di->bat.dev, &dev_attr_df_version);
    device_remove_file(di->bat.dev, &dev_attr_fw_version);
    device_remove_file(di->bat.dev, &dev_attr_hw_version);
    device_remove_file(di->bat.dev, &dev_attr_device_type);
#if defined(CONFIG_MACH_OMAP3621_GOSSAMER)
    device_remove_file(&client->dev, &dev_attr_pause_i2c);
    device_remove_file(&client->dev, &dev_attr_bus_disable);
#endif
	power_supply_unregister(&di->bat);
	power_supply_unregister(&di->usb);
	power_supply_unregister(&di->wall);

	kfree(di->bat.name);

	mutex_lock(&battery_mutex);
	idr_remove(&battery_id, di->id);
	mutex_unlock(&battery_mutex);

	local_di = NULL;
	kfree(di);

	return 0;
}


/*
 * Module stuff
 */

static const struct i2c_device_id bq27510_id[] = {
	{ "bq27510", 0 },
	{},
};

static int bq27510_battery_suspend(struct i2c_client *client, pm_message_t mesg)
{
    struct bq27510_device_info *di = i2c_get_clientdata(client);
    int volts,temp,cap,curr;
#ifdef CONFIG_BATTERY_BQ27520
    int ncap;
    ncap = bq27510_battery_nominal_capacity(di);
#endif /* CONFIG_BATTERY_BQ27520 */

    volts = bq27510_battery_voltage(di);
    curr = bq27510_battery_current(di);

    temp= bq27510_battery_temperature(di) - OFFSET_KELVIN_CELSIUS_DECI;
    cap = bq27510_battery_rsoc(di);
    dev_info(di->dev, "Suspend- %dmV %dmA %d%% NominalAvailableCapacity:%dmAh %d.%dC\n",volts,curr,cap,ncap, temp/10, temp%10);

	return 0;
}

static int bq27510_battery_resume(struct i2c_client *client)
{
    struct bq27510_device_info *di = i2c_get_clientdata(client);
    int volts,temp,cap,curr;
#ifdef CONFIG_BATTERY_BQ27520
    int ncap;
    ncap = bq27510_battery_nominal_capacity(di);
#endif /* CONFIG_BATTERY_BQ27520 */

    volts = bq27510_battery_voltage(di);
    curr = bq27510_battery_current(di);

    temp= bq27510_battery_temperature(di) - OFFSET_KELVIN_CELSIUS_DECI;
    cap = bq27510_battery_rsoc(di);
    dev_info(di->dev, "Resume- %dmV %dmA %d%% NominalAvailableCapacity:%dmAh %d.%dC\n",volts,curr,cap,ncap, temp/10, temp%10);

	cancel_delayed_work_sync(&local_di->bat_work);
	di->rapid_poll_cycle = 0;
	schedule_delayed_work(&di->bat_work,
			msecs_to_jiffies(T_POLL_PLUG_MS));


	return 0;
}



static struct i2c_driver bq27510_battery_driver = {
	.driver = {
		.name = "bq27510-battery",
	},
	.probe = bq27510_battery_probe,
	.remove = bq27510_battery_remove,
	.suspend  = bq27510_battery_suspend,
	.resume	  = bq27510_battery_resume,
	.shutdown = bq27510_battery_shutdown,
	.id_table = bq27510_id,
};

static int __init bq27510_battery_init(void)
{
	int ret;

	ret = i2c_add_driver(&bq27510_battery_driver);
	if (ret)
		printk(KERN_ERR "Unable to register BQ27510 driver\n");

	return ret;
}
module_init(bq27510_battery_init);

static void __exit bq27510_battery_exit(void)
{
	i2c_del_driver(&bq27510_battery_driver);
}
module_exit(bq27510_battery_exit);

MODULE_AUTHOR("Texas Instruments Inc.");
MODULE_DESCRIPTION("BQ27510 battery monitor driver");
MODULE_LICENSE("GPL");

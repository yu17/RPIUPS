// SPDX-License-Identifier: GPL-2.0-only
/* 
 * Power supply driver kernel module for RPi UPSPack V3. (See https://github.com/rcdrones/UPSPACK_V3)
 * 
 * CopyRight 2021  Jiaqi Yu <yjq17@hotmail.com>
 * 
 * Modified from Power supply driver for testing.
 * Copyright 2010  Anton Vorontsov <cbouatmailru@gmail.com>
 *
 * Dynamic module parameter code from the Virtual Battery Driver
 * Copyright (C) 2008 Pylone, Inc.
 * By: Masashi YOKOTA <yokota@pylone.jp>
 * Originally found here:
 * http://downloads.pylone.jp/src/virtual_battery/virtual_battery-0.0.1.tar.bz2
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/power_supply.h>
#include <linux/errno.h>
#include <linux/delay.h>
//#include <linux/vermagic.h>

// Values below are static as non of the following attributes are supported.
#define BAT_TEMPERATURE 260
#define BAT_HEALTH POWER_SUPPLY_HEALTH_GOOD;
#define BAT_TECH POWER_SUPPLY_TECHNOLOGY_LION;

enum ups_powersource_id {
	EXTERNAL,
	BATTERY,
	POWERSOURCE_COUNT,
};

static int external_online = 1;
static int battery_status = POWER_SUPPLY_STATUS_UNKNOWN;
static int battery_percentage = 50;
static int output_voltage = 5250;
static int battery_present = 0; /* false */
static int battery_energy = 3700000;// Default to 10000mAh@3.7V Allowed to be changed via exposed interface.
static int et_charge = -1;
static int et_discharge = -1;


static bool module_initialized;

static int ups_get_external_property(struct power_supply *psy,enum power_supply_property psp,union power_supply_propval *val){
	switch (psp) {
		case POWER_SUPPLY_PROP_ONLINE:
			val->intval = external_online;
			break;
		default:
			return -EINVAL;
	}
	return 0;
}

static int ups_get_battery_property(struct power_supply *psy,enum power_supply_property psp,union power_supply_propval *val){
	switch (psp) {
		case POWER_SUPPLY_PROP_MODEL_NAME:
			val->strval = "RPi UPSPack Standard V3";
			break;
		case POWER_SUPPLY_PROP_MANUFACTURER:
			val->strval = "MakerFocus";
			break;
		case POWER_SUPPLY_PROP_SERIAL_NUMBER:
			val->strval = "0";
			break;
		case POWER_SUPPLY_PROP_PRESENT:
			val->intval = battery_present;
			break;
		case POWER_SUPPLY_PROP_STATUS:
			val->intval = battery_status;
			break;
		case POWER_SUPPLY_PROP_CAPACITY:
			val->intval = battery_percentage;
			break;
		case POWER_SUPPLY_PROP_CHARGE_NOW:
			val->intval = battery_percentage*battery_energy/100;
			break;
		case POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN:
		case POWER_SUPPLY_PROP_CHARGE_FULL:
			val->intval = battery_energy;
			break;
		case POWER_SUPPLY_PROP_VOLTAGE_NOW:
			val->intval = output_voltage;
			break;
		case POWER_SUPPLY_PROP_TIME_TO_EMPTY_AVG:
		case POWER_SUPPLY_PROP_TIME_TO_EMPTY_NOW:
			val->intval = et_discharge;
			break;
		case POWER_SUPPLY_PROP_TIME_TO_FULL_AVG:
		case POWER_SUPPLY_PROP_TIME_TO_FULL_NOW:
			val->intval = et_charge;
			break;
		case POWER_SUPPLY_PROP_CHARGE_TYPE:
			val->intval = POWER_SUPPLY_CHARGE_TYPE_FAST;
			break;
		case POWER_SUPPLY_PROP_CAPACITY_LEVEL:
			val->intval = POWER_SUPPLY_CAPACITY_LEVEL_NORMAL;
			break;
		case POWER_SUPPLY_PROP_TECHNOLOGY:
			val->intval = BAT_TECH;
			break;
		case POWER_SUPPLY_PROP_HEALTH:
			val->intval = BAT_HEALTH;
			break;
		case POWER_SUPPLY_PROP_TEMP:
			val->intval = BAT_TEMPERATURE;
			break;
		default:
			printk(KERN_WARNING "UPS: %s: Power supply property %d unavailable.\n",__func__,psp);
			return -EINVAL;
	}
	return 0;
}

static enum power_supply_property ups_external_props[] = {
	POWER_SUPPLY_PROP_ONLINE,
};

static enum power_supply_property ups_battery_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_CHARGE_TYPE,
	POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN,
	POWER_SUPPLY_PROP_CHARGE_FULL,
	POWER_SUPPLY_PROP_CHARGE_NOW,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_CAPACITY_LEVEL,
	POWER_SUPPLY_PROP_TIME_TO_EMPTY_AVG,
	POWER_SUPPLY_PROP_TIME_TO_FULL_NOW,
	POWER_SUPPLY_PROP_MODEL_NAME,
	POWER_SUPPLY_PROP_MANUFACTURER,
	POWER_SUPPLY_PROP_SERIAL_NUMBER,
	POWER_SUPPLY_PROP_TEMP,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
};

static char *ups_external_supplied_to[] = {
	"battery",
};

static struct power_supply *ups_supplies[POWERSOURCE_COUNT];

static const struct power_supply_desc ups_desc[] = {
	[EXTERNAL] = {
		.name = "external",
		.type = POWER_SUPPLY_TYPE_MAINS,
		.properties = ups_external_props,
		.num_properties = ARRAY_SIZE(ups_external_props),
		.get_property = ups_get_external_property,
	},
	[BATTERY] = {
		.name = "battery",
		.type = POWER_SUPPLY_TYPE_BATTERY,
		.properties = ups_battery_props,
		.num_properties = ARRAY_SIZE(ups_battery_props),
		.get_property = ups_get_battery_property,
	}
};

static const struct power_supply_config ups_configs[] = {
	{
		/* external */
		.supplied_to = ups_external_supplied_to,
		.num_supplicants = ARRAY_SIZE(ups_external_supplied_to),
	},
	{
		/* battery */
	}
};

// Module initialization. Test and start serial communication with UPS module.

static int __init ups_init(void){
	int i;
	int ret;

	BUILD_BUG_ON(POWERSOURCE_COUNT != ARRAY_SIZE(ups_supplies));
	BUILD_BUG_ON(POWERSOURCE_COUNT != ARRAY_SIZE(ups_configs));

	for (i = 0; i < ARRAY_SIZE(ups_supplies); i++) {
		ups_supplies[i] = power_supply_register(NULL,&ups_desc[i],&ups_configs[i]);
		if (IS_ERR(ups_supplies[i])) {
			printk(KERN_ERR "UPS: %s: failed to register %s\n", __func__,ups_desc[i].name);
			ret = PTR_ERR(ups_supplies[i]);
			goto failed;
		}
	}

	module_initialized = true;
	return 0;
failed:
	while (--i >= 0)
		power_supply_unregister(ups_supplies[i]);
	return ret;
}


// Module termination. End serial communication.

static void __exit ups_exit(void){
	int i;

	/* Let's see how we handle changes... */
	external_online = 1;
	battery_status = POWER_SUPPLY_STATUS_UNKNOWN;
	battery_present = 0;
	et_charge = -1;
	et_discharge = -1;
	for (i = 0; i < ARRAY_SIZE(ups_supplies); i++)
		power_supply_changed(ups_supplies[i]);
	printk(KERN_WARNING "UPS: Module unloading. Power parameters reset. Sleep for 1 sec before unregister...\n");
	ssleep(1);

	for (i = 0; i < ARRAY_SIZE(ups_supplies); i++)
		power_supply_unregister(ups_supplies[i]);

	module_initialized = false;
}


#define MAX_KEYLENGTH 256
struct battery_property_map {
	int value;
	char const *key;
};

//static struct battery_property_map map_external_online[] = {
//	{ 0,  "false"  },
//	{ 1,  "true" },
//	{ -1, NULL  },
//};

static struct battery_property_map map_status[] = {
	{ POWER_SUPPLY_STATUS_UNKNOWN,      "unknown"      },
	{ POWER_SUPPLY_STATUS_CHARGING,     "charging"     },
	{ POWER_SUPPLY_STATUS_DISCHARGING,  "discharging"  },
	{ POWER_SUPPLY_STATUS_NOT_CHARGING, "not-charging" },
	{ POWER_SUPPLY_STATUS_FULL,         "full"         },
	{ -1,                               NULL           },
};

//static struct battery_property_map map_present[] = {
//	{ 0,  "false" },
//	{ 1,  "true"  },
//	{ -1, NULL    },
//};

static int map_get_value(struct battery_property_map *map,const char *key,int def_val){
	char buf[MAX_KEYLENGTH];
	int cr;

	strncpy(buf, key, MAX_KEYLENGTH);
	buf[MAX_KEYLENGTH-1] = '\0';

	cr = strnlen(buf, MAX_KEYLENGTH) - 1;
	if (cr < 0)
		return def_val;
	if (buf[cr] == '\n')
		buf[cr] = '\0';

	while (map->key) {
		if (strncasecmp(map->key, buf, MAX_KEYLENGTH) == 0)
			return map->value;
		map++;
	}

	return def_val;
}


static const char *map_get_key(struct battery_property_map *map,int value,const char *def_key){
	while (map->key) {
		if (map->value == value)
			return map->key;
		map++;
	}

	return def_key;
}

static inline void signal_power_supply_changed(struct power_supply *psy){
	if (module_initialized)
		power_supply_changed(psy);
}

//static int param_set_external_online(const char *key, const struct kernel_param *kp){
//	external_online = map_get_value(map_external_online, key, external_online);
//	signal_power_supply_changed(ups_supplies[EXTERNAL]);
//	return 0;
//}
//
//static int param_get_external_online(char *buffer, const struct kernel_param *kp){
//	strcpy(buffer, map_get_key(map_external_online, external_online, "unknown"));
//	return strlen(buffer);
//}

static int param_set_external_online(const char *buffer, const struct kernel_param *kp){
	int ext;

	if (1 != sscanf(buffer, "%d", &ext))
		return -EINVAL;

	if (ext==0||ext==1)	external_online = ext;
	else return -EINVAL;
	signal_power_supply_changed(ups_supplies[BATTERY]);
	return 0;
}

#define param_get_external_online param_get_int

static int param_set_battery_status(const char *key,const struct kernel_param *kp){
	battery_status = map_get_value(map_status, key, battery_status);
	signal_power_supply_changed(ups_supplies[BATTERY]);
	return 0;
}

static int param_get_battery_status(char *buffer, const struct kernel_param *kp){
	strcpy(buffer, map_get_key(map_status, battery_status, "unknown"));
	return strlen(buffer);
}

//static int param_set_battery_present(const char *key,const struct kernel_param *kp){
//	battery_present = map_get_value(map_present, key, battery_present);
//	signal_power_supply_changed(ups_supplies[EXTERNAL]);
//	return 0;
//}
//
//static int param_get_battery_present(char *buffer,const struct kernel_param *kp){
//	strcpy(buffer, map_get_key(map_present, battery_present, "unknown"));
//	return strlen(buffer);
//}

static int param_set_battery_present(const char *buffer,const struct kernel_param *kp){
	int bat;

	if (1 != sscanf(buffer, "%d", &bat))
		return -EINVAL;

	if (bat==0||bat==1)	battery_present = bat;
	else return -EINVAL;
	signal_power_supply_changed(ups_supplies[BATTERY]);
	return 0;
}

#define param_get_battery_present param_get_int

static int param_set_battery_percentage(const char *buffer,const struct kernel_param *kp){
	int cap;

	if (1 != sscanf(buffer, "%d", &cap))
		return -EINVAL;

	if (cap<0||cap>100) return -EINVAL;
	battery_percentage = cap;
	signal_power_supply_changed(ups_supplies[BATTERY]);
	return 0;
}

#define param_get_battery_percentage param_get_int

static int param_set_battery_energy(const char *buffer,const struct kernel_param *kp){
	int cap;

	if (1 != sscanf(buffer, "%d", &cap))
		return -EINVAL;

	battery_energy = cap;
	signal_power_supply_changed(ups_supplies[BATTERY]);
	return 0;
}

#define param_get_battery_energy param_get_int

static int param_set_output_voltage(const char *buffer,const struct kernel_param *kp){
	int vlt;

	if (1 != sscanf(buffer, "%d", &vlt))
		return -EINVAL;

	output_voltage = vlt;
	signal_power_supply_changed(ups_supplies[BATTERY]);
	return 0;
}

#define param_get_output_voltage param_get_int

static int param_set_et_charge(const char *buffer,const struct kernel_param *kp){
	int t;

	if (1 != sscanf(buffer, "%d", &t))
		return -EINVAL;

	et_charge = t;
	signal_power_supply_changed(ups_supplies[BATTERY]);
	return 0;
}

#define param_get_et_charge param_get_int

static int param_set_et_discharge(const char *buffer,const struct kernel_param *kp){
	int t;

	if (1 != sscanf(buffer, "%d", &t))
		return -EINVAL;

	et_discharge = t;
	signal_power_supply_changed(ups_supplies[BATTERY]);
	return 0;
}

#define param_get_et_discharge param_get_int

static const struct kernel_param_ops param_ops_external_online = {
	.set = param_set_external_online,
	.get = param_get_external_online,
};

static const struct kernel_param_ops param_ops_battery_status = {
	.set = param_set_battery_status,
	.get = param_get_battery_status,
};

static const struct kernel_param_ops param_ops_battery_present = {
	.set = param_set_battery_present,
	.get = param_get_battery_present,
};

static const struct kernel_param_ops param_ops_battery_energy = {
	.set = param_set_battery_energy,
	.get = param_get_battery_energy,
};

static const struct kernel_param_ops param_ops_battery_percentage = {
	.set = param_set_battery_percentage,
	.get = param_get_battery_percentage,
};

static const struct kernel_param_ops param_ops_output_voltage = {
	.set = param_set_output_voltage,
	.get = param_get_output_voltage,
};

static const struct kernel_param_ops param_ops_et_charge = {
	.set = param_set_et_charge,
	.get = param_get_et_charge,
};

static const struct kernel_param_ops param_ops_et_discharge = {
	.set = param_set_et_discharge,
	.get = param_get_et_discharge,
};

#define param_check_external_online(name, p) __param_check(name, p, void);
#define param_check_battery_status(name, p) __param_check(name, p, void);
#define param_check_battery_present(name, p) __param_check(name, p, void);
#define param_check_battery_energy(name, p) __param_check(name, p, void);
#define param_check_battery_percentage(name, p) __param_check(name, p, void);
#define param_check_output_voltage(name, p) __param_check(name, p, void);
#define param_check_et_charge(name, p) __param_check(name, p, void);
#define param_check_et_discharge(name, p) __param_check(name, p, void);


module_param(external_online, external_online, 0644);
MODULE_PARM_DESC(external_online, "Charging state <0|1>");

module_param(battery_status, battery_status, 0644);
MODULE_PARM_DESC(battery_status,"battery status <charging|discharging|not-charging|full>");

module_param(battery_present, battery_present, 0644);
MODULE_PARM_DESC(battery_present,"battery presence state <0|1>");

module_param(battery_energy, battery_energy, 0644);
MODULE_PARM_DESC(battery_energy,"battery designed capacity (*0.01mWh)");

module_param(battery_percentage, battery_percentage, 0644);
MODULE_PARM_DESC(battery_percentage, "battery percentage (0-100)");

module_param(output_voltage, output_voltage, 0644);
MODULE_PARM_DESC(output_voltage, "output voltage (millivolts)");

module_param(et_charge, et_charge, 0644);
MODULE_PARM_DESC(et_charge, "estimated charging time (seconds)");

module_param(et_discharge, et_discharge, 0644);
MODULE_PARM_DESC(et_discharge, "estimated charging time (seconds)");

MODULE_DESCRIPTION("Power supply kernel driver for Raspberry Pi UPSPack V3.");
MODULE_AUTHOR("Jiaqi Yu <yjq17@hotmail.com>");
MODULE_LICENSE("GPL v2");
MODULE_VERSION("0.1");

module_init(ups_init);
module_exit(ups_exit);
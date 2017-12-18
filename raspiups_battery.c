/* drivers/power/RaspiUPS_battery.c
 * RaspiUPS battery driver
 * http://www.raspberrypiwiki.com/index.php/Raspi_UPS_HAT_Board
 * --(1)--Save this file in "/drivers/power/"
 * --(2)--Define device under i2c1 devicetree in "arch/arm/boot/dts/rk3288-miniarm.dts" e.g
 * &i2c1 {
 *       raspiups@36 {
 *               reg = <0x36>;
 *       };
 * };
 * --(3)--Add following line to /drivers/power/Makefile
 * obj-$(CONFIG_RASPIUPS_BATTERY)  += raspiups_battery.o
 * --(4)--Add following to ./Kconfig
 * config RASPIUPS_BATTERY
 *       tristate "RaspiUPS Battery HAT"
 *       default y
 *       help
 *         say Y to enable support for the RaspiUPS HAT
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/param.h>
#include <linux/jiffies.h>
#include <linux/workqueue.h>
#include <linux/delay.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/idr.h>
#include <linux/i2c.h>
#include <asm/unaligned.h>
#include <linux/swab.h>
#include <linux/slab.h>

#define DRIVER_VERSION			"1.0.0"

#define BATTERY_CAPACITY_MAH    2500 //  define battery capacity mah

#define RaspiUPS_REG_RSOCL		0x04 // Relative State-of-Charge/Capacity
#define RaspiUPS_REG_VOLTL		0x02

#define RaspiUPS_SPEED 	100000

struct RaspiUPS_device_info {
	struct device 		*dev;
	struct power_supply	bat;
	struct delayed_work work;
	unsigned int interval;
	struct i2c_client	*client;
};

static enum power_supply_property RaspiUPS_battery_props[] = {
	//POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_CAPACITY,
};

/*
 * Return the battery Voltage in millivolts
 * Or < 0 if something fails.
 */
static int RaspiUPS_battery_voltage(struct RaspiUPS_device_info *di)
{
   long decimal_number;
   int lvl = 78.125f;
   int mv = 1000000;
   char hex_number = i2c_smbus_read_word_swapped(di->client,RaspiUPS_REG_VOLTL);
   int ret = kstrtol(&hex_number,0,&decimal_number);
   if (ret){
   return (int)(decimal_number * lvl) / mv;  //voltage (mV)
   } else {return ret;}
}

/*
 * Return the battery Relative State-of-Charge
 * Or < 0 if something fails.
 */
static int RaspiUPS_battery_rsoc(struct RaspiUPS_device_info *di)
{
   long decimal_number;
   char hex_number = i2c_smbus_read_word_swapped(di->client,RaspiUPS_REG_VOLTL);
   int ret = kstrtol(&hex_number,0,&decimal_number);
   if(ret){
   return decimal_number / 256;  //charge capacity (%)
   } else {return ret;}
}
/*
//Get whether the Battery is charging by comparing voltage with previous value
//If Higher than previous then it is charging.
static int dc_charge_status(struct RaspiUPS_device_info *di)
{
        int volts;
        volts = (i2c_smbus_read_word_swapped(di->client,RaspiUPS_REG_VOLTL) * 78.125) / 1000000;
	if(volts > (di.get_property(POWER_SUPPLY_PROP_VOLTAGE_NOW)))
		return POWER_SUPPLY_STATUS_CHARGING;
	else
		return POWER_SUPPLY_STATUS_NOT_CHARGING;
}
*/
static int RaspiUPS_battery_get_property(struct power_supply *psy,
					enum power_supply_property psp,
					union power_supply_propval *val)
{
	struct RaspiUPS_device_info *di = container_of(psy, struct RaspiUPS_device_info, bat);

	switch (psp) {
	/*case POWER_SUPPLY_PROP_STATUS:
		val->intval = dc_charge_status(di);
		break;*/
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		val->intval = RaspiUPS_battery_voltage(di);
		break;
	case POWER_SUPPLY_PROP_CAPACITY:
		val->intval = RaspiUPS_battery_rsoc(di);
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static void RaspiUPS_powersupply_init(struct RaspiUPS_device_info *di)
{
	di->bat.type = POWER_SUPPLY_TYPE_BATTERY;
	di->bat.properties = RaspiUPS_battery_props;
	di->bat.num_properties = ARRAY_SIZE(RaspiUPS_battery_props);
	di->bat.get_property = RaspiUPS_battery_get_property;
	di->bat.external_power_changed = NULL;
}

static void RaspiUPS_battery_update_status(struct RaspiUPS_device_info *di)
{
	power_supply_changed(&di->bat);
}

static void RaspiUPS_battery_work(struct work_struct *work)
{
	struct RaspiUPS_device_info *di = container_of(work, struct RaspiUPS_device_info, work.work); 

	RaspiUPS_battery_update_status(di);
	/* reschedule for the next time */
	schedule_delayed_work(&di->work, di->interval);
}

static int RaspiUPS_battery_probe(struct i2c_client *client,
				 const struct i2c_device_id *id)
{
	struct RaspiUPS_device_info *di;
	int retval = 0;
	//u8 regs[2] = {0x10,0x1d};  ///init regs mode ctrl

	di = kzalloc(sizeof(*di), GFP_KERNEL);
	if (!di) {
		dev_err(&client->dev, "failed to allocate device info data\n");
		retval = -ENOMEM;
		goto batt_failed_2;
	}

	i2c_set_clientdata(client, di);
	di->dev = &client->dev;
	di->bat.name = "RaspiUPS-battery";
	di->client = client;
	/* 4 seconds between monitor runs interval */
	di->interval = msecs_to_jiffies(4 * 1000);
	RaspiUPS_powersupply_init(di);

	retval = power_supply_register(&client->dev, &di->bat);
	if (retval) {
		dev_err(&client->dev, "failed to register battery\n");
		goto batt_failed_3;
	}
	
	INIT_DELAYED_WORK(&di->work, RaspiUPS_battery_work);
	schedule_delayed_work(&di->work, di->interval);
	
	dev_info(&client->dev, "support ver. %s enabled\n", DRIVER_VERSION);

	return 0;

batt_failed_3:
	kfree(di);
batt_failed_2:
	return retval;
}

static int RaspiUPS_battery_remove(struct i2c_client *client)
{
	struct RaspiUPS_device_info *di = i2c_get_clientdata(client);

	cancel_delayed_work_sync(&di->work);
	power_supply_unregister(&di->bat);

	kfree(di->bat.name);

	kfree(di);

	return 0;
}

/*
 * Module stuff
 */

static const struct i2c_device_id RaspiUPS_id[] = {
	{ "RaspiUPS", 0 },
	{},
};

static struct i2c_driver RaspiUPS_battery_driver = {
	.driver = {
		.name = "RaspiUPS-battery",
	},
	.probe = RaspiUPS_battery_probe,
	.remove = RaspiUPS_battery_remove,
	.id_table = RaspiUPS_id,
};

static int __init RaspiUPS_battery_init(void)
{
	int ret;

	ret = i2c_add_driver(&RaspiUPS_battery_driver);
	if (ret)
		printk(KERN_ERR "Unable to register RaspiUPS driver\n");

	return ret;
}
module_init(RaspiUPS_battery_init);

static void __exit RaspiUPS_battery_exit(void)
{
	i2c_del_driver(&RaspiUPS_battery_driver);
}
module_exit(RaspiUPS_battery_exit);

MODULE_AUTHOR("lostangel556");
MODULE_DESCRIPTION("RaspiUPS battery monitor driver");
MODULE_LICENSE("GPL");

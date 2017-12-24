#include <linux/module.h>
#include <linux/param.h>
#include <linux/jiffies.h>
#include <linux/workqueue.h>
#include <linux/delay.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/idr.h>
#include <linux/i2c.h>
#include <asm/unaligned.h>
#include <linux/gpio.h>
#include <linux/slab.h>

#define DRIVER_VERSION			"1.0.0"

#define BATTERY_CAPACITY_MAH   2500   ///  define battery capacity mah
#define RSENSE_RESISTANCE		10     ///  Rsense resistance m

#define MAX17048_REG_MODE		0x06
#define MAX17048_REG_SOC		0x04 // Relative State-of-Charge
#define MAX17048_REG_ROC 		0x16 //Charge or Discharge Rate
#define MAX17048_REG_VOLT		0x02 //Current Voltage
#define MAX17048_REG_STATUS             0x1A // Status

#define MAX17048_SPEED 	100000

struct MAX17048_device_info {
	struct device 		*dev;
	struct power_supply	bat;
	struct delayed_work work;
	unsigned int interval;
	struct i2c_client	*client;
};

static int i2c_master_reg8_send(const struct i2c_client *client, const char reg, const char *buf, int count, int scl_rate)
{
    struct i2c_adapter *adap=client->adapter;
    struct i2c_msg msg;
    int ret;
    char *tx_buf = (char *)kzalloc(count + 1, GFP_KERNEL);
    if(!tx_buf)
        return -ENOMEM;
    tx_buf[0] = reg;
    memcpy(tx_buf+1, buf, count); 
    msg.addr = client->addr;
    msg.flags = client->flags;
    msg.len = count + 1;
    msg.buf = (char *)tx_buf;
    msg.scl_rate = scl_rate;
    ret = i2c_transfer(adap, &msg, 1); 
    kfree(tx_buf);
    return (ret == 1) ? count : ret;
}

static int MAX17048_write_regs(struct i2c_client *client, u8 reg, u8 const buf[], __u16 len)
{
	int ret; 
	ret = i2c_master_reg8_send(client, reg, buf, (int)len, MAX17048_SPEED);
	return ret;
}

/*
 * Return the battery Voltage in millivolts mV
 * Or < 0 if something fails.
 */
int battery_charge;
static int MAX17048_battery_voltage(struct MAX17048_device_info *di)
{
	int ret;
        long div;
        div = 78.125;
        
        ret = i2c_smbus_read_word_swapped(di->client, MAX17048_REG_VOLT) * div;
	if (ret<0) {
		dev_err(di->dev, "MAX17048 error reading voltage\n");
		return ret;
	}
	battery_charge = ret;
	//printk(KERN_INFO "MAX17048 VOLTAGE: %dmV\n", ret );
	return ret /1000;   //voltage (uV) = Voltage_code * 78.125
}

static int MAX17048_battery_capacity(struct MAX17048_device_info *di)
{
	int ret;
        
        ret = i2c_smbus_read_word_swapped(di->client, MAX17048_REG_SOC) / 256;
	if (ret<0) {
		dev_err(di->dev, "MAX17048 error reading capacity\n");
		return ret;
	}
        //printk(KERN_INFO "MAX17048 CAPACITY: %d percent\n", ret);
	if(ret>100)return 100;
	else
	return ret;    ////capacity = swapped hex / 256).
}

static int dc_charge_status(struct MAX17048_device_info *di)
{
         if(MAX17048_battery_voltage(di) > battery_charge)
		return POWER_SUPPLY_STATUS_CHARGING;
	else
		return POWER_SUPPLY_STATUS_DISCHARGING;
}

static enum power_supply_property MAX17048_battery_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_PRESENT,
	12, //POWER_SUPPLY_PROP_VOLTAGE_NOW
        POWER_SUPPLY_PROP_TECHNOLOGY,
        POWER_SUPPLY_PROP_HEALTH,
        39, //POWER_SUPPLY_PROP_CAPACITY
	53, //POWER_SUPPLY_PROP_TYPE,
	POWER_SUPPLY_PROP_CHARGE_TYPE,
	POWER_SUPPLY_PROP_ONLINE,
	54, //POWER_SUPPLY_PROP_SCOPE
};

static int MAX17048_battery_get_property(struct power_supply *psy,
					enum power_supply_property psp,
					union power_supply_propval *val)
{
	struct MAX17048_device_info *di = container_of(psy, struct MAX17048_device_info, bat);
	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		val->intval = dc_charge_status(di);
		break;
	case 12: //POWER_SUPPLY_PROP_VOLTAGE_NOW
		val->intval = MAX17048_battery_voltage(di);
		break;
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = 1;
		break;
        case POWER_SUPPLY_PROP_TECHNOLOGY:
                val->intval = POWER_SUPPLY_TECHNOLOGY_LIPO;
                break;
        case POWER_SUPPLY_PROP_HEALTH:
                val->intval = POWER_SUPPLY_HEALTH_GOOD;
                break;
        case 39: //POWER_SUPPLY_PROP_CAPACITY
                val->intval = MAX17048_battery_capacity(di);
                break;
	case 53: //POWER_SUPPLY_PROP_TYPE:
		val->intval = 1; //POWER_SUPPLY_TYPE_BATTERY
		break;
	case 54: //POWER_SUPPLY_PROP_SCOPE:
		val->intval = 2; //POWER_SUPPLY_SCOPE_DEVICE
		break;
	case POWER_SUPPLY_PROP_CHARGE_TYPE:
		val->intval = POWER_SUPPLY_CHARGE_TYPE_FAST;
		break;
	case POWER_SUPPLY_PROP_ONLINE:
		val->intval = 4;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static void MAX17048_powersupply_init(struct MAX17048_device_info *di)
{
	di->bat.type = POWER_SUPPLY_TYPE_BATTERY;
	di->bat.properties = MAX17048_battery_props;
	di->bat.num_properties = ARRAY_SIZE(MAX17048_battery_props);
	di->bat.get_property = MAX17048_battery_get_property;
	di->bat.external_power_changed = NULL;
}

static void MAX17048_battery_update_status(struct MAX17048_device_info *di)
{
	power_supply_changed(&di->bat);
}

static void MAX17048_battery_work(struct work_struct *work)
{
	struct MAX17048_device_info *di = container_of(work, struct MAX17048_device_info, work.work); 

	MAX17048_battery_update_status(di);
	/* reschedule for the next time */
	schedule_delayed_work(&di->work, di->interval);
}

static int MAX17048_battery_probe(struct i2c_client *client,
				 const struct i2c_device_id *id)
{
	struct MAX17048_device_info *di;
	struct platform_device *pd;
	int retval = 0;
	u8 regs[2] = {0x06,0x00};  ///init regs mode ctrl

	di = kzalloc(sizeof(*di), GFP_KERNEL);
	if (!di) {
		dev_err(&client->dev, "failed to allocate device info data\n");
		retval = -ENOMEM;
		goto batt_failed_2;
	}
	
	pd = platform_device_register_simple("battery", 0, NULL, 0);

	i2c_set_clientdata(client, di);
	di->dev = &client->dev;
	di->bat.name = "battery";
	di->bat.use_for_apm = 1;
	di->client = client;
	MAX17048_write_regs(client, MAX17048_REG_MODE, regs, 2);
	/* 4 seconds between monitor runs interval */
	di->interval = msecs_to_jiffies(4 * 1000);
	MAX17048_powersupply_init(di);

	retval = power_supply_register(&client->dev, &di->bat);
	if (retval) {
		dev_err(&client->dev, "failed to register battery\n");
		goto batt_failed_3;
	}
	
	INIT_DELAYED_WORK(&di->work, MAX17048_battery_work);
	schedule_delayed_work(&di->work, di->interval);
	
	dev_info(&client->dev, "support ver. %s enabled\n", DRIVER_VERSION);

	return 0;

batt_failed_3:
	kfree(di);
batt_failed_2:
	return retval;
}

static int MAX17048_battery_remove(struct i2c_client *client)
{
	struct MAX17048_device_info *di = i2c_get_clientdata(client);

	cancel_delayed_work_sync(&di->work);
	power_supply_unregister(&di->bat);

	kfree(di->bat.name);

	kfree(di);

	return 0;
}

/*
 * Module stuff
 */

static const struct i2c_device_id MAX17048_id[] = {
	{ "MAX17048", 0 },
	{},
};
MODULE_DEVICE_TABLE(i2c,MAX17048_id);

static struct i2c_driver MAX17048_battery_driver = {
	.driver = {
		.name = "MAX17048",
	},
	.probe = MAX17048_battery_probe,
	.remove = MAX17048_battery_remove,
	.id_table = MAX17048_id,
};

static int __init MAX17048_battery_init(void)
{
	int ret;

	ret = i2c_add_driver(&MAX17048_battery_driver);
	if (ret){
		printk(KERN_ERR "Unable to register MAX17048 driver\n");}else{
	printk(KERN_ERR "MAX17048 driver registered\n");}
	return ret;
}
module_init(MAX17048_battery_init);

static void __exit MAX17048_battery_exit(void)
{
	i2c_del_driver(&MAX17048_battery_driver);
}
module_exit(MAX17048_battery_exit);

MODULE_AUTHOR("Lostangel556");
MODULE_DESCRIPTION("Geekworm Raspberry Pi UPS Hat");
MODULE_LICENSE("GPL");

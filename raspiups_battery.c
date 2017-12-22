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

#define RASPIUPS_REG_MODE		0x06
#define RASPIUPS_REG_SOC		0x04 // Relative State-of-Charge
#define RASPIUPS_REG_ROC 		0x16 //Charge or Discharge Rate
#define RASPIUPS_REG_VOLT		0x02 //Current Voltage
#define RASPIUPS_REG_STATUS             0x1A // Status

#define RASPIUPS_SPEED 	100000

struct RaspiUPS_device_info {
	struct device 		*dev;
	struct power_supply	bat;
	struct delayed_work work;
	unsigned int interval;
	struct i2c_client	*client;
};

static enum power_supply_property RaspiUPS_battery_props[] = {
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_CURRENT_NOW,
        POWER_SUPPLY_TYPE_BATTERY,
        POWER_SUPPLY_PROP_CHARGE_TYPE,
        POWER_SUPPLY_PROP_TECHNOLOGY,
        POWER_SUPPLY_PROP_HEALTH,
        POWER_SUPPLY_PROP_CHARGE_COUNTER,
        POWER_SUPPLY_PROP_CHARGE_FULL,
        POWER_SUPPLY_PROP_CAPACITY,
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

static int RaspiUPS_write_regs(struct i2c_client *client, u8 reg, u8 const buf[], __u16 len)
{
	int ret; 
	ret = i2c_master_reg8_send(client, reg, buf, (int)len, RASPIUPS_SPEED);
	return ret;
}

/*
 * Return the battery Voltage in millivolts mV
 * Or < 0 if something fails.
 */
static int RaspiUPS_battery_voltage(struct RaspiUPS_device_info *di)
{
	int ret;
        long div;
        div = 78.125;
        
        ret = i2c_smbus_read_word_swapped(di->client, RASPIUPS_REG_VOLT);// * div;
	if (ret<0) {
		dev_err(di->dev, "RASPI error reading voltage\n");
		return ret;
	}
	return (ret * div);   //voltage (mV) = Voltage_code * 78.125.
}

/*
 * Return the battery Relative State-of-Charge
 * Or < 0 if something fails.
 */
static int RaspiUPS_battery_charge_counter(struct RaspiUPS_device_info *di)
{
	int ret;
        
	ret = i2c_smbus_read_word_swapped(di->client, RASPIUPS_REG_SOC);
	if (ret<0) {
		dev_err(di->dev, "error reading relative State-of-Charge\n");
		return ret;
	}
        //Convert Battery Capacity from mA  then work out what is left from percentage from battery
        //((BATTERY_CAPACITY_MAH) / 100) * (ret / 256) // Returns the amount of battery remaining in µA
        printk(KERN_INFO "RASPI CHARGE: %d\n", ret ); 
	return ret; //charge data (mA). 
}

//Battery Rate of Charge/Discharge
static int RaspiUPS_battery_current(struct RaspiUPS_device_info *di)
{
	int ret;
        long div;
        div = 0.208;
        
        ret = i2c_smbus_read_word_data(di->client, RASPIUPS_REG_ROC);
	if (ret<0) {
		dev_err(di->dev, "RASPI error reading rate of change/current\n");
		return ret;
	}
        printk(KERN_INFO "RASPI ROC: %d\n", ret);
	return ret;    ////current data (mA.h) = (charge_code * 0.208).
}

static int RaspiUPS_battery_capacity(struct RaspiUPS_device_info *di)
{
	int ret;
        
        ret = i2c_smbus_read_word_swapped(di->client, RASPIUPS_REG_SOC);
	if (ret<0) {
		dev_err(di->dev, "RASPI error reading capacity\n");
		return ret;
	}
        //printk(KERN_INFO "RASPI ROC: %d\n", ret);
	return ret / 256;    ////capacity = swapped hex / 256).
}


int battery_current;
static int dc_charge_status(struct RaspiUPS_device_info *di)
{
        int dc;
        
        dc = RaspiUPS_battery_capacity(di);
    
        if(dc > battery_current)
		return POWER_SUPPLY_STATUS_CHARGING;
	else
		return POWER_SUPPLY_STATUS_NOT_CHARGING;
        battery_current = dc;
        
}

static int RaspiUPS_battery_get_property(struct power_supply *psy,
					enum power_supply_property psp,
					union power_supply_propval *val)
{
	struct RaspiUPS_device_info *di = container_of(psy, struct RaspiUPS_device_info, bat);

	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		val->intval = dc_charge_status(di);
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		val->intval = RaspiUPS_battery_voltage(di);
	case POWER_SUPPLY_PROP_PRESENT:
		if (psp == POWER_SUPPLY_PROP_PRESENT)
			val->intval = 1;
		break;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		val->intval = RaspiUPS_battery_current(di);
		break;
        case POWER_SUPPLY_PROP_CHARGE_COUNTER:
                val->intval = RaspiUPS_battery_charge_counter(di);
                break;
        case POWER_SUPPLY_PROP_CHARGE_TYPE:
                val->intval = POWER_SUPPLY_CHARGE_TYPE_TRICKLE;
                break;
        case POWER_SUPPLY_PROP_TECHNOLOGY:
                val->intval = POWER_SUPPLY_TECHNOLOGY_LIPO;
                break;
        case POWER_SUPPLY_PROP_HEALTH:
                val->intval = POWER_SUPPLY_HEALTH_GOOD;
                break;
            case POWER_SUPPLY_PROP_CHARGE_FULL:
                val->intval = BATTERY_CAPACITY_MAH;
                break;
            case POWER_SUPPLY_PROP_CAPACITY:
                val->intval = RaspiUPS_battery_capacity(di);
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
	u8 regs[2] = {0x06,0x00};  ///init regs mode ctrl

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
	RaspiUPS_write_regs(client, RASPIUPS_REG_MODE, regs, 2);
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
MODULE_DEVICE_TABLE(i2c,RaspiUPS_id);

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

MODULE_AUTHOR("Lostangel556");
MODULE_DESCRIPTION("Geekworm Raspberry Pi UPS Hat");
MODULE_LICENSE("GPL");

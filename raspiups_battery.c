/* drivers/power/RaspiUPS_battery.c
 * RaspiUPS battery driver
 * http://www.raspberrypiwiki.com/index.php/Raspi_UPS_HAT_Board
 * --(1)--Save this file in "/drivers/power/"
 * --(2)--Define device under i2c1 devicetree in "arch/arm/boot/dts/rk3288-miniarm.dts" e.g
 * &i2c1 {
 *       RaspiUPS@36 {
 *              compatible = "RaspiUPS";
 *              reg = <0x36>;
 *      };
 * };
 * --(3)--Add following line to /drivers/power/Makefile
 * obj-$(CONFIG_RASPIUPS_BATTERY)  += raspiups_battery.o
 * --(4)--Add following to ./Kconfig
 * config RASPIUPS_BATTERY
 *       tristate "RaspiUPS Battery"
 *       depends on I2C
 *       default y
 *       help
 *         say Y to enable support for the Geekworm Rasp Pi Battery Hat
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
#include <asm/unaligned.h>
#include <linux/gpio.h>
#include <linux/slab.h>

#define DRIVER_VERSION			"1.0.0"

#define BATTERY_CAPACITY_MAH   2500   ///  define battery capacity mah
#define RSENSE_RESISTANCE		10     ///  Rsense resistance m

#define RASPIUPS_REG_MODE		0x06
#define RASPIUPS_REG_RSOC		0x04 // Relative State-of-Charge
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

static int i2c_master_reg8_recv(const struct i2c_client *client, const char reg, char *buf, int count, int scl_rate)
{
    struct i2c_adapter *adap=client->adapter;
    struct i2c_msg msgs[2];
    int ret;
    char reg_buf = reg;
    msgs[0].addr = client->addr;
    msgs[0].flags = client->flags;
    msgs[0].len = 1;
    msgs[0].buf = &reg_buf;
    msgs[0].scl_rate = scl_rate;
    msgs[1].addr = client->addr;
    msgs[1].flags = client->flags | I2C_M_RD;
    msgs[1].len = count;
    msgs[1].buf = (char *)buf;
    msgs[1].scl_rate = scl_rate;
    ret = i2c_transfer(adap, msgs, 2); 
    return (ret == 2)? count : ret;
}

static int RaspiUPS_write_regs(struct i2c_client *client, u8 reg, u8 const buf[], __u16 len)
{
	int ret; 
	ret = i2c_master_reg8_send(client, reg, buf, (int)len, RASPIUPS_SPEED);
	return ret;
}
/*
 * Common code for RASPIUPS devices read
 */
static int RaspiUPS_read_regs(struct i2c_client *client, u8 reg, u8 buf[], unsigned len)
{
	int ret; 
        
	ret = i2c_master_reg8_recv(client, reg, buf, len, RASPIUPS_SPEED);
	return ret; 
}

/*
 * Return the battery Voltage in milivolts
 * Or < 0 if something fails.
 */
static int RaspiUPS_battery_voltage(struct RaspiUPS_device_info *di)
{
	int ret;
        long div;
        div = 0.078125;
        
        ret = i2c_smbus_read_word_swapped(di->client, RASPIUPS_REG_VOLT);// * div;
	if (ret<0) {
		dev_err(di->dev, "RASPI error reading voltage\n");
		return ret;
	}
        printk(KERN_INFO "RASPI VOLTS: %d\n", ret);
	return ret;   //voltage (mV) = Voltage_code * 0.078125.
}

/*
 * Return the battery Relative State-of-Charge
 * Or < 0 if something fails.
 */
static int RaspiUPS_battery_rsoc(struct RaspiUPS_device_info *di)
{
	int ret;

	ret = i2c_smbus_read_word_swapped(di->client, RASPIUPS_REG_RSOC);// / 25600) * BATTERY_CAPACITY_MAH;
	if (ret<0) {
		dev_err(di->dev, "error reading relative State-of-Charge\n");
		return ret;
	}
        printk(KERN_INFO "RASPI RSOC: %d\n", ret);
	return ret;    ////charge data (mAh) = charge_code / 256.
}

static int RaspiUPS_battery_current(struct RaspiUPS_device_info *di)
{
	int ret;
        long div;
        div = 0.208;
        
        ret = /*BATTERY_CAPACITY_MAH / */ i2c_smbus_read_word_swapped(di->client, RASPIUPS_REG_ROC);//) * div);
	//ret = RaspiUPS_read_regs(di->client,RASPIUPS_REG_ROC,regs,2);
	if (ret<0) {
		dev_err(di->dev, "RASPI error reading rate of change/current\n");
		return ret;
	}
        printk(KERN_INFO "RASPI ROC: %d\n", ret);
	return ret;    ////current data (mA.h) = capacity / (charge_code * 0.208).
}

static int dc_charge_status(struct RaspiUPS_device_info *di)
{
        int ret, dc;
	u8 regs[2];
	long div;
        div =0.208;
        ret = RaspiUPS_read_regs(di->client,RASPIUPS_REG_ROC,regs,2);
        
        dc = printk("%d", i2c_smbus_read_word_swapped(di->client, RASPIUPS_REG_VOLT)) * div;
    
        if(dc > 0)
		return POWER_SUPPLY_STATUS_CHARGING;
	else
		return POWER_SUPPLY_STATUS_NOT_CHARGING;
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
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = RaspiUPS_battery_voltage(di);
		if (psp == POWER_SUPPLY_PROP_PRESENT)
			val->intval = val->intval <= 0 ? 0 : 1;
		break;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		val->intval = RaspiUPS_battery_current(di);
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

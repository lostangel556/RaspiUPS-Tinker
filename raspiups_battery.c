#include <linux/module.h>
#include <linux/err.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/types.h>
#include <linux/pci.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/i2c.h>

#define MAX17048_REG_SOC 0x04
#define MAX17048_REG_VOLT 0x02


static struct platform_device *MAX17048_pdev;
struct i2c_client	*my_i2c_client;
int AC_ONLINE;

static int MAX17048_ac_get_property(struct power_supply *psy,
		enum power_supply_property psp,
		union power_supply_propval *val)
{
	int ret = 0;

	switch (psp) {
	case POWER_SUPPLY_PROP_ONLINE:
		val->intval = AC_ONLINE;
		break;
	default:
		ret = -EINVAL;
		break;
	}
	return ret;
}

static int MAX17048_bat_get_property(struct power_supply *psy,
				enum power_supply_property psp,
				union power_supply_propval *val)
{
    int ret = 0;
    int volts;
    switch (psp) {
        case POWER_SUPPLY_PROP_STATUS:
            if (volts < (int)i2c_smbus_read_word_swapped(my_i2c_client, MAX17048_REG_VOLT))
            {
            val->intval = POWER_SUPPLY_STATUS_CHARGING;
            AC_ONLINE = 1;
            }
            else{
            val->intval = POWER_SUPPLY_STATUS_DISCHARGING;
            AC_ONLINE = 0;
            }
            break;
        case POWER_SUPPLY_PROP_HEALTH:
            val->intval = POWER_SUPPLY_HEALTH_GOOD;
            break;
        case POWER_SUPPLY_PROP_PRESENT:
            val->intval = 1;
            break;
        case POWER_SUPPLY_PROP_TECHNOLOGY:
            val->intval = POWER_SUPPLY_TECHNOLOGY_LION;
            break;
        case POWER_SUPPLY_PROP_CAPACITY:
            val->intval = (int)i2c_smbus_read_word_swapped(my_i2c_client, MAX17048_REG_SOC) / 256;
            break;
        case POWER_SUPPLY_PROP_TEMP:
            val->intval = 20;
            break;
        case POWER_SUPPLY_PROP_VOLTAGE_NOW:
            volts = (int)i2c_smbus_read_word_swapped(my_i2c_client, MAX17048_REG_VOLT);
            val->intval = volts;
            break;
        default:
            ret = -EINVAL;
            break;
    }
    return ret;
}

static enum power_supply_property MAX17048_battery_props[] = {
		POWER_SUPPLY_PROP_STATUS,
		POWER_SUPPLY_PROP_HEALTH,
		POWER_SUPPLY_PROP_PRESENT,
		POWER_SUPPLY_PROP_TECHNOLOGY,
		POWER_SUPPLY_PROP_VOLTAGE_NOW,
		POWER_SUPPLY_PROP_TEMP,
		POWER_SUPPLY_PROP_CAPACITY,
};

static enum power_supply_property MAX17048_ac_props[] = {
		POWER_SUPPLY_PROP_ONLINE,
};

static struct power_supply MAX17048_bat = {
		.name = "battery",
		.type = POWER_SUPPLY_TYPE_BATTERY,
		.properties = MAX17048_battery_props,
		.num_properties = ARRAY_SIZE(MAX17048_battery_props),
		.get_property = MAX17048_bat_get_property,
		.use_for_apm = 1,
};

static struct power_supply MAX17048_ac = {
		.name = "ac",
		.type = POWER_SUPPLY_TYPE_MAINS,
		.properties =  MAX17048_ac_props,
		.num_properties = ARRAY_SIZE(MAX17048_ac_props),
		.get_property = MAX17048_ac_get_property,
};

static int MAX17048_battery_probe(struct i2c_client *client,
				 const struct i2c_device_id *id)
{
    my_i2c_client = client;
    return 0;
}

static const struct i2c_device_id MAX17048_id[] = {
	{ "max17048", 0 },
	{},
};
MODULE_DEVICE_TABLE(i2c,MAX17048_id);

static struct i2c_driver MAX17048_battery_driver = {
	.driver = {
		.name = "max17048",
	},
	.probe = MAX17048_battery_probe,
	.id_table = MAX17048_id,
};

static int __init MAX17048_init(void)
{
		int ret = 0;

		MAX17048_pdev = platform_device_register_simple("battery", 0, NULL, 0);
		if (IS_ERR(MAX17048_pdev))return PTR_ERR(MAX17048_pdev);
                
                i2c_add_driver(&MAX17048_battery_driver);
                
		ret = power_supply_register(&MAX17048_pdev->dev, &MAX17048_bat);
		if (ret)
				goto bat_failed;

		ret = power_supply_register(&MAX17048_pdev->dev, &MAX17048_ac);
		if (ret)
				goto ac_failed;

		pr_info("MAX17048: android battery driver loaded\n");
		goto success;

bat_failed:
		power_supply_unregister(&MAX17048_bat);
ac_failed:
		power_supply_unregister(&MAX17048_ac);
		platform_device_unregister(MAX17048_pdev);
success:
		return ret;
}

static void __exit MAX17048_exit(void)
{
		power_supply_unregister(&MAX17048_bat);
		power_supply_unregister(&MAX17048_ac);
		platform_device_unregister(MAX17048_pdev);
                i2c_del_driver(&MAX17048_battery_driver);
		pr_info("MAX17048: android battery driver unloaded\n");
}

module_init(MAX17048_init);
module_exit(MAX17048_exit);

MODULE_AUTHOR("Lostangel556");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Android MAX17048 battery driver");

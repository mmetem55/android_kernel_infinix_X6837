// SPDX-License-Identifier: GPL-2.0

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/types.h>
#include <linux/wait.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/sched.h>
#include <linux/poll.h>
#include <linux/device.h>
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/platform_device.h>
#include <linux/cdev.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/time.h>
#include <linux/io.h>
#include <linux/uaccess.h>
#include <linux/hrtimer.h>
#include <linux/ktime.h>
#include <linux/version.h>
#include <linux/mutex.h>
#include <linux/i2c.h>
#include <linux/of.h>
#include <linux/regulator/consumer.h>

#include "flashlight-core.h"
#include "flashlight-dt.h"
#include "ocp81375.h"

static const int ocp81375_current[OCP81375_LEVEL_NUM] = {
	32,   97,   162,  227,  292,  357,  422,  487,
	552,  617,  682,  747,  812,  877,  942,  1007,
	1072, 1137, 1202, 1267, 1332, 1397, 1462, 1527,
	1592, 1657, 1722, 1787, 1852, 1917,
};

static const int ocp81375_led2_current[OCP81375_LEVEL2_NUM] = {
	200, 550,
};

struct ocp81375_platform_data {
	u8 torch_pin_enable;
	u8 pam_sync_pin_enable;
	u8 thermal_comp_mode_enable;
	u8 strobe_pin_disable;
	u8 vout_mode_enable;
};

struct ocp81375_chip_data {
	struct i2c_client *client;
	struct ocp81375_platform_data *pdata;
	struct mutex lock;
};

static DEFINE_MUTEX(ocp81375_mutex);
static struct work_struct ocp81375_work_ch1;
static struct work_struct ocp81375_work_ch2;
static struct hrtimer ocp81375_timer_ch1;
static struct hrtimer ocp81375_timer_ch2;
static unsigned int ocp81375_timeout_ms[OCP81375_CHANNEL_NUM];

static int use_count;
static struct i2c_client *ocp81375_i2c_client;
static struct regulator *ocp81375_regulator;

static int ocp81375_en_ch1 = OCP81375_NONE;
static int ocp81375_en_ch2 = OCP81375_NONE;
static int ocp81375_level_ch1;
static int ocp81375_level_ch2;

static int ocp81375_read_reg(struct i2c_client *client, u8 reg)
{
	int ret;
	struct ocp81375_chip_data *chip = i2c_get_clientdata(client);

	mutex_lock(&chip->lock);
	ret = i2c_smbus_read_byte_data(client, reg);
	mutex_unlock(&chip->lock);
	if (ret < 0)
		pr_info("failed reading at 0x%02x\n", reg);

	return ret;
}

static int ocp81375_write_reg(struct i2c_client *client, u8 reg, u8 val)
{
	int ret;
	struct ocp81375_chip_data *chip = i2c_get_clientdata(client);

	mutex_lock(&chip->lock);
	ret = i2c_smbus_write_byte_data(client, reg, val);
	mutex_unlock(&chip->lock);
	if (ret < 0)
		pr_info("failed writing at 0x%02x\n", reg);

	return ret;
}

static u8 ocp81375_torch_code(int current_mA)
{
	int code;

	if (current_mA < 10)
		return 0;

	code = (current_mA * 100 - OCP81375_TORCH_STEP_X100)
	/ OCP81375_TORCH_STEP_X100;

	return (u8)(code & OCP81375_LEVEL_MASK);
}

static u8 ocp81375_flash_code(int current_mA)
{
	int code;

	code = (current_mA * 100 - OCP81375_FLASH_STEP_X100)
	/ OCP81375_FLASH_STEP_X100;

	return (u8)(code & OCP81375_LEVEL_MASK);
}

static int ocp81375_set_current(int channel, int current_mA)
{
	u8 flash_reg, torch_reg;
	u8 code;

	if (channel == OCP81375_CHANNEL_CH1) {
		flash_reg = OCP81375_REG_FLASH_LEVEL_LED1;
		torch_reg = OCP81375_REG_TORCH_LEVEL_LED1;
	} else if (channel == OCP81375_CHANNEL_CH2) {
		flash_reg = OCP81375_REG_FLASH_LEVEL_LED2;
		torch_reg = OCP81375_REG_TORCH_LEVEL_LED2;
	} else {
		pr_info("No such command and arg(%d): (%d, %d)\n",
			channel, channel, current_mA);
		goto done;
	}

	if (current_mA >= OCP81375_FLASH_THRESHOLD_MA) {
		code = ocp81375_flash_code(current_mA);
		ocp81375_write_reg(ocp81375_i2c_client, flash_reg, code);
		ocp81375_write_reg(ocp81375_i2c_client, torch_reg, 0);
	} else {
		code = ocp81375_torch_code(current_mA);
		ocp81375_write_reg(ocp81375_i2c_client, torch_reg, code);
		ocp81375_write_reg(ocp81375_i2c_client, flash_reg, 0);
	}

	done:
	udelay(OCP81375_WAIT_TIME_US);
	return 0;
}

static int ocp81375_enable_ch(int channel)
{
	int regval;

	if (channel == OCP81375_CHANNEL_CH1) {
		if (ocp81375_en_ch1 == OCP81375_ENABLE_FLASH) {
			ocp81375_write_reg(ocp81375_i2c_client,
					   OCP81375_REG_ENABLE, OCP81375_LED1_FLASH_ON);
		} else {
			regval = ocp81375_read_reg(ocp81375_i2c_client,
						   OCP81375_REG_ENABLE);
			if (regval < 0)
				regval = 0;
			ocp81375_write_reg(ocp81375_i2c_client,
					   OCP81375_REG_ENABLE,
		      OCP81375_LED1_TORCH_ON |
		      (regval & OCP81375_ENABLE_LED2));
		}
	} else if (channel == OCP81375_CHANNEL_CH2) {
		if (ocp81375_en_ch2 == OCP81375_ENABLE_FLASH) {
			ocp81375_write_reg(ocp81375_i2c_client,
					   OCP81375_REG_ENABLE, OCP81375_LED2_FLASH_ON);
		} else {
			regval = ocp81375_read_reg(ocp81375_i2c_client,
						   OCP81375_REG_ENABLE);
			if (regval < 0)
				regval = 0;
			ocp81375_write_reg(ocp81375_i2c_client,
					   OCP81375_REG_ENABLE,
		      OCP81375_LED2_TORCH_ON |
		      (regval & OCP81375_ENABLE_LED1));
		}
	} else {
		pr_info("Error channel\n");
		return -1;
	}

	return 0;
}

static int ocp81375_disable(void)
{
	return ocp81375_write_reg(ocp81375_i2c_client, OCP81375_REG_ENABLE, 0);
}

static int ocp81375_is_torch(int level)
{
	if (level >= OCP81375_LEVEL_NUM)
		return -1;
	return 0;
}

static int ocp81375_verify_level(int level)
{
	if (level < 0)
		level = 0;
	else if (level >= OCP81375_LEVEL_NUM)
		level = OCP81375_LEVEL_NUM - 1;
	return level;
}

static int ocp81375_set_level(int channel, int level)
{
	int lvl = ocp81375_verify_level(level);
	int current_mA;

	if (channel == OCP81375_CHANNEL_CH1)
		ocp81375_level_ch1 = lvl;
	else if (channel == OCP81375_CHANNEL_CH2)
		ocp81375_level_ch2 = lvl;

	current_mA = ocp81375_current[lvl];
	ocp81375_set_current(channel, current_mA);

	return 0;
}

static void ocp81375_work_disable_ch1(struct work_struct *data)
{
	pr_debug("ht work queue callback\n");
	ocp81375_disable();
}

static void ocp81375_work_disable_ch2(struct work_struct *data)
{
	pr_debug("lt work queue callback\n");
	ocp81375_disable();
}

static enum hrtimer_restart ocp81375_timer_func_ch1(struct hrtimer *timer)
{
	schedule_work(&ocp81375_work_ch1);
	return HRTIMER_NORESTART;
}

static enum hrtimer_restart ocp81375_timer_func_ch2(struct hrtimer *timer)
{
	schedule_work(&ocp81375_work_ch2);
	return HRTIMER_NORESTART;
}

static int ocp81375_timer_start(int channel, ktime_t ktime)
{
	if (channel == OCP81375_CHANNEL_CH1)
		hrtimer_start(&ocp81375_timer_ch1, ktime, HRTIMER_MODE_REL);
	else if (channel == OCP81375_CHANNEL_CH2)
		hrtimer_start(&ocp81375_timer_ch2, ktime, HRTIMER_MODE_REL);
	else {
		pr_info("Error channel\n");
		return -1;
	}
	return 0;
}

static int ocp81375_timer_cancel(int channel)
{
	if (channel == OCP81375_CHANNEL_CH1)
		hrtimer_cancel(&ocp81375_timer_ch1);
	else if (channel == OCP81375_CHANNEL_CH2)
		hrtimer_cancel(&ocp81375_timer_ch2);
	else {
		pr_info("Error channel\n");
		return -1;
	}
	return 0;
}

static int ocp81375_operate(int channel, int enable)
{
	ktime_t ktime;
	unsigned int s, ns;

	if (channel == OCP81375_CHANNEL_CH1) {
		ocp81375_en_ch1 = enable;
		if (ocp81375_en_ch1 && !ocp81375_is_torch(ocp81375_level_ch1))
			ocp81375_en_ch1 = OCP81375_ENABLE_FLASH;
	} else if (channel == OCP81375_CHANNEL_CH2) {
		ocp81375_en_ch2 = enable;
		if (ocp81375_en_ch2 && !ocp81375_is_torch(ocp81375_level_ch2))
			ocp81375_en_ch2 = OCP81375_ENABLE_FLASH;
	} else {
		pr_info("Error channel\n");
		return -1;
	}

	if ((ocp81375_en_ch1 != OCP81375_NONE) &&
		(ocp81375_en_ch2 != OCP81375_NONE)) {
		if ((ocp81375_en_ch1 == OCP81375_DISABLE) &&
			(ocp81375_en_ch2 == OCP81375_DISABLE)) {
			ocp81375_disable();
		ocp81375_timer_cancel(OCP81375_CHANNEL_CH1);
	ocp81375_timer_cancel(OCP81375_CHANNEL_CH2);
			} else {
				if (ocp81375_timeout_ms[OCP81375_CHANNEL_CH1] &&
					ocp81375_en_ch1 != OCP81375_DISABLE) {
					s = ocp81375_timeout_ms[OCP81375_CHANNEL_CH1] / 1000;
				ns = ocp81375_timeout_ms[OCP81375_CHANNEL_CH1] % 1000
				* 1000000;
				ktime = ktime_set(s, ns);
				ocp81375_timer_start(OCP81375_CHANNEL_CH1, ktime);
					}
					if (ocp81375_timeout_ms[OCP81375_CHANNEL_CH2] &&
						ocp81375_en_ch2 != OCP81375_DISABLE) {
						s = ocp81375_timeout_ms[OCP81375_CHANNEL_CH2] / 1000;
					ns = ocp81375_timeout_ms[OCP81375_CHANNEL_CH2] % 1000
					* 1000000;
					ktime = ktime_set(s, ns);
					ocp81375_timer_start(OCP81375_CHANNEL_CH2, ktime);
						}
						ocp81375_enable_ch(channel);
			}

			ocp81375_en_ch1 = OCP81375_NONE;
			ocp81375_en_ch2 = OCP81375_NONE;
		}

		return 0;
}

static int ocp81375_get_hw_fault(void)
{
	ocp81375_read_reg(ocp81375_i2c_client, OCP81375_REG_FLAG1);
	ocp81375_read_reg(ocp81375_i2c_client, OCP81375_REG_FLAG2);
	return 0;
}

static int ocp81375_init(void)
{
	pr_info("%s\n", __func__);

	if (ocp81375_regulator)
		(void)regulator_enable(ocp81375_regulator);
	udelay(OCP81375_WAIT_TIME_US);

	ocp81375_write_reg(ocp81375_i2c_client, OCP81375_REG_ENABLE, 0x00);
	ocp81375_write_reg(ocp81375_i2c_client, OCP81375_REG_BOOST_CONFIG,
			   OCP81375_BOOST_CONFIG_VAL);
	ocp81375_write_reg(ocp81375_i2c_client, OCP81375_REG_TIMING_CONFIG,
			   OCP81375_TIMING_CONFIG_VAL);
	ocp81375_write_reg(ocp81375_i2c_client, OCP81375_REG_FLASH_LEVEL_LED2, 0x00);
	ocp81375_write_reg(ocp81375_i2c_client, OCP81375_REG_TORCH_LEVEL_LED2, 0x00);

	return 0;
}

static int ocp81375_uninit(void)
{
	ocp81375_en_ch1 = OCP81375_NONE;
	ocp81375_en_ch2 = OCP81375_NONE;
	ocp81375_disable();
	if (ocp81375_regulator)
		(void)regulator_disable(ocp81375_regulator);

	return 0;
}

static int ocp81375_set_driver(int set)
{
	mutex_lock(&ocp81375_mutex);
	if (set) {
		if (!use_count)
			ocp81375_init();
		use_count++;
		pr_debug("Set driver: %d\n", use_count);
	} else {
		use_count--;
		if (use_count == 0)
			ocp81375_uninit();
		else if (use_count < 0)
			use_count = 0;
		pr_debug("Unset driver: %d\n", use_count);
	}
	mutex_unlock(&ocp81375_mutex);

	return 0;
}

static ssize_t ocp81375_strobe_store(struct flashlight_arg arg)
{
	ocp81375_set_driver(1);
	ocp81375_set_level(arg.channel, arg.level);
	ocp81375_timeout_ms[arg.channel] = 0;
	ocp81375_operate(arg.channel, OCP81375_ENABLE);
	msleep(arg.dur);
	ocp81375_operate(arg.channel, OCP81375_DISABLE);
	ocp81375_set_driver(0);

	return 0;
}

static int ocp81375_open(void)
{
	return 0;
}

static int ocp81375_release(void)
{
	return 0;
}

static int ocp81375_ioctl(unsigned int cmd, unsigned long arg)
{
	struct flashlight_dev_arg *fl_arg;
	int channel;

	fl_arg = (struct flashlight_dev_arg *)arg;
	channel = fl_arg->channel;

	if (channel < 0 || channel >= OCP81375_CHANNEL_NUM) {
		pr_info("Error channel\n");
		return -EINVAL;
	}

	switch (cmd) {
		case FLASH_IOC_SET_TIME_OUT_TIME_MS:
			pr_debug("FLASH_IOC_SET_TIME_OUT_TIME_MS(%d): %d\n",
				 channel, (int)fl_arg->arg);
			ocp81375_timeout_ms[channel] = fl_arg->arg;
			break;

		case FLASH_IOC_SET_DUTY:
			pr_debug("FLASH_IOC_SET_DUTY(%d): %d\n",
				 channel, (int)fl_arg->arg);
			ocp81375_set_level(channel, fl_arg->arg);
			break;

		case FLASH_IOC_SET_ONOFF:
			pr_debug("FLASH_IOC_SET_ONOFF(%d): %d\n",
				 channel, (int)fl_arg->arg);
			ocp81375_operate(channel, fl_arg->arg);
			break;

		case FLASH_IOC_SET_SCENARIO:
			pr_debug("FLASH_IOC_SET_SCENARIO(%d): %d\n",
				 channel, (int)fl_arg->arg);
			break;

		case FLASH_IOC_GET_DUTY_NUMBER:
			pr_debug("FLASH_IOC_GET_DUTY_NUMBER(%d)\n", channel);
			fl_arg->arg = (channel == OCP81375_CHANNEL_CH1) ?
			OCP81375_LEVEL_NUM : OCP81375_LEVEL2_NUM;
			break;

		case FLASH_IOC_GET_MAX_TORCH_DUTY:
			pr_debug("FLASH_IOC_GET_MAX_TORCH_DUTY\n");
			fl_arg->arg = 0;
			break;

		case FLASH_IOC_GET_DUTY_CURRENT:
			fl_arg->arg = (channel == OCP81375_CHANNEL_CH1) ?
			ocp81375_current[ocp81375_verify_level(fl_arg->arg)] :
			ocp81375_led2_current[fl_arg->arg ? 1 : 0];
			pr_debug("FLASH_IOC_GET_DUTY_CURRENT(%d): %d\n",
				 channel, (int)fl_arg->arg);
			break;

		case FLASH_IOC_GET_HW_FAULT:
			pr_debug("FLASH_IOC_GET_HW_FAULT(%d)\n", channel);
			ocp81375_get_hw_fault();
			break;

		case FLASH_IOC_UNINIT:
			pr_debug("FLASH_IOC_UNINIT(%d): %d\n",
				 channel, (int)fl_arg->arg);
			ocp81375_set_driver(0);
			break;

		case OCP81375_IOC_SET_CURRENT:
			if ((int)fl_arg->arg >= OCP81375_SET_CURRENT_MAX_MA + 1)
				fl_arg->arg = OCP81375_SET_CURRENT_MAX_MA;
		pr_debug("FLASH_IOC_SET_CURRENT(%d): %d\n",
			 channel, (int)fl_arg->arg);
		ocp81375_set_current(channel, fl_arg->arg);
		break;

		case OCP81375_IOC_GENERATE_DEV_PARA:
			pr_debug("FLASH_IOC_GENERATE_DEV_PARA(%d): %d\n",
				 channel, (int)fl_arg->arg);
			break;

		default:
			pr_info("No such command and arg(%d): (%d, %d)\n",
				channel, cmd, (int)fl_arg->arg);
			return -ENOTTY;
	}

	return 0;
}

static struct flashlight_operations ocp81375_ops = {
	.flashlight_open = ocp81375_open,
	.flashlight_release = ocp81375_release,
	.flashlight_ioctl = ocp81375_ioctl,
	.flashlight_strobe_store = ocp81375_strobe_store,
	.flashlight_set_driver = ocp81375_set_driver,
};

/* Güncellenmiş i2c_probe (id parametresi kaldırıldı) */
static int ocp81375_i2c_probe(struct i2c_client *client)
{
	struct ocp81375_chip_data *chip;
	struct ocp81375_platform_data *pdata = client->dev.platform_data;
	int err, id_val;

	pr_debug("%s:%d start\n", __func__, __LINE__);

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_SMBUS_BYTE_DATA)) {
		pr_info("Failed to check i2c functionality.\n");
		err = -ENODEV;
		goto err_out;
	}

	chip = kzalloc(sizeof(struct ocp81375_chip_data), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;

	chip->client = client;

	if (!pdata) {
		pr_info("Platform data does not exist\n");
		pdata = kzalloc(sizeof(struct ocp81375_platform_data), GFP_KERNEL);
		if (!pdata) {
			err = -ENOMEM;
			goto err_free_chip;
		}
	}
	chip->pdata = pdata;

	i2c_set_clientdata(client, chip);
	ocp81375_i2c_client = client;

	mutex_init(&chip->lock);
	mutex_init(&ocp81375_mutex);

	hrtimer_init(&ocp81375_timer_ch1, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	ocp81375_timer_ch1.function = ocp81375_timer_func_ch1;
	hrtimer_init(&ocp81375_timer_ch2, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	ocp81375_timer_ch2.function = ocp81375_timer_func_ch2;
	ocp81375_timeout_ms[OCP81375_CHANNEL_CH1] = 100;
	ocp81375_timeout_ms[OCP81375_CHANNEL_CH2] = 100;
	INIT_WORK(&ocp81375_work_ch1, ocp81375_work_disable_ch1);
	INIT_WORK(&ocp81375_work_ch2, ocp81375_work_disable_ch2);

	if (ocp81375_regulator)
		(void)regulator_enable(ocp81375_regulator);
	udelay(OCP81375_WAIT_TIME_US);

	id_val = ocp81375_read_reg(client, OCP81375_REG_DEVICE_ID);
	ocp81375_write_reg(client, OCP81375_REG_ENABLE, 0x00);

	if (ocp81375_regulator)
		(void)regulator_disable(ocp81375_regulator);

	pr_info("ocp81375 read DEVICE_ID = 0x%x\n", id_val & 0xFFFF);

	if ((id_val & 0xFFFF) == OCP81375_DEVICE_ID) {
		err = flashlight_dev_register(OCP81375_NAME, &ocp81375_ops);
		if (err) {
			pr_info("ocp81375 flashlight_dev_register fail.\n");
			goto err_free_pdata;
		}
	} else {
		pr_info("ocp81375 DEVICE_ID mismatch, skip.\n");
	}

	pr_debug("%s:%d end\n", __func__, __LINE__);
	return 0;

	err_free_pdata:
	kfree(pdata);
	chip->pdata = NULL;
	err_free_chip:
	kfree(chip);
	err_out:
	return err;
}

/* Güncellenmiş i2c_remove (void döndürüyor) */
static void ocp81375_i2c_remove(struct i2c_client *client)
{
	struct ocp81375_chip_data *chip = i2c_get_clientdata(client);

	flashlight_dev_unregister(OCP81375_NAME);
	kfree(chip->pdata);
	kfree(chip);
}

static const struct i2c_device_id ocp81375_i2c_id[] = {
	{OCP81375_NAME, 0},
	{}
};
MODULE_DEVICE_TABLE(i2c, ocp81375_i2c_id);

#ifdef CONFIG_OF
static const struct of_device_id ocp81375_i2c_of_match[] = {
	{.compatible = OCP81375_DTNAME_I2C},
	{},
};
MODULE_DEVICE_TABLE(of, ocp81375_i2c_of_match);
#endif

static struct i2c_driver ocp81375_i2c_driver = {
	.driver = {
		.name = OCP81375_NAME,
		#ifdef CONFIG_OF
		.of_match_table = ocp81375_i2c_of_match,
		#endif
	},
	.probe = ocp81375_i2c_probe,
	.remove = ocp81375_i2c_remove,
	.id_table = ocp81375_i2c_id,
};

static int ocp81375_probe(struct platform_device *pdev)
{
	ocp81375_regulator = regulator_get(&pdev->dev, OCP81375_REGULATOR_NAME);
	if (IS_ERR(ocp81375_regulator)) {
		pr_info("ocp81375_device.regulator get fail!\n");
		ocp81375_regulator = NULL;
	}

	if (i2c_add_driver(&ocp81375_i2c_driver)) {
		pr_info("Failed to register platform driver\n");
		return -1;
	}

	return 0;
}

/* Güncellenmiş platform remove (void döndürüyor) */
static void ocp81375_remove(struct platform_device *pdev)
{
	i2c_del_driver(&ocp81375_i2c_driver);
}

#ifdef CONFIG_OF
static const struct of_device_id ocp81375_of_match[] = {
	{.compatible = OCP81375_DTNAME},
	{},
};
MODULE_DEVICE_TABLE(of, ocp81375_of_match);
#else
static struct platform_device ocp81375_platform_device[] = {
	{
		.name = OCP81375_NAME,
		.id = 0,
		.dev = {}
	},
	{}
};
MODULE_DEVICE_TABLE(platform, ocp81375_platform_device);
#endif

static struct platform_driver ocp81375_platform_driver = {
	.probe = ocp81375_probe,
	.remove = ocp81375_remove,
	.driver = {
		.name = OCP81375_NAME,
		.owner = THIS_MODULE,
		#ifdef CONFIG_OF
		.of_match_table = ocp81375_of_match,
		#endif
	},
};

static int __init flashlight_ocp81375_init(void)
{
	int ret;

	#ifndef CONFIG_OF
	ret = platform_device_register(ocp81375_platform_device);
	if (ret) {
		pr_info("Failed to register platform device\n");
		return ret;
	}
	#endif

	ret = platform_driver_register(&ocp81375_platform_driver);
	if (ret) {
		pr_info("Failed to register platform driver\n");
		return ret;
	}

	pr_debug("Init done.\n");

	return 0;
}

static void __exit flashlight_ocp81375_exit(void)
{
	platform_driver_unregister(&ocp81375_platform_driver);
}

module_init(flashlight_ocp81375_init);
module_exit(flashlight_ocp81375_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("qinlong.lu <qinlong.lu@transsion.com>");
MODULE_DESCRIPTION("TRANSSION Flashlight OCP81375 Driver");

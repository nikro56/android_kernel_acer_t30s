/*
 * kernel/drivers/media/video/tegra
 *
 * Aptina MT9D115 sensor driver
 *
 * Copyright (C) 2010 NVIDIA Corporation
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/i2c.h>
#include <linux/miscdevice.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <mach/gpio.h>
#include <media/yuv_sensor.h>
#include <media/tegra_camera.h>

#if defined(CONFIG_MACH_PICASSO) || defined(CONFIG_MACH_MAYA) || defined(CONFIG_MACH_PICASSO_E2)
#include "yuv_init_tab_picasso.h"
#endif
#if defined(CONFIG_MACH_VANGOGH)
#include "yuv_init_tab_vangogh.h"
#endif

#define YUV_SENSOR_OE_GPIO    90
#define YUV_SENSOR_RST_GPIO   92
#define CAMERA_POWER_GPIO     172

#define SENSOR_WIDTH_REG      0x2703
#define SENSOR_640_WIDTH_VAL  0x280
#define SENSOR_800_WIDTH_VAL  0x320
#define SENSOR_720_WIDTH_VAL  0x500
#define SENSOR_1600_WIDTH_VAL 0x640

static struct mutex yuv_lock;

static int sensor_read_reg(struct i2c_client *client, u16 addr, u16 *val)
{
	int err;
	struct i2c_msg msg[2];
	unsigned char data[4];

	if (!client->adapter)
		return -ENODEV;

	msg[0].addr = client->addr;
	msg[0].flags = 0;
	msg[0].len = 2;
	msg[0].buf = data;

	/* high byte goes out first */
	data[0] = (u8) (addr >> 8);;
	data[1] = (u8) (addr & 0xff);

	msg[1].addr = client->addr;
	msg[1].flags = I2C_M_RD;
	msg[1].len = 2;
	msg[1].buf = data + 2;

	err = i2c_transfer(client->adapter, msg, 2);

	if (err != 2)
		return -EINVAL;

	swap(*(data+2),*(data+3)); // swap high and low byte to match table format
	memcpy(val, data+2, 2);

	return 0;
}

static int sensor_write_reg(struct i2c_client *client, u16 addr, u16 val)
{
	int err;
	struct i2c_msg msg;
	unsigned char data[4];
	int retry = 0;

	if (!client->adapter)
		return -ENODEV;

	data[0] = (u8) (addr >> 8);
	data[1] = (u8) (addr & 0xff);
	data[2] = (u8) (val >> 8);
	data[3] = (u8) (val & 0xff);

	msg.addr = client->addr;
	msg.flags = 0;
	msg.len = 4;
	msg.buf = data;

	do {
		err = i2c_transfer(client->adapter, &msg, 1);
		if (err == 1)
			return 0;
		retry++;
		pr_err("yuv %s: i2c transfer failed, retrying %x %x\n",
			__func__, addr, val);
		msleep(3);
	} while (retry <= SENSOR_MAX_RETRIES);

	return err;
}

static int sensor_write_table(struct i2c_client *client, const struct sensor_reg table[])
{
	int err;
	const struct sensor_reg *next;
	u16 val;

	pr_debug("yuv %s\n", __func__);
	for (next = table; next->addr != SENSOR_TABLE_END; next++) {
		if (next->addr == SENSOR_WAIT_MS) {
			msleep(next->val);
			continue;
		}

		val = next->val;

		err = sensor_write_reg(client, next->addr, val);
		if (err)
			return err;
	}

	return 0;
}

static int get_sensor_current_width(struct i2c_client *client, u16 *val)
{
	int err;

	err = sensor_write_reg(client, 0x098c, 0x2703);
	if (err)
		return err;

	err = sensor_read_reg(client, 0x0990, val);
	if (err)
		return err;

	return 0;
}

static void poll_current_state(u16 expected_state)
{
	u16 state = 0;

	pr_info("yuv %s: check current seq_state for %u\n", __func__, expected_state);
	do {
		sensor_write_reg(info->i2c_client, 0x098c, 0xa104);  //MCU_ADDRESS[SEQ_STATE]
		sensor_read_reg(info->i2c_client, 0x0990, &state);   //MCU_DATA_0 value
		pr_debug("yuv %s: MCU_DATA_0 = %u\n", __func__, state);
	} while (state != expected_state);
}

static int sensor_set_mode(struct sensor_info *info, struct sensor_mode *mode)
{
	int sensor_table;
	int err;
	u16 val;

	pr_info("yuv %s: xres %u yres %u\n", __func__, mode->xres, mode->yres);

	mutex_lock(&yuv_lock);

	if (mode->xres == 1600 && mode->yres == 1200)
		sensor_table = SENSOR_MODE_1600x1200;
	else if (mode->xres == 1280 && mode->yres == 720)
		sensor_table = SENSOR_MODE_1280x720;
	else if (mode->xres == 800 && mode->yres == 600)
		sensor_table = SENSOR_MODE_800x600;
	else {
		pr_err("yuv %s: invalid resolution supplied to set mode %d %d\n",
			__func__, mode->xres, mode->yres);
		mutex_unlock(&yuv_lock);
		return -EINVAL;
	}

	if (mode->xres == 1600 && mode->yres == 1200) {
		// Get context a coarse time
		sensor_read_reg(info->i2c_client, 0x3012, &(info->coarse_time));
		pr_info("yuv %s: get context a coarse time = %d\n", __func__, info->coarse_time);
	}

	err = get_sensor_current_width(info->i2c_client, &val);

	// if no table is programming before and request set to 1600x1200, then
	// we must use 1600x1200 table to fix CTS testing issue
	if (!(val == SENSOR_800_WIDTH_VAL || val == SENSOR_720_WIDTH_VAL || val == SENSOR_1600_WIDTH_VAL) && sensor_table == SENSOR_MODE_1600x1200) {
		err = sensor_write_table(info->i2c_client, CTS_ZoomTest_mode_1600x1200);
		pr_info("yuv %s: 1600x1200 cts table\n", __func__);
	} else {
		// check already program the sensor mode, Aptina support Context B fast switching capture mode back to preview mode
		// we don't need to re-program the sensor mode for 640x480/720P table
		if (!((val == SENSOR_800_WIDTH_VAL && sensor_table == SENSOR_MODE_800x600)
			||(val == SENSOR_720_WIDTH_VAL && sensor_table == SENSOR_MODE_1280x720))) {

			err = sensor_write_table(info->i2c_client, mode_table[sensor_table]);
			if (err) {
				mutex_unlock(&yuv_lock);
				return err;
			}

			if (mode->xres == 1600 && mode->yres == 1200) {
				// don't use double in the kernel
				long val = (long)(info->coarse_time);
				val = (val * 1648) / 2284;

				// Set context b coarse time
				sensor_write_reg(info->i2c_client, 0x3012, (u16)val);
				sensor_write_reg(info->i2c_client, 0x301A, 0x12CE);
				sensor_write_reg(info->i2c_client, 0x3400, 0x7A20);
			}

			// polling sensor to confirm it's already in capture flow or in preview flow.
			// this can avoid frame mismatch issue due to inproper delay
			if (sensor_table == SENSOR_MODE_1600x1200) {
				// check current seq_state for capture
				poll_current_state(7);
			} else {
				// check current seq_state for preview
				poll_current_state(3);
			}
		} else {
			// check current seq_state for preview
			poll_current_state(3);
		}
	}
	info->mode = sensor_table;

	mutex_unlock(&yuv_lock);

	return 0;
}

static int sensor_get_exposure_time(struct sensor_info *info, unsigned int *exposure_time_denominator)
{
	int err;
	u16 val = 0;

	err = sensor_write_reg(info->i2c_client, 0x098C, 0xA21B);
	if (err)
		return err;

	err = sensor_read_reg(info->i2c_client, 0x0990, &val);
	if (err)
		return err;

	// exposure time = 1/frame rate
	// exposure_time_denominator = frame rate
	// when val under 4, camera frame rate is always 30 fps
	if(val < 4)
		*exposure_time_denominator = 30;
	else
		*exposure_time_denominator = (unsigned int)(120/val);

	return 0;
}

static long sensor_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct sensor_info *info = file->private_data;
	int err=0;

	pr_debug("yuv %s: cmd %u\n", __func__, cmd);
	switch (cmd) {
		case SENSOR_IOCTL_SET_MODE: {
			struct sensor_mode mode;

			if (copy_from_user(&mode,
					(const void __user *)arg,
					sizeof(struct sensor_mode))) {
				return -EFAULT;
			}
			pr_debug("yuv %s: set_mode\n", __func__);

			return sensor_set_mode(info, &mode);
		}

		case SENSOR_IOCTL_GET_STATUS:
			pr_info("yuv %s: get_status\n", __func__);
			return 0;

		case SENSOR_IOCTL_SET_COLOR_EFFECT: {
			int coloreffect;

			if (copy_from_user(&coloreffect,
					(const void __user *)arg,
					sizeof(coloreffect))) {
				return -EFAULT;
			}
			pr_info("yuv %s: coloreffect %d\n", __func__, coloreffect);

			switch(coloreffect) {
				case YUV_ColorEffect_Mono:
					err = sensor_write_table(info->i2c_client, ColorEffect_Mono);
					break;

				case YUV_ColorEffect_Negative:
					err = sensor_write_table(info->i2c_client, ColorEffect_Negative);
					break;

				case YUV_ColorEffect_None:
					err = sensor_write_table(info->i2c_client, ColorEffect_None);
					break;

				case YUV_ColorEffect_Sepia:
					err = sensor_write_table(info->i2c_client, ColorEffect_Sepia);
					break;

				case YUV_ColorEffect_Solarize:
					err = sensor_write_table(info->i2c_client, ColorEffect_Solarize);
					break;

				default:
					break;
			}
			if (err)
				return err;
			return 0;
		}

		case SENSOR_IOCTL_SET_WHITE_BALANCE: {
			int whitebalance;

			if (copy_from_user(&whitebalance,
					(const void __user *)arg,
					sizeof(whitebalance))) {
				return -EFAULT;
			}
			pr_info("yuv %s: whitebalance %d\n", __func__, whitebalance);

			switch(whitebalance) {
				case YUV_Whitebalance_Auto:
					err = sensor_write_table(info->i2c_client, Whitebalance_Auto);
					break;

				case YUV_Whitebalance_Incandescent:
					err = sensor_write_table(info->i2c_client, Whitebalance_Incandescent);
					break;

				case YUV_Whitebalance_Daylight:
					err = sensor_write_table(info->i2c_client, Whitebalance_Daylight);
					break;

				case YUV_Whitebalance_Fluorescent:
					err = sensor_write_table(info->i2c_client, Whitebalance_Fluorescent);
					break;

				case YUV_Whitebalance_CloudyDaylight:
					err = sensor_write_table(info->i2c_client, Whitebalance_CloudyDaylight);
					break;

				default:
					break;
			}
			if (err)
				return err;
			return 0;
		}

		case SENSOR_IOCTL_GET_EXPOSURE_TIME: {
			unsigned int exposure_time_denominator;
			sensor_get_exposure_time(info, &exposure_time_denominator);
			if (copy_to_user((void __user *)arg,
					&exposure_time_denominator,
					sizeof(exposure_time_denominator)))
				return -EFAULT;

			pr_info("yuv %s: exposure time %d\n", __func__, exposure_time_denominator);

			return 0;
		}

		case SENSOR_IOCTL_SET_SCENE_MODE:
			pr_info("yuv %s: scene_mode\n", __func__);
			return 0;

		case SENSOR_IOCTL_SET_EXPOSURE: {
			int exposure;

			if (copy_from_user(&exposure,
					(const void __user *)arg,
					sizeof(exposure))) {
				return -EFAULT;
			}
			pr_info("yuv %s: exposure %d\n", __func__, exposure);

			switch(exposure) {
				case YUV_Exposure_Zero:
					err = sensor_write_table(info->i2c_client, Exposure_Zero);
					break;

				case YUV_Exposure_Plus_One:
					err = sensor_write_table(info->i2c_client, Exposure_Plus_One);
					break;

				case YUV_Exposure_Plus_Two:
					err = sensor_write_table(info->i2c_client, Exposure_Plus_Two);
					break;

				case YUV_Exposure_Minus_One:
					err = sensor_write_table(info->i2c_client, Exposure_Minus_One);
					break;

				case YUV_Exposure_Minus_Two:
					err = sensor_write_table(info->i2c_client, Exposure_Minus_Two);
					break;

				default:
					break;
			}
			if (err)
				return err;
			return 0;
		}

		default:
			pr_info("yuv %s: default\n", __func__);
			return -EINVAL;
	}

	return 0;
}

static int sensor_open(struct inode *inode, struct file *file)
{
	pr_info("yuv %s\n", __func__);
	file->private_data = info;
	if (info->pdata && info->pdata->power_on)
		info->pdata->power_on();

	return 0;
}

int sensor_release(struct inode *inode, struct file *file)
{
	pr_info("yuv %s\n", __func__);
	if (info->pdata && info->pdata->power_off)
		info->pdata->power_off();
	file->private_data = NULL;

	return 0;
}

static const struct file_operations sensor_fileops = {
	.owner = THIS_MODULE,
	.open = sensor_open,
	.unlocked_ioctl = sensor_ioctl,
	.release = sensor_release,
};

static struct miscdevice sensor_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = SENSOR_NAME,
	.fops = &sensor_fileops,
};

static int yuv_initialize(struct sensor_info *info){
	int err;

	struct tegra_camera_clk_info clk_info;

	extern void extern_tegra_camera_enable_vi(void);
	extern void extern_tegra_camera_disable_vi(void);
	extern void extern_tegra_camera_clk_set_rate(struct tegra_camera_clk_info *);

	pr_info("%s ++\n", __func__);

	// set MCLK to 24MHz
	clk_info.id = TEGRA_CAMERA_MODULE_VI;
	clk_info.clk_id = TEGRA_CAMERA_VI_SENSOR_CLK;
	clk_info.rate = 24000000;
	extern_tegra_camera_clk_set_rate(&clk_info);

	// turn on MCLK and pull down PWDN pin
	extern_tegra_camera_enable_vi();
	if (info->pdata && info->pdata->power_on)
		info->pdata->power_on();

	err = sensor_write_table(info->i2c_client, mode_table[SENSOR_MODE_INIT]);
	if (err) {
		pr_err("set mode write table fail\n");
		return err;
	}

	poll_current_state(3);

	// pull high PWDN pin and turn off MCLK
	if (info->pdata && info->pdata->power_off)
		info->pdata->power_off();
	extern_tegra_camera_disable_vi();

	info->mode = SENSOR_MODE_800x600;

	pr_info("%s --\n", __func__);
	return 0;
}

static struct kobject *codec_dev_info_kobj;

static ssize_t vendor_show(struct kobject *kobj, struct kobj_attribute *attr, char * buf)
{
	char *s = buf;
	s += sprintf(s, "Aptina MT9D115\n");
	return (s - buf);
}

#define debug_attr(_name) \
	static struct kobj_attribute _name##_attr = { \
		.attr = { \
			.name = __stringify(_name), \
			.mode = 0644, \
		}, \
		.show = _name##_show, \
	}

debug_attr(vendor);

static struct attribute * group[] = {
	&vendor_attr.attr,
	NULL,
};

static struct attribute_group attr_group =
{
	.attrs = group,
};

static int sensor_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
	int err;

	pr_info("yuv %s\n", __func__);

	info = kzalloc(sizeof(struct sensor_info), GFP_KERNEL);

	if (!info) {
		pr_err("yuv %s: Unable to allocate memory\n", __func__);
		return -ENOMEM;
	}

	mutex_init(&yuv_lock);

	err = misc_register(&sensor_device);
	if (err) {
		pr_err("yuv %s: Unable to register misc device\n", __func__);
		kfree(info);
		return err;
	}

	info->pdata = client->dev.platform_data;
	info->i2c_client = client;
	info->mode = 0;

	if (yuv_initialize(info) != 0) {
		kfree(info);
		return -ENODEV;
	}

	codec_dev_info_kobj = kobject_create_and_add("dev-info_front-camera-model_id", NULL);
	if (codec_dev_info_kobj == NULL) {
		pr_err("%s: kobject_create_and_add failed\n", __FUNCTION__);
	}
	err = sysfs_create_group(codec_dev_info_kobj, &attr_group);

	if (err) {
		pr_err("%s: sysfs_create_group failed\n", __FUNCTION__);
	}

	i2c_set_clientdata(client, info);

	return 0;
}

static int sensor_remove(struct i2c_client *client)
{
	struct sensor_info *info;

	pr_info("yuv %s\n", __func__);
	info = i2c_get_clientdata(client);
	misc_deregister(&sensor_device);
	kfree(info);

	return 0;
}

static const struct i2c_device_id sensor_id[] = {
	{ SENSOR_NAME, 0 },
	{ },
};

MODULE_DEVICE_TABLE(i2c, sensor_id);

static struct i2c_driver sensor_i2c_driver = {
	.driver = {
		.name = SENSOR_NAME,
		.owner = THIS_MODULE,
	},
	.probe = sensor_probe,
	.remove = sensor_remove,
	.id_table = sensor_id,
};

static int __init sensor_init(void)
{
	pr_info("yuv %s\n", __func__);
	return i2c_add_driver(&sensor_i2c_driver);
}

static void __exit sensor_exit(void)
{
	pr_info("yuv %s\n", __func__);
	i2c_del_driver(&sensor_i2c_driver);
}

module_init(sensor_init);
module_exit(sensor_exit);

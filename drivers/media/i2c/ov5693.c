// SPDX-License-Identifier: GPL-2.0
/*
 * Support for OmniVision OV5693 1080p HD camera sensor.
 *
 * Copyright (c) 2013 Intel Corporation. All Rights Reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version
 * 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 *
 */

#include <linux/module.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/string.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/kmod.h>
#include <linux/device.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/i2c.h>
#include <linux/moduleparam.h>
#include <media/v4l2-device.h>
#include <linux/io.h>
#include <linux/acpi.h>
#include <linux/regulator/consumer.h>

#include "ov5693.h"
#include "ad5823.h"

#define __cci_delay(t) \
	do { \
		if ((t) < 10) { \
			usleep_range((t) * 1000, ((t) + 1) * 1000); \
		} else { \
			msleep((t)); \
		} \
	} while (0)

/* Value 30ms reached through experimentation on byt ecs.
 * The DS specifies a much lower value but when using a smaller value
 * the I2C bus sometimes locks up permanently when starting the camera.
 * This issue could not be reproduced on cht, so we can reduce the
 * delay value to a lower value when insmod.
 */
static uint up_delay = 30;
module_param(up_delay, uint, 0644);
MODULE_PARM_DESC(up_delay,
		 "Delay prior to the first CCI transaction for ov5693");

static int vcm_ad_i2c_wr8(struct i2c_client *client, u8 reg, u8 val)
{
	int err;
	struct i2c_msg msg;
	u8 buf[2];

	buf[0] = reg;
	buf[1] = val;

	msg.addr = VCM_ADDR;
	msg.flags = 0;
	msg.len = 2;
	msg.buf = &buf[0];

	err = i2c_transfer(client->adapter, &msg, 1);
	if (err != 1) {
		dev_err(&client->dev, "%s: vcm i2c fail, err code = %d\n",
			__func__, err);
		return -EIO;
	}
	return 0;
}

static int ad5823_i2c_write(struct i2c_client *client, u8 reg, u8 val)
{
	struct i2c_msg msg;
	u8 buf[2];

	buf[0] = reg;
	buf[1] = val;
	msg.addr = AD5823_VCM_ADDR;
	msg.flags = 0;
	msg.len = 0x02;
	msg.buf = &buf[0];

	if (i2c_transfer(client->adapter, &msg, 1) != 1)
		return -EIO;
	return 0;
}

static int ad5823_i2c_read(struct i2c_client *client, u8 reg, u8 *val)
{
	struct i2c_msg msg[2];
	u8 buf[2];

	buf[0] = reg;
	buf[1] = 0;

	msg[0].addr = AD5823_VCM_ADDR;
	msg[0].flags = 0;
	msg[0].len = 0x01;
	msg[0].buf = &buf[0];

	msg[1].addr = 0x0c;
	msg[1].flags = I2C_M_RD;
	msg[1].len = 0x01;
	msg[1].buf = &buf[1];
	*val = 0;
	if (i2c_transfer(client->adapter, msg, 2) != 2)
		return -EIO;
	*val = buf[1];
	return 0;
}

static const u32 ov5693_embedded_effective_size = 28;

/* i2c read/write stuff */
static int ov5693_read_reg(struct i2c_client *client,
			   u16 data_length, u16 reg, u16 *val)
{
	int err;
	struct i2c_msg msg[2];
	unsigned char data[6];

	if (!client->adapter) {
		dev_err(&client->dev, "%s error, no client->adapter\n",
			__func__);
		return -ENODEV;
	}

	if (data_length != OV5693_8BIT && data_length != OV5693_16BIT &&
	    data_length != OV5693_32BIT) {
		dev_err(&client->dev, "%s error, invalid data length\n",
			__func__);
		return -EINVAL;
	}

	memset(msg, 0, sizeof(msg));

	msg[0].addr = client->addr;
	msg[0].flags = 0;
	msg[0].len = I2C_MSG_LENGTH;
	msg[0].buf = data;

	/* high byte goes out first */
	data[0] = (u8)(reg >> 8);
	data[1] = (u8)(reg & 0xff);

	msg[1].addr = client->addr;
	msg[1].len = data_length;
	msg[1].flags = I2C_M_RD;
	msg[1].buf = data;

	err = i2c_transfer(client->adapter, msg, 2);
	if (err != 2) {
		if (err >= 0)
			err = -EIO;
		dev_err(&client->dev,
			"read from offset 0x%x error %d", reg, err);
		return err;
	}

	*val = 0;
	/* high byte comes first */
	if (data_length == OV5693_8BIT)
		*val = (u8)data[0];
	else if (data_length == OV5693_16BIT)
		*val = be16_to_cpu(*(__be16 *)&data[0]);
	else
		*val = be32_to_cpu(*(__be32 *)&data[0]);

	return 0;
}

static int ov5693_i2c_write(struct i2c_client *client, u16 len, u8 *data)
{
	struct i2c_msg msg;
	const int num_msg = 1;
	int ret;

	msg.addr = client->addr;
	msg.flags = 0;
	msg.len = len;
	msg.buf = data;
	ret = i2c_transfer(client->adapter, &msg, 1);

	return ret == num_msg ? 0 : -EIO;
}

static int vcm_dw_i2c_write(struct i2c_client *client, u16 data)
{
	struct i2c_msg msg;
	const int num_msg = 1;
	int ret;
	__be16 val;

	val = cpu_to_be16(data);
	msg.addr = VCM_ADDR;
	msg.flags = 0;
	msg.len = OV5693_16BIT;
	msg.buf = (void *)&val;

	ret = i2c_transfer(client->adapter, &msg, 1);

	return ret == num_msg ? 0 : -EIO;
}

/*
 * Theory: per datasheet, the two VCMs both allow for a 2-byte read.
 * The DW9714 doesn't actually specify what this does (it has a
 * two-byte write-only protocol, but specifies the read sequence as
 * legal), but it returns the same data (zeroes) always, after an
 * undocumented initial NAK.  The AD5823 has a one-byte address
 * register to which all writes go, and subsequent reads will cycle
 * through the 8 bytes of registers.  Notably, the default values (the
 * device is always power-cycled affirmatively, so we can rely on
 * these) in AD5823 are not pairwise repetitions of the same 16 bit
 * word.  So all we have to do is sequentially read two bytes at a
 * time and see if we detect a difference in any of the first four
 * pairs.
 */
static int vcm_detect(struct i2c_client *client)
{
	int i, ret;
	struct i2c_msg msg;
	u16 data0 = 0, data;

	for (i = 0; i < 4; i++) {
		msg.addr = VCM_ADDR;
		msg.flags = I2C_M_RD;
		msg.len = sizeof(data);
		msg.buf = (u8 *)&data;
		ret = i2c_transfer(client->adapter, &msg, 1);

		/*
		 * DW9714 always fails the first read and returns
		 * zeroes for subsequent ones
		 */
		if (i == 0 && ret == -EREMOTEIO) {
			data0 = 0;
			continue;
		}

		if (i == 0)
			data0 = data;

		if (data != data0)
			return VCM_AD5823;
	}
	return ret == 1 ? VCM_DW9714 : ret;
}

static int ov5693_write_reg(struct i2c_client *client, u16 data_length,
			    u16 reg, u16 val)
{
	int ret;
	unsigned char data[4] = {0};
	__be16 *wreg = (void *)data;
	const u16 len = data_length + sizeof(u16); /* 16-bit address + data */

	if (data_length != OV5693_8BIT && data_length != OV5693_16BIT) {
		dev_err(&client->dev,
			"%s error, invalid data_length\n", __func__);
		return -EINVAL;
	}

	/* high byte goes out first */
	*wreg = cpu_to_be16(reg);

	if (data_length == OV5693_8BIT) {
		data[2] = (u8)(val);
	} else {
		/* OV5693_16BIT */
		__be16 *wdata = (void *)&data[2];

		*wdata = cpu_to_be16(val);
	}

	ret = ov5693_i2c_write(client, len, data);
	if (ret)
		dev_err(&client->dev,
			"write error: wrote 0x%x to offset 0x%x error %d",
			val, reg, ret);

	return ret;
}

/*
 * ov5693_write_reg_array - Initializes a list of OV5693 registers
 * @client: i2c driver client structure
 * @reglist: list of registers to be written
 *
 * This function initializes a list of registers. When consecutive addresses
 * are found in a row on the list, this function creates a buffer and sends
 * consecutive data in a single i2c_transfer().
 *
 * __ov5693_flush_reg_array, __ov5693_buf_reg_array() and
 * __ov5693_write_reg_is_consecutive() are internal functions to
 * ov5693_write_reg_array_fast() and should be not used anywhere else.
 *
 */

static int __ov5693_flush_reg_array(struct i2c_client *client,
				    struct ov5693_write_ctrl *ctrl)
{
	u16 size;
	__be16 *reg = (void *)&ctrl->buffer.addr;

	if (ctrl->index == 0)
		return 0;

	size = sizeof(u16) + ctrl->index; /* 16-bit address + data */

	*reg = cpu_to_be16(ctrl->buffer.addr);
	ctrl->index = 0;

	return ov5693_i2c_write(client, size, (u8 *)reg);
}

static int __ov5693_buf_reg_array(struct i2c_client *client,
				  struct ov5693_write_ctrl *ctrl,
				  const struct ov5693_reg *next)
{
	int size;
	__be16 *data16;

	switch (next->type) {
	case OV5693_8BIT:
		size = 1;
		ctrl->buffer.data[ctrl->index] = (u8)next->val;
		break;
	case OV5693_16BIT:
		size = 2;

		data16 = (void *)&ctrl->buffer.data[ctrl->index];
		*data16 = cpu_to_be16((u16)next->val);
		break;
	default:
		return -EINVAL;
	}

	/* When first item is added, we need to store its starting address */
	if (ctrl->index == 0)
		ctrl->buffer.addr = next->reg;

	ctrl->index += size;

	/*
	 * Buffer cannot guarantee free space for u32? Better flush it to avoid
	 * possible lack of memory for next item.
	 */
	if (ctrl->index + sizeof(u16) >= OV5693_MAX_WRITE_BUF_SIZE)
		return __ov5693_flush_reg_array(client, ctrl);

	return 0;
}

static int __ov5693_write_reg_is_consecutive(struct i2c_client *client,
					     struct ov5693_write_ctrl *ctrl,
					     const struct ov5693_reg *next)
{
	if (ctrl->index == 0)
		return 1;

	return ctrl->buffer.addr + ctrl->index == next->reg;
}

static int ov5693_write_reg_array(struct i2c_client *client,
				  const struct ov5693_reg *reglist)
{
	const struct ov5693_reg *next = reglist;
	struct ov5693_write_ctrl ctrl;
	int err;

	ctrl.index = 0;
	for (; next->type != OV5693_TOK_TERM; next++) {
		switch (next->type & OV5693_TOK_MASK) {
		case OV5693_TOK_DELAY:
			err = __ov5693_flush_reg_array(client, &ctrl);
			if (err)
				return err;
			msleep(next->val);
			break;
		default:
			/*
			 * If next address is not consecutive, data needs to be
			 * flushed before proceed.
			 */
			if (!__ov5693_write_reg_is_consecutive(client, &ctrl,
							       next)) {
				err = __ov5693_flush_reg_array(client, &ctrl);
				if (err)
					return err;
			}
			err = __ov5693_buf_reg_array(client, &ctrl, next);
			if (err) {
				dev_err(&client->dev,
					"%s: write error, aborted\n",
					__func__);
				return err;
			}
			break;
		}
	}

	return __ov5693_flush_reg_array(client, &ctrl);
}

static long __ov5693_set_exposure(struct v4l2_subdev *sd, int coarse_itg,
				  int gain, int digitgain)

{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	struct ov5693_device *dev = to_ov5693_sensor(sd);
	u16 vts, hts;
	int ret, exp_val;

	hts = ov5693_res[dev->fmt_idx].pixels_per_line;
	vts = ov5693_res[dev->fmt_idx].lines_per_frame;
	/*
	 * If coarse_itg is larger than 1<<15, can not write to reg directly.
	 * The way is to write coarse_itg/2 to the reg, meanwhile write 2*hts
	 * to the reg.
	 */
	if (coarse_itg > (1 << 15)) {
		hts = hts * 2;
		coarse_itg = (int)coarse_itg / 2;
	}
	/* group hold */
	ret = ov5693_write_reg(client, OV5693_8BIT,
			       OV5693_GROUP_ACCESS, 0x00);
	if (ret) {
		dev_err(&client->dev, "%s: write %x error, aborted\n",
			__func__, OV5693_GROUP_ACCESS);
		return ret;
	}

	ret = ov5693_write_reg(client, OV5693_8BIT,
			       OV5693_TIMING_HTS_H, (hts >> 8) & 0xFF);
	if (ret) {
		dev_err(&client->dev, "%s: write %x error, aborted\n",
			__func__, OV5693_TIMING_HTS_H);
		return ret;
	}

	ret = ov5693_write_reg(client, OV5693_8BIT,
			       OV5693_TIMING_HTS_L, hts & 0xFF);
	if (ret) {
		dev_err(&client->dev, "%s: write %x error, aborted\n",
			__func__, OV5693_TIMING_HTS_L);
		return ret;
	}
	/* Increase the VTS to match exposure + MARGIN */
	if (coarse_itg > vts - OV5693_INTEGRATION_TIME_MARGIN)
		vts = (u16)coarse_itg + OV5693_INTEGRATION_TIME_MARGIN;

	ret = ov5693_write_reg(client, OV5693_8BIT,
			       OV5693_TIMING_VTS_H, (vts >> 8) & 0xFF);
	if (ret) {
		dev_err(&client->dev, "%s: write %x error, aborted\n",
			__func__, OV5693_TIMING_VTS_H);
		return ret;
	}

	ret = ov5693_write_reg(client, OV5693_8BIT,
			       OV5693_TIMING_VTS_L, vts & 0xFF);
	if (ret) {
		dev_err(&client->dev, "%s: write %x error, aborted\n",
			__func__, OV5693_TIMING_VTS_L);
		return ret;
	}

	/* set exposure */

	/* Lower four bit should be 0*/
	exp_val = coarse_itg << 4;
	ret = ov5693_write_reg(client, OV5693_8BIT,
			       OV5693_EXPOSURE_L, exp_val & 0xFF);
	if (ret) {
		dev_err(&client->dev, "%s: write %x error, aborted\n",
			__func__, OV5693_EXPOSURE_L);
		return ret;
	}

	ret = ov5693_write_reg(client, OV5693_8BIT,
			       OV5693_EXPOSURE_M, (exp_val >> 8) & 0xFF);
	if (ret) {
		dev_err(&client->dev, "%s: write %x error, aborted\n",
			__func__, OV5693_EXPOSURE_M);
		return ret;
	}

	ret = ov5693_write_reg(client, OV5693_8BIT,
			       OV5693_EXPOSURE_H, (exp_val >> 16) & 0x0F);
	if (ret) {
		dev_err(&client->dev, "%s: write %x error, aborted\n",
			__func__, OV5693_EXPOSURE_H);
		return ret;
	}

	/* Analog gain */
	ret = ov5693_write_reg(client, OV5693_8BIT,
			       OV5693_AGC_L, gain & 0xff);
	if (ret) {
		dev_err(&client->dev, "%s: write %x error, aborted\n",
			__func__, OV5693_AGC_L);
		return ret;
	}

	ret = ov5693_write_reg(client, OV5693_8BIT,
			       OV5693_AGC_H, (gain >> 8) & 0xff);
	if (ret) {
		dev_err(&client->dev, "%s: write %x error, aborted\n",
			__func__, OV5693_AGC_H);
		return ret;
	}

	/* Digital gain */
	if (digitgain) {
		ret = ov5693_write_reg(client, OV5693_16BIT,
				       OV5693_MWB_RED_GAIN_H, digitgain);
		if (ret) {
			dev_err(&client->dev, "%s: write %x error, aborted\n",
				__func__, OV5693_MWB_RED_GAIN_H);
			return ret;
		}

		ret = ov5693_write_reg(client, OV5693_16BIT,
				       OV5693_MWB_GREEN_GAIN_H, digitgain);
		if (ret) {
			dev_err(&client->dev, "%s: write %x error, aborted\n",
				__func__, OV5693_MWB_RED_GAIN_H);
			return ret;
		}

		ret = ov5693_write_reg(client, OV5693_16BIT,
				       OV5693_MWB_BLUE_GAIN_H, digitgain);
		if (ret) {
			dev_err(&client->dev, "%s: write %x error, aborted\n",
				__func__, OV5693_MWB_RED_GAIN_H);
			return ret;
		}
	}

	/* End group */
	ret = ov5693_write_reg(client, OV5693_8BIT,
			       OV5693_GROUP_ACCESS, 0x10);
	if (ret)
		return ret;

	/* Delay launch group */
	ret = ov5693_write_reg(client, OV5693_8BIT,
			       OV5693_GROUP_ACCESS, 0xa0);
	if (ret)
		return ret;
	return ret;
}

static int ov5693_set_exposure(struct v4l2_subdev *sd, int exposure,
			       int gain, int digitgain)
{
	struct ov5693_device *dev = to_ov5693_sensor(sd);
	int ret;

	mutex_lock(&dev->input_lock);
	ret = __ov5693_set_exposure(sd, exposure, gain, digitgain);
	mutex_unlock(&dev->input_lock);

	return ret;
}

static int ov5693_read_otp_reg_array(struct i2c_client *client, u16 size,
				     u16 addr, u8 *buf)
{
	u16 index;
	int ret;
	u16 *p_val = NULL;

	for (index = 0; index <= size; index++) {
		p_val = (u16 *)(buf + index);
		ret =
		    ov5693_read_reg(client, OV5693_8BIT, addr + index,
				    p_val);
		if (ret)
			return ret;
	}

	return 0;
}

static int __ov5693_otp_read(struct v4l2_subdev *sd, u8 *buf)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	struct ov5693_device *dev = to_ov5693_sensor(sd);
	int ret;
	int i;
	u8 *b = buf;

	dev->otp_size = 0;
	for (i = 1; i < OV5693_OTP_BANK_MAX; i++) {
		/*set bank NO and OTP read mode. */
		ret = ov5693_write_reg(client, OV5693_8BIT, OV5693_OTP_BANK_REG,
				       (i | 0xc0));	//[7:6] 2'b11 [5:0] bank no
		if (ret) {
			dev_err(&client->dev, "failed to prepare OTP page\n");
			return ret;
		}
		//dev_dbg(&client->dev, "write 0x%x->0x%x\n",OV5693_OTP_BANK_REG,(i|0xc0));

		/*enable read */
		ret = ov5693_write_reg(client, OV5693_8BIT, OV5693_OTP_READ_REG,
				       OV5693_OTP_MODE_READ);	// enable :1
		if (ret) {
			dev_err(&client->dev,
				"failed to set OTP reading mode page");
			return ret;
		}
		//dev_dbg(&client->dev, "write 0x%x->0x%x\n",
		//	OV5693_OTP_READ_REG,OV5693_OTP_MODE_READ);

		/* Reading the OTP data array */
		ret = ov5693_read_otp_reg_array(client, OV5693_OTP_BANK_SIZE,
						OV5693_OTP_START_ADDR,
						b);
		if (ret) {
			dev_err(&client->dev, "failed to read OTP data\n");
			return ret;
		}

		//dev_dbg(&client->dev,
		//	"BANK[%2d] %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
		//	i, *b, *(b + 1), *(b + 2), *(b + 3), *(b + 4), *(b + 5),
		//	*(b + 6), *(b + 7), *(b + 8), *(b + 9), *(b + 10),
		//	*(b + 11), *(b + 12), *(b + 13), *(b + 14), *(b + 15));

		//Intel OTP map, try to read 320byts first.
		if (i == 21) {
			if ((*b) == 0) {
				dev->otp_size = 320;
				break;
			}
			/* (*b) != 0 */
			b = buf;
			continue;
		} else if (i == 24) {
			// if the first 320bytes data doesn't not exist,
			// try to read the next 32bytes data.
			if ((*b) == 0) {
				dev->otp_size = 32;
				break;
			}
			/* (*b) != 0 */
			b = buf;
			continue;
		} else if (i == 27) {
			// if the prvious 32bytes data doesn't exist,
			// try to read the next 32bytes data again.
			if ((*b) == 0) {
				dev->otp_size = 32;
				break;
			}
			/* (*b) != 0 */
			dev->otp_size = 0;	// no OTP data.
			break;
		}

		b = b + OV5693_OTP_BANK_SIZE;
	}
	return 0;
}

/*
 * Read otp data and store it into a kmalloced buffer.
 * The caller must kfree the buffer when no more needed.
 * @size: set to the size of the returned otp data.
 */
static void *ov5693_otp_read(struct v4l2_subdev *sd)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	u8 *buf;
	int ret;

	buf = devm_kzalloc(&client->dev, (OV5693_OTP_DATA_SIZE + 16), GFP_KERNEL);
	if (!buf)
		return ERR_PTR(-ENOMEM);

	//otp valid after mipi on and sw stream on
	ret = ov5693_write_reg(client, OV5693_8BIT, OV5693_FRAME_OFF_NUM, 0x00);

	ret = ov5693_write_reg(client, OV5693_8BIT,
			       OV5693_SW_STREAM, OV5693_START_STREAMING);

	ret = __ov5693_otp_read(sd, buf);

	//mipi off and sw stream off after otp read
	ret = ov5693_write_reg(client, OV5693_8BIT, OV5693_FRAME_OFF_NUM, 0x0f);

	ret = ov5693_write_reg(client, OV5693_8BIT,
			       OV5693_SW_STREAM, OV5693_STOP_STREAMING);

	/* Driver has failed to find valid data */
	if (ret) {
		dev_err(&client->dev, "sensor found no valid OTP data\n");
		return ERR_PTR(ret);
	}

	return buf;
}

/*
 * This returns the exposure time being used. This should only be used
 * for filling in EXIF data, not for actual image processing.
 */
static int ov5693_q_exposure(struct v4l2_subdev *sd, s32 *value)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	u16 reg_v, reg_v2;
	int ret;

	/* get exposure */
	ret = ov5693_read_reg(client, OV5693_8BIT,
			      OV5693_EXPOSURE_L,
			      &reg_v);
	if (ret)
		goto err;

	ret = ov5693_read_reg(client, OV5693_8BIT,
			      OV5693_EXPOSURE_M,
			      &reg_v2);
	if (ret)
		goto err;

	reg_v += reg_v2 << 8;
	ret = ov5693_read_reg(client, OV5693_8BIT,
			      OV5693_EXPOSURE_H,
			      &reg_v2);
	if (ret)
		goto err;

	*value = reg_v + (((u32)reg_v2 << 16));
err:
	return ret;
}

static int ad5823_t_focus_vcm(struct v4l2_subdev *sd, u16 val)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	int ret = -EINVAL;
	u8 vcm_code;

	ret = ad5823_i2c_read(client, AD5823_REG_VCM_CODE_MSB, &vcm_code);
	if (ret)
		return ret;

	/* set reg VCM_CODE_MSB Bit[1:0] */
	vcm_code = (vcm_code & VCM_CODE_MSB_MASK) |
		   ((val >> 8) & ~VCM_CODE_MSB_MASK);
	ret = ad5823_i2c_write(client, AD5823_REG_VCM_CODE_MSB, vcm_code);
	if (ret)
		return ret;

	/* set reg VCM_CODE_LSB Bit[7:0] */
	ret = ad5823_i2c_write(client, AD5823_REG_VCM_CODE_LSB, (val & 0xff));
	if (ret)
		return ret;

	/* set required vcm move time */
	vcm_code = AD5823_RESONANCE_PERIOD / AD5823_RESONANCE_COEF
		   - AD5823_HIGH_FREQ_RANGE;
	ret = ad5823_i2c_write(client, AD5823_REG_VCM_MOVE_TIME, vcm_code);

	return ret;
}

static int ad5823_t_focus_abs(struct v4l2_subdev *sd, s32 value)
{
	value = min(value, AD5823_MAX_FOCUS_POS);
	return ad5823_t_focus_vcm(sd, value);
}

static int ov5693_t_focus_abs(struct v4l2_subdev *sd, s32 value)
{
	struct ov5693_device *dev = to_ov5693_sensor(sd);
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	int ret = 0;

	dev_dbg(&client->dev, "%s: FOCUS_POS: 0x%x\n", __func__, value);
	value = clamp(value, 0, OV5693_VCM_MAX_FOCUS_POS);
	if (dev->vcm == VCM_DW9714) {
		if (dev->vcm_update) {
			ret = vcm_dw_i2c_write(client, VCM_PROTECTION_OFF);
			if (ret)
				return ret;
			ret = vcm_dw_i2c_write(client, DIRECT_VCM);
			if (ret)
				return ret;
			ret = vcm_dw_i2c_write(client, VCM_PROTECTION_ON);
			if (ret)
				return ret;
			dev->vcm_update = false;
		}
		ret = vcm_dw_i2c_write(client,
				       vcm_val(value, VCM_DEFAULT_S));
	} else if (dev->vcm == VCM_AD5823) {
		ad5823_t_focus_abs(sd, value);
	}
	if (ret == 0) {
		dev->number_of_steps = value - dev->focus;
		dev->focus = value;
		dev->timestamp_t_focus_abs = ktime_get();
	} else {
		dev_err(&client->dev, "%s: i2c failed. ret %d\n", __func__, ret);
	}

	return ret;
}

static int ov5693_t_focus_rel(struct v4l2_subdev *sd, s32 value)
{
	struct ov5693_device *dev = to_ov5693_sensor(sd);

	return ov5693_t_focus_abs(sd, dev->focus + value);
}

static int ov5693_update_hflip(struct v4l2_subdev *sd)
{
	struct ov5693_device *dev = to_ov5693_sensor(sd);
	struct i2c_client *client = v4l2_get_subdevdata(sd);

	u8 hflip = dev->hflip ? OV5693_TIMING_REG21_FLIP :
				      OV5693_TIMING_REG21_NORMAL;

	return ov5693_write_reg(client, OV5693_8BIT, OV5693_TIMING_REG21,
				hflip);
}

static int ov5693_update_vflip(struct v4l2_subdev *sd)
{
	struct ov5693_device *dev = to_ov5693_sensor(sd);
	struct i2c_client *client = v4l2_get_subdevdata(sd);

	u8 vflip = dev->vflip ? OV5693_TIMING_REG20_FLIP :
				      OV5693_TIMING_REG20_NORMAL;
	return ov5693_write_reg(client, OV5693_8BIT, OV5693_TIMING_REG20,
				vflip);
}

#define DELAY_PER_STEP_NS	1000000
#define DELAY_MAX_PER_STEP_NS	(1000000 * 1023)

static int ov5693_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct ov5693_device *dev =
	    container_of(ctrl->handler, struct ov5693_device, ctrl_handler);
	struct i2c_client *client = v4l2_get_subdevdata(&dev->sd);
	int ret = 0;

	switch (ctrl->id) {
	case V4L2_CID_FOCUS_ABSOLUTE:
		dev_dbg(&client->dev, "%s: CID_FOCUS_ABSOLUTE:%d.\n",
			__func__, ctrl->val);
		ret = ov5693_t_focus_abs(&dev->sd, ctrl->val);
		break;
	case V4L2_CID_FOCUS_RELATIVE:
		dev_dbg(&client->dev, "%s: CID_FOCUS_RELATIVE:%d.\n",
			__func__, ctrl->val);
		ret = ov5693_t_focus_rel(&dev->sd, ctrl->val);
		break;
	case V4L2_CID_HFLIP:
		dev_info(&client->dev, "%s: CID_HFLIP:%d.\n", __func__,
			 ctrl->val);
		dev->hflip = ctrl->val;
		ret = ov5693_update_hflip(&dev->sd);
		break;
	case V4L2_CID_VFLIP:
		dev_info(&client->dev, "%s: CID_VFLIP:%d.\n", __func__,
			 ctrl->val);
		dev->vflip = ctrl->val;
		ret = ov5693_update_vflip(&dev->sd);
		break;
	default:
		ret = -EINVAL;
	}
	return ret;
}

static int ov5693_g_volatile_ctrl(struct v4l2_ctrl *ctrl)
{
	struct ov5693_device *dev =
	    container_of(ctrl->handler, struct ov5693_device, ctrl_handler);
	int ret = 0;

	switch (ctrl->id) {
	case V4L2_CID_EXPOSURE_ABSOLUTE:
		ret = ov5693_q_exposure(&dev->sd, &ctrl->val);
		break;
	case V4L2_CID_FOCUS_ABSOLUTE:
		/* NOTE: there was atomisp-specific function ov5693_q_focus_abs() */
		break;
	default:
		ret = -EINVAL;
	}

	return ret;
}

static const struct v4l2_ctrl_ops ctrl_ops = {
	.s_ctrl = ov5693_s_ctrl,
	.g_volatile_ctrl = ov5693_g_volatile_ctrl
};

static const struct v4l2_ctrl_config ov5693_controls[] = {
	{
		.ops = &ctrl_ops,
		.id = V4L2_CID_EXPOSURE_ABSOLUTE,
		.type = V4L2_CTRL_TYPE_INTEGER,
		.name = "exposure",
		.min = 0x0,
		.max = 0xffff,
		.step = 0x01,
		.def = 0x00,
		.flags = 0,
	},
	{
		.ops = &ctrl_ops,
		.id = V4L2_CID_FOCUS_ABSOLUTE,
		.type = V4L2_CTRL_TYPE_INTEGER,
		.name = "focus move absolute",
		.min = 0,
		.max = OV5693_VCM_MAX_FOCUS_POS,
		.step = 1,
		.def = 0,
		.flags = 0,
	},
	{
		.ops = &ctrl_ops,
		.id = V4L2_CID_FOCUS_RELATIVE,
		.type = V4L2_CTRL_TYPE_INTEGER,
		.name = "focus move relative",
		.min = OV5693_VCM_MAX_FOCUS_NEG,
		.max = OV5693_VCM_MAX_FOCUS_POS,
		.step = 1,
		.def = 0,
		.flags = 0,
	},
	{
		.ops = &ctrl_ops,
		.id = V4L2_CID_HFLIP,
		.type = V4L2_CTRL_TYPE_BOOLEAN,
		.name = "sensor flipped horizontally",
		.max = 1,
		.step = 1,
	},
	{
		.ops = &ctrl_ops,
		.id = V4L2_CID_VFLIP,
		.type = V4L2_CTRL_TYPE_BOOLEAN,
		.name = "sensor flipped vertically",
		.max = 1,
		.step = 1,
	},
};

static int ov5693_init(struct v4l2_subdev *sd)
{
	struct ov5693_device *dev = to_ov5693_sensor(sd);
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	int ret;

	if (!dev->has_vcm)
		return 0;

	dev_info(&client->dev, "%s\n", __func__);
	mutex_lock(&dev->input_lock);
	dev->vcm_update = false;

	if (dev->vcm == VCM_AD5823) {
		ret = vcm_ad_i2c_wr8(client, 0x01, 0x01); /* vcm init test */
		if (ret)
			dev_err(&client->dev,
				"vcm reset failed\n");
		/*change the mode*/
		ret = ad5823_i2c_write(client, AD5823_REG_VCM_CODE_MSB,
				       AD5823_RING_CTRL_ENABLE);
		if (ret)
			dev_err(&client->dev,
				"vcm enable ringing failed\n");
		ret = ad5823_i2c_write(client, AD5823_REG_MODE,
				       AD5823_ARC_RES1);
		if (ret)
			dev_err(&client->dev,
				"vcm change mode failed\n");
	}

	/*change initial focus value for ad5823*/
	if (dev->vcm == VCM_AD5823) {
		dev->focus = AD5823_INIT_FOCUS_POS;
		ov5693_t_focus_abs(sd, AD5823_INIT_FOCUS_POS);
	} else {
		dev->focus = 0;
		ov5693_t_focus_abs(sd, 0);
	}

	mutex_unlock(&dev->input_lock);

	return 0;
}

static int __power_up(struct v4l2_subdev *sd)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	struct ov5693_device *sensor = to_ov5693_sensor(sd);
	int ret;

	if (sensor->indicator_led)
		gpiod_set_value_cansleep(sensor->indicator_led, 1);

	ret = regulator_bulk_enable(OV5693_NUM_SUPPLIES, sensor->supplies);
	if (ret)
		goto fail_power;

	__cci_delay(up_delay);

	return 0;

fail_power:
	if (sensor->indicator_led)
		gpiod_set_value_cansleep(sensor->indicator_led, 0);
	dev_err(&client->dev, "sensor power-up failed\n");

	return ret;
}

static int power_down(struct v4l2_subdev *sd)
{
	struct ov5693_device *dev = to_ov5693_sensor(sd);

	dev->focus = OV5693_INVALID_CONFIG;

	if (dev->indicator_led)
		gpiod_set_value_cansleep(dev->indicator_led, 0);

	return regulator_bulk_disable(OV5693_NUM_SUPPLIES, dev->supplies);
}

static int power_up(struct v4l2_subdev *sd)
{
	static const int retry_count = 4;
	int i, ret;

	for (i = 0; i < retry_count; i++) {
		ret = __power_up(sd);
		if (!ret)
			return 0;

		power_down(sd);
	}
	return ret;
}

static int ov5693_s_power(struct v4l2_subdev *sd, int on)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	int ret;

	dev_info(&client->dev, "%s: on %d\n", __func__, on);

	if (on == 0)
		return power_down(sd);

	/* on == 1 */
	ret = power_up(sd);
	if (!ret) {
		ret = ov5693_init(sd);
		/* restore settings */
		ov5693_res = ov5693_res_preview;
		N_RES = N_RES_PREVIEW;
	}

	return ret;
}

/*
 * distance - calculate the distance
 * @res: resolution
 * @w: width
 * @h: height
 *
 * Get the gap between res_w/res_h and w/h.
 * distance = (res_w/res_h - w/h) / (w/h) * 8192
 * res->width/height smaller than w/h wouldn't be considered.
 * The gap of ratio larger than 1/8 wouldn't be considered.
 * Returns the value of gap or -1 if fail.
 */
#define LARGEST_ALLOWED_RATIO_MISMATCH 1024
static int distance(struct ov5693_resolution *res, u32 w, u32 h)
{
	int ratio;
	int distance;

	if (w == 0 || h == 0 ||
	    res->width < w || res->height < h)
		return -1;

	ratio = res->width << 13;
	ratio /= w;
	ratio *= h;
	ratio /= res->height;

	distance = abs(ratio - 8192);

	if (distance > LARGEST_ALLOWED_RATIO_MISMATCH)
		return -1;

	return distance;
}

/* Return the nearest higher resolution index
 * Firstly try to find the approximate aspect ratio resolution
 * If we find multiple same AR resolutions, choose the
 * minimal size.
 */
static int nearest_resolution_index(int w, int h)
{
	int i;
	int idx = -1;
	int dist;
	int min_dist = INT_MAX;
	int min_res_w = INT_MAX;
	struct ov5693_resolution *tmp_res = NULL;

	for (i = 0; i < N_RES; i++) {
		tmp_res = &ov5693_res[i];
		dist = distance(tmp_res, w, h);
		if (dist == -1)
			continue;
		if (dist < min_dist) {
			min_dist = dist;
			idx = i;
			min_res_w = ov5693_res[i].width;
			continue;
		}
		if (dist == min_dist && ov5693_res[i].width < min_res_w)
			idx = i;
	}

	return idx;
}

static int get_resolution_index(int w, int h)
{
	int i;

	for (i = 0; i < N_RES; i++) {
		if (w != ov5693_res[i].width)
			continue;
		if (h != ov5693_res[i].height)
			continue;

		return i;
	}

	return -1;
}

/* TODO: remove it. */
static int startup(struct v4l2_subdev *sd)
{
	struct ov5693_device *dev = to_ov5693_sensor(sd);
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	int ret = 0;

	ret = ov5693_write_reg(client, OV5693_8BIT,
			       OV5693_SW_RESET, 0x01);
	if (ret) {
		dev_err(&client->dev, "ov5693 reset err.\n");
		return ret;
	}

	ret = ov5693_write_reg_array(client, ov5693_global_setting);
	if (ret) {
		dev_err(&client->dev, "ov5693 write register err.\n");
		return ret;
	}

	ret = ov5693_write_reg_array(client, ov5693_res[dev->fmt_idx].regs);
	if (ret) {
		dev_err(&client->dev, "ov5693 write register err.\n");
		return ret;
	}

	ret = ov5693_update_hflip(sd);
	if (ret) {
		dev_err(&client->dev, "ov5693 hflip err.\n");
		return ret;
	}

	ret = ov5693_update_vflip(sd);
	if (ret) {
		dev_err(&client->dev, "ov5693 vflip err.\n");
		return ret;
	}

	return ret;
}

static int ov5693_set_fmt(struct v4l2_subdev *sd,
			  struct v4l2_subdev_pad_config *cfg,
			  struct v4l2_subdev_format *format)
{
	struct v4l2_mbus_framefmt *fmt = &format->format;
	struct ov5693_device *dev = to_ov5693_sensor(sd);
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	int ret = 0;
	int idx;
	int cnt;

	if (format->pad)
		return -EINVAL;
	if (!fmt)
		return -EINVAL;

	mutex_lock(&dev->input_lock);
	idx = nearest_resolution_index(fmt->width, fmt->height);
	if (idx == -1) {
		/* return the largest resolution */
		fmt->width = ov5693_res[N_RES - 1].width;
		fmt->height = ov5693_res[N_RES - 1].height;
	} else {
		fmt->width = ov5693_res[idx].width;
		fmt->height = ov5693_res[idx].height;
	}

	fmt->code = MEDIA_BUS_FMT_SBGGR10_1X10;
	if (format->which == V4L2_SUBDEV_FORMAT_TRY) {
		cfg->try_fmt = *fmt;
		ret = 0;
		goto mutex_unlock;
	}

	dev->fmt_idx = get_resolution_index(fmt->width, fmt->height);
	if (dev->fmt_idx == -1) {
		dev_err(&client->dev, "get resolution fail\n");
		ret = -EINVAL;
		goto mutex_unlock;
	}

	for (cnt = 0; cnt < OV5693_POWER_UP_RETRY_NUM; cnt++) {
//		power_down(sd);
		ret = power_up(sd);
		if (ret) {
			dev_err(&client->dev, "power up failed\n");
			continue;
		}

		mutex_unlock(&dev->input_lock);
		ov5693_init(sd);
		mutex_lock(&dev->input_lock);
		ret = startup(sd);
		if (ret)
			dev_err(&client->dev, " startup() FAILED!\n");
		else
			break;
	}
	if (cnt == OV5693_POWER_UP_RETRY_NUM) {
		dev_err(&client->dev, "power up failed, gave up\n");
		goto mutex_unlock;
	}

	/*
	 * After sensor settings are set to HW, sometimes stream is started.
	 * This would cause ISP timeout because ISP is not ready to receive
	 * data yet. So add stop streaming here.
	 */
	ret = ov5693_write_reg(client, OV5693_8BIT, OV5693_SW_STREAM,
			       OV5693_STOP_STREAMING);
	if (ret)
		dev_warn(&client->dev, "ov5693 stream off err\n");

mutex_unlock:
	mutex_unlock(&dev->input_lock);
	return ret;
}

static int ov5693_get_fmt(struct v4l2_subdev *sd,
			  struct v4l2_subdev_pad_config *cfg,
			  struct v4l2_subdev_format *format)
{
	struct v4l2_mbus_framefmt *fmt = &format->format;
	struct ov5693_device *dev = to_ov5693_sensor(sd);

	if (format->pad)
		return -EINVAL;

	if (!fmt)
		return -EINVAL;

	fmt->width = ov5693_res[dev->fmt_idx].width;
	fmt->height = ov5693_res[dev->fmt_idx].height;
	fmt->code = MEDIA_BUS_FMT_SBGGR10_1X10;

	return 0;
}

static int ov5693_detect(struct i2c_client *client)
{
	struct i2c_adapter *adapter = client->adapter;
	u16 high, low;
	int ret;
	u16 id;
	u8 revision;

	if (!i2c_check_functionality(adapter, I2C_FUNC_I2C))
		return -ENODEV;

	ret = ov5693_read_reg(client, OV5693_8BIT,
			      OV5693_SC_CMMN_CHIP_ID_H, &high);
	if (ret) {
		dev_err(&client->dev, "sensor_id_high = 0x%x\n", high);
		return -ENODEV;
	}
	ret = ov5693_read_reg(client, OV5693_8BIT,
			      OV5693_SC_CMMN_CHIP_ID_L, &low);
	id = ((((u16)high) << 8) | (u16)low);

	if (id != OV5693_ID) {
		dev_err(&client->dev, "sensor ID error 0x%x\n", id);
		return -ENODEV;
	}

	ret = ov5693_read_reg(client, OV5693_8BIT,
			      OV5693_SC_CMMN_SUB_ID, &high);
	revision = (u8)high & 0x0f;

	dev_info(&client->dev, "sensor_revision = 0x%x\n", revision);
	dev_info(&client->dev, "sensor_address = 0x%02x\n", client->addr);
	dev_info(&client->dev, "detect ov5693 success\n");
	return 0;
}

static int ov5693_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct ov5693_device *dev = to_ov5693_sensor(sd);
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	int ret;

	mutex_lock(&dev->input_lock);

	/* power_on() here before streaming for regular PCs. */
	if (enable) {
		ret = power_up(sd);
		if (ret) {
			dev_err(&client->dev, "sensor power-up error\n");
//			power_down(sd);
			goto out;
		}
	}

	ret = ov5693_write_reg(client, OV5693_8BIT, OV5693_SW_STREAM,
			       enable ? OV5693_START_STREAMING :
			       OV5693_STOP_STREAMING);

	/* power_off() here after streaming for regular PCs. */
	if (!enable)
		power_down(sd);

out:
	mutex_unlock(&dev->input_lock);

	return ret;
}

static int ov5693_s_config(struct v4l2_subdev *sd, int irq)
{
	struct ov5693_device *dev = to_ov5693_sensor(sd);
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	int ret = 0;

	mutex_lock(&dev->input_lock);
	/* power off the module, then power on it in future
	 * as first power on by board may not fulfill the
	 * power on sequqence needed by the module
	 */
//	ret = power_down(sd);
//	if (ret) {
//		dev_err(&client->dev, "ov5693 power-off err.\n");
//		goto fail_power_off;
//	}

	ret = power_up(sd);
	if (ret) {
		dev_err(&client->dev, "ov5693 power-up err.\n");
		goto fail_power_on;
	}

	if (!dev->vcm)
		dev->vcm = vcm_detect(client);

	/* config & detect sensor */
	ret = ov5693_detect(client);
	if (ret) {
		dev_err(&client->dev, "ov5693_detect err s_config.\n");
		goto fail_power_on;
	}

	dev->otp_data = ov5693_otp_read(sd);

	/* turn off sensor, after probed */
	ret = power_down(sd);
	if (ret) {
		dev_err(&client->dev, "ov5693 power-off err.\n");
		goto fail_power_on;
	}
	mutex_unlock(&dev->input_lock);

	return ret;

fail_power_on:
	power_down(sd);
	dev_err(&client->dev, "sensor power-gating failed\n");
fail_power_off:
	mutex_unlock(&dev->input_lock);
	return ret;
}

static int ov5693_g_frame_interval(struct v4l2_subdev *sd,
				   struct v4l2_subdev_frame_interval *interval)
{
	struct ov5693_device *dev = to_ov5693_sensor(sd);

	interval->interval.numerator = 1;
	interval->interval.denominator = ov5693_res[dev->fmt_idx].fps;

	return 0;
}

static int ov5693_enum_mbus_code(struct v4l2_subdev *sd,
				 struct v4l2_subdev_pad_config *cfg,
				 struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->index >= MAX_FMTS)
		return -EINVAL;

	code->code = MEDIA_BUS_FMT_SBGGR10_1X10;
	return 0;
}

static int ov5693_enum_frame_size(struct v4l2_subdev *sd,
				  struct v4l2_subdev_pad_config *cfg,
				  struct v4l2_subdev_frame_size_enum *fse)
{
	int index = fse->index;

	if (index >= N_RES)
		return -EINVAL;

	fse->min_width = ov5693_res[index].width;
	fse->min_height = ov5693_res[index].height;
	fse->max_width = ov5693_res[index].width;
	fse->max_height = ov5693_res[index].height;

	return 0;
}

static const struct v4l2_subdev_video_ops ov5693_video_ops = {
	.s_stream = ov5693_s_stream,
	.g_frame_interval = ov5693_g_frame_interval,
};

static const struct v4l2_subdev_core_ops ov5693_core_ops = {
	.s_power = ov5693_s_power,
};

static const struct v4l2_subdev_pad_ops ov5693_pad_ops = {
	.enum_mbus_code = ov5693_enum_mbus_code,
	.enum_frame_size = ov5693_enum_frame_size,
	.get_fmt = ov5693_get_fmt,
	.set_fmt = ov5693_set_fmt,
};

static const struct v4l2_subdev_ops ov5693_ops = {
	.core = &ov5693_core_ops,
	.video = &ov5693_video_ops,
	.pad = &ov5693_pad_ops,
};

static int ov5693_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct ov5693_device *ov5693 = to_ov5693_sensor(sd);
	unsigned int i = OV5693_NUM_SUPPLIES;

	dev_info(&client->dev, "%s...\n", __func__);

	gpiod_put(ov5693->reset);
	gpiod_put(ov5693->indicator_led);

	while (i--)
		regulator_put(ov5693->supplies[i].consumer);

	v4l2_async_unregister_subdev(sd);

	media_entity_cleanup(&ov5693->sd.entity);
	v4l2_ctrl_handler_free(&ov5693->ctrl_handler);
	kfree(ov5693);

	return 0;
}

static int ov5693_init_controls(struct ov5693_device *ov5693)
{
	struct i2c_client *client = v4l2_get_subdevdata(&ov5693->sd);
	struct v4l2_ctrl *ctrl;
	unsigned int i;
	int ret;

	ret = v4l2_ctrl_handler_init(&ov5693->ctrl_handler,
				     ARRAY_SIZE(ov5693_controls));
	if (ret) {
		ov5693_remove(client);
		return ret;
	}

	for (i = 0; i < ARRAY_SIZE(ov5693_controls); i++)
		v4l2_ctrl_new_custom(&ov5693->ctrl_handler,
				     &ov5693_controls[i],
				     NULL);

	/* link freq */
	ctrl = v4l2_ctrl_new_int_menu(&ov5693->ctrl_handler, NULL,
				      V4L2_CID_LINK_FREQ,
				      0, 0, link_freq_menu_items);
	if (ctrl)
		ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	/* pixel rate */
	v4l2_ctrl_new_std(&ov5693->ctrl_handler, NULL, V4L2_CID_PIXEL_RATE,
			  0, OV5693_PIXEL_RATE, 1, OV5693_PIXEL_RATE);

	if (ov5693->ctrl_handler.error) {
		ov5693_remove(client);
		return ov5693->ctrl_handler.error;
	}

	/* Use same lock for controls as for everything else. */
	ov5693->ctrl_handler.lock = &ov5693->input_lock;
	ov5693->sd.ctrl_handler = &ov5693->ctrl_handler;

	return 0;
}

static int ov5693_configure_gpios(struct ov5693_device *ov5693)
{
	ov5693->reset = gpiod_get_index(&ov5693->client->dev, "reset", 0,
					GPIOD_OUT_HIGH);
	if (IS_ERR(ov5693->reset)) {
		dev_err(&ov5693->client->dev, "Couldn't find reset GPIO\n");
		return -EINVAL;
	}

	ov5693->indicator_led = gpiod_get_index_optional(&ov5693->client->dev,
							 "indicator-led", 0, GPIOD_OUT_HIGH);

	if (IS_ERR(ov5693->indicator_led)) {
		dev_err(&ov5693->client->dev, "Couldn't find indicator-led GPIO\n");
		return -EINVAL;
	}
	return 0;
}

static int ov5693_get_regulators(struct ov5693_device *ov5693)
{
	unsigned int i;

	for (i = 0; i < OV5693_NUM_SUPPLIES; i++)
		ov5693->supplies[i].supply = ov5693_supply_names[i];

	return regulator_bulk_get(&ov5693->client->dev,
				       OV5693_NUM_SUPPLIES,
				       ov5693->supplies);
}

static int ov5693_probe(struct i2c_client *client)
{
	struct ov5693_device *ov5693;
	int ret = 0;

	dev_info(&client->dev, "%s() called", __func__);

	ov5693 = kzalloc(sizeof(*ov5693), GFP_KERNEL);
	if (!ov5693)
		return -ENOMEM;

	ov5693->client = client;

	/* check if VCM device exists */
	/* TODO: read from SSDB */
	ov5693->has_vcm = false;

	mutex_init(&ov5693->input_lock);

	v4l2_i2c_subdev_init(&ov5693->sd, client, &ov5693_ops);

	ret = ov5693_configure_gpios(ov5693);
	if (ret)
		goto out_free;

	ret = ov5693_get_regulators(ov5693);
	if (ret)
		goto out_put_reset;

	ret = ov5693_s_config(&ov5693->sd, client->irq);
	if (ret)
		goto out_put_reset;

	ov5693->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	ov5693->pad.flags = MEDIA_PAD_FL_SOURCE;
	ov5693->format.code = MEDIA_BUS_FMT_SBGGR10_1X10;
	ov5693->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;

	ret = ov5693_init_controls(ov5693);
	if (ret)
		ov5693_remove(client);

	ret = media_entity_pads_init(&ov5693->sd.entity, 1, &ov5693->pad);
	if (ret)
		ov5693_remove(client);

	ret = v4l2_async_register_subdev_sensor_common(&ov5693->sd);
	if (ret) {
		dev_err(&client->dev, "failed to register V4L2 subdev: %d", ret);
		goto media_entity_cleanup;
	}

	return ret;

media_entity_cleanup:
	media_entity_cleanup(&ov5693->sd.entity);
out_put_reset:
	gpiod_put(ov5693->reset);
out_free:
	v4l2_device_unregister_subdev(&ov5693->sd);
	kfree(ov5693);
	return ret;
}

static const struct acpi_device_id ov5693_acpi_match[] = {
	{"INT33BE"},
	{},
};
MODULE_DEVICE_TABLE(acpi, ov5693_acpi_match);

static struct i2c_driver ov5693_driver = {
	.driver = {
		.name = "ov5693",
		.acpi_match_table = ov5693_acpi_match,
	},
	.probe_new = ov5693_probe,
	.remove = ov5693_remove,
};
module_i2c_driver(ov5693_driver);

MODULE_DESCRIPTION("A low-level driver for OmniVision 5693 sensors");
MODULE_LICENSE("GPL");

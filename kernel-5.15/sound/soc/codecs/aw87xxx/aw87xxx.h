/* SPDX-License-Identifier: GPL-2.0
 *
 * aw87xxx.h  aw87xxx pa module
 *
 * Copyright (c) 2021 AWINIC Technology CO., LTD
 *
 * Author: Barry <zhaozhongbo@awinic.com>
 *
 * This program is free software; you can redistribute  it and/or modify it
 * under  the terms of  the GNU General  Public License as published by the
 * Free Software Foundation;  either version 2 of the  License, or (at your
 * option) any later version.
 *
 */

#ifndef __AW87XXX_H__
#define __AW87XXX_H__
#include <linux/version.h>
#include <linux/kernel.h>
#include <sound/control.h>
#include <sound/soc.h>

#include "aw87xxx_device.h"
#include "aw87xxx_monitor.h"
#include "aw87xxx_acf_bin.h"

#define AW_CFG_UPDATE_DELAY
#define AW_CFG_UPDATE_DELAY_TIMER	(3000)

#define AW87XXX_NO_OFF_BIN		(0)
#define AW87XXX_OFF_BIN_OK		(1)

#define AW87XXX_PRIVATE_KCONTROL_NUM	(3)
#define AW87XXX_PUBLIC_KCONTROL_NUM	(3)

#define AW_I2C_RETRIES			(5)
#define AW_I2C_RETRY_DELAY		(2)
#define AW_I2C_READ_MSG_NUM		(2)

#define AW87XXX_FW_NAME_MAX		(64)
#define AW_NAME_BUF_MAX			(64)
#define AW_LOAD_FW_RETRIES		(3)

#define AW_DEV_REG_RD_ACCESS		(1 << 0)
#define AW_DEV_REG_WR_ACCESS		(1 << 1)

#define AWRW_ADDR_BYTES			(1)
#define AWRW_DATA_BYTES			(1)
#define AWRW_HDR_LEN			(24)

/***********************************************************
 *
 * aw87xxx codec control compatible with kernel 4.19
 *
 ***********************************************************/
#if KERNEL_VERSION(4, 19, 1) <= LINUX_VERSION_CODE
#define AW_KERNEL_VER_OVER_4_19_1
#endif

#if KERNEL_VERSION(5, 4, 0) <= LINUX_VERSION_CODE
#define AW_KERNEL_VER_OVER_5_4_0
#endif

#if KERNEL_VERSION(5, 10, 0) <= LINUX_VERSION_CODE
#define AW_KERNEL_VER_OVER_5_10_0
#endif
#if KERNEL_VERSION(6, 1, 0) <= LINUX_VERSION_CODE
#define AW_KERNEL_VER_OVER_6_1_0
#endif

#if KERNEL_VERSION(6, 6, 0) <= LINUX_VERSION_CODE
#define AW_KERNEL_VER_OVER_6_6_0
#endif

#ifdef AW_KERNEL_VER_OVER_4_19_1
typedef struct snd_soc_component aw_snd_soc_codec_t;
#else
typedef struct snd_soc_codec aw_snd_soc_codec_t;
#endif

struct aw_componet_codec_ops {
	int (*add_codec_controls)(aw_snd_soc_codec_t *codec,
		const struct snd_kcontrol_new *controls, unsigned int num_controls);
	void (*unregister_codec)(struct device *dev);
};


/********************************************
 *
 * aw87xxx devices attributes
 *
 *******************************************/
enum {
	AWRW_FLAG_WRITE = 0,
	AWRW_FLAG_READ,
};

enum {
	AWRW_I2C_ST_NONE = 0,
	AWRW_I2C_ST_READ,
	AWRW_I2C_ST_WRITE,
};

enum {
	AWRW_HDR_WR_FLAG = 0,
	AWRW_HDR_ADDR_BYTES,
	AWRW_HDR_DATA_BYTES,
	AWRW_HDR_REG_NUM,
	AWRW_HDR_REG_ADDR,
	AWRW_HDR_MAX,
};

struct aw_i2c_packet {
	char status;
	unsigned int reg_num;
	unsigned int reg_addr;
	char *reg_data;
};


/********************************************
 *
 * aw87xxx device struct
 *
 *******************************************/
struct aw87xxx {
	char fw_name[AW87XXX_FW_NAME_MAX];
	int32_t dev_index;
	char *current_profile;
	char prof_off_name[AW_PROFILE_STR_MAX];
	uint32_t off_bin_status;
	struct device *dev;
	bool is_suspend;

	struct mutex reg_lock;
	struct aw_device aw_dev;
	struct aw_i2c_packet i2c_packet;

	struct delayed_work fw_load_work;
	struct acf_bin_info acf_info;

	aw_snd_soc_codec_t *codec;

	struct list_head list;

	struct aw_monitor monitor;
#ifdef AW_ALGO_AUTH_DSP
	struct delayed_work auth_work;
#endif
};

int aw87xxx_update_profile(struct aw87xxx *aw87xxx, char *profile);
int aw87xxx_update_profile_esd(struct aw87xxx *aw87xxx, char *profile);

#endif

// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2014 MediaTek Inc.
 * Author: Xudong Chen <xudong.chen@mediatek.com>
 */

#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/scatterlist.h>
#include <linux/sched.h>
#include <linux/slab.h>

#define I2C_CONFERR			(1 << 9)
#define I2C_RS_TRANSFER			(1 << 4)
#define I2C_ARB_LOST			(1 << 3)
#define I2C_HS_NACKERR			(1 << 2)
#define I2C_ACKERR			(1 << 1)
#define I2C_TRANSAC_COMP		(1 << 0)
#define I2C_RESUME_ARBIT		(1 << 1)
#define I2C_TRANSAC_START		(1 << 0)
#define I2C_RS_MUL_CNFG			(1 << 15)
#define I2C_RS_MUL_TRIG			(1 << 14)
#define I2C_HS_HOLD_SEL			(1 << 15)
#define I2C_I3C_EN			(1 << 15)
#define I2C_HFIFO_UNLOCK		(1 << 15)
#define I2C_HFIFO_NINTH_BIT		(2 << 8)
#define I2C_HFIFO_MASTER_CODE		0x0008
#define I2C_DCM_DISABLE			0x0000
#define I2C_IO_CONFIG_OPEN_DRAIN	0x0003
#define I2C_IO_CONFIG_PUSH_PULL		0x0000
#define I2C_SOFT_RST			0x0001
#define I2C_FSM_RST			0x0004
#define I2C_HANDSHAKE_RST		0x0020
#define I2C_FIFO_ADDR_CLR		0x0001
#define I2C_HFIFO_ADDR_CLR		0x0002
#define I2C_FIFO_ADDR_CLR_MCH		0x0004
#define I2C_DELAY_LEN			0x0002
#define I2C_TIME_CLR_VALUE		0x0000
#define I2C_TIME_DEFAULT_VALUE		0x0003
#define I2C_TIME_HS_SPEED_VALUE		0x0080
#define I2C_WRRD_TRANAC_VALUE		0x0002
#define I2C_RD_TRANAC_VALUE		0x0001
#define I2C_SCL_MIS_COMP_VALUE		0x0000
#define I2C_CHN_CLR_FLAG		0x0000
#define I2C_DEBUGCTRL_BUS		0x0004

#define I2C_DMA_CON_TX			0x0000
#define I2C_DMA_CON_RX			0x0001
#define I2C_DMA_ASYNC_MODE		0x0004
#define I2C_DMA_SKIP_CONFIG		0x0010
#define I2C_DMA_DIR_CHANGE		0x0200
#define I2C_DMA_START_EN		0x0001
#define I2C_DMA_INT_FLAG_NONE		0x0000
#define I2C_DMA_CLR_FLAG		0x0000
#define I2C_DMA_WARM_RST		0x0001
#define I2C_DMA_HARD_RST		0x0002
#define I2C_DMA_HANDSHAKE_RST		0x0004
#define I2C_DMA_CON_DEFAULT_VALUE	0x0008

#define MAX_SAMPLE_CNT_DIV		8
#define MAX_STEP_CNT_DIV		64
#define MAX_CLOCK_DIV_8BITS		256
#define MAX_CLOCK_DIV_5BITS		32
#define MAX_HS_STEP_CNT_DIV		8
#define I2C_STANDARD_MODE_BUFFER	(1000 / 3)
#define I2C_FAST_MODE_BUFFER		(300 / 3)
#define I2C_FAST_MODE_PLUS_BUFFER	(20 / 3)
#define I2C_HS_MODE_BUFFER		(10 / 3)

#define I2C_CONTROL_RS                  (0x1 << 1)
#define I2C_CONTROL_DMA_EN              (0x1 << 2)
#define I2C_CONTROL_CLK_EXT_EN          (0x1 << 3)
#define I2C_CONTROL_DIR_CHANGE          (0x1 << 4)
#define I2C_CONTROL_ACKERR_DET_EN       (0x1 << 5)
#define I2C_CONTROL_TRANSFER_LEN_CHANGE (0x1 << 6)
#define I2C_CONTROL_DMAACK_EN           (0x1 << 8)
#define I2C_CONTROL_ASYNC_MODE          (0x1 << 9)
#define I2C_CONTROL_WRAPPER             (0x1 << 0)

#define I2C_SPEED_ADJUST_FACTOR		85
#define I2C_SPEED_ADJUST_THRESH		2200000
#define I2C_CLK_ADJUST_THRESH		100000000

#define I2C_OFFSET_SCP			0x200
#define I2C_CCU_INTR_EN         0x2
#define I2C_MCU_INTR_EN         0x1
#define I2C_FIFO_DATA_LEN_MASK	0x001f
#define MAX_POLLING_CNT		10

#define I2C_DRV_NAME		"i2c-mt65xx"

/* mt6873 use DMA_HW_VERSION1 */
enum {
	DMA_HW_VERSION0 = 0,
	DMA_HW_VERSION1 = 1,
};

enum DMA_REGS_OFFSET {
	OFFSET_INT_FLAG = 0x0,
	OFFSET_INT_EN = 0x04,
	OFFSET_EN = 0x08,
	OFFSET_RST = 0x0c,
	OFFSET_CON = 0x18,
	OFFSET_TX_MEM_ADDR = 0x1c,
	OFFSET_RX_MEM_ADDR = 0x20,
	OFFSET_TX_LEN = 0x24,
	OFFSET_RX_LEN = 0x28,
	OFFSET_DEBUG_STA = 0x50,
	OFFSET_TX_4G_MODE = 0x54,
	OFFSET_RX_4G_MODE = 0x58,
};

enum i2c_trans_st_rs {
	I2C_TRANS_STOP = 0,
	I2C_TRANS_REPEATED_START,
};

enum mtk_trans_op {
	I2C_MASTER_WR = 1,
	I2C_MASTER_RD,
	I2C_MASTER_WRRD,
	I2C_MASTER_CONTINUOUS_WR,
};

enum I2C_REGS_OFFSET {
	OFFSET_DATA_PORT,
	OFFSET_SLAVE_ADDR,
	OFFSET_SLAVE_ADDR1,
	OFFSET_INTR_MASK,
	OFFSET_INTR_STAT,
	OFFSET_CONTROL,
	OFFSET_TRANSFER_LEN,
	OFFSET_TRANSAC_LEN,
	OFFSET_DELAY_LEN,
	OFFSET_TIMING,
	OFFSET_START,
	OFFSET_EXT_CONF,
	OFFSET_FIFO_STAT,
	OFFSET_FIFO_THRESH,
	OFFSET_FIFO_ADDR_CLR,
	OFFSET_IO_CONFIG,
	OFFSET_RSV_DEBUG,
	OFFSET_HS,
	OFFSET_SOFTRESET,
	OFFSET_DCM_EN,
	OFFSET_PATH_DIR,
	OFFSET_DEBUGSTAT,
	OFFSET_DEBUGCTRL,
	OFFSET_TRANSFER_LEN_AUX,
	OFFSET_CLOCK_DIV,
	OFFSET_LTIMING,
	OFFSET_SCL_HIGH_LOW_RATIO,
	OFFSET_HS_SCL_HIGH_LOW_RATIO,
	OFFSET_SCL_MIS_COMP_POINT,
	OFFSET_STA_STO_AC_TIMING,
	OFFSET_HS_STA_STO_AC_TIMING,
	OFFSET_SDA_TIMING,
	OFFSET_DMA_FSM_DEBUG,
	OFFSET_MCU_INTR,
	OFFSET_MULTI_DMA,
	OFFSET_HFIFO_DATA,
};

static const u16 mt_i2c_regs_v1[] = {
	[OFFSET_DATA_PORT] = 0x0,
	[OFFSET_SLAVE_ADDR] = 0x4,
	[OFFSET_INTR_MASK] = 0x8,
	[OFFSET_INTR_STAT] = 0xc,
	[OFFSET_CONTROL] = 0x10,
	[OFFSET_TRANSFER_LEN] = 0x14,
	[OFFSET_TRANSAC_LEN] = 0x18,
	[OFFSET_DELAY_LEN] = 0x1c,
	[OFFSET_TIMING] = 0x20,
	[OFFSET_START] = 0x24,
	[OFFSET_EXT_CONF] = 0x28,
	[OFFSET_FIFO_STAT] = 0x30,
	[OFFSET_FIFO_THRESH] = 0x34,
	[OFFSET_FIFO_ADDR_CLR] = 0x38,
	[OFFSET_IO_CONFIG] = 0x40,
	[OFFSET_RSV_DEBUG] = 0x44,
	[OFFSET_HS] = 0x48,
	[OFFSET_SOFTRESET] = 0x50,
	[OFFSET_DCM_EN] = 0x54,
	[OFFSET_PATH_DIR] = 0x60,
	[OFFSET_DEBUGSTAT] = 0x64,
	[OFFSET_DEBUGCTRL] = 0x68,
	[OFFSET_TRANSFER_LEN_AUX] = 0x6c,
	[OFFSET_CLOCK_DIV] = 0x70,
	[OFFSET_SCL_HIGH_LOW_RATIO] = 0x74,
	[OFFSET_HS_SCL_HIGH_LOW_RATIO] = 0x78,
	[OFFSET_SCL_MIS_COMP_POINT] = 0x7C,
	[OFFSET_STA_STO_AC_TIMING] = 0x80,
	[OFFSET_HS_STA_STO_AC_TIMING] = 0x84,
	[OFFSET_SDA_TIMING] = 0x88,
	[OFFSET_DMA_FSM_DEBUG] = 0xec,
};

static const u16 mt_i2c_regs_v2[] = {
	[OFFSET_DATA_PORT] = 0x0,
	[OFFSET_SLAVE_ADDR] = 0x4,
	[OFFSET_SLAVE_ADDR1] = 0x94,
	[OFFSET_INTR_MASK] = 0x8,
	[OFFSET_INTR_STAT] = 0xc,
	[OFFSET_CONTROL] = 0x10,
	[OFFSET_TRANSFER_LEN] = 0x14,
	[OFFSET_TRANSAC_LEN] = 0x18,
	[OFFSET_DELAY_LEN] = 0x1c,
	[OFFSET_TIMING] = 0x20,
	[OFFSET_START] = 0x24,
	[OFFSET_EXT_CONF] = 0x28,
	[OFFSET_LTIMING] = 0x2c,
	[OFFSET_HS] = 0x30,
	[OFFSET_IO_CONFIG] = 0x34,
	[OFFSET_FIFO_ADDR_CLR] = 0x38,
	[OFFSET_SDA_TIMING] = 0x3c,
	[OFFSET_MCU_INTR] = 0x40,
	[OFFSET_TRANSFER_LEN_AUX] = 0x44,
	[OFFSET_CLOCK_DIV] = 0x48,
	[OFFSET_SOFTRESET] = 0x50,
	[OFFSET_HFIFO_DATA] = 0x70,
	[OFFSET_SCL_MIS_COMP_POINT] = 0x90,
	[OFFSET_DEBUGSTAT] = 0xe4,
	[OFFSET_DEBUGCTRL] = 0xe8,
	[OFFSET_DMA_FSM_DEBUG] = 0xec,
	[OFFSET_FIFO_STAT] = 0xf4,
	[OFFSET_FIFO_THRESH] = 0xf8,
	[OFFSET_DCM_EN] = 0xf88,
	[OFFSET_MULTI_DMA] = 0xf8c,
};

struct mtk_i2c_compatible {
	const struct i2c_adapter_quirks *quirks;
	const u16 *regs;
	unsigned char pmic_i2c: 1;
	unsigned char dcm: 1;
	unsigned char auto_restart: 1;
	unsigned char aux_len_reg: 1;
	unsigned char timing_adjust: 1;
	unsigned char dma_sync: 1;
	unsigned char ltiming_adjust: 1;
	unsigned char dma_ver;
	unsigned char apdma_sync: 1;
	unsigned char max_dma_support;
	unsigned char slave_addr_ver;
	unsigned char fifo_size;
};

struct mtk_i2c_ac_timing {
	u16 htiming;
	u16 ltiming;
	u16 hs;
	u16 ext;
	u16 inter_clk_div;
	u16 scl_hl_ratio;
	u16 hs_scl_hl_ratio;
	u16 sta_stop;
	u16 hs_sta_stop;
	u16 sda_timing;
};

struct mtk_i2c {
	struct i2c_adapter adap;	/* i2c host adapter */
	struct device *dev;
	struct completion msg_complete;

	/* set in i2c probe */
	void __iomem *base;		/* i2c base addr */
	void __iomem *pdmabase;		/* dma base address*/
	struct clk *clk_main;		/* main clock for i2c bus */
	struct clk *clk_dma;		/* DMA clock for i2c via DMA */
	struct clk *clk_pmic;		/* PMIC clock for i2c from PMIC */
	struct clk *clk_arb;		/* Arbitrator clock for i2c */
	bool have_pmic;			/* can use i2c pins from PMIC */
	bool use_push_pull;		/* IO config push-pull mode */

	u16 irq_stat;			/* interrupt status */
	unsigned int clk_src_div;
	unsigned int speed_hz;		/* The speed in transfer */
	unsigned int clk_src_in_hz;
	unsigned int ch_offset_i2c;
	unsigned int ch_offset_dma;
	enum mtk_trans_op op;
	u16 timing_reg;
	u16 high_speed_reg;
	u16 ltiming_reg;
	unsigned char auto_restart;
	int  scl_gpio_id;
	int  sda_gpio_id;
	bool ignore_restart_irq;
	bool master_code_sended;
	struct mtk_i2c_ac_timing ac_timing;
	const struct mtk_i2c_compatible *dev_comp;
};

/**
 * struct i2c_spec_values:
 * @min_low_ns: min LOW period of the SCL clock
 * @min_su_sta_ns: min set-up time for a repeated START condition
 * @max_hd_dat_ns: max data hold time
 * @min_su_dat_ns: min data set-up time
 */
struct i2c_spec_values {
	unsigned int min_low_ns;
	unsigned int min_su_sta_ns;
	unsigned int max_hd_dat_ns;
	unsigned int min_su_dat_ns;
};

static const struct i2c_spec_values standard_mode_spec = {
	.min_low_ns = 4700 + I2C_STANDARD_MODE_BUFFER,
	.min_su_sta_ns = 4700 + I2C_STANDARD_MODE_BUFFER,
	.max_hd_dat_ns = 3450 - I2C_STANDARD_MODE_BUFFER,
	.min_su_dat_ns = 250 + I2C_STANDARD_MODE_BUFFER,
};

static const struct i2c_spec_values fast_mode_spec = {
	.min_low_ns = 1300 + I2C_FAST_MODE_BUFFER,
	.min_su_sta_ns = 600 + I2C_FAST_MODE_BUFFER,
	.max_hd_dat_ns = 900 - I2C_FAST_MODE_BUFFER,
	.min_su_dat_ns = 100 + I2C_FAST_MODE_BUFFER,
};

static const struct i2c_spec_values fast_mode_plus_spec = {
	.min_low_ns = 500 + I2C_FAST_MODE_PLUS_BUFFER,
	.min_su_sta_ns = 260 + I2C_FAST_MODE_PLUS_BUFFER,
	.max_hd_dat_ns = 400 - I2C_FAST_MODE_PLUS_BUFFER,
	.min_su_dat_ns = 50 + I2C_FAST_MODE_PLUS_BUFFER,
};

static const struct i2c_spec_values hs_mode_spec = {
	.min_low_ns = 160 + I2C_HS_MODE_BUFFER,
	.min_su_sta_ns = 160 + I2C_HS_MODE_BUFFER,
	.max_hd_dat_ns = 70 - I2C_HS_MODE_BUFFER,
	.min_su_dat_ns = 10 + I2C_HS_MODE_BUFFER,
};

static const struct i2c_adapter_quirks mt6577_i2c_quirks = {
	.flags = I2C_AQ_COMB_WRITE_THEN_READ,
	.max_num_msgs = 1,
	.max_write_len = 255,
	.max_read_len = 255,
	.max_comb_1st_msg_len = 255,
	.max_comb_2nd_msg_len = 31,
};

static const struct i2c_adapter_quirks mt7622_i2c_quirks = {
	.max_num_msgs = 255,
};

static const struct i2c_adapter_quirks mt8183_i2c_quirks = {
	.flags = I2C_AQ_NO_ZERO_LEN,
};

static const struct mtk_i2c_compatible mt2712_compat = {
	.regs = mt_i2c_regs_v1,
	.pmic_i2c = 0,
	.dcm = 1,
	.auto_restart = 1,
	.aux_len_reg = 1,
	.timing_adjust = 1,
	.dma_sync = 0,
	.ltiming_adjust = 0,
	.apdma_sync = 0,
	.max_dma_support = 33,
};

static const struct mtk_i2c_compatible mt6577_compat = {
	.quirks = &mt6577_i2c_quirks,
	.regs = mt_i2c_regs_v1,
	.pmic_i2c = 0,
	.dcm = 1,
	.auto_restart = 0,
	.aux_len_reg = 0,
	.timing_adjust = 0,
	.dma_sync = 0,
	.ltiming_adjust = 0,
	.apdma_sync = 0,
	.max_dma_support = 32,
};

static const struct mtk_i2c_compatible mt6589_compat = {
	.quirks = &mt6577_i2c_quirks,
	.regs = mt_i2c_regs_v1,
	.pmic_i2c = 1,
	.dcm = 0,
	.auto_restart = 0,
	.aux_len_reg = 0,
	.timing_adjust = 0,
	.dma_sync = 0,
	.ltiming_adjust = 0,
	.apdma_sync = 0,
	.max_dma_support = 32,
};

static const struct mtk_i2c_compatible mt7622_compat = {
	.quirks = &mt7622_i2c_quirks,
	.regs = mt_i2c_regs_v1,
	.pmic_i2c = 0,
	.dcm = 1,
	.auto_restart = 1,
	.aux_len_reg = 1,
	.timing_adjust = 0,
	.dma_sync = 0,
	.ltiming_adjust = 0,
	.apdma_sync = 0,
	.max_dma_support = 32,
};

static const struct mtk_i2c_compatible mt8173_compat = {
	.regs = mt_i2c_regs_v1,
	.pmic_i2c = 0,
	.dcm = 1,
	.auto_restart = 1,
	.aux_len_reg = 1,
	.timing_adjust = 0,
	.dma_sync = 0,
	.ltiming_adjust = 0,
	.apdma_sync = 0,
	.max_dma_support = 33,
};

static const struct mtk_i2c_compatible mt8183_compat = {
	.quirks = &mt8183_i2c_quirks,
	.regs = mt_i2c_regs_v2,
	.pmic_i2c = 0,
	.dcm = 0,
	.auto_restart = 1,
	.aux_len_reg = 1,
	.timing_adjust = 1,
	.dma_sync = 1,
	.ltiming_adjust = 1,
	.apdma_sync = 0,
	.max_dma_support = 33,
};

static const struct mtk_i2c_compatible mt8192_compat = {
	.quirks = &mt8183_i2c_quirks,
	.regs = mt_i2c_regs_v2,
	.pmic_i2c = 0,
	.dcm = 0,
	.auto_restart = 1,
	.aux_len_reg = 1,
	.timing_adjust = 1,
	.dma_sync = 1,
	.ltiming_adjust = 1,
	.apdma_sync = 1,
	.max_dma_support = 36,
};

static const struct mtk_i2c_compatible mt6873_compat = {
	.regs = mt_i2c_regs_v2,
	.pmic_i2c = 0,
	.dcm = 0,
	.auto_restart = 1,
	.aux_len_reg = 1,
	.timing_adjust = 1,
	.dma_sync = 1,
	.ltiming_adjust = 1,
	.dma_ver = 1,
	.apdma_sync = 1,
	.max_dma_support = 36,
	.fifo_size = 8,
};

static const struct mtk_i2c_compatible mt6879_compat = {
	.regs = mt_i2c_regs_v2,
	.pmic_i2c = 0,
	.dcm = 0,
	.auto_restart = 1,
	.aux_len_reg = 1,
	.timing_adjust = 1,
	.dma_sync = 1,
	.ltiming_adjust = 1,
	.dma_ver = 0,
	.apdma_sync = 1,
	.max_dma_support = 36,
	.slave_addr_ver = 1,
	.fifo_size = 16,
};

static const struct mtk_i2c_compatible mt6983_compat = {
	.regs = mt_i2c_regs_v2,
	.pmic_i2c = 0,
	.dcm = 0,
	.auto_restart = 1,
	.aux_len_reg = 1,
	.timing_adjust = 1,
	.dma_sync = 1,
	.ltiming_adjust = 1,
	.dma_ver = 1,
	.apdma_sync = 1,
	.max_dma_support = 36,
	.slave_addr_ver = 1,
	.fifo_size = 16,
};

static const struct of_device_id mtk_i2c_of_match[] = {
	{ .compatible = "mediatek,mt2712-i2c", .data = &mt2712_compat },
	{ .compatible = "mediatek,mt6577-i2c", .data = &mt6577_compat },
	{ .compatible = "mediatek,mt6589-i2c", .data = &mt6589_compat },
	{ .compatible = "mediatek,mt6873-i2c", .data = &mt6873_compat },
	{ .compatible = "mediatek,mt6879-i2c", .data = &mt6879_compat },
	{ .compatible = "mediatek,mt6983-i2c", .data = &mt6983_compat },
	{ .compatible = "mediatek,mt7622-i2c", .data = &mt7622_compat },
	{ .compatible = "mediatek,mt8173-i2c", .data = &mt8173_compat },
	{ .compatible = "mediatek,mt8183-i2c", .data = &mt8183_compat },
	{ .compatible = "mediatek,mt8192-i2c", .data = &mt8192_compat },
	{}
};
MODULE_DEVICE_TABLE(of, mtk_i2c_of_match);

static u8 mtk_i2c_readb(struct mtk_i2c *i2c, enum I2C_REGS_OFFSET reg)
{
	return readb(i2c->base + i2c->ch_offset_i2c + i2c->dev_comp->regs[reg]);
}

static void mtk_i2c_writeb(struct mtk_i2c *i2c, u8 val,
			   enum I2C_REGS_OFFSET reg)
{
	writeb(val, i2c->base + i2c->ch_offset_i2c + i2c->dev_comp->regs[reg]);
}

static u16 mtk_i2c_readw(struct mtk_i2c *i2c, enum I2C_REGS_OFFSET reg)
{
	return readw(i2c->base + i2c->ch_offset_i2c + i2c->dev_comp->regs[reg]);
}

static void mtk_i2c_writew(struct mtk_i2c *i2c, u16 val,
			   enum I2C_REGS_OFFSET reg)
{
	writew(val, i2c->base + i2c->ch_offset_i2c + i2c->dev_comp->regs[reg]);
}

static u16 mtk_i2c_readw_shadow(struct mtk_i2c *i2c, enum I2C_REGS_OFFSET reg)
{
	return readw(i2c->base + i2c->dev_comp->regs[reg]);
}

static void mtk_i2c_writew_shadow(struct mtk_i2c *i2c, u16 val,
			   enum I2C_REGS_OFFSET reg)
{
	writew(val, i2c->base + i2c->dev_comp->regs[reg]);
}

static int mtk_i2c_clock_enable(struct mtk_i2c *i2c)
{
	int ret;

	ret = clk_prepare_enable(i2c->clk_dma);
	if (ret)
		return ret;

	ret = clk_prepare_enable(i2c->clk_main);
	if (ret)
		goto err_main;

	if (i2c->have_pmic) {
		ret = clk_prepare_enable(i2c->clk_pmic);
		if (ret)
			goto err_pmic;
	}

	if (i2c->clk_arb) {
		ret = clk_prepare_enable(i2c->clk_arb);
		if (ret)
			goto err_arb;
	}

	return 0;

err_arb:
	if (i2c->have_pmic)
		clk_disable_unprepare(i2c->clk_pmic);
err_pmic:
	clk_disable_unprepare(i2c->clk_main);
err_main:
	clk_disable_unprepare(i2c->clk_dma);

	return ret;
}

static void mtk_i2c_clock_disable(struct mtk_i2c *i2c)
{
	if (i2c->clk_arb)
		clk_disable_unprepare(i2c->clk_arb);

	if (i2c->have_pmic)
		clk_disable_unprepare(i2c->clk_pmic);

	clk_disable_unprepare(i2c->clk_main);
	clk_disable_unprepare(i2c->clk_dma);
}

static void mtk_i2c_init_hw(struct mtk_i2c *i2c)
{
	u16 control_reg;
	u16 intr_stat_reg;
	u16 debugctrl_reg;

	mtk_i2c_writew(i2c, I2C_CHN_CLR_FLAG, OFFSET_START);
	intr_stat_reg = mtk_i2c_readw(i2c, OFFSET_INTR_STAT);
	mtk_i2c_writew(i2c, intr_stat_reg, OFFSET_INTR_STAT);

	if (i2c->dev_comp->apdma_sync) {
		writel(I2C_DMA_WARM_RST, i2c->pdmabase + OFFSET_RST);
		udelay(10);
		writel(I2C_DMA_CLR_FLAG, i2c->pdmabase + OFFSET_RST);
		udelay(10);
		writel(I2C_DMA_HANDSHAKE_RST | I2C_DMA_HARD_RST,
		       i2c->pdmabase + OFFSET_RST);
		mtk_i2c_writew(i2c, I2C_HANDSHAKE_RST | I2C_SOFT_RST,
			       OFFSET_SOFTRESET);
		udelay(10);
		writel(I2C_DMA_CLR_FLAG, i2c->pdmabase + OFFSET_RST);
		mtk_i2c_writew(i2c, I2C_CHN_CLR_FLAG, OFFSET_SOFTRESET);
	} else {
		writel(I2C_DMA_HARD_RST, i2c->pdmabase + OFFSET_RST);
		udelay(50);
		writel(I2C_DMA_CLR_FLAG, i2c->pdmabase + OFFSET_RST);
		mtk_i2c_writew(i2c, I2C_SOFT_RST, OFFSET_SOFTRESET);
	}
	/* config scp i2c ch2 intr to ap */
	if (i2c->ch_offset_i2c == I2C_OFFSET_SCP)
		mtk_i2c_writew(i2c, I2C_CCU_INTR_EN, OFFSET_MCU_INTR);
	if ((i2c->ch_offset_i2c != I2C_OFFSET_SCP) &&
		(mtk_i2c_readw_shadow(i2c, OFFSET_DMA_FSM_DEBUG) & I2C_I3C_EN)) {
		debugctrl_reg = mtk_i2c_readw_shadow(i2c, OFFSET_DEBUGCTRL);
		mtk_i2c_writew_shadow(i2c, debugctrl_reg & (~I2C_DEBUGCTRL_BUS), OFFSET_DEBUGCTRL);
	}
	/* Set ioconfig */
	if (i2c->use_push_pull)
		mtk_i2c_writew(i2c, I2C_IO_CONFIG_PUSH_PULL, OFFSET_IO_CONFIG);
	else
		mtk_i2c_writew(i2c, I2C_IO_CONFIG_OPEN_DRAIN, OFFSET_IO_CONFIG);

	if (i2c->dev_comp->dcm)
		mtk_i2c_writew(i2c, I2C_DCM_DISABLE, OFFSET_DCM_EN);

	mtk_i2c_writew(i2c, i2c->timing_reg, OFFSET_TIMING);
	mtk_i2c_writew(i2c, i2c->high_speed_reg, OFFSET_HS);
	if (i2c->dev_comp->ltiming_adjust)
		mtk_i2c_writew(i2c, i2c->ltiming_reg, OFFSET_LTIMING);

	if (i2c->dev_comp->timing_adjust) {
		mtk_i2c_writew(i2c, i2c->ac_timing.ext, OFFSET_EXT_CONF);
		mtk_i2c_writew(i2c, i2c->ac_timing.inter_clk_div,
			       OFFSET_CLOCK_DIV);
		mtk_i2c_writew(i2c, I2C_SCL_MIS_COMP_VALUE,
			       OFFSET_SCL_MIS_COMP_POINT);
		mtk_i2c_writew(i2c, i2c->ac_timing.sda_timing,
			       OFFSET_SDA_TIMING);

		if (i2c->dev_comp->ltiming_adjust) {
			mtk_i2c_writew(i2c, i2c->ac_timing.htiming,
				       OFFSET_TIMING);
			mtk_i2c_writew(i2c, i2c->ac_timing.hs, OFFSET_HS);
			mtk_i2c_writew(i2c, i2c->ac_timing.ltiming,
				       OFFSET_LTIMING);
		} else {
			mtk_i2c_writew(i2c, i2c->ac_timing.scl_hl_ratio,
				       OFFSET_SCL_HIGH_LOW_RATIO);
			mtk_i2c_writew(i2c, i2c->ac_timing.hs_scl_hl_ratio,
				       OFFSET_HS_SCL_HIGH_LOW_RATIO);
			mtk_i2c_writew(i2c, i2c->ac_timing.sta_stop,
				       OFFSET_STA_STO_AC_TIMING);
			mtk_i2c_writew(i2c, i2c->ac_timing.hs_sta_stop,
				       OFFSET_HS_STA_STO_AC_TIMING);
		}
	}

	/* If use i2c pin from PMIC mt6397 side, need set PATH_DIR first */
	if (i2c->have_pmic)
		mtk_i2c_writew(i2c, I2C_CONTROL_WRAPPER, OFFSET_PATH_DIR);

	control_reg = I2C_CONTROL_ACKERR_DET_EN |
		      I2C_CONTROL_CLK_EXT_EN | I2C_CONTROL_DMA_EN;
	if (i2c->dev_comp->dma_sync)
		control_reg |= I2C_CONTROL_DMAACK_EN | I2C_CONTROL_ASYNC_MODE;

	mtk_i2c_writew(i2c, control_reg, OFFSET_CONTROL);
	mtk_i2c_writew(i2c, I2C_DELAY_LEN, OFFSET_DELAY_LEN);
}

static const struct i2c_spec_values *mtk_i2c_get_spec(unsigned int speed)
{
	if (speed <= I2C_MAX_STANDARD_MODE_FREQ)
		return &standard_mode_spec;
	else if (speed <= I2C_MAX_FAST_MODE_FREQ)
		return &fast_mode_spec;
	else if (speed <= I2C_MAX_FAST_MODE_PLUS_FREQ)
		return &fast_mode_plus_spec;
	else
		return &hs_mode_spec;
}

static int mtk_i2c_max_step_cnt(unsigned int target_speed)
{
	if (target_speed > I2C_MAX_FAST_MODE_PLUS_FREQ)
		return MAX_HS_STEP_CNT_DIV;
	else
		return MAX_STEP_CNT_DIV;
}

static int mtk_i2c_get_clk_div_restri(struct mtk_i2c *i2c,
				      unsigned int sample_cnt)
{
	int clk_div_restri = 0;

	if (i2c->dev_comp->ltiming_adjust == 0)
		return 0;

	if (sample_cnt == 1) {
		if (i2c->ac_timing.inter_clk_div == 0)
			clk_div_restri = 0;
		else
			clk_div_restri = 1;
	} else {
		if (i2c->ac_timing.inter_clk_div == 0)
			clk_div_restri = -1;
		else if (i2c->ac_timing.inter_clk_div == 1)
			clk_div_restri = 0;
		else
			clk_div_restri = 1;
	}
	return clk_div_restri;
}

/*
 * Check and Calculate i2c ac-timing
 *
 * Hardware design:
 * sample_ns = (1000000000 * (sample_cnt + 1)) / clk_src
 * xxx_cnt_div =  spec->min_xxx_ns / sample_ns
 *
 * Sample_ns is rounded down for xxx_cnt_div would be greater
 * than the smallest spec.
 * The sda_timing is chosen as the middle value between
 * the largest and smallest.
 */
static int mtk_i2c_check_ac_timing(struct mtk_i2c *i2c,
				   unsigned int clk_src,
				   unsigned int check_speed,
				   unsigned int step_cnt,
				   unsigned int sample_cnt)
{
	const struct i2c_spec_values *spec;
	unsigned int su_sta_cnt, low_cnt, high_cnt, max_step_cnt;
	unsigned int sda_max, sda_min, clk_ns, max_sta_cnt = 0x3f;
	unsigned int sample_ns = div_u64(1000000000ULL * (sample_cnt + 1),
					 clk_src);

	if (!i2c->dev_comp->timing_adjust)
		return 0;

	if (i2c->dev_comp->ltiming_adjust)
		max_sta_cnt = 0x100;
	if (check_speed > I2C_MAX_FAST_MODE_PLUS_FREQ)
		max_sta_cnt = 0x80;

	spec = mtk_i2c_get_spec(check_speed);

	if (i2c->dev_comp->ltiming_adjust)
		clk_ns = 1000000000 / clk_src;
	else
		clk_ns = sample_ns / 2;

	su_sta_cnt = DIV_ROUND_UP(spec->min_su_sta_ns, clk_ns);
	if (su_sta_cnt > max_sta_cnt)
		return -1;

	low_cnt = DIV_ROUND_UP(spec->min_low_ns, sample_ns);
	max_step_cnt = mtk_i2c_max_step_cnt(check_speed);
	if (check_speed > I2C_MAX_FAST_MODE_PLUS_FREQ) {
		if ((step_cnt > 1) && (step_cnt < max_step_cnt - 1)) {
			high_cnt = step_cnt - 1;
			low_cnt = step_cnt + 1;
		} else {
			return -2;
		}
	} else {
		if ((2 * step_cnt) > low_cnt && low_cnt < max_step_cnt) {
			if (low_cnt > step_cnt) {
				high_cnt = 2 * step_cnt - low_cnt;
			} else {
				high_cnt = step_cnt;
				low_cnt = step_cnt;
			}
		} else {
			return -2;
		}
	}

	sda_max = spec->max_hd_dat_ns / sample_ns;
	if (sda_max > low_cnt)
		sda_max = 0;

	sda_min = DIV_ROUND_UP(spec->min_su_dat_ns, sample_ns);
	if (sda_min < low_cnt)
		sda_min = 0;

	if (sda_min > sda_max)
		return -3;

	if (check_speed > I2C_MAX_FAST_MODE_PLUS_FREQ) {
		if (i2c->dev_comp->ltiming_adjust) {
			i2c->ac_timing.hs = I2C_TIME_DEFAULT_VALUE |
				I2C_TIME_HS_SPEED_VALUE | (sample_cnt << 12) |
				(high_cnt << 8);
			i2c->ac_timing.ltiming &= ~GENMASK(15, 9);
			i2c->ac_timing.ltiming |= I2C_HS_HOLD_SEL | (sample_cnt << 12) |
				(low_cnt << 9);
			i2c->ac_timing.ext &= ~GENMASK(7, 1);
			i2c->ac_timing.ext |= (su_sta_cnt << 1) | (1 << 0);
		} else {
			i2c->ac_timing.hs_scl_hl_ratio = (1 << 12) |
				(high_cnt << 6) | low_cnt;
			i2c->ac_timing.hs_sta_stop = (su_sta_cnt << 8) |
				su_sta_cnt;
		}
		i2c->ac_timing.sda_timing = 0;
	} else {
		if (i2c->dev_comp->ltiming_adjust) {
			i2c->ac_timing.hs = 0;
			i2c->ac_timing.htiming = (sample_cnt << 8) | (high_cnt);
			i2c->ac_timing.ltiming = (sample_cnt << 6) | (low_cnt);
			i2c->ac_timing.ext = (su_sta_cnt << 8) | (1 << 0);
		} else {
			i2c->ac_timing.scl_hl_ratio = (1 << 12) |
				(high_cnt << 6) | low_cnt;
			i2c->ac_timing.sta_stop = (su_sta_cnt << 8) |
				su_sta_cnt;
		}

		i2c->ac_timing.sda_timing = (1 << 12) |
			(sda_max + sda_min) / 2;
	}

	return 0;
}

/*
 * Calculate i2c port speed
 *
 * Hardware design:
 * i2c_bus_freq = parent_clk / (clock_div * 2 * sample_cnt * step_cnt)
 * clock_div: fixed in hardware, but may be various in different SoCs
 *
 * The calculation want to pick the highest bus frequency that is still
 * less than or equal to i2c->speed_hz. The calculation try to get
 * sample_cnt and step_cn
 */
static int mtk_i2c_calculate_speed(struct mtk_i2c *i2c, unsigned int clk_src,
				   unsigned int target_speed,
				   unsigned int *timing_step_cnt,
				   unsigned int *timing_sample_cnt)
{
	unsigned int step_cnt;
	unsigned int sample_cnt;
	unsigned int max_step_cnt;
	unsigned int base_sample_cnt = MAX_SAMPLE_CNT_DIV;
	unsigned int base_step_cnt;
	unsigned int opt_div;
	unsigned int best_mul;
	unsigned int cnt_mul;
	unsigned int vir_clk_src = clk_src;
	int clk_div_restri = 0;
	int ret = -EINVAL;

	if (target_speed > I2C_MAX_HIGH_SPEED_MODE_FREQ)
		target_speed = I2C_MAX_HIGH_SPEED_MODE_FREQ;

	max_step_cnt = mtk_i2c_max_step_cnt(target_speed);
	base_step_cnt = max_step_cnt;
	if ((target_speed > I2C_SPEED_ADJUST_THRESH) && (clk_src > I2C_CLK_ADJUST_THRESH))
		vir_clk_src = (clk_src / 100 * I2C_SPEED_ADJUST_FACTOR);
	/* Find the best combination */
	opt_div = DIV_ROUND_UP(vir_clk_src >> 1, target_speed);
	best_mul = MAX_SAMPLE_CNT_DIV * max_step_cnt;

	/* Search for the best pair (sample_cnt, step_cnt) with
	 * 0 < sample_cnt < MAX_SAMPLE_CNT_DIV
	 * 0 < step_cnt < max_step_cnt
	 * sample_cnt * step_cnt >= opt_div
	 * optimizing for sample_cnt * step_cnt being minimal
	 */
	for (sample_cnt = 1; sample_cnt <= MAX_SAMPLE_CNT_DIV; sample_cnt++) {
		clk_div_restri = mtk_i2c_get_clk_div_restri(i2c, sample_cnt);
		step_cnt = DIV_ROUND_UP(opt_div + clk_div_restri, sample_cnt);
		cnt_mul = step_cnt * sample_cnt;
		if (step_cnt > max_step_cnt)
			continue;

		if (cnt_mul < best_mul) {
			ret = mtk_i2c_check_ac_timing(i2c, vir_clk_src,
				target_speed, step_cnt - 1, sample_cnt - 1);
			if (ret)
				continue;

			best_mul = cnt_mul;
			base_sample_cnt = sample_cnt;
			base_step_cnt = step_cnt;
			if (best_mul == (opt_div + clk_div_restri))
				break;
		}
	}

	if (ret)
		return -EINVAL;

	sample_cnt = base_sample_cnt;
	step_cnt = base_step_cnt;

	if ((vir_clk_src / (2 * (sample_cnt * step_cnt - clk_div_restri))) > target_speed) {
		/* In this case, hardware can't support such
		 * low i2c_bus_freq
		 */
		dev_dbg(i2c->dev, "Unsupported speed (%uhz)\n",	target_speed);
		return -EINVAL;
	}

	*timing_step_cnt = step_cnt - 1;
	*timing_sample_cnt = sample_cnt - 1;

	return 0;
}

static int mtk_i2c_set_speed(struct mtk_i2c *i2c, unsigned int parent_clk)
{
	unsigned int clk_src;
	unsigned int step_cnt;
	unsigned int sample_cnt;
	unsigned int l_step_cnt;
	unsigned int l_sample_cnt;
	unsigned int target_speed;
	unsigned int clk_div;
	unsigned int max_clk_div;
	int ret;

	target_speed = i2c->speed_hz;
	parent_clk /= i2c->clk_src_div;

	if (i2c->dev_comp->timing_adjust && i2c->dev_comp->ltiming_adjust)
		max_clk_div = MAX_CLOCK_DIV_5BITS;
	else if (i2c->dev_comp->timing_adjust)
		max_clk_div = MAX_CLOCK_DIV_8BITS;
	else
		max_clk_div = 1;

	for (clk_div = 1; clk_div <= max_clk_div; clk_div++) {
		clk_src = parent_clk / clk_div;
		i2c->ac_timing.inter_clk_div = clk_div - 1;

		if (target_speed > I2C_MAX_FAST_MODE_PLUS_FREQ) {
			/* Set master code speed register */
			ret = mtk_i2c_calculate_speed(i2c, clk_src,
						      I2C_MAX_FAST_MODE_FREQ - 1000,
						      &l_step_cnt,
						      &l_sample_cnt);
			if (ret < 0)
				continue;

			i2c->timing_reg = (l_sample_cnt << 8) | l_step_cnt;

			/* Set the high speed mode register */
			ret = mtk_i2c_calculate_speed(i2c, clk_src,
						      target_speed, &step_cnt,
						      &sample_cnt);
			if (ret < 0)
				continue;

			i2c->high_speed_reg = I2C_TIME_DEFAULT_VALUE |
					(sample_cnt << 12) | (step_cnt << 8);

			if (i2c->dev_comp->ltiming_adjust)
				i2c->ltiming_reg =
					(l_sample_cnt << 6) | l_step_cnt |
					(sample_cnt << 12) | (step_cnt << 9);
		} else {
			ret = mtk_i2c_calculate_speed(i2c, clk_src,
						      target_speed, &l_step_cnt,
						      &l_sample_cnt);
			if (ret < 0)
				continue;

			i2c->timing_reg = (l_sample_cnt << 8) | l_step_cnt;

			/* Disable the high speed transaction */
			i2c->high_speed_reg = I2C_TIME_CLR_VALUE;

			if (i2c->dev_comp->ltiming_adjust)
				i2c->ltiming_reg =
					(l_sample_cnt << 6) | l_step_cnt;
		}

		break;
	}

	i2c->ac_timing.inter_clk_div = ((clk_div - 1) << 8) | (clk_div - 1);

	return 0;
}

extern void gpio_dump_regs_range(int start, int end);
static void mtk_i2c_gpio_dump(struct mtk_i2c *i2c)
{
	int start;
	int end;

	start = i2c->scl_gpio_id < i2c->sda_gpio_id ? i2c->scl_gpio_id : i2c->sda_gpio_id;
	end = i2c->scl_gpio_id < i2c->sda_gpio_id ? i2c->sda_gpio_id : i2c->scl_gpio_id;
	gpio_dump_regs_range(start, end);
}

static void mtk_i2c_dump_reg(struct mtk_i2c *i2c)
{
	u16 slave_addr_value = 0;

	if (i2c->dev_comp->slave_addr_ver == 1)
		slave_addr_value = mtk_i2c_readw(i2c, OFFSET_SLAVE_ADDR1);
	else
		slave_addr_value = mtk_i2c_readw(i2c, OFFSET_SLAVE_ADDR);
	dev_info(i2c->dev, "I2C (speed %d) register:\n"
		"SLAVE_ADDR=0x%x,INTR_STAT=0x%x,CONTROL=0x%x,\n"
		"TRANSFER_LEN=0x%x,TRANSAC_LEN=0x%x,START=0x%x,\n"
		"FIFO_STAT=0x%x,DEBUGSTAT=0x%x,DMA_FSM_DEBUG=0x%x,\n"
		"INTR_MASK=0x%x,EXT_CONF=0x%x,IO_CONFIG=0x%x,\n"
		"CLOCK_DIV=0x%x,FIFO_THRESH=0x%x,DEBUGCTRL=0x%x,\n"
		"HS=0x%x,TIMING=0x%x\n",
		i2c->speed_hz,
		(slave_addr_value),
		(mtk_i2c_readw(i2c, OFFSET_INTR_STAT)),
		(mtk_i2c_readw(i2c, OFFSET_CONTROL)),
		(mtk_i2c_readw(i2c, OFFSET_TRANSFER_LEN)),
		(mtk_i2c_readw(i2c, OFFSET_TRANSAC_LEN)),
		(mtk_i2c_readw(i2c, OFFSET_START)),
		(mtk_i2c_readw(i2c, OFFSET_FIFO_STAT)),
		(mtk_i2c_readw(i2c, OFFSET_DEBUGSTAT)),
		(mtk_i2c_readw(i2c, OFFSET_DMA_FSM_DEBUG)),
		(mtk_i2c_readw(i2c, OFFSET_INTR_MASK)),
		(mtk_i2c_readw(i2c, OFFSET_EXT_CONF)),
		(mtk_i2c_readw(i2c, OFFSET_IO_CONFIG)),
		(mtk_i2c_readw(i2c, OFFSET_CLOCK_DIV)),
		(mtk_i2c_readw(i2c, OFFSET_FIFO_THRESH)),
		(mtk_i2c_readw(i2c, OFFSET_DEBUGCTRL)),
		(mtk_i2c_readw(i2c, OFFSET_HS)),
		(mtk_i2c_readw(i2c, OFFSET_TIMING)));
	if (i2c->dev_comp->ltiming_adjust)
		dev_info(i2c->dev, "I2C register: LTIMING=0x%x\n",
			mtk_i2c_readw(i2c, OFFSET_LTIMING));
	if (i2c->dev_comp->aux_len_reg)
		dev_info(i2c->dev, "I2C register: TRANSFER_LEN_AUX=0x%x\n",
			mtk_i2c_readw(i2c, OFFSET_TRANSFER_LEN_AUX));

	dev_info(i2c->dev, "DMA register:\n"
		"INT_FLAG=0x%x,INT_EN=0x%x,EN=0x%x,\n"
		"CON=0x%x,TX_MEM_ADDR=0x%x,RX_MEM_ADDR=0x%x,\n"
		"TX_LEN=0x%x,RX_LEN=0x%x,DEBUG_STA=0x%x\n",
		(readl(i2c->pdmabase + OFFSET_INT_FLAG)),
		(readl(i2c->pdmabase + OFFSET_INT_EN)),
		(readl(i2c->pdmabase + OFFSET_EN)),
		(readl(i2c->pdmabase + OFFSET_CON)),
		(readl(i2c->pdmabase + OFFSET_TX_MEM_ADDR)),
		(readl(i2c->pdmabase + OFFSET_RX_MEM_ADDR)),
		(readl(i2c->pdmabase + OFFSET_TX_LEN)),
		(readl(i2c->pdmabase + OFFSET_RX_LEN)),
		(readl(i2c->pdmabase + OFFSET_DEBUG_STA)));
	if (i2c->dev_comp->max_dma_support > 32)
		dev_info(i2c->dev, "DMA register:\n"
			"TX_4G_MODE=0x%x,RX_4G_MODE=0x%x\n",
			(readl(i2c->pdmabase + OFFSET_TX_4G_MODE)),
			(readl(i2c->pdmabase + OFFSET_RX_4G_MODE)));
}

static int mtk_i2c_do_transfer(struct mtk_i2c *i2c, struct i2c_msg *msgs,
			       int num, int left_num)
{
	u16 addr_reg;
	u16 start_reg;
	u16 control_reg;
	u16 restart_flag = 0;
	u16 dma_sync = 0;
	u16 data_size = 0;
	u16 fifo_data_len = 0;
	u32 reg_4g_mode;
	u32 reg_dma_reset;
	u8 *ptr = NULL;
	u8 *dma_rd_buf = NULL;
	u8 *dma_wr_buf = NULL;
	dma_addr_t rpaddr = 0;
	dma_addr_t wpaddr = 0;
	bool isDMA = false;
	int i = 0;
	int ret;

	i2c->irq_stat = 0;

	if (i2c->auto_restart)
		restart_flag = I2C_RS_TRANSFER;

	reinit_completion(&i2c->msg_complete);
	if ((i2c->ch_offset_i2c) && (left_num > 0)) {
		dev_info(i2c->dev, "i2c_multi-ch not_support,ch_offset:%d,num:%d,left_num:%d\n",
					i2c->ch_offset_i2c, num, left_num);
		if (i2c->op == I2C_MASTER_CONTINUOUS_WR)
			kfree(msgs->buf);
		return -EPERM;
	}
	if (i2c->dev_comp->apdma_sync &&
	    i2c->op == I2C_MASTER_RD && num > 1) {
		writel(I2C_DMA_HANDSHAKE_RST | I2C_DMA_WARM_RST,
		       i2c->pdmabase + OFFSET_RST);

		ret = readw_poll_timeout(i2c->pdmabase + OFFSET_RST,
					 reg_dma_reset,
					 !(reg_dma_reset & I2C_DMA_WARM_RST),
					 0, 100);
		if (ret) {
			dev_info(i2c->dev, "DMA warm reset timeout\n");
			return -ETIMEDOUT;
		}

		writel(I2C_DMA_CLR_FLAG, i2c->pdmabase + OFFSET_RST);
		mtk_i2c_writew(i2c, I2C_HANDSHAKE_RST, OFFSET_SOFTRESET);
		mtk_i2c_writew(i2c, I2C_CHN_CLR_FLAG, OFFSET_SOFTRESET);
	}

	if ((msgs->len > i2c->dev_comp->fifo_size) || ((i2c->op == I2C_MASTER_WRRD) &&
		((msgs + 1)->len > i2c->dev_comp->fifo_size))) {
		if (i2c->ch_offset_i2c == I2C_OFFSET_SCP) {
			dev_dbg(i2c->dev, "Not_support_dma! msgs->len:%d,fifo_size:%d\n",
					msgs->len, i2c->dev_comp->fifo_size);
			if (i2c->op == I2C_MASTER_CONTINUOUS_WR)
				kfree(msgs->buf);
			return -EPERM;
		}
		isDMA = true;
	} else {
		isDMA = false;
	}
	/* Make sure the clock is ready before read register */
	mb();
	if (mtk_i2c_readw(i2c, OFFSET_TIMING) == 0 ||
		readl(i2c->pdmabase + OFFSET_CON) == I2C_DMA_CON_DEFAULT_VALUE)
		mtk_i2c_init_hw(i2c);
	control_reg = mtk_i2c_readw(i2c, OFFSET_CONTROL) &
			~(I2C_CONTROL_DIR_CHANGE | I2C_CONTROL_RS | I2C_CONTROL_DMA_EN |
			I2C_CONTROL_DMAACK_EN | I2C_CONTROL_ASYNC_MODE);
	if ((i2c->speed_hz > I2C_MAX_FAST_MODE_PLUS_FREQ) || (left_num >= 1))
		control_reg |= I2C_CONTROL_RS;

	if (i2c->op == I2C_MASTER_WRRD)
		control_reg |= I2C_CONTROL_DIR_CHANGE | I2C_CONTROL_RS;

	if (isDMA == true) {
		control_reg |= I2C_CONTROL_DMA_EN;
		if (i2c->dev_comp->dma_sync)
			control_reg |= I2C_CONTROL_DMAACK_EN | I2C_CONTROL_ASYNC_MODE;
	}

	mtk_i2c_writew(i2c, control_reg, OFFSET_CONTROL);

	addr_reg = i2c_8bit_addr_from_msg(msgs);
	if (i2c->dev_comp->slave_addr_ver == 1)
		mtk_i2c_writew(i2c, addr_reg, OFFSET_SLAVE_ADDR1);
	else
		mtk_i2c_writew(i2c, addr_reg, OFFSET_SLAVE_ADDR);

	/* Clear interrupt status */
	mtk_i2c_writew(i2c, restart_flag | I2C_CONFERR | I2C_HS_NACKERR | I2C_ACKERR |
			    I2C_ARB_LOST | I2C_TRANSAC_COMP, OFFSET_INTR_STAT);

	if (i2c->ch_offset_i2c)
		mtk_i2c_writew(i2c, I2C_FIFO_ADDR_CLR_MCH, OFFSET_FIFO_ADDR_CLR);
	if ((i2c->speed_hz > I2C_MAX_FAST_MODE_PLUS_FREQ) &&
		(mtk_i2c_readw(i2c, OFFSET_DMA_FSM_DEBUG) & I2C_I3C_EN)) {
		mtk_i2c_writew(i2c, I2C_HFIFO_ADDR_CLR | I2C_FIFO_ADDR_CLR,
					OFFSET_FIFO_ADDR_CLR);
		if (!i2c->master_code_sended) {
			i2c->master_code_sended = true;
			mtk_i2c_writew(i2c, I2C_HFIFO_UNLOCK | I2C_HFIFO_NINTH_BIT |
						I2C_HFIFO_MASTER_CODE, OFFSET_HFIFO_DATA);
		}
	} else {
		mtk_i2c_writew(i2c, I2C_FIFO_ADDR_CLR, OFFSET_FIFO_ADDR_CLR);
	}

	/* Enable interrupt */
	mtk_i2c_writew(i2c, restart_flag | I2C_HS_NACKERR | I2C_ACKERR |
			    I2C_ARB_LOST | I2C_TRANSAC_COMP, OFFSET_INTR_MASK);

	/* Set transfer and transaction len */
	if (i2c->op == I2C_MASTER_WRRD) {
		if (i2c->dev_comp->aux_len_reg) {
			mtk_i2c_writew(i2c, msgs->len, OFFSET_TRANSFER_LEN);
			mtk_i2c_writew(i2c, (msgs + 1)->len,
					    OFFSET_TRANSFER_LEN_AUX);
		} else {
			mtk_i2c_writew(i2c, msgs->len | ((msgs + 1)->len) << 8,
					    OFFSET_TRANSFER_LEN);
		}
		mtk_i2c_writew(i2c, I2C_WRRD_TRANAC_VALUE, OFFSET_TRANSAC_LEN);
	} else if (i2c->op == I2C_MASTER_CONTINUOUS_WR) {
		mtk_i2c_writew(i2c, msgs->len / num, OFFSET_TRANSFER_LEN);
		mtk_i2c_writew(i2c, num, OFFSET_TRANSAC_LEN);
	} else {
		mtk_i2c_writew(i2c, msgs->len, OFFSET_TRANSFER_LEN);
		mtk_i2c_writew(i2c, num, OFFSET_TRANSAC_LEN);
	}

	if (i2c->dev_comp->apdma_sync) {
		dma_sync = I2C_DMA_SKIP_CONFIG | I2C_DMA_ASYNC_MODE;
		if (i2c->op == I2C_MASTER_WRRD)
			dma_sync |= I2C_DMA_DIR_CHANGE;
	}

	if (isDMA == true) {
		/* Prepare buffer data to start transfer */
		if (i2c->op == I2C_MASTER_RD) {
			writel(I2C_DMA_INT_FLAG_NONE, i2c->pdmabase + OFFSET_INT_FLAG);

			if (i2c->dev_comp->dma_ver == DMA_HW_VERSION1)
				writel(I2C_DMA_CON_RX | I2C_DMA_SKIP_CONFIG |
					I2C_DMA_ASYNC_MODE,
						i2c->pdmabase + OFFSET_CON);
			else
				writel(I2C_DMA_CON_RX, i2c->pdmabase + OFFSET_CON);

			dma_rd_buf = i2c_get_dma_safe_msg_buf(msgs, 1);
			if (!dma_rd_buf)
				return -ENOMEM;

			rpaddr = dma_map_single(i2c->dev, dma_rd_buf,
						msgs->len, DMA_FROM_DEVICE);
			if (dma_mapping_error(i2c->dev, rpaddr)) {
				i2c_put_dma_safe_msg_buf(dma_rd_buf, msgs, false);

				return -ENOMEM;
			}

			if (i2c->dev_comp->max_dma_support > 32) {
				reg_4g_mode = upper_32_bits(rpaddr);
				writel(reg_4g_mode, i2c->pdmabase + OFFSET_RX_4G_MODE);
			}

			writel((u32)rpaddr, i2c->pdmabase + OFFSET_RX_MEM_ADDR);
			writel(msgs->len, i2c->pdmabase + OFFSET_RX_LEN);
		} else if (i2c->op == I2C_MASTER_WR) {
			writel(I2C_DMA_INT_FLAG_NONE, i2c->pdmabase + OFFSET_INT_FLAG);
			if (i2c->dev_comp->dma_ver == DMA_HW_VERSION1)
				writel(I2C_DMA_CON_TX | I2C_DMA_SKIP_CONFIG |
						I2C_DMA_ASYNC_MODE,
							i2c->pdmabase + OFFSET_CON);
			else
				writel(I2C_DMA_CON_TX, i2c->pdmabase + OFFSET_CON);

			dma_wr_buf = i2c_get_dma_safe_msg_buf(msgs, 1);
			if (!dma_wr_buf)
				return -ENOMEM;

			wpaddr = dma_map_single(i2c->dev, dma_wr_buf,
						msgs->len, DMA_TO_DEVICE);
			if (dma_mapping_error(i2c->dev, wpaddr)) {
				i2c_put_dma_safe_msg_buf(dma_wr_buf, msgs, false);

				return -ENOMEM;
			}

			if (i2c->dev_comp->max_dma_support > 32) {
				reg_4g_mode = upper_32_bits(wpaddr);
				writel(reg_4g_mode, i2c->pdmabase + OFFSET_TX_4G_MODE);
			}

			writel((u32)wpaddr, i2c->pdmabase + OFFSET_TX_MEM_ADDR);
			writel(msgs->len, i2c->pdmabase + OFFSET_TX_LEN);
		} else if (i2c->op == I2C_MASTER_CONTINUOUS_WR) {
			writel(I2C_DMA_INT_FLAG_NONE, i2c->pdmabase + OFFSET_INT_FLAG);
			if (i2c->dev_comp->dma_ver == DMA_HW_VERSION1)
				writel(I2C_DMA_CON_TX | I2C_DMA_SKIP_CONFIG |
					I2C_DMA_ASYNC_MODE,
						i2c->pdmabase + OFFSET_CON);
			else
				writel(I2C_DMA_CON_TX, i2c->pdmabase + OFFSET_CON);

			wpaddr = dma_map_single(i2c->dev, msgs->buf,
						msgs->len, DMA_TO_DEVICE);
			if (dma_mapping_error(i2c->dev, wpaddr)) {
				kfree(msgs->buf);
				return -ENOMEM;
			}

			if (i2c->dev_comp->max_dma_support > 32) {
				reg_4g_mode = upper_32_bits(wpaddr);
				writel(reg_4g_mode, i2c->pdmabase + OFFSET_TX_4G_MODE);
			}

			writel((u32)wpaddr, i2c->pdmabase + OFFSET_TX_MEM_ADDR);
			writel(msgs->len, i2c->pdmabase + OFFSET_TX_LEN);
		} else if (i2c->op == I2C_MASTER_WRRD) {
			writel(I2C_DMA_CLR_FLAG, i2c->pdmabase + OFFSET_INT_FLAG);

			if (i2c->dev_comp->dma_ver == DMA_HW_VERSION1)
				writel(0x0000 | I2C_DMA_SKIP_CONFIG |
					I2C_DMA_ASYNC_MODE | I2C_DMA_DIR_CHANGE,
					i2c->pdmabase + OFFSET_CON);
			else
				writel(I2C_DMA_CLR_FLAG, i2c->pdmabase + OFFSET_CON);

			dma_wr_buf = i2c_get_dma_safe_msg_buf(msgs, 1);
			if (!dma_wr_buf)
				return -ENOMEM;

			wpaddr = dma_map_single(i2c->dev, dma_wr_buf,
						msgs->len, DMA_TO_DEVICE);
			if (dma_mapping_error(i2c->dev, wpaddr)) {
				i2c_put_dma_safe_msg_buf(dma_wr_buf, msgs, false);

				return -ENOMEM;
			}

			dma_rd_buf = i2c_get_dma_safe_msg_buf((msgs + 1), 1);
			if (!dma_rd_buf) {
				dma_unmap_single(i2c->dev, wpaddr,
						msgs->len, DMA_TO_DEVICE);

				i2c_put_dma_safe_msg_buf(dma_wr_buf, msgs, false);

				return -ENOMEM;
			}

			rpaddr = dma_map_single(i2c->dev, dma_rd_buf,
						(msgs + 1)->len,
						DMA_FROM_DEVICE);
			if (dma_mapping_error(i2c->dev, rpaddr)) {
				dma_unmap_single(i2c->dev, wpaddr,
						msgs->len, DMA_TO_DEVICE);

				i2c_put_dma_safe_msg_buf(dma_wr_buf, msgs, false);
				i2c_put_dma_safe_msg_buf(dma_rd_buf, (msgs + 1), false);

				return -ENOMEM;
			}

			if (i2c->dev_comp->max_dma_support > 32) {
				reg_4g_mode = upper_32_bits(wpaddr);
				writel(reg_4g_mode, i2c->pdmabase + OFFSET_TX_4G_MODE);

				reg_4g_mode = upper_32_bits(rpaddr);
				writel(reg_4g_mode, i2c->pdmabase + OFFSET_RX_4G_MODE);
			}

			writel((u32)wpaddr, i2c->pdmabase + OFFSET_TX_MEM_ADDR);
			writel((u32)rpaddr, i2c->pdmabase + OFFSET_RX_MEM_ADDR);
			writel(msgs->len, i2c->pdmabase + OFFSET_TX_LEN);
			writel((msgs + 1)->len, i2c->pdmabase + OFFSET_RX_LEN);
		}

		/* flush before sending DMA start */
		mb();
		writel(I2C_DMA_START_EN, i2c->pdmabase + OFFSET_EN);
	} else if (i2c->op != I2C_MASTER_RD) {
		data_size = msgs->len;
		ptr = msgs->buf;
		while (data_size--) {
			mtk_i2c_writeb(i2c, *ptr, OFFSET_DATA_PORT);
			ptr++;
		}
	}

	if (!i2c->auto_restart) {
		start_reg = I2C_TRANSAC_START;
	} else {
		start_reg = I2C_TRANSAC_START | I2C_RS_MUL_TRIG;
		if (left_num >= 1)
			start_reg |= I2C_RS_MUL_CNFG;
	}
	/* flush before sending i2c start */
	mb();
	if ((i2c->ch_offset_i2c == I2C_OFFSET_SCP) &&
		(isDMA == false) && (i2c->op != I2C_MASTER_RD)) {
		do {
			fifo_data_len = mtk_i2c_readw(i2c, OFFSET_FIFO_STAT)
				& I2C_FIFO_DATA_LEN_MASK;
			if (msgs->len == fifo_data_len)
				break;
			i++;
		} while (i < MAX_POLLING_CNT);
		if (i == MAX_POLLING_CNT)
			dev_info(i2c->dev, "I2C fifo status: 0x%x maybe not ready!\n",
				fifo_data_len);
	}
	mtk_i2c_writew(i2c, start_reg, OFFSET_START);

	ret = wait_for_completion_timeout(&i2c->msg_complete,
					  i2c->adap.timeout);

	/* Clear interrupt mask */
	mtk_i2c_writew(i2c, ~(restart_flag | I2C_CONFERR | I2C_HS_NACKERR | I2C_ACKERR |
			    I2C_ARB_LOST | I2C_TRANSAC_COMP), OFFSET_INTR_MASK);

	if (isDMA == true) {
		if (i2c->op == I2C_MASTER_WR) {
			dma_unmap_single(i2c->dev, wpaddr,
					 msgs->len, DMA_TO_DEVICE);

			i2c_put_dma_safe_msg_buf(dma_wr_buf, msgs, true);
		} else if (i2c->op == I2C_MASTER_RD) {
			dma_unmap_single(i2c->dev, rpaddr,
					 msgs->len, DMA_FROM_DEVICE);

			i2c_put_dma_safe_msg_buf(dma_rd_buf, msgs, true);
		} else if (i2c->op == I2C_MASTER_CONTINUOUS_WR) {
			dma_unmap_single(i2c->dev, wpaddr,
					 msgs->len, DMA_TO_DEVICE);
		} else if (i2c->op == I2C_MASTER_WRRD) {
			dma_unmap_single(i2c->dev, wpaddr, msgs->len,
					 DMA_TO_DEVICE);
			dma_unmap_single(i2c->dev, rpaddr, (msgs + 1)->len,
					 DMA_FROM_DEVICE);

			i2c_put_dma_safe_msg_buf(dma_wr_buf, msgs, true);
			i2c_put_dma_safe_msg_buf(dma_rd_buf, (msgs + 1), true);
		}
	}

	if (i2c->op == I2C_MASTER_CONTINUOUS_WR) {
		kfree(msgs->buf);
	}

	if (ret == 0) {
		u16 start_reg = mtk_i2c_readw(i2c, OFFSET_START);
		mtk_i2c_writew(i2c, 0, OFFSET_INTR_MASK);
		if (i2c->ch_offset_i2c != 0)
			mtk_i2c_writew_shadow(i2c, I2C_FSM_RST | I2C_SOFT_RST, OFFSET_SOFTRESET);
		dev_dbg(i2c->dev, "addr: %x, transfer timeout\n", msgs->addr);
		mtk_i2c_dump_reg(i2c);
		mtk_i2c_gpio_dump(i2c);
		if (i2c->ch_offset_i2c) {
			mtk_i2c_writew(i2c, I2C_FIFO_ADDR_CLR_MCH | I2C_FIFO_ADDR_CLR,
						OFFSET_FIFO_ADDR_CLR);
		} else {
			mtk_i2c_writew(i2c, I2C_FIFO_ADDR_CLR, OFFSET_FIFO_ADDR_CLR);
		}
		mtk_i2c_init_hw(i2c);
		if ((i2c->ch_offset_i2c) && (start_reg & I2C_RESUME_ARBIT)) {
			mtk_i2c_writew_shadow(i2c, I2C_RESUME_ARBIT, OFFSET_START);
			dev_dbg(i2c->dev, "bus channel transferred\n");
		}

		return -ETIMEDOUT;
	}

	if (i2c->irq_stat & (I2C_HS_NACKERR | I2C_ACKERR)) {
		dev_dbg(i2c->dev, "addr: %x, transfer ACK error\n", msgs->addr);
		mtk_i2c_init_hw(i2c);
		if (i2c->ch_offset_i2c) {
			mtk_i2c_writew_shadow(i2c, I2C_RESUME_ARBIT, OFFSET_START);
			dev_dbg(i2c->dev, "bus channel transferred\n");
		}
		return -ENXIO;
	}
	if ((i2c->op != I2C_MASTER_WR) &&
			(i2c->op != I2C_MASTER_CONTINUOUS_WR) && (isDMA == false)) {
		if (i2c->op == I2C_MASTER_WRRD) {
			data_size = (msgs + 1)->len;
			ptr = (msgs + 1)->buf;
		} else {
			data_size = msgs->len;
			ptr = msgs->buf;
		}
		while (data_size--) {
			*ptr = mtk_i2c_readb(i2c, OFFSET_DATA_PORT);
			ptr++;
		}
	}
	return 0;
}

static int mtk_i2c_transfer(struct i2c_adapter *adap,
			    struct i2c_msg msgs[], int num)
{
	int ret;
	int left_num = num;
	int i, j;
	u8 *dma_multi_wr_buf;
	struct i2c_msg multi_msg[1];
	struct mtk_i2c *i2c = i2c_get_adapdata(adap);

	ret = mtk_i2c_clock_enable(i2c);
	if (ret)
		return ret;

	i2c->master_code_sended = false;
	i2c->auto_restart = i2c->dev_comp->auto_restart;

	/* checking if we can skip restart and optimize using WRRD mode */
	if (i2c->auto_restart && num == 2) {
		if (!(msgs[0].flags & I2C_M_RD) && (msgs[1].flags & I2C_M_RD) &&
		    msgs[0].addr == msgs[1].addr) {
			i2c->auto_restart = 0;
		}
	}

	if (num > 1) {
		for (i = 0; i < num - 1; i++) {
			if (!(msgs[i].flags & I2C_M_RD) && !(msgs[i+1].flags &
				I2C_M_RD) && (msgs[i].addr == msgs[i+1].addr)
					&& (msgs[i].len == msgs[i+1].len)) {
				continue;
			} else
				break;
		}
		if (i >= num - 1) {
			i2c->op = I2C_MASTER_CONTINUOUS_WR;
			j = 0;
			dma_multi_wr_buf = kzalloc(msgs->len * num, GFP_KERNEL);
			if (!dma_multi_wr_buf) {
				ret =  -ENOMEM;
				goto err_exit;
			}
			multi_msg->addr  = msgs->addr;
			multi_msg->len   = msgs->len * num;
			multi_msg->buf   = dma_multi_wr_buf;
			multi_msg->flags  = 0;
			while (j < num) {
				memcpy(dma_multi_wr_buf + msgs->len * j,
							msgs->buf, msgs->len);
				j++;
				msgs++;
				}

			i2c->ignore_restart_irq = false;
			ret = mtk_i2c_do_transfer(i2c, multi_msg, num, 0);
			if (ret < 0)
				goto err_exit;
			ret = num;
				goto err_exit;

		}
	}

	if (i2c->auto_restart && num >= 2 &&
		i2c->speed_hz > I2C_MAX_FAST_MODE_PLUS_FREQ)
		/* ignore the first restart irq after the master code,
		 * otherwise the first transfer will be discarded.
		 */
		i2c->ignore_restart_irq = true;
	else
		i2c->ignore_restart_irq = false;

	while (left_num--) {
		if (!msgs->buf) {
			dev_dbg(i2c->dev, "data buffer is NULL.\n");
			ret = -EINVAL;
			goto err_exit;
		}

		if (msgs->flags & I2C_M_RD)
			i2c->op = I2C_MASTER_RD;
		else
			i2c->op = I2C_MASTER_WR;

		if (!i2c->auto_restart) {
			if (num > 1) {
				/* combined two messages into one transaction */
				i2c->op = I2C_MASTER_WRRD;
				left_num--;
			}
		}

		/* always use DMA mode. */
		ret = mtk_i2c_do_transfer(i2c, msgs, num, left_num);
		if (ret < 0)
			goto err_exit;

		msgs++;
	}
	/* the return value is number of executed messages */
	ret = num;

err_exit:
	mtk_i2c_clock_disable(i2c);
	return ret;
}

static irqreturn_t mtk_i2c_irq(int irqno, void *dev_id)
{
	struct mtk_i2c *i2c = dev_id;
	u16 restart_flag = 0;
	u16 intr_stat;

	if (i2c->auto_restart)
		restart_flag = I2C_RS_TRANSFER;

	intr_stat = mtk_i2c_readw(i2c, OFFSET_INTR_STAT);
	mtk_i2c_writew(i2c, intr_stat, OFFSET_INTR_STAT);

	/*
	 * when occurs ack error, i2c controller generate two interrupts
	 * first is the ack error interrupt, then the complete interrupt
	 * i2c->irq_stat need keep the two interrupt value.
	 */
	i2c->irq_stat |= intr_stat;

	if (i2c->ignore_restart_irq && (i2c->irq_stat & restart_flag)) {
		i2c->ignore_restart_irq = false;
		i2c->irq_stat = 0;
		mtk_i2c_writew(i2c, I2C_RS_MUL_CNFG | I2C_RS_MUL_TRIG |
				    I2C_TRANSAC_START, OFFSET_START);
	} else {
		if (i2c->irq_stat & (I2C_TRANSAC_COMP | restart_flag))
			complete(&i2c->msg_complete);
	}

	return IRQ_HANDLED;
}

static u32 mtk_i2c_functionality(struct i2c_adapter *adap)
{
	if (i2c_check_quirks(adap, I2C_AQ_NO_ZERO_LEN))
		return I2C_FUNC_I2C |
			(I2C_FUNC_SMBUS_EMUL & ~I2C_FUNC_SMBUS_QUICK);
	else
		return I2C_FUNC_I2C | I2C_FUNC_SMBUS_EMUL;
}

static const struct i2c_algorithm mtk_i2c_algorithm = {
	.master_xfer = mtk_i2c_transfer,
	.functionality = mtk_i2c_functionality,
};

static int mtk_i2c_parse_dt(struct device_node *np, struct mtk_i2c *i2c)
{
	int ret;

	ret = of_property_read_u32(np, "clock-frequency", &i2c->speed_hz);
	if (ret < 0)
		i2c->speed_hz = I2C_MAX_STANDARD_MODE_FREQ;

	ret = of_property_read_u32(np, "clock-div", &i2c->clk_src_div);
	if (ret < 0)
		return ret;

	if (i2c->clk_src_div == 0)
		return -EINVAL;

	of_property_read_u32(np, "clk_src_in_hz", &i2c->clk_src_in_hz);
	of_property_read_u32(np, "ch_offset_i2c", &i2c->ch_offset_i2c);
	of_property_read_u32(np, "ch_offset_dma", &i2c->ch_offset_dma);
	dev_dbg(i2c->dev, "clk_src=%d,ch_offset_i2c=0x%x, ch_offset_dma=0x%x\n",
			i2c->clk_src_in_hz, i2c->ch_offset_i2c, i2c->ch_offset_dma);
	i2c->have_pmic = of_property_read_bool(np, "mediatek,have-pmic");
	i2c->use_push_pull =
		of_property_read_bool(np, "mediatek,use-push-pull");
	of_property_read_u32(np, "scl-gpio-id", &i2c->scl_gpio_id);
	of_property_read_u32(np, "sda-gpio-id", &i2c->sda_gpio_id);

	return 0;
}

static int mtk_i2c_probe(struct platform_device *pdev)
{
	int ret = 0;
	struct mtk_i2c *i2c;
	struct clk *clk;
	struct resource *res;
	int irq;

	i2c = devm_kzalloc(&pdev->dev, sizeof(*i2c), GFP_KERNEL);
	if (!i2c)
		return -ENOMEM;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	i2c->base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(i2c->base))
		return PTR_ERR(i2c->base);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	i2c->pdmabase = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(i2c->pdmabase))
		return PTR_ERR(i2c->pdmabase);

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;

	init_completion(&i2c->msg_complete);

	i2c->dev_comp = of_device_get_match_data(&pdev->dev);
	i2c->adap.dev.of_node = pdev->dev.of_node;
	i2c->dev = &pdev->dev;
	i2c->adap.dev.parent = &pdev->dev;
	i2c->adap.owner = THIS_MODULE;
	i2c->adap.algo = &mtk_i2c_algorithm;
	i2c->adap.quirks = i2c->dev_comp->quirks;
	i2c->adap.timeout = 2 * HZ;
	i2c->adap.retries = 1;
	i2c->adap.bus_regulator = devm_regulator_get_optional(&pdev->dev, "vbus");
	if (IS_ERR(i2c->adap.bus_regulator)) {
		if (PTR_ERR(i2c->adap.bus_regulator) == -ENODEV)
			i2c->adap.bus_regulator = NULL;
		else
			return PTR_ERR(i2c->adap.bus_regulator);
	}

	ret = mtk_i2c_parse_dt(pdev->dev.of_node, i2c);
	if (ret)
		return -EINVAL;

	if (i2c->have_pmic && !i2c->dev_comp->pmic_i2c)
		return -EINVAL;

	if (i2c->ch_offset_dma)
		i2c->pdmabase += i2c->ch_offset_dma;

	i2c->clk_main = devm_clk_get(&pdev->dev, "main");
	if (IS_ERR(i2c->clk_main)) {
		dev_err(&pdev->dev, "cannot get main clock\n");
		return PTR_ERR(i2c->clk_main);
	}

	i2c->clk_dma = devm_clk_get(&pdev->dev, "dma");
	if (IS_ERR(i2c->clk_dma)) {
		dev_err(&pdev->dev, "cannot get dma clock\n");
		return PTR_ERR(i2c->clk_dma);
	}

	i2c->clk_arb = devm_clk_get(&pdev->dev, "arb");
	if (IS_ERR(i2c->clk_arb))
		i2c->clk_arb = NULL;

	clk = i2c->clk_main;
	if (i2c->have_pmic) {
		i2c->clk_pmic = devm_clk_get(&pdev->dev, "pmic");
		if (IS_ERR(i2c->clk_pmic)) {
			dev_err(&pdev->dev, "cannot get pmic clock\n");
			return PTR_ERR(i2c->clk_pmic);
		}
		clk = i2c->clk_pmic;
	}

	strlcpy(i2c->adap.name, I2C_DRV_NAME, sizeof(i2c->adap.name));

	if (i2c->ch_offset_i2c == I2C_OFFSET_SCP) {
		if (i2c->clk_src_in_hz)
			ret = mtk_i2c_set_speed(i2c, i2c->clk_src_in_hz);
		else
			ret = -EINVAL;
	} else {
		ret = mtk_i2c_set_speed(i2c, clk_get_rate(clk));
	}
	if (ret) {
		dev_err(&pdev->dev, "Failed to set the speed.\n");
		return -EINVAL;
	}

	if (i2c->dev_comp->max_dma_support > 32) {
		ret = dma_set_mask(&pdev->dev,
				DMA_BIT_MASK(i2c->dev_comp->max_dma_support));
		if (ret) {
			dev_err(&pdev->dev, "dma_set_mask return error.\n");
			return ret;
		}
	}

	if (i2c->dev_comp->max_dma_support > 32) {
		ret = dma_set_mask(&pdev->dev,
				DMA_BIT_MASK(i2c->dev_comp->max_dma_support));
		if (ret) {
			dev_err(&pdev->dev, "dma_set_mask return error.\n");
			return ret;
		}
	}

	ret = mtk_i2c_clock_enable(i2c);
	if (ret) {
		dev_err(&pdev->dev, "clock enable failed!\n");
		return ret;
	}
	mtk_i2c_init_hw(i2c);
	mtk_i2c_clock_disable(i2c);

	ret = devm_request_irq(&pdev->dev, irq, mtk_i2c_irq,
			       IRQF_NO_SUSPEND | IRQF_TRIGGER_NONE,
			       I2C_DRV_NAME, i2c);
	if (ret < 0) {
		dev_err(&pdev->dev,
			"Request I2C IRQ %d fail\n", irq);
		return ret;
	}

	i2c_set_adapdata(&i2c->adap, i2c);
	ret = i2c_add_adapter(&i2c->adap);
	if (ret)
		return ret;

	platform_set_drvdata(pdev, i2c);

	return 0;
}

static int mtk_i2c_remove(struct platform_device *pdev)
{
	struct mtk_i2c *i2c = platform_get_drvdata(pdev);

	i2c_del_adapter(&i2c->adap);

	return 0;
}

#ifdef CONFIG_PM_SLEEP
static int mtk_i2c_suspend_noirq(struct device *dev)
{
	struct mtk_i2c *i2c = dev_get_drvdata(dev);

	i2c_mark_adapter_suspended(&i2c->adap);

	return 0;
}

static int mtk_i2c_resume_noirq(struct device *dev)
{
	int ret;
	struct mtk_i2c *i2c = dev_get_drvdata(dev);

	ret = mtk_i2c_clock_enable(i2c);
	if (ret) {
		dev_err(dev, "clock enable failed!\n");
		return ret;
	}

	mtk_i2c_init_hw(i2c);

	mtk_i2c_clock_disable(i2c);

	i2c_mark_adapter_resumed(&i2c->adap);

	return 0;
}
#endif

static const struct dev_pm_ops mtk_i2c_pm = {
	SET_NOIRQ_SYSTEM_SLEEP_PM_OPS(mtk_i2c_suspend_noirq,
				      mtk_i2c_resume_noirq)
};

static struct platform_driver mtk_i2c_driver = {
	.probe = mtk_i2c_probe,
	.remove = mtk_i2c_remove,
	.driver = {
		.name = I2C_DRV_NAME,
		.pm = &mtk_i2c_pm,
		.of_match_table = of_match_ptr(mtk_i2c_of_match),
	},
};

module_platform_driver(mtk_i2c_driver);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("MediaTek I2C Bus Driver");
MODULE_AUTHOR("Xudong Chen <xudong.chen@mediatek.com>");

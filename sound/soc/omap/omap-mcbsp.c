/*
 * omap-mcbsp.c  --  OMAP ALSA SoC DAI driver using McBSP port
 *
 * Copyright (C) 2008 Nokia Corporation
 *
 * Contact: Jarkko Nikula <jarkko.nikula@nokia.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA
 *
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/device.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/initval.h>
#include <sound/soc.h>

#include <mach/control.h>
#include <mach/dma.h>
#include <mach/mcbsp.h>
#include "omap-mcbsp.h"
#include "omap-pcm.h"

#define OMAP_MCBSP_RATES	(SNDRV_PCM_RATE_8000_96000)

struct omap_mcbsp_data {
	unsigned int			bus_id;
	struct omap_mcbsp_reg_cfg	regs;
	unsigned int			fmt;
	int				clk_id;
	/*
	 * Flags indicating is the bus already onfigured by
	 * another substream
	 */
	int				configured;
	int				tx_active;
	int 				rx_active;
};

#define to_mcbsp(priv)	container_of((priv), struct omap_mcbsp_data, bus_id)

static struct omap_mcbsp_data mcbsp_data[NUM_LINKS];

/*
 * Stream DMA parameters. DMA request line and port address are set runtime
 * since they are different between OMAP1 and later OMAPs
 */
static struct omap_pcm_dma_data omap_mcbsp_dai_dma_params[NUM_LINKS][2];

#if defined(CONFIG_ARCH_OMAP15XX) || defined(CONFIG_ARCH_OMAP16XX)
static const int omap1_dma_reqs[][2] = {
	{ OMAP_DMA_MCBSP1_TX, OMAP_DMA_MCBSP1_RX },
	{ OMAP_DMA_MCBSP2_TX, OMAP_DMA_MCBSP2_RX },
	{ OMAP_DMA_MCBSP3_TX, OMAP_DMA_MCBSP3_RX },
};
static const unsigned long omap1_mcbsp_port[][2] = {
	{ OMAP1510_MCBSP1_BASE + OMAP_MCBSP_REG_DXR1,
	  OMAP1510_MCBSP1_BASE + OMAP_MCBSP_REG_DRR1 },
	{ OMAP1510_MCBSP2_BASE + OMAP_MCBSP_REG_DXR1,
	  OMAP1510_MCBSP2_BASE + OMAP_MCBSP_REG_DRR1 },
	{ OMAP1510_MCBSP3_BASE + OMAP_MCBSP_REG_DXR1,
	  OMAP1510_MCBSP3_BASE + OMAP_MCBSP_REG_DRR1 },
};
#else
static const int omap1_dma_reqs[][2] = {};
static const unsigned long omap1_mcbsp_port[][2] = {};
#endif

#if defined(CONFIG_ARCH_OMAP24XX) || defined(CONFIG_ARCH_OMAP34XX)
static const int omap24xx_dma_reqs[][2] = {
	{ OMAP24XX_DMA_MCBSP1_TX, OMAP24XX_DMA_MCBSP1_RX },
	{ OMAP24XX_DMA_MCBSP2_TX, OMAP24XX_DMA_MCBSP2_RX },
#if defined(CONFIG_ARCH_OMAP2430) || defined(CONFIG_ARCH_OMAP34XX)
	{ OMAP24XX_DMA_MCBSP3_TX, OMAP24XX_DMA_MCBSP3_RX },
	{ OMAP24XX_DMA_MCBSP4_TX, OMAP24XX_DMA_MCBSP4_RX },
	{ OMAP24XX_DMA_MCBSP5_TX, OMAP24XX_DMA_MCBSP5_RX },
#endif
};
#else
static const int omap24xx_dma_reqs[][2] = {};
#endif

#if defined(CONFIG_ARCH_OMAP2420)
static const unsigned long omap2420_mcbsp_port[][2] = {
	{ OMAP24XX_MCBSP1_BASE + OMAP_MCBSP_REG_DXR1,
	  OMAP24XX_MCBSP1_BASE + OMAP_MCBSP_REG_DRR1 },
	{ OMAP24XX_MCBSP2_BASE + OMAP_MCBSP_REG_DXR1,
	  OMAP24XX_MCBSP2_BASE + OMAP_MCBSP_REG_DRR1 },
};
#else
static const unsigned long omap2420_mcbsp_port[][2] = {};
#endif

#if defined(CONFIG_ARCH_OMAP2430)
static const unsigned long omap2430_mcbsp_port[][2] = {
	{ OMAP24XX_MCBSP1_BASE + OMAP_MCBSP_REG_DXR,
	  OMAP24XX_MCBSP1_BASE + OMAP_MCBSP_REG_DRR },
	{ OMAP24XX_MCBSP2_BASE + OMAP_MCBSP_REG_DXR,
	  OMAP24XX_MCBSP2_BASE + OMAP_MCBSP_REG_DRR },
	{ OMAP2430_MCBSP3_BASE + OMAP_MCBSP_REG_DXR,
	  OMAP2430_MCBSP3_BASE + OMAP_MCBSP_REG_DRR },
	{ OMAP2430_MCBSP4_BASE + OMAP_MCBSP_REG_DXR,
	  OMAP2430_MCBSP4_BASE + OMAP_MCBSP_REG_DRR },
	{ OMAP2430_MCBSP5_BASE + OMAP_MCBSP_REG_DXR,
	  OMAP2430_MCBSP5_BASE + OMAP_MCBSP_REG_DRR },
};
#else
static const unsigned long omap2430_mcbsp_port[][2] = {};
#endif

#if defined(CONFIG_ARCH_OMAP34XX)
static const unsigned long omap34xx_mcbsp_port[][2] = {
	{ OMAP34XX_MCBSP1_BASE + OMAP_MCBSP_REG_DXR,
	  OMAP34XX_MCBSP1_BASE + OMAP_MCBSP_REG_DRR },
	{ OMAP34XX_MCBSP2_BASE + OMAP_MCBSP_REG_DXR,
	  OMAP34XX_MCBSP2_BASE + OMAP_MCBSP_REG_DRR },
	{ OMAP34XX_MCBSP3_BASE + OMAP_MCBSP_REG_DXR,
	  OMAP34XX_MCBSP3_BASE + OMAP_MCBSP_REG_DRR },
	{ OMAP34XX_MCBSP4_BASE + OMAP_MCBSP_REG_DXR,
	  OMAP34XX_MCBSP4_BASE + OMAP_MCBSP_REG_DRR },
	{ OMAP34XX_MCBSP5_BASE + OMAP_MCBSP_REG_DXR,
	  OMAP34XX_MCBSP5_BASE + OMAP_MCBSP_REG_DRR },
};
#else
static const unsigned long omap34xx_mcbsp_port[][2] = {};
#endif

static int omap_mcbsp_dai_set_clks_src(struct omap_mcbsp_data *mcbsp_data,
					int clk_id);

static int omap_mcbsp_dai_startup(struct snd_pcm_substream *substream,
				  struct snd_soc_dai *dai)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *cpu_dai = rtd->dai->cpu_dai;
	struct omap_mcbsp_data *mcbsp_data = to_mcbsp(cpu_dai->private_data);
	int err = 0;

	if (cpu_is_omap343x() && mcbsp_data->bus_id == 1) {
		/*
		 * McBSP2 in OMAP3 has 1024 * 32-bit internal audio buffer.
		 * Set constraint for minimum buffer size to the same than FIFO
		 * size in order to avoid underruns in playback startup because
		 * HW is keeping the DMA request active until FIFO is filled.
		 */
		if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
			snd_pcm_hw_constraint_minmax(substream->runtime,
			SNDRV_PCM_HW_PARAM_BUFFER_BYTES, 4096, UINT_MAX);
		else
			snd_pcm_hw_constraint_minmax(substream->runtime,
			SNDRV_PCM_HW_PARAM_BUFFER_BYTES, 1024, UINT_MAX);
	}

	if (!cpu_dai->active) {
		err = omap_mcbsp_request(mcbsp_data->bus_id);
		cpu_dai->active = 1;
	}

	return err;
}

static void omap_mcbsp_dai_shutdown(struct snd_pcm_substream *substream,
				    struct snd_soc_dai *dai)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *cpu_dai = rtd->dai->cpu_dai;
	struct omap_mcbsp_data *mcbsp_data = to_mcbsp(cpu_dai->private_data);

	if (!cpu_dai->active) {
		omap_mcbsp_free(mcbsp_data->bus_id);
		cpu_dai->active = 0;
		mcbsp_data->configured = 0;
	}
}

static int omap_mcbsp_dai_trigger(struct snd_pcm_substream *substream, int cmd,
				  struct snd_soc_dai *dai)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *cpu_dai = rtd->dai->cpu_dai;
	struct omap_mcbsp_data *mcbsp_data = to_mcbsp(cpu_dai->private_data);
	int err = 0, play = (substream->stream == SNDRV_PCM_STREAM_PLAYBACK);

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
	case SNDRV_PCM_TRIGGER_RESUME:
		if (cpu_dai->active)
			omap_mcbsp_dai_set_clks_src(mcbsp_data,
					mcbsp_data->clk_id);
		omap_mcbsp_start(mcbsp_data->bus_id, play, !play);
		if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
			mcbsp_data->tx_active = 1;
		else
			mcbsp_data->rx_active = 1;
		break;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
	case SNDRV_PCM_TRIGGER_SUSPEND:
		if (cpu_dai->active)
			omap_mcbsp_dai_set_clks_src(mcbsp_data,
				OMAP_MCBSP_SYSCLK_CLKS_FCLK);
		omap_mcbsp_stop(mcbsp_data->bus_id, play, !play);
		if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
			mcbsp_data->tx_active = 0;
		else
			mcbsp_data->rx_active = 0;
		break;
	default:
		err = -EINVAL;
	}

	return err;
}

static int omap_mcbsp_dai_hw_params(struct snd_pcm_substream *substream,
				    struct snd_pcm_hw_params *params,
				    struct snd_soc_dai *dai)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *cpu_dai = rtd->dai->cpu_dai;
	struct omap_mcbsp_data *mcbsp_data = to_mcbsp(cpu_dai->private_data);
	struct omap_mcbsp_reg_cfg *regs = &mcbsp_data->regs;
	int dma, bus_id = mcbsp_data->bus_id, id = cpu_dai->id;
	int uninitialized_var(wlen);
	int uninitialized_var(channels);
	int uninitialized_var(wpf);
	unsigned long uninitialized_var(port);
	unsigned int uninitialized_var(format);
	int xfer_size = 0;

	if (cpu_class_is_omap1()) {
		dma = omap1_dma_reqs[bus_id][substream->stream];
		port = omap1_mcbsp_port[bus_id][substream->stream];
	} else if (cpu_is_omap2420()) {
		dma = omap24xx_dma_reqs[bus_id][substream->stream];
		port = omap2420_mcbsp_port[bus_id][substream->stream];
	} else if (cpu_is_omap2430()) {
		dma = omap24xx_dma_reqs[bus_id][substream->stream];
		port = omap2430_mcbsp_port[bus_id][substream->stream];
	} else if (cpu_is_omap343x()) {
		dma = omap24xx_dma_reqs[bus_id][substream->stream];
		port = omap34xx_mcbsp_port[bus_id][substream->stream];
		xfer_size = omap34xx_mcbsp_thresholds[bus_id]
					[substream->stream];
		/* reset the xfer_size to the integral multiple of
		the buffer size. This is for DMA packet mode transfer */
		if (xfer_size) {
			int buffer_size = params_buffer_size(params);
			if (xfer_size > buffer_size) {
				printk(KERN_DEBUG "buffer_size is %d \n",
						buffer_size);
				xfer_size = 0;
			} else {
				int temp =  buffer_size / xfer_size;
				while (buffer_size % xfer_size) {
					temp++;
					xfer_size = buffer_size / (temp);
				}
			}
		}
	} else {
		return -ENODEV;
	}
	omap_mcbsp_dai_dma_params[id][substream->stream].name =
		substream->stream ? "Audio Capture" : "Audio Playback";
	omap_mcbsp_dai_dma_params[id][substream->stream].dma_req = dma;
	omap_mcbsp_dai_dma_params[id][substream->stream].port_addr = port;
	omap_mcbsp_dai_dma_params[id][substream->stream].xfer_size = xfer_size;
	cpu_dai->dma_data = &omap_mcbsp_dai_dma_params[id][substream->stream];

	if (mcbsp_data->configured) {
		/* McBSP already configured by another stream */
		return 0;
	}

	format = mcbsp_data->fmt & SND_SOC_DAIFMT_FORMAT_MASK;
	wpf = channels = params_channels(params);
	switch (channels) {
	case 2:
		if (format == SND_SOC_DAIFMT_I2S) {
			/* Use dual-phase frames */
			regs->rcr2	|= RPHASE;
			regs->xcr2	|= XPHASE;
			/* Set 1 word per (McBSP) frame for phase1 and phase2 */
			wpf--;
			regs->rcr2	|= RFRLEN2(wpf - 1);
			regs->xcr2	|= XFRLEN2(wpf - 1);
		} else if (format == SND_SOC_DAIFMT_I2S_1PHASE) {
			printk(KERN_DEBUG "Configure McBSP for 1 phase\n");
			regs->xcr2	&= ~(XPHASE);
			regs->rcr2	&= ~(RPHASE);
			wpf--;
		}
	case 1:
	case 4:
		/* Set word per (McBSP) frame for phase1 */
		regs->rcr1	|= RFRLEN1(wpf - 1);
		regs->xcr1	|= XFRLEN1(wpf - 1);
		break;
	default:
		/* Unsupported number of channels */
		return -EINVAL;
	}

	switch (params_format(params)) {
	case SNDRV_PCM_FORMAT_S16_LE:
		/* Set word lengths */
		if (format == SND_SOC_DAIFMT_I2S_1PHASE) {
			regs->xcr1	|= XWDLEN1(OMAP_MCBSP_WORD_32);
			regs->rcr1	|= RWDLEN1(OMAP_MCBSP_WORD_32);
			omap_mcbsp_dai_dma_params[id]
			[SNDRV_PCM_STREAM_PLAYBACK].dma_word_size = 32;
			omap_mcbsp_dai_dma_params[id]
			[SNDRV_PCM_STREAM_CAPTURE].dma_word_size = 32;
		} else {
			wlen = 16;
			regs->rcr2	|= RWDLEN2(OMAP_MCBSP_WORD_16);
			regs->rcr1	|= RWDLEN1(OMAP_MCBSP_WORD_16);
			regs->xcr2	|= XWDLEN2(OMAP_MCBSP_WORD_16);
			regs->xcr1	|= XWDLEN1(OMAP_MCBSP_WORD_16);
			omap_mcbsp_dai_dma_params[id]
			[substream->stream].dma_word_size = 16;
		}
		break;
	default:
		/* Unsupported PCM format */
		return -EINVAL;
	}

	/* Set FS period and length in terms of bit clock periods */
	switch (format) {
	case SND_SOC_DAIFMT_I2S:
	case SND_SOC_DAIFMT_I2S_1PHASE:
		regs->srgr2	|= FPER(wlen * channels - 1);
		regs->srgr1	|= FWID(wlen - 1);
		break;
	case SND_SOC_DAIFMT_DSP_A:
	case SND_SOC_DAIFMT_DSP_B:
		regs->srgr2	|= FPER(wlen * channels - 1);
		regs->srgr1	|= FWID(0);
		break;
	}

	regs->xccr |= XDMAEN;
	regs->wken = XRDYEN;
	regs->rccr |= RDMAEN;

	omap_mcbsp_config(bus_id, &mcbsp_data->regs);

	if ((bus_id == 1) && (xfer_size != 0)) {
		printk(KERN_DEBUG "Configure McBSP TX FIFO threshold to %d\n",
			xfer_size);
		omap_mcbsp_set_tx_threshold(bus_id, xfer_size);
	}

	mcbsp_data->configured = 1;

	return 0;
}

static int omap_mcbsp_dai_prepare(struct snd_pcm_substream *substream,
						struct snd_soc_dai *dai)
{
		struct snd_soc_pcm_runtime *rtd = substream->private_data;
		struct snd_soc_dai *cpu_dai = rtd->dai->cpu_dai;
		struct omap_mcbsp_data *mcbsp_data =
				to_mcbsp(cpu_dai->private_data);
		int bus_id = mcbsp_data->bus_id, id = cpu_dai->id;
		int     xfer_size;

		xfer_size =
		omap_mcbsp_dai_dma_params[id][substream->stream].xfer_size;

		if (!(mcbsp_data->tx_active || mcbsp_data->rx_active)) {
			omap_mcbsp_config(bus_id, &mcbsp_data->regs);

		if ((bus_id == 1) && (xfer_size > 0))
			omap_mcbsp_set_tx_threshold(bus_id, xfer_size);
		}
		return 0;
}

/*
 * This must be called before _set_clkdiv and _set_sysclk since McBSP register
 * cache is initialized here
 */
static int omap_mcbsp_dai_set_dai_fmt(struct snd_soc_dai *cpu_dai,
				      unsigned int fmt)
{
	struct omap_mcbsp_data *mcbsp_data = to_mcbsp(cpu_dai->private_data);
	struct omap_mcbsp_reg_cfg *regs = &mcbsp_data->regs;
	unsigned int temp_fmt = fmt;

	if (mcbsp_data->configured)
		return 0;

	mcbsp_data->fmt = fmt;
	memset(regs, 0, sizeof(*regs));
	/* Generic McBSP register settings */
	regs->spcr2	|= XINTM(3) | FREE;
	regs->spcr1	|= RINTM(3);
	regs->rcr2	|= RFIG;
	regs->xcr2	|= XFIG;
	if (cpu_is_omap2430() || cpu_is_omap34xx()) {
		regs->xccr = DXENDLY(1);
		regs->rccr = RFULL_CYCLE;
	}

	switch (fmt & SND_SOC_DAIFMT_FORMAT_MASK) {
	case SND_SOC_DAIFMT_I2S:
	case SND_SOC_DAIFMT_I2S_1PHASE:
		/* 1-bit data delay */
		regs->rcr2	|= RDATDLY(1);
		regs->xcr2	|= XDATDLY(1);
		break;
	case SND_SOC_DAIFMT_DSP_A:
		/* 1-bit data delay */
		regs->rcr2      |= RDATDLY(1);
		regs->xcr2      |= XDATDLY(1);
		/* Invert FS polarity configuration */
		temp_fmt ^= SND_SOC_DAIFMT_NB_IF;
		break;
	case SND_SOC_DAIFMT_DSP_B:
		/* 0-bit data delay */
		regs->rcr2      |= RDATDLY(0);
		regs->xcr2      |= XDATDLY(0);
		/* Invert FS polarity configuration */
		temp_fmt ^= SND_SOC_DAIFMT_NB_IF;
		break;
	default:
		/* Unsupported data format */
		return -EINVAL;
	}

	switch (fmt & SND_SOC_DAIFMT_MASTER_MASK) {
	case SND_SOC_DAIFMT_CBS_CFS:
		/* McBSP master. Set FS and bit clocks as outputs */
		regs->pcr0	|= FSXM | FSRM |
				   CLKXM | CLKRM;
		/* Sample rate generator drives the FS */
		regs->srgr2	|= FSGM;
		break;
	case SND_SOC_DAIFMT_CBM_CFM:
		/* McBSP slave */
		break;
	default:
		/* Unsupported master/slave configuration */
		return -EINVAL;
	}

	/* Set bit clock (CLKX/CLKR) and FS polarities */
	switch (temp_fmt & SND_SOC_DAIFMT_INV_MASK) {
	case SND_SOC_DAIFMT_NB_NF:
		/*
		 * Normal BCLK + FS.
		 * FS active low. TX data driven on falling edge of bit clock
		 * and RX data sampled on rising edge of bit clock.
		 */
		regs->pcr0	|= FSXP | FSRP |
				   CLKXP | CLKRP;
		break;
	case SND_SOC_DAIFMT_NB_IF:
		regs->pcr0	|= CLKXP | CLKRP;
		break;
	case SND_SOC_DAIFMT_IB_NF:
		regs->pcr0	|= FSXP | FSRP;
		break;
	case SND_SOC_DAIFMT_IB_IF:
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int omap_mcbsp_dai_set_clkdiv(struct snd_soc_dai *cpu_dai,
				     int div_id, int div)
{
	struct omap_mcbsp_data *mcbsp_data = to_mcbsp(cpu_dai->private_data);
	struct omap_mcbsp_reg_cfg *regs = &mcbsp_data->regs;

	if (div_id != OMAP_MCBSP_CLKGDV)
		return -ENODEV;

	regs->srgr1	|= CLKGDV(div - 1);

	return 0;
}

static int omap_mcbsp_dai_set_clks_src(struct omap_mcbsp_data *mcbsp_data,
				       int clk_id)
{
	int sel_bit;
	u16 reg, reg_devconf1 = OMAP243X_CONTROL_DEVCONF1;

	if (cpu_class_is_omap1()) {
		/* OMAP1's can use only external source clock */
		if (unlikely(clk_id == OMAP_MCBSP_SYSCLK_CLKS_FCLK))
			return -EINVAL;
		else
			return 0;
	}

	if (cpu_is_omap2420() && mcbsp_data->bus_id > 1)
		return -EINVAL;

	if (cpu_is_omap343x())
		reg_devconf1 = OMAP343X_CONTROL_DEVCONF1;

	switch (mcbsp_data->bus_id) {
	case 0:
		reg = OMAP2_CONTROL_DEVCONF0;
		sel_bit = 2;
		break;
	case 1:
		reg = OMAP2_CONTROL_DEVCONF0;
		sel_bit = 6;
		break;
	case 2:
		reg = reg_devconf1;
		sel_bit = 0;
		break;
	case 3:
		reg = reg_devconf1;
		sel_bit = 2;
		break;
	case 4:
		reg = reg_devconf1;
		sel_bit = 4;
		break;
	default:
		return -EINVAL;
	}

	if (clk_id == OMAP_MCBSP_SYSCLK_CLKS_FCLK)
		omap_ctrl_writel(omap_ctrl_readl(reg) & ~(1 << sel_bit), reg);
	else
		omap_ctrl_writel(omap_ctrl_readl(reg) | (1 << sel_bit), reg);

	return 0;
}

static int omap_mcbsp_dai_set_dai_sysclk(struct snd_soc_dai *cpu_dai,
					 int clk_id, unsigned int freq,
					 int dir)
{
	struct omap_mcbsp_data *mcbsp_data = to_mcbsp(cpu_dai->private_data);
	struct omap_mcbsp_reg_cfg *regs = &mcbsp_data->regs;
	int err = 0;

	switch (clk_id) {
	case OMAP_MCBSP_SYSCLK_CLK:
		regs->srgr2	|= CLKSM;
		break;
	case OMAP_MCBSP_SYSCLK_CLKS_FCLK:
	case OMAP_MCBSP_SYSCLK_CLKS_EXT:
		err = omap_mcbsp_dai_set_clks_src(mcbsp_data, clk_id);
		mcbsp_data->clk_id = clk_id;
		break;

	case OMAP_MCBSP_SYSCLK_CLKX_EXT:
		regs->srgr2	|= CLKSM;
	case OMAP_MCBSP_SYSCLK_CLKR_EXT:
		regs->pcr0	|= SCLKME;
		break;
	default:
		err = -ENODEV;
	}

	return err;
}

int omap_mcbsp_dai_suspend(struct snd_soc_dai *cpu_dai)
{
	struct omap_mcbsp_data *mcbsp_data = to_mcbsp(cpu_dai->private_data);

    printk(KERN_INFO "%s: cpu_dai->active: %d mcbsp pending: 0x%x\n", 
           __FUNCTION__, cpu_dai->active, omap_mcbsp_pending_status(mcbsp_data->bus_id));

	if (cpu_dai->active) {
		omap_mcbsp_dai_set_clks_src(mcbsp_data, OMAP_MCBSP_SYSCLK_CLKS_FCLK);
		omap_mcbsp_disable_fclk(mcbsp_data->bus_id);
	}

	return 0;
}

int omap_mcbsp_dai_resume(struct snd_soc_dai *cpu_dai)
{
	struct omap_mcbsp_data *mcbsp_data = to_mcbsp(cpu_dai->private_data);

    printk(KERN_INFO "%s: cpu_dai->active: %d mcbsp pending: 0%x\n", 
           __FUNCTION__, cpu_dai->active, omap_mcbsp_pending_status(mcbsp_data->bus_id));

	if (cpu_dai->active) {
		omap_mcbsp_enable_fclk(mcbsp_data->bus_id);
		omap_mcbsp_config(mcbsp_data->bus_id, &mcbsp_data->regs);
		omap_mcbsp_dai_set_clks_src(mcbsp_data, mcbsp_data->clk_id);
	}

	return 0;
}

#define OMAP_MCBSP_DAI_BUILDER(link_id)				\
{								\
	.name = "omap-mcbsp-dai-"#link_id,			\
	.id = (link_id),					\
	.playback = {						\
		.channels_min = 1,				\
		.channels_max = 4,				\
		.rates = OMAP_MCBSP_RATES,			\
		.formats = SNDRV_PCM_FMTBIT_S16_LE,		\
	},							\
	.capture = {						\
		.channels_min = 1,				\
		.channels_max = 4,				\
		.rates = OMAP_MCBSP_RATES,			\
		.formats = SNDRV_PCM_FMTBIT_S16_LE,		\
	},							\
	.suspend = omap_mcbsp_dai_suspend,			\
	.resume = omap_mcbsp_dai_resume,			\
	.ops = {						\
		.startup = omap_mcbsp_dai_startup,		\
		.shutdown = omap_mcbsp_dai_shutdown,		\
		.trigger = omap_mcbsp_dai_trigger,		\
		.hw_params = omap_mcbsp_dai_hw_params,		\
		.prepare = omap_mcbsp_dai_prepare,		\
		.set_fmt = omap_mcbsp_dai_set_dai_fmt,		\
		.set_clkdiv = omap_mcbsp_dai_set_clkdiv,	\
		.set_sysclk = omap_mcbsp_dai_set_dai_sysclk,	\
	},							\
	.private_data = &mcbsp_data[(link_id)].bus_id,		\
}

struct snd_soc_dai omap_mcbsp_dai[] = {
	OMAP_MCBSP_DAI_BUILDER(0),
	OMAP_MCBSP_DAI_BUILDER(1),
#if NUM_LINKS >= 3
	OMAP_MCBSP_DAI_BUILDER(2),
#endif
#if NUM_LINKS == 5
	OMAP_MCBSP_DAI_BUILDER(3),
	OMAP_MCBSP_DAI_BUILDER(4),
#endif
};

EXPORT_SYMBOL_GPL(omap_mcbsp_dai);

static int __init snd_omap_mcbsp_init(void)
{
	return snd_soc_register_dais(omap_mcbsp_dai,
				     ARRAY_SIZE(omap_mcbsp_dai));
}
module_init(snd_omap_mcbsp_init);

static void __exit snd_omap_mcbsp_exit(void)
{
	snd_soc_unregister_dais(omap_mcbsp_dai, ARRAY_SIZE(omap_mcbsp_dai));
}
module_exit(snd_omap_mcbsp_exit);

MODULE_AUTHOR("Jarkko Nikula <jarkko.nikula@nokia.com>");
MODULE_DESCRIPTION("OMAP I2S SoC Interface");
MODULE_LICENSE("GPL");

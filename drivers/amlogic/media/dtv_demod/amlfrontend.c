// SPDX-License-Identifier: (GPL-2.0+ OR MIT)
/*
 * Copyright (c) 2019 Amlogic, Inc. All rights reserved.
 */

/*****************************************************************
 **  author :
 **	     Shijie.Rong@amlogic.com
 **  version :
 **	v1.0	  12/3/13
 **	v2.0	 15/10/12
 **	v3.0	  17/11/15
 *****************************************************************/
#define __DVB_CORE__	/*ary 2018-1-31*/

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/delay.h>
#include <linux/jiffies.h>
#include <linux/slab.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/pm_domain.h>
#include <linux/firmware.h>
#include <linux/err.h>	/*IS_ERR*/
#include <linux/clk.h>	/*clk tree*/
#include <linux/of_device.h>
#include <linux/of_reserved_mem.h>
#include <linux/of_irq.h>
#include <linux/interrupt.h>
#include <linux/crc32.h>

#ifdef ARC_700
#include <asm/arch/am_regs.h>
#else
/* #include <mach/am_regs.h> */
#endif
#include <linux/i2c.h>
#include <linux/gpio.h>

#include "aml_demod.h"
#include "demod_func.h"
#include "demod_dbg.h"
#include "dvbt_func.h"
#include "dvbs.h"
#include "dvbc_func.h"
#include "dvbs_diseqc.h"

/*dma_get_cma_size_int_byte*/
#include <linux/amlogic/media/codec_mm/codec_mm.h>
#include <linux/dma-contiguous.h>

#include <linux/amlogic/media/frame_provider/tvin/tvin.h>
#include <linux/amlogic/media/vout/vdac_dev.h>
#include <linux/amlogic/aml_dtvdemod.h>

#define AMLDTVDEMOD_VER "V1.0.15"

MODULE_PARM_DESC(auto_search_std, "\n\t\t atsc-c std&hrc search");
static unsigned int auto_search_std;
module_param(auto_search_std, int, 0644);

MODULE_PARM_DESC(std_lock_timeout, "\n\t\t atsc-c std lock timeout");
static unsigned int std_lock_timeout = 1000;
module_param(std_lock_timeout, int, 0644);

/*use this flag to mark the new method for dvbc channel fast search
 *it's disabled as default, can be enabled if needed
 *we can make it always enabled after all testing are passed
 */
static unsigned int demod_dvbc_speedup_en = 1;

int aml_demod_debug = DBG_INFO;
module_param(aml_demod_debug, int, 0644);
MODULE_PARM_DESC(aml_demod_debug, "set debug level (info=bit1, reg=bit2, atsc=bit4,");

/*-----------------------------------*/
struct amldtvdemod_device_s *dtvdd_devp;

static DEFINE_MUTEX(amldtvdemod_device_mutex);

static int cci_thread;
static int memstart = 0x1ef00000;/* move to aml_dtv_demod*/
long *mem_buf;

static int dvb_tuner_delay = 100;
module_param(dvb_tuner_delay, int, 0644);
MODULE_PARM_DESC(dvb_atsc_count, "dvb_tuner_delay");

const char *name_reg[] = {
	"demod",
	"iohiu",
	"aobus",
	"reset",
};

#define END_SYS_DELIVERY	19
const char *name_fe_delivery_system[] = {
	"UNDEFINED",
	"DVBC_ANNEX_A",
	"DVBC_ANNEX_B",
	"DVBT",
	"DSS",
	"DVBS",
	"DVBS2",
	"DVBH",
	"ISDBT",
	"ISDBS",
	"ISDBC",
	"ATSC",
	"ATSCMH",
	"DTMB",
	"CMMB",
	"DAB",
	"DVBT2",
	"TURBO",
	"DVBC_ANNEX_C",
	"ANALOG",	/*19*/
};

const char *dtvdemod_get_cur_delsys(enum fe_delivery_system delsys)
{
	if ((delsys >= SYS_UNDEFINED) && (delsys <= SYS_ANALOG))
		return name_fe_delivery_system[delsys];
	else
		return "invalid delsys";
}

static void dtvdemod_vdac_enable(bool on);
static void dtvdemod_set_agc_pinmux(int on);
static int dtvdemod_leave_mode(struct amldtvdemod_device_s *devp);

struct amldtvdemod_device_s *dtvdemod_get_dev(void)
{
	return dtvdd_devp;
}

int convert_snr(int in_snr)
{
	int out_snr;
	static int calce_snr[40] = {
		5, 6, 8, 10, 13,
		16, 20, 25, 32, 40,
		50, 63, 80, 100, 126,
		159, 200, 252, 318, 400,
		504, 634, 798, 1005, 1265,
		1592, 2005, 2524, 3177, 4000,
		5036, 6340, 7981, 10048, 12649,
		15924, 20047, 25238, 31773, 40000};
	for (out_snr = 1 ; out_snr < 40; out_snr++)
		if (in_snr <= calce_snr[out_snr])
			break;

	return out_snr;
}

static void dtvdemod_do_8vsb_rst(void)
{
	union atsc_cntl_reg_0x20 val;

	val.bits = atsc_read_reg_v4(ATSC_CNTR_REG_0X20);
	val.b.cpu_rst = 1;
	atsc_write_reg_v4(ATSC_CNTR_REG_0X20, val.bits);
	val.b.cpu_rst = 0;
	atsc_write_reg_v4(ATSC_CNTR_REG_0X20, val.bits);
}

static int amdemod_check_8vsb_rst(struct aml_dtvdemod *demod)
{
	int ret = 0;

	/* for tl1/tm2, first time of pwr on,
	 * reset after signal locked
	 * for lock event, 0x79 is safe for reset
	 */
	if (demod->atsc_rst_done == 0) {
		if (demod->atsc_rst_needed) {
			dtvdemod_do_8vsb_rst();
			demod->atsc_rst_done = 1;
			ret = 0;
			PR_ATSC("reset done\n");
		}

		if (atsc_read_reg_v4(0x2e) >= 0x79) {
			demod->atsc_rst_needed = 1;
			ret = 0;
			PR_ATSC("need reset\n");
		} else {
			demod->atsc_rst_wait_cnt++;
			PR_ATSC("wait cnt: %d\n",
					demod->atsc_rst_wait_cnt);
		}

		if (demod->atsc_rst_wait_cnt >= 3 &&
			(atsc_read_reg_v4(0x2e) >= 0x76)) {
			demod->atsc_rst_done = 1;
			ret = 1;
		}
	} else if (atsc_read_reg_v4(0x2e) >= 0x76) {
		ret = 1;
	}

	return ret;
}

static int amdemod_stat_islock(struct aml_dtvdemod *demod,
		enum fe_delivery_system delsys)
{
	struct aml_demod_sts demod_sts;
	int lock_status;
	int dvbt_status1;
	int atsc_fsm;
	int ret = 0;
	unsigned int val;

	switch (delsys) {
	case SYS_DVBC_ANNEX_A:
	case SYS_DVBC_ANNEX_C:
		demod_sts.ch_sts = dvbc_get_ch_sts(demod);
		ret = demod_sts.ch_sts & 0x1;
		break;

	case SYS_ISDBT:
		dvbt_status1 = (dvbt_isdbt_rd_reg((0x0a << 2)) >> 20) & 0x3ff;
		lock_status = (dvbt_isdbt_rd_reg((0x2a << 2))) & 0xf;
		if (((lock_status == 9) || (lock_status == 10)) && (dvbt_status1 != 0))
			ret = 1;
		else
			ret = 0;
		break;

	case SYS_ATSC:
	case SYS_ATSCMH:
	case SYS_DVBC_ANNEX_B:
		if (demod->atsc_mode == QAM_64 || demod->atsc_mode == QAM_256) {
			if ((atsc_read_iqr_reg() >> 16) == 0x1f)
				ret = 1;
		} else if (demod->atsc_mode == VSB_8) {
			if (cpu_after_eq(MESON_CPU_MAJOR_ID_TL1)) {
				ret = amdemod_check_8vsb_rst(demod);
			} else {
				atsc_fsm = atsc_read_reg(0x0980);
				PR_DBGL("atsc status [%x]\n", atsc_fsm);

				if (atsc_read_reg(0x0980) >= 0x79)
					ret = 1;
			}
		} else {
			atsc_fsm = atsc_read_reg(0x0980);
			PR_DBGL("atsc status [%x]\n", atsc_fsm);
			if (atsc_read_reg(0x0980) >= 0x79)
				ret = 1;
		}
		break;

	case SYS_DTMB:
		ret = dtmb_reg_r_fec_lock();
		break;

	case SYS_DVBT:
		if (dvbt_t2_rdb(TS_STATUS) & 0x80)
			ret = 1;
		else
			ret = 0;
		break;

	case SYS_DVBT2:
		val = front_read_reg(0x3e);

		/* val[0], 0x581[7] */
		if (val & 0x1)
			ret = 1;
		else
			ret = 0;
		break;

	case SYS_DVBS:
	case SYS_DVBS2:
		ret = (dvbs_rd_byte(0x160) >> 3) & 0x1;
		break;

	default:
		break;
	}

	return ret;
}

unsigned int dtvdemod_get_atsc_lock_sts(struct aml_dtvdemod *demod)
{
	return amdemod_stat_islock(demod, SYS_ATSC);
}

void store_dvbc_qam_mode(struct aml_dtvdemod *demod,
		int qam_mode, int symbolrate)
{
	int qam_para;

	switch (qam_mode) {
	case QAM_16:
		qam_para = 4;
		break;
	case QAM_32:
		qam_para = 5;
		break;
	case QAM_64:
		qam_para = 6;
		break;
	case QAM_128:
		qam_para = 7;
		break;
	case QAM_256:
		qam_para = 8;
		break;
	case QAM_AUTO:
		qam_para = 6;
		break;
	default:
		qam_para = 6;
		break;
	}

	demod->para_demod.dvbc_qam = qam_para;
	demod->para_demod.dvbc_symbol = symbolrate;
}

static void dtvdemod_version(struct amldtvdemod_device_s *dev)
{
	atsc_set_version(dev->atsc_version);
}

static int amdemod_qam(enum fe_modulation qam)
{
	switch (qam) {
	case QAM_16:
		return 0;
	case QAM_32:
		return 1;
	case QAM_64:
		return 2;
	case QAM_128:
		return 3;
	case QAM_256:
		return 4;
	case VSB_8:
		return 5;
	case QAM_AUTO:
		return 6;
	default:
		return 2;
	}
	return 2;
}

static void gxtv_demod_dvbc_release(struct dvb_frontend *fe)
{

}

int timer_set_max(struct aml_dtvdemod *demod,
		enum ddemod_timer_s tmid, unsigned int max_val)
{
	demod->gtimer[tmid].max = max_val;

	return 0;
}

int timer_begain(struct aml_dtvdemod *demod, enum ddemod_timer_s tmid)
{
	demod->gtimer[tmid].start = jiffies_to_msecs(jiffies);
	demod->gtimer[tmid].enable = 1;

	PR_TIME("st %d=%d\n", tmid, (int)demod->gtimer[tmid].start);
	return 0;
}

int timer_disable(struct aml_dtvdemod *demod, enum ddemod_timer_s tmid)
{
	demod->gtimer[tmid].enable = 0;

	return 0;
}

int timer_is_en(struct aml_dtvdemod *demod, enum ddemod_timer_s tmid)
{
	return demod->gtimer[tmid].enable;
}

int timer_not_enough(struct aml_dtvdemod *demod, enum ddemod_timer_s tmid)
{
	int ret = 0;
	unsigned int time;

	if (demod->gtimer[tmid].enable) {
		time = jiffies_to_msecs(jiffies);
		if ((time - demod->gtimer[tmid].start) < demod->gtimer[tmid].max) {
			PR_TIME("now=%d\n", (int)time);
			ret = 1;
		}
	}
	return ret;
}

int timer_is_enough(struct aml_dtvdemod *demod, enum ddemod_timer_s tmid)
{
	int ret = 0;
	unsigned int time;

	/*Signal stability takes 200ms */
	if (demod->gtimer[tmid].enable) {
		time = jiffies_to_msecs(jiffies);
		if ((time - demod->gtimer[tmid].start) >= demod->gtimer[tmid].max) {
			PR_TIME("now=%d\n", (int)time);
			ret = 1;
		}
	}

	return ret;
}

int timer_tuner_not_enough(struct aml_dtvdemod *demod)
{
	int ret = 0;
	unsigned int time;
	enum ddemod_timer_s tmid;

	tmid = D_TIMER_DETECT;

	/*Signal stability takes 200ms */
	if (demod->gtimer[tmid].enable) {
		time = jiffies_to_msecs(jiffies);
		if ((time - demod->gtimer[tmid].start) < 200) {
			PR_TIME("nowt=%d\n", (int)time);
			ret = 1;
		}
	}

	return ret;
}

unsigned int demod_get_adc_clk(struct aml_dtvdemod *demod)
{
	return demod->demod_status.adc_freq;
}

unsigned int demod_get_sys_clk(struct aml_dtvdemod *demod)
{
	return demod->demod_status.clk_freq;
}

static int gxtv_demod_dvbc_read_status_timer
	(struct dvb_frontend *fe, enum fe_status *status)
{
	struct aml_dtvdemod *demod = (struct aml_dtvdemod *)fe->demodulator_priv;
	struct amldtvdemod_device_s *devp = (struct amldtvdemod_device_s *)demod->priv;
	struct aml_demod_sts demod_sts;
	int strenth = 0;
	int ilock = 0;

	/*check tuner*/
	if (!timer_tuner_not_enough(demod)) {
		strenth = tuner_get_ch_power2(fe);

		/*agc control,fine tune strength*/
		if (devp->pin_name &&
		    (strncmp(fe->ops.tuner_ops.info.name, "r842", 4) == 0)) {
			strenth += 22;

			if (strenth <= -80)
				strenth = dvbc_get_power_strength(
					qam_read_reg(demod, 0x27) & 0x7ff, strenth);
		}

		if (strenth < -87) {
			PR_DVBC("tuner no signal, strength:%d\n", strenth);
			*status = FE_TIMEDOUT;
			return 0;
		}
	}

	/*demod_sts.ch_sts = qam_read_reg(demod, 0x6);*/
	demod_sts.ch_sts = dvbc_get_ch_sts(demod);
	dvbc_status(demod, &demod_sts);

	if (demod_sts.ch_sts & 0x1) {
		ilock = 1;
		*status =
			FE_HAS_LOCK | FE_HAS_SIGNAL | FE_HAS_CARRIER |
			FE_HAS_VITERBI | FE_HAS_SYNC;
	} else {
		ilock = 0;

		if (timer_not_enough(demod, D_TIMER_DETECT)) {
			*status = 0;
			PR_TIME("s=0\n");
		} else {
			*status = FE_TIMEDOUT;
			timer_disable(demod, D_TIMER_DETECT);
		}
	}

	if (demod->last_lock != ilock) {
		PR_DBG("%s [id %d]: %s.\n", __func__, demod->id,
			ilock ? "!!  >> LOCK << !!" : "!! >> UNLOCK << !!");
		demod->last_lock = ilock;
	}

	return 0;
}

static int demod_dvbc_speed_up(struct aml_dtvdemod *demod,
		enum fe_status *status)
{
	unsigned int cnt, i, sts, check_ok = 0;
	struct aml_demod_sts demod_sts;
	const int dvbc_count = 5;
	int ilock = 0;

	if (*status == 0) {
		for (cnt = 0; cnt < 10; cnt++) {
			demod_sts.ch_sts = dvbc_get_ch_sts(demod);

			if (demod_sts.ch_sts & 0x1) {
				/*have signal*/
				*status =
					FE_HAS_LOCK | FE_HAS_SIGNAL |
					FE_HAS_CARRIER |
					FE_HAS_VITERBI | FE_HAS_SYNC;
				ilock = 1;
				check_ok = 1;
			} else {
				for (i = 0; i < dvbc_count; i++) {
					msleep(25);
					sts = dvbc_get_status(demod);

					if (sts >= 0x3)
						break;
				}

				PR_DBG("[rsj]dvbc_status is 0x%x\n", sts);

				if (sts < 0x3) {
					*status = FE_TIMEDOUT;
					ilock = 0;
					check_ok = 1;
					timer_disable(demod, D_TIMER_DETECT);
				}
			}

			if (check_ok == 1)
				break;

			msleep(20);
		}
	}

	if (demod->last_lock != ilock) {
		PR_DBG("%s [id %d]: %s.\n", __func__, demod->id,
			 ilock ? "!!  >> LOCK << !!" : "!! >> UNLOCK << !!");
		demod->last_lock = ilock;
	}

	return 0;
}

static int gxtv_demod_dvbc_read_ber(struct dvb_frontend *fe, u32 *ber)
{
	struct aml_demod_sts demod_sts;
	struct aml_dtvdemod *demod = (struct aml_dtvdemod *)fe->demodulator_priv;

	dvbc_status(demod, &demod_sts);
	*ber = demod_sts.ch_ber;

	return 0;
}

static int gxtv_demod_dvbc_read_signal_strength
	(struct dvb_frontend *fe, u16 *strength)
{
	struct aml_dtvdemod *demod = (struct aml_dtvdemod *)fe->demodulator_priv;
	struct amldtvdemod_device_s *devp = (struct amldtvdemod_device_s *)demod->priv;
	int tuner_sr;

#if 0
	if (fe->ops.tuner_ops.get_strength)
		tn_strength = fe->ops.tuner_ops.get_strength(fe);
	*strength = 256 - tn_strength;
#else
	if (devp->pin_name &&
	    (strncmp(fe->ops.tuner_ops.info.name, "r842", 4) == 0)) {
		tuner_sr = tuner_get_ch_power2(fe);
		tuner_sr += 22;

		if (tuner_sr <= -80)
			tuner_sr = dvbc_get_power_strength(
				qam_read_reg(demod, 0x27) & 0x7ff, tuner_sr);

		if (tuner_sr < -100)
			*strength = 0;
		else
			*strength = tuner_sr + 100;
	} else {
		*strength = tuner_get_ch_power3(fe);
	}
#endif

	return 0;
}

static int gxtv_demod_dvbc_read_snr(struct dvb_frontend *fe, u16 *snr)
{
	struct aml_demod_sts demod_sts;
	struct aml_dtvdemod *demod = (struct aml_dtvdemod *)fe->demodulator_priv;

	dvbc_status(demod, &demod_sts);
	*snr = demod_sts.ch_snr / 100;

	return 0;
}

static int gxtv_demod_dvbc_read_ucblocks(struct dvb_frontend *fe, u32 *ucblocks)
{
	*ucblocks = 0;
	return 0;
}

static int Gxtv_Demod_Dvbc_Init(struct aml_dtvdemod *demod, int mode)
{
	struct aml_demod_sys sys;
	struct amldtvdemod_device_s *devp = (struct amldtvdemod_device_s *)demod->priv;

	memset(&sys, 0, sizeof(sys));

	demod->demod_status.delsys = SYS_DVBC_ANNEX_A;

	if (mode == ADC_MODE) {
		sys.adc_clk = ADC_CLK_25M;
		sys.demod_clk = DEMOD_CLK_200M;
		demod->demod_status.tmp = ADC_MODE;
	} else {
		sys.adc_clk = ADC_CLK_24M;
		sys.demod_clk = DEMOD_CLK_72M;
		demod->demod_status.tmp = CRY_MODE;
	}

	if (cpu_after_eq(MESON_CPU_MAJOR_ID_TL1)) {
		sys.adc_clk = ADC_CLK_24M;
		sys.demod_clk = DEMOD_CLK_250M;
		demod->demod_status.tmp = CRY_MODE;
	}

	demod->demod_status.ch_if = SI2176_5M_IF * 1000;
	demod->auto_flags_trig = 0;
	demod->demod_status.adc_freq = sys.adc_clk;
	demod->demod_status.clk_freq = sys.demod_clk;

	if (devp->dvbc_inited)
		return 0;

	devp->dvbc_inited = true;

	PR_DBG("[%s] adc_clk is %d, demod_clk is %d, Pll_Mode is %d\n",
			__func__, sys.adc_clk, sys.demod_clk, demod->demod_status.tmp);

	/* sys clk div */
	if (devp->data->hw_ver == DTVDEMOD_HW_S4)
		dd_tvafe_hiu_reg_write(0x80 << 2, 0x501);
	else if (devp->data->hw_ver >= DTVDEMOD_HW_TL1)
		dd_tvafe_hiu_reg_write(HHI_DEMOD_CLK_CNTL << 2, 0x501);

	demod_set_sys(demod, &sys);

	return 0;
}

static int gxtv_demod_dvbc_set_frontend(struct dvb_frontend *fe)
{
	struct aml_dtvdemod *demod = (struct aml_dtvdemod *)fe->demodulator_priv;
	struct dtv_frontend_properties *c = &fe->dtv_property_cache;
	struct aml_demod_dvbc param;    /*mode 0:16, 1:32, 2:64, 3:128, 4:256*/
	struct aml_demod_sts demod_sts;
	int ret = 0;

	PR_INFO("%s [id %d]: delsys:%d, freq:%d, symbol_rate:%d, bw:%d, modul:%d, invert:%d.\n",
			__func__, demod->id, c->delivery_system, c->frequency, c->symbol_rate,
			c->bandwidth_hz, c->modulation, c->inversion);

	/*timer_set_max(demod, D_TIMER_DETECT, 4000);*/
	/*timer_begain(demod, D_TIMER_DETECT);*/
	memset(&param, 0, sizeof(param));
	param.ch_freq = c->frequency / 1000;
	param.mode = amdemod_qam(c->modulation);
	param.symb_rate = c->symbol_rate / 1000;

	if (demod->symb_rate_en)
		param.symb_rate = demod->symbol_rate;

	store_dvbc_qam_mode(demod, c->modulation, param.symb_rate);

	if (!cpu_after_eq(MESON_CPU_MAJOR_ID_TL1)) {
		if (param.mode == 3 && demod->demod_status.tmp != ADC_MODE)
			ret = Gxtv_Demod_Dvbc_Init(demod, ADC_MODE);
	}

	if (demod->autoflags == 1 && demod->auto_flags_trig == 0 &&
		demod->freq_dvbc == param.ch_freq) {
		PR_DBG("now is auto symbrating\n");
		return 0;
	}

	demod->auto_flags_trig = 0;
	demod->last_lock = -1;

	tuner_set_params(fe);/*aml_fe_analog_set_frontend(fe);*/
	dvbc_set_ch(demod, &param);
	/*0xf33 dvbc mode, 0x10f33 j.83b mode*/
	#if 0
	if (is_meson_txlx_cpu() || is_meson_gxlx_cpu())
		/*qam_write_reg(demod, 0x7, 0xf33);*/
		dvbc_init_reg_ext(demod);
	#endif
	if (cpu_after_eq(MESON_CPU_MAJOR_ID_TXLX) && !is_meson_txhd_cpu())
		dvbc_init_reg_ext(demod);

	if (demod->autoflags == 1) {
		PR_DBG("QAM_PLAYING mode,start auto sym\n");
		dvbc_set_auto_symtrack(demod);
		/* flag=1;*/
	}
	dvbc_status(demod, &demod_sts);
	demod->freq_dvbc = param.ch_freq;

	return ret;
}

static int gxtv_demod_dvbc_get_frontend(struct dvb_frontend *fe)
{
	#if 0
	/*these content will be writed into eeprom .*/
	struct aml_dtvdemod *demod = (struct aml_dtvdemod *)fe->demodulator_priv;
	struct dtv_frontend_properties *c = &fe->dtv_property_cache;
	u32 qam_mode;

	qam_mode = dvbc_get_qam_mode(demod);
	c->modulation = qam_mode + 1;
	PR_DBG("[mode] is %d\n", c->modulation);
	#endif

	return 0;
}

static void gxtv_demod_dvbt_release(struct dvb_frontend *fe)
{

}

static int dvbt_isdbt_read_status(struct dvb_frontend *fe, enum fe_status *status)
{
	struct aml_dtvdemod *demod = (struct aml_dtvdemod *)fe->demodulator_priv;
	int ilock;
	unsigned char s = 0;

	s = dvbt_get_status_ops()->get_status();

	if (s == 1) {
		ilock = 1;
		*status =
			FE_HAS_LOCK | FE_HAS_SIGNAL | FE_HAS_CARRIER |
			FE_HAS_VITERBI | FE_HAS_SYNC;
	} else {
		if (timer_not_enough(demod, D_TIMER_DETECT)) {
			ilock = 0;
			*status = 0;
			PR_TIME("timer not enough\n");

		} else {
			ilock = 0;
			*status = FE_TIMEDOUT;
			timer_disable(demod, D_TIMER_DETECT);
		}
	}

	if (demod->last_lock != ilock) {
		PR_INFO("%s [id %d]: %s.\n", __func__, demod->id,
			ilock ? "!!  >> LOCK << !!" : "!! >> UNLOCK << !!");
		demod->last_lock = ilock;
	}

	return 0;
}

static int dvbt_read_status(struct dvb_frontend *fe, enum fe_status *status)
{
	int ilock;
	unsigned char s = 0;
	int strenth;
	int strength_limit = THRD_TUNER_STRENTH_DVBT;
	struct aml_dtvdemod *demod = (struct aml_dtvdemod *)fe->demodulator_priv;
	struct amldtvdemod_device_s *devp = (struct amldtvdemod_device_s *)demod->priv;
	unsigned int tps_coderate, ts_fifo_cnt = 0, ts_cnt = 0;

	strenth = tuner_get_ch_power(fe);
	if (devp->tuner_strength_limit)
		strength_limit = devp->tuner_strength_limit;

	if (strenth < strength_limit) {
		*status = FE_TIMEDOUT;
		demod->last_lock = -1;
		PR_DVBT("tuner:no signal! strength:%d\n", strenth);
		return 0;
	}

	s = amdemod_stat_islock(demod, SYS_DVBT);

	if (s == 1) {
		ilock = 1;
		*status =
			FE_HAS_LOCK | FE_HAS_SIGNAL | FE_HAS_CARRIER |
			FE_HAS_VITERBI | FE_HAS_SYNC;
	} else {
		if (timer_not_enough(demod, D_TIMER_DETECT)) {
			ilock = 0;
			*status = 0;
		} else {
			ilock = 0;
			*status = FE_TIMEDOUT;
			timer_disable(demod, D_TIMER_DETECT);
		}
	}

	/* porting from ST driver FE_368dvbt_LockFec() */
	if (ilock) {
		do {
			dvbt_t2_wr_byte_bits(0x53d, 0, 6, 1);
			dvbt_t2_wr_byte_bits(0x572, 0, 0, 1);
			dvbt_t2_rdb(0x2913);

			if (dvbt_t2_rdb(0x3760) >> 5 & 1)
				tps_coderate = dvbt_t2_rdb(0x2913) >> 4 & 0x7;
			else
				tps_coderate = dvbt_t2_rdb(0x2913) & 0x7;

			switch (tps_coderate) {
			case 0: /*  CR=1/2*/
				dvbt_t2_wrb(0x53c, 0x41);
				break;
			case 1: /*  CR=2/3*/
				dvbt_t2_wrb(0x53c, 0x42);
				break;
			case 2: /*  CR=3/4*/
				dvbt_t2_wrb(0x53c, 0x44);
				break;
			case 3: /*  CR=5/6*/
				dvbt_t2_wrb(0x53c, 0x48);
				break;
			case 4: /*  CR=7/8*/
				dvbt_t2_wrb(0x53c, 0x60);
				break;
			default:
				dvbt_t2_wrb(0x53c, 0x6f);
				break;
			}

			switch (tps_coderate) {
			case 0: /*  CR=1/2*/
				dvbt_t2_wrb(0x5d1, 0x78);
				break;
			default: /* other CR */
				dvbt_t2_wrb(0x5d1, 0x60);
				break;
			}

			if (amdemod_stat_islock(demod, SYS_DVBT))
				ts_fifo_cnt++;
			else
				ts_fifo_cnt = 0;

			ts_cnt++;
			usleep_range(10000, 10001);
		} while (ts_fifo_cnt < 4 && ts_cnt <= 10);
	}

	if (ilock && ts_fifo_cnt < 4)
		*status = 0;

	if (demod->last_lock != ilock) {
		if (*status == (FE_HAS_LOCK | FE_HAS_SIGNAL | FE_HAS_CARRIER |
		    FE_HAS_VITERBI | FE_HAS_SYNC)) {
			PR_INFO("%s [id %d]: !!  >> LOCK << !!, freq:%d\n",
					__func__, demod->id, fe->dtv_property_cache.frequency);
			demod->last_lock = ilock;
		} else if (*status == FE_TIMEDOUT) {
			PR_INFO("%s [id %d]: !!  >> UNLOCK << !!\n", __func__, demod->id);
			demod->last_lock = ilock;
		} else {
			PR_INFO("%s [id %d]: !!  >> WAIT << !!\n", __func__, demod->id);
		}
	}

	return 0;
}

static int dvbt2_check_ldpc_ite_max(unsigned int val)
{
	unsigned int ldpc_ite_max;
	unsigned int ret = 0;
	unsigned int ldpc_ite_val;

	/* val[6-2]:0x8c3 */
	switch ((val >> 2) & 0x1f) {
	case QPSK_1_2:
	case QPSK_3_4:
	case QPSK_4_5:
	case QAM16_1_2:
	case QAM16_3_4:
	case QAM64_1_2:
	case QAM64_3_4:
	case QAM256_1_2:
	case QAM256_3_4:
		ldpc_ite_max = 0x23;
		break;

	case QPSK_3_5:
	case QAM16_3_5:
	case QAM64_3_5:
	case QAM256_3_5:
		ldpc_ite_max = 0x1b;
		break;

	case QPSK_2_3:
	case QAM16_2_3:
	case QAM64_2_3:
	case QAM256_2_3:
		ldpc_ite_max = 0x24;
		break;

	case QPSK_5_6:
	case QAM16_5_6:
	case QAM64_5_6:
	case QAM256_5_6:
		ldpc_ite_max = 0x21;
		break;

	case QAM16_4_5:
	case QAM64_4_5:
	case QAM256_4_5:
		ldpc_ite_max = 0x22;
		break;

	default:
		ldpc_ite_max = 0x24;
		break;
	}

	/* val[12-7]:0xa50 */
	ldpc_ite_val = (val >> 7) & 0x3f;
	if (ldpc_ite_val >= ldpc_ite_max)
		ret = 1;

	return ret;
}

static int dvbt2_read_status(struct dvb_frontend *fe, enum fe_status *status)
{
	int ilock;
	unsigned char s = 0;
	int strenth;
	int strength_limit = THRD_TUNER_STRENTH_DVBT;
	struct aml_dtvdemod *demod = (struct aml_dtvdemod *)fe->demodulator_priv;
	struct amldtvdemod_device_s *devp = (struct amldtvdemod_device_s *)demod->priv;

	if (!devp->demod_thread)
		return 0;

	strenth = tuner_get_ch_power(fe);
	if (devp->tuner_strength_limit)
		strength_limit = devp->tuner_strength_limit;

	if (strenth < strength_limit) {
		*status = FE_TIMEDOUT;
		demod->last_lock = -1;
		PR_DVBT("tuner:no signal! strength:%d\n", strenth);
		return 0;
	}

	s = amdemod_stat_islock(demod, SYS_DVBT2);

	if (s == 1) {
		ilock = 1;
		*status =
			FE_HAS_LOCK | FE_HAS_SIGNAL | FE_HAS_CARRIER |
			FE_HAS_VITERBI | FE_HAS_SYNC;
	} else {
		if (timer_not_enough(demod, D_TIMER_DETECT)) {
			ilock = 0;
			*status = 0;
		} else {
			ilock = 0;
			*status = FE_TIMEDOUT;
			timer_disable(demod, D_TIMER_DETECT);
		}
	}
	if (demod->last_lock != ilock) {
		if (*status == (FE_HAS_LOCK | FE_HAS_SIGNAL | FE_HAS_CARRIER |
		    FE_HAS_VITERBI | FE_HAS_SYNC)) {
			PR_INFO("%s [id %d]: !!  >> LOCK << !!, freq:%d\n",
					__func__, demod->id, fe->dtv_property_cache.frequency);
			demod->last_lock = ilock;
		} else if (*status == FE_TIMEDOUT) {
			PR_INFO("%s [id %d]: !!  >> UNLOCK << !!\n", __func__, demod->id);
			demod->last_lock = ilock;
		} else {
			PR_INFO("%s [id %d]: !!  >> WAIT << !!\n", __func__, demod->id);
		}
	}

	return 0;
}

static int dvbt_isdbt_read_ber(struct dvb_frontend *fe, u32 *ber)
{
	*ber = dvbt_get_status_ops()->get_ber() & 0xffff;
	return 0;
}

static int dvbt_read_ber(struct dvb_frontend *fe, u32 *ber)
{
	*ber = 0;
	return 0;
}

static int gxtv_demod_dvbt_read_signal_strength
	(struct dvb_frontend *fe, u16 *strength)
{
	struct aml_dtvdemod *demod = (struct aml_dtvdemod *)fe->demodulator_priv;
	struct amldtvdemod_device_s *devp = (struct amldtvdemod_device_s *)demod->priv;

	if (!devp->demod_thread)
		return 0;

	/**strength = 256 - tuner_get_ch_power(fe);*/

	*strength = tuner_get_ch_power3(fe);

	PR_DBGL("demod [id %d] tuner strength is %d dbm\n", demod->id, *strength);
	return 0;
}

static int dtvdemod_dvbs_read_signal_strength
	(struct dvb_frontend *fe, u16 *strength)
{
	struct aml_dtvdemod *demod = (struct aml_dtvdemod *)fe->demodulator_priv;
	struct amldtvdemod_device_s *devp = (struct amldtvdemod_device_s *)demod->priv;

	if (!devp->demod_thread)
		return 0;

	*strength = tuner_get_ch_power3(fe);

	PR_DBGL("demod [id %d] tuner strength is %d dbm\n", demod->id, *strength);

	return 0;
}

static int gxtv_demod_dvbt_isdbt_read_snr(struct dvb_frontend *fe, u16 *snr)
{
	struct aml_demod_sta demod_sta;
	struct aml_dtvdemod *demod = (struct aml_dtvdemod *)fe->demodulator_priv;
	struct amldtvdemod_device_s *devp = (struct amldtvdemod_device_s *)demod->priv;

	if (!devp->demod_thread)
		return 0;

	*snr = dvbt_get_status_ops()->get_snr(&demod_sta/*, &demod_i2c*/);
	*snr /= 8;
	PR_DBGL("demod [id %d] snr is %d dbm\n", demod->id, *snr);
	return 0;
}

static int dvbt_read_snr(struct dvb_frontend *fe, u16 *snr)
{
	struct aml_dtvdemod *demod = (struct aml_dtvdemod *)fe->demodulator_priv;
	struct amldtvdemod_device_s *devp = (struct amldtvdemod_device_s *)demod->priv;

	if (!devp->demod_thread)
		return 0;

	*snr = ((dvbt_t2_rdb(CHC_CIR_SNR1) & 0x7) << 8) | dvbt_t2_rdb(CHC_CIR_SNR0);
	*snr = *snr * 3 / 64;

	/* 1-34db, mappting to 0-100% */
	*snr = *snr * 100 / 34;

	if (*snr > 100)
		*snr = 100;

	PR_DVBT("demod [id %d] signal quality is %d%%\n", demod->id, *snr);

	return 0;
}

static int dvbt2_read_snr(struct dvb_frontend *fe, u16 *snr)
{
	unsigned int val;
	struct aml_dtvdemod *demod = (struct aml_dtvdemod *)fe->demodulator_priv;
	struct amldtvdemod_device_s *devp = (struct amldtvdemod_device_s *)demod->priv;

	if (!devp->demod_thread)
		return 0;

	val = front_read_reg(0x3e);
	/* val[23-21]:0x2a09,val[20-13]:0x2a08 */
	val >>= 13;
	*snr = val & 0x7ff;
	*snr = *snr * 3 / 64;

	/* 1-34db, mappting to 0-100% */
	*snr = *snr * 100 / 34;

	if (*snr > 100)
		*snr = 100;

	PR_DVBT("demod [id %d] signal quality is %d%%\n", demod->id, *snr);

	return 0;
}

static int dvbs_read_snr(struct dvb_frontend *fe, u16 *snr)
{
	struct aml_dtvdemod *demod = (struct aml_dtvdemod *)fe->demodulator_priv;
	struct amldtvdemod_device_s *devp = (struct amldtvdemod_device_s *)demod->priv;

	if (!devp->demod_thread)
		return 0;

	*snr = (dvbs_rd_byte(CNR_HIGH) << 8) | dvbs_rd_byte(CNR_LOW);
	PR_DVBS("demod [id %d] snr is %d dbm\n", demod->id, *snr);

	return 0;
}

static int gxtv_demod_dvbt_read_ucblocks(struct dvb_frontend *fe, u32 *ucblocks)
{
	*ucblocks = 0;
	return 0;
}

static enum fe_bandwidth dtvdemod_convert_bandwidth(unsigned int input)
{
	enum fe_bandwidth output = BANDWIDTH_AUTO;

	switch (input) {
	case 10000000:
		output = BANDWIDTH_10_MHZ;
		break;

	case 8000000:
		output = BANDWIDTH_8_MHZ;
		break;

	case 7000000:
		output = BANDWIDTH_7_MHZ;
		break;

	case 6000000:
		output = BANDWIDTH_6_MHZ;
		break;

	case 5000000:
		output = BANDWIDTH_5_MHZ;
		break;

	case 1712000:
		output = BANDWIDTH_1_712_MHZ;
		break;

	case 0:
		output = BANDWIDTH_AUTO;
		break;
	}

	return output;
}

static int dvbt_isdbt_set_frontend(struct dvb_frontend *fe)
{
	struct dtv_frontend_properties *c = &fe->dtv_property_cache;
	/*struct aml_demod_sts demod_sts;*/
	struct aml_demod_dvbt param;
	struct aml_dtvdemod *demod = (struct aml_dtvdemod *)fe->demodulator_priv;

	PR_INFO("%s [id %d]: delsys:%d, freq:%d, symbol_rate:%d, bw:%d, modul:%d, invert:%d.\n",
			__func__, demod->id, c->delivery_system, c->frequency, c->symbol_rate,
			c->bandwidth_hz, c->modulation, c->inversion);

	/* bw == 0 : 8M*/
	/*       1 : 7M*/
	/*       2 : 6M*/
	/*       3 : 5M*/
	/* agc_mode == 0: single AGC*/
	/*             1: dual AGC*/
	memset(&param, 0, sizeof(param));
	param.ch_freq = c->frequency / 1000;
	param.bw = dtvdemod_convert_bandwidth(c->bandwidth_hz);
	param.agc_mode = 1;
	/*ISDBT or DVBT : 0 is QAM, 1 is DVBT, 2 is ISDBT,*/
	/* 3 is DTMB, 4 is ATSC */
	param.dat0 = 1;
	demod->last_lock = -1;

	tuner_set_params(fe);
	dvbt_isdbt_set_ch(demod, &param);

	return 0;
}

static int dvbt_set_frontend(struct dvb_frontend *fe)
{
	struct dtv_frontend_properties *c = &fe->dtv_property_cache;
	/*struct aml_demod_sts demod_sts;*/
	struct aml_demod_dvbt param;
	struct aml_dtvdemod *demod = (struct aml_dtvdemod *)fe->demodulator_priv;

	PR_INFO("%s [id %d]: delsys:%d, freq:%d, symbol_rate:%d, bw:%d, modul:%d, invert:%d.\n",
			__func__, demod->id, c->delivery_system, c->frequency, c->symbol_rate,
			c->bandwidth_hz, c->modulation, c->inversion);

	memset(&param, 0, sizeof(param));
	param.ch_freq = c->frequency / 1000;
	demod->freq = c->frequency / 1000;
	param.bw = dtvdemod_convert_bandwidth(c->bandwidth_hz);
	param.agc_mode = 1;
	param.dat0 = 1;
	demod->last_lock = -1;

	tuner_set_params(fe);
	dvbt_set_ch(demod, &param);
	/* wait tuner stable */
	msleep(30);

	return 0;
}

static int dvbt2_set_frontend(struct dvb_frontend *fe)
{
	struct dtv_frontend_properties *c = &fe->dtv_property_cache;
	struct aml_dtvdemod *demod = (struct aml_dtvdemod *)fe->demodulator_priv;

	PR_INFO("%s [id %d]: delsys:%d, freq:%d, symbol_rate:%d, bw:%d, modul:%d, invert:%d.\n",
			__func__, demod->id, c->delivery_system, c->frequency, c->symbol_rate,
			c->bandwidth_hz, c->modulation, c->inversion);
	demod->bw = dtvdemod_convert_bandwidth(c->bandwidth_hz);
	demod->freq = c->frequency / 1000;
	demod->last_lock = -1;
	tuner_set_params(fe);
	dvbt2_set_ch(demod);
	/* wait tuner stable */
	msleep(30);

	return 0;
}

static int gxtv_demod_dvbt_get_frontend(struct dvb_frontend *fe)
{
	return 0;
}

int dvbt_isdbt_Init(struct aml_dtvdemod *demod)
{
	struct aml_demod_sys sys;
	struct amldtvdemod_device_s *devp = (struct amldtvdemod_device_s *)demod->priv;

	PR_DBG("AML Demod DVB-T/isdbt init\r\n");

	memset(&sys, 0, sizeof(sys));

	memset(&demod->demod_status, 0, sizeof(demod->demod_status));

	demod->demod_status.delsys = SYS_ISDBT;
	sys.adc_clk = ADC_CLK_24M;
	sys.demod_clk = DEMOD_CLK_60M;
	demod->demod_status.ch_if = SI2176_5M_IF * 1000;
	if (devp->data->hw_ver == DTVDEMOD_HW_T5D)
		dd_tvafe_hiu_reg_write(HHI_DEMOD_CLK_CNTL << 2, 0x507);
	else
		dd_tvafe_hiu_reg_write(HHI_DEMOD_CLK_CNTL << 2, 0x501);

	demod_set_sys(demod, &sys);

	return 0;
}

static unsigned int dvbt_init(struct aml_dtvdemod *demod)
{
	struct aml_demod_sys sys;
	struct amldtvdemod_device_s *devp = (struct amldtvdemod_device_s *)demod->priv;

	if (!devp) {
		pr_err("%s devp is NULL\n", __func__);
		return -EFAULT;
	}

	PR_DBG("AML Demod DVB-T init\r\n");

	memset(&sys, 0, sizeof(sys));

	memset(&demod->demod_status, 0, sizeof(demod->demod_status));

	demod->demod_status.delsys = SYS_DVBT;
	sys.adc_clk = ADC_CLK_54M;
	sys.demod_clk = DEMOD_CLK_216M;
	demod->demod_status.ch_if = SI2176_5M_IF * 1000;
	dd_tvafe_hiu_reg_write(HHI_DEMOD_CLK_CNTL1 << 2, 0x704);
	dd_tvafe_hiu_reg_write(HHI_DEMOD_CLK_CNTL << 2, 0x501);
	demod_set_sys(demod, &sys);

	return 0;
}

static unsigned int dtvdemod_dvbt2_init(struct aml_dtvdemod *demod)
{
	struct aml_demod_sys sys;

	PR_DBG("AML Demod DVB-T2 init\r\n");

	memset(&sys, 0, sizeof(sys));

	memset(&demod->demod_status, 0, sizeof(demod->demod_status));

	demod->demod_status.delsys = SYS_DVBT2;
	sys.adc_clk = ADC_CLK_54M;
	sys.demod_clk = DEMOD_CLK_216M;
	demod->demod_status.ch_if = SI2176_5M_IF * 1000;
	dd_tvafe_hiu_reg_write(HHI_DEMOD_CLK_CNTL1 << 2, 0x704);
	dd_tvafe_hiu_reg_write(HHI_DEMOD_CLK_CNTL << 2, 0x501);
	demod_set_sys(demod, &sys);

	return 0;
}

static void gxtv_demod_atsc_release(struct dvb_frontend *fe)
{

}

static enum dvbfe_algo gxtv_demod_get_frontend_algo(struct dvb_frontend *fe)
{
	return DVBFE_ALGO_HW;
}

unsigned  int ats_thread_flg;
static int gxtv_demod_atsc_read_status
	(struct dvb_frontend *fe, enum fe_status *status)
{
	struct aml_dtvdemod *demod = (struct aml_dtvdemod *)fe->demodulator_priv;
	struct amldtvdemod_device_s *devp = (struct amldtvdemod_device_s *)demod->priv;
	struct dtv_frontend_properties *c = &fe->dtv_property_cache;
	struct aml_demod_sts demod_sts;
	int ilock;
	unsigned char s = 0;
	int strength = 0;
	/*debug only*/

	if (!devp->demod_thread) {
		ilock = 1;
		*status =
			FE_HAS_LOCK | FE_HAS_SIGNAL | FE_HAS_CARRIER |
			FE_HAS_VITERBI | FE_HAS_SYNC;
		return 0;
	}
	if (!get_dtvpll_init_flag())
		return 0;

	if ((c->modulation <= QAM_AUTO) && (c->modulation != QPSK)) {
		s = amdemod_stat_islock(demod, SYS_DVBC_ANNEX_A);
		dvbc_status(demod, &demod_sts);
	} else if (c->modulation > QAM_AUTO) {
		/*atsc_thread();*/
		s = amdemod_stat_islock(demod, SYS_ATSC);

		if (!cpu_after_eq(MESON_CPU_MAJOR_ID_TL1)) {
			if (s == 0 && demod->last_lock == 1 &&
				(atsc_read_reg(0x0980) >= 0x76)) {
				s = 1;
				PR_ATSC("[rsj] unlock,but fsm >= 0x76\n");
			}
		}
	}

	if (s == 1) {
		ilock = 1;
		*status =
			FE_HAS_LOCK | FE_HAS_SIGNAL | FE_HAS_CARRIER |
			FE_HAS_VITERBI | FE_HAS_SYNC;
	} else {
		ilock = 0;
		if (cpu_after_eq(MESON_CPU_MAJOR_ID_TL1)) {
			if (timer_not_enough(demod, D_TIMER_DETECT)) {
				*status = 0;
				PR_DBG("s=0\n");
			} else {
				*status = FE_TIMEDOUT;
				timer_disable(demod, D_TIMER_DETECT);
			}
		} else {
			*status = FE_TIMEDOUT;
		}
		#if 0
		if (ats_thread_flg)
			*status = FE_TIMEDOUT;
		else
			*status = 0;
		#endif

	}

	if (demod->last_lock != ilock) {
		PR_INFO("%s [id %d]: %s.\n", __func__, demod->id,
			ilock ? "!!  >> LOCK << !!" : "!! >> UNLOCK << !!");
		demod->last_lock = ilock;
	}

	if (aml_demod_debug & DBG_ATSC) {
		if (demod->atsc_dbg_lst_status != s || demod->last_lock != ilock) {
			/* check tuner */
			strength = tuner_get_ch_power2(fe);

			PR_ATSC("s=%d(1 is lock),lock=%d\n", s, ilock);
			PR_ATSC("[rsj_test]freq[%d] strength[%d]\n",
					demod->freq, strength);

			/*update */
			demod->atsc_dbg_lst_status = s;
			demod->last_lock = ilock;
		}
		/*aml_dbgatscl(".");*/
	}
	return 0;
}

static int gxtv_demod_atsc_read_ber(struct dvb_frontend *fe, u32 *ber)
{
	struct dtv_frontend_properties *c = &fe->dtv_property_cache;
	struct aml_dtvdemod *demod = (struct aml_dtvdemod *)fe->demodulator_priv;

	if (!get_dtvpll_init_flag())
		return 0;

	if (c->modulation > QAM_AUTO) {
		if (cpu_after_eq(MESON_CPU_MAJOR_ID_TL1))
			*ber = atsc_read_reg_v4(ATSC_FEC_BER);
		else
			*ber = atsc_read_reg(0x980) & 0xffff;
	} else if ((c->modulation == QAM_256) || (c->modulation == QAM_64)) {
		*ber = dvbc_get_status(demod);
	}

	return 0;
}

static int gxtv_demod_atsc_read_signal_strength
	(struct dvb_frontend *fe, u16 *strength)
{
	int strenth;
	struct aml_dtvdemod *demod = (struct aml_dtvdemod *)fe->demodulator_priv;
	struct amldtvdemod_device_s *devp = (struct amldtvdemod_device_s *)demod->priv;

	strenth = tuner_get_ch_power(fe);
/*	struct aml_fe *afe = (struct aml_dtvdemod *)fe->demodulator_priv;*/
	/*struct aml_fe_dev *dev = afe->dtv_demod; */
	if (devp->pin_name &&
	    (strncmp(fe->ops.tuner_ops.info.name, "r842", 4) == 0)) {
		if ((fe->dtv_property_cache.modulation <= QAM_AUTO) &&
			(fe->dtv_property_cache.modulation != QPSK))
			strenth += 18;
		else {
			strenth += 15;
			if (strenth <= -80)
				strenth = atsc_get_power_strength(
					atsc_read_reg_v4(0x44) & 0xfff,
						strenth);
		}

		if (strenth < -100)
			*strength = 0;
		else
			*strength = strenth + 100;
	} else
		*strength = tuner_get_ch_power3(fe);

	if (*strength > 100)
		*strength = 100;

	return 0;
}

static int gxtv_demod_atsc_read_snr(struct dvb_frontend *fe, u16 *snr)
{
	struct aml_dtvdemod *demod = (struct aml_dtvdemod *)fe->demodulator_priv;
	struct dtv_frontend_properties *c = &fe->dtv_property_cache;
	struct aml_demod_sts demod_sts;

	if ((c->modulation <= QAM_AUTO)	&& (c->modulation != QPSK)) {
		dvbc_status(demod, &demod_sts);
		*snr = demod_sts.ch_snr / 100;
	} else if (c->modulation > QAM_AUTO) {
		*snr = atsc_read_snr();
	}

	return 0;
}

static int gxtv_demod_atsc_read_ucblocks(struct dvb_frontend *fe, u32 *ucblocks)
{
	struct dtv_frontend_properties *c = &fe->dtv_property_cache;
	struct aml_dtvdemod *demod = (struct aml_dtvdemod *)fe->demodulator_priv;
	struct amldtvdemod_device_s *devp = (struct amldtvdemod_device_s *)demod->priv;

	if (!devp->demod_thread)
		return 0;

	if (c->modulation > QAM_AUTO)
		atsc_thread();

	*ucblocks = 0;

	return 0;
}

static int gxtv_demod_atsc_set_frontend(struct dvb_frontend *fe)
{
	struct aml_dtvdemod *demod = (struct aml_dtvdemod *)fe->demodulator_priv;
	struct amldtvdemod_device_s *devp = (struct amldtvdemod_device_s *)demod->priv;
	struct dtv_frontend_properties *c = &fe->dtv_property_cache;
	struct aml_demod_atsc param_atsc;
	struct aml_demod_dvbc param_j83b;
	int temp_freq = 0;
	union ATSC_DEMOD_REG_0X6A_BITS Val_0x6a;
	union atsc_cntl_reg_0x20 val;
	int nco_rate;
	/*[0]: specturm inverse(1),normal(0); [1]:if_frequency*/
	unsigned int tuner_freq[2] = {0};
	enum fe_delivery_system delsys = demod->last_delsys;

	PR_INFO("%s [id %d]: delsys:%d, freq:%d, symbol_rate:%d, bw:%d, modul:%d, invert:%d.\n",
			__func__, demod->id, c->delivery_system, c->frequency, c->symbol_rate,
			c->bandwidth_hz, c->modulation, c->inversion);

	memset(&param_atsc, 0, sizeof(param_atsc));
	memset(&param_j83b, 0, sizeof(param_j83b));
	if (!devp->demod_thread)
		return 0;

	demod->freq = c->frequency / 1000;
	demod->last_lock = -1;
	demod->atsc_mode = c->modulation;
	tuner_set_params(fe);

	if ((c->modulation <= QAM_AUTO) && (c->modulation != QPSK)) {
		if (cpu_after_eq(MESON_CPU_MAJOR_ID_TL1)) {
			/* add for j83b 256 qam, livetv and timeshif */
			demod->demod_status.clk_freq = DEMOD_CLK_167M;
			nco_rate = (demod->demod_status.adc_freq * 256) /
					demod->demod_status.clk_freq + 2;
			front_write_bits(AFIFO_ADC, nco_rate,
					 AFIFO_NCO_RATE_BIT, AFIFO_NCO_RATE_WID);
			front_write_reg(SFIFO_OUT_LENS, 0x5);
			/* sys clk = 167M */
			dd_tvafe_hiu_reg_write(D_HHI_DEMOD_CLK_CNTL, 0x502);
		} else {
			/* txlx j.83b set sys clk to 222M */
			dd_tvafe_hiu_reg_write(D_HHI_DEMOD_CLK_CNTL, 0x502);
		}

		demod_set_mode_ts(SYS_DVBC_ANNEX_A);
		param_j83b.ch_freq = c->frequency / 1000;
		param_j83b.mode = amdemod_qam(c->modulation);
		PR_ATSC("gxtv_demod_atsc_set_frontend, modulation: %d\n", c->modulation);
		if (c->modulation == QAM_64)
			param_j83b.symb_rate = 5057;
		else if (c->modulation == QAM_256)
			param_j83b.symb_rate = 5361;
		else
			param_j83b.symb_rate = 5361;

		dvbc_set_ch(demod, &param_j83b);

		if (cpu_after_eq(MESON_CPU_MAJOR_ID_TL1)) {
			qam_write_reg(demod, 0x7, 0x10f33);
			set_j83b_filter_reg_v4(demod);
			qam_write_reg(demod, 0x12, 0x50e1000);
			qam_write_reg(demod, 0x30, 0x41f2f69);
		}
	} else if (c->modulation > QAM_AUTO) {
		if (fe->ops.tuner_ops.get_if_frequency)
			fe->ops.tuner_ops.get_if_frequency(fe, tuner_freq);
		if (cpu_after_eq(MESON_CPU_MAJOR_ID_TL1)) {
			Val_0x6a.bits = atsc_read_reg_v4(ATSC_DEMOD_REG_0X6A);
			Val_0x6a.b.peak_thd = 0x6;//Let CCFO Quality over 6
			atsc_write_reg_v4(ATSC_DEMOD_REG_0X6A, Val_0x6a.bits);
			atsc_write_reg_v4(ATSC_EQ_REG_0XA5, 0x8c);
			/* bit30: enable CCI */
			atsc_write_reg_v4(ATSC_EQ_REG_0X92, 0x40000240);
			/* bit0~3: AGC bandwidth select */
			atsc_write_reg_v4(ATSC_DEMOD_REG_0X58, 0x528220d);
			/* clk recover confidence control */
			atsc_write_reg_v4(ATSC_DEMOD_REG_0X5D, 0x14400202);
			/* CW bin frequency */
			atsc_write_reg_v4(ATSC_DEMOD_REG_0X61, 0x2ee);

			/*bit 2: invert specturm, for r842 tuner AGC control*/
			if (tuner_freq[0] == 1)
				atsc_write_reg_v4(ATSC_DEMOD_REG_0X56, 0x4);
			else
				atsc_write_reg_v4(ATSC_DEMOD_REG_0X56, 0x0);

			if (demod->demod_status.adc_freq == ADC_CLK_24M) {
				atsc_write_reg_v4(ATSC_DEMOD_REG_0X54,
					0x1aaaaa);

				atsc_write_reg_v4(ATSC_DEMOD_REG_0X55,
					0x3ae28d);

				atsc_write_reg_v4(ATSC_DEMOD_REG_0X6E,
					0x16e3600);
			}

			/*for timeshift mosaic issue*/
			atsc_write_reg_v4(0x12, 0x18);
			val.bits = atsc_read_reg_v4(ATSC_CNTR_REG_0X20);
			val.b.cpu_rst = 1;
			atsc_write_reg_v4(ATSC_CNTR_REG_0X20, val.bits);
			usleep_range(1000, 1001);
			val.b.cpu_rst = 0;
			atsc_write_reg_v4(ATSC_CNTR_REG_0X20, val.bits);
			usleep_range(5000, 5001);
		} else {
			/*demod_set_demod_reg(0x507, TXLX_ADC_REG6);*/
			dd_tvafe_hiu_reg_write(D_HHI_DEMOD_CLK_CNTL, 0x507);
			demod_set_mode_ts(delsys);
			param_atsc.ch_freq = c->frequency / 1000;
			param_atsc.mode = c->modulation;
			atsc_set_ch(demod, &param_atsc);

			/* bit 2: invert specturm, 0:normal, 1:invert */
			if (tuner_freq[0] == 1)
				atsc_write_reg(0x716, atsc_read_reg(0x716) | 0x4);
			else
				atsc_write_reg(0x716, atsc_read_reg(0x716) & ~0x4);
		}
	}

	if ((auto_search_std == 1) && ((c->modulation <= QAM_AUTO)
	&& (c->modulation != QPSK))) {
		unsigned char s = 0;

		msleep(std_lock_timeout);
		s = amdemod_stat_islock(demod, SYS_DVBC_ANNEX_A);
		if (s == 1) {
			PR_DBG("atsc std mode is %d locked\n", demod->atsc_mode);

			return 0;
		}
		if ((c->frequency == 79000000) || (c->frequency == 85000000)) {
			temp_freq = (c->frequency + 2000000) / 1000;
			param_j83b.ch_freq = temp_freq;
			PR_DBG("irc fre:%d\n", param_j83b.ch_freq);
			c->frequency = param_j83b.ch_freq * 1000;

			tuner_set_params(fe);
			demod_set_mode_ts(SYS_DVBC_ANNEX_A);
			param_j83b.mode = amdemod_qam(c->modulation);
			if (c->modulation == QAM_64)
				param_j83b.symb_rate = 5057;
			else if (c->modulation == QAM_256)
				param_j83b.symb_rate = 5361;
			else
				param_j83b.symb_rate = 5361;
			dvbc_set_ch(demod, &param_j83b);

			msleep(std_lock_timeout);
			s = amdemod_stat_islock(demod, SYS_DVBC_ANNEX_A);
			if (s == 1) {
				PR_DBG("irc mode is %d locked\n", demod->atsc_mode);
			} else {
				temp_freq = (c->frequency - 1250000) / 1000;
				param_j83b.ch_freq = temp_freq;
				PR_DBG("hrc fre:%d\n", param_j83b.ch_freq);
				c->frequency = param_j83b.ch_freq * 1000;

				tuner_set_params(fe);
				demod_set_mode_ts(SYS_DVBC_ANNEX_A);
				param_j83b.mode = amdemod_qam(c->modulation);
				if (c->modulation == QAM_64)
					param_j83b.symb_rate = 5057;
				else if (c->modulation == QAM_256)
					param_j83b.symb_rate = 5361;
				else
					param_j83b.symb_rate = 5361;
				dvbc_set_ch(demod, &param_j83b);
			}
		} else {
			param_j83b.ch_freq = (c->frequency - 1250000) / 1000;
			PR_DBG("hrc fre:%d\n", param_j83b.ch_freq);
			c->frequency = param_j83b.ch_freq * 1000;

			demod_set_mode_ts(SYS_DVBC_ANNEX_A);
			param_j83b.mode = amdemod_qam(c->modulation);
			if (c->modulation == QAM_64)
				param_j83b.symb_rate = 5057;
			else if (c->modulation == QAM_256)
				param_j83b.symb_rate = 5361;
			else
				param_j83b.symb_rate = 5361;
			dvbc_set_ch(demod, &param_j83b);
		}
	}
	PR_DBG("atsc_mode is %d\n", demod->atsc_mode);
	return 0;
}

static int gxtv_demod_atsc_get_frontend(struct dvb_frontend *fe)
{                               /*these content will be writed into eeprom .*/
	struct dtv_frontend_properties *c = &fe->dtv_property_cache;

	PR_DBG("c->frequency is %d\n", c->frequency);
	return 0;
}

void atsc_detect_first(struct dvb_frontend *fe, enum fe_status *status)
{
	unsigned int ucblocks;
	unsigned int atsc_status;
	enum fe_status s;
	int strenth;
	int cnt;
	int check_ok;
	struct aml_dtvdemod *demod = (struct aml_dtvdemod *)fe->demodulator_priv;
	struct amldtvdemod_device_s *devp = (struct amldtvdemod_device_s *)demod->priv;

	/*tuner strength*/
	if (dvb_tuner_delay > 9)
		msleep(dvb_tuner_delay);

	strenth = tuner_get_ch_power(fe);

	/*agc control,fine tune strength*/
	if (devp->pin_name &&
	    (strncmp(fe->ops.tuner_ops.info.name, "r842", 4) == 0)) {
		strenth += 15;
		if (strenth <= -80)
			strenth = atsc_get_power_strength(
				atsc_read_reg_v4(0x44) & 0xfff, strenth);
	}

	if (strenth < THRD_TUNER_STRENTH_ATSC) {
		*status = FE_TIMEDOUT;
		PR_ATSC("tuner:no signal!\n");
		return;
	}

	#define CNT_FIRST_ATSC  (2)
	check_ok = 0;

	for (cnt = 0; cnt < CNT_FIRST_ATSC; cnt++) {
		if (!cpu_after_eq(MESON_CPU_MAJOR_ID_TL1))
			gxtv_demod_atsc_read_ucblocks(fe, &ucblocks);

		gxtv_demod_atsc_read_status(fe, &s);

		if (cpu_after_eq(MESON_CPU_MAJOR_ID_TL1)) {
			*status = s;
			break;
		}

		//	*status = s;

		if (s != 0x1f) {
			gxtv_demod_atsc_read_ber(fe, &atsc_status);
			if ((atsc_status < 0x60)) {
				*status = FE_TIMEDOUT;
				check_ok = 1;
			}
		} else {
			check_ok = 1;
			*status = s;
		}

		if (check_ok)
			break;

	}

	PR_DBGL("%s,detect=0x%x,cnt=%d\n", __func__, (unsigned int)*status, cnt);
}


static int dvb_j83b_count = 5;
module_param(dvb_j83b_count, int, 0644);
MODULE_PARM_DESC(dvb_atsc_count, "dvb_j83b_count");
/*come from j83b_speedup_func*/

static int atsc_j83b_detect_first(struct dvb_frontend *fe, enum fe_status *s)
{
	int j83b_status, i;
	/*struct dvb_frontend_private *fepriv = fe->frontend_priv;*/
	struct dtv_frontend_properties *c = &fe->dtv_property_cache;
	struct aml_dtvdemod *demod = (struct aml_dtvdemod *)fe->demodulator_priv;
	struct amldtvdemod_device_s *devp = (struct amldtvdemod_device_s *)demod->priv;
	int strenth;
	enum fe_status cs;
	int cnt;
	int check_ok;

	j83b_status = 0;

	/*tuner:*/
	if (dvb_tuner_delay > 9)
		msleep(dvb_tuner_delay);

	strenth = tuner_get_ch_power(fe);

	/*agc control,fine tune strength*/
	if (devp->pin_name &&
	    (strncmp(fe->ops.tuner_ops.info.name, "r842", 4) == 0))
		strenth += 18;

	if (strenth < THRD_TUNER_STRENTH_J83) {
		*s = FE_TIMEDOUT;
		PR_ATSC("tuner:no signal!j83\n");
		return 0;
	}
	check_ok = 0;

	/*first check signal max time*/
	#define CNT_FIRST  (10)

	for (cnt = 0; cnt < CNT_FIRST; cnt++) {
		gxtv_demod_atsc_read_status(fe, &cs);

		if (cs != 0x1f) {
			/*msleep(200);*/
			PR_DBG("[j.83b] 1\n");
			for (i = 0; i < dvb_j83b_count; i++) {
				msleep(25);
				gxtv_demod_atsc_read_ber(fe, &j83b_status);

				/*J.83 status >=0x38,has signal*/
				if (j83b_status >= 0x3)
					break;
			}
			PR_DBG("[rsj]j.83b_status is %x,modulation is %d\n",
					j83b_status,
					c->modulation);

			if (j83b_status < 0x3) {
				*s = FE_TIMEDOUT;
				check_ok = 1;
			}

		} else {
			/*have signal*/
			*s = cs;
			check_ok = 1;
		}

		if (check_ok)
			break;

		msleep(50);
	}


	if (!check_ok)
		*s = FE_TIMEDOUT;

	return 0;
}

static int atsc_j83b_polling(struct dvb_frontend *fe, enum fe_status *s)
{
	int j83b_status, i;
	/*struct dvb_frontend_private *fepriv = fe->frontend_priv;*/
	struct dtv_frontend_properties *c = &fe->dtv_property_cache;
	int strenth;

	j83b_status = 0;
	strenth = tuner_get_ch_power(fe);
	if (strenth < THRD_TUNER_STRENTH_J83) {
		*s = FE_TIMEDOUT;
		PR_DBGL("tuner:no signal!j83\n");
		return 0;
	}

	gxtv_demod_atsc_read_status(fe, s);

	if (*s != 0x1f) {
		/*msleep(200);*/
		PR_DBG("[j.83b] 1\n");
		for (i = 0; i < dvb_j83b_count; i++) {
			msleep(25);
			gxtv_demod_atsc_read_ber(fe, &j83b_status);

			/*J.83 status >=0x38,has signal*/
			if (j83b_status >= 0x3)
				break;
		}
		PR_DBG("[rsj]j.83b_status is %x,modulation is %d\n",
				j83b_status,
				c->modulation);

		if (j83b_status < 0x3)
			*s = FE_TIMEDOUT;
	}




	return 0;
}

void atsc_polling(struct dvb_frontend *fe, enum fe_status *status)
{
	struct aml_dtvdemod *demod = (struct aml_dtvdemod *)fe->demodulator_priv;
	struct amldtvdemod_device_s *devp = (struct amldtvdemod_device_s *)demod->priv;
	struct dtv_frontend_properties *c = &fe->dtv_property_cache;

	if (!devp->demod_thread)
		return;

	if (c->modulation == QPSK) {
		PR_DBG("mode is qpsk, return;\n");
		/*return;*/
	} else if (c->modulation <= QAM_AUTO) {
		atsc_j83b_polling(fe, status);
	} else {
		atsc_thread();
		gxtv_demod_atsc_read_status(fe, status);
	}



}

static int gxtv_demod_atsc_tune(struct dvb_frontend *fe, bool re_tune,
	unsigned int mode_flags, unsigned int *delay, enum fe_status *status)
{
	/*
	 * It is safe to discard "params" here, as the DVB core will sync
	 * fe->dtv_property_cache with fepriv->parameters_in, where the
	 * DVBv3 params are stored. The only practical usage for it indicate
	 * that re-tuning is needed, e. g. (fepriv->state & FESTATE_RETUNE) is
	 * true.
	 */
	struct aml_dtvdemod *demod = (struct aml_dtvdemod *)fe->demodulator_priv;
	struct amldtvdemod_device_s *devp = (struct amldtvdemod_device_s *)demod->priv;
	struct dtv_frontend_properties *c = &fe->dtv_property_cache;

	*delay = HZ / 8;

	if (!devp->demod_thread)
		return 0;

	if (re_tune) {
		demod->en_detect = 1; /*fist set*/
		gxtv_demod_atsc_set_frontend(fe);

		if (cpu_after_eq(MESON_CPU_MAJOR_ID_TL1))
			timer_begain(demod, D_TIMER_DETECT);

		if (c->modulation == QPSK) {
			PR_ATSC("[id %d] modulation is QPSK do nothing!", demod->id);
		} else if (c->modulation <= QAM_AUTO) {
			PR_ATSC("[id %d] detect modulation is j83 first.\n", demod->id);
			atsc_j83b_detect_first(fe, status);
		} else if (c->modulation > QAM_AUTO) {
			PR_ATSC("[id %d] modulation is AUTO.\n", demod->id);
			atsc_detect_first(fe, status);
		} else {
			PR_ATSC("[id %d] modulation is %d unsupported!\n",
					demod->id, c->modulation);
		}

		return 0;
	}
#if 1
	if (!demod->en_detect) {
		PR_DBGL("[id %d] tune:not enable\n", demod->id);
		return 0;
	}
#endif
	if (cpu_after_eq(MESON_CPU_MAJOR_ID_TL1)) {
		if (c->modulation > QAM_AUTO)
			atsc_detect_first(fe, status);
		else if (c->modulation <= QAM_AUTO &&
			(c->modulation !=  QPSK))
			atsc_j83b_detect_first(fe, status);
	} else {
		atsc_polling(fe, status);
	}

	return 0;
}

static int dvbt_isdbt_tune(struct dvb_frontend *fe, bool re_tune,
	unsigned int mode_flags, unsigned int *delay, enum fe_status *status)
{
	/*
	 * It is safe to discard "params" here, as the DVB core will sync
	 * fe->dtv_property_cache with fepriv->parameters_in, where the
	 * DVBv3 params are stored. The only practical usage for it indicate
	 * that re-tuning is needed, e. g. (fepriv->state & FESTATE_RETUNE) is
	 * true.
	 */
	struct aml_dtvdemod *demod = (struct aml_dtvdemod *)fe->demodulator_priv;

	*delay = HZ/2;

	/*PR_ATSC("%s:\n", __func__);*/
#if 1
	if (re_tune) {

		timer_begain(demod, D_TIMER_DETECT);
		PR_INFO("%s [id %d]: re_tune.\n", __func__, demod->id);
		demod->en_detect = 1; /*fist set*/

		dvbt_isdbt_set_frontend(fe);
		dvbt_isdbt_read_status(fe, status);
		return 0;
	}
#endif
	if (!demod->en_detect) {
		PR_DBGL("[id %d] tune:not enable\n", demod->id);
		return 0;
	}
	/*polling*/
	dvbt_isdbt_read_status(fe, status);

	if (*status & FE_HAS_LOCK) {
		timer_disable(demod, D_TIMER_SET);
	} else {
		if (!timer_is_en(demod, D_TIMER_SET))
			timer_begain(demod, D_TIMER_SET);
	}

	if (timer_is_enough(demod, D_TIMER_SET)) {
		dvbt_isdbt_set_frontend(fe);
		timer_disable(demod, D_TIMER_SET);
	}

	return 0;
}

static void dvbt_rst_demod(struct aml_dtvdemod *demod)
{
	struct amldtvdemod_device_s *devp = (struct amldtvdemod_device_s *)demod->priv;

	if (devp->data->hw_ver >= DTVDEMOD_HW_T5D) {
		demod_top_write_reg(DEMOD_TOP_REGC, 0x11);
		demod_top_write_reg(DEMOD_TOP_REGC, 0x110011);
		demod_top_write_reg(DEMOD_TOP_REGC, 0x110010);
		usleep_range(1000, 1001);
		demod_top_write_reg(DEMOD_TOP_REGC, 0x110011);
		demod_top_write_reg(DEMOD_TOP_CFG_REG_4, 0x0);
		front_write_bits(AFIFO_ADC, 0x80, AFIFO_NCO_RATE_BIT,
				 AFIFO_NCO_RATE_WID);
		front_write_reg(0x22, 0x7200a06);
		front_write_reg(0x2f, 0x0);
		front_write_reg(0x39, 0x40001000);
		demod_top_write_reg(DEMOD_TOP_CFG_REG_4, 0x182);
	}

	dvbt_reg_initial(demod->bw);
}

static int dvbt_tune(struct dvb_frontend *fe, bool re_tune,
	unsigned int mode_flags, unsigned int *delay, enum fe_status *status)
{
	struct aml_dtvdemod *demod = (struct aml_dtvdemod *)fe->demodulator_priv;
	struct amldtvdemod_device_s *devp = (struct amldtvdemod_device_s *)demod->priv;
	unsigned int fsm;

	if (unlikely(!devp))
		return -1;

	*delay = HZ / 5;

	if (!devp->demod_thread)
		return 0;

	if (re_tune) {
		PR_INFO("%s [id %d]: re_tune.\n", __func__, demod->id);
		demod->en_detect = 1; /*fist set*/

		dvbt_set_frontend(fe);
		timer_begain(demod, D_TIMER_DETECT);
		dvbt_read_status(fe, status);
		demod->t_cnt = 1;
		return 0;
	}

	if (!demod->en_detect) {
		PR_DBGL("[id %d] tune:not enable\n", demod->id);
		return 0;
	}

	/*polling*/
	dvbt_read_status(fe, status);

	dvbt_info(demod, NULL);

	/* GID lock */
	if (dvbt_t2_rdb(0x2901) >> 5 & 0x1)
		/* reduce agc target to default, otherwise will influence on snr */
		dvbt_t2_wrb(0x15d6, 0x50);
	else
		/* increase agc target to make signal strong enouth for locking */
		dvbt_t2_wrb(0x15d6, 0xa0);

	if (*status ==
	    (FE_HAS_LOCK | FE_HAS_SIGNAL | FE_HAS_CARRIER | FE_HAS_VITERBI | FE_HAS_SYNC)) {
		dvbt_t2_wr_byte_bits(0x2906, 2, 3, 4);
		dvbt_t2_wrb(0x2815, 0x02);
	} else {
		dvbt_t2_wr_byte_bits(0x2906, 0, 3, 4);
		dvbt_t2_wrb(0x2815, 0x03);
	}

	fsm = dvbt_t2_rdb(DVBT_STATUS);

	if ((demod->t_cnt == 2 && (fsm & 0xf) < 9) ||
	    (demod->t_cnt == 5 && (fsm & 0xf) == 9 && (fsm >> 6 & 1) && (*status != 0x1f))) {
		dvbt_rst_demod(demod);
		demod->t_cnt = 0;
		PR_INFO("[id %d] rst, tps or ts unlock\n", demod->id);
	}

	demod->t_cnt++;

	return 0;
}

static int dvbt2_tune(struct dvb_frontend *fe, bool re_tune,
	unsigned int mode_flags, unsigned int *delay, enum fe_status *status)
{
	unsigned int val, snr;
	struct aml_dtvdemod *demod = (struct aml_dtvdemod *)fe->demodulator_priv;
	struct amldtvdemod_device_s *devp = (struct amldtvdemod_device_s *)demod->priv;

	*delay = HZ / 8;

	if (!devp->demod_thread)
		return 0;

	if (re_tune) {
		PR_INFO("%s [id %d]: re_tune.\n", __func__, demod->id);
		demod->en_detect = 1; /*fist set*/

		dvbt2_set_frontend(fe);
		timer_begain(demod, D_TIMER_DETECT);
		dvbt2_read_status(fe, status);
		/* return directly, cnt increase 1 */
		demod->t2_cnt = 1;
		demod->t2_timeout_cnt = 1;
		demod->t2_prt_sts_cnt = 1;
		return 0;
	}

	if (!demod->en_detect) {
		PR_DBGL("[id %d] tune:not enable\n", demod->id);
		return 0;
	}

	/*polling*/
	dvbt2_read_status(fe, status);
	if (devp->debug_on && !(demod->t2_prt_sts_cnt++ % 3)) {
		demod_top_write_reg(DEMOD_TOP_CFG_REG_4, 0x182);
		dvbt2_info(NULL);
		demod_top_write_reg(DEMOD_TOP_CFG_REG_4, 0x0);
	}

	val = front_read_reg(0x3e);
	snr = val >> 13;
	snr &= 0x7ff;
	snr = snr * 3 / 64;

	/* no any err more than 1s,also need rst */
	if (*status == 0x1f) {
		demod->t2_timeout_cnt = 0;
	} else if (demod->t2_timeout_cnt++ >= 8) {
		dtvdemod_reset_fw(demod);
		demod->t2_timeout_cnt = 1;
		demod->t2_cnt = 1;
		PR_INFO("[id %d] rst fw 1 s timeout, LDPC: %d, snr: %ddb\n",
				demod->id, ((val >> 7) & 0x3f), snr);
		return 0;
	}

	/* LDPC = 1 and deinterleave has error
	 * val[30-24]:0x805,[23-21]:0x2a09,[20-13]:0x2a08,[12:7]:0xa50
	 * val[6-2]:0x8c3,[1]:0x361b[7],[0]:0x581[7],
	 */
	if ((((val >> 7) & 0x3f) == 1)) {
		if ((val >> 1) & 1) {
			dtvdemod_reset_fw(demod);
			demod->t2_timeout_cnt = 1;
			PR_INFO("[id %d] rst fw, LDPC: %d, snr: %ddb\n",
					demod->id, ((val >> 7) & 0x3f), snr);
		}

		demod->t2_cnt = 0;
	}

	/* 500ms interval to check err */
	if ((demod->t2_cnt++) % 4)
		return 0;

	if (dvbt2_check_ldpc_ite_max(val)) {
		dtvdemod_reset_fw(demod);
		demod->t2_cnt = 1;
		demod->t2_timeout_cnt = 1;
		PR_INFO("[id %d] rst fw, LDPC: %d, snr: %ddb\n",
				demod->id,  ((val >> 7) & 0x3f), snr);
	}

	return 0;
}

static int dtvdemod_atsc_init(struct aml_dtvdemod *demod)
{
	struct aml_demod_sys sys;
	struct dtv_frontend_properties *c = &demod->frontend.dtv_property_cache;
	struct amldtvdemod_device_s *devp = (struct amldtvdemod_device_s *)demod->priv;

	PR_DBG("%s\n", __func__);

	memset(&sys, 0, sizeof(sys));
	memset(&demod->demod_status, 0, sizeof(demod->demod_status));
	demod->demod_status.delsys = SYS_ATSC;
	sys.adc_clk = ADC_CLK_24M;

	PR_ATSC("c->modulation : %d\n", c->modulation);
	if (cpu_after_eq(MESON_CPU_MAJOR_ID_TL1))
		sys.demod_clk = DEMOD_CLK_250M;
	else
		sys.demod_clk = DEMOD_CLK_225M;

	demod->demod_status.ch_if = 5000;
	demod->demod_status.tmp = ADC_MODE;
	if (devp->data->hw_ver >= DTVDEMOD_HW_TL1)
		dd_tvafe_hiu_reg_write(HHI_DEMOD_CLK_CNTL << 2, 0x501);
	demod_set_sys(demod, &sys);

	return 0;
}

static void gxtv_demod_dtmb_release(struct dvb_frontend *fe)
{
}

static int gxtv_demod_dtmb_read_status
	(struct dvb_frontend *fe, enum fe_status *status)
{
	struct aml_dtvdemod *demod = (struct aml_dtvdemod *)fe->demodulator_priv;
	struct poll_machie_s *pollm = &demod->poll_machie;

	*status = pollm->last_s;

	return 0;
}

void dtmb_save_status(struct aml_dtvdemod *demod, unsigned int s)
{
	struct poll_machie_s *pollm = &demod->poll_machie;

	if (s) {
		pollm->last_s =
			FE_HAS_LOCK | FE_HAS_SIGNAL | FE_HAS_CARRIER |
			FE_HAS_VITERBI | FE_HAS_SYNC;

	} else {
		pollm->last_s = FE_TIMEDOUT;
	}
}

void dtmb_poll_start(struct aml_dtvdemod *demod)
{
	struct poll_machie_s *pollm = &demod->poll_machie;

	pollm->last_s = 0;
	pollm->flg_restart = 1;
	PR_DTMB("dtmb_poll_start2\n");
}

void dtmb_poll_stop(struct aml_dtvdemod *demod)
{
	struct poll_machie_s *pollm = &demod->poll_machie;

	pollm->flg_stop = 1;
}

void dtmb_set_delay(struct aml_dtvdemod *demod, unsigned int delay)
{
	struct poll_machie_s *pollm = &demod->poll_machie;

	pollm->delayms = delay;
	pollm->flg_updelay = 1;
}

unsigned int dtmb_is_update_delay(struct aml_dtvdemod *demod)
{
	struct poll_machie_s *pollm = &demod->poll_machie;

	return pollm->flg_updelay;
}

unsigned int dtmb_get_delay_clear(struct aml_dtvdemod *demod)
{
	struct poll_machie_s *pollm = &demod->poll_machie;

	pollm->flg_updelay = 0;

	return pollm->delayms;
}

void dtmb_poll_clear(struct aml_dtvdemod *demod)
{
	struct poll_machie_s *pollm = &demod->poll_machie;

	memset(pollm, 0, sizeof(struct poll_machie_s));
}

#define DTMBM_NO_SIGNEL_CHECK	0x01
#define DTMBM_HV_SIGNEL_CHECK	0x02
#define DTMBM_BCH_OVER_CHEK	0x04

#define DTMBM_CHEK_NO		(DTMBM_NO_SIGNEL_CHECK)
#define DTMBM_CHEK_HV		(DTMBM_HV_SIGNEL_CHECK | DTMBM_BCH_OVER_CHEK)
#define DTMBM_WORK		(DTMBM_NO_SIGNEL_CHECK | DTMBM_HV_SIGNEL_CHECK\
					| DTMBM_BCH_OVER_CHEK)

#define DTMBM_POLL_CNT_NO_SIGNAL	(10)
#define DTMBM_POLL_CNT_WAIT_LOCK	(3) /*from 30 to 3 */
#define DTMBM_POLL_DELAY_NO_SIGNAL	(120)
#define DTMBM_POLL_DELAY_HAVE_SIGNAL	(100)
/*dtmb_poll_v3 is same as dtmb_check_status_txl*/
void dtmb_poll_v3(struct aml_dtvdemod *demod)
{
	struct poll_machie_s *pollm = &demod->poll_machie;
	unsigned int bch_tmp;
	unsigned int s;

	if (!pollm->state) {
		/* idle */
		/* idle -> start check */
		if (!pollm->flg_restart) {
			PR_DBG("x");
			return;
		}
	} else {
		if (pollm->flg_stop) {
			PR_DBG("dtmb poll stop !\n");
			dtmb_poll_clear(demod);
			dtmb_set_delay(demod, 3 * HZ);

			return;
		}
	}

	/* restart: clear */
	if (pollm->flg_restart) {
		PR_DBG("dtmb poll restart!\n");
		dtmb_poll_clear(demod);

	}
	PR_DBG("-");
	s = check_dtmb_fec_lock();

	/* bch exceed the threshold: wait lock*/
	if (pollm->state & DTMBM_BCH_OVER_CHEK) {
		if (pollm->crrcnt < DTMBM_POLL_CNT_WAIT_LOCK) {
			pollm->crrcnt++;

			if (s) {
				PR_DBG("after reset get lock again!cnt=%d\n",
					pollm->crrcnt);
				dtmb_constell_check();
				pollm->state = DTMBM_HV_SIGNEL_CHECK;
				pollm->crrcnt = 0;
				pollm->bch = dtmb_reg_r_bch();


				dtmb_save_status(demod, s);
			}
		} else {
			PR_DBG("can't lock after reset!\n");
			pollm->state = DTMBM_NO_SIGNEL_CHECK;
			pollm->crrcnt = 0;
			/* to no signal*/
			dtmb_save_status(demod, s);
		}
		return;
	}


	if (s) {
		/*have signal*/
		if (!pollm->state) {
			pollm->state = DTMBM_CHEK_NO;
			PR_DBG("from idle to have signal wait 1\n");
			return;
		}
		if (pollm->state & DTMBM_CHEK_NO) {
			/*no to have*/
			PR_DBG("poll machie: from no signal to have signal\n");
			pollm->bch = dtmb_reg_r_bch();
			pollm->state = DTMBM_HV_SIGNEL_CHECK;

			dtmb_set_delay(demod, DTMBM_POLL_DELAY_HAVE_SIGNAL);
			return;
		}


		bch_tmp = dtmb_reg_r_bch();
		if (bch_tmp > (pollm->bch + 50)) {
			pollm->state = DTMBM_BCH_OVER_CHEK;

			PR_DBG("bch add ,need reset,wait not to reset\n");
			dtmb_reset();

			pollm->crrcnt = 0;
			dtmb_set_delay(demod, DTMBM_POLL_DELAY_HAVE_SIGNAL);
		} else {
			pollm->bch = bch_tmp;
			pollm->state = DTMBM_HV_SIGNEL_CHECK;

			dtmb_save_status(demod, s);
			/*have signale to have signal*/
			dtmb_set_delay(demod, 300);
		}
		return;
	}

	/*no signal */
	if (!pollm->state) {
		/* idle -> no signal */
		PR_DBG("poll machie: from idle to no signal\n");
		pollm->crrcnt = 0;

		pollm->state = DTMBM_NO_SIGNEL_CHECK;
	} else if (pollm->state & DTMBM_CHEK_HV) {
		/*have signal -> no signal*/
		PR_DBG("poll machie: from have signal to no signal\n");
		pollm->crrcnt = 0;
		pollm->state = DTMBM_NO_SIGNEL_CHECK;
		dtmb_save_status(demod, s);
	}

	/*no siganel check process */
	if (pollm->crrcnt < DTMBM_POLL_CNT_NO_SIGNAL) {
		dtmb_no_signal_check_v3();
		pollm->crrcnt++;

		dtmb_set_delay(demod, DTMBM_POLL_DELAY_NO_SIGNAL);
	} else {
		dtmb_no_signal_check_finishi_v3(demod);
		pollm->crrcnt = 0;

		dtmb_save_status(demod, s);
		/*no signal to no signal*/
		dtmb_set_delay(demod, 300);
	}
}

void dtmb_poll_start_tune(struct aml_dtvdemod *demod, unsigned int state)
{
	struct poll_machie_s *pollm = &demod->poll_machie;

	dtmb_poll_clear(demod);

	pollm->state = state;
	if (state & DTMBM_NO_SIGNEL_CHECK)
		dtmb_save_status(demod, 0);
	else
		dtmb_save_status(demod, 1);

	PR_DTMB("dtmb_poll_start tune to %d\n", state);
}

/*come from gxtv_demod_dtmb_read_status, have ms_delay*/
int dtmb_poll_v2(struct dvb_frontend *fe, enum fe_status *status)
{
	struct aml_dtvdemod *demod = (struct aml_dtvdemod *)fe->demodulator_priv;
	int ilock;
	unsigned char s = 0;

	s = dtmb_check_status_gxtv(fe);

	if (s == 1) {
		ilock = 1;
		*status =
			FE_HAS_LOCK | FE_HAS_SIGNAL | FE_HAS_CARRIER |
			FE_HAS_VITERBI | FE_HAS_SYNC;
	} else {
		ilock = 0;
		*status = FE_TIMEDOUT;
	}

	if (demod->last_lock != ilock) {
		PR_INFO("%s [id %d]: %s.\n", __func__, demod->id,
			 ilock ? "!!  >> LOCK << !!" : "!! >> UNLOCK << !!");
		demod->last_lock = ilock;
	}

	return 0;
}

/*this is ori gxtv_demod_dtmb_read_status*/
static int gxtv_demod_dtmb_read_status_old
	(struct dvb_frontend *fe, enum fe_status *status)
{
	struct aml_dtvdemod *demod = (struct aml_dtvdemod *)fe->demodulator_priv;
	int ilock;
	unsigned char s = 0;

	if (is_meson_gxtvbb_cpu()) {
#ifndef CONFIG_AMLOGIC_REMOVE_OLD
		dtmb_check_status_gxtv(fe);
#endif
	} else if (cpu_after_eq(MESON_CPU_MAJOR_ID_TXL)) {
		dtmb_bch_check();
		if (!cpu_after_eq(MESON_CPU_MAJOR_ID_TL1))
			dtmb_check_status_txl(fe);
	} else {
		return -1;
	}

	s = amdemod_stat_islock(demod, SYS_DTMB);

	if (s == 1) {
		ilock = 1;
		*status =
			FE_HAS_LOCK | FE_HAS_SIGNAL | FE_HAS_CARRIER |
			FE_HAS_VITERBI | FE_HAS_SYNC;
	} else {
		ilock = 0;
		*status = FE_TIMEDOUT;
	}
	if (demod->last_lock != ilock) {
		PR_INFO("%s [%d]: %s.\n", __func__, demod->id,
			 ilock ? "!!  >> LOCK << !!" : "!! >> UNLOCK << !!");
		demod->last_lock = ilock;
	}

	return 0;
}

static int gxtv_demod_dtmb_read_ber(struct dvb_frontend *fe, u32 *ber)
{
	*ber = 0;

	return 0;
}

static int gxtv_demod_dtmb_read_signal_strength
		(struct dvb_frontend *fe, u16 *strength)
{
	int tuner_sr;
	struct aml_dtvdemod *demod = (struct aml_dtvdemod *)fe->demodulator_priv;
	struct amldtvdemod_device_s *devp = (struct amldtvdemod_device_s *)demod->priv;

	if (devp->pin_name &&
	    (strncmp(fe->ops.tuner_ops.info.name, "r842", 4) == 0)) {
		tuner_sr = tuner_get_ch_power2(fe);
		tuner_sr += 16;

		if (tuner_sr < -100)
			*strength = 0;
		else
			*strength = tuner_sr + 100;
	} else
		*strength = tuner_get_ch_power3(fe);
	return 0;
}

static int gxtv_demod_dtmb_read_snr(struct dvb_frontend *fe, u16 *snr)
{
	int tmp, snr_avg;

	tmp = snr_avg = 0;
	/* tmp = dtmb_read_reg(DTMB_TOP_FEC_LOCK_SNR);*/
	tmp = dtmb_reg_r_che_snr();

	*snr = convert_snr(tmp);

	return 0;
}

static int gxtv_demod_dtmb_read_ucblocks(struct dvb_frontend *fe, u32 *ucblocks)
{
	*ucblocks = 0;
	return 0;
}

static int gxtv_demod_dtmb_set_frontend(struct dvb_frontend *fe)
{
	struct dtv_frontend_properties *c = &fe->dtv_property_cache;
	struct aml_dtvdemod *demod = (struct aml_dtvdemod *)fe->demodulator_priv;
	struct amldtvdemod_device_s *devp = (struct amldtvdemod_device_s *)demod->priv;
	struct aml_demod_dtmb param;
	int times;
	/*[0]: specturm inverse(1),normal(0); [1]:if_frequency*/
	unsigned int tuner_freq[2] = {0};

	PR_INFO("%s [id %d]: delsys:%d, freq:%d, symbol_rate:%d, bw:%d, modul:%d, invert:%d.\n",
			__func__, demod->id, c->delivery_system, c->frequency, c->symbol_rate,
			c->bandwidth_hz, c->modulation, c->inversion);

	if (!devp->demod_thread)
		return 0;

	times = 2;
	memset(&param, 0, sizeof(param));
	param.ch_freq = c->frequency / 1000;

	demod->last_lock = -1;
	tuner_set_params(fe);	/*aml_fe_analog_set_frontend(fe);*/
	msleep(100);

	if (cpu_after_eq(MESON_CPU_MAJOR_ID_TL1)) {
		if (fe->ops.tuner_ops.get_if_frequency)
			fe->ops.tuner_ops.get_if_frequency(fe, tuner_freq);
		if (tuner_freq[0] == 0)
			demod->demod_status.spectrum = 0;
		else if (tuner_freq[0] == 1)
			demod->demod_status.spectrum = 1;
		else
			pr_err("wrong specturm val get from tuner\n");
	}

	dtmb_set_ch(demod, &param);

	return 0;
}

static int gxtv_demod_dtmb_get_frontend(struct dvb_frontend *fe)
{
	/* these content will be writed into eeprom. */

	return 0;
}

int Gxtv_Demod_Dtmb_Init(struct aml_dtvdemod *demod)
{
	struct aml_demod_sys sys;
	struct amldtvdemod_device_s *devp = (struct amldtvdemod_device_s *)demod->priv;

	if (!devp) {
		pr_err("%s devp is NULL\n", __func__);
		return -EFAULT;
	}

	PR_DBG("AML Demod DTMB init\r\n");

	memset(&sys, 0, sizeof(sys));
	/*memset(&i2c, 0, sizeof(i2c));*/
	memset(&demod->demod_status, 0, sizeof(demod->demod_status));
	demod->demod_status.delsys = SYS_DTMB;

	if (is_meson_gxtvbb_cpu()) {
#ifndef CONFIG_AMLOGIC_REMOVE_OLD
		sys.adc_clk = ADC_CLK_25M;
		sys.demod_clk = DEMOD_CLK_200M;
#endif
	} else if (cpu_after_eq(MESON_CPU_MAJOR_ID_TXL)) {
		if (is_meson_txl_cpu()) {
			sys.adc_clk = ADC_CLK_25M;
			sys.demod_clk = DEMOD_CLK_225M;
		} else if (cpu_after_eq(MESON_CPU_MAJOR_ID_TL1)) {
			sys.adc_clk = ADC_CLK_24M;
			sys.demod_clk = DEMOD_CLK_250M;
		} else {
			sys.adc_clk = ADC_CLK_24M;
			sys.demod_clk = DEMOD_CLK_225M;
		}
	} else {
		return -1;
	}

	demod->demod_status.ch_if = SI2176_5M_IF;
	demod->demod_status.tmp = ADC_MODE;
	demod->demod_status.spectrum = devp->spectrum;

	if (devp->data->hw_ver >= DTVDEMOD_HW_TL1)
		dd_tvafe_hiu_reg_write(HHI_DEMOD_CLK_CNTL << 2, 0x501);

	demod_set_sys(demod, &sys);

	return 0;
}

//#ifdef DVB_CORE_ORI
unsigned int demod_dvbc_get_fast_search(void)
{
	return demod_dvbc_speedup_en;
}

void demod_dvbc_set_fast_search(unsigned int en)
{
	if (en)
		demod_dvbc_speedup_en = 1;
	else
		demod_dvbc_speedup_en = 0;
}

void demod_dvbc_fsm_reset(struct aml_dtvdemod *demod)
{
	//qam_write_reg(demod, 0x7, 0xf23);
	qam_write_reg(demod, 0x7, qam_read_reg(demod, 0x7) & ~(1 << 4));
	qam_write_reg(demod, 0x3a, 0x0);
	//qam_write_reg(demod, 0x7, 0xf33);
	qam_write_reg(demod, 0x7, qam_read_reg(demod, 0x7) | (1 << 4));
	qam_write_reg(demod, 0x3a, 0x4);
}

//return val : 0=no signal,1=has signal,2=waiting
unsigned int demod_dvbc_auto_fast(struct dvb_frontend *fe, unsigned int *delay, bool re_tune)
{
	struct aml_dtvdemod *demod = (struct aml_dtvdemod *)fe->demodulator_priv;

	if (re_tune) {
		/*avoid that the previous channel has not been locked/unlocked*/
		/*the next channel is set again*/
		/*the wait time is too long,then miss channel*/
		demod->auto_no_sig_cnt = 0;
		demod->auto_times = 0;
		demod->auto_qam_mode = QAM_MODE_16;
		PR_DVBC("fast search : retune reset\n");
		return 2;
	}

	if (tuner_get_ch_power2(fe) < -87) {
		demod->auto_no_sig_cnt = 0;
		demod->auto_times = 0;
		*delay = HZ / 4;
		demod->auto_qam_mode = QAM_MODE_16;
		return 0;
	}
	demod->auto_times++;
	PR_DVBC("fast search : times = %d\n", demod->auto_times);
	if (demod->auto_times < 15)
		*delay = HZ / 8;//125ms
	else
		*delay = HZ / 2;//500ms

	if ((qam_read_reg(demod, 0x31) & 0xf) < 3) {
		demod->auto_no_sig_cnt++;

		if (demod->auto_no_sig_cnt == 2 && demod->auto_times == 2) {//250ms
			demod->auto_no_sig_cnt = 0;
			demod->auto_times = 0;
			*delay = HZ / 4;
			demod->auto_qam_mode = QAM_MODE_16;
			return 0;
		}
	} else if ((qam_read_reg(demod, 0x31) & 0xf) == 5) {
		demod->auto_no_sig_cnt = 0;
		demod->auto_times = 0;
		*delay = HZ / 4;
		demod->auto_qam_mode = QAM_MODE_16;
		return 1;
	}

	if (demod->auto_times == 35) {
		demod->auto_times = 0;
		demod->auto_no_sig_cnt = 0;
		*delay = HZ / 4;
		demod->auto_qam_mode = QAM_MODE_16;
		return 0;
	}

	/* loop from 16 to 256 */
	if (demod->auto_qam_mode++ >= QAM_MODE_NUM)
		demod->auto_qam_mode = QAM_MODE_16;

	PR_DVBC("fast search : qam mode = %d\n", demod->auto_qam_mode);
	demod_dvbc_set_qam(demod, demod->auto_qam_mode);
	demod_dvbc_fsm_reset(demod);

	return 2;
}

//return val : 0=no signal,1=has signal,2=waiting
unsigned int demod_dvbc_fast_search(struct dvb_frontend *fe, unsigned int *delay)
{
	struct aml_dtvdemod *demod = (struct aml_dtvdemod *)fe->demodulator_priv;

	if (tuner_get_ch_power2(fe) < -87)
		return 0;

	demod->times++;
	PR_DVBC("fast search : times = %d\n", demod->times);
	if (demod->times < 3)
		*delay = HZ / 8;//125ms
	else
		*delay = HZ / 2;//500ms

	if ((qam_read_reg(demod, 0x31) & 0xf) < 3) {
		demod->no_sig_cnt++;

		if (demod->no_sig_cnt == 2 && demod->times == 2) {//250ms
			demod->no_sig_cnt = 0;
			demod->times = 0;
			*delay = HZ / 4;
			return 0;
		}
	} else if ((qam_read_reg(demod, 0x31) & 0xf) == 5) {
		demod->no_sig_cnt = 0;
		demod->times = 0;
		*delay = HZ / 4;
		return 1;
	}

	if (demod->times == 7) {
		demod->times = 0;
		demod->no_sig_cnt = 0;
		*delay = HZ / 4;
		return 0;
	}

	demod_dvbc_fsm_reset(demod);

	return 2;
}

static int gxtv_demod_dvbc_tune(struct dvb_frontend *fe, bool re_tune,
	unsigned int mode_flags, unsigned int *delay, enum fe_status *status)
{
	int ret = 0;
	unsigned int sig_flg;
	struct aml_dtvdemod *demod = (struct aml_dtvdemod *)fe->demodulator_priv;
	struct amldtvdemod_device_s *devp = (struct amldtvdemod_device_s *)demod->priv;

	*delay = HZ / 4;

	if (!devp->demod_thread)
		return 0;

	if (re_tune) {
		/*first*/
		demod->en_detect = 1;

		gxtv_demod_dvbc_set_frontend(fe);

		if (demod_dvbc_speedup_en == 1) {
			demod->fast_search_finish = 0;
			*status = 0;
			*delay = HZ / 8;
			qam_write_reg(demod, 0x65, 0x400c);
			qam_write_reg(demod, 0x60, 0x10466000);
			qam_write_reg(demod, 0xac, (qam_read_reg(demod, 0xac) & (~0xff00))
				| 0x800);
			qam_write_reg(demod, 0xae, (qam_read_reg(demod, 0xae)
				& (~0xff000000)) | 0x8000000);

			if (fe->dtv_property_cache.modulation == QAM_AUTO) {
				demod_dvbc_set_qam(demod, QAM_MODE_16);
				demod_dvbc_auto_fast(fe, delay, re_tune);
			}
		} else
			qam_write_reg(demod, 0x65, 0x800c);

		if (demod_dvbc_speedup_en == 1)
			return 0;

		timer_begain(demod, D_TIMER_DETECT);
		gxtv_demod_dvbc_read_status_timer(fe, status);

		if (cpu_after_eq(MESON_CPU_MAJOR_ID_TL1))
			demod_dvbc_speed_up(demod, status);

		PR_DBG("[id %d] tune finish!\n", demod->id);

		return ret;
	}

	if (!demod->en_detect) {
		PR_DBGL("[id %d] tune:not enable\n", demod->id);
		return ret;
	}

	if (demod_dvbc_speedup_en == 1) {
		if (!demod->fast_search_finish) {
			if (fe->dtv_property_cache.modulation == QAM_AUTO)
				sig_flg = demod_dvbc_auto_fast(fe, delay, re_tune);
			else
				sig_flg = demod_dvbc_fast_search(fe, delay);

			switch (sig_flg) {
			case 0:
				*status = FE_TIMEDOUT;
				demod->fast_search_finish = 1;
				PR_DBG("%s [id %d] >>>unlock<<<\n", __func__, demod->id);
				break;
			case 1:
				*status =
				FE_HAS_LOCK | FE_HAS_SIGNAL | FE_HAS_CARRIER |
				FE_HAS_VITERBI | FE_HAS_SYNC;
				demod->fast_search_finish = 1;
				PR_DBG("%s [id %d] >>>lock<<<\n", __func__, demod->id);
				break;
			case 2:
				*status = 0;
				break;
			default:
				PR_DVBC("[id %d] wrong return value\n", demod->id);
				break;
			}
		}	else {
			gxtv_demod_dvbc_read_status_timer(fe, status);
		}
	} else {
		gxtv_demod_dvbc_read_status_timer(fe, status);
	}

	if (demod_dvbc_speedup_en == 1)
		return 0;

	if (*status & FE_HAS_LOCK) {
		timer_disable(demod, D_TIMER_SET);
	} else {
		if (!timer_is_en(demod, D_TIMER_SET))
			timer_begain(demod, D_TIMER_SET);
	}

	if (timer_is_enough(demod, D_TIMER_SET)) {
		gxtv_demod_dvbc_set_frontend(fe);
		timer_disable(demod, D_TIMER_SET);
	}

	return ret;
}

static int dtvdemod_dvbs_set_ch(struct aml_demod_sta *demod_sta)
{
	int ret = 0;
	unsigned int symb_rate;

	symb_rate = demod_sta->symb_rate;

	/* 0:16, 1:32, 2:64, 3:128, 4:256 */
	demod_sta->agc_mode = 1;
	demod_sta->ch_bw = 8000;
	if (demod_sta->ch_if == 0)
		demod_sta->ch_if = 5000;

	dvbs2_reg_initial(symb_rate);

	return ret;
}

int dtvdemod_dvbs_set_frontend(struct dvb_frontend *fe)
{
	struct aml_dtvdemod *demod = (struct aml_dtvdemod *)fe->demodulator_priv;
	struct dtv_frontend_properties *c = &fe->dtv_property_cache;
	int ret = 0;

	PR_INFO("%s [id %d]: delsys:%d, freq:%d, symbol_rate:%d, bw:%d, modul:%d, invert:%d.\n",
			__func__, demod->id, c->delivery_system, c->frequency, c->symbol_rate,
			c->bandwidth_hz, c->modulation, c->inversion);

	demod->demod_status.symb_rate = c->symbol_rate / 1000;
	demod->last_lock = -1;

	tuner_set_params(fe);
	dtvdemod_dvbs_set_ch(&demod->demod_status);

	return ret;
}

int dtvdemod_dvbs_read_status(struct dvb_frontend *fe, enum fe_status *status)
{
	int ilock = 0;
	unsigned char s = 0;
	int strenth = 0;
	struct aml_dtvdemod *demod = (struct aml_dtvdemod *)fe->demodulator_priv;

	strenth = tuner_get_ch_power(fe);

	if (strenth < THRD_TUNER_STRENTH_DVBS) {
		*status = FE_TIMEDOUT;
		demod->last_lock = -1;
		PR_DVBT("[id %d] tuner:no signal!\n", demod->id);
		return 0;
	}

	s = amdemod_stat_islock(demod, SYS_DVBS);

	if (s == 1) {
		ilock = 1;
		*status =
			FE_HAS_LOCK | FE_HAS_SIGNAL | FE_HAS_CARRIER |
			FE_HAS_VITERBI | FE_HAS_SYNC;
	} else {
		if (timer_not_enough(demod, D_TIMER_DETECT)) {
			ilock = 0;
			*status = 0;
			PR_TIME("[id %d] timer not enough\n", demod->id);

		} else {
			ilock = 0;
			*status = FE_TIMEDOUT;
			timer_disable(demod, D_TIMER_DETECT);
		}
	}

	if (demod->last_lock != ilock) {
		PR_INFO("%s [id %d]: %s.\n", __func__, demod->id,
			 ilock ? "!!  >> LOCK << !!" : "!! >> UNLOCK << !!");
		demod->last_lock = ilock;
	}

	return 0;
}

static int dtvdemod_dvbs_tune(struct dvb_frontend *fe, bool re_tune,
	unsigned int mode_flags, unsigned int *delay, enum fe_status *status)
{
	int ret = 0;
	struct aml_dtvdemod *demod = (struct aml_dtvdemod *)fe->demodulator_priv;

	*delay = HZ / 4;

	if (re_tune) {
		/*first*/
		demod->en_detect = 1;
		dtvdemod_dvbs_set_frontend(fe);
		timer_begain(demod, D_TIMER_DETECT);
		dtvdemod_dvbs_read_status(fe, status);
		pr_info("[id %d] tune finish!\n", demod->id);
		return ret;
	}

	if (!demod->en_detect) {
		pr_err("[id %d] tune:not enable\n", demod->id);
		return ret;
	}

	dtvdemod_dvbs_read_status(fe, status);

	return ret;
}

static int dtvdemod_dvbs2_init(struct aml_dtvdemod *demod)
{
	struct aml_demod_sys sys;
	struct amldtvdemod_device_s *devp = (struct amldtvdemod_device_s *)demod->priv;

	PR_DBG("%s\n", __func__);
	memset(&sys, 0, sizeof(sys));

	demod->demod_status.delsys = SYS_DVBS2;
	sys.adc_clk = ADC_CLK_135M;
	sys.demod_clk = DEMOD_CLK_270M;
	demod->demod_status.tmp = CRY_MODE;

	demod->demod_status.ch_if = SI2176_5M_IF * 1000;
	PR_DBG("[%s]adc_clk is %d,demod_clk is %d\n", __func__, sys.adc_clk,
	       sys.demod_clk);
	demod->auto_flags_trig = 0;
	dd_tvafe_hiu_reg_write(HHI_DEMOD_CLK_CNTL1 << 2, 0x702);
	dd_tvafe_hiu_reg_write(HHI_DEMOD_CLK_CNTL << 2, 0x501);
	demod_set_sys(demod, &sys);
	aml_dtv_demode_isr_en(devp, 1);
	/*enable diseqc irq*/
	return 0;
}

static int gxtv_demod_dtmb_tune(struct dvb_frontend *fe, bool re_tune,
	unsigned int mode_flags, unsigned int *delay, enum fe_status *status)
{
	/*struct dtv_frontend_properties *c = &fe->dtv_property_cache;*/
	int ret = 0;
	//unsigned int up_delay;
	unsigned int firstdetet;
	struct aml_dtvdemod *demod = (struct aml_dtvdemod *)fe->demodulator_priv;

	if (re_tune) {
		/*first*/
		demod->en_detect = 1;

		*delay = HZ / 4;
		gxtv_demod_dtmb_set_frontend(fe);
		firstdetet = dtmb_detect_first();

		if (firstdetet == 1) {
			*status = FE_TIMEDOUT;
			/*polling mode*/
			dtmb_poll_start_tune(demod, DTMBM_NO_SIGNEL_CHECK);

		} else if (firstdetet == 2) {  /*need check*/
			*status = FE_HAS_LOCK | FE_HAS_SIGNAL | FE_HAS_CARRIER |
			FE_HAS_VITERBI | FE_HAS_SYNC;
			dtmb_poll_start_tune(demod, DTMBM_HV_SIGNEL_CHECK);

		} else if (firstdetet == 0) {
			PR_DBG("[id %d] use read_status\n", demod->id);
			gxtv_demod_dtmb_read_status_old(fe, status);
			if (*status == (0x1f))
				dtmb_poll_start_tune(demod, DTMBM_HV_SIGNEL_CHECK);
			else
				dtmb_poll_start_tune(demod, DTMBM_NO_SIGNEL_CHECK);
		}
		PR_DBG("[id %d] tune finish!\n", demod->id);

		return ret;
	}

	if (!demod->en_detect) {
		PR_DBG("[id %d] tune:not enable\n", demod->id);
		return ret;
	}

	*delay = HZ / 4;
	gxtv_demod_dtmb_read_status_old(fe, status);

	if (*status == (FE_HAS_LOCK | FE_HAS_SIGNAL | FE_HAS_CARRIER |
		FE_HAS_VITERBI | FE_HAS_SYNC))
		dtmb_poll_start_tune(demod, DTMBM_HV_SIGNEL_CHECK);
	else
		dtmb_poll_start_tune(demod, DTMBM_NO_SIGNEL_CHECK);

	return ret;
}

//#endif

#ifdef CONFIG_CMA
bool dtvdemod_cma_alloc(struct amldtvdemod_device_s *devp)
{
	bool ret;

	unsigned int mem_size = devp->cma_mem_size;
	/*	dma_alloc_from_contiguous*/
	devp->venc_pages =
		dma_alloc_from_contiguous(&(devp->this_pdev->dev),
			mem_size >> PAGE_SHIFT, 0, 0);
	PR_DBG("[cma]mem_size is %d,%d\n",
		mem_size, mem_size >> PAGE_SHIFT);
	if (devp->venc_pages) {
		devp->mem_start = page_to_phys(devp->venc_pages);
		devp->mem_size  = mem_size;
		devp->flg_cma_allc = true;
		PR_DBG("demod mem_start = 0x%x, mem_size = 0x%x\n",
			devp->mem_start, devp->mem_size);
		PR_DBG("demod cma alloc ok!\n");
		ret = true;
	} else {
		PR_DBG("demod cma mem undefined2.\n");
		ret = false;
	}
	return ret;
}

void dtvdemod_cma_release(struct amldtvdemod_device_s *devp)
{
	/*	dma_release_from_contiguous*/
	dma_release_from_contiguous(&(devp->this_pdev->dev),
			devp->venc_pages,
			devp->cma_mem_size>>PAGE_SHIFT);
		PR_DBG("demod cma release ok!\n");
	devp->mem_start = 0;
	devp->mem_size = 0;
}
#endif

static bool enter_mode(struct aml_dtvdemod *demod, enum fe_delivery_system delsys)
{
	struct amldtvdemod_device_s *devp = (struct amldtvdemod_device_s *)demod->priv;
	int ret = 0;

	if (delsys < SYS_ANALOG)
		PR_INFO("%s [id %d]:%s\n", __func__, demod->id,
				name_fe_delivery_system[delsys]);
	else
		PR_ERR("%s [id %d]:%d\n", __func__, demod->id, delsys);

	dtvdemod_set_agc_pinmux(1);

	/*-------------------*/
	/* must enable the adc ref signal for demod, */
	/*vdac_enable(1, VDAC_MODULE_DTV_DEMOD);*/
	dtvdemod_vdac_enable(1);/*on*/
	demod->en_detect = 0;/**/
	dtmb_poll_stop(demod);/*polling mode*/

	demod->auto_flags_trig = 1;
	if (cci_thread)
		if (dvbc_get_cci_task(demod) == 1)
			dvbc_create_cci_task(demod);
	/*mem_buf = (long *)phys_to_virt(memstart);*/

	if (devp->cma_flag == 1 && !devp->flg_cma_allc) {
		PR_DBG("CMA MODE, cma flag is %d,mem size is %d",
				devp->cma_flag, devp->cma_mem_size);

		if (!dtvdemod_cma_alloc(devp)) {
			ret = -ENOMEM;
			return ret;
		}
	}

	demod->inited = true;

	switch (delsys) {
	case SYS_DTMB:
		ret = Gxtv_Demod_Dtmb_Init(demod);
		demod->act_dtmb = true;
		dtmb_set_mem_st(devp->mem_start);

		if (!cpu_after_eq(MESON_CPU_MAJOR_ID_TL1))
			demod_top_write_reg(DEMOD_TOP_REGC, 0x8);
		break;

	case SYS_DVBC_ANNEX_A:
	case SYS_DVBC_ANNEX_C:
		ret = Gxtv_Demod_Dvbc_Init(demod, ADC_MODE);

		/* The maximum time of signal detection is 3s */
		timer_set_max(demod, D_TIMER_DETECT, 3000);
		/* reset is 4s */
		timer_set_max(demod, D_TIMER_SET, 4000);
		break;

	case SYS_ATSC:
	case SYS_ATSCMH:
	case SYS_DVBC_ANNEX_B:
		ret = dtvdemod_atsc_init(demod);

		if (cpu_after_eq(MESON_CPU_MAJOR_ID_TL1))
			timer_set_max(demod, D_TIMER_DETECT, demod->timeout_atsc_ms);

		PR_ATSC("timeout is %dms\n", demod->timeout_atsc_ms);
		break;

	case SYS_DVBT:
		ret = dvbt_init(demod);
		timer_set_max(demod, D_TIMER_DETECT, demod->timeout_dvbt_ms);
		PR_DVBT("timeout is %dms\n", demod->timeout_dvbt_ms);
		break;

	case SYS_DVBT2:
		if (devp->data->hw_ver == DTVDEMOD_HW_T5D) {
			devp->dmc_saved = dtvdemod_dmc_reg_read(0x274);
			PR_INFO("dmc val 0x%x\n", devp->dmc_saved);
			dtvdemod_dmc_reg_write(0x274, 0x18100000);
		}

		dtvdemod_dvbt2_init(demod);
		timer_set_max(demod, D_TIMER_DETECT, demod->timeout_dvbt_ms);
		PR_DVBT("timeout is %dms\n", demod->timeout_dvbt_ms);
		break;

	case SYS_ISDBT:
		ret = dvbt_isdbt_Init(demod);
		/*The maximum time of signal detection is 2s */
		timer_set_max(demod, D_TIMER_DETECT, 2000);
		/*reset is 4s*/
		timer_set_max(demod, D_TIMER_SET, 4000);
		PR_DBG("[im]memstart is %x\n", devp->mem_start);
		dvbt_isdbt_wr_reg((0x10 << 2), devp->mem_start);
		break;

	case SYS_DVBS:
	case SYS_DVBS2:
		dtvdemod_dvbs2_init(demod);
		timer_set_max(demod, D_TIMER_DETECT, demod->timeout_dvbs_ms);
		PR_DVBS("timeout is %dms\n", demod->timeout_dvbs_ms);
		break;

	default:
		break;
	}

	return ret;
}

static int leave_mode(struct aml_dtvdemod *demod, enum fe_delivery_system delsys)
{
	bool realy_leave = true;
	struct aml_dtvdemod *tmp = NULL;
	struct amldtvdemod_device_s *devp = (struct amldtvdemod_device_s *)demod->priv;

	if (delsys < SYS_ANALOG)
		PR_INFO("%s [id %d]:%s\n", __func__, demod->id,
				name_fe_delivery_system[delsys]);
	else
		PR_ERR("%s [id %d]:%d\n", __func__, demod->id, delsys);

	demod->en_detect = 0;
	demod->last_delsys = SYS_UNDEFINED;

	list_for_each_entry(tmp, &devp->demod_list, list) {
		if (tmp->id != demod->id && tmp->inited) {
			realy_leave = false;
			break;
		}
	}

	if (realy_leave)
		dtvpll_init_flag(0);

	/*dvbc_timer_exit();*/
	if (cci_thread)
		dvbc_kill_cci_task(demod);

	switch (delsys) {
	case SYS_DVBC_ANNEX_A:
	case SYS_DVBC_ANNEX_C:
		if (realy_leave && devp->dvbc_inited)
			devp->dvbc_inited = false;

		break;

	case SYS_DTMB:
		if (demod->act_dtmb) {
			dtmb_poll_stop(demod); /*polling mode*/
			/* close arbit */
			demod_top_write_reg(DEMOD_TOP_REGC, 0x0);
			demod->act_dtmb = false;
		}
		break;

	case SYS_DVBT:
		/* disable dvbt mode to avoid hang when switch to other demod */
		demod_top_write_reg(DEMOD_TOP_REGC, 0x11);
		demod_top_write_reg(DEMOD_TOP_CFG_REG_4, 0x0);
		break;

	case SYS_ISDBT:
	case SYS_DVBT2:

		if (devp->data->hw_ver == DTVDEMOD_HW_T5D && delsys == SYS_DVBT2) {
			PR_INFO("resume dmc val 0x%x\n", devp->dmc_saved);
			dtvdemod_dmc_reg_write(0x274, devp->dmc_saved);
		}

		/* disable dvbt mode to avoid hang when switch to other demod */
		demod_top_write_reg(DEMOD_TOP_REGC, 0x11);
		demod_top_write_reg(DEMOD_TOP_CFG_REG_4, 0x0);
		break;
	case SYS_DVBS:
	case SYS_DVBS2:
		/*disable irq*/
		aml_dtv_demode_isr_en(devp, 0);
		/* disable dvbs mode to avoid hang when switch to other demod */
		demod_top_write_reg(DEMOD_TOP_REGC, 0x11);
		break;
	default:
		break;
	}

	if (realy_leave) {
#ifdef CONFIG_AMLOGIC_MEDIA_ADC
		adc_set_pll_cntl(0, ADC_DTV_DEMOD, NULL);
		adc_set_pll_cntl(0, ADC_DTV_DEMODPLL, NULL);
#endif
		/* should disable the adc ref signal for demod */
		/*vdac_enable(0, VDAC_MODULE_DTV_DEMOD);*/
		dtvdemod_vdac_enable(0);/*off*/
		dtvdemod_set_agc_pinmux(0);

		if (devp->cma_flag == 1 && devp->flg_cma_allc) {
			dtvdemod_cma_release(devp);
			devp->flg_cma_allc = false;
		}

		PR_INFO("%s: realy_leave.\n", __func__);
	}

	demod->inited = false;
	demod->suspended = true;

	return 0;
}

#ifndef CONFIG_AMLOGIC_REMOVE_OLD
/* when can't get ic_config by dts, use this*/
const struct meson_ddemod_data  data_gxtvbb = {
	.regoff = {
		.off_demod_top = 0xc00,
		.off_dvbc = 0x400,
		.off_dtmb = 0x00,
	},
};

const struct meson_ddemod_data  data_txlx = {
	.regoff = {
		.off_demod_top = 0xf00,
		.off_dvbc = 0xc00,
		.off_dtmb = 0x000,
		.off_dvbt_isdbt = 0x400,
		.off_isdbt = 0x400,
		.off_atsc = 0x800,
	},
	.hw_ver = DTVDEMOD_HW_TXLX,
};
#endif

const struct meson_ddemod_data  data_tl1 = {
	.regoff = {
		.off_demod_top = 0x3c00,
		.off_dvbc = 0x1000,
		.off_dtmb = 0x0000,
		.off_atsc = 0x0c00,
		.off_front = 0x3800,
	},
};

const struct meson_ddemod_data  data_t5 = {
	.regoff = {
		.off_demod_top = 0x3c00,
		.off_dvbc = 0x1000,
		.off_dtmb = 0x0000,
		.off_front = 0x3800,
	},
	.hw_ver = DTVDEMOD_HW_T5,
};

const struct meson_ddemod_data  data_t5d = {
	.regoff = {
		.off_demod_top = 0xf000,
		.off_dvbc = 0x1000,
		.off_dtmb = 0x0000,
		.off_atsc = 0x0c00,
		.off_isdbt = 0x800,
		.off_front = 0x3800,
		.off_dvbs = 0x2000,
		.off_dvbt_isdbt = 0x800,
		.off_dvbt_t2 = 0x0000,
	},
	.hw_ver = DTVDEMOD_HW_T5D,
};

const struct meson_ddemod_data  data_s4 = {
	.regoff = {
		.off_demod_top = 0xf000,
		.off_front = 0x3800,
		.off_dvbc = 0x1000,
		.off_dvbc_2 = 0x1400,
	},
	.hw_ver = DTVDEMOD_HW_S4,
};

const struct meson_ddemod_data  data_t5d_revb = {
	.regoff = {
		.off_demod_top = 0xf000,
		.off_dvbc = 0x1000,
		.off_dtmb = 0x0000,
		.off_atsc = 0x0c00,
		.off_isdbt = 0x800,
		.off_front = 0x3800,
		.off_dvbs = 0x2000,
		.off_dvbt_isdbt = 0x800,
		.off_dvbt_t2 = 0x0000,
	},
	.hw_ver = DTVDEMOD_HW_T5D_B,
};

static const struct of_device_id meson_ddemod_match[] = {
#ifndef CONFIG_AMLOGIC_REMOVE_OLD
	{
		.compatible = "amlogic, ddemod-gxtvbb",
		.data		= &data_gxtvbb,
	}, {
		.compatible = "amlogic, ddemod-txl",
		.data		= &data_gxtvbb,
	}, {
		.compatible = "amlogic, ddemod-txlx",
		.data		= &data_txlx,
	}, {
		.compatible = "amlogic, ddemod-gxlx",
		.data		= &data_txlx,
	}, {
		.compatible = "amlogic, ddemod-txhd",
		.data		= &data_txlx,
	}, {
		.compatible = "amlogic, ddemod-tl1",
		.data		= &data_tl1,
	},
#endif
	{
		.compatible = "amlogic, ddemod-tm2",
		.data		= &data_tl1,
	}, {
		.compatible = "amlogic, ddemod-t5",
		.data		= &data_t5,
	}, {
		.compatible = "amlogic, ddemod-t5d",
		.data		= &data_t5d,
	}, {
		.compatible = "amlogic, ddemod-s4",
		.data		= &data_s4,
	}, {
		.compatible = "amlogic, ddemod-t5d-revB",
		.data		= &data_t5d_revb,
	},
	/* DO NOT remove, to avoid scan err of KASAN */
	{}
};



/*
 * dds_init_reg_map - physical addr map
 *
 * map physical address of I/O memory resources
 * into the core virtual address space
 */
static int dds_init_reg_map(struct platform_device *pdev)
{
	struct amldtvdemod_device_s *devp =
			(struct amldtvdemod_device_s *)platform_get_drvdata(pdev);

	struct ss_reg_phy *preg = &devp->reg_p[0];
	struct ss_reg_vt *pv = &devp->reg_v[0];
	int i;
	struct resource *res = 0;
	int size = 0;
	int ret = 0;

	for (i = 0; i < ES_MAP_ADDR_NUM; i++) {
		res = platform_get_resource(pdev, IORESOURCE_MEM, i);
		if (!res) {
			PR_ERR("%s: res %d is faile\n", __func__, i);
			ret = -ENOMEM;
			break;
		}
		size = resource_size(res);
		preg[i].size = size;
		preg[i].phy_addr = res->start;
		pv[i].v = devm_ioremap_nocache(&pdev->dev, res->start, size);
	}

	if (devp->data->hw_ver >= DTVDEMOD_HW_T5D) {
		devp->dmc_phy_addr = 0xff638000;
		devp->dmc_v_addr = devm_ioremap_nocache(&pdev->dev, 0xff638000, 0x2000);
	}

	return ret;
}

irqreturn_t aml_dtv_demode_isr(int irq, void *dev_id)
{
	struct amldtvdemod_device_s *devp = dtvdemod_get_dev();
	struct aml_dtvdemod *demod = NULL, *tmp = NULL;

	list_for_each_entry(tmp, &devp->demod_list, list) {
		if (tmp->id == 0) {
			demod = tmp;
			break;
		}
	}

	if (!demod) {
		PR_INFO("%s: demod == NULL.\n", __func__);
		return IRQ_NONE;
	}

	if (demod->last_delsys == SYS_DVBS || demod->last_delsys == SYS_DVBS2)
		aml_diseqc_isr();

	/*PR_INFO("%s\n", __func__);*/
	return IRQ_HANDLED;
}

void aml_dtv_demode_isr_en(struct amldtvdemod_device_s *devp, u32 en)
{
	if (en) {
		if (!devp->demod_irq_en && devp->demod_irq_num > 0) {
			enable_irq(devp->demod_irq_num);
			devp->demod_irq_en = 1;
			PR_INFO("enable demod_irq\n");
		}
	} else {
		if (devp->demod_irq_en && devp->demod_irq_num > 0) {
			disable_irq(devp->demod_irq_num);
			devp->demod_irq_en = 0;
			PR_INFO("disable demod_irq\n");
		}
	}
}

int dtvdemod_set_iccfg_by_dts(struct platform_device *pdev)
{
	/*int size_io_reg;*/
	u32 value;
	int ret;
	/*struct resource *res = &dtvdemod_mem;*/
	struct amldtvdemod_device_s *devp =
			(struct amldtvdemod_device_s *)platform_get_drvdata(pdev);

	PR_DBG("%s:\n", __func__);

	ret = of_reserved_mem_device_init(&pdev->dev);
	if (ret != 0)
		PR_INFO("no reserved mem.\n");

	/*agc pinmux: option*/
	//ret = of_property_read_string(pdev->dev.of_node, "pinctrl-names",
	//		&devp->pin_name);
	if (of_get_property(pdev->dev.of_node, "pinctrl-names", NULL)) {
		devp->pin = devm_pinctrl_get(&pdev->dev);
		devp->pin_agc_st =
		pinctrl_lookup_state(devp->pin, "dtvdemod_agc_pins");
		if (IS_ERR(devp->pin_agc_st)) {
			devp->pin_name = NULL;
			PR_INFO("no pin_agc_st pinctrl state\n");
		} else {
			devp->pin_name = "dtvdemod_agc_pins";
		}
		PR_INFO("pinmux name:%s\n", devp->pin_name);
	} else {
		PR_INFO("pinmux:not define in dts\n");
		devp->pin = NULL;
	}

/*move from aml_fe*/
	/*snprintf(buf, sizeof(buf), "%s%d_spectrum", name, id);*/
#ifdef CONFIG_OF
	ret = of_property_read_u32(pdev->dev.of_node, "spectrum", &value);
	if (!ret) {
		devp->spectrum = value;
		PR_INFO("spectrum: %d\n", value);
	} else {
		devp->spectrum = 2;
	}
#else				/*CONFIG_OF */
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "spectrum");
	if (res) {
		int spectrum = res->start;

		devp->spectrum = spectrum;
	} else {
		devp->spectrum = 0;
	}
#endif
	/*snprintf(buf, sizeof(buf), "%s%d_cma_flag", name, id);*/
#ifdef CONFIG_OF
	ret = of_property_read_u32(pdev->dev.of_node, "cma_flag", &value);
	if (!ret) {
		devp->cma_flag = value;
		PR_INFO("cma_flag: %d\n", value);
	} else {
		devp->cma_flag = 0;
	}
#else				/*CONFIG_OF */
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "cma_flag");
	if (res) {
		int cma_flag = res->start;

		devp->cma_flag = cma_flag;
	} else {
		devp->cma_flag = 0;
	}
#endif
	/*snprintf(buf, sizeof(buf), "%s%d_atsc_version", name, id);*/
#ifdef CONFIG_OF
	ret = of_property_read_u32(pdev->dev.of_node, "atsc_version", &value);
	if (!ret) {
		devp->atsc_version = value;
		PR_INFO("atsc_version: %d\n", value);
	} else {
		devp->atsc_version = 0;
	}
#else /*CONFIG_OF */
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM,
					"atsc_version");
	if (res) {
		int atsc_version = res->start;

		devp->atsc_version = atsc_version;
	} else {
		devp->atsc_version = 0;
	}
#endif

	if (devp->cma_flag == 1) {
		/*snprintf(buf, sizeof(buf), "%s%d_cma_mem_size", name, id);*/
#ifdef CONFIG_CMA
#ifdef CONFIG_OF
		ret = of_property_read_u32(pdev->dev.of_node,
					"cma_mem_size", &value);
		if (!ret) {
			devp->cma_mem_size = value;
			PR_INFO("cma_mem_size: %d\n", value);
		} else {
			devp->cma_mem_size = 0;
		}
#else				/*CONFIG_OF */
		res = platform_get_resource_byname(pdev, IORESOURCE_MEM,
							"cma_mem_size");
		if (res) {
			int cma_mem_size = res->start;

			devp->cma_mem_size = cma_mem_size;
		} else {
			devp->cma_mem_size = 0;
		}
#endif
		devp->cma_mem_size =
				dma_get_cma_size_int_byte(&pdev->dev);
		devp->this_pdev = pdev;
		devp->cma_mem_alloc = 0;
		PR_INFO("[cma]demod cma_mem_size = %d MB\n",
				(u32)devp->cma_mem_size / SZ_1M);
#endif
		} else {
#ifdef CONFIG_OF
			devp->mem_start = memstart;
#endif
	}

	/* diseqc sel */
	ret = of_property_read_string(pdev->dev.of_node, "diseqc_name",
			&devp->diseqc_name);
	if (ret) {
		PR_INFO("diseqc_name:not define in dts\n");
		devp->diseqc_name = NULL;
	} else {
		PR_INFO("diseqc_name name:%s\n", devp->diseqc_name);
	}

	/*get demod irq*/
	ret = of_irq_get_byname(pdev->dev.of_node, "demod_isr");
	if (ret > 0) {
		devp->demod_irq_num = ret;
		ret = request_irq(devp->demod_irq_num, aml_dtv_demode_isr,
				  IRQF_SHARED, "demod_isr",
				  (void *)devp);
		if (ret != 0)
			PR_INFO("req demod_isr fail\n");
		disable_irq(devp->demod_irq_num);
		devp->demod_irq_en = false;
		PR_INFO("demod_isr num:%d\n", devp->demod_irq_num);
	} else {
		devp->demod_irq_num = 0;
		PR_INFO("no demod isr\n");
	}

	/*diseqc pin ctrl*/
	if (!IS_ERR_OR_NULL(devp->pin)) {
		devp->diseqc_pin_st =
			pinctrl_lookup_state(devp->pin, "diseqc");
		if (IS_ERR_OR_NULL(devp->diseqc_pin_st))
			PR_INFO("no diseqc pinctrl\n");
		else
			pinctrl_select_state(devp->pin,
					devp->diseqc_pin_st);
	}
/*end-------------*/
	return 0;

}
#if (defined CONFIG_AMLOGIC_DTV_DEMOD)	/*move to aml_dtv_demod*/
static int rmem_demod_device_init(struct reserved_mem *rmem, struct device *dev)
{
	unsigned int demod_mem_start;
	unsigned int demod_mem_size;

	demod_mem_start = rmem->base;
	demod_mem_size = rmem->size;
	memstart = demod_mem_start;
	pr_info("demod reveser memory 0x%x, size %dMB.\n",
		demod_mem_start, (demod_mem_size >> 20));
	return 1;
}

static void rmem_demod_device_release(struct reserved_mem *rmem,
				      struct device *dev)
{
}

static const struct reserved_mem_ops rmem_demod_ops = {
	.device_init = rmem_demod_device_init,
	.device_release = rmem_demod_device_release,
};

static int __init rmem_demod_setup(struct reserved_mem *rmem)
{
	/*
	 * struct cma *cma;
	 * int err;
	 * pr_info("%s setup.\n",__func__);
	 * err = cma_init_reserved_mem(rmem->base, rmem->size, 0, &cma);
	 * if (err) {
	 *      pr_err("Reserved memory: unable to setup CMA region\n");
	 *      return err;
	 * }
	 */
	rmem->ops = &rmem_demod_ops;
	/* rmem->priv = cma; */

	pr_info
	    ("DTV demod reserved memory: %pa, size %ld MiB\n",
	     &rmem->base, (unsigned long)rmem->size / SZ_1M);

	return 0;
}

RESERVEDMEM_OF_DECLARE(demod, "amlogic, demod-mem", rmem_demod_setup);
#endif

/* It's a correspondence with enum es_map_addr*/

void dbg_ic_cfg(struct amldtvdemod_device_s *devp)
{
	struct ss_reg_phy *preg = &devp->reg_p[0];
	int i;

	for (i = 0; i < ES_MAP_ADDR_NUM; i++)
		PR_INFO("reg:%s:st=0x%x,size=0x%x\n",
			name_reg[i], preg[i].phy_addr, preg[i].size);
}

void dbg_reg_addr(struct amldtvdemod_device_s *devp)
{
	struct ss_reg_vt *regv = &devp->reg_v[0];
	int i;

	PR_INFO("%s\n", __func__);

	PR_INFO("reg address offset:\n");
	PR_INFO("\tdemod top:\t0x%x\n", devp->data->regoff.off_demod_top);
	PR_INFO("\tdvbc:\t0x%x\n", devp->data->regoff.off_dvbc);
	PR_INFO("\tdtmb:\t0x%x\n", devp->data->regoff.off_dtmb);
	PR_INFO("\tdvbt/isdbt:\t0x%x\n", devp->data->regoff.off_dvbt_isdbt);
	PR_INFO("\tisdbt:\t0x%x\n", devp->data->regoff.off_isdbt);
	PR_INFO("\tatsc:\t0x%x\n", devp->data->regoff.off_atsc);
	PR_INFO("\tfront:\t0x%x\n", devp->data->regoff.off_front);

	PR_INFO("virtual addr:\n");
	for (i = 0; i < ES_MAP_ADDR_NUM; i++)
		PR_INFO("\t%s:\t0x%p\n", name_reg[i], regv[i].v);
}

static void dtvdemod_set_agc_pinmux(int on)
{
	struct amldtvdemod_device_s *devp = dtvdemod_get_dev();

	if (!devp->pin_name) {
		PR_INFO("no pinmux control\n");
		return;
	}

	if (on) {
		if (devp->agc_pin_enable) {
			PR_INFO("agc_pin_enable is already on\n");
			return;
		}

		devp->pin = devm_pinctrl_get_select(devp->dev,
							"dtvdemod_agc_pins");
		if (IS_ERR_OR_NULL(devp->pin)) {
			devp->pin = NULL;
			PR_ERR("get agc pins fail\n");
		} else {
			devp->agc_pin_enable = true;
		}
	} else {
		if (!devp->agc_pin_enable) {
			PR_INFO("agc_pin_enable is already off\n");
			return;
		}

		/*off*/
		if (!IS_ERR_OR_NULL(devp->pin)) {
			devm_pinctrl_put(devp->pin);
			devp->pin = NULL;
		}

		devp->agc_pin_enable = false;
	}
}

static void dtvdemod_clktree_probe(struct device *dev)
{
	struct amldtvdemod_device_s *devp = dtvdemod_get_dev();

	devp->clk_gate_state = 0;

	devp->vdac_clk_gate = devm_clk_get(dev, "vdac_clk_gate");
	if (IS_ERR_OR_NULL(devp->vdac_clk_gate))
		PR_ERR("error: %s: clk vdac_clk_gate\n", __func__);
}

static void dtvdemod_clktree_remove(struct device *dev)
{
	struct amldtvdemod_device_s *devp = dtvdemod_get_dev();

	if (!IS_ERR_OR_NULL(devp->vdac_clk_gate))
		devm_clk_put(dev, devp->vdac_clk_gate);
}

static void vdac_clk_gate_ctrl(int status)
{
	struct amldtvdemod_device_s *devp = dtvdemod_get_dev();

	if (status) {
		if (devp->clk_gate_state) {
			PR_INFO("clk_gate is already on\n");
			return;
		}

		if (IS_ERR(devp->vdac_clk_gate))
			PR_ERR("error: %s: vdac_clk_gate\n", __func__);
		else
			clk_prepare_enable(devp->vdac_clk_gate);

		devp->clk_gate_state = 1;
	} else {
		if (devp->clk_gate_state == 0) {
			PR_INFO("clk_gate is already off\n");
			return;
		}

		if (IS_ERR(devp->vdac_clk_gate))
			PR_ERR("error: %s: vdac_clk_gate\n", __func__);
		else
			clk_disable_unprepare(devp->vdac_clk_gate);

		devp->clk_gate_state = 0;
	}
}
/*
 * use dtvdemod_vdac_enable replace vdac_enable
 */
static void dtvdemod_vdac_enable(bool on)
{
	struct amldtvdemod_device_s *devp = dtvdemod_get_dev();

	if (on) {
		vdac_clk_gate_ctrl(1);
		if (!devp->vdac_enable) {
			vdac_enable(1, VDAC_MODULE_DTV_DEMOD);
			devp->vdac_enable = true;
		}
	} else {
		vdac_clk_gate_ctrl(0);
		if (devp->vdac_enable) {
			vdac_enable(0, VDAC_MODULE_DTV_DEMOD);
			devp->vdac_enable = false;
		}
	}
}

static int dtvdemod_request_firmware(const char *file_name, char *buf, int size)
{
	int ret = -1;
	const struct firmware *fw;
	int offset = 0;
	unsigned int i;
	struct amldtvdemod_device_s *devp = dtvdemod_get_dev();

	if (!buf) {
		pr_err("%s fw buf is NULL\n", __func__);
		ret = -ENOMEM;
		goto err;
	}

	ret = request_firmware(&fw, file_name, devp->dev);
	if (ret < 0) {
		pr_err("%d can't load the %s.\n", ret, file_name);
		goto err;
	}

	if (fw->size > size) {
		pr_err("not enough memory size for fw.\n");
		ret = -ENOMEM;
		goto release;
	}

	memcpy(buf, (char *)fw->data + offset, fw->size - offset);
	ret = fw->size;

	PR_DBGL("dtvdemod_request_firmware:\n");
	for (i = 0; i < 100; i++)
		PR_DBGL("[%d] = 0x%x\n", i, fw->data[i]);
release:
	release_firmware(fw);
err:
	return ret;
}

static int fw_check_sum(char *buf, unsigned int len)
{
	unsigned int crc;

	crc = crc32_le(~0U, buf, len);

	PR_INFO("firmware crc result : 0x%x, len: %d\n", crc ^ ~0U, len);

	/* return fw->head.checksum != (crc ^ ~0U) ? 0 : 1; */
	return 0;
}

static int dtvdemod_download_firmware(struct amldtvdemod_device_s *devp)
{
	char *path = __getname();
	int len = 0;
	int size = 0;

	if (!path)
		return -ENOMEM;

	len = snprintf(path, PATH_MAX_LEN, "%s/%s", FIRMWARE_DIR, FIRMWARE_NAME);
	if (len >= PATH_MAX_LEN)
		goto path_fail;

	strncpy(devp->firmware_path, path, sizeof(devp->firmware_path));
	devp->firmware_path[sizeof(devp->firmware_path) - 1] = '\0';

	size = dtvdemod_request_firmware(devp->firmware_path, devp->fw_buf, FW_BUFF_SIZE);

	if (size >= 0)
		fw_check_sum(devp->fw_buf, size);

path_fail:
	__putname(path);

	return size;
}

static void dtvdemod_fw_dwork(struct work_struct *work)
{
	int ret;
	static unsigned int cnt;
	struct delayed_work *dwork = to_delayed_work(work);
	struct amldtvdemod_device_s *devp =
		container_of(dwork, struct amldtvdemod_device_s, fw_dwork);

	if (!devp) {
		pr_info("%s, dwork error !!!\n", __func__);
		return;
	}

	ret = dtvdemod_download_firmware(devp);

	if ((ret < 0) && (cnt < 10))
		schedule_delayed_work(&devp->fw_dwork, 5 * HZ);
	else
		cancel_delayed_work(&devp->fw_dwork);

	cnt++;
}

/* platform driver*/
static int aml_dtvdemod_probe(struct platform_device *pdev)
{
	int ret = 0;
	const struct of_device_id *match;

	PR_INFO("%s\n", __func__);
	/*memory*/
	dtvdd_devp = kzalloc(sizeof(*dtvdd_devp), GFP_KERNEL);
	if (!dtvdd_devp)
		goto fail_alloc_region;

	dtvdd_devp->dev = &pdev->dev;
	platform_set_drvdata(pdev, dtvdd_devp);

	dtvdd_devp->state = DTVDEMOD_ST_NOT_INI;

	/*class attr */
	dtvdd_devp->clsp = class_create(THIS_MODULE, DEMOD_DEVICE_NAME);
	if (!dtvdd_devp->clsp)
		goto fail_create_class;

	ret = dtvdemod_create_class_files(dtvdd_devp->clsp);
	if (ret)
		goto fail_class_create_file;

	match = of_match_device(meson_ddemod_match, &pdev->dev);
	if (match == NULL) {
		PR_ERR("%s,no matched table\n", __func__);
		goto fail_ic_config;
	}
	dtvdd_devp->data = (struct meson_ddemod_data *)match->data;

	/*reg*/
	ret = dds_init_reg_map(pdev);
	if (ret)
		goto fail_ic_config;

	/*mem info from dts*/
	ret = dtvdemod_set_iccfg_by_dts(pdev);
	if (ret)
		goto fail_ic_config;
		/*dtvdemod_set_iccfg_by_cputype();*/

	/*debug:*/
	dbg_ic_cfg(dtvdd_devp);
	dbg_reg_addr(dtvdd_devp);

	/**/
	dtvpll_lock_init();
	demod_init_mutex();
	INIT_LIST_HEAD(&dtvdd_devp->demod_list);
	mutex_init(&dtvdd_devp->lock);
	mutex_init(&dtvdd_devp->mutex_tx_msg);

	dtvdemod_clktree_probe(&pdev->dev);

	dtvdd_devp->state = DTVDEMOD_ST_IDLE;
	dtvdemod_version(dtvdd_devp);
	dtvdd_devp->flg_cma_allc = false;
	dtvdd_devp->demod_thread = 1;
	//ary temp:
	aml_demod_init();

	if (dtvdd_devp->data->hw_ver >= DTVDEMOD_HW_T5D) {
		pm_runtime_enable(dtvdd_devp->dev);
		if (pm_runtime_get_sync(dtvdd_devp->dev) < 0)
			pr_err("failed to set pwr\n");

		dtvdd_devp->fw_buf = kzalloc(FW_BUFF_SIZE, GFP_KERNEL);
		if (!dtvdd_devp->fw_buf)
			ret = -ENOMEM;

		if (dtvdd_devp->data->hw_ver != DTVDEMOD_HW_S4) {
			INIT_DELAYED_WORK(&dtvdd_devp->fw_dwork, dtvdemod_fw_dwork);
			schedule_delayed_work(&dtvdd_devp->fw_dwork, 10 * HZ);
		}
	}

	PR_INFO("[amldtvdemod.] : version: %s (%s), probe ok.\n",
			AMLDTVDEMOD_VER, DTVDEMOD_VER);

	return 0;
fail_ic_config:
	PR_ERR("ic config error.\n");
fail_class_create_file:
	PR_ERR("dtvdemod class file create error.\n");
	class_destroy(dtvdd_devp->clsp);
fail_create_class:
	PR_ERR("dtvdemod class create error.\n");
	kfree(dtvdd_devp);
fail_alloc_region:
	PR_ERR("dtvdemod alloc error.\n");
	PR_ERR("dtvdemod_init fail.\n");

	return ret;
}

static int __exit aml_dtvdemod_remove(struct platform_device *pdev)
{
	struct amldtvdemod_device_s *devp =
			(struct amldtvdemod_device_s *)platform_get_drvdata(pdev);

	mutex_lock(&amldtvdemod_device_mutex);

	if (!devp) {
		mutex_unlock(&amldtvdemod_device_mutex);

		return -EFAULT;
	}

	dtvdemod_clktree_remove(&pdev->dev);
	mutex_destroy(&devp->lock);
	dtvdemod_remove_class_files(devp->clsp);
	class_destroy(devp->clsp);

	if (devp->data->hw_ver >= DTVDEMOD_HW_T5D) {
		kfree(devp->fw_buf);
		pm_runtime_put_sync(devp->dev);
		pm_runtime_disable(devp->dev);
	}

	aml_demod_exit();

	list_del_init(&devp->demod_list);

	kfree(devp);

	dtvdd_devp = NULL;

	PR_INFO("%s:remove.\n", __func__);

	mutex_unlock(&amldtvdemod_device_mutex);

	return 0;
}

static void aml_dtvdemod_shutdown(struct platform_device *pdev)
{
	struct amldtvdemod_device_s *devp =
			(struct amldtvdemod_device_s *)platform_get_drvdata(pdev);
	struct aml_dtvdemod *demod = NULL;

	mutex_lock(&amldtvdemod_device_mutex);

	if (!devp) {
		mutex_unlock(&amldtvdemod_device_mutex);

		return;
	}

	list_for_each_entry(demod, &devp->demod_list, list) {
		/* It will be waked up when it is re-tune.
		 * So it don't have to call the internal resume function.
		 * But need to reinitialize it.
		 */
		if (devp->state != DTVDEMOD_ST_IDLE) {
			if (demod->last_delsys != SYS_UNDEFINED)
				leave_mode(demod, demod->last_delsys);
		}
	}

	devp->state = DTVDEMOD_ST_IDLE;

#ifdef CONFIG_AMLOGIC_MEDIA_ADC
	adc_pll_down();
#endif

	mutex_unlock(&amldtvdemod_device_mutex);
}

static int aml_dtvdemod_suspend(struct platform_device *pdev,
					pm_message_t state)
{
	struct amldtvdemod_device_s *devp =
			(struct amldtvdemod_device_s *)platform_get_drvdata(pdev);
	int ret = 0;

	mutex_lock(&amldtvdemod_device_mutex);

	if (!devp) {
		mutex_unlock(&amldtvdemod_device_mutex);

		return -EFAULT;
	}

	ret = dtvdemod_leave_mode(devp);

	PR_INFO("%s state event %d, ret %d, OK\n", __func__, state.event, ret);

	mutex_unlock(&amldtvdemod_device_mutex);

	return 0;
}

static int aml_dtvdemod_resume(struct platform_device *pdev)
{
	PR_INFO("%s is called\n", __func__);

	return 0;
}

static int dtvdemod_leave_mode(struct amldtvdemod_device_s *devp)
{
	enum fe_delivery_system delsys = SYS_UNDEFINED;
	struct aml_dtvdemod *demod = NULL;

	if (IS_ERR_OR_NULL(devp))
		return -EFAULT;

	list_for_each_entry(demod, &devp->demod_list, list) {
		/* It will be waked up when it is re-tune.
		 * So it don't have to call the internal resume function.
		 * But need to reinitialize it.
		 */
		delsys = demod->last_delsys;
		PR_INFO("%s, delsys = %s\n", __func__, name_fe_delivery_system[delsys]);
		if (delsys != SYS_UNDEFINED)
			leave_mode(demod, delsys);
	}

	return 0;
}

static __maybe_unused int dtv_demod_pm_suspend(struct device *dev)
{
	int ret = 0;
	struct platform_device *pdev = to_platform_device(dev);
	struct amldtvdemod_device_s *devp =
			(struct amldtvdemod_device_s *)platform_get_drvdata(pdev);

	ret = dtvdemod_leave_mode(devp);

	PR_INFO("%s ret %d, OK.\n", __func__, ret);

	return 0;
}

static __maybe_unused int dtv_demod_pm_resume(struct device *dev)
{
	PR_INFO("%s OK.\n", __func__);

	return 0;
}

static int __maybe_unused dtv_demod_runtime_suspend(struct device *dev)
{
	return 0;
}

static int __maybe_unused dtv_demod_runtime_resume(struct device *dev)
{
	return 0;
}

static const struct dev_pm_ops dtv_demod_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(dtv_demod_pm_suspend, dtv_demod_pm_resume)
	SET_RUNTIME_PM_OPS(dtv_demod_runtime_suspend,
			   dtv_demod_runtime_resume, NULL)
};

static struct platform_driver aml_dtvdemod_driver = {
	.driver = {
		.name = "aml_dtv_demod",
		.owner = THIS_MODULE,
		.pm = &dtv_demod_pm_ops,
		/*aml_dtvdemod_dt_match*/
		.of_match_table = meson_ddemod_match,
	},
	.shutdown   = aml_dtvdemod_shutdown,
	.probe = aml_dtvdemod_probe,
	.remove = __exit_p(aml_dtvdemod_remove),
	.suspend  = aml_dtvdemod_suspend,
	.resume   = aml_dtvdemod_resume,
};


int __init aml_dtvdemod_init(void)
{
	if (platform_driver_register(&aml_dtvdemod_driver)) {
		pr_err("failed to register amldtvdemod driver module\n");
		return -ENODEV;
	}

	PR_INFO("[amldtvdemod..]%s.\n", __func__);
	return 0;
}

void __exit aml_dtvdemod_exit(void)
{
	platform_driver_unregister(&aml_dtvdemod_driver);
	PR_INFO("[amldtvdemod..]%s: driver removed ok.\n", __func__);
}

static int delsys_set(struct dvb_frontend *fe, unsigned int delsys)
{
	struct aml_dtvdemod *demod = (struct aml_dtvdemod *)fe->demodulator_priv;
	enum fe_delivery_system ldelsys = demod->last_delsys;
	enum fe_delivery_system cdelsys = delsys;

	int ncaps, support;

	/* T/T2 have to do top rst every time enter mode */
	if (ldelsys == cdelsys && cdelsys != SYS_DVBT && cdelsys != SYS_DVBT2)
		return 0;

	ncaps = 0;
	support = 0;
	while (ncaps < MAX_DELSYS && fe->ops.delsys[ncaps]) {
		if (fe->ops.delsys[ncaps] == cdelsys) {

			support = 1;
			break;
		}
		ncaps++;
	}

	if (!support) {
		PR_INFO("[id %d] delsys:%d is not support!\n", demod->id, cdelsys);
		return 0;
	}

	if (ldelsys <= END_SYS_DELIVERY && cdelsys <= END_SYS_DELIVERY) {
		PR_DBG("%s [id %d]: delsys last=%s, cur=%s\n",
				__func__, demod->id,
				name_fe_delivery_system[ldelsys],
				name_fe_delivery_system[cdelsys]);
	} else
		PR_ERR("%s [id %d]: delsys last=%d, cur=%d\n",
				__func__, demod->id, ldelsys, cdelsys);

	switch (cdelsys) {
	case SYS_DVBC_ANNEX_A:
	case SYS_DVBC_ANNEX_C:
		/* dvbc */
		fe->ops.info.type = FE_QAM;
		break;

	case SYS_ATSC:
	case SYS_ATSCMH:
	case SYS_DVBC_ANNEX_B:
		/* atsc */
		fe->ops.info.type = FE_ATSC;
		break;

	case SYS_DVBT:
	case SYS_DVBT2:
		/* dvbt, OFDM */
		fe->ops.info.type = FE_OFDM;
		break;

	case SYS_ISDBT:
		fe->ops.info.type = FE_ISDBT;
		break;

	case SYS_DTMB:
		/* dtmb */
		fe->ops.info.type = FE_DTMB;
		break;

	case SYS_DVBS:
	case SYS_DVBS2:
		/* QPSK */
		fe->ops.info.type = FE_QPSK;
		break;

	case SYS_DSS:
	case SYS_DVBH:
	case SYS_ISDBS:
	case SYS_ISDBC:
	case SYS_CMMB:
	case SYS_DAB:
	case SYS_TURBO:
	case SYS_UNDEFINED:
		return 0;

#ifdef CONFIG_AMLOGIC_DVB_COMPAT
	case SYS_ANALOG:
		if (get_dtvpll_init_flag()) {
			PR_INFO("delsys not support : %d\n", cdelsys);
			leave_mode(demod, ldelsys);

			if (fe->ops.tuner_ops.release)
				fe->ops.tuner_ops.release(fe);
		}

		return 0;
#endif
	}

	if (cdelsys != SYS_UNDEFINED) {
		if (ldelsys != SYS_UNDEFINED) {
			leave_mode(demod, ldelsys);

			if (fe->ops.tuner_ops.release)
				fe->ops.tuner_ops.release(fe);
		}

		if (enter_mode(demod, cdelsys)) {
			PR_INFO("enter_mode failed,leave!\n");
			leave_mode(demod, cdelsys);

			if (fe->ops.tuner_ops.release)
				fe->ops.tuner_ops.release(fe);
			return 0;
		}
	}

	if (!get_dtvpll_init_flag()) {
		PR_INFO("pll is not set!\n");
		leave_mode(demod, cdelsys);

		if (fe->ops.tuner_ops.release)
			fe->ops.tuner_ops.release(fe);
		return 0;
	}

	demod->last_delsys = cdelsys;
	PR_INFO("[id %d] info fe type:%d.\n", demod->id, fe->ops.info.type);

	if (fe->ops.tuner_ops.set_config)
		fe->ops.tuner_ops.set_config(fe, NULL);

	return 0;
}

static int is_not_active(struct dvb_frontend *fe)
{
	struct aml_dtvdemod *demod = (struct aml_dtvdemod *)fe->demodulator_priv;
	enum fe_delivery_system cdelsys;
	enum fe_delivery_system ldelsys = demod->last_delsys;

	if (!get_dtvpll_init_flag())
		return 1;

	cdelsys = fe->dtv_property_cache.delivery_system;
	if (ldelsys != cdelsys)
		return 2;


	return 0;/*active*/
}

static int aml_dtvdm_init(struct dvb_frontend *fe)
{
	struct aml_dtvdemod *demod = (struct aml_dtvdemod *)fe->demodulator_priv;
	struct amldtvdemod_device_s *devp = (struct amldtvdemod_device_s *)demod->priv;

	mutex_lock(&devp->lock);

	demod->suspended = false;
	demod->last_delsys = SYS_UNDEFINED;

	PR_INFO("%s [id %d] OK.\n", __func__, demod->id);

	mutex_unlock(&devp->lock);

	return 0;
}

static int aml_dtvdm_sleep(struct dvb_frontend *fe)
{
	struct aml_dtvdemod *demod = (struct aml_dtvdemod *)fe->demodulator_priv;
	struct amldtvdemod_device_s *devp = (struct amldtvdemod_device_s *)demod->priv;
	enum fe_delivery_system delsys = SYS_UNDEFINED;

	mutex_lock(&devp->lock);

	delsys = demod->last_delsys;

	if (get_dtvpll_init_flag()) {
		PR_INFO("%s\n", __func__);
		if (delsys != SYS_UNDEFINED)
			leave_mode(demod, delsys);

		if (fe->ops.tuner_ops.release)
			fe->ops.tuner_ops.release(fe);
	}

	PR_INFO("%s [id %d] OK.\n", __func__, demod->id);

	mutex_unlock(&devp->lock);

	return 0;
}

static int aml_dtvdm_set_parameters(struct dvb_frontend *fe)
{
	struct aml_dtvdemod *demod = (struct aml_dtvdemod *)fe->demodulator_priv;
	struct amldtvdemod_device_s *devp = (struct amldtvdemod_device_s *)demod->priv;
	enum fe_delivery_system delsys = SYS_UNDEFINED;
	int ret = 0;
	struct dtv_frontend_properties *c = NULL;

	mutex_lock(&devp->lock);

	delsys = demod->last_delsys;
	c = &fe->dtv_property_cache;

	PR_DBGL("%s [id %d] delsys %d %s.\n", __func__,
			demod->id, delsys, name_fe_delivery_system[delsys]);
	PR_DBGL("[id %d] delsys=%d freq=%d,symbol_rate=%d,bw=%d,modulation=%d,inversion=%d.\n",
		demod->id, c->delivery_system, c->frequency,
		c->symbol_rate, c->bandwidth_hz, c->modulation,
		c->inversion);

	if (is_not_active(fe)) {
		PR_DBG("[id %d] set parm:not active\n", demod->id);
		mutex_unlock(&devp->lock);
		return 0;
	}

	switch (delsys) {
	case SYS_DVBS:
	case SYS_DVBS2:
		break;

	case SYS_DVBC_ANNEX_A:
	case SYS_DVBC_ANNEX_C:
		timer_begain(demod, D_TIMER_DETECT);
		demod->en_detect = 1; /*fist set*/
		ret = gxtv_demod_dvbc_set_frontend(fe);
		break;

	case SYS_DVBT:
	case SYS_DVBT2:
		break;

	case SYS_ISDBT:
		timer_begain(demod, D_TIMER_DETECT);
		demod->en_detect = 1; /*fist set*/
		ret = dvbt_isdbt_set_frontend(fe);
		break;

	case SYS_ATSC:
	case SYS_ATSCMH:
	case SYS_DVBC_ANNEX_B:
		ret = gxtv_demod_atsc_set_frontend(fe);
		break;

	case SYS_DTMB:
		ret = gxtv_demod_dtmb_set_frontend(fe);
		break;

	case SYS_UNDEFINED:
	default:
		break;
	}

	mutex_unlock(&devp->lock);

	return ret;
}

static int aml_dtvdm_get_frontend(struct dvb_frontend *fe,
				 struct dtv_frontend_properties *p)
{
	struct aml_dtvdemod *demod = (struct aml_dtvdemod *)fe->demodulator_priv;
	struct amldtvdemod_device_s *devp = (struct amldtvdemod_device_s *)demod->priv;
	enum fe_delivery_system delsys = SYS_UNDEFINED;
	int ret = 0;

	mutex_lock(&devp->lock);

	delsys = demod->last_delsys;

	if (is_not_active(fe)) {
		PR_DBGL("[id %d] get parm:not active\n", demod->id);
		mutex_unlock(&devp->lock);
		return 0;
	}

	switch (delsys) {
	case SYS_DVBS:
	case SYS_DVBS2:
		break;

	case SYS_DVBC_ANNEX_A:
	case SYS_DVBC_ANNEX_C:
		ret = gxtv_demod_dvbc_get_frontend(fe);
		break;

	case SYS_DVBT:
	case SYS_DVBT2:
		ret = gxtv_demod_dvbt_get_frontend(fe);
		break;

	case SYS_ISDBT:
		ret = gxtv_demod_dvbt_get_frontend(fe);
		break;

	case SYS_ATSC:
	case SYS_ATSCMH:
	case SYS_DVBC_ANNEX_B:
		ret = gxtv_demod_atsc_get_frontend(fe);
		break;

	case SYS_DTMB:
		ret = gxtv_demod_dtmb_get_frontend(fe);
		break;

	case SYS_UNDEFINED:
	default:
		break;
	}

	mutex_unlock(&devp->lock);

	return ret;
}
static int aml_dtvdm_get_tune_settings(struct dvb_frontend *fe,
				      struct dvb_frontend_tune_settings
					*fe_tune_settings)
{
	struct aml_dtvdemod *demod = (struct aml_dtvdemod *)fe->demodulator_priv;
	struct amldtvdemod_device_s *devp = (struct amldtvdemod_device_s *)demod->priv;
	enum fe_delivery_system delsys = SYS_UNDEFINED;
	int ret = 0;

	mutex_lock(&devp->lock);

	delsys = demod->last_delsys;

	if (is_not_active(fe)) {
		PR_DBGL("[id %d] get parm:not active\n", demod->id);
		mutex_unlock(&devp->lock);
		return 0;
	}

	switch (delsys) {
	case SYS_DVBS:
	case SYS_DVBS2:
		break;

	case SYS_DVBC_ANNEX_A:
	case SYS_DVBC_ANNEX_C:
		fe_tune_settings->min_delay_ms = 300;
		fe_tune_settings->step_size = 0; /* no zigzag */
		fe_tune_settings->max_drift = 0;
		break;

	case SYS_DVBT:
	case SYS_DVBT2:
		fe_tune_settings->min_delay_ms = 500;
		fe_tune_settings->step_size = 0;
		fe_tune_settings->max_drift = 0;
		break;

	case SYS_ISDBT:
		fe_tune_settings->min_delay_ms = 300;
		fe_tune_settings->step_size = 0;
		fe_tune_settings->max_drift = 0;
		break;

	case SYS_ATSC:
	case SYS_ATSCMH:
	case SYS_DVBC_ANNEX_B:
		break;

	case SYS_DTMB:
		break;

	case SYS_UNDEFINED:
	default:
		break;
	}

	mutex_unlock(&devp->lock);

	return ret;
}
static int aml_dtvdm_read_status(struct dvb_frontend *fe,
					enum fe_status *status)
{
	struct aml_dtvdemod *demod = (struct aml_dtvdemod *)fe->demodulator_priv;
	struct amldtvdemod_device_s *devp = (struct amldtvdemod_device_s *)demod->priv;
	enum fe_delivery_system delsys = SYS_UNDEFINED;
	int ret = 0;

	mutex_lock(&devp->lock);

	delsys = demod->last_delsys;

	if (delsys == SYS_UNDEFINED) {
		mutex_unlock(&devp->lock);
		return 0;
	}

	if (is_not_active(fe)) {
		PR_DBGL("[id %d] read status:not active\n", demod->id);
		mutex_unlock(&devp->lock);
		return 0;
	}

	switch (delsys) {
	case SYS_DVBS:
	case SYS_DVBS2:
		ret = dtvdemod_dvbs_read_status(fe, status);
		break;

	case SYS_DVBC_ANNEX_A:
	case SYS_DVBC_ANNEX_C:
		ret = gxtv_demod_dvbc_read_status_timer(fe, status);
		break;

	case SYS_DVBT:
		ret = dvbt_read_status(fe, status);
		break;

	case SYS_DVBT2:
		ret = dvbt2_read_status(fe, status);
		break;

	case SYS_ISDBT:
		ret = dvbt_isdbt_read_status(fe, status);
		break;

	case SYS_ATSC:
	case SYS_ATSCMH:
	case SYS_DVBC_ANNEX_B:
		ret = gxtv_demod_atsc_read_status(fe, status);
		break;

	case SYS_DTMB:
		ret = gxtv_demod_dtmb_read_status(fe, status);
		break;

	default:
		break;
	}

	mutex_unlock(&devp->lock);

	return ret;
}

static int aml_dtvdm_read_ber(struct dvb_frontend *fe, u32 *ber)
{
	struct aml_dtvdemod *demod = (struct aml_dtvdemod *)fe->demodulator_priv;
	struct amldtvdemod_device_s *devp = (struct amldtvdemod_device_s *)demod->priv;
	enum fe_delivery_system delsys = SYS_UNDEFINED;
	int ret = 0;

	mutex_lock(&devp->lock);

	delsys = demod->last_delsys;

	if (is_not_active(fe)) {
		PR_DBGL("[id %d] read ber:not active\n", demod->id);
		mutex_unlock(&devp->lock);
		return 0;
	}

	switch (delsys) {
	case SYS_DVBS:
	case SYS_DVBS2:
		break;

	case SYS_DVBC_ANNEX_A:
	case SYS_DVBC_ANNEX_C:
		ret = gxtv_demod_dvbc_read_ber(fe, ber);
		break;

	case SYS_DVBT:
		ret = dvbt_read_ber(fe, ber);
		break;

	case SYS_DVBT2:
		break;

	case SYS_ISDBT:
		ret = dvbt_isdbt_read_ber(fe, ber);
		break;

	case SYS_ATSC:
	case SYS_ATSCMH:
	case SYS_DVBC_ANNEX_B:
		ret = gxtv_demod_atsc_read_ber(fe, ber);
		break;

	case SYS_DTMB:
		ret = gxtv_demod_dtmb_read_ber(fe, ber);
		break;

	default:
		break;
	}

	mutex_unlock(&devp->lock);

	return ret;
}

static int aml_dtvdm_read_signal_strength(struct dvb_frontend *fe,
					 u16 *strength)
{
	struct aml_dtvdemod *demod = (struct aml_dtvdemod *)fe->demodulator_priv;
	struct amldtvdemod_device_s *devp = (struct amldtvdemod_device_s *)demod->priv;
	enum fe_delivery_system delsys = SYS_UNDEFINED;
	int ret = 0;

	mutex_lock(&devp->lock);

	delsys = demod->last_delsys;

	if (is_not_active(fe)) {
		PR_DBGL("[id %d] read strength:not active\n", demod->id);
		mutex_unlock(&devp->lock);
		return 0;
	}

	switch (delsys) {
	case SYS_DVBS:
	case SYS_DVBS2:
		ret = dtvdemod_dvbs_read_signal_strength(fe, strength);
		break;

	case SYS_DVBC_ANNEX_A:
	case SYS_DVBC_ANNEX_C:
		ret = gxtv_demod_dvbc_read_signal_strength(fe, strength);
		break;

	case SYS_DVBT:
	case SYS_DVBT2:
		ret = gxtv_demod_dvbt_read_signal_strength(fe, strength);
		break;

	case SYS_ISDBT:
		ret = gxtv_demod_dvbt_read_signal_strength(fe, strength);
		break;

	case SYS_ATSC:
	case SYS_ATSCMH:
	case SYS_DVBC_ANNEX_B:
		ret = gxtv_demod_atsc_read_signal_strength(fe, strength);
		break;

	case SYS_DTMB:
		ret = gxtv_demod_dtmb_read_signal_strength(fe, strength);
		break;

	default:
		break;
	}

	mutex_unlock(&devp->lock);

	return ret;
}

static int aml_dtvdm_read_snr(struct dvb_frontend *fe, u16 *snr)
{
	struct aml_dtvdemod *demod = (struct aml_dtvdemod *)fe->demodulator_priv;
	struct amldtvdemod_device_s *devp = (struct amldtvdemod_device_s *)demod->priv;
	enum fe_delivery_system delsys = SYS_UNDEFINED;
	int ret = 0;

	mutex_lock(&devp->lock);

	delsys = demod->last_delsys;

	if (is_not_active(fe)) {
		PR_DBGL("[id %d] read snr :not active\n", demod->id);
		mutex_unlock(&devp->lock);
		return 0;
	}

	switch (delsys) {
	case SYS_DVBS:
	case SYS_DVBS2:
		ret = dvbs_read_snr(fe, snr);
		break;

	case SYS_DVBC_ANNEX_A:
	case SYS_DVBC_ANNEX_C:
		ret = gxtv_demod_dvbc_read_snr(fe, snr);
		break;

	case SYS_DVBT:
		ret = dvbt_read_snr(fe, snr);
		break;

	case SYS_DVBT2:
		ret = dvbt2_read_snr(fe, snr);
		break;

	case SYS_ISDBT:
		ret = gxtv_demod_dvbt_isdbt_read_snr(fe, snr);
		break;

	case SYS_ATSC:
	case SYS_ATSCMH:
	case SYS_DVBC_ANNEX_B:
		ret = gxtv_demod_atsc_read_snr(fe, snr);
		break;

	case SYS_DTMB:
		ret = gxtv_demod_dtmb_read_snr(fe, snr);
		break;

	default:
		break;
	}

	mutex_unlock(&devp->lock);

	return ret;
}

static int aml_dtvdm_read_ucblocks(struct dvb_frontend *fe, u32 *ucblocks)
{
	struct aml_dtvdemod *demod = (struct aml_dtvdemod *)fe->demodulator_priv;
	struct amldtvdemod_device_s *devp = (struct amldtvdemod_device_s *)demod->priv;
	enum fe_delivery_system delsys = SYS_UNDEFINED;
	int ret = 0;

	mutex_lock(&devp->lock);

	delsys = demod->last_delsys;

	if (is_not_active(fe)) {
		PR_DBGL("[id %d] read ucblocks :not active\n", demod->id);
		mutex_unlock(&devp->lock);
		return 0;
	}

	switch (delsys) {
	case SYS_DVBS:
	case SYS_DVBS2:
		break;

	case SYS_DVBC_ANNEX_A:
	case SYS_DVBC_ANNEX_C:
		ret = gxtv_demod_dvbc_read_ucblocks(fe, ucblocks);
		break;

	case SYS_DVBT:
	case SYS_DVBT2:
		ret = gxtv_demod_dvbt_read_ucblocks(fe, ucblocks);
		break;

	case SYS_ISDBT:
		ret = gxtv_demod_dvbt_read_ucblocks(fe, ucblocks);
		break;

	case SYS_ATSC:
	case SYS_ATSCMH:
	case SYS_DVBC_ANNEX_B:
		ret = gxtv_demod_atsc_read_ucblocks(fe, ucblocks);
		break;

	case SYS_DTMB:
		ret = gxtv_demod_dtmb_read_ucblocks(fe, ucblocks);
		break;

	default:
		break;
	}

	mutex_unlock(&devp->lock);

	return ret;
}

static void aml_dtvdm_release(struct dvb_frontend *fe)
{
	struct aml_dtvdemod *demod = (struct aml_dtvdemod *)fe->demodulator_priv;
	struct amldtvdemod_device_s *devp = (struct amldtvdemod_device_s *)demod->priv;
	enum fe_delivery_system delsys = SYS_UNDEFINED;

	mutex_lock(&devp->lock);

	delsys = demod->last_delsys;

	switch (delsys) {
	case SYS_DVBS:
	case SYS_DVBS2:
		break;

	case SYS_DVBC_ANNEX_A:
	case SYS_DVBC_ANNEX_C:
		gxtv_demod_dvbc_release(fe);
		break;

	case SYS_DVBT:
	case SYS_DVBT2:
		break;

	case SYS_ISDBT:
		gxtv_demod_dvbt_release(fe);
		break;

	case SYS_ATSC:
	case SYS_ATSCMH:
	case SYS_DVBC_ANNEX_B:
		gxtv_demod_atsc_release(fe);
		break;

	case SYS_DTMB:
		gxtv_demod_dtmb_release(fe);
		break;

	default:
		break;
	}

	if (get_dtvpll_init_flag()) {
		PR_INFO("%s\n", __func__);
		if (delsys != SYS_UNDEFINED)
			leave_mode(demod, delsys);

		if (fe->ops.tuner_ops.release)
			fe->ops.tuner_ops.release(fe);
	}

	PR_INFO("%s [id %d] OK.\n", __func__, demod->id);

	mutex_unlock(&devp->lock);
}

static int aml_dtvdm_tune(struct dvb_frontend *fe, bool re_tune,
	unsigned int mode_flags, unsigned int *delay, enum fe_status *status)
{
	struct aml_dtvdemod *demod = (struct aml_dtvdemod *)fe->demodulator_priv;
	struct amldtvdemod_device_s *devp = (struct amldtvdemod_device_s *)demod->priv;
	enum fe_delivery_system delsys = SYS_UNDEFINED;
	int ret = 0;
	static int flg; /*debug only*/

	mutex_lock(&devp->lock);

	delsys = demod->last_delsys;

	if (delsys == SYS_UNDEFINED) {
		*delay = HZ * 5;
		*status = 0;
		mutex_unlock(&devp->lock);
		return 0;
	}

	if (is_not_active(fe)) {
		*status = 0;
		PR_INFO("[id %d] tune :not active\n", demod->id);
		mutex_unlock(&devp->lock);
		return 0;
	}

	if ((flg > 0) && (flg < 5))
		PR_INFO("%s [id %d]\n", __func__, demod->id);

	switch (delsys) {
	case SYS_DVBS:
	case SYS_DVBS2:
		ret = dtvdemod_dvbs_tune(fe, re_tune, mode_flags, delay, status);
		break;

	case SYS_DVBC_ANNEX_A:
	case SYS_DVBC_ANNEX_C:
		gxtv_demod_dvbc_tune(fe, re_tune, mode_flags, delay, status);
		break;

	case SYS_DVBT:
		ret = dvbt_tune(fe, re_tune, mode_flags, delay, status);
		break;

	case SYS_DVBT2:
		ret = dvbt2_tune(fe, re_tune, mode_flags, delay, status);
		break;

	case SYS_ISDBT:
		ret = dvbt_isdbt_tune(fe, re_tune, mode_flags, delay, status);
		break;

	case SYS_ATSC:
	case SYS_ATSCMH:
	case SYS_DVBC_ANNEX_B:
		ret = gxtv_demod_atsc_tune(fe, re_tune, mode_flags, delay, status);
		flg++;
		break;

	case SYS_DTMB:
		ret = gxtv_demod_dtmb_tune(fe, re_tune, mode_flags, delay, status);
		break;

	default:
		flg = 0;
		break;
	}

	mutex_unlock(&devp->lock);

	return ret;
}

static int aml_dtvdm_set_property(struct dvb_frontend *fe,
			struct dtv_property *tvp)
{
	struct aml_dtvdemod *demod = (struct aml_dtvdemod *)fe->demodulator_priv;
	struct amldtvdemod_device_s *devp = (struct amldtvdemod_device_s *)demod->priv;
	int r = 0;
	u32 delsys = SYS_UNDEFINED;

	mutex_lock(&devp->lock);

	switch (tvp->cmd) {
	case DTV_DELIVERY_SYSTEM:
		delsys = tvp->u.data;
		delsys_set(fe, delsys);
		break;

	case DTV_DVBT2_PLP_ID:
		demod->plp_id = tvp->u.data;
		PR_INFO("[id %d] DTV_DVBT2_PLP_ID, %d\n", demod->id, demod->plp_id);

		if (demod->last_delsys == SYS_DVBT2) {
			demod_top_write_reg(DEMOD_TOP_REGC, 0x11);
			demod_top_write_reg(DEMOD_TOP_REGC, 0x110011);
			demod_top_write_reg(DEMOD_TOP_REGC, 0x110010);
			usleep_range(1000, 1001);
			demod_top_write_reg(DEMOD_TOP_REGC, 0x110011);
			front_write_bits(AFIFO_ADC, 0x80, AFIFO_NCO_RATE_BIT,
					 AFIFO_NCO_RATE_WID);
			front_write_reg(0x22, 0x7200a06);
			front_write_reg(0x2f, 0x0);
			front_write_reg(0x39, 0xc0002000);
			front_write_reg(TEST_BUS_VLD, 0x80000000);
			demod_top_write_reg(DEMOD_TOP_CFG_REG_4, 0x97);
			riscv_ctl_write_reg(0x30, 5);
			riscv_ctl_write_reg(0x30, 4);
			/* t2 top test bus addr */
			dvbt_t2_wr_word_bits(0x38, 0, 16, 4);
			demod_top_write_reg(DEMOD_TOP_CFG_REG_4, 0x0);
		}
		break;

	default:
		break;
	}

	mutex_unlock(&devp->lock);

	return r;
}

static int aml_dtvdm_get_property(struct dvb_frontend *fe,
			struct dtv_property *tvp)
{
	struct aml_dtvdemod *demod = (struct aml_dtvdemod *)fe->demodulator_priv;
	struct amldtvdemod_device_s *devp = (struct amldtvdemod_device_s *)demod->priv;

	mutex_lock(&devp->lock);

	switch (tvp->cmd) {
	case DTV_DELIVERY_SYSTEM:
		tvp->u.data = demod->last_delsys;
		PR_INFO("[id %d] get delivery system : %d\n", demod->id, tvp->u.data);
		break;

	case DTV_DVBT2_PLP_ID:
		if (!devp->demod_thread)
			break;

		/* plp nums & ids */
		dtvdemod_get_plp(tvp);
		break;

	default:
		break;
	}

	mutex_unlock(&devp->lock);

	return 0;
}

static struct dvb_frontend_ops aml_dtvdm_ops = {
	.delsys = {SYS_UNDEFINED},
	.info = {
		/*in aml_fe, it is 'amlogic dvb frontend' */
		.name = "",
		.caps = FE_CAN_FEC_1_2 | FE_CAN_FEC_2_3 | FE_CAN_FEC_3_4 |
			FE_CAN_FEC_5_6 | FE_CAN_FEC_7_8 | FE_CAN_FEC_AUTO |
			FE_CAN_QPSK | FE_CAN_QAM_16 | FE_CAN_QAM_64 |
			FE_CAN_QAM_AUTO | FE_CAN_TRANSMISSION_MODE_AUTO |
			FE_CAN_GUARD_INTERVAL_AUTO | FE_CAN_HIERARCHY_AUTO |
			FE_CAN_RECOVER | FE_CAN_MUTE_TS
	},
	.init = aml_dtvdm_init,
	.sleep = aml_dtvdm_sleep,
	.set_frontend = aml_dtvdm_set_parameters,
	.get_frontend = aml_dtvdm_get_frontend,
	.get_tune_settings = aml_dtvdm_get_tune_settings,
	.read_status = aml_dtvdm_read_status,
	.read_ber = aml_dtvdm_read_ber,
	.read_signal_strength = aml_dtvdm_read_signal_strength,
	.read_snr = aml_dtvdm_read_snr,
	.read_ucblocks = aml_dtvdm_read_ucblocks,
	.release = aml_dtvdm_release,
	.set_property = aml_dtvdm_set_property,
	.get_property = aml_dtvdm_get_property,
	.tune = aml_dtvdm_tune,
	.get_frontend_algo = gxtv_demod_get_frontend_algo,
};

struct dvb_frontend *aml_dtvdm_attach(const struct demod_config *config)
{
	int ic_version = get_cpu_type();
	unsigned int ic_is_supportted = 1;
	struct amldtvdemod_device_s *devp = dtvdemod_get_dev();
	struct aml_dtvdemod *demod = NULL;

	if (IS_ERR_OR_NULL(devp)) {
		pr_err("%s: error devp is NULL\n", __func__);

		return NULL;
	}

	mutex_lock(&amldtvdemod_device_mutex);

	if ((ic_version != MESON_CPU_MAJOR_ID_S4 && devp->index > 0) ||
		(ic_version == MESON_CPU_MAJOR_ID_S4 && devp->index > 1)) {
		pr_err("%s: Had attached (%d), only S4 support 2 attach.\n",
				__func__, devp->index);

		mutex_unlock(&amldtvdemod_device_mutex);

		return NULL;
	}

	demod = kzalloc(sizeof(*demod), GFP_KERNEL);
	if (!demod) {
		pr_err("%s: kzalloc for demod fail.\n", __func__);

		mutex_unlock(&amldtvdemod_device_mutex);

		return NULL;
	}

	INIT_LIST_HEAD(&demod->list);

	demod->id = devp->index;
	demod->act_dtmb = false;
	demod->timeout_atsc_ms = TIMEOUT_ATSC;
	demod->timeout_dvbt_ms = TIMEOUT_DVBT;
	demod->timeout_dvbs_ms = TIMEOUT_DVBS;
	demod->last_lock = -1;
	demod->inited = false;
	demod->suspended = true;

	/* select dvbc module for s4 */
	if (ic_version == MESON_CPU_MAJOR_ID_S4)
		demod->dvbc_sel = demod->id;
	else
		demod->dvbc_sel = 0;

	demod->priv = devp;
	demod->frontend.demodulator_priv = demod;
	demod->last_delsys = SYS_UNDEFINED;

	switch (ic_version) {
#ifndef CONFIG_AMLOGIC_REMOVE_OLD
	case MESON_CPU_MAJOR_ID_GXTVBB:
	case MESON_CPU_MAJOR_ID_TXL:
		aml_dtvdm_ops.delsys[0] = SYS_DVBC_ANNEX_A;
		aml_dtvdm_ops.delsys[1] = SYS_DTMB;
#ifdef CONFIG_AMLOGIC_DVB_COMPAT
		aml_dtvdm_ops.delsys[2] = SYS_ANALOG;
#endif
		if (ic_version == MESON_CPU_MAJOR_ID_GXTVBB)
			strcpy(aml_dtvdm_ops.info.name, "amlogic DVB-C/DTMB dtv demod gxtvbb");
		else
			strcpy(aml_dtvdm_ops.info.name, "amlogic DVB-C/DTMB dtv demod txl");
		break;

	case MESON_CPU_MAJOR_ID_TXLX:
		aml_dtvdm_ops.delsys[0] = SYS_ATSC;
		aml_dtvdm_ops.delsys[1] = SYS_DVBC_ANNEX_B;
		aml_dtvdm_ops.delsys[2] = SYS_DVBC_ANNEX_A;
		aml_dtvdm_ops.delsys[3] = SYS_DVBT;
		aml_dtvdm_ops.delsys[4] = SYS_ISDBT;
#ifdef CONFIG_AMLOGIC_DVB_COMPAT
		aml_dtvdm_ops.delsys[5] = SYS_ANALOG;
#endif
		strcpy(aml_dtvdm_ops.info.name, "amlogic DVB-C/T/ISDBT/ATSC dtv demod txlx");
		break;

	case MESON_CPU_MAJOR_ID_GXLX:
		aml_dtvdm_ops.delsys[0] = SYS_DVBC_ANNEX_A;
		strcpy(aml_dtvdm_ops.info.name, "amlogic DVBC dtv demod gxlx");
		break;

	case MESON_CPU_MAJOR_ID_TXHD:
		aml_dtvdm_ops.delsys[0] = SYS_DTMB;
		strcpy(aml_dtvdm_ops.info.name, "amlogic DTMB dtv demod txhd");
		break;

	case MESON_CPU_MAJOR_ID_TL1:
#endif
	case MESON_CPU_MAJOR_ID_TM2:
		aml_dtvdm_ops.delsys[0] = SYS_DVBC_ANNEX_A;
		aml_dtvdm_ops.delsys[1] = SYS_DVBC_ANNEX_B;
		aml_dtvdm_ops.delsys[2] = SYS_ATSC;
		aml_dtvdm_ops.delsys[3] = SYS_DTMB;
#ifdef CONFIG_AMLOGIC_DVB_COMPAT
		aml_dtvdm_ops.delsys[4] = SYS_ANALOG;
#endif
		if (ic_version == MESON_CPU_MAJOR_ID_TL1) {
#ifndef CONFIG_AMLOGIC_REMOVE_OLD
			strcpy(aml_dtvdm_ops.info.name, "amlogic DVB-C/DTMB/ATSC dtv demod tl1");
#endif
		} else {
			strcpy(aml_dtvdm_ops.info.name, "amlogic DVB-C/DTMB/ATSC dtv demod tm2");
		}
		break;

	case MESON_CPU_MAJOR_ID_T5:
		/* max delsys is 8, index: 0~7 */
		aml_dtvdm_ops.delsys[0] = SYS_DVBC_ANNEX_A;
		aml_dtvdm_ops.delsys[1] = SYS_DTMB;
#ifdef CONFIG_AMLOGIC_DVB_COMPAT
		aml_dtvdm_ops.delsys[2] = SYS_ANALOG;
#endif
		strcpy(aml_dtvdm_ops.info.name, "amlogic DVB-C/DTMB dtv demod t5");
		break;

	default:
		switch (devp->data->hw_ver) {
		case DTVDEMOD_HW_T5D:
		case DTVDEMOD_HW_T5D_B:
			/* max delsys is 8, index: 0~7 */
			aml_dtvdm_ops.delsys[0] = SYS_DVBC_ANNEX_A;
			aml_dtvdm_ops.delsys[1] = SYS_ATSC;
			aml_dtvdm_ops.delsys[2] = SYS_DVBS2;
			aml_dtvdm_ops.delsys[3] = SYS_ISDBT;
			aml_dtvdm_ops.delsys[4] = SYS_DVBS;
			aml_dtvdm_ops.delsys[5] = SYS_DVBT2;
			aml_dtvdm_ops.delsys[6] = SYS_DVBT;
			aml_dtvdm_ops.delsys[7] = SYS_DVBC_ANNEX_B;
#ifdef CONFIG_AMLOGIC_DVB_COMPAT
			aml_dtvdm_ops.delsys[8] = SYS_ANALOG;
#endif
			strcpy(aml_dtvdm_ops.info.name,
					"amlogic DVB-C/T/T2/S/S2/ATSC/ISDBT dtv demod t5d");
			break;

		case DTVDEMOD_HW_S4:
			aml_dtvdm_ops.delsys[0] = SYS_DVBC_ANNEX_A;
			aml_dtvdm_ops.delsys[1] = SYS_DVBC_ANNEX_B;
#ifdef CONFIG_AMLOGIC_DVB_COMPAT
			aml_dtvdm_ops.delsys[2] = SYS_ANALOG;
#endif
			strcpy(aml_dtvdm_ops.info.name, "amlogic DVB-C dtv demod s4");
			break;

		default:
			ic_is_supportted = 0;
			PR_ERR("%s: error unsupportted ic=%d\n", __func__, ic_version);
			kfree(demod);
			mutex_unlock(&amldtvdemod_device_mutex);

			return NULL;
		}
		break;
	}

	memcpy(&demod->frontend.ops, &aml_dtvdm_ops, sizeof(struct dvb_frontend_ops));

	/*diseqc attach*/
	if (!IS_ERR_OR_NULL(devp->diseqc_name))
		aml_diseqc_attach(devp, &demod->frontend);

	list_add_tail(&demod->list, &devp->demod_list);

	devp->index++;

	PR_INFO("%s [id = %d, total attach: %d] OK.\n",
			__func__, demod->id, devp->index);

	mutex_unlock(&amldtvdemod_device_mutex);

	return &demod->frontend;
}
EXPORT_SYMBOL_GPL(aml_dtvdm_attach);

#ifdef AML_DTVDEMOD_EXP_ATTACH
static struct aml_exp_func aml_exp_ops = {
	.leave_mode = leave_mode,
};

struct aml_exp_func *aml_dtvdm_exp_attach(struct aml_exp_func *exp)
{
	if (exp) {
		memcpy(exp, &aml_exp_ops, sizeof(struct aml_exp_func));
	} else {
		PR_ERR("%s:fail!\n", __func__);
		return NULL;

	}
	return exp;
}
EXPORT_SYMBOL_GPL(aml_dtvdm_exp_attach);

void aml_exp_attach(struct aml_exp_func *afe)
{

}
EXPORT_SYMBOL_GPL(aml_exp_attach);
#endif /* AML_DTVDEMOD_EXP_ATTACH */

//#ifndef MODULE
//fs_initcall(aml_dtvdemod_init);
//module_exit(aml_dtvdemod_exit);

//MODULE_VERSION(AMLDTVDEMOD_VER);
//MODULE_DESCRIPTION("gxtv_demod DVB-T/DVB-C/DTMB Demodulator driver");
//MODULE_AUTHOR("RSJ");
//MODULE_LICENSE("GPL");
//#endif

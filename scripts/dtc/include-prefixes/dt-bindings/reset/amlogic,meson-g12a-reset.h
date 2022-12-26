/* SPDX-License-Identifier: GPL-2.0+ OR BSD-3-Clause */
/*
 * Copyright (c) 2019 BayLibre, SAS.
 * Author: Jerome Brunet <jbrunet@baylibre.com>
 *
 */

#ifndef _DT_BINDINGS_AMLOGIC_MESON_G12A_RESET_H
#define _DT_BINDINGS_AMLOGIC_MESON_G12A_RESET_H

/*	RESET0					*/
#define RESET_HIU			0
/*					1	*/
#define RESET_DOS			2
/*					3-4	*/
#define RESET_VIU			5
#define RESET_AFIFO			6
#define RESET_VID_PLL_DIV		7
/*					8-9	*/
#define RESET_VENC			10
#define RESET_ASSIST			11
#define RESET_PCIE_CTRL_A		12
#define RESET_VCBUS			13
#define RESET_PCIE_PHY			14
#define RESET_PCIE_APB			15
#define RESET_GIC			16
#define RESET_CAPB3_DECODE		17
/*					18	*/
#define RESET_HDMITX_CAPB3		19
#define RESET_DVALIN_CAPB3		20
#define RESET_DOS_CAPB3			21
/*					22	*/
#define RESET_CBUS_CAPB3		23
#define RESET_AHB_CNTL			24
#define RESET_AHB_DATA			25
#define RESET_VCBUS_CLK81		26
/*					27-31	*/
/*	RESET1					*/
/*					32	*/
#define RESET_DEMUX			33
#define RESET_USB			34
#define RESET_DDR			35
/*					36	*/
#define RESET_BT656			37
#define RESET_AHB_SRAM			38
/*					39	*/
#define RESET_PARSER			40
/*					41	*/
#define RESET_ISA			42
#define RESET_ETHERNET			43
#define RESET_SD_EMMC_A			44
#define RESET_SD_EMMC_B			45
#define RESET_SD_EMMC_C			46
/*					47	*/
#define RESET_USB_PHY20			48
#define RESET_USB_PHY21			49
/*					50-60	*/
#define RESET_AUDIO_CODEC		61
/*					62-63	*/
/*	RESET2					*/
/*					64	*/
#define RESET_AUDIO			65
#define RESET_HDMITX_PHY		66
/*					67	*/
#define RESET_MIPI_DSI_HOST		68
#define RESET_ALOCKER			69
#define RESET_GE2D			70
#define RESET_PARSER_REG		71
#define RESET_PARSER_FETCH		72
#define RESET_CTL			73
#define RESET_PARSER_TOP		74
#define RESET_NN_PIPELINE0		75
#define RESET_NN			76
#define RESET_NN_PIPELINE1		77
#define RESET_DVALIN			78
#define RESET_HDMITX			79
/*					80-95	*/
/*	RESET3					*/
/*					96-95	*/
#define RESET_DEMUX_TOP			105
#define RESET_DEMUX_DES_PL		106
#define RESET_DEMUX_S2P_0		107
#define RESET_DEMUX_S2P_1		108
#define RESET_DEMUX_0			109
#define RESET_DEMUX_1			110
#define RESET_DEMUX_2			111
/*					112-127	*/
/*	RESET4					*/
/*					128-129	*/
#define RESET_MIPI_DSI_PHY		130
/*					131-132	*/
#define RESET_RDMA			133
#define RESET_VENCI			134
#define RESET_VENCP			135
/*					136	*/
#define RESET_VDAC			137
/*					138-139 */
#define RESET_VDI6			140
#define RESET_VENCL			141
#define RESET_I2C_M1			142
#define RESET_I2C_M2			143
/*					144-159	*/
/*	RESET5					*/
/*					160-191	*/
/*	RESET6					*/
#define RESET_GEN			192
#define RESET_SPICC0			193
#define RESET_SC			194
#define RESET_SANA_3			195
#define RESET_I2C_M0			196
#define RESET_TS_PLL			197
#define RESET_SPICC1			198
#define RESET_STREAM			199
#define RESET_TS_CPU			200
#define RESET_UART0			201
#define RESET_UART1_2			202
#define RESET_ASYNC0			203
#define RESET_ASYNC1			204
#define RESET_SPIFC0			205
#define RESET_I2C_M3			206
/*					207-223	*/
/*	RESET7					*/
#define RESET_USB_DDR_0			224
#define RESET_USB_DDR_1			225
#define RESET_USB_DDR_2			226
#define RESET_USB_DDR_3			227
#define RESET_TS_GPU			228
#define RESET_DEVICE_MMC_ARB		229
#define RESET_DVALIN_DMC_PIPL		230
#define RESET_VID_LOCK			231
#define RESET_NIC_DMC_PIPL		232
#define RESET_DMC_VPU_PIPL		233
#define RESET_GE2D_DMC_PIPL		234
#define RESET_HCODEC_DMC_PIPL		235
#define RESET_WAVE420_DMC_PIPL		236
#define RESET_HEVCF_DMC_PIPL		237
/*					238-255	*/

#endif

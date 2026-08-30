/********************************** (C) COPYRIGHT *******************************
 * File Name          : sd.h
 * Description        : SD card initialization driver header
 *
 * This module handles SD card initialization using the CH569's eMMC controller.
 * It implements the SD-specific init sequence (as opposed to the eMMC sequence
 * in CH56x_emmc.c) because SD cards use a different command flow:
 *   - CMD55+ACMD41 instead of CMD1 for OCR (SD vs eMMC distinction)
 *   - CMD3 returns RCA (SD assigns) vs CMD3 sets RCA (eMMC host assigns)
 *   - ACMD6 for bus width (SD) vs CMD6 with different args (eMMC)
 *   - ACMD51 for SCR register (SD-specific)
 *
 * [ref/SD_Physical_Layer_Spec_v6.00.pdf: Section 4.2 - Card Initialization]
 *******************************************************************************/
#ifndef __SD_H__
#define __SD_H__

#include "CH56x_common.h"
#include "CH56x_emmc.h"

/* Full SD card init using the fixed board timing configuration. */
uint8_t SDCardInit(PSD_PARAMETER pEMMCPara);

#endif

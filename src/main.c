/* main.c - USB MSC device: Host <-> CH569 USB2 <-> SD card
 * SPDX-License-Identifier: Apache-2.0
 */
#include "CH56x_common.h"
#include "debug.h"

#include "CH56x_usb20_devbulk.h"
#include "CH56x_usb_devbulk_desc_cmd.h"

#include "bot.h"
#include "sd.h"
#include "phantomdrive.h"
#include "crypto.h"

#undef FREQ_SYS
#define FREQ_SYS (120000000)

#if(defined DEBUG)
#define UART1_BAUD (115200)
#endif

debug_log_buf_t log_buf;

#define FLASH_ROMA_UID_ADDR (0x77fe4)
usb_descriptor_serial_number_t unique_id;

usb_descriptor_usb_vid_pid_t vid_pid =
{
	.vid = { .id_16b = 0x16C0 },
	.pid = { .id_16b = 0x05DC }
};


/* Init: GPIO/BSP -> UART -> USB -> SD -> main loop (bot_poll) */
int main()
{
	bsp_gpio_init();
	bsp_init(FREQ_SYS);
	log_init(&log_buf);

#if(defined DEBUG)
	UART1_init(UART1_BAUD, FREQ_SYS);
#endif
	cprintf("\n\r");
	log_printf("Phantomdrive, ChipID(Hex)=%02X\r\n", SYS_GetChipID());

	memset(&unique_id, 0, 8);
	FLASH_ROMA_READ(FLASH_ROMA_UID_ADDR, (uint32_t*)&unique_id, 8);

	log_printf("Phantomdrive FW v0.1 (CPr Freq=%d MHz)\r\n", (FREQ_SYS/1000000));

	R32_USB_CONTROL = 0;
	usb_descriptor_set_string_serial_number(&unique_id);

	usb_descriptor_set_usb_vid_pid(&vid_pid);
	PFIC_EnableIRQ(USBHS_IRQn);
	USB20_Device_Init(ENABLE);

	log_printf("Initializing SD card...\r\n");
	uint8_t sta = SDCardInit(&TF_EMMCParam);

	/* Enable eMMC error IRQ after init (BKGAP/TRANDONE polled in data path) */
	PFIC_EnableIRQ(EMMC_IRQn);
	TF_EMMCParam.EMMCOpErr = 0;

	if(sta == OP_SUCCESS)
	{
		g_bot.device_ready = true;
		log_printf("SD OK: %d sectors (%d MB)\r\n",
			TF_EMMCParam.EMMCSecNum,
			TF_EMMCParam.EMMCSecNum / 2048);
		phantomdrive_init(unique_id.sn_64b);
	}
	else
	{
		log_printf("SD init FAILED (sta=%d)\r\n", sta);
		while(1) {} // Something's wrong, sit here forever
	}

	log_printf("MSC ready\r\n");
	while(1)
	{
		bot_poll();
	}
}

__attribute__((interrupt("WCH-Interrupt-fast"))) void HardFault_Handler(void)
{
	printf("HardFault\r\n");
	printf(" SP=0x%08X\r\n", __get_SP());
	printf(" MIE=0x%08X\r\n", __get_MIE());
	printf(" MSTATUS=0x%08X\r\n", __get_MSTATUS());
	printf(" MCAUSE=0x%08X\r\n", __get_MCAUSE());
	bsp_wait_ms_delay(1);
}

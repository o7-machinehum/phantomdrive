/* bot.h - BOT (Bulk-Only Transport) protocol layer
 *
 * CBW parsing, SCSI command dispatch, CSW construction, and main loop poll.
 *
 * [ref/USB_MSC_Bulk_Only_Transport_v1.0.pdf]
 * [ref/SCSI_Block_Commands_SBC3_r25.pdf]
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef BOT_H_
#define BOT_H_

#include "bot_state.h"

void bot_set_sense(uint8_t key, uint8_t asc, uint8_t status);
void bot_handle_bulk_in(void);
void bot_handle_bulk_out(uint8_t *pbuf, uint16_t packlen);
void bot_send_response_data(void);
void bot_send_csw(void);
void bot_poll(void);

#endif /* BOT_H_ */

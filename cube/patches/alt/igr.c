/* 
 * Copyright (c) 2019, Extrems <extrems@extremscorner.org>
 * All rights reserved.
 */

#include <stdint.h>
#include "../base/common.h"
#include "../base/dolformat.h"
#include "../base/pad.h"

void check_pad(int32_t chan, PADStatus *status)
{
	uint32_t igr_exit_flag = *(uint32_t *)VAR_IGR_EXIT_FLAG;

	status->button &= ~PAD_USE_ORIGIN;
	if ((status->button & PAD_COMBO_EXIT) == PAD_COMBO_EXIT) {
		igr_exit_flag |= (1 << chan);
	} else {
		if (igr_exit_flag & (1 << chan)) (*DI_EMU)[1] |= 0b001;
		igr_exit_flag &= ~(1 << chan);
	}

	*(uint32_t *)VAR_IGR_EXIT_FLAG = igr_exit_flag;
}

static void load_dol(uint32_t offset, uint32_t size)
{
	DOLImage image;
	device_frag_read(&image, sizeof(image), offset);

	for (int i = 0; i < DOL_MAX_TEXT; i++) {
		device_frag_read(image.text[i], image.textLen[i], offset + image.textData[i]);
		dcache_flush_icache_inv(image.text[i], image.textLen[i]);
	}

	for (int i = 0; i < DOL_MAX_DATA; i++) {
		device_frag_read(image.data[i], image.dataLen[i], offset + image.dataData[i]);
		dcache_flush_icache_inv(image.data[i], image.dataLen[i]);
	}

	image.entry();
}

void igr_exit(void)
{
	disable_interrupts();
	end_read();

	uint8_t igr_exit_type = *(uint8_t *)VAR_IGR_EXIT_TYPE;
	uint32_t igr_dol_size = *(uint32_t *)VAR_IGR_DOL_SIZE;

	switch (igr_exit_type) {
		case IGR_BOOTBIN:
			if (igr_dol_size)
				load_dol(0x60000000, igr_dol_size);
			break;
	}
}

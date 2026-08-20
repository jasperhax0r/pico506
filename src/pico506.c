// Copyright (c) Kuba Szczodrzyński 2025-8-4.

#include "pico506.h"

int main() {
	set_sys_clock_khz(200000, true);
	stdio_init_all();
        LT_I("BUILD %s %s", __DATE__, __TIME__);

	pico506_t *pico = malloc(sizeof(*pico));
	memset(pico, 0, sizeof(*pico));
        
	LT_D("Initializing SPI...");
	pico->storage.sd.spi.sck_pin = PIN_SD_SCK;
	pico->storage.sd.spi.tx_pin	 = PIN_SD_MOSI;
	pico->storage.sd.spi.rx_pin	 = PIN_SD_MISO;
	pico->storage.sd.spi.cs_pin	 = PIN_SD_CS;
	pico->storage.sd.spi.use_dma = true;
	sd_spi_init(&pico->storage.sd, 25 * 1000 * 1000);

	uint retry_ms = 5000;
	while (1) {
		if (storage_init(pico) != 0)
			goto retry;
		if (storage_open(pico, "/HDD.RLL") != 0)
			goto retry;

		if (pico->storage.file_size != DRIVE_BYTES) {
			LT_E("Image file size invalid, found %u bytes, expected %u bytes", pico->storage.file_size, DRIVE_BYTES);
			storage_close(pico);
			goto retry;
		}
		LT_I("Image file size OK");

		break;

	retry:
		LT_E("Retrying in %u ms...", retry_ms);
		sleep_ms(retry_ms);
	}

	st506_start(pico);

	LT_I(
		"Emulator initialized - C/H/S: %u/%u/%u, data rate: %.1f Mb/s",
		CYLINDERS,
		HEADS,
		SECTORS_PER_PULSE * PULSES_PER_TRACK,
		(double)DATA_RATE / 1e6
	);

	while (1) {
		st506_loop(pico);
	}
}

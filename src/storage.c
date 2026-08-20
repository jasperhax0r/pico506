// Copyright (c) Kuba Szczodrzyński 2025-9-12.

#include "pico506.h"

int storage_init(pico506_t *pico) {
	sd_err_t sd_err;
	FRESULT fs_err;

	LT_D("Initializing SD card...");
	if ((sd_err = sd_init(&pico->storage.sd))) {
		LT_E("Failed to initialize SD card, sd_err = %u", sd_err);
		disk_set_sd(NULL);
		return -1;
	}
	LT_I("Detected SD card '%s', %lu MiB", pico->storage.sd.name, pico->storage.sd.sectors / 2048);
	disk_set_sd(&pico->storage.sd);
        
        spi_set_baudrate(pico->storage.sd.spi.inst, 8 * 1000 * 1000);
	LT_D("Initializing FAT filesystem...");
	if ((fs_err = f_mount(&pico->storage.fs, "", 1))) {
		LT_E("Failed to mount FAT filesystem, fs_err = %u", fs_err);
		return -1;
	}

	const char *fs_type = pico->storage.fs.fs_type == FS_FAT12	 ? "FAT12"
						  : pico->storage.fs.fs_type == FS_FAT16 ? "FAT16"
						  : pico->storage.fs.fs_type == FS_FAT32 ? "FAT32"
						  : pico->storage.fs.fs_type == FS_EXFAT ? "exFAT"
																 : "unknown";
	char fs_label[13]	= {0};
	// ulong fs_sn			= 0;
	DWORD fs_sn		= 0;
	f_getlabel("", fs_label, &fs_sn);
	LT_I("Mounted %s volume '%s' with UUID %04lX-%04lX", fs_type, fs_label, fs_sn >> 16, fs_sn & 0xFFFF);

	return 0;
}

int storage_open(pico506_t *pico, const char *filename) {
	FRESULT fs_err;

	LT_D("Opening %s...", filename);
	if ((fs_err = f_open(&pico->storage.fp, filename, FA_READ | FA_WRITE | FA_OPEN_EXISTING))) {
		LT_E("Failed to open file, fs_err = %u", fs_err);
		return -1;
	}
	LT_I("Opened %s", filename);
	pico->storage.file_size = f_size(&pico->storage.fp);

	// use fast seek for large files only
	if (pico->storage.file_size < 1024 * 1024)
		return 0;

	pico->storage.fp.cltbl = malloc(32 * sizeof(uint));
	if (!pico->storage.fp.cltbl)
		return -1;
	pico->storage.fp.cltbl[0] = 32;
	if ((fs_err = f_lseek(&pico->storage.fp, CREATE_LINKMAP))) {
		LT_E("Failed to create fast seek link map, fs_err = %u", fs_err);
		pico->storage.fp.cltbl = NULL;
		return -1;
	}

	return 0;
}

void storage_close(pico506_t *pico) {
	free(pico->storage.fp.cltbl);
	pico->storage.fp.cltbl	= NULL;
	pico->storage.file_size = 0;
	f_close(&pico->storage.fp);
}

int storage_read(pico506_t *pico, uint pos, void *data, uint len, sd_interrupt_t interrupt_check) {
	FRESULT fs_err	= FR_OK;
	uint bytes_read = 0;

	// seek to cylinder data in file
	if ((fs_err = f_lseek(&pico->storage.fp, pos)))
		goto err;

	// allow interrupting long read operations (i.e. multiple sector read)
	// this makes it possible to react quickly if a step happens during seeking
	pico->storage.sd.interrupted	 = false;
	pico->storage.sd.interrupt_check = interrupt_check;
	pico->storage.sd.interrupt_param = pico;
	fs_err							 = f_read(&pico->storage.fp, data, len, &bytes_read);
	pico->storage.sd.interrupt_check = NULL;

	if (pico->storage.sd.interrupted)
		// if the read was interrupted due to drive seek, simply return
		// (interrupted reads don't return errors)
		// this function will be called again with the new cylinder number
		return -1;

	if (fs_err)
		// otherwise fail and return
		goto err;
	if (bytes_read != len)
		LT_W("Read %u out of %u bytes", bytes_read, len);

	return 0;
err:
	LT_E("File read failed, fs_err = %u", fs_err);
	return -1;
}

int storage_write(pico506_t *pico, uint pos, const void *data, uint len) {
	FRESULT fs_err	   = FR_OK;
	uint bytes_written = 0;

	// seek to cylinder data in file
	if ((fs_err = f_lseek(&pico->storage.fp, pos)))
		goto err;

	fs_err = f_write(&pico->storage.fp, data, len, &bytes_written);
	if (fs_err)
		// fail and return
		goto err;
	if (bytes_written != len)
		LT_W("Wrote %u out of %u bytes", bytes_written, len);

	return 0;
err:
	LT_E("File write failed, fs_err = %u", fs_err);
	return -1;
}

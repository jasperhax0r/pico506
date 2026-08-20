// Copyright (c) Kuba Szczodrzyński 2025-8-4.

#pragma once

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <hardware/clocks.h>
#include <hardware/dma.h>
#include <hardware/pio.h>
#include <hardware/pwm.h>
#include <hardware/spi.h>
#include <pico/multicore.h>
#include <pico/stdlib.h>
#include <pico/time.h>

#include <ffconfig.h>

#include <ff.h>
#include <lt_logger.h>
#include <sd.h>

#include "config.h"
#include "utils.h"

typedef struct {
	// pico506.c
	struct {

	} drive;

	// st506.c
	struct {
		uint *pulse_data;
		uint *cyl_data;
		uint cyl;
		uint hd;
		volatile uint cyl_next;
		uint seek_target;
		bool seek_pending;
		bool needs_load;
		absolute_time_t seek_time;
		bool write_any;
		bool write_all;
		bool *write_block;
		absolute_time_t last_activity;
	} st506;

	// storage.c
	struct {
		sd_t sd;
		FATFS fs;
		FIL fp;
		uint file_size;
	} storage;
} pico506_t;

// diskio.c
void disk_set_sd(sd_t *new_sd);

// st506.c
int st506_start(pico506_t *pico);
void st506_stop(pico506_t *pico);
void st506_loop(pico506_t *pico);
bool st506_interrupt_check(pico506_t *pico);
void st506_on_write(pico506_t *pico, uint trans_count, uint end_addr);
void st506_on_head(pico506_t *pico, uint hd);
void st506_on_seek(pico506_t *pico, uint cyl);
uint st506_do_write(pico506_t *pico);

// storage.c
int storage_init(pico506_t *pico);
int storage_open(pico506_t *pico, const char *filename);
void storage_close(pico506_t *pico);
int storage_read(pico506_t *pico, uint pos, void *data, uint len, sd_interrupt_t interrupt_check);
int storage_write(pico506_t *pico, uint pos, const void *data, uint len);

// clicker.c
void clicker_start(pico506_t *pico);
void clicker_enqueue(uint count);
void clicker_seek(pico506_t *pico, uint count);
void clicker_pulse(pico506_t *pico, uint freq, uint us);

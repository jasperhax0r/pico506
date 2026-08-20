// Copyright (c) Kuba Szczodrzyński 2025-9-12.

#include "pico506.h"

#include "pio/st506_pio_util.h"

#include "st506_rddt.pio.h"
#include "st506_rdgt.pio.h"
#include "st506_step.pio.h"
#include "st506_wrdt.pio.h"
#include "st506_wrgt.pio.h"
#include "st506_wrrm.pio.h"
#include "SEGGER_RTT.h"

static pico506_t *g_pico = NULL;

static void st506_head_irq(uint gpio, uint32_t event_mask);

int st506_start(pico506_t *pico) {
	uint malloc_len;
	st506_stop(pico);

	// allocate memory for pulse config
	LT_V("Allocating pulse data memory...");
	pico->st506.pulse_data = malloc(malloc_len = (1 + PULSES_PER_TRACK) * sizeof(uint));
	if (pico->st506.pulse_data == NULL)
		goto err;
	// allocate memory for cylinder data
	LT_V("Allocating cylinder data memory...");
	pico->st506.cyl_data = calloc(1, malloc_len = CYLINDER_BYTES + 256);
	if (pico->st506.cyl_data == NULL)
		goto err;
	// allocate memory for write status
	LT_V("Allocating write status memory...");
	pico->st506.write_block = calloc(1, malloc_len = CYLINDER_BLOCKS);
	if (pico->st506.write_block == NULL)
		goto err;

	// start the clicker
	LT_D("Starting clicker...");
	clicker_start(pico);

	// initialize GPIOs
	LT_V("Initializing GPIOs...");
	gpio_init_mask((1 << PIN_SEEK_COMPLETE) | (1 << PIN_TRACK_0) | (1 << PIN_READY));
	gpio_set_dir_out_masked((1 << PIN_SEEK_COMPLETE) | (1 << PIN_TRACK_0) | (1 << PIN_READY));
	gpio_put(PIN_SEEK_COMPLETE, true);
	gpio_put(PIN_TRACK_0, true);
	gpio_put(PIN_READY, true);
	
	gpio_init(3);
	gpio_set_dir(3, GPIO_OUT);
	gpio_put(3, 0);

	pio_gpio_init(PIO_RDDT, PIN_READ);
	pio_gpio_init(PIO_RDGT, PIN_INDEX);
	gpio_set_inover(PIN_DIR_IN, GPIO_OVERRIDE_INVERT);
        gpio_set_inover(PIN_HEAD_1, GPIO_OVERRIDE_INVERT);
        gpio_set_inover(PIN_HEAD_2, GPIO_OVERRIDE_INVERT);
        gpio_set_inover(PIN_WRITE_GATE, GPIO_OVERRIDE_INVERT);

	// store pico506_t* for IRQ callback
	g_pico = pico;
	gpio_set_irq_enabled_with_callback(PIN_HEAD_1, GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL, true, st506_head_irq);
        gpio_set_irq_enabled(PIN_HEAD_2, GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL, true);
	// load initial cylinder data
	LT_V("Loading cylinder data...");
	pico->st506.cyl = CYL_INVALID;
	st506_on_seek(pico, /* cyl= */ 0);
        

	// load all PIO programs
	LT_V("Loading PIO programs...");
	uint st506_rddt_offset = pio_add_program(PIO_RDDT, &st506_rddt_program); // 1 instruction
	uint st506_rdgt_offset = pio_add_program(PIO_RDGT, &st506_rdgt_program); // 5 instructions
	uint st506_step_offset = pio_add_program(PIO_STEP, &st506_step_program); // 11 instructions
	uint st506_wrdt_offset = pio_add_program(PIO_WRDT, &st506_wrdt_program); // 14 instructions
	uint st506_wrgt_offset = pio_add_program(PIO_WRGT, &st506_wrgt_program); // 4 instructions
	uint st506_wrrm_offset = pio_add_program(PIO_WRRM, &st506_wrrm_program); // 2 instructions

	// build RDGT lines configuration
	LT_V("Building pulse data...");
	st506_rdgt_program_fill_data(
		pico->st506.pulse_data,
		PULSES_PER_TRACK,
		MARK_BITS,
		SECTOR_BITS * SECTORS_PER_PULSE,
		PIN_INDEX,
		PIN_SERVO_GATE
	);
        LT_D("RDGT pulse[0]=%08lx pulse[1]=%08lx MARK_BITS=%d SECTOR_BITS=%d", pico->st506.pulse_data[0], pico->st506.pulse_data[1], MARK_BITS, SECTOR_BITS*SECTORS_PER_PULSE);

	// initialize all PIO state machines
	LT_V("Initializing PIO state machines...");
	st506_rddt_program_init(PIO_SM_RDDT, st506_rddt_offset, PIN_READ, DATA_RATE * 2);				   // 1x DMA
	st506_rdgt_program_init(PIO_SM_RDGT, st506_rdgt_offset, PIN_INDEX, PIN_SERVO_GATE, DATA_RATE * 2); // 4x DMA
	st506_step_program_init(PIO_SM_STEP, st506_step_offset, PIN_STEP, PIN_DIR_IN);					   // 1x DMA
	st506_wrdt_program_init(PIO_SM_WRDT, st506_wrdt_offset, PIN_WRITE, DATA_RATE * 2);				   // 1x DMA
	st506_wrgt_program_init(PIO_SM_WRGT, st506_wrgt_offset, PIN_WRITE_GATE);						   // 3x DMA
	st506_wrrm_program_init(PIO_SM_WRRMC, st506_wrrm_offset);
	st506_wrrm_program_init(PIO_SM_WRRMA, st506_wrrm_offset);

	// start the step PIO - it's pretty much independent of other PIOs
	LT_V("Starting STEP...");
	st506_step_program_start(PIO_SM_STEP, pico->st506.cyl, &pico->st506.cyl_next, CYLINDERS - 1);

	// start RDDT/WRDT PIO - they will stall until Control Channels start the DMA
	LT_V("Starting RDDT...");
	st506_rddt_program_start(PIO_SM_RDDT);
	LT_V("Starting WRDT...");
	st506_wrdt_program_start(PIO_SM_WRDT);

	// start the RDGT PIO and DMA
	LT_V("Starting RDGT...");
	st506_rdgt_program_start(
		PIO_SM_RDGT,
		st506_rddt_chan,
		pico->st506.pulse_data,
		pico->st506.cyl_data,
		1 + PULSES_PER_TRACK,
		TRACK_WORDS
	);
        for (int i = 0; i < 5; i++) {
        LT_D("RDGT chan tc=%lu, RDDT chan tc=%lu", dma_hw->ch[st506_rdgt_chan].transfer_count, dma_hw->ch[st506_rddt_chan].transfer_count);
        sleep_ms(4);
        }
	LT_V("Starting WRGT...");
	st506_wrgt_program_start(
		PIO_SM_WRGT,
		st506_rddt_chan,
		st506_wrdt_chan,
		&PIO_WRDT->rxf[SM_WRDT],
		&PIO_WRRM->txf[SM_WRRMC],
		&PIO_WRRM->txf[SM_WRRMA]
	);

	return 0;
err:
	LT_E("Failed to allocate %u bytes of memory", malloc_len);
	st506_stop(pico);
	return -1;
}

void st506_stop(pico506_t *pico) {
	LT_V("Stopping PIO and DMA...");
	st506_wrgt_program_stop(PIO_SM_WRGT);
	st506_rdgt_program_stop(PIO_SM_RDGT);
	st506_wrdt_program_stop(PIO_SM_WRDT);
	st506_rddt_program_stop(PIO_SM_RDDT);
	st506_step_program_stop(PIO_SM_STEP);
	st506_wrrm_program_deinit(PIO_SM_WRRMA);
	st506_wrrm_program_deinit(PIO_SM_WRRMC);
	pio_clear_instruction_memory(pio0);
	pio_clear_instruction_memory(pio1);

	LT_V("Disabling GPIO...");
	g_pico = NULL;
	gpio_set_irq_enabled_with_callback(PIN_HEAD_1, GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL, false, NULL);
	gpio_set_irq_enabled(PIN_HEAD_2, GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL, false);
	gpio_set_function(PIN_SERVO_GATE, GPIO_FUNC_NULL);
	gpio_set_function(PIN_INDEX, GPIO_FUNC_NULL);
	gpio_set_function(PIN_READ, GPIO_FUNC_NULL);
	gpio_set_function(PIN_READY, GPIO_FUNC_NULL);
	gpio_set_function(PIN_TRACK_0, GPIO_FUNC_NULL);
	gpio_set_function(PIN_SEEK_COMPLETE, GPIO_FUNC_NULL);

	LT_V("Freeing memory...");
	free(pico->st506.cyl_data);
	free(pico->st506.pulse_data);
	memset(&pico->st506, 0, sizeof(pico->st506));
}

void st506_loop(pico506_t *pico) {
        // check write address received from WRRM PIO
	if (pio_sm_get_rx_fifo_level(PIO_SM_WRRMA)) {
		uint end_addr	 = pio_sm_get(PIO_SM_WRRMA);
		uint trans_count = pio_sm_get(PIO_SM_WRRMC);
		if (pio1_hw->fdebug & (((1 << SM_WRRMA) | (1 << SM_WRRMC)) << PIO_FDEBUG_TXOVER_LSB)) {
			LT_E("Write WRRM PIO overflow");
			pio1_hw->fdebug = PIO_FDEBUG_TXOVER_BITS;
			// assume all data was modified in case of overflows
			pico->st506.write_all = true;
		} else {
			st506_on_write(pico, trans_count, end_addr);
		}
	}

	// check cylinder number received from STEP PIO
	uint cyl_next = pico->st506.cyl_next;
	if (cyl_next != CYL_INVALID) {
	        gpio_put(PIN_SEEK_COMPLETE, false);
	        // Realtime position signal, assert the moment the head reaches 0
	        gpio_put(PIN_TRACK_0, cyl_next == 0);
	        
	        // latch target for the debounced (expensive) read
		pico->st506.cyl_next     = CYL_INVALID;   // consume on sight
		pico->st506.seek_target  = cyl_next;
		pico->st506.seek_pending = true;
		pico->st506.seek_time    = get_absolute_time();
	}
        if (pico->st506.seek_pending &&
	    absolute_time_diff_us(pico->st506.seek_time, get_absolute_time()) > STEP_SETTLE_US) {
		pico->st506.seek_pending = false;
		pico->st506.needs_load = true;               // but don't read SD yet
		gpio_put(PIN_TRACK_0, pico->st506.cyl == 0);
		gpio_put(PIN_SEEK_COMPLETE, true);           // release the controller
	}

	// load only once stepping has actually stopped
	if (pico->st506.needs_load &&
	    absolute_time_diff_us(pico->st506.seek_time, get_absolute_time()) > 5000) {
		pico->st506.needs_load = false;
		// uint target = pico->st506.cyl;
		// pico->st506.cyl = CYL_INVALID;
		st506_on_seek(pico, pico->st506.seek_target);  // the existing storage_read + flush logic
	}
	

	// check time elapsed since last activity
	if (pico->st506.write_any &&
		absolute_time_diff_us(pico->st506.last_activity, get_absolute_time()) > IDLE_TIMEOUT * 1000) {
		// do a dummy seek to write any pending changes
		st506_on_seek(pico, pico->st506.cyl);
	}
}

bool st506_interrupt_check(pico506_t *pico) {
	// interrupt seek operations if a step happens during the readout
	// but only if the next track is actually different from the one being loaded
	return pico->st506.cyl_next != CYL_INVALID && pico->st506.cyl_next != pico->st506.cyl;
}

static void st506_head_irq(uint gpio, uint32_t event_mask) {
	(void)gpio, (void)event_mask;
	if (!g_pico)
		return;
	uint hd = gpio_get(PIN_HEAD_1) | (gpio_get(PIN_HEAD_2) << 1);
	st506_on_head(g_pico, hd);
}

void st506_on_write(pico506_t *pico, uint trans_count, uint end_addr) {	
        (void)trans_count; (void)end_addr;
        uint start_byte = pico->st506.hd * TRACK_BYTES;
        uint end_byte   = start_byte + TRACK_BYTES;
	uint start_block = start_byte /512;
	uint end_block   = (end_byte + 511) / 512;
	for (uint block = start_block; block <= end_block && block < CYLINDER_BLOCKS; block++)
		pico->st506.write_block[block] = true;
	pico->st506.write_any = true;
	pico->st506.last_activity = get_absolute_time();
	clicker_enqueue(CYL_INVALID);
}

void st506_on_head(pico506_t *pico, uint hd) {
        LT_D("on_head: hd=%u  HSEL0=%u HSEL1=%u", hd, gpio_get(PIN_HEAD_1), gpio_get(PIN_HEAD_2));
	if (hd >= HEADS)
		hd = HEADS - 1;
	bool changed = (hd != pico->st506.hd);
	pico->st506.hd = hd;

	// only retarget once the RDGT DMA channels exist (they're set up in
	// st506_start AFTER the initial on_seek/on_head call). Guards the boot path.
	if (st506_rdgt_ctrl_chan >= 0) {
		uint rddt_data_old   = st506_rdgt_rddt_data;
		uint rddt_data_new   = (uint)pico->st506.cyl_data + hd * TRACK_BYTES;
		// in on_head, after computing rddt_data_new:
		uint rddt_cnt        = st506_rdgt_rddt_cnt;
		st506_rdgt_rddt_data = rddt_data_new;
		st506_rddt_program_retarget(
			st506_rdgt_ctrl_chan,
			st506_rdgt_work_chan,
			st506_rdgt_trig_chan,
			rddt_data_old,
			rddt_data_new,
			rddt_cnt
		);
	}

	if (changed) {
		pico->st506.last_activity = get_absolute_time();
		clicker_enqueue(CYL_INVALID);
	}
}

void st506_on_seek(pico506_t *pico, uint cyl) {
	if (cyl >= CYLINDERS)
		cyl = CYLINDERS - 1;

	// update GPIOs
	gpio_put(PIN_SEEK_COMPLETE, false);
	gpio_put(PIN_TRACK_0, cyl == 0);
	pico->st506.cyl_next = CYL_INVALID;

	if (pico->st506.cyl == CYL_INVALID) {
		// cannot write if the current cylinder is invalid
		pico->st506.write_any = false;
	} else {
		// run the clicker on seek
		if (cyl >= pico->st506.cyl)
			clicker_enqueue(cyl - pico->st506.cyl);
		else
			clicker_enqueue(pico->st506.cyl - cyl);
	}


        // dont flush while controller is still writing
        absolute_time_t wg_deadline = make_timeout_time_ms(50);
        while (gpio_get(PIN_WRITE_GATE) && !time_reached(wg_deadline))
              tight_loop_contents();
        sleep_us(300);
        
	if (pico->st506.write_any) {
		// write data to storage if anything was modified
		if (!pico->st506.write_all) {
			// if more than half of each head was modified, write all of it
			for (uint head = 0; head < HEADS; head++) {
				uint changes = 0;
				for (uint block = 0; block < TRACK_BLOCKS; block++) {
					if (pico->st506.write_block[head * TRACK_BLOCKS + block])
						changes++;
					if (changes >= TRACK_BLOCKS / 2) {
						memset(&pico->st506.write_block[head * TRACK_BLOCKS], true, TRACK_BLOCKS);
						break;
					}
				}
			}
		}

		// run the write operation
		absolute_time_t start = get_absolute_time();
		uint write_bytes	  = st506_do_write(pico);
		absolute_time_t end	  = get_absolute_time();

		unsigned long micros = absolute_time_diff_us(start, end);
		double speed = (double)write_bytes / ((double)micros / 1000000.0) / 1024.0;
		LT_D(
			"Write of cylinder %u (%u bytes) finished in %lu us -> %.03f KiB/s",
			pico->st506.cyl,
			write_bytes,
			micros,
			speed
		);
	}

	// seeking not possible - already on the first/last track - do a dummy wait
	if (cyl == pico->st506.cyl) {
		LT_D("Already on cylinder %u", cyl);
		sleep_us(680);
		gpio_put(PIN_SEEK_COMPLETE, true);
		return;
	}
	// otherwise update the current track number
	pico->st506.cyl = cyl;

	absolute_time_t start = get_absolute_time();
	uint read_bytes		  = CYLINDER_BYTES;
	{
		// read data from storage
		int err = storage_read(
			pico,
			cyl * CYLINDER_BYTES,
			pico->st506.cyl_data,
			CYLINDER_BYTES,
			(sd_interrupt_t)st506_interrupt_check
		);
		if (err) {
			// invalidate current cylinder on error or interrupt
			pico->st506.cyl = CYL_INVALID;
			gpio_put(PIN_SEEK_COMPLETE, true);
			return;
		}
	}
	absolute_time_t end = get_absolute_time();

	unsigned long micros = absolute_time_diff_us(start, end);
	double speed = (double)read_bytes / ((double)micros / 1000000.0) / 1024.0;
	LT_D(
		"Read of cylinder %u (%u bytes) finished in %lu us -> %.03f KiB/s",
		pico->st506.cyl,
		read_bytes,
		micros,
		speed
	);
        LT_D("RDGT pulse[0]=%08lx pulse[1]=%08lx MARK_BITS=%d SECTOR_BITS=%d", pico->st506.pulse_data[0], pico->st506.pulse_data[1], MARK_BITS, SECTOR_BITS*SECTORS_PER_PULSE);	
        
        // store last activity time
	pico->st506.last_activity = get_absolute_time();

	// re-establish the head pointer against the newly-loaded cylinder buffer.
	// on_head() now retargets unconditionally, so this also covers the case
	// where a write arrives before any head-select on the new cylinder.
	st506_on_head(pico, pico->st506.hd < HEADS ? pico->st506.hd : 0);

	// update GPIOs
	if (cyl == 0)
		gpio_put(PIN_TRACK_0, true);
	gpio_put(PIN_SEEK_COMPLETE, true);
}


uint st506_do_write(pico506_t *pico) {
	uint out_bytes = 0;
	uint out_pos   = pico->st506.cyl * CYLINDER_BYTES;
	int err		   = 0;
	                  
	if (pico->st506.write_all) {
		// write the entire cylinder
		out_bytes += CYLINDER_BYTES;
		err = storage_write(pico, out_pos, pico->st506.cyl_data, CYLINDER_BYTES);
	} else {
		// write only modified blocks
		int first = -1;
		int last  = 0;
		for (int block = 0; block < CYLINDER_BLOCKS + 1; block++) {
			if (block < CYLINDER_BLOCKS && pico->st506.write_block[block]) {
				// block changed, count to current modified range
				if (first == -1)
					first = block;
				last = block + 1;
				continue;
			}
			// found unchanged block or went past the last block
			if (first == -1)
				continue;

			// calculate starting position and length (in both src and dst)
			uint pos = first * 512;
			uint len = (last - first) * 512;
			if (pos + len > CYLINDER_BYTES)
				len = CYLINDER_BYTES - pos;
			out_bytes += len;
			err	  = storage_write(pico, out_pos + pos, (uint8_t *)pico->st506.cyl_data + pos, len);
			first = -1;
			if (err)
				goto end;
		}
	}
        // Flush buffer to SD
	FRESULT fsr = f_sync(&pico->storage.fp);

end:
	memset(pico->st506.write_block, 0, CYLINDER_BLOCKS);
	pico->st506.write_all = false;
	pico->st506.write_any = false;

	if (err)
		LT_E("Write error: %d", err);
	return out_bytes;
}

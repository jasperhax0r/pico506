// pico506 config.h — Kaypro / WD1002-HDO, 5 MB MFM
// Board: SN74AHCT245 (inputs, non-inverting) + ULN2803 (outputs, inverting)
//        SN75176 x2 (RDDATA driver, WRDATA receiver)
// Pinout matches the protoboard/PCB map — VERIFY against the board.

#pragma once

// MFM data
#define PIN_READ          5   // -> SN75176 (U4) D input, direct from GPIO
#define PIN_WRITE         6   // <- SN75176 (U5) R output, via '245
// Control Input (all via '245, arrive ACTIVE-LOW -> need inover invert)
#define PIN_SELECT        13
#define PIN_WRITE_GATE    9
#define PIN_HEAD_1        10
#define PIN_HEAD_2        11
#define PIN_DIR_IN        8
#define PIN_STEP          7
// Control Output (all via ULN2803, inverting -> active-HIGH at Pico)
#define PIN_SEEK_COMPLETE 1
#define PIN_INDEX         0
#define PIN_TRACK_0       2
#define PIN_READY         4
// SERVO_GATE is JVC-only and unused here, but the RDGT PIO requires it to sit
// adjacent to PIN_INDEX. GP1 is SEEK_COMPLETE, so this pin is declared but must
// NOT be handed to the PIO - see the st506.c edits.
#define PIN_SERVO_GATE    1
// GP3  = /WRITE_FAULT - left undriven; bus pull-up keeps it deasserted
// GP12 = /HEAD_SEL_4  - unused at 4 heads
// Buzzer / LED
#define PIN_BUZZER        18 //swapped to led for testing
#define PIN_LED           19
// SD Card (SPI1: GP14 SCK, GP15 TX, GP28 RX; GP17 is software CS)
#define PIN_SD_SCK        14
#define PIN_SD_MOSI       15
#define PIN_SD_MISO       28
#define PIN_SD_CS         17

// ---- Geometry: 153 x 4 x 17 x 512 = 5.08 MiB ----
// TRACK_BITS = 16*17*(MARK + HEADER + DATA) = 272 * 612 = 166464
//   -> 16.646 ms/rev -> 60.07 Hz INDEX
//   -> TRACK_WORDS 5202, CYLINDER_BYTES 83232 (all derived macros are integral)
#define DATA_RATE         5000000
#define MARK_LBYTES       42
#define HEADER_LBYTES     58
#define DATA_LBYTES       512
#define SECTORS_PER_PULSE 1
#define PULSES_PER_TRACK  17
#define HEADS             4
#define CYLINDERS         306   // 306 for the 10 MB / two-partition case

#define MARK_BITS   (MARK_LBYTES * 8 * 2)
#define SECTOR_BITS ((HEADER_LBYTES + DATA_LBYTES) * 8 * 2)
#define PULSE_BITS  (MARK_BITS + SECTOR_BITS * SECTORS_PER_PULSE)
#define TRACK_BITS  (PULSE_BITS * PULSES_PER_TRACK)

#define MARK_WORDS      (MARK_BITS / 32)
#define SECTOR_WORDS    (SECTOR_BITS / 32)
#define PULSE_WORDS     (PULSE_BITS / 32)
#define TRACK_BYTES     (TRACK_BITS / 8)
#define TRACK_WORDS     (TRACK_BYTES / 4)
#define CYLINDER_BYTES  (TRACK_BYTES * HEADS)
#define CYLINDER_BLOCKS (((CYLINDER_BYTES - 1) / 512) + 1)
#define TRACK_BLOCKS    (CYLINDER_BLOCKS / HEADS)
#define DRIVE_BYTES     (CYLINDER_BYTES * CYLINDERS)

#define STEP_SETTLE_US 10000

#define IDLE_TIMEOUT 5000

#define CYL_INVALID 2048

#define PIO_RDDT pio0
#define PIO_RDGT pio0
#define PIO_STEP pio0
#define PIO_WRDT pio1
#define PIO_WRGT pio1
#define PIO_WRRM pio1

#define SM_RDDT  0
#define SM_RDGT  1
#define SM_STEP  2
#define SM_WRDT  0
#define SM_WRGT  1
#define SM_WRRMC 2
#define SM_WRRMA 3

#define PIO_SM_RDDT  PIO_RDDT, SM_RDDT
#define PIO_SM_RDGT  PIO_RDGT, SM_RDGT
#define PIO_SM_STEP  PIO_STEP, SM_STEP
#define PIO_SM_WRDT  PIO_WRDT, SM_WRDT
#define PIO_SM_WRGT  PIO_WRGT, SM_WRGT
#define PIO_SM_WRRMC PIO_WRRM, SM_WRRMC
#define PIO_SM_WRRMA PIO_WRRM, SM_WRRMA

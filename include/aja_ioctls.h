/*******************************************************************************
 * aja_ioctls.h
 *
 * Creation Date: June 06, 2004
 * Author(s): 
 *   Jason Howard <jason@spectsoft.com>
 *
 * 
 * Copyright (c) 2004, SpectSoft, LLC
 *   All Rights Reserved.
 *   http://www.spectsoft.com/
 *   info@spectsoft.com
 * 
 * $Id$
 * 
 * DESC:
 * 
 ******************************************************************************/

#ifndef _IOCTLS_H_
#define _IOCTLS_H_

#ifndef __KERNEL__
#include <stdint.h>
#include <sys/time.h>
#endif

#include <linux/ioctl.h>
#include <aja_registers.h>
#include <pcitc.h>
#include <timecode.h>

#define AJA_API_VERSION 	202

/* AJA board IDs */
#define AJA_KONASD11		0x10111900
#define AJA_KONAHD11 		0x10120900
#define AJA_XENAHDR11MM		0x10113801
#define AJA_XENAHD11MM		0x10113800
#define AJA_XENASD11MM		0x10114400
#define AJA_XENASD22MM		0x10128600
#define AJA_XENAHD22MM		0x10160100
#define AJA_HDNTV2MM		0x10160200
#define AJA_XENASD11DA		0x10133700
#define AJA_XENAHDR11DA		0x10133901
#define AJA_XENAHD11DA		0x10133900
#define AJA_XENAHD22DA		0x10148300
#define AJA_XENASD22DA		0x10152100
#define AJA_HDNTV2DA		0x10156100
#define AJA_KONA2		0x10174700
#define AJA_KONA3 		0x10196500
#define AJA_KONAHS 		0x10182800
#define AJA_KONAHS2		0x10182801

#define AJA_MAXPAGES 		64
#define AJA_MAXCHANS 		2
#define AJA_MAXCARDS 		8
#define AJA_MINORS_PER_CARD 	2
#define AJA_SD_PAGESIZE		0x200000
#define AJA_HD_PAGESIZE		0x800000
#define AJA_SD_APLAY		AJA_SD_PAGESIZE * 63
#define AJA_HD_APLAY		AJA_HD_PAGESIZE * 31
#define AJA_K2_APLAY		AJA_HD_PAGESIZE * 15
#define AJA_SD_ACAP		AJA_SD_APLAY + 0x100000
#define AJA_HD_ACAP		AJA_HD_APLAY + 0x100000
#define AJA_K2_ACAP		AJA_K2_APLAY + 0x100000
#define AJA_APLAY_CARDSIZE 	3072 * 340
#define AJA_ACAP_CARDSIZE 	3072 * 340

#define AJA_SPEED_DIVISOR 	1000000

#define AJAREG_READ           0x1
#define AJAREG_WRITE          0x2
#define AJAREG_STATE          0x4

typedef struct {
	const char 		*name;
	const uint32_t 		value;
} aja_regoption_t;

typedef struct {
	const char 		*name;
	const char 		*desc;
	const aja_register_t 	*reg;
	const aja_regoption_t 	*opts;
	const unsigned int 	flags;
} aja_regstring_t;

/****************************************************************************************/
/* Board Information                                                                    */


/* NOTE: If you add stuff to this structure, always add it to the bottom so everything
 * else still aligns!  Thank you, The Mgmt.
 */
typedef struct {
	char			name[32];		/* Card Name */
	uint32_t 		id;			/* Card ID */
	uint32_t 		flags;			/* Card flags */
	uint32_t 		acapbuf;		/* Card Aud. Capture Buffer Pointer */
	uint32_t 		aplaybuf;		/* Card Aud. Playback Buffer Pointer */
	uint32_t 		pagesize;		/* Static Pagesize */
	int32_t			pages;			/* Number of pages on the card */
	/* NOTE: The previous values need to stay in the exact order, as the driver caps gets copied here */
	uint32_t 		fwver; 			/* Firmware Version */
	uint32_t 		fpgaver;		/* FPGA Version */
	uint32_t 		boardver;		/* Board Version */
	uint32_t 		acapsize; 		/* Board capture buffer size */
	uint32_t 		aplaysize;		/* Board playback size */
	char 			dver[128];		/* Driver Version */
	char 			serial[16];		/* Card Serial Number */
} aja_boardinfo_t;

enum aja_board_flags {
	AJA_HasK2 = 		0x0001,		/* The board uses the K2 archetecture */
	AJA_HasAudio = 		0x0002,		/* The board supports audio */
	AJA_HasDualLink =	0x0004, 	/* The board supports dual link HD-SDI */
	AJA_HasHD = 		0x0008, 	/* The board supports HD */
	AJA_HasSD = 		0x0010,		/* The board supports SD */
	AJA_HasSerial = 	0x0020, 	/* The board has an onboard serial port */
	AJA_HasLUT = 		0x0040, 	/* The board has LUT support */
	AJA_SmallReg = 		0x0080 		/* The board has a small register space (only 64 regs) */
};

#define AJACTL_BOARDINFO 		_IOR('y', 0, aja_boardinfo_t)

/* Returns the driver API version */
#define AJACTL_GETAPIVER 		_IOR('y', 1, int)

/****************************************************************************************/
/* Registers                                                                            */

/* structure for reading and writing directly to/from card registers */
typedef struct {
	aja_register_t 	reginfo;
	uint32_t 	value;
} aja_registerio_t;

#define AJACTL_GETREGISTER		_IOR('y', 15, aja_registerio_t)
#define AJACTL_SETREGISTER		_IOW('y', 16, aja_registerio_t)


/****************************************************************************************/
/* Interrupts                                                                           */

#define AJA_IRQ_TYPES 		11
#define AJA_DMA_COUNT 		3
enum aja_interrupt {
	AJA_BusError = 0,
	AJA_Output,
	AJA_Input1,
	AJA_Input2,
	AJA_AudioWrap,
	AJA_DMA1,
	AJA_DMA2,
	AJA_DMA3,
	AJA_DMA4,
	AJA_UART_TX,
	AJA_UART_RX
};

typedef struct {
	int 			type;
	int64_t 		count;
} aja_irqcountget_t;
/* returns the number of interrupts of a given type */
#define AJACTL_IRQCOUNT			_IOR('y', 20, aja_irqcountget_t)

/* sleeps until an interrupt of a given type */
#define AJACTL_IRQSLEEP 		_IO('y', 21)

/* Returns the timing of the last output IRQ event */
#define AJACTL_IRQTIMING 		_IOR('y', 22, aja_timing_t)

/* Enable card IRQ */
#define AJACTL_IRQENABLE 		_IOR('y', 23, int)

/****************************************************************************************/
/* Frame information and I/O                                                            */

typedef struct {
	int64_t			id;		// Frame ID
	int32_t			line;		// Output line number
	uint32_t 		timer; 		// Internal 48KHz timer value
	struct timeval 		tstamp;		// System timestamp
	timecode_t 		tcg;		// Timecode generator timestamp
	uint64_t 		priv;		// User specific private data
} aja_frametime_t;

/* Get the current frame ID */
#define AJACTL_FRAME_CURID 		_IOR('y', 30, int64_t)

/* Get the current frame time structure */
#define AJACTL_FRAME_CURTIME 		_IOR('y', 31, aja_frametime_t)

/* Block until a new frame */
#define AJACTL_FRAME_WAITFORNEXT 	_IO('y', 32)


/****************************************************************************************/
/* DMA                                                                                  */

typedef struct {
	int		engine; 	/* DMA Engine to use */
	uint32_t 	cadd;		/* Address on the card */
	uint32_t 	len;		/* Length of the trasfer */
	uint32_t 	dir;		/* Transfer direction */
	void 		*uadd;		/* Userspace address */
} aja_dmainfo_t;

#define AJA_DMATOCARD 		0
#define AJA_PLAYBACK 		0
#define AJA_DMAFROMCARD 	1
#define AJA_CAPTURE 		1

#define AJACTL_DMA 			_IOWR('y', 40, aja_dmainfo_t)

/****************************************************************************************/
/* Timecode                                                                             */

#define AJA_TIMECODE_TYPES 	4
enum aja_timecode_types {
	AJA_TimecodeInternal 	= 0,
	AJA_TimecodeSDI1 	= 1,
	AJA_TimecodeSDI2 	= 2,
	AJA_TimecodeLTC 	= 3,
};

enum aja_tcg_flags {
	AJA_TCG_Running 	= 0x01
};

/* Get the timecode flags */
#define AJACTL_TIMECODE_GETFLAGS 	_IOR('y', 50, int)

/* Set the timecode flags */
#define AJACTL_TIMECODE_SETFLAGS 	_IOW('y', 51, int)

/* Clears the selected timecode flags */
#define AJACTL_TIMECODE_CLRFLAGS 	_IOW('y', 52, int)

/* Set the current timecode */
#define AJACTL_TIMECODE_SET 		_IOW('y', 53, timecode_t)

/* Get the current timecode */
#define AJACTL_TIMECODE_GET 		_IOR('y', 54, timecode_t)

/* Get the SDI1 timecode */
#define AJACTL_TIMECODE_SDI1 		_IOR('y', 55, timecode_t)

/* Get the SDI2 timecode */
#define AJACTL_TIMECODE_SDI2 		_IOR('y', 56, timecode_t)

/* Get the LTC timecode */
#define AJACTL_TIMECODE_LTC 		_IOR('y', 57, timecode_t)


/****************************************************************************************/
/* Stream interface                                                                     */

enum aja_stream_flags {
	AJA_Capture 		= 0x01,
	AJA_Playback 		= 0x02,
	AJA_TriggerAudio 	= 0x04
};

enum aja_stream_frame_flags {
	AJA_ResetAudio 		= 0x01,
	AJA_TimecodeBreak	= 0x02		/* Capture timecode was broken on this frame */
};

typedef struct {
	aja_frametime_t 	timing;				/* Frame timing information */
	int 			flags; 				/* Flags */
	int			inc;				/* Frame increment count */
	int 			chan;				/* Frame channel number */
	uint32_t 		audioptr;			/* Last audio buffer point */
	uint64_t 		priv;				/* User specific data */
	timecode_t 		tc[AJA_TIMECODE_TYPES]; 	/* Timecode Data */
} aja_stream_meta_t;

typedef struct {
	int 			page; 		/* Page numer */
	aja_stream_meta_t 	*meta; 		/* Pointer to metadata structure */
} aja_stream_meta_req_t;

typedef struct {
	int			pagect;		/* Number of pages to allocate */
	int			flags;		/* Flags (see above) */
	int 			chans;		/* Number of video channels */
	int 			tcsource; 	/* Timecode source (from aja_timecode_types) */
} aja_stream_init_t;

typedef struct {
	int			count;		/* Number of dropped frames */
	int64_t 		id;		/* ID of the dropped frame */
	timecode_t 		tc;		/* Timecode (from the TCG) of the dropped frame */
} aja_stream_drop_t;

#define AJACTL_STREAM_RUNNING 		_IO('y', 60)				/* Returns true if engine is running */
#define AJACTL_STREAM_DROPPED 		_IOR('y', 61, aja_stream_drop_t) 	/* Returns the number of dropped frames information structure */
#define AJACTL_STREAM_INIT		_IOW('y', 62, aja_stream_init_t)	/* Initialize the stream */
#define AJACTL_STREAM_START 		_IO('y', 63)				/* Stream start */
#define AJACTL_STREAM_STOP		_IO('y', 64)				/* Stream stop */
#define AJACTL_STREAM_SETMETA 		_IOW('y', 65, aja_stream_meta_req_t)	/* Set the stream metadata */
#define AJACTL_STREAM_GETMETA 		_IOWR('y', 66, aja_stream_meta_req_t)	/* Get the stream metadata */
#define AJACTL_STREAM_PAGE_ALLOC 	_IO('y', 67)				/* Allocate a stream page */
#define AJACTL_STREAM_PAGE_FREE 	_IOW('y', 68, int) 			/* Free a stream page */
#define AJACTL_STREAM_FIFO_PUSH 	_IOW('y', 69, int)			/* Push a page into the fifo */
#define AJACTL_STREAM_FIFO_POP 		_IO('y', 70) 				/* Pop a page from the fifo */
#define AJACTL_STREAM_FIFO 		_IO('y', 72)				/* Get the current FIFO level */
#define AJACTL_STREAM_SETSPEED 		_IOW('y', 73, int)			/* Set the speed of the stream */
#define AJACTL_STREAM_GETSPEED 		_IO('y', 74)				/* Get the speed of the stream */
#define AJACTL_STREAM_SETTRIG 		_IOW('y', 75, int64_t)			/* Set stream trigger point */

/****************************************************************************************/
/* Audio                                                                                */

typedef struct {
	uint32_t 	pos;
	uint32_t 	count;
} aja_audio_position_t;

typedef struct {
	int		frames;
} aja_aframe_t;

#define AJACTL_APLAY_START 		_IO('y', 91)
#define AJACTL_APLAY_STOP 		_IO('y', 92)
#define AJACTL_APLAY_POSITION 		_IOR('y', 93, aja_audio_position_t)
#define AJACTL_ACAP_START 		_IO('y', 94)
#define AJACTL_ACAP_STOP 		_IO('y', 95)
#define AJACTL_ACAP_POSITION 		_IOR('y', 96, aja_audio_position_t)

#define AJACTL_AFRAME_ADD		_IOW('y', 97, aja_aframe_t)
#define AJACTL_AFRAME_GET		_IO('y', 98)

/****************************************************************************************/
/* Benchmarks                                                                           */

/* Ticks are 1/48000 of a second, as read from the on-card timer */
typedef struct {
	uint32_t 		start; 		/* Start tick */
	uint32_t 		stop; 		/* Stop tick */
} aja_benchmark_t;

typedef struct {
	int 			engine;
	aja_benchmark_t 	setup;
	aja_benchmark_t 	xfer;
	aja_benchmark_t 	cleanup;
} aja_dmabench_t;

#define AJACTL_DMABENCH 		_IOR('y', 100, aja_dmabench_t)

/****************************************************************************************/
/* Firmware                                                                             */

struct AjaFirmwareInfo {
	int			id;
	size_t 			bytes;
	uint8_t 		*data;
};

#define AJACTL_FIRMWARE_LOAD 		_IOW('y', 110, struct AjaFirmwareInfo)
#define AJACTL_FIRMWARE_QUERYID 	_IOR('y', 111, int)


/****************************************************************************************/
/* LUTs                                                                                 */

typedef struct {
	int		table;
	uint32_t	c1[512];
	uint32_t	c2[512];
	uint32_t	c3[512];
} aja_lut_t;

#define AJACTL_LUTLOAD			_IOW('y', 120, aja_lut_t)


#endif /* ifndef _IOCTLS_H_ */



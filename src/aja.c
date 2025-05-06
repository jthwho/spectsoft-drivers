/*******************************************************************************
 * aja.c
 *
 * Creation Date: October 03, 2005
 * Author(s): 
 *   Jason Howard <jason@spectsoft.com>
 *
 * 
 * Copyright (c) 2005, SpectSoft
 *   All Rights Reserved.
 *   http://www.spectsoft.com/
 *   info@spectsoft.com
 * 
 * $Id: aja.c 1869 2009-05-07 20:57:20Z jhoward $
 * 
 * DESC: Linux 2.6 driver for the AJA line of video I/O cards
 * 
 ******************************************************************************/

#include <linux/version.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/pagemap.h>
#include <asm/cacheflush.h>
#include <linux/fs.h>
#include <asm/atomic.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <asm/bitops.h>
#include <linux/spinlock.h>
#include <linux/scatterlist.h>
#include <linux/wait.h>

#include <aja_ioctls.h>
#include <aja_registers.h>
#include <p2slave.h>
#include "timecode.h"
#include "utils.h"
#include "linux-abstraction.h"

#define MODNAME 		"ajadriver"
#define LTC_BOARD_NUM 		0
#define MAX_DMA_SIZE		0xF0000
#define AJA_FIRMWARE_TIMEOUT 	500

#define MIN(a, b) (((a) > (b)) ? (b) : (a))
#define MAX(a, b) (((a) > (b)) ? (a) : (b))

/* HACK: Fixes compile bug on older kernels that don't define IRQF_SHARED */
#ifndef IRQF_SHARED
#define IRQF_SHARED 	SA_SHIRQ
#endif

/* Module Parameters */
static int 			force64 		= 1; 
static unsigned int 		max_play_speed 		= 3000000; 		/* 3.000x speed */
static int 			max_dmalist 		= 4000; 		/* Maximum number of scatter gather list items */
static int 			use_dma64		= 1;			/* Should use 64bit DMA (if supported)*/
static int 			dma_merge		= 1; 			/* DMA should attempt to merge adjacent pages */
static int			api_version		= AJA_API_VERSION;

module_param(force64, bool, S_IRUGO);
module_param(max_play_speed, uint, S_IRUGO);
module_param(max_dmalist, int, S_IRUGO);
module_param(use_dma64, bool, S_IRUGO);
module_param(dma_merge, bool, S_IRUGO);
module_param(api_version, int, S_IRUGO);

/* private global driver data */
static CLASS_T 			*aja_class = NULL;
static dev_t 			aja_chrdev = 0;			/* Character Device */
static int 			aja_cards = 0;			/* Number of detected cards */

/* LTC I/O  */
pcitc_ops_t pcitc;
void (*ltcio_register)(pcitc_ops_t *)			= NULL;

/* AJA Card Information */
/* NOTE: This matches the first 7 items in the public aja_cardinfo structure */
typedef struct {
	char			name[32];		/* Card Name */
	uint32_t		id;			/* Card ID */
	uint32_t 		flags;			/* Card flags */
	uint32_t 		acapbuf;		/* Card Aud. Capture Buffer Pointer */
	uint32_t 		aplaybuf;		/* Card Aud. Playback Buffer Pointer */
	uint32_t 		pagesize;		/* Static Pagesize */
	int32_t 		pages;			/* Number of pages on the card */
} aja_cardcap_t;

static const aja_cardcap_t aja_cardcaps[] = {
	{
		"Kona SD-11",
		AJA_KONASD11,
		AJA_SmallReg | AJA_HasAudio | AJA_HasSD,
		AJA_SD_ACAP,
		AJA_SD_APLAY,
		AJA_SD_PAGESIZE,
		64
	},
	
	{
		"Kona HD-11",
		AJA_KONAHD11,
		AJA_HasAudio | AJA_HasHD | AJA_HasLUT,
		AJA_HD_ACAP,
		AJA_HD_APLAY,
		AJA_HD_PAGESIZE,
		32
	},
	
	{
		"Xena HDR-11 MM", 
		AJA_XENAHDR11MM,
		AJA_HasAudio | AJA_HasHD | AJA_HasLUT,
		AJA_HD_ACAP,
		AJA_HD_APLAY,
		AJA_HD_PAGESIZE,
		32
	},

	{
		"Kona/Xena HS", 
		AJA_KONAHS,
		AJA_HasAudio | AJA_HasHD | AJA_HasLUT,
		AJA_HD_ACAP,
		AJA_HD_APLAY,
		AJA_HD_PAGESIZE,
		32
	},

	{
		"Kona/Xena HS (rev 2)", 
		AJA_KONAHS2,
		AJA_HasAudio | AJA_HasHD | AJA_HasLUT,
		AJA_HD_ACAP,
		AJA_HD_APLAY,
		AJA_HD_PAGESIZE,
		32
	},		
	
	{
		"Xena HD-11 MM", 
		AJA_XENAHD11MM,
		AJA_HasAudio | AJA_HasHD | AJA_HasLUT,
		AJA_HD_ACAP,
		AJA_HD_APLAY,
		AJA_HD_PAGESIZE,
		32
	},
	
	{
		"Xena SD-11 MM",
		AJA_XENASD11MM, 	
		AJA_SmallReg | AJA_HasAudio | AJA_HasSD,
		AJA_SD_ACAP,
		AJA_SD_APLAY,
		AJA_SD_PAGESIZE,
		64
	},
	
	{
		"Xena SD-22 MM",
		AJA_XENASD22MM, 	
		AJA_HasAudio | AJA_HasSD,
		AJA_SD_ACAP,
		AJA_SD_APLAY,
		AJA_SD_PAGESIZE,
		64
	},
	
	{
		"Xena HD-22 MM",
		AJA_XENAHD22MM,
		AJA_HasAudio | AJA_HasHD | AJA_HasDualLink | AJA_HasSerial | AJA_HasLUT,
		AJA_HD_ACAP,
		AJA_HD_APLAY,
		AJA_HD_PAGESIZE,
		32
	},
	
	{
		"HDNTV 2 MM", 
		AJA_HDNTV2MM,
		AJA_HasHD,
		AJA_HD_ACAP,
		AJA_HD_APLAY,
		AJA_HD_PAGESIZE,
		32
	},
	
	{
		"Xena SD-11 DA", 	
		AJA_XENASD11DA,
		AJA_SmallReg | AJA_HasAudio | AJA_HasSD,
		AJA_SD_ACAP,
		AJA_SD_APLAY,
		AJA_SD_PAGESIZE,
		64
	},
	
	{
		"Xena HDR-11 DA",
		AJA_XENAHDR11DA,
		AJA_HasAudio | AJA_HasHD | AJA_HasLUT,
		AJA_HD_ACAP,
		AJA_HD_APLAY,
		AJA_HD_PAGESIZE,
		32
	},
	
	{
		"Xena HD-11 DA",
		AJA_XENAHD11DA,
		AJA_HasAudio | AJA_HasHD | AJA_HasLUT,
		AJA_HD_ACAP,
		AJA_HD_APLAY,
		AJA_HD_PAGESIZE,
		32
	},
	
	{
		"Xena HD-22 DA",
		AJA_XENAHD22DA,
		AJA_HasAudio | AJA_HasHD | AJA_HasDualLink | AJA_HasLUT | AJA_HasSerial,
		AJA_HD_ACAP,
		AJA_HD_APLAY,
		AJA_HD_PAGESIZE,
		32
	},
	
	{
		"Xena SD-22 DA",
		AJA_XENASD22DA,
		AJA_HasAudio | AJA_HasSD,
		AJA_SD_ACAP,
		AJA_SD_APLAY,
		AJA_SD_PAGESIZE,
		64
	},
	
	{
		"HDNTV 2 DA",
		AJA_HDNTV2DA,
		AJA_HasHD,
		AJA_HD_ACAP,
		AJA_HD_APLAY,
		AJA_HD_PAGESIZE,
		32
	},
	
	{	"Kona/Xena 2",
		AJA_KONA2,
		AJA_HasAudio | AJA_HasSD | AJA_HasHD | AJA_HasK2 | AJA_HasLUT | AJA_HasSerial | AJA_HasDualLink,
		AJA_K2_ACAP,
		AJA_K2_APLAY,
		AJA_HD_PAGESIZE,
		16
	},

	{	"Kona/Xena 2K/3",
		AJA_KONA3,
		AJA_HasAudio | AJA_HasSD | AJA_HasHD | AJA_HasK2 | AJA_HasLUT | AJA_HasSerial | AJA_HasDualLink,
		AJA_HD_ACAP,
		AJA_HD_APLAY,
		AJA_HD_PAGESIZE,
		32
	},	
	
	{
		"Unknown Board",
		0, /* This must be zero for the search to stop */
		0,
		0,
		0,
		0,
		0
	}
};

/* AJA Card scatter gather list entry */
/* Bit 31 of the "count" variable serves as a DMA direction flag.
 * 1 = To Host Memory
 * 0  = To Card Memory
 *
 * NOTE: This structure should be 16 bytes (4 x 32bit unsigned ints) 
 */
typedef struct {
	uint32_t		hadd;		/* Host Address */
	uint32_t		cadd;		/* Card Address */
	uint32_t		count;		/* Number of 32bit words to move */
	uint32_t		next;		/* Address of next scatter gather entry (this struct) */
	uint32_t		hadd_high;	/* Upper 32bits of host address */
	uint32_t		next_high;	/* Upper 32bits of next address */
	uint32_t		pad[2];		/* 8 pad bytes to make sure this structure always aligns to a page */
}  __attribute__ ((packed)) aja_sglist_t;


/* DMA information struct */
typedef struct {
	int 			engine; 	/* Engine Number */

	struct semaphore	mutex;		/* DMA Engine MUTEXes */
	aja_dmabench_t 		bench; 		/* Benchmark Information */
	int 			dir; 		/* PCI direction */
	int 			count;		/* Number of pages in the buffer */
	int 			sgcnt; 		/* Number of scatter gather entries */
	struct page 		**pages;	/* Array of page pointers for the buffer */
	struct scatterlist 	*sgl;		/* Array of scattergather entries */
	aja_sglist_t 		*list;		/* Hardware linked list */
	dma_addr_t 		list_pac; 	/* Phys address from pci_alloc_consistent */
	int 			irq; 		/* IRQ to use */
	aja_register_t 		reg_hadd; 	/* card register for hardware address */
	aja_register_t 		reg_cadd; 	/* card register for card address */
	aja_register_t 		reg_count; 	/* card register for transfer size */
	aja_register_t 		reg_next; 	/* card register for next dma descriptor */
	aja_register_t 		reg_dmago;	/* card register for dma go flag */
	aja_register_t		reg_hadd_high;	/* card register for hardware address (high 32bits) */
	aja_register_t		reg_next_high;	/* card register for next dma destriptor (high 32bits) */
} aja_dma_t;

typedef struct {
	void 		*next;
	void 		*prev;
	int 		page;
	atomic_t 	inuse;
} aja_pageitem_t;

typedef struct {
	int 			page;
	int 			type;
} aja_lastpage_t;

/* Structure for playback pageflip engine */
typedef struct {
	pid_t 				pid;					/* PID that owns this stream */
	int 				flags;					/* Flip flags */
	int				pagect;					/* Number of pages to use as stream buffer */
	int 				chans;					/* Number of video channels to capture */
	int 				tcsource;				/* Capture timecode source */
	int 				spdct; 					/* Speed count */
	atomic_t 			speed; 					/* Speed in x/1000000 fixed point */
	atomic_t			running;				/* Is the engine running? */
	atomic_t			fifo;					/* Fifo Level */
	volatile aja_stream_drop_t	drop;					/* Dropped frames information */
	volatile int64_t	 	trigger;				/* Trigger id */
	volatile int 			atrig; 					/* Should trigger audio */
	aja_lastpage_t			last[AJA_MAXCHANS]; 			/* The last page allocated */
	aja_stream_meta_t		meta[AJA_MAXPAGES];			/* Frame metadata */
	aja_pageitem_t 			pagealloc[AJA_MAXPAGES]; 		/* Page allocation array */
	aja_pageitem_t 			*pfirst; 				/* First page in the list */
	aja_pageitem_t 			*plast; 				/* Last page in the list */
} aja_stream_t;

enum aja_slavepkt_state {
	AJA_SlaveRx = 0,
	AJA_SlaveTx
};

typedef struct {
	uint8_t				in[64];					/* Input Data */
	uint8_t				out[64];				/* Output Data */
	volatile int64_t 		tstamp; 				/* Interrupt timestamp */
	volatile int			state;					/* State */
	volatile int			inpt;					/* Input Point */
	volatile int			insize;					/* Input Data Size */
	volatile int			outpt;					/* Output Point */
	volatile int 			outsize;				/* Output Data Size */
} aja_slavepkt_t;

enum aja_flags {
	AJA_ResetUART 	= 0x0001,
	AJA_DMA64 	= 0x0002
};

typedef struct {
	atomic_t			frames;				/* Number of frame to play */
	atomic_t			running;			/* Running Flag */
} aja_audio_t;

typedef struct {
	int64_t 			id; 				/* ID of the current frame */
	wait_queue_head_t 		wait; 				/* Frame wait queue */
	aja_frametime_t			time;				/* IRQ timing */
} aja_frameinfo_t;

typedef struct {
	atomic_t 			flags;				/* TCG flags */
	volatile timecode_t 		value;				/* Current timecode generator value */
} aja_tcg_t;

/* This struct contains all the information for a card */
typedef struct {
	int				index;						/* Card number in system */
	int 				fwid;						/* Firmware ID number */
	char 				name[16];					/* Name of the card device */
	dev_t 				devt;						/* Character device id */
	DEVICE_T 			*dev;						/* Driver (udev) device */
	struct cdev 			cdev;						/* Linux char device for this card */
	struct pci_dev 			*pcidev;					/* Linux PCI device for this card */
	volatile int 			flags;						/* Flags */
	spinlock_t 			spin_reg;					/* Card register I/O spinlock */
	aja_frameinfo_t 		frame;						/* Current frame information structure */
	aja_stream_t 			stream;						/* Stream structure */
	aja_tcg_t 			tcg;						/* Timecode generator structure */
	volatile int64_t 		irqcount[AJA_IRQ_TYPES];			/* An array of IRQ counts (one for every type of IRQ) */
	wait_queue_head_t 		irqwait[AJA_IRQ_TYPES];				/* An array of IRQ wait queues (same as above) */
	const aja_cardcap_t		*caps;						/* Capabilities of the board */
	void __iomem			*regadd;					/* Address of the remapped card register space */
	size_t 				reglen;						/* Lenght (in bytes) of the card register space */
	aja_dma_t 			dma[AJA_DMA_COUNT]; 				/* DMA management */
	aja_slavepkt_t			slavepkt;					/* Current P2 slave packet data */
	struct p2slave_t		*p2slave;					/* P2 slave data */
	pcitc_t				*pcitc;						/* LTC I/O device */
	aja_audio_t			aplay;						/* Audio Playback Structure */
} aja_card_t;


/* Driver & device attributes */
static ssize_t aja_attr_show_cards(struct device_driver *drv, char *buf) {
	return snprintf(buf, PAGE_SIZE, "%d", aja_cards);
}
DRIVER_ATTR(cards, 0444, aja_attr_show_cards, NULL);

static int IsValidPage(const int v) {
	if(unlikely(v < 0)) return 0;
	if(unlikely(v >= AJA_MAXPAGES)) return 0;
	return 1;
}

static int IsValidIRQ(const int v) {
	if(unlikely(v < 0)) return 0;
	if(unlikely(v >= AJA_IRQ_TYPES)) return 0;
	return 1;
}

static int IsValidDMA(const int v) {
	switch(v) {
		case AJA_DMA1:
		case AJA_DMA2:
		case AJA_DMA3:
			return 1;
	}
	return 0;
}

static int64_t aja_irqcount(aja_card_t *card, const int type) {
	if(unlikely(!IsValidIRQ(type))) {
		perror("unknown type: %d\n", type);
		return 0;
	}	
	return card->irqcount[type];
}

static int aja_prset(aja_card_t *card, const aja_register_t preg, uint32_t value) {
	unsigned long flags;
	uint32_t reg;
	
	/* Silently fail if we are trying to write to a register that doesn't exist on an older card */
	if(unlikely(((card->caps->flags & AJA_SmallReg) && (preg.add > 255)))) return 0;
	
	if (unlikely((preg.add > card->reglen))) {
		perror("non-existant register: %u\n", preg.add / 4);
		return -1;
	}
	
	if(preg.mask == 0xFFFFFFFF) {
		iowrite32(value, card->regadd + preg.add);
		return 0;
	}

	spin_lock_irqsave(&card->spin_reg, flags);
	reg = ioread32(card->regadd + preg.add) & (~preg.mask);
	value = value << preg.shift;
	iowrite32(reg + value, card->regadd + preg.add);
	spin_unlock_irqrestore(&card->spin_reg, flags);
	return 0;
}

static uint32_t aja_prget(aja_card_t *card, const aja_register_t preg) {
	if(unlikely(((card->caps->flags & AJA_SmallReg) && (preg.add > 255)))) return 0;
	
	if (unlikely((preg.add > card->reglen))) {
		perror("non-existant register: %u\n", preg.add / 4);
		return 0;
	}

	return (ioread32(card->regadd + preg.add) & preg.mask) >> preg.shift;
}

/* Benchmark functions */
static void aja_benchmark_start(aja_card_t *card, aja_benchmark_t *bm) {
	bm->start = aja_prget(card, ajareg_audiocount);
	return;
}

static void aja_benchmark_stop(aja_card_t *card, aja_benchmark_t *bm) {
	bm->stop = aja_prget(card, ajareg_audiocount);
	return;
}

static int aja_irqset(aja_card_t *card, int val) {
	pdebug("card %d: settings interrupts %d\n", card->index, val);
	// If we are disabling interrupts, make sure any running DMAs have been stopped.
	if(!val) {
		aja_prset(card, ajareg_dma1go, 0);
		aja_prset(card, ajareg_dma2go, 0);
		aja_prset(card, ajareg_dma3go, 0);
	}
	aja_prset(card, ajareg_buserrorirqenable, val);
	aja_prset(card, ajareg_dma1irqenable, val);
	aja_prset(card, ajareg_dma2irqenable, val);
	aja_prset(card, ajareg_dma3irqenable, val);
	aja_prset(card, ajareg_outputirqenable, val);
	aja_prset(card, ajareg_input1irqenable, val);
	aja_prset(card, ajareg_input2irqenable, val);
	aja_prset(card, ajareg_audiowrapirqenable, val);

	if(card->caps->flags & AJA_HasSerial) {
		aja_prset(card, ajareg_uarttxirqenable, val);
		aja_prset(card, ajareg_uartrxirqenable, val);
		aja_prset(card, ajareg_uartenabletx, val);
		aja_prset(card, ajareg_uartenablerx, val);
	}
	return 0;
}

static aja_audio_position_t aja_aplay_position(aja_card_t *card) {
	aja_audio_position_t pos;
	pos.pos = aja_prget(card, ajareg_aplaylast);
	pos.count = aja_prget(card, ajareg_audiocount);
	return pos;
}

/* Function starts the audio playback */
static int aja_aplay_start(aja_card_t *card) {
	aja_prset(card, ajareg_aplayreset, 1);
	aja_prset(card, ajareg_aplayreset, 0);
	return 0;
}

/* Function stops the audio playback */
static int aja_aplay_stop(aja_card_t *card) {
	aja_prset(card, ajareg_aplayreset, 1);
	return 0;
}

static aja_audio_position_t aja_acap_position(aja_card_t *card) {
	aja_audio_position_t pos;
	pos.pos = aja_prget(card, ajareg_acaplast);
	pos.count = aja_prget(card, ajareg_audiocount);
	return pos;
}

/* Function starts the audio capture */
static int aja_acap_start(aja_card_t *card) {
	aja_prset(card, ajareg_dma3irqenable, 1);
	aja_prset(card, ajareg_acapreset, 1);
	aja_prset(card, ajareg_acapenable, 0);
	aja_prset(card, ajareg_acapreset, 0);
	aja_prset(card, ajareg_acapreset, 1);
	aja_prset(card, ajareg_acapreset, 0);
	aja_prset(card, ajareg_acapenable, 1);
	return 0;
}

/* Function stops the audio playback */
static int aja_acap_stop(aja_card_t *card) {
	aja_prset(card, ajareg_acapenable, 0);
	return 0;
}

/*****************************************************************************/
/* TIMECODE                                                                  */
/*****************************************************************************/

static int aja_timecode_getfps(aja_card_t *card) {
	uint32_t d = aja_prget(card, ajareg_framerate);
	switch(d) {
		case 1: /* 60 */
		case 2: /* 59.94 */
			return 60;
		case 3: /* 30 */
		case 4: /* 29.97 */
			return 30;
		case 5: /* 25 */
			return 25;
		case 6: /* 24 */
		case 7: /* 23.976 */
			return 24;
	}
	return 0;
}

static int aja_timecode_set_sdi1(aja_card_t *card, const timecode_t *tc) {
	timecode_rp188_t rp = timecode_to_rp188(tc);
	aja_prset(card, ajareg_ch1rp188_1, rp.d1);
	aja_prset(card, ajareg_ch1rp188_2, rp.d2);
	return 0;
}

static int aja_timecode_get_sdi1(aja_card_t *card, timecode_t *tc) {
	timecode_rp188_t rp;
	rp.d1 = aja_prget(card, ajareg_ch1rp188_1);
	rp.d2 = aja_prget(card, ajareg_ch1rp188_2);
	*tc = timecode_from_rp188(&rp);
	tc->fps = aja_timecode_getfps(card);
	return 0;
}

static int aja_timecode_set_sdi2(aja_card_t *card, const timecode_t *tc) {
	timecode_rp188_t rp = timecode_to_rp188(tc);
	aja_prset(card, ajareg_ch2rp188_1, rp.d1);
	aja_prset(card, ajareg_ch2rp188_2, rp.d2);
	return 0;
}

static int aja_timecode_get_sdi2(aja_card_t *card, timecode_t *tc) {
	timecode_rp188_t rp;
	rp.d1 = aja_prget(card, ajareg_ch2rp188_1);
	rp.d2 = aja_prget(card, ajareg_ch2rp188_2);
	*tc = timecode_from_rp188(&rp);
	tc->fps = aja_timecode_getfps(card);
	return 0;
}

static void aja_timecode_set_ltc(aja_card_t *card, const timecode_t *ntc) {
	timecode_t tc;
	if(!card->pcitc) return;
	if(!pcitc.readtcgen(card->pcitc, &tc)) {
		if(!timecode_compare(&tc, ntc)) {
			char buf1[32], buf2[32];
			pcitc.writetc(card->pcitc, ntc);
			timecode_to_string(&tc, buf1, 32);
			timecode_to_string(ntc, buf2, 32);
			//pinfo("LTC GEN shows %s, setting to %s\n", buf1, buf2);
		}
	}
	return;
}

static int aja_timecode_get_ltc(aja_card_t *card, timecode_t *tc) {
	if(card->pcitc) return pcitc.readtc(card->pcitc, tc);
	timecode_init(tc);
	return 0;
}

static void aja_timecode_setflags(aja_card_t *card, int flags) {
	int cf = atomic_read(&card->tcg.flags);
	atomic_set(&card->tcg.flags, cf | flags);
	return;
}

static void aja_timecode_clrflags(aja_card_t *card, int flags) {
	int cf = atomic_read(&card->tcg.flags);
	atomic_set(&card->tcg.flags, cf & (~flags));
	return;
}

static int aja_ioctl_timecode_getflags(aja_card_t *card, const unsigned long v) {
	int f = atomic_read(&card->tcg.flags);
	copy_to_user((void *)v, (const void *)&f, sizeof(f));
	return 0;
}

static int aja_ioctl_timecode_setflags(aja_card_t *card, const unsigned long v) {
	int nf;
	copy_to_user((void *)&nf, (const void *)v, sizeof(nf));
	aja_timecode_setflags(card, nf);
	return 0;
}

static int aja_ioctl_timecode_clrflags(aja_card_t *card, const unsigned long v) {
	int nf;
	copy_to_user((void *)&nf, (const void *)v, sizeof(nf));
	aja_timecode_clrflags(card, nf);
	return 0;
}

static int aja_ioctl_timecode_set(aja_card_t *card, const unsigned long v) {
	unsigned long flags;
	spin_lock_irqsave(&card->spin_reg, flags);
	copy_from_user((void *)&card->tcg.value, (const void *)v, sizeof(card->tcg.value));
	spin_unlock_irqrestore(&card->spin_reg, flags);
	return 0;
}

static int aja_ioctl_timecode_get(aja_card_t *card, const unsigned long v) {
	unsigned long flags;
	spin_lock_irqsave(&card->spin_reg, flags);
	copy_to_user((void *)v, (const void *)&card->tcg.value, sizeof(card->tcg.value));
	spin_unlock_irqrestore(&card->spin_reg, flags);
	return 0;
}

static int aja_ioctl_timecode_sdi1(aja_card_t *card, const unsigned long v) {
	timecode_t tc;
	aja_timecode_get_sdi1(card, &tc);
	copy_to_user((void *)v, (const void *)&tc, sizeof(tc));
	return 0;
}

static int aja_ioctl_timecode_sdi2(aja_card_t *card, const unsigned long v) {
	timecode_t tc;
	aja_timecode_get_sdi2(card, &tc);
	copy_to_user((void *)v, (const void *)&tc, sizeof(tc));
	return 0;
}

static int aja_ioctl_timecode_ltc(aja_card_t *card, const unsigned long v) {
	timecode_t tc;
	aja_timecode_get_ltc(card, &tc);
	copy_to_user((void *)v, (const void *)&tc, sizeof(tc));
	return 0;
}

/*
 * This is commented out because of the class abstraction used in
 * DEVICE_T.  I need to figure out a way to make this abstract as well. 
static ssize_t aja_show_tcg(struct class_device *cls, char *buf) {
	aja_card_t *card = (aja_card_t *)cls->class_data;
	timecode_t 	tc = card->tcg.value;
	if(timecode_is_valid(&tc)) {
		char tcstr[32];
		timecode_to_string(&tc, tcstr, 32);
		return snprintf(buf, PAGE_SIZE, "%s %dfps %08X\n", 
			tcstr, tc.fps, tc.userbits);
	}
	return snprintf(buf, PAGE_SIZE, "INVALID\n");
}
CLASS_DEVICE_ATTR(tcg, 0444, aja_show_tcg, NULL);
*/

/* Creates a new cardinfo structure */
static aja_card_t *aja_card_create(struct pci_dev *pcidev, uint32_t id, int index) {
	int i;
	aja_dma_t *dma;
	const char *pciname = pci_name(pcidev);
	aja_card_t *card = (aja_card_t *)kmalloc(sizeof(aja_card_t), GFP_KERNEL);
	if (unlikely(card == NULL)) return NULL;
	
	/* Initalize the the structure */
	memset(card, 0, sizeof(aja_card_t));
	pci_set_drvdata(pcidev, (void *)card);
	card->pcidev = pcidev;
	spin_lock_init(&card->spin_reg);

	/* Card data */
	card->index = index;
	card->fwid = 0;		// 0 indicates that no firmware is loaded
	card->devt = MKDEV(MAJOR(aja_chrdev), index * AJA_MINORS_PER_CARD);
	sprintf(card->name, "aja%d", index);

	/* Intialize the frame count system */
	init_waitqueue_head(&card->frame.wait);

	/* IRQ wait queues */
	for(i = 0; i < AJA_IRQ_TYPES; i++) {
		init_waitqueue_head(&card->irqwait[i]);
	}

	/* Card capabilities */
	for(i = 0; aja_cardcaps[i].id != 0; i++) if(aja_cardcaps[i].id == id) break;
	card->caps = &aja_cardcaps[i];

	/* DMA engines */
	for(i = 0; i < AJA_DMA_COUNT; i++) {
		unsigned long s;
		dma = &card->dma[i];
		dma->engine = i;
		init_MUTEX(&dma->mutex);
		
		dma->pages = kmalloc(max_dmalist * sizeof(struct page *), GFP_KERNEL);
		if(unlikely(dma->pages == NULL)) {
			perror("DMA %d: couldn't allocate space for the page list!\n", i);
		}

		dma->sgl = kmalloc(max_dmalist * sizeof(struct scatterlist), GFP_KERNEL);
		if(unlikely(dma->sgl == NULL)) {
			perror("DMA %d: couldn't allocate space for the scatterlist!\n", i);
		}

		s = max_dmalist * sizeof(aja_sglist_t);
		dma->list = pci_alloc_consistent(pcidev, s, &dma->list_pac);	
		if(dma->list == NULL) {
			perror("%s: DMA %d: failed to allocate scatter list buffer\n", pciname, i);
		} else {
			pinfo("%s: DMA %d scatter list is %lu bytes. CPU 0x%lX, Bus 0x%lX\n", 
				pciname, i, s, (unsigned long)dma->list, (unsigned long)dma->list_pac);
		}

		/* Setup the DMA engine parameters */
		switch(i) {
			case 0:
				dma->reg_hadd		= ajareg_dma1hadd;
				dma->reg_cadd		= ajareg_dma1cadd;
				dma->reg_count		= ajareg_dma1count;
				dma->reg_next		= ajareg_dma1next;
				dma->reg_dmago		= ajareg_dma1go;
				dma->reg_hadd_high	= ajareg_dma1hadd_high;
				dma->reg_next_high	= ajareg_dma1next_high;
				dma->irq		= AJA_DMA1;
				break;

			case 1:
				dma->reg_hadd 		= ajareg_dma2hadd;
				dma->reg_cadd	 	= ajareg_dma2cadd;
				dma->reg_count	 	= ajareg_dma2count;
				dma->reg_next 		= ajareg_dma2next;
				dma->reg_dmago 		= ajareg_dma2go;
				dma->reg_hadd_high	= ajareg_dma2hadd_high;
				dma->reg_next_high	= ajareg_dma2next_high;
				dma->irq		= AJA_DMA2;
				break;

			case 2:
				dma->reg_hadd	 	= ajareg_dma3hadd;
				dma->reg_cadd 		= ajareg_dma3cadd;
				dma->reg_count		= ajareg_dma3count;
				dma->reg_next 		= ajareg_dma3next;
				dma->reg_dmago 		= ajareg_dma3go;
				dma->reg_hadd_high 	= ajareg_dma3hadd_high;
				dma->reg_next_high	= ajareg_dma3next_high;
				dma->irq 		= AJA_DMA3;
				break;
		}
	}
	return card;
}

static void aja_card_free(aja_card_t *card) {
	int i;
	aja_dma_t *dma;

	if(unlikely(card == NULL)) return;

	/* DMA engines */
	for(i = 0; i < AJA_DMA_COUNT; i++) {
		dma = &card->dma[i];
		if(dma->pages) kfree(dma->pages);
		if(dma->sgl) kfree(dma->sgl);
		if(dma->list) {
			pci_free_consistent(card->pcidev, max_dmalist * sizeof(aja_sglist_t), dma->list, dma->list_pac);
		}
	}
	kfree(card);
	return;
}


static int aja_dma_getpages(aja_dma_t *dma, unsigned long data, unsigned long size) {
	int i, err;
	if(dma->count > max_dmalist) {
		perror("DMA %d: Page count of %d is greater than max_dmalist (%d)\n",
				dma->engine, dma->count, max_dmalist);
		return -EINVAL;
	}

	/* Try to fault in all of the necessary pages */
	down_read(&current->mm->mmap_sem);
	err = get_user_pages(
		current,
		current->mm,
		data & PAGE_MASK,
		dma->count,
		(dma->dir == PCI_DMA_FROMDEVICE),
		0, /* force */
		dma->pages,
		NULL);
	up_read(&current->mm->mmap_sem);

	if(err != dma->count) {
		perror("DMA %d: get_user_pages failed with %d (should be %d)  Data: 0x%lX Size: 0x%lX\n", 
				dma->engine, err, dma->count, data, size);
		return err < 0 ? err : -EINVAL;
	}

	for(i = 0; i < dma->count; i++) {
		BUG_ON(dma->pages[i] == NULL);
		flush_dcache_page(dma->pages[i]);
	}
	return 0;
}

static int aja_dma_mapsg(aja_card_t *card, aja_dma_t *dma, unsigned long data, unsigned long size) {
	int 		i;
	unsigned long 	off = data & ~PAGE_MASK;
	unsigned long 	fbs = MIN(size, PAGE_SIZE - off);
	sg_init_table(dma->sgl, dma->count);

	/* The first mapped page is a bit weird, as the actual buffer can start anywhere inside it */
	sg_set_page(&dma->sgl[0], dma->pages[0], fbs, off);
	size -= fbs;

	for(i = 1; i < dma->count; i++) {
		fbs = MIN(size, PAGE_SIZE);
		sg_set_page(&dma->sgl[i], dma->pages[i], fbs, 0);
		size -= fbs;
	}

	dma->sgcnt = pci_map_sg(card->pcidev, dma->sgl, dma->count, dma->dir);
	if(dma->sgcnt < 1) {
		perror("DMA %d: pci_map_sg failed with %d\n", dma->engine, dma->sgcnt);
		return -EFAULT;
	}
	return 0;

}

static int aja_dma_cardsg(aja_dma_t *dma, uint32_t cardoff, int dma64) {
	int 		i;
	unsigned long	paddr = 0, addr, size, tsize = 0;
	uint32_t 	tcm = dma->dir == PCI_DMA_FROMDEVICE ? 0x80000000 : 0x00000000;
	aja_sglist_t 	*next = (aja_sglist_t *)dma->list_pac;
	aja_sglist_t	*list = dma->list, *plist = NULL;
	if(dma64) tcm |= 0x10000000;

	for (i = 0; i < dma->sgcnt; i++) {
		addr = sg_dma_address(&dma->sgl[i]);
		size = sg_dma_len(&dma->sgl[i]);
		if(dma_merge && plist && tsize < MAX_DMA_SIZE && paddr == addr) {
			// If the address of this block is next to the end of the last block,
			// we can merge these blocks into a single transfer.
			tsize += size;
			paddr += size;
			cardoff += size;
			plist->count += (size / 4);
		} else {
			next++;
			list->hadd = addr;
			list->cadd = cardoff;
			list->count = (size / 4) | tcm;
			list->next = (unsigned long)next;
			list->hadd_high = addr >> 32;
			list->next_high = (unsigned long)next >> 32;
			plist = list;
			paddr = addr + size;
			cardoff += size;
			tsize = size;
			list++;
		}
	}
	if(plist) {
		plist->next = 0;
		plist->next_high = 0;
	}
	return 0;
}

/* This function maps all the pages of the userspace buffer referenced
 * in the dmainfo structure to physical pages and then assembles a
 * dmamem structure that is then ready to be DMAed */
static int aja_dma_init(aja_card_t *card, aja_dma_t *dma, int dir, unsigned long data, uint32_t cardoff, unsigned long size) {
	int 		err;
	unsigned long 	fpage = data >> PAGE_SHIFT;
	unsigned long 	lpage = (data + size - 1) >> PAGE_SHIFT;
	dma->count = lpage - fpage + 1;
	dma->dir = (dir == AJA_DMATOCARD) ? PCI_DMA_TODEVICE : PCI_DMA_FROMDEVICE;

	/* Start the benchmark */
	aja_benchmark_start(card, &dma->bench.setup);

	if(!dma->pages || !dma->sgl || !dma->list) {
		perror("DMA %d: System failed to allocated needed buffers.  Aborted.\n", dma->engine);
		return -ENOMEM;
	}
	if(unlikely(!data)) {
		perror("DMA %d: User buffer is NULL!  Aborted.\n", dma->engine);
		return -EFAULT;
	}
	if(unlikely(size < 8)) {
		perror("DMA %d: Transfer length must be >= 8 bytes (len: %ld).  Aborted.\n", dma->engine, size);
		return -ERANGE;
	}
	if (unlikely((data + size) < data)) {
		perror("DMA %d: Address and length overflow address bus!  Aborted.\n", dma->engine);
		return -EOVERFLOW;
	}

	/* Map the userspace pages into kernel space */
	err = aja_dma_getpages(dma, data, size);
	if(err) return err;

	/* Build a scatter gather list */
	err = aja_dma_mapsg(card, dma, data, size);
	if(err) return err;

	/* Build the card list */
	err = aja_dma_cardsg(dma, cardoff, card->flags & AJA_DMA64);
	if(err) return err;

	pdebug("DMA %d: [%s] Card 0x%X, Mem 0x%lX, %ld bytes.  %d pages, %d sg entries\n", 
		dma->engine, dir == AJA_DMATOCARD ? "TO CARD" : "FROM CARD",
		cardoff, data, size, dma->count, dma->sgcnt);
	
	aja_benchmark_stop(card, &dma->bench.setup);
	return 0;
}

static void aja_dma_cleanup(aja_card_t *card, aja_dma_t *dma) {
	int i, dirty = (dma->dir == PCI_DMA_FROMDEVICE);
	struct page *p;

	/* Start the cleanup benchmark */
	aja_benchmark_start(card, &dma->bench.cleanup);

//	if(dma->dir == PCI_DMA_FROMDEVICE) {
//		dma_sync_sg_for_cpu(&card->pcidev->dev, dma->sgl, dma->count, DMA_FROM_DEVICE);
//	}
	
	/* Unmap the scatter list */
	pci_unmap_sg(card->pcidev, dma->sgl, dma->count, dma->dir);

	/* Release the pages, making sure to mark them dirty if we wrote to them */
	for(i = 0; i < dma->count; i++) {
		p = dma->pages[i];
		if(dirty && !PageReserved(p)) { SetPageDirty(p); }
		page_cache_release(p);
	}

	/* Stop the cleanup benchmark */
	aja_benchmark_stop(card, &dma->bench.cleanup);
	return;
}

static int aja_dma(aja_card_t *card, aja_dmainfo_t *dmainfo) {
	int 			i, ret = 0;
	aja_dma_t 		*dma = NULL;

	/* Try to get a DMA engine */
	for(i = 0; i < AJA_DMA_COUNT; i++) {
		if(!down_trylock(&card->dma[i].mutex)) {
			dma = &card->dma[i];
			break;
		}
	}
	
	if(dma == NULL) {
			perror("DMA: Engine is busy.  Unable to allocate an engine!  Aborted.\n");
			return -EBUSY;
	}

	/* Ok, now initialize the dma structure */
	ret = aja_dma_init(card, dma, dmainfo->dir, (unsigned long)dmainfo->uadd, dmainfo->cadd, dmainfo->len);
	if(unlikely(ret)) goto dma_fail1;

	/* Start the benchmark */
	aja_benchmark_start(card, &dma->bench.xfer);

	/* Load the initial dma vector onto the card */
	aja_prset(card, dma->reg_hadd, dma->list[0].hadd);
	aja_prset(card, dma->reg_cadd, dma->list[0].cadd);
	aja_prset(card, dma->reg_count, dma->list[0].count);
	aja_prset(card, dma->reg_next, dma->list[0].next);
	if(card->flags & AJA_DMA64) {
		aja_prset(card, dma->reg_hadd_high, dma->list[0].hadd_high);
		aja_prset(card, dma->reg_next_high, dma->list[0].next_high);
	}
	aja_prset(card, dma->reg_dmago, 1);

	/* Start the DMA and block until we get word that the DMA has finished */
	if(!wait_event_interruptible_timeout(card->irqwait[dma->irq], (!aja_prget(card, dma->reg_dmago)), 1000)) {
		aja_prset(card, dma->reg_dmago, 0);
		perror("DMA %d: Engine timed out or was interrupted.  Aborted.\n", dma->engine);
		ret = -ETIMEDOUT;
	}

	/* Stop the transfer benchmark */
	aja_benchmark_stop(card, &dma->bench.xfer);

	/* Clean up the buffers */
	aja_dma_cleanup(card, dma);

dma_fail1:
	/* Unlock the engine */
	up(&dma->mutex);
	return ret;

}

static int aja_firmware_ready_wait(aja_card_t *card) {
	void __iomem *preg = card->regadd + 0x44;
	int i;
	// Waiting for program/ready bit to clear
	for(i = 0; i < AJA_FIRMWARE_TIMEOUT; i++) {
		if(!(ioread32(preg) & 0x100)) break;
		mdelay(1);
	}
	if(i == AJA_FIRMWARE_TIMEOUT) return 1;
	return 0;
}

static int aja_firmware_done_wait(aja_card_t *card) {
	void __iomem *preg = card->regadd + 0x44;
	int i;
	// Waiting for done bit to go high
	for(i = 0; i < AJA_FIRMWARE_TIMEOUT; i++) {
		if(ioread32(preg) & 0x200) break;	// Break on 
		mdelay(1);
	}
	if(i == AJA_FIRMWARE_TIMEOUT) return 1;
	return 0;
}

static int aja_firmware_upload(aja_card_t *card, size_t bytes, uint8_t *data) {
	void __iomem *preg = card->regadd + 0x44;
	uint32_t val;
	for( ; bytes > 0; bytes--) {
		val = *data++;
		iowrite32(val, preg);
	}
	return 0;
}

static int aja_alive_wait(aja_card_t *card) {
	void __iomem *preg = card->regadd + 0x70;
	int 		i, ok = 0;
	uint32_t 	v1, v2;

	v1 = ioread32(preg);
	for(i = AJA_FIRMWARE_TIMEOUT; i > 0; i--) {
		v2 = ioread32(preg);
		if(v1 == v2) {
			mdelay(1);
		} else {
			if(ok++ == 10) break;
			v1 = v2;
			mdelay(33);
		}
	}
	return i ? 0 : 1;
}

static int aja_firmware_load(aja_card_t *card, int id, size_t bytes, uint8_t *data) {
	void __iomem *preg = card->regadd + 0x44;

	pinfo("Card %d: Loading firmware 0x%X, %zd bytes\n", card->index, id, bytes);

	// Start the programing cycle
	card->fwid = id;
	aja_irqset(card, 0);

	iowrite32(0x2000000, preg); // PCI-X programming mode
	iowrite32(0x100, preg);	// Start FPGA program
	if(aja_firmware_ready_wait(card)) {
		perror("Firmware 0x%X ready wait failed\n", id);
		goto fwloadbail;
	}
	mdelay(50);	// Not documented, but there must be a delay here or the board will fail to program
	if(aja_firmware_upload(card, bytes, data)) {
		perror("Firmware 0x%X upload failed\n", id);
		goto fwloadbail;
	}
	if(aja_firmware_done_wait(card)) {
		perror("Firmware 0x%X done wait failed\n", id);
		goto fwloadbail;
	}

	iowrite32(0x400, preg);	// Reset the FPGA
	if(aja_alive_wait(card)) {
		perror("Firmware 0x%X wait for alive failed\n", id);
		goto fwloadbail;
	}
	aja_irqset(card, 1);
	return 0;

fwloadbail:
	card->fwid = 0;	// Upon failure, we set the fwid back to the no firmware state.
	return -EBUSY;
}

static int aja_ioctl_firmware_load(aja_card_t *card, const unsigned long v) {
	struct AjaFirmwareInfo 	fi;
	uint8_t 		*data = NULL;
	unsigned long 		ret;
	int 			fwret;

	/* Copy the initial structure into kernel space */
	ret = copy_from_user((void *)&fi, (const void *)v, sizeof(fi));
	if(ret) {
		perror("copy firmware info failed with %lu\n", ret);
		goto fwloadbail;
	}
	/* Allocate the firmware data structure */
	data = (uint8_t *)vmalloc(fi.bytes);
	if(data == NULL) {
		perror("vmalloc failed, %zu bytes\n", fi.bytes);
		goto fwloadbail;
	}
	/* Copy the firmware data from userspace */
	ret = copy_from_user((void *)data, (const void *)fi.data, fi.bytes);
	if(ret) {
		perror("copy firmware failed with %lu\n", ret);
		goto fwloadbail;
	}

	/* Load the firmware */
	fwret = aja_firmware_load(card, fi.id, fi.bytes, data);
	vfree(data);
	return fwret;

fwloadbail:
	if(data != NULL) vfree(data);
	return -ENOMEM;
}

static int aja_ioctl_firmware_queryid(aja_card_t *card, const unsigned long v) {
	return put_user(card->fwid, (int *)v) ? : 0;
}

static int aja_ioctl_boardinfo(aja_card_t *card, const unsigned long v) {
	aja_boardinfo_t bi;
	unsigned int t;
	if(unlikely(card->caps == NULL)) return -1;
	memset((void *)&bi, 0, sizeof(aja_boardinfo_t));
	memcpy((void *)&bi, card->caps, sizeof(aja_cardcap_t));
	bi.fwver = aja_prget(card, ajareg_firmwarerev);
	bi.fpgaver = aja_prget(card, ajareg_fpgaversion);
	bi.boardver = aja_prget(card, ajareg_boardversion);
	bi.acapsize = AJA_ACAP_CARDSIZE;
	bi.aplaysize = AJA_APLAY_CARDSIZE;
	snprintf(bi.dver, sizeof(bi.dver), "%d", AJA_API_VERSION);
	t = aja_prget(card, ajareg_serialhigh);
	memcpy(bi.serial + 4, &t, 4);
	t = aja_prget(card, ajareg_seriallow);
	memcpy(bi.serial, &t, 4);
	copy_to_user((void *)v, (const void *)&bi, sizeof(aja_boardinfo_t));
	return 0;
}

static int aja_ioctl_getregister(aja_card_t *card, const unsigned long v) {
	aja_registerio_t rio;
	copy_from_user((void *)&rio, (const void *)v, sizeof(rio.reginfo));
	rio.value = aja_prget(card, rio.reginfo);
	copy_to_user((void *)v + sizeof(rio.reginfo), (const void *)&rio + sizeof(rio.reginfo), sizeof(rio.value));
	return 0;
}

static int aja_ioctl_setregister(aja_card_t *card, const unsigned long v) {
	aja_registerio_t rio;
	copy_from_user((void *)&rio, (const void *)v, sizeof(aja_registerio_t));
	return aja_prset(card, rio.reginfo, rio.value);
}

static int aja_ioctl_irqcount(aja_card_t *card, const unsigned long v) {
	aja_irqcountget_t get;
	copy_from_user((void *)&get, (const void *)v, sizeof(get.type));
	get.count = aja_irqcount(card, get.type);
	copy_to_user((void *)v, (const void *)&get, sizeof(aja_irqcountget_t));
	return 0;
}

static int aja_ioctl_irqsleep(aja_card_t *card, const unsigned long v) {
	unsigned long long cirq;
	wait_queue_t wait;
	if(unlikely(!IsValidIRQ(v))) return -1;
	cirq = aja_irqcount(card, v);
	init_waitqueue_entry(&wait, current);
	add_wait_queue(&card->irqwait[v], &wait);
	while(aja_irqcount(card, v) == cirq) {
		set_current_state(TASK_INTERRUPTIBLE);
		if(unlikely(signal_pending(current))) break;
		schedule();
	}
	set_current_state(TASK_RUNNING);
	remove_wait_queue(&card->irqwait[v], &wait);
	return 0;
}

/******************************************************************************/
/* Frame ID                                                                   */

/* Waits for an frame interrupt.  Returns the number of jiffies left to wait.
 * Or 0 on a timeout (didn't get frame).  You still need to check 
 * signal_pending() to see if the wait was interrupted by a signal */
static int aja_wait_for_frame(aja_card_t *card) {
	int64_t cid = card->frame.id;
	return wait_event_interruptible_timeout(
			card->frame.wait,
			(card->frame.id != cid),
			HZ / 4	// Wait up to 1/4 of a second for the next frame
	);
};

static int aja_ioctl_frame_curid(aja_card_t *card, const unsigned long v) {
	int64_t val = card->frame.id;
	copy_to_user((void *)v, (const void *)&val, sizeof(val));
	return 0;
}

static int aja_ioctl_frame_curtime(aja_card_t *card, const unsigned long v) {
	unsigned long flags;
	spin_lock_irqsave(&card->spin_reg, flags);
	copy_to_user((void *)v, (const void *)&card->frame.time, sizeof(card->frame.time));
	spin_unlock_irqrestore(&card->spin_reg, flags);
	return 0;
}

static int aja_ioctl_frame_waitfornext(aja_card_t *card, const unsigned long v) {
	int ret = aja_wait_for_frame(card);
	if(unlikely(signal_pending(current))) return -EINTR;	// Interrupted by a signal
	return ret ? 0 : -ETIMEDOUT;
}

static int aja_ioctl_dma(aja_card_t *card, const unsigned long v) {
	aja_dmainfo_t dmainfo;
	copy_from_user((void *)&dmainfo, (const void *)v, sizeof(aja_dmainfo_t));
	return aja_dma(card, &dmainfo);
}

/*****************************************************************************/
/* Streaming interface */

/* Clears the stream structure */
static void aja_stream_clear(aja_card_t *card) {
	int 		i;
	unsigned long 	flags;
	//pinfo("Clear stream\n");
	spin_lock_irqsave(&card->spin_reg, flags);
	card->stream.pid = current->pid;
	card->stream.pagect = 0;
	card->stream.spdct = 0;
	card->stream.chans = 0;
	card->stream.drop.count = 0;
	card->stream.drop.id = 0;
	atomic_set(&card->stream.running, 0);
	atomic_set(&card->stream.fifo, 0);
	card->stream.trigger = 0;
	atomic_set(&card->stream.speed, 1 * AJA_SPEED_DIVISOR);
	card->stream.pfirst = NULL;
	card->stream.plast = NULL;
	memset(card->stream.last, 0, sizeof(card->stream.last));
	for(i = 0; i < AJA_MAXPAGES; i++) {
		aja_pageitem_t *p = &card->stream.pagealloc[i];
		p->next = NULL;
		p->prev = NULL;
		p->page = i;
		atomic_set(&p->inuse, 0);
	}
	spin_unlock_irqrestore(&card->spin_reg, flags);
	return;
}

static int aja_stream_page_alloc(aja_card_t *card) {
	int 		i, ret = -EAGAIN;
	int 		pagect = card->stream.pagect;
	unsigned long 	flags;

	spin_lock_irqsave(&card->spin_reg, flags);
	for(i = 0; i < pagect; i++) {
		if(!atomic_read(&card->stream.pagealloc[i].inuse)) {
			card->stream.pagealloc[i].prev = NULL;
			card->stream.pagealloc[i].next = NULL;
			atomic_set(&card->stream.pagealloc[i].inuse, 1);
			ret = i;
			break;
		}
	}
	spin_unlock_irqrestore(&card->spin_reg, flags);
	//pinfo("ALLOC: %d\n", ret);
	return ret;
}

static void aja_stream_page_free(aja_card_t *card, int page) {
	unsigned long flags;
	if(page < 0 || page >= AJA_MAXPAGES) return;
	spin_lock_irqsave(&card->spin_reg, flags);
	atomic_set(&card->stream.pagealloc[page].inuse, 0);
	spin_unlock_irqrestore(&card->spin_reg, flags);
	//pinfo("FREE: %d\n", page);
	return;
}

static int aja_stream_fifo_push(aja_card_t *card, int page) {
	unsigned long 		flags;
	aja_pageitem_t 		*p, *np;

	if(!IsValidPage(page)) return -EINVAL;
	p = (aja_pageitem_t *)&card->stream.pagealloc[page];
	spin_lock_irqsave(&card->spin_reg, flags);
	if(card->stream.plast == NULL) {
		card->stream.pfirst = card->stream.plast = p;
		p->next = NULL;
		p->prev = NULL;
	} else {
		np = (aja_pageitem_t *)card->stream.plast;
		np->next = p;
		p->prev = np;
		p->next = NULL;
		card->stream.plast = p;
	}
	atomic_inc(&card->stream.fifo);
	spin_unlock_irqrestore(&card->spin_reg, flags);
	//pinfo("PUSH: %d\n", page);
	//pinfo("push %d, fifo %d\n", page, atomic_read(&card->stream.fifo));
	//
	//p = card->stream.pfirst;
	//while(p != NULL) {
	//	pinfo("ITEM %d\n", p->page);
	//	p = p->next;
	//}
	return 0;
}

static int aja_stream_fifo_pop(aja_card_t *card) {
	unsigned long 		flags;
	aja_pageitem_t 		*p, *np;
	int 			ret = -EAGAIN;

	spin_lock_irqsave(&card->spin_reg, flags);
	p = (aja_pageitem_t *)card->stream.pfirst;
	if(p != NULL) {
		ret = p->page;
		np = p->next;
		if(np == NULL) {
			card->stream.plast = NULL;
		} else {
			np->prev = NULL;
		}
		card->stream.pfirst = np;
	}
	atomic_dec(&card->stream.fifo);
	spin_unlock_irqrestore(&card->spin_reg, flags);
	//pinfo("POP: %d\n", ret);
	//pinfo("pop %d, fifo %d\n", ret, atomic_read(&card->stream.fifo));
	//p = card->stream.pfirst;
	//while(p != NULL) {
	//	pinfo("ITEM %d\n", p->page);
	//	p = p->next;
	//}	
	return ret;
}

static void aja_stream_start(aja_card_t *card) {
	card->stream.trigger = 0;
	card->stream.atrig = card->stream.flags & AJA_TriggerAudio;
	atomic_set(&card->stream.running, 1);
	if(card->pcitc) pcitc.run(card->pcitc, 1);
	aja_timecode_setflags(card, AJA_TCG_Running);
	return;
}

static void aja_stream_stop(aja_card_t *card) {
	card->stream.trigger = 0;
	card->stream.atrig = 0;
	atomic_set(&card->stream.running, 0);
	if(card->pcitc) pcitc.run(card->pcitc, 0);
	aja_timecode_clrflags(card, AJA_TCG_Running);

	if(card->stream.flags & AJA_TriggerAudio) {
		if(card->stream.flags & AJA_Playback) aja_aplay_stop(card);
		if(card->stream.flags & AJA_Capture) aja_acap_stop(card);
	}
	aja_prset(card, ajareg_ch1mode, 0); /* Put ch1 back into playback mode */
	aja_prset(card, ajareg_ch2mode, 0); 
	aja_prset(card, ajareg_regclocking, 0); // Go back to field clocking.
	return;
}


static int aja_ioctl_stream_init(aja_card_t *card, const unsigned long v) {
	int 			pagect;
	aja_stream_init_t 	stinit;
	copy_from_user((void *)&stinit, (const void *)v, sizeof(stinit));

	if(stinit.chans < 1) stinit.chans = 1;
	if(stinit.chans > AJA_MAXCHANS) stinit.chans = AJA_MAXCHANS;

	pagect = stinit.pagect;
	if(pagect > AJA_MAXPAGES) pagect = AJA_MAXPAGES;

	aja_stream_clear(card);
	card->stream.flags = stinit.flags;
	card->stream.pagect = pagect;
	card->stream.chans = stinit.chans;
	card->stream.tcsource = stinit.tcsource;

	// Make sure any register changes happen right away.
	aja_prset(card, ajareg_regclocking, 2);

	/* Set the dma pointer (next) to the first frame to dma */
	if(stinit.flags & AJA_Playback) {
		aja_prset(card, ajareg_ch1mode, 0); /* Playback mode */
		aja_prset(card, ajareg_ch2mode, 0);
	} else {
		aja_prset(card, ajareg_ch1mode, 1);
		aja_prset(card, ajareg_ch2mode, 1);
	}
	
	/* If we are doing dual buffering, set the ch2 input/output to the frame after the ch1 input/output */
	aja_prset(card, ajareg_ch2disable, stinit.chans > 1 ? 0 : 1);
	return 0;
}	

static int aja_ioctl_stream_start(aja_card_t *card, const unsigned long v) {
	int 		ret;

	if(unlikely(atomic_read(&card->stream.running))) {
		perror("Stream is already running\n");
		return -EBUSY;
	}

	aja_prset(card, ajareg_regclocking, 2); // Immediate clocking.

	/* Reset the structure */
	atomic_set(&card->stream.speed, 1 * AJA_SPEED_DIVISOR);
	card->stream.spdct = 0;

	/* Wait for the start of a frame */
	ret = aja_wait_for_frame(card);
	if(signal_pending(current)) return -EINTR;
	if(!ret) {
		perror("Failed to start stream, timeout waiting for frame start\n");
		return -ETIMEDOUT;
	}
	aja_stream_start(card);
	return 0;
}

static int aja_ioctl_stream_stop(aja_card_t *card, const unsigned long v) {
	int ret;

	/* Wait for the start of a frame */
	ret = aja_wait_for_frame(card);
	if(signal_pending(current)) return -EINTR;
	if(!ret) {
		perror("Failed to start stream, timeout waiting for frame start\n");
		return -ETIMEDOUT;
	}
	aja_stream_stop(card);
	return 0;
}

static int aja_ioctl_stream_getmeta(aja_card_t *card, const unsigned long v) {
	aja_stream_meta_req_t 	req;

	if(copy_from_user((void *)&req, (const void *)v, sizeof(req))) return -EFAULT;
	if(unlikely(!IsValidPage(req.page))) {
		perror("Invalid Page: %d\n", req.page);
		return -1;
	}
	return copy_to_user((void *)req.meta, (const void *)&card->stream.meta[req.page], sizeof(aja_stream_meta_t));
}

static int aja_ioctl_stream_setmeta(aja_card_t *card, const unsigned long v) {
	aja_stream_meta_req_t 	req;

	if(copy_from_user((void *)&req, (const void *)v, sizeof(req))) return -EFAULT;
	if(unlikely(!IsValidPage(req.page))) {
		perror("Invalid Page: %d\n", req.page);
		return -1;
	}
	return copy_from_user((void *)&card->stream.meta[req.page], (const void *)req.meta, sizeof(aja_stream_meta_t));
}

static int aja_ioctl_stream_page_alloc(aja_card_t *card, const unsigned long v) {
	return aja_stream_page_alloc(card);
}

static int aja_ioctl_stream_page_free(aja_card_t *card, const unsigned long v) {
	int page;
	if(copy_from_user((void *)&page, (const void *)v, sizeof(page))) return -EFAULT;
	aja_stream_page_free(card, page);
	return 0;
}

static int aja_ioctl_stream_fifo_push(aja_card_t *card, const unsigned long v) {
	int page;
	if(copy_from_user((void *)&page, (const void *)v, sizeof(page))) return -EFAULT;
	return aja_stream_fifo_push(card, page);
}

static int aja_ioctl_stream_fifo_pop(aja_card_t *card, const unsigned long v) {
	return aja_stream_fifo_pop(card);
}

static int aja_ioctl_stream_running(aja_card_t *card, const unsigned long v) {
	return atomic_read(&card->stream.running);
}

static int aja_ioctl_stream_dropped(aja_card_t *card, const unsigned long v) {
	unsigned long 		flags;
	aja_stream_drop_t 	ret;

	spin_lock_irqsave(&card->spin_reg, flags);
	ret = card->stream.drop;
	spin_unlock_irqrestore(&card->spin_reg, flags);
	return copy_to_user((void *)v, (const void *)&ret, sizeof(ret));
}

static int aja_ioctl_stream_fifo(aja_card_t *card, const unsigned long v) {
	return atomic_read(&card->stream.fifo);
}

static int aja_ioctl_stream_setspeed(aja_card_t *card, const unsigned long v) {
	int speed;
	if(copy_from_user((void *)&speed, (const void *)v, sizeof(speed))) return -EFAULT;
	if(speed > max_play_speed) speed = max_play_speed;	// Make sure we don't go over the max speed
	atomic_set(&card->stream.speed, speed);
	return 0;
}

static int aja_ioctl_stream_getspeed(aja_card_t *card, const unsigned long v) {
	return atomic_read(&card->stream.speed);
}

static int aja_ioctl_stream_settrig(aja_card_t *card, const unsigned long v) {
	int64_t 		trig;
	if(copy_from_user((void *)&trig, (const void *)v, sizeof(trig))) return -EFAULT;
	if(card->frame.id >= trig) return -EINVAL;
	card->stream.trigger = trig;
	return 0;
}

static int aja_ioctl_aplay_position(aja_card_t *card, const unsigned long v) {
	aja_audio_position_t pos = aja_aplay_position(card);
	copy_to_user((void *)v, (const void *)&pos, sizeof(aja_audio_position_t));
	return 0;
}

static int aja_ioctl_acap_position(aja_card_t *card, const unsigned long v) {
	aja_audio_position_t pos = aja_acap_position(card);
	copy_to_user((void *)v, (const void *)&pos, sizeof(aja_audio_position_t));
	return 0;
}

static int aja_ioctl_dmabench(aja_card_t *card, const unsigned long v) {
	int engine;
	copy_from_user((void *)&engine, (const void *)v, sizeof(int));
	if(!IsValidDMA(engine)) return -EINVAL;
	engine -= AJA_DMA1;
	copy_to_user((void *)v, (const void *)&card->dma[engine].bench, sizeof(aja_dmabench_t));
	return 0;
}

static int aja_ioctl_lutload(aja_card_t *card, const unsigned long v) {
	int i;
	void *c1a, *c2a, *c3a;
	uint32_t *c1, *c2, *c3;
	aja_lut_t *lut;

	if(!(card->caps->flags & AJA_HasLUT)) return -EINVAL;
	lut = (aja_lut_t *)vmalloc(sizeof(*lut));
	if(lut == NULL) {
		perror("Failed to allocate %zd bytes for the lut\n", sizeof(*lut));
		return -ENOMEM;
	}

	copy_from_user((void *)lut, (const void *)v, sizeof(*lut));
	c1a = card->regadd + 0x800;
	c2a = card->regadd + 0x1000;
	c3a = card->regadd + 0x1800;
	c1 = lut->c1;
	c2 = lut->c2;
	c3 = lut->c3;

	for(i = 0; i < 512; i++) {
		iowrite32(*c1++, c1a);
		iowrite32(*c2++, c2a);
		iowrite32(*c3++, c3a);
		c1a += 4; c2a += 4; c3a += 4;
	}
	vfree(lut);
	return 0;
}

static int aja_ioctl_irqenable(aja_card_t *card, const unsigned long v) {
	int val;
	copy_from_user((void *)&val, (const void *)v, sizeof(val));
	aja_irqset(card, val);
	return 0;
}

static int aja_ioctl_aframe_add(aja_card_t *card, unsigned long v) {
	aja_aframe_t val;
	copy_from_user((void *)&val, (const void *)v, sizeof(val));
	atomic_add(val.frames, &card->aplay.frames);
	return 0;
}

static int aja_ioctl_aframe_get(aja_card_t *card) {
	return atomic_read(&card->aplay.frames);
}

static int aja_ioctl_getapiver(aja_card_t *card, unsigned long v) {
	int val = AJA_API_VERSION;
	copy_to_user((void *)v, (const void *)&val, sizeof(val));
	return 0;
}

int aja_ioctl(struct inode *inode, struct file *filp, unsigned int call, unsigned long val) {
	int ret = 0;
	aja_card_t *card = (aja_card_t *)filp->private_data;

	switch (call) {
		case AJACTL_BOARDINFO: 			ret = aja_ioctl_boardinfo(card, val); break;
		case AJACTL_GETAPIVER: 			ret = aja_ioctl_getapiver(card, val); break;
		case AJACTL_GETREGISTER: 		ret = aja_ioctl_getregister(card, val); break;
		case AJACTL_SETREGISTER: 		ret = aja_ioctl_setregister(card, val); break;
		case AJACTL_IRQCOUNT: 			ret = aja_ioctl_irqcount(card, val); break;
		case AJACTL_IRQSLEEP: 			ret = aja_ioctl_irqsleep(card, val); break;
		case AJACTL_IRQENABLE:			ret = aja_ioctl_irqenable(card, val); break;
		case AJACTL_DMA: 			ret = aja_ioctl_dma(card, val); break;
		case AJACTL_APLAY_START: 		ret = aja_aplay_start(card); break;
		case AJACTL_APLAY_STOP: 		ret = aja_aplay_stop(card); break;
		case AJACTL_APLAY_POSITION: 		ret = aja_ioctl_aplay_position(card, val); break;
		case AJACTL_AFRAME_ADD:			ret = aja_ioctl_aframe_add(card, val); break;
		case AJACTL_AFRAME_GET:			ret = aja_ioctl_aframe_get(card); break;
		case AJACTL_ACAP_START: 		ret = aja_acap_start(card); break;
		case AJACTL_ACAP_STOP: 			ret = aja_acap_stop(card); break;
		case AJACTL_ACAP_POSITION: 		ret = aja_ioctl_acap_position(card, val); break;
		case AJACTL_DMABENCH: 			ret = aja_ioctl_dmabench(card, val); break;
		case AJACTL_LUTLOAD:			ret = aja_ioctl_lutload(card, val); break;
		case AJACTL_FIRMWARE_LOAD:		ret = aja_ioctl_firmware_load(card, val); break;
		case AJACTL_FIRMWARE_QUERYID: 		ret = aja_ioctl_firmware_queryid(card, val); break;

		case AJACTL_FRAME_CURID: 		ret = aja_ioctl_frame_curid(card, val); break;
		case AJACTL_FRAME_CURTIME: 		ret = aja_ioctl_frame_curtime(card, val); break;
		case AJACTL_FRAME_WAITFORNEXT: 		ret = aja_ioctl_frame_waitfornext(card, val); break;

		case AJACTL_STREAM_RUNNING: 		ret = aja_ioctl_stream_running(card, val); break;
		case AJACTL_STREAM_DROPPED: 		ret = aja_ioctl_stream_dropped(card, val); break;
		case AJACTL_STREAM_INIT: 		ret = aja_ioctl_stream_init(card, val); break;
		case AJACTL_STREAM_START: 		ret = aja_ioctl_stream_start(card, val); break;
		case AJACTL_STREAM_STOP: 		ret = aja_ioctl_stream_stop(card, val); break;
		case AJACTL_STREAM_GETMETA: 		ret = aja_ioctl_stream_getmeta(card, val); break;
		case AJACTL_STREAM_SETMETA: 		ret = aja_ioctl_stream_setmeta(card, val); break;
		case AJACTL_STREAM_PAGE_ALLOC: 		ret = aja_ioctl_stream_page_alloc(card, val); break;
		case AJACTL_STREAM_PAGE_FREE: 		ret = aja_ioctl_stream_page_free(card, val); break;
		case AJACTL_STREAM_FIFO_PUSH:		ret = aja_ioctl_stream_fifo_push(card, val); break;
		case AJACTL_STREAM_FIFO_POP: 		ret = aja_ioctl_stream_fifo_pop(card, val); break;
		case AJACTL_STREAM_FIFO: 		ret = aja_ioctl_stream_fifo(card, val); break;
		case AJACTL_STREAM_SETSPEED: 		ret = aja_ioctl_stream_setspeed(card, val); break;
		case AJACTL_STREAM_GETSPEED: 		ret = aja_ioctl_stream_getspeed(card, val); break;
		case AJACTL_STREAM_SETTRIG:		ret = aja_ioctl_stream_settrig(card, val); break;

		case AJACTL_TIMECODE_GETFLAGS: 		ret = aja_ioctl_timecode_getflags(card, val); break;
		case AJACTL_TIMECODE_SETFLAGS: 		ret = aja_ioctl_timecode_setflags(card, val); break;
		case AJACTL_TIMECODE_CLRFLAGS: 		ret = aja_ioctl_timecode_clrflags(card, val); break;
		case AJACTL_TIMECODE_SET:		ret = aja_ioctl_timecode_set(card, val); break;
		case AJACTL_TIMECODE_GET: 		ret = aja_ioctl_timecode_get(card, val); break;
		case AJACTL_TIMECODE_SDI1:		ret = aja_ioctl_timecode_sdi1(card, val); break;
		case AJACTL_TIMECODE_SDI2: 		ret = aja_ioctl_timecode_sdi2(card, val); break;
		case AJACTL_TIMECODE_LTC:		ret = aja_ioctl_timecode_ltc(card, val); break;
		
		default:
			pinfo("Card %d: Unknown ioctl: 0x%X = 0x%lX\n", card->index, call, val);
			ret = ENOTTY;
			break;

	}	
	return ret;
}

static void aja_uart_enable(aja_card_t *card, int val) {
	aja_prset(card, ajareg_uartenabletx, val);
	aja_prset(card, ajareg_uartenablerx, val);
	return;
}

static int aja_uart_rx(aja_card_t *card, uint8_t *data, int max) {
	uint32_t 	val;
	int 		i, error = 0;
	
	for(i = 0; i < max; i++) {
		val = aja_prget(card, ajareg_uartstatusrx);
		if(!val) break; // Break from the loop if there is no data in the FIFO
		if(val & 0x04) {
			// Check for Parity Error and clear it
			aja_prset(card, ajareg_uartrxparityerror, 1);
			return -1;
		}
		if(val & 0x08) {
			// Check for RX buffer overrun
			aja_prset(card, ajareg_uartrxoverrun, 1);
			return -2;
		}
		val = aja_prget(card, ajareg_uartin);
		if(data) *data++ = (uint8_t)val;
	}
	return (error) ? error : i;
}

static int aja_uart_tx(aja_card_t *card, uint8_t *data, int size) {
	uint32_t val;
	int i;
	for(i = 0; i < size; i++) {
		// If the FIFO is full, bail.  We are going to have to wait until we can send more.
		if(aja_prget(card, ajareg_uartstatustx) & 0x02) break;
		val = *data++;
		aja_prset(card, ajareg_uartout, val);
	}
	return i;
}

static void aja_slavepkt_reset(aja_slavepkt_t *pkt) {
	pkt->state = AJA_SlaveRx;
	pkt->insize = pkt->outsize = pkt->inpt = pkt->outpt = 0;
	return;
}

static int aja_slavepkt_size(uint8_t *data) {
	return (data[0] & 0x0F) + 3;
}

static void aja_slavepkt_tx(aja_card_t *card) {
	aja_slavepkt_t 	*pkt = &card->slavepkt;
	int cbytes, bytes;
	
	if(pkt->state != AJA_SlaveTx) return; // Don't transmit unless we have been asked to.
	cbytes = pkt->outpt;
	bytes = aja_uart_tx(card, pkt->out + cbytes, pkt->outsize - cbytes);
	if(bytes > 0) {
		pkt->outpt += bytes;
		if(pkt->outpt >= pkt->outsize) {
			// The packet has completed, setup to Rx another.
			aja_slavepkt_reset(pkt);
		}
	}
	return;
}

static void aja_slavepkt_rx(aja_card_t *card) {
	int 		cbytes, bytes;
	aja_slavepkt_t 	*pkt = &card->slavepkt;
	
	if(!card->p2slave) return;
	if(pkt->state == AJA_SlaveTx) {
		pinfo("ERROR: UART Rx when in Tx state\n");
		return;
	}
	pkt->tstamp = card->irqcount[AJA_AudioWrap];
	cbytes = pkt->inpt;
	bytes = aja_uart_rx(card, pkt->in + cbytes, 32 - cbytes);
	if(bytes > 0) {
		// If this rx contains the first byte, set the size.
		if(!cbytes) pkt->insize = aja_slavepkt_size(pkt->in);
		pkt->inpt += bytes;
		//pinfo("%lld: uart rx %d (%d)\n", pkt->tstamp, bytes, pkt->inpt);
		if(pkt->inpt >= pkt->insize) {
			// Ok, we have a full packet.  Parse it and transmit the response.
			p2slave_handle_pkt(card->p2slave, pkt->out, pkt->in);
			pkt->outsize = aja_slavepkt_size(pkt->out);
			pkt->state = AJA_SlaveTx;
			aja_slavepkt_tx(card);
		}
	} else {
		perror("UART Rx Error: %d (buf len %d)\n", bytes, cbytes);
		aja_slavepkt_reset(pkt);
		// Ask for the UART to be reset.
		aja_uart_enable(card, 0);
		card->flags |= AJA_ResetUART;
	}
	return;
}

static int aja_stream_speedcount(aja_card_t *card) {
	int frms;
	card->stream.spdct += atomic_read(&card->stream.speed);
	frms = card->stream.spdct / AJA_SPEED_DIVISOR;
	card->stream.spdct -= (frms * AJA_SPEED_DIVISOR);
	return frms;
}

/* Frees up frames there were previously played or captured */
static void aja_stream_freeup(aja_card_t *card) {
	int 		i;
	timecode_t 	ltc, sdi1, sdi2;
	uint32_t aptr = aja_prget(card, ajareg_acaplast);

	// Read the current timecodes
	aja_timecode_get_ltc(card, &ltc);
	aja_timecode_get_sdi1(card, &sdi1);
	aja_timecode_get_sdi2(card, &sdi2);

	for(i = 0; i < 2; i++) {
		if(card->stream.last[i].type & AJA_Playback) {
			aja_stream_page_free(card, card->stream.last[i].page);
		}
		if(card->stream.last[i].type & AJA_Capture) {
			int pg = card->stream.last[i].page;
			card->stream.meta[pg].audioptr = aptr;
			card->stream.meta[pg].tc[AJA_TimecodeSDI1] = sdi1;
			card->stream.meta[pg].tc[AJA_TimecodeSDI2] = sdi2;
			card->stream.meta[pg].tc[AJA_TimecodeLTC] = ltc;
			aja_stream_fifo_push(card, pg);
		}
		card->stream.last[i].type = 0;
	}
	return;
}

static int aja_stream_playback_frame(aja_card_t *card, int chan) {
	int newp = 	aja_stream_fifo_pop(card);
	aja_register_t 	reg = chan ? ajareg_ch2output : ajareg_ch1output;
	if(newp < 0) return -ENOMEM;					// Buffer is dry
	if(card->stream.meta[newp].chan != chan) {
		perror("Page %d: expecting channel %d, got channel %d\n",
			newp, chan, card->stream.meta[newp].chan);
		return -EINVAL;	// Wrong channel
	}
	aja_prset(card, reg, newp);
	card->stream.last[chan].type = AJA_Playback;
	card->stream.last[chan].page = newp;
	return newp;
}

static void aja_stream_drop(aja_card_t *card) {
	unsigned long 		flags;
	spin_lock_irqsave(&card->spin_reg, flags);
	card->stream.drop.count++;
	card->stream.drop.id = card->frame.id;
	card->stream.drop.tc = card->tcg.value;
	spin_unlock_irqrestore(&card->spin_reg, flags);
	return;
}

static void aja_stream_playback(aja_card_t *card) {
	int 		newp = 0, ret, i, n;
	int 		chans = card->stream.chans;
	timecode_t 	tc;
	if(!(card->stream.flags & AJA_Playback)) return;

	if(atomic_read(&card->stream.fifo) < card->stream.chans) {
		aja_stream_drop(card);
		return;
	}

	// Handle any TSO bumping
	ret = aja_stream_speedcount(card);
	if(ret != 1) {
		int bumpsize = ret - 1;
		pinfo("%lld: playback bump %d\n", card->frame.id, bumpsize);
		if(!ret) return; // If we are asked to bump 0, we just skip this frame interval
		for(i = 0; i < bumpsize; i++) {
			if(atomic_read(&card->stream.fifo) < card->stream.chans) {
				aja_stream_drop(card);
				return;
			}
			for(n = 0; n < chans; n++) {
				ret = aja_stream_fifo_pop(card);
				if(ret < 0) {
					perror("%lld: Error %d: bump %d/%d, chan %d/%d\n", 
							card->frame.id, ret, i, bumpsize, n, chans);
					return;
				}
			}
		}
	}

	// Load the next frame
	for(n = 0; n < chans; n++) {
		ret = aja_stream_playback_frame(card, n);	// Load next frame
		if(ret < 0) {
			perror("%lld: Error %d: load frame, chan %d/%d\n", 
					card->frame.id, ret, n, chans);
			return;
		}
		//pinfo("C%d: %d\n", n, ret);
		if(!n) newp = ret;	// This is the master frame
	}

	// If the frame has a valid timecode in the metadata, update the tcg
	tc = card->stream.meta[newp].tc[AJA_TimecodeInternal];
	if(timecode_is_valid(&tc)) {
		card->tcg.value = tc;
		timecode_init(&card->stream.meta[newp].tc[AJA_TimecodeInternal]);
	}
	card->frame.time.priv = card->stream.meta[newp].priv;

	// Zero out the frame metadata so we don't accidently use it again
	memset(&card->stream.meta[newp], 0, sizeof(card->stream.meta[newp]));
	return;
}

static int aja_stream_capture_frame(aja_card_t *card, int chan) {
	int newp = 	aja_stream_page_alloc(card);
	if(newp < 0) return -ENOMEM;				// No more buffers
	aja_prset(card, chan ? ajareg_ch2input : ajareg_ch1input, newp);
	card->stream.last[chan].type = AJA_Capture;
	card->stream.last[chan].page = newp;
	return newp;
}

static void aja_stream_capture(aja_card_t *card) {
	int 		n, ret;
	int 		chans = card->stream.chans;
	int 		newp = 0;
	int 		inc;
	if(!(card->stream.flags & AJA_Capture)) return;

	if(card->stream.pagect - atomic_read(&card->stream.fifo) <= chans) {
		aja_stream_drop(card);
		return;
	}

	// Figure out how many frames to bump.
	inc = aja_stream_speedcount(card);
	if(inc == 0) {
		timecode_dec((timecode_t *)&card->tcg.value);
	} else if(inc > 0) {
		for(n = 0; n < inc - 1; n++) timecode_inc((timecode_t *)&card->tcg.value);
	}

	// Load the next frame
	for(n = 0; n < chans; n++) {
		ret = aja_stream_capture_frame(card, n);	// Load next frame
		if(ret < 0) {
			perror("%lld: Error %d: capture frame, chan %d/%d, fifo %d\n",
				card->frame.id, ret, n, chans, atomic_read(&card->stream.fifo));
			return;
		}
		memset(&card->stream.meta[ret], 0, sizeof(card->stream.meta[ret]));
		card->stream.meta[ret].timing = card->frame.time;
		card->stream.meta[ret].chan = n;
		card->stream.meta[ret].inc = inc;
		card->stream.meta[ret].priv = card->frame.time.priv;
		card->stream.meta[ret].tc[AJA_TimecodeInternal] = card->tcg.value;
	
		//pinfo("C%d: %d\n", n, ret);
		if(!n) newp = ret;	// This is the master frame
	}

	return;
}

static void aja_handle_frame(aja_card_t *card, uint32_t timer) {
	uint32_t line = aja_prget(card, ajareg_outputline);
	uint32_t field = aja_prget(card, ajareg_outputfield);
	int64_t id, trig = card->stream.trigger;

	/* If this is the start of a new frame, update the frame count */
	if(!field) {
		int ltcsync = 1;
		int srun = atomic_read(&card->stream.running);
		timecode_t curtc, ntc;

		// Increment the counters
		card->frame.id++;
		id = card->frame.id;
		if(atomic_read(&card->tcg.flags) & AJA_TCG_Running) {
			timecode_inc((timecode_t *)&card->tcg.value);
			timecode_clear_flag((timecode_t *)&card->tcg.value, TimecodeField2);
		}

		// Check for a trigger
		if(trig && trig == id) aja_stream_start(card);

		// Update the current frame info
		card->frame.time.id = id;
		card->frame.time.line = line;
		card->frame.time.timer = timer;
		do_gettimeofday(&card->frame.time.tstamp);
		
		// Free up the previous frame
		aja_stream_freeup(card);

		// Handle the streams
		if(srun) {
			if(card->stream.atrig) {
				if(card->stream.flags & AJA_Capture) aja_acap_start(card);
				if(card->stream.flags & AJA_Playback) aja_aplay_start(card);
				card->stream.atrig = 0;
			}
			aja_stream_playback(card);
			aja_stream_capture(card);
		}

		// The current timecode isn't valid until this point as it may be altered by the
		// stream capture/playback
		ntc = curtc = card->tcg.value;
		if(srun) timecode_inc(&ntc);
		card->frame.time.tcg = curtc;

		// Since 60/50 fps timecode doesn't exist over the wire, we need
		// to convert it to 30/25 fps timecode.  We then mark the odd frames
		// with a field 2 marker.  Also, in a 60/50 mode, the LTC board needs
		// sync at half the actual timecode rate.  So we sync on the even fields.
		if(curtc.fps > 30) ltcsync = !(curtc.frame % 2);
		timecode_makevalidwire(&curtc);
		timecode_makevalidwire(&ntc);

		// Update the output timecode
		aja_timecode_set_sdi1(card, &ntc);
		aja_timecode_set_sdi2(card, &ntc);

		// Sync the LTC board
		if(card->pcitc && ltcsync) {
			aja_timecode_set_ltc(card, &curtc);
			pcitc.sync(card->pcitc, 1); 
			pcitc.sync(card->pcitc, 0);
		}

		// Update the P2 slave device
		if(card->p2slave) { 
			p2slave_set_tc(card->p2slave, &curtc);
		}

		// Wake up anyone waiting on a new frame
		wake_up_all(&card->frame.wait);

	} else {
		if(atomic_read(&card->tcg.flags) & AJA_TCG_Running) {
			timecode_set_flag((timecode_t *)&card->tcg.value, TimecodeField2);
		}
	}


	/* Check the APLAY frame */
	if(!field && atomic_read(&card->aplay.frames)) {
		if(atomic_read(&card->aplay.running)) {
			if(atomic_dec_and_test(&card->aplay.frames)) {
				atomic_set(&card->aplay.running, 0);
				aja_aplay_stop(card);
			}
		} else {
			atomic_set(&card->aplay.running, 1);
			aja_aplay_start(card);
		}
	}

	return;
}

/* This function is called when ever the kernel detects that there was an interrupt on our irq line */
#if LINUX_VERSION_CODE > KERNEL_VERSION(2,6,18)
irqreturn_t aja_irq_service(int irq, void *dev_id) {
#else
irqreturn_t aja_irq_service(int irq, void *dev_id, struct pt_regs *regs) {
#endif
	aja_card_t *card = (aja_card_t *)dev_id;
	int handled = 0;
	/*
	unsigned long long st, et;
	rdtscll(st);
	*/
	
	/* Now we need to determine if the card is the source of the interrupt.  If so handle it, if not ignore it */
	/* Bus Error IRQ */
	if (unlikely(aja_prget(card, ajareg_buserrorirq))) {
		card->irqcount[AJA_BusError]++;
		aja_prset(card, ajareg_buserrorirqclear, 1);
		handled = 1;
		perror("Card %d: Bus Error!", card->index);
		wake_up_all(&card->irqwait[AJA_BusError]);
	}
	
	/* DMA4 IRQ */
	if (aja_prget(card, ajareg_dma4irq)) {
		card->irqcount[AJA_DMA4]++;
		aja_prset(card, ajareg_dma4irqclear, 1);
		handled = 1;
		wake_up_all(&card->irqwait[AJA_DMA4]);
	}
	
	/* DMA3 IRQ */
	if (aja_prget(card, ajareg_dma3irq)) {
		card->irqcount[AJA_DMA3]++;
		aja_prset(card, ajareg_dma3irqclear, 1);
		handled = 1;
		wake_up_all(&card->irqwait[AJA_DMA3]);
	}
	
	/* DMA2 IRQ */
	if (aja_prget(card, ajareg_dma2irq)) {
		card->irqcount[AJA_DMA2]++;
		aja_prset(card, ajareg_dma2irqclear, 1);
		handled = 1;
		wake_up_all(&card->irqwait[AJA_DMA2]);
	}
	
	/* DMA1 IRQ */
	if (aja_prget(card, ajareg_dma1irq)) {
		card->irqcount[AJA_DMA1]++;
		aja_prset(card, ajareg_dma1irqclear, 1);
		handled = 1;
		wake_up_all(&card->irqwait[AJA_DMA1]);	
	}

	/* We can only trust the status register if the VIV (dma reg, bit 26) bit is high  */
	if (aja_prget(card, ajareg_viv)) {
		uint32_t timer = aja_prget(card, ajareg_audiocount);

		/* Output Vertical IRQ */
		if (aja_prget(card, ajareg_outputirq)) {
			card->irqcount[AJA_Output]++;
			aja_prset(card, ajareg_outputirqclear, 1);
			handled = 1;
			aja_handle_frame(card, timer);
			wake_up_all(&card->irqwait[AJA_Output]);
		}

		/* Check for all possible interrupt types, if we find one handle it!!! */
		/* Input 1 Vertical IRQ */
		if (aja_prget(card, ajareg_input1irq)) {
			card->irqcount[AJA_Input1]++;
			aja_prset(card, ajareg_input1irqclear, 1);
			handled = 1;
			wake_up_all(&card->irqwait[AJA_Input1]);
		}
	
		/* Input 2 Vertical IRQ */		
		if (aja_prget(card, ajareg_input2irq)) {
			card->irqcount[AJA_Input2]++;
			aja_prset(card, ajareg_input2irqclear, 1);
			handled = 1;
			wake_up_all(&card->irqwait[AJA_Input2]);
		}
			
	
		/* Audio Wrap */
		if (aja_prget(card, ajareg_audiowrapirq)) {
			card->irqcount[AJA_AudioWrap]++;
			aja_prset(card, ajareg_audiowrapirqclear, 1);
			
			/* Check for a UART Rx timeout */
			if(card->slavepkt.inpt && (card->slavepkt.tstamp + 2) < card->irqcount[AJA_AudioWrap]) {
				perror("UART Rx timeout w/ %d bytes\n", card->slavepkt.inpt);
				aja_slavepkt_reset(&card->slavepkt);
				aja_uart_enable(card, 0);
				card->flags |= AJA_ResetUART;
			}

			/* Housekeeping functions */
			if(card->flags & AJA_ResetUART) {
				perror("UART reset\n");
				card->flags &= (~AJA_ResetUART);
				aja_uart_enable(card, 1);
			}
			
			wake_up_all(&card->irqwait[AJA_AudioWrap]);
			handled = 1;
		}

		/* UART Tx */
		if (aja_prget(card, ajareg_uarttxirq)) {
			card->irqcount[AJA_UART_TX]++;
			aja_slavepkt_tx(card);
			aja_prset(card, ajareg_uarttxirqclear, 1);
			wake_up_all(&card->irqwait[AJA_UART_TX]);
			handled = 1;
		}

		/* UART Rx */
		if (aja_prget(card, ajareg_uartrxirq)) {
			card->irqcount[AJA_UART_RX]++;
			aja_slavepkt_rx(card);
			aja_prset(card, ajareg_uartrxirqclear, 1);
			wake_up_all(&card->irqwait[AJA_UART_RX]);
			handled = 1;
		}

		/* Handle possible legacy interrupts - we don't do anything with these */
		if(aja_prget(card, ajareg_audioirq)) {
			aja_prset(card, ajareg_audioirqclear, 1);
			handled = 1;
		}

		if(aja_prget(card, ajareg_aplaywrapirq)) {
			aja_prset(card, ajareg_aplaywrapirqclear, 1);
			handled = 1;
		}

		if(aja_prget(card, ajareg_acapwrapirq)) {
			aja_prset(card, ajareg_acapwrapirqclear, 1);
			handled = 1;
		}
	
	}

	/*
	rdtscll(et);
	pinfo("IRQ took %llu cycles\n", et - st);
	*/
	if (handled) return IRQ_HANDLED;
	return IRQ_NONE;
}

/* Function to open device */
static int aja_open(struct inode *minode, struct file *filp) {
	aja_card_t *card = container_of(minode->i_cdev, aja_card_t, cdev);
	filp->private_data = card;
	pdebug("called\n");
	aja_irqset(card, 1);
	return 0;
}

/* Gets called when the device is released */
static int aja_release(struct inode *minode, struct file *file) {
	aja_card_t *card = container_of(minode->i_cdev, aja_card_t, cdev);
	if(atomic_read(&card->stream.running) && current->pid == card->stream.pid) {
		aja_stream_stop(card);
		pinfo("Stream stopped- process %d(%s) that owned running stream has released it.\n",
			current->pid, current->comm);
	}
	return 0;
}

/* maps a PCI buffer */
static int aja_mmap(struct file *filp, struct vm_area_struct *vma) {
	aja_card_t 	*card = (aja_card_t *)filp->private_data;
	struct pci_dev 	*pcidev = card->pcidev;
	unsigned long 	reglen = 0;
	unsigned long 	regstart = 0;
	unsigned long 	size = vma->vm_end - vma->vm_start;

	// WTF: Not sure why, but the aja card bus resources are 0, 2, and 4.
	// I'm pretty sure that wasn't the case a while back.
	switch(vma->vm_pgoff) {
		case 0:	reglen = pci_resource_len(pcidev, 0);
			regstart = pci_resource_start(pcidev, 0);
			break;

		case 1:	reglen = pci_resource_len(pcidev, 2);
			regstart = pci_resource_start(pcidev, 2);
			break;

		case 2:	reglen = pci_resource_len(pcidev, 4);
			regstart = pci_resource_start(pcidev, 4);
			break;

		default:
			return -ENODEV;
	}
	if(!reglen || !regstart) return -ENODEV;
	vma->vm_flags |= VM_RESERVED;
	pinfo("Mapping resource %ld - 0x%lX @ 0x%lX\n", vma->vm_pgoff, reglen, regstart);
	if(remap_pfn_range(vma, vma->vm_start, regstart >> PAGE_SHIFT, size, vma->vm_page_prot)) return -EAGAIN;

	return 0;
}

static struct file_operations aja_fops = { 
	.owner =		THIS_MODULE,
	.ioctl = 		aja_ioctl,
	.open = 		aja_open,
	.release = 		aja_release,
	.mmap = 		aja_mmap
};

static int aja_dma64_supported(void *regadd, uint32_t cardid, uint32_t pcifw) {
	/* Is this even a 64bit machine? */
	if(!use_dma64 || sizeof(void *) < 8) return 0;

	switch(cardid) {
		/* The kona 3 supports 64bit DMA, if the PCI firmware
		 * is >= 0x70 */
		case AJA_KONA3: 
			if(pcifw >= 0x70) return 1;
	}
	return 0;
}

/* This funciton creates all the device handles for the card.  This consists of a udev handle and a
   character device handle */
static void aja_device_create(aja_card_t *card) {
	int err;

	card->dev = DEVICE_CREATE(aja_class, NULL, card->devt, card->name);
	if(card->dev == NULL) {
		perror("Failed to create device for card %d\n", card->index);
		return;
	}
	DEVICE_SET_DATA(card->dev, card);

	cdev_init(&card->cdev, &aja_fops);
	card->cdev.owner = THIS_MODULE;
	err = cdev_add(&card->cdev, card->devt, 1);
	if(err) {
		perror("Failed to add character device for card %d: Error %d\n", card->index, err);
	}
	return;
}

static void aja_device_destroy(aja_card_t *card) {
	if(card->dev != NULL) {
		DEVICE_DESTROY(aja_class, card->devt);
		card->dev = NULL;
	}
	cdev_del(&card->cdev);
	return;
}

/* This function is called by the Linux PCI subsystem when a device is found
 * that matches our given criteria.  We are responsible for intializing the
 * device here */
static int aja_pci_probe(struct pci_dev *dev, const struct pci_device_id *id) {
	
	int error, dma64 = 0;
	unsigned long reglen;
	unsigned long regstart;
	void *regadd;
	uint32_t val, boardid, pcifw;
	aja_card_t *card = NULL;
	const char *pciname = pci_name(dev);

	pinfo("Found board %d @ %s\n", aja_cards, pciname);
	
	/* Enable the card on the PCI bus */
	pdebug("%s: Enable device\n", pciname);
	error = pci_enable_device(dev);
	if(error) {
		perror("%s: unable to enable card. [error %d]\n", pciname, error);
		return error;
	}

	/* Request the register memory region */
	pdebug("%s: requesting register space\n", pciname);
	reglen = pci_resource_len(dev, 0);
	regstart = pci_resource_start(dev, 0);

	if (!request_mem_region(regstart, reglen, MODNAME)) {
		perror("%s: request_region failed!\n", pciname);
		error = -ENOMEM;
		goto err0;
	}

	/* Map the register space and get the board ID */
	pdebug("%s: mapping the register space\n", pciname);
	regadd = ioremap_nocache(regstart, reglen);
	if(regadd == 0) {
		perror("%s: unable to remap the card register space!\n", pciname);
		error = -ENOMEM;
		goto err1;
	}
	pinfo("%s: reg space 0x%lX remapped to 0x%p\n", pciname, regstart, regadd);
	
	/* Read the card ID */
	pdebug("%s: Setting up card structure\n", pciname);
	if(reglen < 0xC8) {
		perror("%s: register address space is not large enough to read the card ID!\n", pciname);
		error = -ENOMEM;
		goto err2;
	}
	pcifw = (ioread32(regadd + 0xC0) >> 8) & 0xFF;
	boardid = ioread32(regadd + 0xC8);

	/* Setup the DMA support */
	dma64 = aja_dma64_supported(regadd, boardid, pcifw);
	error = pci_set_dma_mask(dev, dma64 ? 0xFFFFFFFFFFFFFFFC : 0xFFFFFFFC);
	if(error) {
		perror("%s: Failed to setup %dbit DMA mask. [error %d]\n", pciname, dma64 ? 64 : 32, error);
	} else {
		pinfo("%s: %dbit DMA support enabled\n", pciname, dma64 ? 64 : 32);
	}
	pci_set_master(dev);

	pdebug("%s: creating card structure\n", pciname);
	card = aja_card_create(dev, boardid, aja_cards);
	if (card == NULL) {
		perror("%s: couldn't allocate a cardinfo structure!\n", pciname);
		error = -ENOMEM;
		goto err2;
	}
	
	/* And set the PCI private data to point to this structure */
	if(dma64) card->flags |= AJA_DMA64;
	card->reglen = reglen;
	card->regadd = regadd;
	
	pinfo("%s: %s [0x%X], PCIFW 0x%X, IRQ %d\n", 
		pciname, card->caps->name, card->caps->id, 
		pcifw, dev->irq);
	
	/* Request the card IRQ from the kernel */
	pdebug("%s: requesting IRQ\n", pciname);
	error = request_irq(dev->irq, aja_irq_service, IRQF_SHARED, MODNAME, card);
	if(error) {
		perror("%s: couldn't allocate the card IRQ. [error %d]\n", pciname, error);
		goto err3;
	}

	/* Add the class devices */
	aja_device_create(card);
	
	/* Check to see if we need to force the card to do 64bit xfers */
	if(force64) {
		val = ioread32(regadd + 48 * 4);
		if(!(val & 0x20)) {
			pinfo("%s: reports 32bit mode, but forcing to 64bit\n", pciname);
			val |= 0x10;
			iowrite32(val, regadd + 48 * 4);
		}
	}

	/* FIXME: This is a hack to set the LTC I/O for this card. */
	if(pcitc.device_find) {
		card->pcitc = pcitc.device_find(aja_cards);
		if(card->pcitc) pinfo("%s: connected to LTC I/O %p\n", pciname, card->pcitc);
	}

	/* Allocate a p2slave device for the card */
	card->p2slave = p2slave_register(card->name);
	if(card->p2slave == NULL) pinfo("Failed to register a p2slave device for %s\n", card->name);

	/* Ok, now increment the cardcount */
	aja_cards++;
	return 0;

err3:
	aja_card_free(card);

err2:
	iounmap((void *)regadd);

err1:
	release_mem_region(dev->resource[0].start, reglen);

err0:
	pci_set_drvdata(dev, NULL);
	pci_disable_device(dev);
	return error;

}

/* This function gets called when the driver is unloading.  We must release our
 * useage of the device here */
static void aja_pci_remove(struct pci_dev *dev) {
	const char *pciname = pci_name(dev);
	aja_card_t *card = (aja_card_t *)pci_get_drvdata(dev);
	if (card == NULL) return;

	pinfo("%s: removing card\n", pciname);

	aja_irqset(card, 0);			/* Turn off the interrupts for this card */
	p2slave_unregister(card->p2slave);	/* Unregister the p2slave device */
	aja_device_destroy(card);		/* remove the device from the class */
	
	/* Remove the IRQ */
	if (card->pcidev->irq) free_irq(card->pcidev->irq, card);
	
	/* Unmap and free the register space */
	iounmap((void *)card->regadd);
	release_mem_region(dev->resource[0].start, card->reglen);

	/* Disable the PCI device */
	pci_set_drvdata(dev, NULL);
	pci_disable_device(dev);
	
	/* Free the memory being used by the cardinfo structure */
	aja_card_free(card);
	return;
}


/* This is a list of all the PCI devices we support */
static struct pci_device_id aja_pci_device_ids[] = {
	{0xF1D0, 0xC0FE, PCI_ANY_ID, PCI_ANY_ID, 0, 0, 0},
	{0xF1D0, 0xC0FF, PCI_ANY_ID, PCI_ANY_ID, 0, 0, 0},
	{0xF1D0, 0xCFEE, PCI_ANY_ID, PCI_ANY_ID, 0, 0, 0},
	{0xF1D0, 0xDFEE, PCI_ANY_ID, PCI_ANY_ID, 0, 0, 0},
	{0xF1D0, 0xEFAC, PCI_ANY_ID, PCI_ANY_ID, 0, 0, 0},
	{0xF1D0, 0xFACD, PCI_ANY_ID, PCI_ANY_ID, 0, 0, 0},
	{0xF1D0, 0xCAFE, PCI_ANY_ID, PCI_ANY_ID, 0, 0, 0},
	{0xF1D0, 0xDCAF, PCI_ANY_ID, PCI_ANY_ID, 0, 0, 0},
	{0,      0,      0,          0,          0, 0, 0}
};

static struct pci_driver aja_pci_driver = {
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,15)
	.owner = 	THIS_MODULE,
#endif
	.name = 	MODNAME,
	.id_table =	aja_pci_device_ids,
	.probe = 	aja_pci_probe,
	.remove =	aja_pci_remove
	
};

static void aja_cleanup(void) {
	/* Unregister the driver attributes */
	driver_remove_file(&aja_pci_driver.driver, &driver_attr_cards);
	pci_unregister_driver(&aja_pci_driver);

	if(aja_chrdev) {
		unregister_chrdev_region(aja_chrdev, 256);
		aja_chrdev = 0;
	}

	/* Unregister the class */
	if(aja_class != NULL) {
		class_destroy(aja_class);
		aja_class = NULL;
	}

	/* Remove links to ltc functions, if any */
	if(ltcio_register) {
		symbol_put(pcitc_register);
	}
	return;
}	

/* This function is responsible for loading the driver */
static int __init aja_init(void) {
	int error = 0;
	aja_cards = 0;

	/* introduce ourself to the kernel log */
	pinfo("Driver for the AJA line of PCI video I/O cards. API version: %d\n", AJA_API_VERSION);
	pinfo("Copyright 2000-2008, SpectSoft <http://www.spectsoft.com>\n");

	/* Register the ajacards class */
	pdebug("Registering class\n");
	aja_class = class_create(THIS_MODULE, "ajacards");
	if(unlikely(aja_class == NULL)) {
		perror("Unable to create aja class\n");
		goto initbail;
	}
	
	/* Since we aren't given a static device major by the kernel development, we must request one at run time.
	   This isn't a big deal, as the mapping to device is handled automagicly by udev */
	pdebug("Getting char major\n");
	error = alloc_chrdev_region(&aja_chrdev, 0, 255, MODNAME);
	if (unlikely(error)) {
		perror("Couldn't register as a character device!  Error: %d\n", error);
		goto initbail;
	}
	pdebug("Registered as char device %d\n", MAJOR(aja_chrdev));
	
	/* Check to see if the PCI-TC board is installed */
	memset(&pcitc, 0, sizeof(pcitc));
	ltcio_register = symbol_get(pcitc_register);
	if(ltcio_register != NULL) {
		pinfo("Connected to PCITC module.\n");
		ltcio_register(&pcitc);
	}

	/* initialize the cards */
	pdebug("Initializing cards\n");

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,22)
	error = pci_module_init(&aja_pci_driver);
#else
	error = pci_register_driver(&aja_pci_driver);
#endif
	
	if(error) {
		perror("Error initializing devices: %d\n", error);
		goto initbail;
	}

	/* Make sure we have at least one card */
	if(!aja_cards) {
		error = -ENODEV;
		goto initbail;
	}

	/* Register the driver attributes */
	driver_create_file(&aja_pci_driver.driver, &driver_attr_cards);

	pdebug("Module Loaded.\n");
	return 0;

initbail:
	aja_cleanup();
	return error;
}

/* This function is responsible for unloading the driver */
static void __exit aja_exit(void) {
	aja_cleanup();
	return;
}


/* Declare the module initialization and exit points */
module_init(aja_init);
module_exit(aja_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Jason Howard <jason@spectsoft.com>");
MODULE_DESCRIPTION("Linux 2.6 driver for the AJA line of professional video I/O PCI cards");




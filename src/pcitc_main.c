/*******************************************************************************
 * pcitc.c
 *
 * Creation Date: January 01, 2006
 * Author(s): 
 *   Jason Howard <jason@spectsoft.com>
 *
 * 
 * Copyright (c) 2006, SpectSoft
 *   All Rights Reserved.
 *   http://www.spectsoft.com/
 *   info@spectsoft.com
 * 
 * $Id$
 * 
 * DESC: Linux 2.6 driver for the PCI-TC boards
 * 
 ******************************************************************************/

#include <linux/version.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <asm/uaccess.h>
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
#include <asm/scatterlist.h>
#include <linux/wait.h>
#include <linux/firmware.h>

#include <tcio.h>
#include "utils.h"

#define PCITC_API_VERSION	20100420
#define MBOX_DELAY 		5000
#define WRITE_WAIT_MAX		4000 	/* 1 ms */
#define READ_TRIES		15

#define MODNAME "pcitc"

#define PCITC_VENDOR_ID 	0xAECB
#define PCITC_PCI_ID 		0x6250
#define PCITC_PCIE_ID 		0x6900

struct pcitc_t;

struct pcitc_dops_t {
	int 			(*init)(struct pcitc_t *card);
	int 			(*cleanup)(struct pcitc_t *card);
	int 			(*write8)(struct pcitc_t *card, unsigned long off, uint8_t val);
	int 			(*write32)(struct pcitc_t *card, unsigned long off, uint32_t val);
	uint8_t 		(*read8)(struct pcitc_t *card, unsigned long off, int *status);
	uint32_t 		(*read32)(struct pcitc_t *card, unsigned long off, int *status);
};

/* Internal card description structure */
struct pcitc_t {
	int 			id;			/* Card number in the system */
	char 			name[TCIO_NAME_MAX];	/* Device name */
	char 			info[64];		/* Device info */
	char 			serial[32];		/* Firmware serial number */
	int 			deviceid;		/* PCI device ID of the card */
	void 			*reg; 			/* Mapped register space */
	size_t			reglen; 		/* Length of mapped register space */
	struct tcio_t		*tcio;			/* Timecode I/O device handle */
	struct pci_dev 		*pcidev; 		/* PCI device */
	struct pcitc_dops_t 	*ops;			/* Device low level operations structure */
	int 			cfps;			/* Current Frame Rate */
};


/* Driver private data */
static int 		cardct = 0;			/* Number of detected cards */

/* These are the four functions to read/write to a PCI based PCI-TC card */
static int pcitc_pci_write8(struct pcitc_t *card, unsigned long off, uint8_t val) {
	iowrite8(val, card->reg + off);
	return 0;
}

static int pcitc_pci_write32(struct pcitc_t *card, unsigned long off, uint32_t val) {
	iowrite32(val, card->reg + off);
	return 0;
}

static uint8_t pcitc_pci_read8(struct pcitc_t *card, unsigned long off, int *status) {
	if(status != NULL) *status = 0;
	return ioread8(card->reg + off);
}

static uint32_t pcitc_pci_read32(struct pcitc_t *card, unsigned long off, int *status) {
	if(status != NULL) *status = 0;
	return ioread32(card->reg + off);
}

static int pcitc_pci_init(struct pcitc_t *card) {
	struct pci_dev 	*dev = card->pcidev;
	
	card->reglen = dev->resource[0].end - dev->resource[0].start;
	if(!request_region(dev->resource[0].start, card->reglen, MODNAME)) {
		perror("Card %d: Unable to request register memory region %s\n", card->id, 
				dev->resource[0].name);
		return -ENOMEM;
	}

	card->reg = ioport_map(dev->resource[0].start, card->reglen);
	if(card->reg == NULL) {
		perror("Card %d: Unable to map register space\n", card->id);
		return -ENOMEM;
	}
	return 0;
};

static int pcitc_pci_cleanup(struct pcitc_t *card) {
	if(card->reg) {
		ioport_unmap(card->reg);
		release_region(card->pcidev->resource[0].start, card->reglen);
		card->reg = NULL;
	}
	return 0;
}

static int pcitc_sdrw(struct pcitc_t *card, unsigned long sdcmd, unsigned long port, unsigned long data) {
	uint8_t 	b1;
	uint16_t 	b2;
	unsigned long 	mixcode, temp;
	int 		i;

	temp = (data & 0x000000FF);
	mixcode = ((port & 0x000000FF) * 0x10000);
	mixcode = mixcode + temp + sdcmd;

	//Error/Normal Interrupt Status Enable settings
	iowrite32(0x000F0101, card->reg + 0x34);

	/*Check "RESET" is Clear */
	b1 = ioread8(card->reg + 0x24);
	if(b1 & 0x01) { /* need to reset, CMD not ready */
		iowrite8(0x02, card->reg + 0x2F); //Reset CMD line only
		for(i = 100000; i > 0; i--) {
			b1 = ioread8(card->reg + 0x2F);
			if((b1 & 0x02) == 0) break;
			udelay(10);
		}
		if(!i) return -ETIMEDOUT;
	}

	iowrite32(0x000F0001, card->reg + 0x30);
	/* byte3 is CTL: 0x84=W1, 0x04=R1, 0x06=R4 (only three options)
	 * byte2 is OFFSET (address of "time code" card register)
	 * byte1 is currently reserved
	 * byte0 is the "data" for W1 operation
	 */

	iowrite32(mixcode, card->reg + 0x08);
	iowrite8(0x1A, card->reg + 0x0E);
	iowrite8(0x34, card->reg + 0x0F); /* CMD52 START */

	for(i = 200000; i > 0; i--) {
		b2 = ioread16(card->reg + 0x30); /* Normal Interrupt Status */
		if(b2 & 0x8001) break;
		udelay(10);
	}
	if(!i) return -ETIMEDOUT;
	if(b2 & 0x8000) return -EBUSY;
	return (b2 & 0x0001) ? 0 : -ENOMEM;
}

static int pcitc_pcie_write8(struct pcitc_t *card, unsigned long off, uint8_t val) {
	return pcitc_sdrw(card, 0x84000000, off, val);
}

static int pcitc_pcie_write32(struct pcitc_t *card, unsigned long off, uint32_t val) {
	int i, ret;
	for(i = 0; i < 4; i++) {
		ret = pcitc_sdrw(card, 0x84000000, off, val);
		if(ret) return ret;
		off++;
		val = val >> 8;
	}
	return 0;
}

static uint8_t pcitc_pcie_read8(struct pcitc_t *card, unsigned long off, int *status) {
        int 	ret;
	/* align the "dwCode" for this operation!
         * byte3 is CTL: 0x84=W1, 0x04=R1, 0x06=R4 (only three options)
         * byte2 is OFFSET (address of "time code" card register)
         * byte1 is currently reserved
         * byte0 is the "data" for W1 operation
	 */
        ret = pcitc_sdrw(card, 0x04000000, off, 0x00000000);
	if(ret) {
		if(status != NULL) *status = ret;
		return 0;
	}
	
	if(status != NULL) *status = 0;
	return ioread8(card->reg + 0x10);
}

static uint32_t pcitc_pcie_read32(struct pcitc_t *card, unsigned long off, int *status) {
	int ret = pcitc_sdrw(card, 0x06000000, off, 0x00000000);
	if(ret) {
		if(status != NULL) *status = ret;
		return 0;
	}

	if(status != NULL) *status = 0;
	return ioread32(card->reg + 0x10);
}

static int pcitc_pcie_init(struct pcitc_t *card) {
	struct pci_dev 		*dev = card->pcidev;
	int 			i;
	uint8_t 		b1, data;

	card->reglen = dev->resource[0].end - dev->resource[0].start;
	if (!request_mem_region(dev->resource[0].start, card->reglen, MODNAME)) {
		perror("Card %d: request_region failed!\n", card->id);
		return -ENOMEM;
	}

	card->reg = ioremap_nocache(dev->resource[0].start, card->reglen);
	if(card->reg == NULL) {
		perror("Card %d: Unable to remap register space\n", card->id);
		return -ENOMEM;
	}

	/* Do a JMB reset operation (Write/Strobe the reset ALL bit) */
	iowrite8(0x01, card->reg + 0x2F);

	for(i = 1000; i > 0; i--) {
		b1 = ioread8(card->reg + 0x2F);
		if(!(b1 & 0x01)) break;
		udelay(1000);
	}
	if(!i) {
		perror("Card %d: Timed out waiting for reset\n", card->id);
		return -ENOMEM;
	}

	/* Bit0 and Bit1 both need to be '0' to show 'insert' & 'stable' */
	b1 = ioread8(card->reg + 0x26);
	//if(b1 & 0x03) {
	//	perror("Card %d: insert and stable failed: 0x%0X\n", card->id, b1);
	//	return -ENOMEM;
	//}
	
	iowrite8(0x0E, card->reg + 0x29); /* W1, 3.3v power */
	iowrite8(0x0F, card->reg + 0x29); /* W1, Turn on SD power */
	iowrite8(0x00, card->reg + 0x28); /* W1, Init host controller */
	b1 = ioread8(card->reg + 0x41);
	iowrite8(0x01, card->reg + 0x2C); /* W1, En int. Clk/Dis SDCLK */

	b1 = (b1 & 0x3F);
	if (b1 > 50)		data = 0x02;    /* div/4 */
	else if (b1 > 25)	data = 0x01;    /* div/2 */
	else 			data = 0x00;    /* div/1 */

	iowrite8(data, card->reg + 0x2D); /* Set divisor value */
	iowrite8(0x05, card->reg + 0x2C); /* W1, Enable int. CLK & SDCLK */
	
	/* Now loop until the card indicates that it is 'stable' */
	for(i = 1000; i > 0; i--) {
		b1 = ioread8(card->reg + 0x2C);
		if(b1 & 0x02) break;
		udelay(1000);
	}
	if(!i) {
		perror("Card %d: timed out waiting for stable\n", card->id);
		return -ENOMEM;
	}
	b1 = ioread8(card->reg + 0x2C);

	/* Write is enabled? */
	b1 = ioread8(card->reg + 0x26);
	if(!(b1 & 0x04)) perror("Card %d: controller is write protected!\n", card->id);

	/* Intial tests show that this status does NOT matter in this PCIe-TC instance
	 * TODO: What to do when 'bit3' == '0'????
	 * 
	 * Error/Normal Interrupt SIGNAL Enables 3Bh-38h
	 * iowrite32(0x00000100, card->reg + 0x38); // W4, ULONG data
	 * The 0x00000000 will Disable ALL hardware interrupts
	 * The 0x00000100 will Enable ONLY CARD hardware interrupts, not CMD Complete/Err.
	 */

	/* Error/Normal Interrupt Status Enable settings */
	iowrite32(0x000F0101, card->reg + 0x34);
	return 0;
}

static int pcitc_pcie_cleanup(struct pcitc_t *card) {
	if(card->reg) {
		iounmap(card->reg);
		release_mem_region(card->pcidev->resource[0].start, card->reglen);
		card->reg = NULL;
	}
	return 0;
}

static inline int pcitc_mailbox_send(struct pcitc_t *card, uint8_t cmd) {
	int 			i;
	uint8_t 		status, ret;
	struct pcitc_dops_t 	*ops = card->ops;

	/* Wait for bit 2 of the status register to go low */
	i = 0;
	status = ops->read8(card, 0xFE, NULL);
	while(status & 0x04) {
		udelay(1);
		i++; if(i > MBOX_DELAY) return -1;
		status = ops->read8(card, 0xFE, NULL);
	}
	
	/* Write the command to the mailbox */
	ops->write8(card, 0x2F, cmd);
	
	/* Wait for bit 0 of the status reg to go high */
	i = 0;
	status = ops->read8(card, 0xFE, NULL);
	while(!(status & 0x01)) {
		udelay(1);
		i++; if(i > MBOX_DELAY) return -2;
		status = ops->read8(card, 0xFE, NULL);
	}
	
	/* Read the return code */
	ret = ops->read8(card, 0x0F, NULL);

	/* Wait for bit 2 of the status register to go low */
	i = 0;
	status = ops->read8(card, 0xFE, NULL);
	while(status & 0x04) {
		udelay(1);
		i++; if(i > MBOX_DELAY) return -3;
		status = ops->read8(card, 0xFE, NULL);
	}

	/* Tell card to handle next command */
	ops->write8(card, 0x2F, 0x00);
	
	return ret;
}

static void pcitc_getcardinfo(struct pcitc_t *card) {
	struct pcitc_dops_t 	*ops = card->ops;
	int 			aec = ops->read8(card, 0, NULL) | (ops->read8(card, 1, NULL) << 8);
	int 			board = ops->read8(card, 2, NULL) | (ops->read8(card, 3, NULL) << 8);
	int 			hardware = ops->read8(card, 4, NULL);
	char 			sw1 = ops->read8(card, 6, NULL);
	char 			sw2 = ops->read8(card, 7, NULL);
	int 			caps = ops->read8(card, 8, NULL);

	snprintf(card->info, sizeof(card->info), "%04X %04X Voltage: %s Rev: %c%c Caps: %s%s%s%s%s%s%s",
			aec, board, 
			(hardware & 0x4) ? "5.0v/3.3v" : (hardware & 0x8) ? "3.3v" : "5.0v",
			sw1, sw2,
			caps & 0x80 ? "VGEN " : "",
			caps & 0x40 ? "VRDR " : "",
			caps & 0x20 ? "LGEN " : "",
			caps & 0x10 ? "LRDR " : "",
			caps & 0x08 ? "OSD " : "",
			caps & 0x04 ? "L21 " : "",
			caps & 0x02 ? "SER " : "");
	return;
}

void pcitc_getserial(struct pcitc_t *card) {
	int i;
	int vals[8];
	int ret = pcitc_mailbox_send(card, 0x3B);
	if(ret != 0x3B) {
		perror("Unknown response: %d\n", ret);
		return;
	}
	
	for(i = 0; i < 8; i++) vals[i] = card->ops->read8(card, 0x50 + i, NULL);
	snprintf(card->serial, sizeof(card->serial), "%02X%02X.%02X%02X.%02X%02X.%02X%02X", 
		vals[0], vals[1], vals[2], vals[3], vals[4], vals[5], vals[6], vals[7]);
	return;
}



/* Setup the card */
int pcitc_setup(struct pcitc_t *card, int fps) {
	struct pcitc_dops_t 	*ops = card->ops;
	int 			ret = 0;

	ops->write8(card, 0x2C, 0x06);		/* Set the LTC reader to be "ontime" and leave embedded bits in with the timecode */
	ops->write8(card, 0x6A, 0x02);		/* Set the card to do software sync */
	ops->write8(card, 0x2E, 0x00);		/* Make sure all interrupts are off */
	ops->write8(card, 0x69, 0x04);		/* Set the generator to keep embedded bits */

	/* Set the frame rate */
	switch(fps) {
		case 24: ret = pcitc_mailbox_send(card, 0x05); break;
		case 25: ret = pcitc_mailbox_send(card, 0x04); break;
		case 30: ret = pcitc_mailbox_send(card, 0x03); break;
		default:
			 perror("Unsupported frame rate: %d\n", fps);
			 return -1;
			 break;
	}
	if(ret < 0) {
		perror("Failed to set frame rate: %d\n", ret);
		return -2;
	}

	/* Set the card up for LTC read/write */
	ret = pcitc_mailbox_send(card, 0x25);
	if(ret < 0) {
		perror("Failed to setup for LTC read/write: %d\n", ret);
		return -3;
	}

	/* Zero the cards timecode and load the values */
	ops->write32(card, 0x60, 0x00000000);
	ops->write32(card, 0x64, 0x00000000);
	ret = pcitc_mailbox_send(card, 0x40);
	if(ret < 0) {
		perror("Failed to zero values: %d\n", ret);
		return -4;
	}

	/* Enable the output and freeze any counting */
	ret = pcitc_mailbox_send(card, 0x4B);
	if(ret < 0) {
		perror("Failed to enable output: %d\n", ret);
		return -5;
	}

	ret = pcitc_mailbox_send(card, 0x4D);
	if(ret < 0) {
		perror("Failed to freeze counting: %d\n", ret);
		return -6;
	}

	pinfo("LTC board set for %d fps\n", fps);
	return 0;
}

int pcitc_run(void *dev, int val) {
	struct pcitc_t *card = (struct pcitc_t *)dev;
	int ret = (val) ? pcitc_mailbox_send(card, 0x4E) :  pcitc_mailbox_send(card, 0x4D);
	return (ret < 0) ? -EIO : 0;
}

int pcitc_sync(void *dev, int val) {
	struct pcitc_t *card = (struct pcitc_t *)dev;
	card->ops->write8(card, 0xBF, val ? 0x81 : 0x80);
	return 0;
}

int pcitc_readtcgen(struct pcitc_t *card, timecode_t *tc) {
	uint8_t 		rate;
	uint8_t 		d1, d2;
	uint32_t 		tcode, ubits;
	int 			t;
	struct pcitc_dops_t 	*ops = card->ops;
	timecode_smpte_t 	stc;

	/* Set the timecode to invalid, until we know otherwise */
	timecode_init(tc);

	/* If the validation bytes don't jive, try reading it all again */
	for(t = 0; t < READ_TRIES; t++) {
		d1 = ops->read8(card, 0x4B, NULL);
		tcode = ops->read32(card, 0x40, NULL);
		ubits = ops->read32(card, 0x44, NULL);
		d2 = ops->read8(card, 0x4B, NULL);
		if(d1 == d2) break;
		udelay(10);
	}

	if(t == READ_TRIES) {
		perror("Failed to read tc generator for LTC %d\n", card->id);
		return -EAGAIN;
	}

	/* Figure out the frame rate */
	d1 = ops->read8(card, 0x0D, NULL);
	if(d1 & 0x10) {
		rate = 25;
	} else if(d1 & 0x40) {
		rate = 24;
	} else {
		rate = 30;
	}

	/* Set the structure */
	stc.timecode = tcode; 
	stc.userbits = ubits; 
	stc.fps = rate;
	*tc = timecode_from_smpte(&stc);
	return 0;
}

int pcitc_read(void *dev, timecode_t *tc) {
	struct pcitc_t 		*card = (struct pcitc_t *)dev;
	uint8_t 		rate;
	uint8_t 		d1, d2, flags;
	uint32_t 		tcode, ubits;
	int			t;
	struct pcitc_dops_t 	*ops = card->ops;
	timecode_smpte_t 	stc;

	/* Set the timecode to invalid, until we know otherwise */
	timecode_init(tc);

	/* First check to see if the timecode is valid */
	d1 = ops->read8(card, 0x0C, NULL);
	if(!(d1 & 0x10)) return -ENOTCONN;

	/* If the validation bytes don't jive, try reading it all again */
	for(t = 0; t < READ_TRIES; t++) {
		d1 = ops->read8(card, 0x1A, NULL);
		tcode = ops->read32(card, 0x10, NULL);
		ubits = ops->read32(card, 0x14, NULL);
		flags = ops->read8(card, 0x18, NULL);
		d2 = ops->read8(card, 0x1A, NULL);
		if(d1 == d2) break;
		udelay(10);
	}
	if(t == READ_TRIES) return -EAGAIN;

	/* Figure out the frame rate */
	d1 = ops->read8(card, 0x0D, NULL);
	if(d1 & 0x10) {
		rate = 25;
	} else if(d1 & 0x40) {
		rate = 24;
	} else {
		rate = 30;
	}

	/* Set the structure */
	stc.timecode = tcode; 
	stc.userbits = ubits; 
	stc.fps = rate;
	*tc = timecode_from_smpte(&stc);
	return 0;
}

int pcitc_write(void *dev, const timecode_t *tc) {
	struct pcitc_t 		*card = (struct pcitc_t *)dev;
	struct pcitc_dops_t 	*ops = card->ops;
	int 			ret;
	timecode_smpte_t 	stc;

	/* Check to see if we need to change frames rates */
	if(card->cfps != tc->fps) {
		ret = pcitc_setup(card, tc->fps);
		if(ret < 0) {
			perror("Failed to setup board: %d\n", ret);
		} else {
			card->cfps = tc->fps;
		}
	}

	/* Wait for the OK to change the generator data */
	/* NOTE: It seems the window on the C8 firmware is much tighter
	 * and will cause a timeout in 30 FPS, so I commented this out
	 * to see if it makes a difference --jth */
	//for(i = 0; i < WRITE_WAIT_MAX; i++) {
	//	status = ops->read8(card, 0x49, NULL);
	//	if(status & 0x20) break;
	//	udelay(1);
	//}
	//if(i == WRITE_WAIT_MAX) perror("Waited max time for LTC write\n");

	/* Load the timecode and userbits into the card */
	stc = timecode_to_smpte(tc);
	ops->write32(card, 0x60, stc.timecode);
	ops->write32(card, 0x64, stc.userbits);
	ret = pcitc_mailbox_send(card, 0x40);
	return (ret < 0) ? -EIO : 0;
}

#if 0
static ssize_t pcitc_show_genstatus(struct class_device *cls, char *buf) {
	pcitc_t 	*card = (pcitc_t *)cls->class_data;
	ssize_t 	rsize = 0;
	uint8_t		status = card->ops->read8(card, 0x4A, NULL);

	if(status & 0x01) rsize += snprintf(buf + rsize, PAGE_SIZE, "RUN ");
	if(status & 0x02) rsize += snprintf(buf + rsize, PAGE_SIZE, "OUTPUT ");
	if(status & 0x04) rsize += snprintf(buf + rsize, PAGE_SIZE, "LOCK ");
	rsize += snprintf(buf + rsize, PAGE_SIZE, "\n");
	return rsize;
}

static ssize_t pcitc_show_reader(struct class_device *cls, char *buf) {
	pcitc_t 	*card = (pcitc_t *)cls->class_data;
	timecode_t 	tc;
	int 		r = pcitc_readtc(card, &tc);
	if(timecode_is_valid(&tc)) {
		char tcstr[32];
		timecode_to_string(&tc, tcstr, 32);
		return snprintf(buf, PAGE_SIZE, "%s %dfps %08X\n", 
			tcstr, tc.fps, tc.userbits);
	}
	return snprintf(buf, PAGE_SIZE, "error %d\n", r);
}

static ssize_t pcitc_show_regdump(struct class_device *cls, char *buf) {
	pcitc_t 		*card = (pcitc_t *)cls->class_data;
	struct pcitc_dops_t 	*ops = card->ops;
	int 			i;
	ssize_t 		cnt = 0;

	for(i = 0; i < 256; i++) {
		cnt += sprintf(buf + cnt, "%02X: %02X\n", i, ops->read8(card, i, NULL));
	}
	return cnt;
}

static ssize_t pcitc_show_cardinfo(struct class_device *cls, char *buf) {
	pcitc_t *card = (pcitc_t *)cls->class_data;
	return pcitc_getcardinfo(card, buf, 4096);
}
#endif

static int pcitc_getinfo(void *dev, char *buf, size_t size) {
	struct pcitc_t 		*card = (struct pcitc_t *)dev;
	return snprintf(buf, size, "%s\nFirmware serial: %s\n", card->info, card->serial);
}

static struct pcitc_dops_t pcitc_pci_dops = {
	.init = 		pcitc_pci_init,
	.cleanup = 		pcitc_pci_cleanup,
	.read8 = 		pcitc_pci_read8,
	.read32 = 		pcitc_pci_read32,
	.write8 = 		pcitc_pci_write8,
	.write32 = 		pcitc_pci_write32
};

static struct pcitc_dops_t pcitc_pcie_dops = {
	.init = 		pcitc_pcie_init,
	.cleanup = 		pcitc_pcie_cleanup,
	.read8 = 		pcitc_pcie_read8,
	.read32 = 		pcitc_pcie_read32,
	.write8 = 		pcitc_pcie_write8,
	.write32 = 		pcitc_pcie_write32
};

static struct tcio_ops_t pcitc_tcio_ops = {
	.getinfo = 		pcitc_getinfo,
	.read = 		pcitc_read,
	.write = 		pcitc_write,
	.sync = 		pcitc_sync,
	.run = 			pcitc_run
};

static void pcitc_device_cleanup(struct pcitc_t *card) {
	if(card != NULL) {
		if(card->tcio != NULL) tcio_unregister(card->tcio);
		card->ops->cleanup(card);
		pci_set_drvdata(card->pcidev, NULL);
		pci_disable_device(card->pcidev);
		kfree(card);
	}
	return;
}

static int pcitc_pci_probe(struct pci_dev *dev, const struct pci_device_id *id) {
	struct pcitc_t 	*card = NULL;

	card = (struct pcitc_t *)kmalloc(sizeof(*card), GFP_KERNEL);
	if(card == NULL) {
		perror("Allocate card structure failed for %s\n", pci_name(dev));
		goto setupfail;
	}
	pci_set_drvdata(dev, (void *)card);
	memset(card, 0, sizeof(*card));
	card->id = cardct++;
	snprintf(card->name, TCIO_NAME_MAX, "pcitc%d", card->id);
	card->pcidev = dev;
	card->deviceid = id->device;

	switch(card->deviceid) {
		case PCITC_PCI_ID:
			card->ops = &pcitc_pci_dops;
			break;

		case PCITC_PCIE_ID:
			card->ops = &pcitc_pcie_dops;
			break;

		default:
			perror("%s: unsupported card 0x%04X\n", pci_name(dev), card->deviceid);
			goto setupfail;
	}

	if(pci_enable_device(dev)) {
		perror("pci_enable_device failed on device %s\n", pci_name(dev));
		goto setupfail;
	}

	if(card->ops->init(card)) {
		perror("Failed to initialize card at %s\n", pci_name(dev));
		goto setupfail;
	}
	pcitc_getcardinfo(card);
	pcitc_getserial(card);
	pcitc_setup(card, 30);

	card->tcio = tcio_register(card->name, card, &pcitc_tcio_ops);
	if(card->tcio == NULL) {
		perror("Failed to register tcio interface for %s\n", pci_name(dev));
		goto setupfail;
	}
	return 0;
	
setupfail:
	pcitc_device_cleanup(card);
	return -ENOMEM;
}

/* This function gets called when the driver is unloading.  We must release our
 * useage of the device here */
static void pcitc_pci_remove(struct pci_dev *dev) {
	struct pcitc_t *card = (struct pcitc_t *)pci_get_drvdata(dev);
	pcitc_device_cleanup(card);
	return;
}

/* This is a list of all the PCI devices we support */
static struct pci_device_id pcitc_pci_device_ids[] = {
	{PCI_DEVICE(PCITC_VENDOR_ID, PCITC_PCI_ID)},
	{PCI_DEVICE(PCITC_VENDOR_ID, PCITC_PCIE_ID)},
	{0}
};

static struct pci_driver pcitc_pci_driver = {
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,15)
	.owner = 	THIS_MODULE,
#endif
	.name = 	MODNAME,
	.id_table =	pcitc_pci_device_ids,
	.probe = 	pcitc_pci_probe,
	.remove =	pcitc_pci_remove
	
};

static void pcitc_cleanup(void) {
	pci_unregister_driver(&pcitc_pci_driver);
	return;
}

/* This function is responsible for loading the driver */
static int __init pcitc_init(void) {
	int error;
	
	/* introduce ourself to the kernel log */
	pinfo("Driver for the Adrienne PCI-TC boards, Version %d\n", PCITC_API_VERSION);
	pinfo("Copyright 2000-2010, SpectSoft <http://www.spectsoft.com>\n");

	error = pci_register_driver(&pcitc_pci_driver);
	if(error) {
		perror("Error initializing devices: %d\n", error);
		goto initfail;
	}

	/* Make sure we found at least one card */
	if(!cardct) {
		error = -ENODEV;
		goto initfail;
	}
	return 0;

initfail:
	pcitc_cleanup();
	return error;
}

/* This function is responsible for unloading the driver */
static void __exit pcitc_exit(void) {
	pcitc_cleanup();
	return;
}

/* Declare the module initialization and exit points */
module_init(pcitc_init);
module_exit(pcitc_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Jason Howard <jason@spectsoft.com>");
MODULE_DESCRIPTION("Linux 2.6 driver for the Adrienne PCI-TC boards");
MODULE_DEVICE_TABLE(pci, pcitc_pci_device_ids);


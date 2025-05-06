/*******************************************************************************
 * pcitc.h
 *
 * Creation Date: May 09, 2006
 * Author(s): 
 *   Jason Howard <jth@spectsoft.com>
 *
 * 
 * Copyright (c) 2005, SpectSoft
 *   All Rights Reserved.
 *   http://www.spectsoft.com/
 *   info@spectsoft.com
 * 
 * $Id$
 * 
 ******************************************************************************/

#ifndef _PCITC_H_
#define _PCITC_H_

#include <timecode.h>

/* Exported kernel functions */
#ifdef __KERNEL__

#include <asm/atomic.h>
#include <linux/miscdevice.h>
#include <linux/spinlock.h>
#include <linux/sched.h>
#include <linux/wait.h>
#include <linux-abstraction.h>

struct pcitc_dops_t;

/* Internal card description structure */
typedef struct {
	int 			id;			/* Card number in the system */
	int 			deviceid;		/* PCI device ID of the card */
	void 			*reg; 			/* Mapped register space */
	size_t			reglen; 		/* Length of mapped register space */
	
	struct cdev 		cdev; 			/* Char device */
	struct pci_dev 		*pcidev; 		/* PCI device */
	struct kobject 		kobj; 			/* Control KObject */
	int 			cfps;			/* Current Frame Rate */
	struct pcitc_dops_t 	*ops;
} pcitc_t;

struct pcitc_dops_t {
	int 			(*init)(pcitc_t *card);
	int 			(*cleanup)(pcitc_t *card);
	int 			(*write8)(pcitc_t *card, unsigned long off, uint8_t val);
	int 			(*write32)(pcitc_t *card, unsigned long off, uint32_t val);
	uint8_t 		(*read8)(pcitc_t *card, unsigned long off, int *status);
	uint32_t 		(*read32)(pcitc_t *card, unsigned long off, int *status);
} pcitc_driver_ops_t;

typedef struct {
	pcitc_t		*(*device_find)(int);
	int 		(*readtc)(pcitc_t *card, timecode_t *tc);
	int 		(*readtcgen)(pcitc_t *card, timecode_t *tc);
	void 		(*writetc)(pcitc_t *card, const timecode_t *tc);
	void 		(*sync)(pcitc_t *card, int val);
	void 		(*run)(pcitc_t *card, int val);
} pcitc_ops_t;

void pcitc_register(pcitc_ops_t *);

#endif

#endif /* ifndef _PCITC_H_ */


/*******************************************************************************
 * p2slave.h
 *
 * Creation Date: June 20, 2008
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
 * DESC: p2slave kernel driver
 * 
 ******************************************************************************/
#ifndef _P2SLAVE_H_
#define _P2SLAVE_H_

#ifndef __KERNEL__
#include <stdint.h>
#endif

#define P2SLAVE_API_VERSION	20100420
#define P2SLAVE_PKTSIZE		32

#include <linux/ioctl.h>
#include <timecode.h>

/** This structure is the data unit returned by reading the /dev/p2slave device */
struct p2slave_pkt_t {
	struct timeval 			tstamp;
	uint8_t				data[P2SLAVE_PKTSIZE];
};

struct p2slave_status_t {
	uint8_t				data[16];
};

#define P2SLAVE_IOC_MAGIC		'P'
#define P2SLAVE_IOC_SETID		_IOW(P2SLAVE_IOC_MAGIC, 1, uint16_t)
#define P2SLAVE_IOC_SETSTATUS		_IOW(P2SLAVE_IOC_MAGIC, 2, struct p2slave_status_t)
#define P2SLAVE_IOC_SETTCGEN		_IOW(P2SLAVE_IOC_MAGIC, 3, timecode_t)
#define P2SLAVE_IOC_SETTC		_IOW(P2SLAVE_IOC_MAGIC, 4, timecode_t)
#define P2SLAVE_IOC_SETIN		_IOW(P2SLAVE_IOC_MAGIC, 5, timecode_t)
#define P2SLAVE_IOC_SETOUT		_IOW(P2SLAVE_IOC_MAGIC, 6, timecode_t)
#define P2SLAVE_IOC_SETAIN		_IOW(P2SLAVE_IOC_MAGIC, 7, timecode_t)
#define P2SLAVE_IOC_SETAOUT		_IOW(P2SLAVE_IOC_MAGIC, 8, timecode_t)
#define P2SLAVE_IOC_SETPREROLL 		_IOW(P2SLAVE_IOC_MAGIC, 9, timecode_t)
#define P2SLAVE_IOC_SETSPEED 		_IOW(P2SLAVE_IOC_MAGIC, 10, int)

/* Exported kernel functions */
#ifdef __KERNEL__

#include <asm/atomic.h>
#include <linux/miscdevice.h>
#include <linux/spinlock.h>
#include <linux/sched.h>
#include <linux/wait.h>

struct p2slave_t;

/** Creates a p2slave kernel object */
struct p2slave_t *p2slave_register(const char *name);

/** Destroys and frees p2slave kernel object */
void p2slave_unregister(struct p2slave_t *p2slave);

/** Handles a single packet and returns a response packet */
int p2slave_handle_pkt(struct p2slave_t *p2slave, uint8_t *resp, const uint8_t *pkt);

/** Sets the vtr ID value */
void p2slave_set_id(struct p2slave_t *, uint16_t idval);

/** Sets the vtr status data */
void p2slave_set_status(struct p2slave_t *, const struct p2slave_status_t *);

/** Sets the vtr timecode */
void p2slave_set_tc(struct p2slave_t *, const timecode_t *);

/** Sets the vtr timecode generator */
void p2slave_set_tcgen(struct p2slave_t *, const timecode_t *);

/** Sets the vtr video in point */
void p2slave_set_vidin(struct p2slave_t *, const timecode_t *);

/** Sets the vtr video out point */
void p2slave_set_vidout(struct p2slave_t *, const timecode_t *);

/** Sets the vtr audio in point */
void p2slave_set_audin(struct p2slave_t *, const timecode_t *);

/** Sets the vtr audio out point */
void p2slave_set_audout(struct p2slave_t *, const timecode_t *);

/** Sets the vtr preroll value */
void p2slave_set_preroll(struct p2slave_t *, const timecode_t *);

/** Sets the vtr speed */
void p2slave_set_speed(struct p2slave_t *, int);


#endif

#endif /* ifndef _P2SLAVE_H_ */


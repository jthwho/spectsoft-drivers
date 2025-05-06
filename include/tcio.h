/*******************************************************************************
 * tcio.h
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

#ifndef _TCIO_H_
#define _TCIO_H_
#include <timecode.h>

#define TCIO_API_VERSION 	20100420
#define TCIO_NAME_MAX		32

#ifdef __KERNEL__

struct tcio_t;
struct device;
struct cdev;

/** Timecode operations structure
 * This structure is defined by any driver that wants to implement a
 * timecode I/O device */
struct tcio_ops_t {
	int		(*getinfo)(void *device, char *buf, size_t size);
	int 		(*read)(void *device, timecode_t *tc);
	int 		(*write)(void *device, const timecode_t *tc);
	int 		(*sync)(void *device, int field);
	int 		(*run)(void *device, int val);
};

/** Registers a timecode device.  Returns the tcio_t handle */
struct tcio_t *tcio_register(const char *name, void *device, struct tcio_ops_t *ops);

/** Unregisters a timecode device. */
void tcio_unregister(struct tcio_t *device);

/** Returns the timecode I/O device named 'name', or NULL if not found */ 
struct tcio_t *tcio_find(int device_num);

/** Returns textual information about the timecode I/O device */
int tcio_getinfo(struct tcio_t *tcio, char *buf, size_t size);

/** Reads a timecode from the device */
int tcio_read(struct tcio_t *tcio, timecode_t *tc);

/** Writes a timecode to the device */
int tcio_write(struct tcio_t *tcio, const timecode_t *tc);

/** Issues a sync "pulse" to the timecode device */
int tcio_sync(struct tcio_t *tcio, int field);

/** Enables timecode running */
int tcio_run(struct tcio_t *tcio, int val);

#endif /* ifdef __KERNEL__ */
#endif /* ifndef _TCIO_H_ */


/*******************************************************************************
 * aja_userspace.h
 *
 * Creation Date: October 04, 2005
 * Author(s): 
 *   Jason Howard <jason@spectsoft.com>
 *
 * 
 * Copyright (c) 2005, SpectSoft
 *   All Rights Reserved.
 *   http://www.spectsoft.com/
 *   info@spectsoft.com
 * 
 * $Id$
 * 
 * DESC: Userspace functions for the AJA driver
 * 
 ******************************************************************************/
#ifndef _AJA_USERSPACE_H_
#define _AJA_USERSPACE_H_

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <string.h>

#ifdef _cplusplus
extern "C" {
#endif

#include <aja_ioctls.h>

	
static inline aja_boardinfo_t aja_getboardinfo(int fd) {
	aja_boardinfo_t info;
	ioctl(fd, AJACTL_BOARDINFO, &info);
	return info;
}

static inline uint32_t aja_getregister(int fd, const aja_register_t reg) {
	aja_registerio_t rio;
	rio.reginfo = reg;
	ioctl(fd, AJACTL_GETREGISTER, &rio);
	return rio.value;
}

static inline int aja_setregister(int fd, const aja_register_t reg, uint32_t val) {
	aja_registerio_t rio;
	rio.reginfo = reg;
	rio.value = val;
	return ioctl(fd, AJACTL_SETREGISTER, &rio);
}

static inline uint64_t aja_getirqcount(int fd, int type) {
	aja_irqcountget_t get;
	get.type = type;
	ioctl(fd, AJACTL_IRQCOUNT, &get);
	return get.count;
}

static inline int aja_irqsleep(int fd, int type) {
	return ioctl(fd, AJACTL_IRQSLEEP, type);
}

static inline int aja_dmatocard(int fd, int engine, void *buffer, uint32_t cardadd, uint32_t len) {
	aja_dmainfo_t dma;
	dma.engine = engine;
	dma.uadd = buffer;
	dma.cadd = cardadd;
	dma.len = len;
	dma.dir = AJA_DMATOCARD;
	return ioctl(fd, AJACTL_DMA, &dma);
}

static inline int aja_dmafromcard(int fd, int engine, void *buffer, uint32_t cardadd, uint32_t len) {
	aja_dmainfo_t dma;
	dma.engine = engine;
	dma.uadd = buffer;
	dma.cadd = cardadd;
	dma.len = len;
	dma.dir = AJA_DMAFROMCARD;
	return ioctl(fd, AJACTL_DMA, &dma);
}

static inline int aja_stream_running(int fd) {
	return ioctl(fd, AJACTL_STREAM_RUNNING);
}

static inline int aja_stream_dropped(int fd) {
	int retval;
	int ret = ioctl(fd, AJACTL_STREAM_DROPPED, &retval);
	if(ret) return ret;
	return retval;
}

static inline int aja_stream_init(int fd, int flags, int chans, int pages) {
	aja_stream_init_t si;
	si.flags = flags;
	si.pagect = pages;
	si.chans = chans;
	return ioctl(fd, AJACTL_STREAM_INIT, &si);
}

static inline int aja_stream_start(int fd) {
	return ioctl(fd, AJACTL_STREAM_START);
}

static inline int aja_stream_stop(int fd) {
	return ioctl(fd, AJACTL_STREAM_STOP);
}

static inline int aja_stream_setmeta(int fd, int page, const aja_stream_meta_t *meta) {
	aja_stream_meta_req_t m;
	m.page = page;
	m.meta = (aja_stream_meta_t *)meta;
	return ioctl(fd, AJACTL_STREAM_SETMETA, &m);
}

static inline int aja_stream_getmeta(int fd, int page, aja_stream_meta_t *meta) {
	aja_stream_meta_req_t m;
	m.page = page;
	m.meta = meta;
	return ioctl(fd, AJACTL_STREAM_GETMETA, &m);
}

static inline int aja_stream_page_alloc(int fd) {
	return ioctl(fd, AJACTL_STREAM_PAGE_ALLOC);
}

static inline int aja_stream_page_free(int fd, int page) {
	return ioctl(fd, AJACTL_STREAM_PAGE_FREE, &page);
}

static inline int aja_stream_fifo_pop(int fd) {
	return ioctl(fd, AJACTL_STREAM_FIFO_POP);
}

static inline int aja_stream_fifo_push(int fd, int page) {
	return ioctl(fd, AJACTL_STREAM_FIFO_PUSH, &page);
}

static inline int aja_stream_fifosize(int fd) {
	return ioctl(fd, AJACTL_STREAM_FIFO);
}

static inline int aja_stream_settrigger(int fd, int64_t id) {
	return ioctl(fd, AJACTL_STREAM_SETTRIG, &id);
}

static inline int aja_stream_setspeed(int fd, double speed) {
	int val = (int)(speed * AJA_SPEED_DIVISOR);
	return ioctl(fd, AJACTL_STREAM_SETSPEED, &val);
}

static inline int aja_stream_getspeed(int fd, double *speed) {
	int val;
	int ret = ioctl(fd, AJACTL_STREAM_GETSPEED, &val);
	if(!ret) {
		*speed = (double)val / (double)AJA_SPEED_DIVISOR;
	}
	return ret;
}

#ifdef _cplusplus
}
#endif

#endif /* ifndef _AJA_USERSPACE_H_ */



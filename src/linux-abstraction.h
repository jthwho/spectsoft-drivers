/*******************************************************************************
 * linux-abstraction.h
 *
 * Creation Date: January 20, 2010
 * Author(s):
 *   Jason Howard <jth@spectsoft.com>
 *
 *
 * Copyright (c) 2010, SpectSoft
 *   All Rights Reserved.
 *   http://www.spectsoft.com
 *   info@spectsoft.com
 *
 * $Id$
 *
 * DESC:
 *
 ******************************************************************************/
 
#ifndef _LINUX_ABSTRACTION_H_
#define _LINUX_ABSTRACTION_H_

#include <linux/device.h>
#include <linux/scatterlist.h>

#if LINUX_VERSION_CODE <= KERNEL_VERSION(2,6,18)
static inline void sg_init_table(struct scatterlist *sgl, unsigned int nents) {
	memset(sgl, 0, sizeof(*sgl) * nents);
	return;
}

static inline void sg_set_page(struct scatterlist *sg, struct page *page, unsigned int len, unsigned int offset) {
	sg->page = page;
	sg->offset = offset;
	sg->length = len;
	return;
}
#endif

// Fix the class system
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,13)
#define CLASS_T struct class
#define CLASS_CREATE(mod, name) class_create(mod, name)
#define CLASS_DESTROY(ptr) class_destroy(ptr)
#else
#define CLASS_T struct class_simple
#define CLASS_CREATE(mod, name) simple_class_create(mod, name)
#define CLASS_DESTROY(ptr) simple_class_destroy(ptr)
#endif

// Fix the device system
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,27)
#define DEVICE_T struct device
#define DEVICE_CREATE(cls, parent, devt, name) device_create(cls, parent, devt, NULL, "%s", name)
#define DEVICE_DESTROY(cls, devt) device_destroy(cls, devt)
#define DEVICE_SET_DATA(devptr, data) dev_set_drvdata(devptr, data)

#elif LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,15)
#define DEVICE_T struct device
#define DEVICE_CREATE(cls, parent, devt, name) device_create(cls, parent, devt, name)
#define DEVICE_DESTROY(cls, devt) device_destroy(cls, devt)
#define DEVICE_SET_DATA(devptr, data) dev_set_drvdata(devptr, data)

#elif LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,13)
#define DEVICE_T struct class_device
#define DEVICE_CREATE(cls, parent, devt, name) class_device_create(cls, devt, NULL, name)
#define DEVICE_DESTROY(cls, devt) class_device_destroy(cls, devt)
#define DEVICE_SET_DATA(devptr, data) class_set_devdata(devptr, data)

#else
#define DEVICE_T struct class_device
#define DEVICE_CREATE(cls, parent, devt, name) class_simple_device_add(cls, devt, NULL, name)
#define DEVICE_DESTROY(cls, devt) class_simple_device_remove(devt)
#define DEVICE_SET_DATA(devptr, data) class_set_devdata(devptr, data)

#endif



#endif /* ifndef _LINUX_ABSTRACTION_H_ */
 

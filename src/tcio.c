/*******************************************************************************
 * tcio.c
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
 * DESC: Linux 2.6 driver for timecode interfaces
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
#include <asm/atomic.h>
#include <linux/miscdevice.h>
#include <linux/spinlock.h>
#include <linux/sched.h>
#include <linux/wait.h>
#include <linux/err.h>

#include <tcio.h>
#include "utils.h"
#include "linux-abstraction.h"

#define MODNAME		"timecodeio"
#define MAX_DEVS	256

static dev_t 		chrdev 		= 0;
static CLASS_T 		*class 		= NULL;
static struct tcio_t 	*devs[MAX_DEVS];
static DEFINE_MUTEX(mutex);
static struct tcio_t	*testhdl = NULL;

struct tcio_t {
	int			index;
	char			name[TCIO_NAME_MAX];
	void 			*device;
	struct tcio_ops_t	ops;
	DEVICE_T		*dev;
	struct cdev 		cdev;
};

static int tcio_open(struct inode *minode, struct file *filp) {
	return 0;
}

static int tcio_release(struct inode *minode, struct file *file) {
	return 0;
}

static struct file_operations tcio_fops = { 
	.owner =		THIS_MODULE,
	//.ioctl = 		tcio_ioctl,
	.open = 		tcio_open,
	.release = 		tcio_release,
};

static ssize_t tcio_show_name(struct device *dev, struct device_attribute *attr, char *buf) {
	struct tcio_t 	*tcio = (struct tcio_t *)dev_get_drvdata(dev);
	return snprintf(buf, PAGE_SIZE, "%s\n", tcio->name);
}
DEVICE_ATTR(name, 0444, tcio_show_name, NULL);


static ssize_t tcio_show_timecode(struct device *dev, struct device_attribute *attr, char *buf) {
	struct tcio_t 	*tcio = (struct tcio_t *)dev_get_drvdata(dev);
	timecode_t 	tc;
	int 		r = tcio_read(tcio, &tc);
	char 		tcstr[32];
	if(r) return snprintf(buf, PAGE_SIZE, "ERROR %d\n", r);
	timecode_to_string(&tc, tcstr, 32);
	return snprintf(buf, PAGE_SIZE, "%s %dfps %08X\n", tcstr, tc.fps, tc.userbits);
}
DEVICE_ATTR(timecode, 0444, tcio_show_timecode, NULL);

static ssize_t tcio_show_info(struct device *dev, struct device_attribute *attr, char *buf) {
	char 		buffer[512];
	struct tcio_t 	*tcio = (struct tcio_t *)dev_get_drvdata(dev);
	int 		r = tcio_getinfo(tcio, buffer, sizeof(buffer));
	if(r < 0) return snprintf(buf, PAGE_SIZE, "ERROR %d\n", r);
	strncpy(buf, buffer, sizeof(buffer));
	return r;
}
DEVICE_ATTR(info, 0444, tcio_show_info, NULL);

static struct tcio_t *tcio_allocate(const char *name, void *device, struct tcio_ops_t *ops) {
	int i;
	struct tcio_t *tcio = (struct tcio_t *)kzalloc(sizeof(*tcio), GFP_KERNEL);
	if(tcio == NULL) {
		perror("Failed to allocate device memory for %p\n", device);
		return NULL;
	}
	strncpy(tcio->name, name, TCIO_NAME_MAX);
	tcio->device = device;
	if(ops != NULL) tcio->ops = *ops;

	/* Try to find a spot for the tcio object */
	mutex_lock(&mutex);
	for(i = 0; i < MAX_DEVS; i++) {
		if(devs[i] == NULL) {
			tcio->index = i;
			devs[i] = tcio;
			break;
		}
	}
	mutex_unlock(&mutex);

	/* Make sure we found one */
	if(i == MAX_DEVS) {
		perror("Failed to find a free tcio_t object\n");
		kfree(tcio);
		return NULL;
	}
	return tcio;
}

static void tcio_free(struct tcio_t *tcio) {
	mutex_lock(&mutex);
	devs[tcio->index] = NULL;
	mutex_unlock(&mutex);
	kfree(tcio);
	return;
}

struct tcio_t *tcio_find(int index) {
	if(index < 0 || index >= MAX_DEVS) {
		perror("index %d is out of bounds\n", index);
		return NULL;
	}
	return devs[index];
}

void tcio_unregister(struct tcio_t *tcio) {
	if(tcio == NULL) return;
	if(tcio->dev != NULL) {
		device_remove_file(tcio->dev, &dev_attr_name);
		device_remove_file(tcio->dev, &dev_attr_timecode);
		device_remove_file(tcio->dev, &dev_attr_info);
		DEVICE_DESTROY(class, tcio->dev->devt);
	}
	cdev_del(&tcio->cdev);
	tcio_free(tcio);
	return;
}

struct tcio_t *tcio_register(const char *name, void *device, struct tcio_ops_t *ops) {
	int 		error = 0;
	struct tcio_t 	*tcio = NULL;
	dev_t		devt;
	char		buf[32];

	tcio = tcio_allocate(name, device, ops);
	if(tcio == NULL) {
		perror("Unable to find a free device.  Failed to register %s/%p\n", name, device);
		goto regfail;
	}
	devt = MKDEV(MAJOR(chrdev), tcio->index);

	snprintf(buf, sizeof(buf), "tcio%d", tcio->index);
	tcio->dev = DEVICE_CREATE(class, NULL, devt, buf);
	if(!tcio->dev || IS_ERR(tcio->dev)) {
		perror("Failed to set name for %s/%d/%p: %ld\n", 
			name, tcio->index, device, PTR_ERR(tcio->dev));
		tcio->dev = NULL;
		goto regfail;
	}
	DEVICE_SET_DATA(tcio->dev, tcio);

	error = device_create_file(tcio->dev, &dev_attr_name);
	if(error < 0) perror("Failed to add name file for %s/%d/%p)\n", name, tcio->index, device);
	error = device_create_file(tcio->dev, &dev_attr_timecode);
	if(error < 0) perror("Failed to add timecode file for %s/%d/%p)\n", name, tcio->index, device);
	error = device_create_file(tcio->dev, &dev_attr_info);
	if(error < 0) perror("Failed to add info file for %s/%d/%p\n", name, tcio->index, device);

	cdev_init(&tcio->cdev, &tcio_fops);
	tcio->cdev.owner = THIS_MODULE;
	error = cdev_add(&tcio->cdev, devt, 1);
	if(error < 0) {
		perror("cdev_add %s/%d/%p failed, error %d\n", name, tcio->index, device, error);
		goto regfail;
	}

	pinfo("Registered %s as device (%d, %d)\n", tcio->name, MAJOR(chrdev), tcio->index);
	return tcio;

regfail:
	tcio_unregister(tcio);
	return NULL;
}

int tcio_getinfo(struct tcio_t *tcio, char *buf, size_t size) {
	if(tcio == NULL) perror("tcio_getinfo passed NULL pointer");
	if(tcio->ops.getinfo == NULL) return -ENXIO;
	return tcio->ops.getinfo(tcio->device, buf, size);
}

int tcio_read(struct tcio_t *tcio, timecode_t *tc) {
	if(tcio == NULL) perror("tcio_read passed NULL pointer");
	if(tcio->ops.read == NULL) return -ENXIO;
	return tcio->ops.read(tcio->device, tc);
}

int tcio_write(struct tcio_t *tcio, const timecode_t *tc) {
	if(tcio == NULL) perror("tcio_write passed NULL pointer");
	if(tcio->ops.write == NULL) return -ENXIO;
	return tcio->ops.write(tcio->device, tc);
}

int tcio_sync(struct tcio_t *tcio, int field) {
	if(tcio == NULL) perror("tcio_sync passed NULL pointer");
	if(tcio->ops.sync == NULL) return -ENXIO;
	return tcio->ops.sync(tcio->device, field);
}

int tcio_run(struct tcio_t *tcio, int val) {
	if(tcio == NULL) perror("tcio_run passed NULL pointer");
	if(tcio->ops.run == NULL) return -ENXIO;
	return tcio->ops.run(tcio->device, val);
}

static void tcio_cleanup(void) {
	if(chrdev) {
		unregister_chrdev_region(chrdev, 256);
		chrdev = 0;
	}
	if(class != NULL) {
		CLASS_DESTROY(class);
		class = NULL;
	}
	return;
}

/* This function is responsible for loading the driver */
static int __init tcio_init(void) {
	int error = 0;
	
	memset(devs, 0, sizeof(devs));
	pinfo("Timecode I/O, API version %d\n", TCIO_API_VERSION);
	pinfo("Copyright 2000-2010, SpectSoft <http://www.spectsoft.com>\n");

	/* Register the ajacards class */
	class = CLASS_CREATE(THIS_MODULE, MODNAME);
	if(class == NULL) {
		error = -ENODEV;
		perror("Failed to register class\n");
		goto initfail;
	}
	
	/* Get a char device */
	error = alloc_chrdev_region(&chrdev, 0, 256, MODNAME);
	if(error) {
		perror("alloc_chrdev_region failed.  Error: %d\n", error);
		goto initfail;
	}

	testhdl = tcio_register("testing", NULL, NULL);
	if(testhdl == NULL) perror("Failed to register test handle\n");
	return 0;

initfail:
	tcio_cleanup();
	return error;
}

/* This function is responsible for unloading the driver */
static void __exit tcio_exit(void) {
	tcio_unregister(testhdl);
	tcio_cleanup();
	return;
}

/* Declare the module initialization and exit points */
module_init(tcio_init);
module_exit(tcio_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Jason Howard <jason@spectsoft.com>");
MODULE_DESCRIPTION("Timecode I/O for Linux 2.6");

EXPORT_SYMBOL(tcio_register);
EXPORT_SYMBOL(tcio_unregister);
EXPORT_SYMBOL(tcio_find);
EXPORT_SYMBOL(tcio_read);
EXPORT_SYMBOL(tcio_write);
EXPORT_SYMBOL(tcio_sync);
EXPORT_SYMBOL(tcio_run);


/*******************************************************************************
 * p2slave.c
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
 * DESC: Linux kernel module for a Sony P2 slave.
 * 
 ******************************************************************************/


#include <linux/version.h>
#include <linux/ioctl.h>
#include <linux/fs.h>
#include <linux/kobject.h>
#include <linux/cdev.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>
#include <linux/kernel.h>       /* printk() */
#include <linux/slab.h>         /* kmalloc() */
#include <linux/errno.h>        /* error codes */
#include <linux/types.h>        /* size_t */
#include <linux/proc_fs.h>
#include <linux/fcntl.h>        /* O_ACCMODE */
#include <linux/aio.h>
#include <asm/uaccess.h>
#include <linux/device.h>
#include <linux/poll.h>

#include <p2slave.h>
#include "utils.h"
#include "linux-abstraction.h"

#define MODNAME			"p2slave"
#define MAX_DEVS		256
#define P2SLAVE_BUFSIZE		128

#define FUNC_SET(name, type, dconv) 						\
	void p2slave_set_##name(struct p2slave_t *dev, type data) {		\
		unsigned long flags;						\
		spin_lock_irqsave(&dev->datalock, flags);			\
		dev->data.name = dconv;						\
		spin_unlock_irqrestore(&dev->datalock, flags);			\
		return;								\
	}									\
	EXPORT_SYMBOL(p2slave_set_##name)

#define FUNC_GET(name, type)					\
	type p2slave_get_##name(struct p2slave_t *dev) {	\
		unsigned long 	flags;				\
		type 		ret;				\
		spin_lock_irqsave(&dev->datalock, flags);	\
		ret = dev->data.name;				\
		spin_unlock_irqrestore(&dev->datalock, flags);	\
		return ret;					\
	}							\
	EXPORT_SYMBOL(p2slave_get_##name)

#define FUNC_GET_SMPTE(name)								\
	static timecode_smpte_t p2slave_get_smpte_##name(struct p2slave_t *dev) {	\
		timecode_t tc = p2slave_get_##name(dev);				\
		return timecode_to_smpte(&tc);						\
	}

#define FUNC_GET_USER(name) 											\
	static int p2slave_get_user_##name(struct p2slave_t *dev, unsigned long arg) {				\
		unsigned long 	flags;										\
		int		ret;										\
		spin_lock_irqsave(&dev->datalock, flags);							\
		ret = copy_from_user((void *)&dev->data.name, (const void *)arg, sizeof(dev->data.name));	\
		spin_unlock_irqrestore(&dev->datalock, flags);							\
		return ret;											\
	}

static int			chrdev = 0;
static CLASS_T			*class = NULL;	/* class structure */ 
static struct p2slave_t		*devs[MAX_DEVS];  /* Devices structure */
static spinlock_t 		spinlock = SPIN_LOCK_UNLOCKED;

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Jason Howard <jth@spectsoft.com>");
MODULE_DESCRIPTION("Linux 2.6 driver for a Sony P2 slave");

struct p2slave_buffer_t {
	volatile int			read;	// Read Pointer
	volatile int			write;	// Write Pointer
	struct p2slave_pkt_t		buf[P2SLAVE_BUFSIZE];
};

struct p2slave_data_t {
	uint16_t			id;
	struct p2slave_status_t		status;
	timecode_t			tcgen;
	timecode_t			tc;
	timecode_t			in;
	timecode_t			out;
	timecode_t			ain;
	timecode_t			aout;
	timecode_t 			preroll;
	int 				speed;
};

enum p2slave_flags {
	P2S_Debug		= 0x0001	/* Output Packet Debug */
};

struct p2slave_stat_t {
	int 				count;	/* Number of packets */
	int				err_crc; /* Number of CRC errors */
};

struct p2slave_t {
	char				name[32];
	int				index;
	int 				flags;
	struct p2slave_stat_t		stats;
	atomic_t			opens;			// Number of opens
	wait_queue_head_t		wait;			// Wait Queue
	spinlock_t			datalock;
	struct p2slave_data_t		data;
	struct p2slave_buffer_t		buffer;
	struct cdev			cdev;
	struct device 			*device;
};

/* Standard P2 packets 
 * NOTE: The NAK packets have the checksum precalculated since they
 * get returned immediatly 
 */
static const uint8_t pkt_ack[] 		= {0x10, 0x01};
static const uint8_t pkt_nak_csum[]	= {0x11, 0x12, 0x04, 0x27};

FUNC_SET(id, uint16_t, data);
FUNC_GET(id, uint16_t);
FUNC_GET_USER(id);

FUNC_SET(status, const struct p2slave_status_t *, *data);
FUNC_GET(status, struct p2slave_status_t);
FUNC_GET_USER(status);

FUNC_SET(tcgen, const timecode_t *, *data);
FUNC_GET(tcgen, timecode_t);
FUNC_GET_SMPTE(tcgen);
FUNC_GET_USER(tcgen);

FUNC_SET(tc, const timecode_t *, *data);
FUNC_GET(tc, timecode_t);
FUNC_GET_SMPTE(tc);
FUNC_GET_USER(tc);

FUNC_SET(in, const timecode_t *, *data);
FUNC_GET(in, timecode_t);
FUNC_GET_SMPTE(in);
FUNC_GET_USER(in);

FUNC_SET(out, const timecode_t *, *data);
FUNC_GET(out, timecode_t);
FUNC_GET_SMPTE(out);
FUNC_GET_USER(out);

FUNC_SET(ain, const timecode_t *, *data);
FUNC_GET(ain, timecode_t);
FUNC_GET_SMPTE(ain);
FUNC_GET_USER(ain);

FUNC_SET(aout, const timecode_t *, *data);
FUNC_GET(aout, timecode_t);
FUNC_GET_SMPTE(aout);
FUNC_GET_USER(aout);

FUNC_SET(preroll, const timecode_t *, *data);
FUNC_GET(preroll, timecode_t);
FUNC_GET_SMPTE(preroll);
FUNC_GET_USER(preroll);

FUNC_SET(speed, int, data);
FUNC_GET(speed, int);
FUNC_GET_USER(speed);

static int p2slave_pkt_size(const uint8_t *data) {
	return (data[0] & 0x0F) + 3;
}

static int p2slave_pkt_tostring(char *str, const uint8_t *data, int max) {
	int i, val, pt = 0, size = p2slave_pkt_size(data);
	for(i = 0; i < size; i++) {
		val = data[i];
		pt += snprintf(str + pt, max, "%02X ", val);
	}
	return pt;
}

static void p2slave_handlepkt_log(const uint8_t *pin, const uint8_t *pout) {
	char in[256];
	char out[256];
	p2slave_pkt_tostring(in, pin, 255);
	p2slave_pkt_tostring(out, pout, 255);
	pinfo("%s -> %s\n", in, out);
	return;
}

/* Pushes the packet onto the buffer */
static void p2slave_pushpkt(struct p2slave_t *dev, const uint8_t *data, int size) {
	if(atomic_read(&dev->opens)) {
		int wp = dev->buffer.write;
		struct p2slave_pkt_t *pkt = &dev->buffer.buf[wp];
		do_gettimeofday(&pkt->tstamp);
		memcpy((void *)pkt->data, (const void *)data, size);
		
		/* Increment the write pointer */
		wp++; if(wp >= P2SLAVE_BUFSIZE) wp = 0;
		dev->buffer.write = wp;

		/* Wake up the buffer wait queue */
		wake_up_interruptible(&dev->wait);
	}
	return;
}

static uint8_t p2slave_checksum(const uint8_t *data, int size) {
	int i;
	uint8_t csum = 0;
	for(i = 0; i < size; i++) csum += *data++;
	return csum;
}

static void p2slave_checksum_add(uint8_t *data) {
	int size = (data[0] & 0x0F) + 2; // This is the size BEFORE the checksum
	uint8_t csum = p2slave_checksum(data, size);
	data[size] = csum;
	return;
}

static int p2slave_checksum_verify(const uint8_t *data) {
	int size = (data[0] & 0x0F) + 2; // This is the size BEFORE the checksum
	uint8_t csum = p2slave_checksum(data, size);
	return(csum != data[size]);
}

#define SETPKT_INT(b1, b2, i1)		pout[0] = b1; \
					pout[1] = b2; \
					pout[2] = i1 & 0xFF; \
					pout[3] = (i1 >> 8) & 0xFF; \
					pout[4] = (i1 >> 16) & 0xFF; \
					pout[5] = (i1 >> 24) & 0xFF; \
					sendack = 0

#define SETPKT_2INT(b1, b2, i1, i2) 	pout[0] = b1; \
					pout[1] = b2; \
					pout[2] = i1 & 0xFF; \
					pout[3] = (i1 >> 8) & 0xFF; \
					pout[4] = (i1 >> 16) & 0xFF; \
					pout[5] = (i1 >> 24) & 0xFF; \
					pout[6] = i2 & 0xFF; \
					pout[7] = (i2 >> 8) & 0xFF; \
					pout[8] = (i2 >> 16) & 0xFF; \
					pout[9] = (i2 >> 24) & 0xFF; \
					sendack = 0

#define SETPKT(obj) 			memcpy(pout, obj, sizeof(obj)); \
					sendack = 0

int p2slave_handle_pkt(struct p2slave_t *dev, uint8_t *pout, const uint8_t *pin) {
	int cmd, size;
	int sendack = 1; /* If this is set, respond with the default ACK */
	int usersend = 1; /* If this is set, the packet will not be forwarded to userspace. */

	dev->stats.count++;

	/* Make sure the packet checksum is good */
	if(p2slave_checksum_verify(pin)) {
		SETPKT(pkt_nak_csum);
		dev->stats.err_crc++;
		return 0;
	}

	/* Ok, the packet is good.  Handle it! */
	cmd = ((pin[0] & 0xF0) << 8) | pin[1];
	size = p2slave_pkt_size(pin);

	/* Catch any commands that need to return data */
	switch(cmd) {
		case 0x0011: { /* Device Type Request */
				     uint16_t id = p2slave_get_id(dev);
				     uint8_t pret[] = {0x12, 0x11, (id >> 8) & 0xFF, id & 0xFF};
				     SETPKT(pret);
			     }
			     usersend = 0;
			     break;

		case 0x600A: { /* Timecode Generator Sense */
				     uint8_t type = pin[2];
				     timecode_smpte_t stc = p2slave_get_smpte_tcgen(dev);
				     switch(type) {
					     case 0x01: /* Timecode */
						     SETPKT_INT(0x74, 0x08, stc.timecode); break;

					     case 0x10: /* Userbits */
						     SETPKT_INT(0x74, 0x09, stc.userbits); break;

					     case 0x11: /* Timecode and Userbits */
						     SETPKT_2INT(0x78, 0x08, stc.timecode, stc.userbits); break;
				     }
			     }
			     usersend = 0;
			     break;

		case 0x600C: { /* Timecode Sense */
				     uint8_t type = pin[2];
				     timecode_smpte_t stc = p2slave_get_smpte_tc(dev);

				     switch(type) {
					     case 0x01: /* LTC Timecode */
					     case 0x03: /* LTC or VITC timecode */
						     SETPKT_INT(0x74, 0x04, stc.timecode); break;

					     case 0x02: /* VITC Timecode */
						     SETPKT_INT(0x74, 0x06, stc.timecode); break;

					     case 0x04: /* Timer1 Timecode */
						     SETPKT_INT(0x74, 0x00, stc.timecode); break;

					     case 0x10: /* LTC Userbits */
					     case 0x30: /* LTC or VITC userbits */
						     SETPKT_INT(0x74, 0x05, stc.userbits); break;

					     case 0x20: /* VITC Userbits */
						     SETPKT_INT(0x74, 0x07, stc.userbits); break;

					     case 0x11: /* LTC Timecode and Userbits */
					     case 0x33: /* LTC or VITC timecode and userbits */
						     SETPKT_2INT(0x78, 0x04, stc.timecode, stc.userbits); break;

					     case 0x22: /* VITC timecode and userbits */
						     SETPKT_2INT(0x78, 0x06, stc.timecode, stc.userbits); break;
				     }
			     }
			     usersend = 0;
			     break;

		case 0x6010: { /* In Data Sense */
				     timecode_smpte_t stc = p2slave_get_smpte_in(dev);
				     SETPKT_INT(0x74, 0x10, stc.timecode);
			     }
			     usersend = 0;
			     break;

		case 0x6011: { /* Out Data Sense */
				     timecode_smpte_t stc = p2slave_get_smpte_out(dev);
				     SETPKT_INT(0x74, 0x11, stc.timecode);
			     }
			     usersend = 0;
			     break;

		case 0x6012: { /* Audio In Data Sense */
				     timecode_smpte_t stc = p2slave_get_smpte_ain(dev);
				     SETPKT_INT(0x74, 0x12, stc.timecode);
			     }
			     usersend = 0;
			     break;

		case 0x6013: { /* Audio Out Data Sense */
				     timecode_smpte_t stc = p2slave_get_smpte_aout(dev);
				     SETPKT_INT(0x74, 0x13, stc.timecode);
			     }
			     usersend = 0;
			     break;

		case 0x6020: { /* Status Sense */
				     struct p2slave_status_t status = p2slave_get_status(dev);
				     uint8_t start = (pin[2] & 0xF0) >> 4;
				     uint8_t len = (pin[2] & 0x0F);
				     pout[0] = 0x70 | len;
				     pout[1] = 0x20;
				     memcpy((void *)pout + 2, (const void *)&status + start, len);
				     sendack = 0;
			     }
			     /* Always send the status sense packets to userspace to make sure
			      * the userspace program knows to update the status */
			     usersend = 1;
			     break;

		case 0x602E: { /* Command Speed Sense */
				     pout[0] = 0x71;
				     pout[1] = 0x2E;
				     pout[2] = p2slave_get_speed(dev);
				     sendack = 0;
			     }
			     usersend = 0;
			     break;

		case 0x6031: { /* Preroll sense */
				     timecode_smpte_t stc = p2slave_get_smpte_preroll(dev);
				     SETPKT_INT(0x74, 0x31, stc.timecode);
			     }
			     usersend = 0;
			     break;

		case 0x6036: { /* Timer Mode Sense */
				     pout[0] = 0x71;
				     pout[1] = 0x36;
				     pout[2] = 0x00; /* Timecode */
				     sendack = 0;
			     }
			     usersend = 0;
			     break;

		/* Edit On/Off are special cases.  We modify the packet we are going to send to userspace
		 * to include the current timecode.  This is used to ensure that the edit timing is correct */
		case 0x2064: { /* Edit On */
				     timecode_smpte_t 	stc = p2slave_get_smpte_tc(dev);
				     uint8_t 		npkt[8];
				     
				     npkt[0] = 0x24;
				     npkt[1] = 0x64;
				     npkt[2] = stc.timecode & 0xFF;
				     npkt[3] = (stc.timecode >> 8) & 0xFF;
				     npkt[4] = (stc.timecode >> 16) & 0xFF;
				     npkt[5] = (stc.timecode >> 24) & 0xFF;
				     
				     /* Now push the new packet to the user */
				     p2slave_pushpkt(dev, npkt, 6);
			     }
			     usersend = 0;
			     sendack = 1;
			     break; 

		case 0x2065: { /* Edit Off */
				     timecode_smpte_t 	stc = p2slave_get_smpte_tc(dev);
				     uint8_t 		npkt[8];
				     
				     npkt[0] = 0x24;
				     npkt[1] = 0x65;
				     npkt[2] = stc.timecode & 0xFF;
				     npkt[3] = (stc.timecode >> 8) & 0xFF;
				     npkt[4] = (stc.timecode >> 16) & 0xFF;
				     npkt[5] = (stc.timecode >> 24) & 0xFF;
				     
				     /* Now push the new packet to the user */
				     p2slave_pushpkt(dev, npkt, 6);
			     }
			     usersend = 0;
			     sendack = 1;
			     break;
	}
	
	/* If sendack is still set, just respond with and ACK */
	if(sendack) SETPKT(pkt_ack);
	
	/* Push the packet to userspace */
	if(usersend) p2slave_pushpkt(dev, pin, size);

	/* Add the checksum to the outgoing packet */
	p2slave_checksum_add(pout);
	if(dev->flags & P2S_Debug) p2slave_handlepkt_log(pin, pout);
	return 0;
}
EXPORT_SYMBOL(p2slave_handle_pkt);

static struct p2slave_t *p2slave_allocate(const char *name) {
	int i;
	struct p2slave_t *dev = (struct p2slave_t *)kzalloc(sizeof(struct p2slave_t), GFP_KERNEL);
	if(dev == NULL) {
		perror("Failed to allocate device for %s\n", name);
		return NULL;
	}
	strncpy(dev->name, name, sizeof(dev->name));
	spin_lock_init(&dev->datalock);
	init_waitqueue_head(&dev->wait);

	spin_lock(&spinlock);
	for(i = 0; i < MAX_DEVS; i++) {
		if(devs[i] == NULL) {
			dev->index = i;
			devs[i] = dev;
			break;
		}
	}
	spin_unlock(&spinlock);

	if(i == MAX_DEVS) {
		perror("Failed to find a free device slot for %s\n", name);
		kfree(dev);
		return NULL;
	}
	return dev;
}

static void p2slave_free(struct p2slave_t *dev) {
	spin_lock(&spinlock);
	devs[dev->index] = NULL;
	spin_unlock(&spinlock);
	kfree(dev);
	return;
}

static void p2slave_buffer_reset(struct p2slave_t *dev) {
	dev->buffer.read = 0;
	dev->buffer.write = 0;
	return;
}

static int p2slave_open(struct inode *inode, struct file *filp) {
	struct p2slave_t *dev = NULL;
	int minor = iminor(inode);

	dev = devs[minor];
	if(dev == NULL) return -ENODEV;
	
	filp->private_data = dev;
	p2slave_buffer_reset(dev);
	atomic_inc(&dev->opens);
	return 0;
}

static int p2slave_release(struct inode *inode, struct file *filp) {
	struct p2slave_t *dev = (struct p2slave_t *)filp->private_data;
	atomic_dec(&dev->opens);
	return 0;
}

static ssize_t p2slave_read(struct file *filp, char __user *buf, size_t count, loff_t *f_pos) {
	struct p2slave_t 	*dev = (struct p2slave_t *)filp->private_data;
	int 			rp, total = 0;

	/* Wait until we have data in the buffer */
	while(dev->buffer.read == dev->buffer.write) {
		if(filp->f_flags & O_NONBLOCK) return -EAGAIN;
		if(wait_event_interruptible(dev->wait, dev->buffer.read != dev->buffer.write))
			return -ERESTARTSYS;
	}

	rp = dev->buffer.read;
	while(rp != dev->buffer.write) {
		/* Make sure there is enough room */
		if(total + sizeof(struct p2slave_pkt_t) > count) break;
		if(copy_to_user(buf, &dev->buffer.buf[rp], sizeof(struct p2slave_pkt_t))) return -EIO;
		total += sizeof(struct p2slave_pkt_t);
		buf += sizeof(struct p2slave_pkt_t);
		rp++; if(rp >= P2SLAVE_BUFSIZE) rp = 0;
	}
	dev->buffer.read = rp;
	if(!total) perror("Read is returning 0.\n");
	return total;
}

static int p2slave_ioctl(struct inode *inode, struct file *filp, unsigned int cmd, unsigned long arg) {
	struct p2slave_t 	*dev = (struct p2slave_t *)filp->private_data;
	int			ret = 0;

	switch(cmd) {
		case P2SLAVE_IOC_SETID:		ret = p2slave_get_user_id(dev, arg); break;
		case P2SLAVE_IOC_SETSTATUS:	ret = p2slave_get_user_status(dev, arg); break;
		case P2SLAVE_IOC_SETTCGEN:	ret = p2slave_get_user_tcgen(dev, arg); break; 
		case P2SLAVE_IOC_SETTC:		ret = p2slave_get_user_tc(dev, arg); break;
		case P2SLAVE_IOC_SETIN:		ret = p2slave_get_user_in(dev, arg); break; 
		case P2SLAVE_IOC_SETOUT:	ret = p2slave_get_user_out(dev, arg); break; 
		case P2SLAVE_IOC_SETAIN:	ret = p2slave_get_user_ain(dev, arg); break; 
		case P2SLAVE_IOC_SETAOUT:	ret = p2slave_get_user_aout(dev, arg); break; 
		case P2SLAVE_IOC_SETPREROLL:	ret = p2slave_get_user_preroll(dev, arg); break;
		case P2SLAVE_IOC_SETSPEED:	ret = p2slave_get_user_speed(dev, arg); break;

		default: return -ENOTTY; break;
	}
	return ret;
}

unsigned int p2slave_poll(struct file *filp, poll_table *wait) {
	struct p2slave_t 	*dev = (struct p2slave_t *)filp->private_data;
	unsigned int		mask = 0;
	
	/* Add the poll to the device's wait queue */
	poll_wait(filp, &dev->wait, wait);

	/* Let the poll know if we can read */
	if (dev->buffer.read != dev->buffer.write) 
		mask |= POLLIN | POLLRDNORM;

	return mask;
}

struct file_operations p2slave_fops = {
         .owner =	THIS_MODULE,
	 .open =	p2slave_open,
         .release =	p2slave_release,
         .read =	p2slave_read,
	 .ioctl = 	p2slave_ioctl,
	 .poll =	p2slave_poll
};

static ssize_t p2slave_show_name(struct device *dev, struct device_attribute *attr, char *buf) {
	struct p2slave_t *p2s = (struct p2slave_t *)dev_get_drvdata(dev);
	return snprintf(buf, PAGE_SIZE, "%s\n", p2s->name);
}
DEVICE_ATTR(name, 0444, p2slave_show_name, NULL);

static ssize_t p2slave_show_stats(struct device *dev, struct device_attribute *attr, char *buf) {
	struct p2slave_t *p2s = (struct p2slave_t *)dev_get_drvdata(dev);
	return snprintf(buf, PAGE_SIZE, 
			"Total pkts: %d\n"
			"CRC Errors: %d\n",
			p2s->stats.count, 
			p2s->stats.err_crc
		);
}
DEVICE_ATTR(stats, 0444, p2slave_show_stats, NULL);


void p2slave_unregister(struct p2slave_t *dev) {
	if(dev == NULL) return;
	if(dev->device != NULL) {
		device_remove_file(dev->device, &dev_attr_name);
		device_remove_file(dev->device, &dev_attr_stats);
		DEVICE_DESTROY(class, dev->device->devt);
	}
	cdev_del(&dev->cdev);
	p2slave_free(dev);
	return;
}
EXPORT_SYMBOL(p2slave_unregister);

struct p2slave_t *p2slave_register(const char *name) {
	int 			error = 0;
	struct p2slave_t 	*dev;
	dev_t			devt;
	char 			buf[32];

	dev = p2slave_allocate(name);
	if(dev == NULL) {
		perror("Failed to allocate device for %s\n", name);
		goto regfail;
	}
	devt = MKDEV(MAJOR(chrdev), dev->index);

	snprintf(buf, sizeof(buf), "p2slave%d", dev->index);
	dev->device = DEVICE_CREATE(class, NULL, devt, buf);
	if(!dev->device || IS_ERR(dev->device)) {
		perror("Failed to create device for %s: %ld\n", name, PTR_ERR(dev->device));
		dev->device = NULL;
		goto regfail;
	}
	DEVICE_SET_DATA(dev->device, dev);
	device_create_file(dev->device, &dev_attr_name);
	device_create_file(dev->device, &dev_attr_stats);

	cdev_init(&dev->cdev, &p2slave_fops);
	dev->cdev.owner = THIS_MODULE;
	error = cdev_add(&dev->cdev, devt, 1);
	if(error) {
		perror("cdev_add failed with %d for %s\n", error, name);
		goto regfail;
	}

	pinfo("Register %s as device (%d, %d)\n", name, MAJOR(chrdev), dev->index);
	return dev;

regfail:
	p2slave_unregister(dev);
	return NULL;
}
EXPORT_SYMBOL(p2slave_register);

static void p2slave_cleanup(void) {
	if(chrdev) unregister_chrdev_region(chrdev, 256);
	if(class != NULL) CLASS_DESTROY(class);
	return;
}

int p2slave_init(void) {
	int error = 0;

	memset(devs, 0, sizeof(devs));
	pinfo("Slave interface for the Sony P2 protocol, API version %d\n", P2SLAVE_API_VERSION);
	pinfo("Copyright 2008 - SpectSoft <http://www.spectsoft.com>\n");

	class = CLASS_CREATE(THIS_MODULE, MODNAME);
	if(class == NULL) {
		error = -ENODEV;
		perror("Failed to create class\n");
		goto initfail;
	}

	error = alloc_chrdev_region(&chrdev, 0, 256, MODNAME);
	if(error) {
		perror("Failed to allocate char device with %d\n", error);
		goto initfail;
	}
	return 0;

initfail:
	p2slave_cleanup();
	return error;
}	

void p2slave_exit(void) {
	p2slave_cleanup();
	return;
}

/* Declare the module initialization and exit points */
module_init(p2slave_init);
module_exit(p2slave_exit);



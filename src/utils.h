/*******************************************************************************
 * utils.h
 *
 * Creation Date: April 20, 2010
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
 * DESC: Utility macros
 *
 ******************************************************************************/
 
#ifndef _UTILS_H_
#define _UTILS_H_

/* pdebug is for DEBUG statements ONLY! */
#ifdef _DEBUG_
#	define pdebug(fmt, args...) printk(KERN_DEBUG MODNAME ":%d: " fmt, __LINE__, ## args)
#else
#	define pdebug(fmt, args...)
#endif 

/* perror is for ERRORS only! */
#define perror(fmt, args...) printk(KERN_ERR MODNAME ":%d: " fmt, __LINE__, ## args)

/* pinfo is for general information */
#define pinfo(fmt, args...) printk(KERN_INFO MODNAME ":%d: " fmt, __LINE__, ## args)

#endif /* ifndef _UTILS_H_ */
 

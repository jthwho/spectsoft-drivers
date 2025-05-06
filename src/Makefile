#
# SpectSoft linux kernel modules makefile
#
#

ifneq ($(KERNELRELEASE),)

# Build in debugging support if the BUILD_DEBUG environment variable is set to 1
# You can do this with the following shell command:
# export BUILD_DEBUG=1
# 

ifeq (${BUILD_DEBUG}, 1) 
EXTRA_CFLAGS 	+= -g -D_DEBUG_ -D_BENCHMARK_
endif

EXTRA_CFLAGS 	+= -I$(src)/../include -I$(src)
obj-m 		:= p2slave.o ajadriver.o timecodeio.o pcitc.o
ajadriver-objs  := aja.o timecode.o
pcitc-objs 	:= pcitc_main.o timecode.o
p2slave-objs 	:= p2slave_main.o timecode.o
timecodeio-objs := tcio.o timecode.o

else
KREL 		:= $(shell uname -r)
KDIR 		:= /lib/modules/$(KREL)/build
PWD 		:= $(shell pwd)

ifeq (${BUILD_DEBUG}, 1) 
DEBUG_FLAG = -DEBUG
endif
		
.PHONY: all clean insmod rmmod bmod install regcode

all: modules

modules: aja.c p2slave_main.c pcitc_main.c
	@echo "Building kernel modules..."
	@if [ $$BUILD_DEBUG ]; then echo -e '\n#################### !!BUILDING DEBUG VERSION!! ####################\n'; fi
	@$(MAKE) -C $(KDIR) SUBDIRS=$(PWD) modules

clean:
	@echo "Cleaning kernel modules..."
	@$(MAKE) -C $(KDIR) SUBDIRS=$(PWD) clean
	@rm -f Module.symvers

insmod:
	-insmod ./pcitc.ko
	-insmod ./p2slave.ko
	-insmod ./ajadriver.ko

rmmod:
	-rmmod ajadriver
	-rmmod p2slave
	-rmmod pcitc

bmod: rmmod insmod

endif


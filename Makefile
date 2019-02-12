#
# Makefile for the linux NOVA filesystem routines.
#
KERNELDIR ?= /lib/modules/$(shell uname -r)/build
obj-m += nova.o

nova-y := balloc.o bbuild.o dax.o dir.o file.o inode.o ioctl.o journal.o namei.o stats.o super.o symlink.o sysfs.o wprotect.o

all:
	make -C $(KERNELDIR) M=`pwd`

clean:
	make -C $(KERNELDIR) M=`pwd` clean

obj-m += UPS_powermod.o

KERN_VER=$(shell uname -r)

all:
		make -C /lib/modules/$(KERN_VER)/build M=$(shell pwd) modules
		gcc UPS_comm.c -o UPS_comm

clean:
		rm -f *.cmd *.ko *.o Module.symvers modules.order *.mod.c
		rm UPS_comm

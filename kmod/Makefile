obj-m += predatortune_fan.o

KDIR := /lib/modules/$(shell uname -r)/build

all:
	make -C $(KDIR) M=$(PWD) modules

clean:
	make -C $(KDIR) M=$(PWD) clean

install: all
	sudo insmod predatortune_fan.ko

uninstall:
	sudo rmmod predatortune_fan

.PHONY: all clean install uninstall

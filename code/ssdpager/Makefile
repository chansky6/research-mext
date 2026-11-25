obj-m := ssdpager.o
KDIR ?= /lib/modules/$(shell uname -r)/build

all: modules sptest

modules:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

sptest: sptest.c ssdpager.h
	$(CC) -O2 -Wall -o $@ sptest.c -lpthread

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
	rm -f sptest

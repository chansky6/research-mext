obj-m := ssdpager.o
KDIR ?= /lib/modules/$(shell uname -r)/build

all: modules sptest spd bench/mcloc bench/mctrace

modules:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

sptest: sptest.c ssdpager.h
	$(CC) -O2 -Wall -o $@ sptest.c -lpthread

spd: spd.c ssdpager.h
	$(CC) -O2 -Wall -o $@ spd.c -lpthread

bench/mcloc: bench/mcloc.c
	$(CC) -O2 -Wall -o $@ bench/mcloc.c -lpthread

bench/mctrace: bench/mctrace.c
	$(CC) -O2 -Wall -o $@ bench/mctrace.c -lpthread

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
	rm -f sptest spd bench/mcloc bench/mctrace

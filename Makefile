KDIR ?= /lib/modules/`uname -r`/build

default: pierdolnij_ioctl
	$(MAKE) -C $(KDIR) M=$(PWD)/build src=$(PWD)/src

install:
	$(MAKE) -C $(KDIR) M=$(PWD)/build src=$(PWD)/src modules_install

clean:
	$(MAKE) -C $(KDIR) M=$(PWD)/build src=$(PWD)/src clean
	rm pierdolnij_ioctl -f

pierdolnij_ioctl: pierdolnij_ioctl.c
	gcc pierdolnij_ioctl.c -o pierdolnij_ioctl -Wall -Wextra

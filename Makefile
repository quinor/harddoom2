KDIR ?= /lib/modules/`uname -r`/build

default:
	$(MAKE) -C $(KDIR) M=$(PWD)/build src=$(PWD)/src

install:
	$(MAKE) -C $(KDIR) M=$(PWD)/build src=$(PWD)/src modules_install

clean:
	$(MAKE) -C $(KDIR) M=$(PWD)/build src=$(PWD)/src clean

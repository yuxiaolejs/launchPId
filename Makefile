
REPO_ROOT := $(abspath .)

all: main.bin

deps:
	$(MAKE) -C $(REPO_ROOT)/lib
	$(MAKE) -C $(REPO_ROOT)/engine
	$(MAKE) -C $(REPO_ROOT)/pos

$(REPO_ROOT)/host/xfb.host.elf: deps
	$(MAKE) -C $(REPO_ROOT)/host

$(REPO_ROOT)/pos/main.bin: deps
	$(MAKE) -C $(REPO_ROOT)/pos

main.bin: $(REPO_ROOT)/pos/main.bin
	cp $< $@


clean:
	$(MAKE) -C $(REPO_ROOT)/lib clean
	$(MAKE) -C $(REPO_ROOT)/pos clean
	$(MAKE) -C $(REPO_ROOT)/engine clean
	rm -f host.elf
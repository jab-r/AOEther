.PHONY: all talker receiver clean

all: talker receiver

talker:
	$(MAKE) -C talker

receiver:
	$(MAKE) -C receiver

clean:
	$(MAKE) -C talker clean
	$(MAKE) -C receiver clean

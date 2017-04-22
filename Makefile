/=./

include config.mk

MAKEFLAGS += --no-print-directory

all: libs build

libs: lib/all.a

lib/all.a:
	$(MAKE) -C lib

build: lib/all.a
	$(MAKE) -C src rec

install:
	$(MAKE) -C src install

clean: clean-lib clean-src

clean-src:
	$(MAKE) -C src clean-rec

clean-lib:
	$(MAKE) -C lib clean

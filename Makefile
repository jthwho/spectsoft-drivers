
.PHONY: all clean getdeps insmod rmmod ajareg distclean

all: getdeps
	cd src && make

clean:
	cd src && make clean

getdeps:
	if [ -d ../basecode ]; then cp -f ../basecode/timecode.* src; fi

insmod:
	insmod ./src/p2slave.ko
	insmod ./src/pcitc.ko
	insmod ./src/ajadriver.ko

rmmod:
	rmmod ajadriver
	rmmod p2slave
	rmmod pcitc

ajareg:
	cd extras && ./registers.pl strings > ../include/aja_regstrings.h
	cd extras && ./registers.pl header > ../include/aja_registers.h

distclean: clean
	

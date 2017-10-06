#!/bin/sh
export PATH=/sbin:/usr/sbin:/bin:/usr/bin
gpsirq=`setserial /dev/ttyS2 | sed 's/.* //'`
if [ "$gpsirq" = "$2" ]
then
	echo "ban=true"
	# interrupt handling core number is (without leading 0x): 1H << core
	echo 8 > /proc/irq/$2/smp_affinity
else
	echo "ban=false"
fi

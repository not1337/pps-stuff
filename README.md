                        Linux Pulse Per Second Stuff

                         (c) 2017 Andreas Steinmetz

-------------------------------------------------------------------------

This is all about getting precision timing from a serial line PPS signal.

For this stuff to be of any use you first need a GPS receiver with a
PPS output and a host system with a real (i.e. no USB) RS232 interface
(at least 4wire which means RxD, TxD, RTS and CTS) as well as a not
too historic kernel.

Personally I'm using a Navilock NL-8005P with a Sub-D9 adapter. In my
experience this u-blox 8 based GPS receiver is sensitive enough for
indoor (near window) use.

Though the communication of this receiver is RS232 level, the PPS output
is open drain. To convert this to RS232 you have to heat up your soldering
iron and create a little gadget based on a MAX233A (if you can't get
one a MAX233 will do) like shown in the PNG image. With this gadget
you convert 3.3V or 5V logic based open drain or open collector output
to RS232 level.

Now if you're unlucky, chances are your serial interface is not fully
wired and missing the DCD input which is the kernel's PPS source.
With the supplied kernel patch you can use CTS instead of DCD.
CTS should be available even on crippled interfaces.
Note that the provided patch is against Linux 4.12. It is preliminary
and needs to be reworked before any attempt of kernel inclusion but
in my opinion providing a working preliminary patch is better than
nothing at all.

Assuming that your kernel is properly configured with PPS support and
that your GPS device is attached to /dev/ttyS2 you now can create a
basic configuration for chronyd (/etc/chrony/chrony.conf):

refclock SOCK /run/chrony.ttyS2.sock refid GPS poll 5 filter 80 precision 1e-9

corrtimeratio 3.2

Then first start chronyd followed by gpsd:

chronyd -f /etc/chrony/chrony.conf

gpsd -n -D 0 -F /run/gpsd.sock -P /run/gpsd.pid /dev/ttyS2

If you look at the output of "chronyc sources" after a few minutes of
stabilization you will be disappointed. Time precision will be well
and far worse than 1us, even more like 10us. This is due to the
aggressive power management of modern processors. The typical answer
to this problem is to completely disable power management which,
however, causes excessive heat and an ugly power bill. If you
can accept a precision which is not optimal but quite close to
it there is a power saving alternative.

"unidled", which is included here, switches either all cores or, if
the intel_idle driver is in use, a single core to poll mode for 0.2%
of the time and let the power management handle the remaining 99.8%.
This keeps the CPU cool and your power bill healthy. For an initial
test after compiling unidled (see the comment in the source) just
start unidled after chronyd and gpsd:

unidled -f /run/unidled.pid -d /dev/ttyS2 -a

Check, that unidled is running and wait again a few minutes for chrony
to settle. You should now find that the precision displayed by
"chronyc sources" is quite better than 1us, typically you should
see a precision better than 500ns.

If things work out well and you now are a happy camper, you can start
to refine your setup e.g. by using realtime priorities for gpsd and
chronyd (the realtime priority for unidled must be higher than those).
In case of a CPU that uses the intel_idle driver you can configure
the serial device interrupt as well as unidled to be served by the
same core (beware of irqbalance) and remove the "-a" option for unidled
(you may have to assert that chronyd as well as gpsd run on the
same core, too). This way, only one core will have the forced 0.2% of
poll time.

In case you're using irqbalance have a look at the provided script (you
at least need to modify the core selection). Start irqbalance with
"-l path-to-script" and irqbalance will not mess with the serial line
interrupt.

Note that you will have to configure chrony's temperature compensation
to prevent a wide frequency adjustment range which in turn causes loss
of precision (to be documented soon).

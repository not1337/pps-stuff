                        Linux Pulse Per Second Stuff

                         (c) 2017 Andreas Steinmetz

-------------------------------------------------------------------------

This is all about getting precision timing from a serial line PPS signal.

For this stuff to be of any use you first need a GPS receiver with a
PPS output and a host system with a real (i.e. no USB) RS232 interface
(at least 4wire which means RxD, TxD, RTS and CTS) as well as a not
too historic kernel. Preferably the system should have NICs with
hardware timestamping support.

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
the intel\_idle driver is in use, a single core to poll mode for 0.2%
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
In case of a CPU that uses the intel\_idle driver you can configure
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
of precision. You need to get a list of temperature and frequency offset
pairs. You can use the provided "heatppm" utility for this.

First have a look at what clock sources are in use by looking at the
contents of /sys/devices/system/clocksource/clocksource\*/current\_clocksource.
Then look at the contents of /sys/class/hwmon/hwmon\*/name to check
which temperature measurement sources are available and check the
jitter of the temperatures provided by /sys/class/hwmon/hwmon\*/temp\*\_input.
In my case with TSC being the clock source I decided to use the
temperature provided by "acpitz" which shows quite less jitter than
"coretemp".

After having selected the clock source make sure that the system is
as idle as possible while keeping chronyd, gpsd and unidled running.
Let the system cool down as far as possible, then start heatppm,
e.g. as (adapt the temperature source!):

    heatppm -t /sys/class/hwmon/hwmon0/temp1_input

Wait for several hours. heatppm will slowly but steadily output a list
of temperature and frequency offset pairs. When either heatppm terminates
or you interrupt heatppm, copy and paste the generated list to a file.
In the following I'll assume it is named "tempcomp.txt".

You then can either create a frequency offset correction file from the
list or you can generate the parameters for the frequency offset
correction function. I'll describe the latter.

Make sure that you have gnuplot as well as tempcomp.txt available on
an arbitrary system. Start gnuplot. Issue the following commands
(replace "value-of-row1-column1-of-tempcomp.txt" with the actual value):

    f1(x) = k0+(x-t0)*k1+(x-t0)*(x-t0)*k2
    t0=value-of-row1-column1-of-tempcomp.txt
    fit f1(x) 'tempcomp.txt' using 1:2 via k0, k1, k2

From the generated output you need the "Final set of parameters".
Ignore k0 but remember the k1 and k2 values. To get visible data you
can now issue:

    plot "tempcomp.txt" using 1:2,k0+(x-t0)*k1+(x-t0)*(x-t0)*k2

When you're satisfied, creating a temperature compensation configuration
for chronyd is easy:

    tempcomp /sys/class/hwmon/hwmon0/temp1_input 1 t0 0.0 -k1 -k2

t0, k1 and k2 are the values entered into or generated by gnuplot. Note
the sign inversion for k1 and k2!

Another point: if you have hardware timestamping available, use it! Probably
most if not all onboard NICs supported by the "igb" or "e1000e" drivers
should support hardware timestamping. For other NICs look for "IEEE 1588"
advertisements as well as linux hardware timestamping support.

If your time server doesn't support NIC hardware timestamping, stop here and
use NTP to distribute time in yiur LAN.

If, however, your time server does support NIC hardware timestamping and you
want higher precision time in your LAN, read on.

To distribute the precision time in your LAN use PTP and thus have a look at
"linuxptp" and "ptpd2" (just google for it). You will probably need both.
Now, PTP as well as GPS actually use TAI time. TAI is effectively UTC without
any leap seconds applied. As for GPS the signal contains a TAI to UTC offset
which is why you get UTC time from a GPS receiver by default. PTP, however,
needs either to be corrected for every leap second or has to be configured
to use UTC instead of TAI. Well, I'm lazy and don't want to track leap
second changes. So I'm going to use UTC for PTP and describe that here.

The utilities you need on your time server are "phc2sys" and "ptp4l" which
are both part of linuxptp. First you need to run php2sys as a kind of sys2phc,
i.e. you make your NIC clocks follow your system clock. For every NIC you
need to start a phc2sys instance (replace "eth0" with your NIC device):

    phc2sys -s CLOCK_REALTIME -c eth0 -O 0 -R 10 -N 2 -E linreg -L 50000000 -n 0 -l 0 -q -m

Run phc2sys with realtime privilege, otherwise it will spill errors sooner
or later as it seemingly can't handle longer scheduling delays.

Then you have to distribute the time using the PTP protocol aka IEEE 1588.
To do so, create a configuration file for ptp4l, let's assume you use
/etc/ptp4l.conf:

    [global]
    transportSpecific 0x0
    delay_mechanism E2E
    network_transport L2
    gmCapable 1
    slaveOnly 0
    priority1 64
    priority2 64
    # clock class 13 means application specific, leave as is
    clockClass 13
    # use one of the following values for clock accuracy:
    # 0x20 25ns    0x24 2.5us    0x28 250us    0x2c 25ms    0x30 10s
    # 0x21 100ns   0x25 10us     0x29 1ms      0x2d 100ms   0x31 more than 10s
    # 0x22 250ns   0x26 25us     0x2a 2.5ms    0x2e 250ms   0xfe unknown
    # 0x23 1us     0x27 100us    0x2b 10ms     0x2f 1s
    clockAccuracy 0x22
    domainNumber 0
    free_running 0
    udp6_scope 0x05
    dscp_event 46
    dscp_general 34
    logging_level 0
    verbose 0
    use_syslog 0
    # use one of the following for time source:
    # 0x10 atomic clock  0x30 terrestrial radio  0x50 NTP       0x90 other
    # 0x20 GPS           0x40 PTP                0x60 hand set  0xa0 int. oscillator
    timeSource 0x20
    time_stamping hardware
    boundary_clock_jbod 0
    [eth0]
    hybrid_e2e 1

Add a section for every interface you may want to distribute time using PTP.
If you use VLANs use the VLAN interfaces , e.g. [eth1.5]. Add the "hybrid\_e2e 1"
to every interface section. Then start ptp4l as:

    ptp4l -f /etc/ptp4l.conf

Running ptp4l with realtime privilege doesn't hurt, either.

Now you have a time server distributing NTP time via chronyd (presuming proper
NTP configuration) as well as PTP time via the linuxptp tools. Where NTP time
is quite coarse and a fallback, PTP time is the precision time you want to
use in your LAN.

Next is PTP client configuration, to be documented soon.

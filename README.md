# SpectSoft RaveHD Linux Kernel Drivers

These are quite old and probably won't work without heavy modification for modern
kernels.  They're here mostly for historical and curiosity purposes.

There's basically three drivers here:

1. A driver for the [AJA](https://aja.com) line of PCI and PCIe video I/O boards.  All the boards 
supported by this driver have gone end-of-life.  If you're looking for a driver 
+ SDK for the modern boards, please check out the 
[libajantv2 GitHub repo](https://github.com/aja-video/libajantv2)

2. A driver for the [Adrienne Electronics](http://www.adrielec.com/) PCI and PCIe
LTC timecode boards.

3. A driver that implemented the device/VTR side of the [Sony Machine Control](https://en.wikipedia.org/wiki/9-Pin_Protocol)
protocol.  Sometimes this is called other names like: 9-Pin Protocol, Sony P2, etc.  It's a protocol
used by video tape machines to control them via RS-422.  The reason we implemented it in the
kernel was actually one of the reasons our disk recorder was so successful (i.e. could emulate a
VTR well): it allowed us to adhere to the tight timing requirements of the protocol as well
as modify the video playback all in kernel space.  This made things like TSO (tape speed
override possible) emulation possible, which allowed controllers to sync up our disk recorder with
another tape machine, telecine, disk recorder, etc.



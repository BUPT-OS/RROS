Kernel driver ftsteutates
=========================

Supported chips:

  * FTS Teutates

    Prefix: 'ftsteutates'

    Addresses scanned: I2C 0x73 (7-Bit)

Author: Thilo Cestonaro <thilo.cestonaro@ts.fujitsu.com>


Description
-----------

The BMC Teutates is the Eleventh generation of Superior System
monitoring and thermal management solution. It is builds on the basic
functionality of the BMC Theseus and contains several new features and
enhancements. It can monitor up to 4 voltages, 16 temperatures and
8 fans. It also contains an integrated watchdog which is currently
implemented in this driver.

The ``pwmX_auto_channels_temp`` attributes show which temperature sensor
is currently driving which fan channel. This value might dynamically change
during runtime depending on the temperature sensor selected by
the fan control circuit.

The 4 voltages require a board-specific multiplier, since the BMC can
only measure voltages up to 3.3V and thus relies on voltage dividers.
Consult your motherboard manual for details.

To clear a temperature or fan alarm, execute the following command with the
correct path to the alarm file::

	echo 0 >XXXX_alarm

Specifications of the chip can be found at the `Kontron FTP Server <http://ftp.kontron.com/>`_ (username = "anonymous", no password required)
under the following path:

  /Services/Software_Tools/Linux_SystemMonitoring_Watchdog_GPIO/BMC-Teutates_Specification_V1.21.pdf

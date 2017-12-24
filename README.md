# RaspiUPS-Tinker

Update. As it stands, the code works but doesnt display in the
Note. this is still in development and I'm not a programmer by trade so bear with me if there are issues, or help me out if you can.
Board: http://www.raspberrypiwiki.com/index.php/Raspi_UPS_HAT_Board

The above board uses the i2c1 interface on the Tinkerboard to send battery level back to the Android Build of TinkerOS.

# Installation
1. Setup your Build Environment as per https://tinkerboarding.co.uk/wiki/index.php?title=Software#How_to_build_Kernel_source_code
2. Copy raspiups_battery.c to {android_kernel}/driver/power
3. In the same folder edit the Kconfig file with an entry as per below.
config RASPIUPS_BATTERY
      tristate "RaspiUPS Battery HAT"
      default y
      help
        say Y to enable support for the RaspiUPS HAT
4. In Same folder, edit the Makefile to add a line "obj-$(CONFIG_RASPIUPS_BATTERY)  += raspiups_battery.o"
5. Then edit the devicetree file "{android_kernel}/arch/arm/boot/dts/rk3288-miniarm.dts" to add the below in the i2c1 entry
&i2c1 {
      battery@36 {
		compatible = "MAX17048";
		reg = <0x36>;
	};
};

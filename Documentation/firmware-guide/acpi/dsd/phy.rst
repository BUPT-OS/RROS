.. SPDX-License-Identifier: GPL-2.0

=========================
MDIO bus and PHYs in ACPI
=========================

The PHYs on an MDIO bus [phy] are probed and registered using
fwnode_mdiobus_register_phy().

Later, for connecting these PHYs to their respective MACs, the PHYs registered
on the MDIO bus have to be referenced.

This document introduces two _DSD properties that are to be used
for connecting PHYs on the MDIO bus [dsd-properties-rules] to the MAC layer.

These properties are defined in accordance with the "Device
Properties UUID For _DSD" [dsd-guide] document and the
daffd814-6eba-4d8c-8a91-bc9bbf4aa301 UUID must be used in the Device
Data Descriptors containing them.

phy-handle
----------
For each MAC node, a device property "phy-handle" is used to reference
the PHY that is registered on an MDIO bus. This is mandatory for
network interfaces that have PHYs connected to MAC via MDIO bus.

During the MDIO bus driver initialization, PHYs on this bus are probed
using the _ADR object as shown below and are registered on the MDIO bus.

.. code-block:: none

      Scope(\_SB.MDI0)
      {
        Device(PHY1) {
          Name (_ADR, 0x1)
        } // end of PHY1

        Device(PHY2) {
          Name (_ADR, 0x2)
        } // end of PHY2
      }

Later, during the MAC driver initialization, the registered PHY devices
have to be retrieved from the MDIO bus. For this, the MAC driver needs
references to the previously registered PHYs which are provided
as device object references (e.g. \_SB.MDI0.PHY1).

phy-mode
--------
The "phy-mode" _DSD property is used to describe the connection to
the PHY. The valid values for "phy-mode" are defined in [ethernet-controller].

managed
-------
Optional property, which specifies the PHY management type.
The valid values for "managed" are defined in [ethernet-controller].

fixed-link
----------
The "fixed-link" is described by a data-only subnode of the
MAC port, which is linked in the _DSD package via
hierarchical data extension (UUID dbb8e3e6-5886-4ba6-8795-1319f52a966b
in accordance with [dsd-guide] "_DSD Implementation Guide" document).
The subnode should comprise a required property ("speed") and
possibly the optional ones - complete list of parameters and
their values are specified in [ethernet-controller].

The following ASL example illustrates the usage of these properties.

DSDT entry for MDIO node
------------------------

The MDIO bus has an SoC component (MDIO controller) and a platform
component (PHYs on the MDIO bus).

a) Silicon Component
This node describes the MDIO controller, MDI0
---------------------------------------------

.. code-block:: none

	Scope(_SB)
	{
	  Device(MDI0) {
	    Name(_HID, "NXP0006")
	    Name(_CCA, 1)
	    Name(_UID, 0)
	    Name(_CRS, ResourceTemplate() {
	      Memory32Fixed(ReadWrite, MDI0_BASE, MDI_LEN)
	      Interrupt(ResourceConsumer, Level, ActiveHigh, Shared)
	       {
		 MDI0_IT
	       }
	    }) // end of _CRS for MDI0
	  } // end of MDI0
	}

b) Platform Component
The PHY1 and PHY2 nodes represent the PHYs connected to MDIO bus MDI0
---------------------------------------------------------------------

.. code-block:: none

	Scope(\_SB.MDI0)
	{
	  Device(PHY1) {
	    Name (_ADR, 0x1)
	  } // end of PHY1

	  Device(PHY2) {
	    Name (_ADR, 0x2)
	  } // end of PHY2
	}

DSDT entries representing MAC nodes
-----------------------------------

Below are the MAC nodes where PHY nodes are referenced.
phy-mode and phy-handle are used as explained earlier.
------------------------------------------------------

.. code-block:: none

	Scope(\_SB.MCE0.PR17)
	{
	  Name (_DSD, Package () {
	     ToUUID("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
		 Package () {
		     Package (2) {"phy-mode", "rgmii-id"},
		     Package (2) {"phy-handle", \_SB.MDI0.PHY1}
	      }
	   })
	}

	Scope(\_SB.MCE0.PR18)
	{
	  Name (_DSD, Package () {
	    ToUUID("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
		Package () {
		    Package (2) {"phy-mode", "rgmii-id"},
		    Package (2) {"phy-handle", \_SB.MDI0.PHY2}}
	    }
	  })
	}

MAC node example where "managed" property is specified.
-------------------------------------------------------

.. code-block:: none

	Scope(\_SB.PP21.ETH0)
	{
	  Name (_DSD, Package () {
	     ToUUID("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
		 Package () {
		     Package () {"phy-mode", "sgmii"},
		     Package () {"managed", "in-band-status"}
		 }
	   })
	}

MAC node example with a "fixed-link" subnode.
---------------------------------------------

.. code-block:: none

	Scope(\_SB.PP21.ETH1)
	{
	  Name (_DSD, Package () {
	    ToUUID("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
		 Package () {
		     Package () {"phy-mode", "sgmii"},
		 },
	    ToUUID("dbb8e3e6-5886-4ba6-8795-1319f52a966b"),
		 Package () {
		     Package () {"fixed-link", "LNK0"}
		 }
	  })
	  Name (LNK0, Package(){ // Data-only subnode of port
	    ToUUID("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
		 Package () {
		     Package () {"speed", 1000},
		     Package () {"full-duplex", 1}
		 }
	  })
	}

References
==========

[phy] Documentation/networking/phy.rst

[dsd-properties-rules]
    Documentation/firmware-guide/acpi/DSD-properties-rules.rst

[ethernet-controller]
    Documentation/devicetree/bindings/net/ethernet-controller.yaml

[dsd-guide] DSD Guide.
    https://github.com/UEFI/DSD-Guide/blob/main/dsd-guide.adoc, referenced
    2021-11-30.

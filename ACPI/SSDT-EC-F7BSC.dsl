// Based on OpenCorePkg Docs/AcpiSamples/Source/SSDT-EC-USBX.dsl.
// F7BSC firmware has PCI0.SBRG.EC0 with an existing _STA returning 0x0F.
// Add the macOS compatibility EC without renaming or overriding firmware EC0.
// USBX is already supplied by SSDT-USBX.aml; do not define it a second time.
DefinitionBlock ("", "SSDT", 2, "F7BSC", "ECFAKE", 0x00000001)
{
    External (_SB_.PCI0.SBRG, DeviceObj)

    Scope (\_SB.PCI0.SBRG)
    {
        Device (EC)
        {
            Name (_HID, "ACID0001")

            Method (_STA, 0, NotSerialized)
            {
                If (_OSI ("Darwin"))
                {
                    Return (0x0F)
                }

                Return (Zero)
            }
        }
    }
}

"""Report Phoenix discovery on the Radeon that vcnctl will open."""
import plistlib
import subprocess


def services(name, depth, show_children=False):
    result = subprocess.run(
        ["/usr/sbin/ioreg", "-alrc" if show_children else "-arc", name, "-d" + str(depth)],
        stdout=subprocess.PIPE, check=True)
    return plistlib.loads(result.stdout) if result.stdout.strip() else []


def children(node):
    return node.get("IORegistryEntryChildren", [])


def classify(drivers, pci_devices, accelerators):
    # vcnctl opens the first PhxBringup. Act only when that service is unambiguous.
    if len(drivers) != 1:
        return "skip"
    driver = drivers[0]
    if (driver.get("CFBundleIdentifier") != "org.drip.PhxBringup"
            or driver.get("PhxVideoDecodeStreamABI") != 1):
        return "skip"
    for pci in pci_devices:
        if (pci.get("vendor-id") != bytes.fromhex("02100000")
                or pci.get("device-id") != bytes.fromhex("bf150000")):
            continue
        siblings = {node["IORegistryEntryID"] for node in children(pci)}
        if driver["IORegistryEntryID"] not in siblings:
            continue
        for accel in accelerators:
            entry = accel["IORegistryEntryID"]
            if (entry not in siblings or accel.get("CFBundleIdentifier") != "com.luuumi.PhxAccel"
                    or accel.get("PhxVideoDecodeStreamABI") != 1):
                continue
            attached = any(child.get("IOObjectClass") == "PhoenixVALoadProbe"
                           and child.get("CFBundleIdentifier") == "org.nullarc.PhoenixVALoadProbe"
                           for child in children(accel))
            keys = ("IODVDBundleName", "IOVARendererID", "IOGVACodec")
            if (attached and accel.get(keys[0]) == "PhoenixVALoadProbe"
                    and isinstance(accel.get(keys[1]), int) and accel[keys[1]] > 0
                    and accel.get(keys[2]) == "PhoenixVCN4"):
                return f"ready {entry}"
            if attached or any(key in accel for key in keys):
                return f"conflict {entry}"
            return f"missing {entry}"
    return "skip"


if __name__ == "__main__":
    print(classify(services("PhxBringup", 1), services("IOPCIDevice", 2),
                   services("NMXAccel", 2, show_children=True)))

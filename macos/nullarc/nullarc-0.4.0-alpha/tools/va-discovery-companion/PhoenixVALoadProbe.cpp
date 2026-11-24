#include <IOKit/IOService.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>

// These fixed values describe only this local H.264 factory-entry experiment.
// Do not advertise HEVC or publish any key on a different GPU.
static const char *const keys[] = {"IODVDBundleName", "IOVARendererID", "IOGVACodec"};
static constexpr unsigned keyCount = sizeof(keys) / sizeof(keys[0]);

class PhoenixVALoadProbe final : public IOService {
    OSDeclareDefaultStructors(PhoenixVALoadProbe)
public:
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
private:
    OSObject *values[keyCount] = {};
    static IOReturn publish(OSObject *target, void *arg, void *, void *, void *);
    static IOReturn withdraw(OSObject *target, void *arg, void *, void *, void *);
    void releaseValues();
};

OSDefineMetaClassAndStructors(PhoenixVALoadProbe, IOService)

IOReturn PhoenixVALoadProbe::withdraw(OSObject *target, void *arg, void *, void *, void *) {
    auto *self = static_cast<PhoenixVALoadProbe *>(target);
    auto *provider = static_cast<IOService *>(arg);
    for (unsigned n = keyCount; n > 0; --n) {
        unsigned i = n - 1;
        if (self->values[i] && provider->getProperty(keys[i]) == self->values[i])
            provider->removeProperty(keys[i]);
    }
    return kIOReturnSuccess;
}

IOReturn PhoenixVALoadProbe::publish(OSObject *target, void *arg, void *, void *, void *) {
    auto *self = static_cast<PhoenixVALoadProbe *>(target);
    auto *provider = static_cast<IOService *>(arg);
    for (const char *key : keys)
        if (provider->getProperty(key)) return kIOReturnExclusiveAccess;
    for (unsigned i = 0; i < keyCount; ++i) {
        if (!provider->setProperty(keys[i], self->values[i])) {
            withdraw(target, arg, nullptr, nullptr, nullptr);
            return kIOReturnNoMemory;
        }
    }
    return kIOReturnSuccess;
}

void PhoenixVALoadProbe::releaseValues() {
    for (OSObject *&value : values)
        OSSafeReleaseNULL(value);
}

bool PhoenixVALoadProbe::start(IOService *provider) {
    // NMXAccel is a direct child of the Radeon PCI function on this machine.
    IOPCIDevice *pci = OSDynamicCast(IOPCIDevice, provider->getProvider());
    auto *owner = OSDynamicCast(OSString, provider->getProperty("CFBundleIdentifier"));
    if (!owner || !owner->isEqualTo("com.luuumi.PhxAccel") || !pci ||
        pci->configRead16(kIOPCIConfigVendorID) != 0x1002 ||
        pci->configRead16(kIOPCIConfigDeviceID) != 0x15bf) {
        return false;
    }
    if (!IOService::start(provider)) {
        return false;
    }

    values[0] = OSString::withCString("PhoenixVALoadProbe");
    values[1] = OSNumber::withNumber(0x01080001, 32);
    values[2] = OSString::withCString("PhoenixVCN4");
    if (!values[0] || !values[1] || !values[2] ||
        provider->runPropertyAction(publish, this, provider) != kIOReturnSuccess) {
        releaseValues();
        IOService::stop(provider);
        return false;
    }
    registerService();
    return true;
}

void PhoenixVALoadProbe::stop(IOService *provider) {
    provider->runPropertyAction(withdraw, this, provider);
    releaseValues();
    IOService::stop(provider);
}

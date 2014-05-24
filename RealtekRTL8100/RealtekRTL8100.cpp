/* RealtekRTL8100.c -- RTL8100 driver class implementation.
 *
 * Copyright (c) 2013 Laura Müller <laura-mueller@uni-duesseldorf.de>
 * All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * Driver for Realtek RTL8100x PCIe ethernet controllers.
 *
 * This driver is based on Realtek's r8101 Linux driver (1.024.0).
 */


#include "RealtekRTL8100.h"

#pragma mark --- function prototypes ---

static inline void fillDescriptorAddr(volatile void *baseAddr, IOPhysicalAddress64 txPhyAddr, IOPhysicalAddress64 rxPhyAddr);
static inline u32 ether_crc(int length, unsigned char *data);

#pragma mark --- public methods ---

OSDefineMetaClassAndStructors(RTL8100, super)

/* IOService (or its superclass) methods. */

bool RTL8100::init(OSDictionary *properties)
{
    bool result;
    
    result = super::init(properties);
    
    if (result) {
        workLoop = NULL;
        commandGate = NULL;
        pciDevice = NULL;
        mediumDict = NULL;
        txQueue = NULL;
        interruptSource = NULL;
        timerSource = NULL;
        netif = NULL;
        netStats = NULL;
        etherStats = NULL;
        baseMap = NULL;
        baseAddr = NULL;
        rxMbufCursor = NULL;
        txNext2FreeMbuf = NULL;
        txMbufCursor = NULL;
        statBufDesc = NULL;
        statPhyAddr = NULL;
        statData = NULL;
        isEnabled = false;
        promiscusMode = false;
        multicastMode = false;
        linkUp = false;
        stalled = false;
        useMSI = false;
        mtu = ETH_DATA_LEN;
        powerState = 0;
        speed = SPEED_1000;
        duplex = DUPLEX_FULL;
        autoneg = AUTONEG_ENABLE;
        linuxData.aspm = 0;
        pciDeviceData.vendor = 0;
        pciDeviceData.device = 0;
        pciDeviceData.subsystem_vendor = 0;
        pciDeviceData.subsystem_device = 0;
        linuxData.pci_dev = &pciDeviceData;
        unitNumber = 0;
        intrMitigateValue = 0;
        wolCapable = false;
        wolActive = false;
        enableTSO4 = false;
        enableCSO6 = false;
    }
    
done:
    return result;
}

void RTL8100::free()
{
    UInt32 i;
    
    DebugLog("free() ===>\n");
    
    if (workLoop) {
        if (interruptSource) {
            workLoop->removeEventSource(interruptSource);
            RELEASE(interruptSource);
        }
        if (timerSource) {
            workLoop->removeEventSource(timerSource);
            RELEASE(timerSource);
        }
        workLoop->release();
        workLoop = NULL;
    }
    RELEASE(commandGate);
    RELEASE(txQueue);
    RELEASE(mediumDict);
    
    for (i = MEDIUM_INDEX_AUTO; i < MEDIUM_INDEX_COUNT; i++)
        mediumTable[i] = NULL;
    
    RELEASE(baseMap);
    baseAddr = NULL;
    linuxData.mmio_addr = NULL;
    
    RELEASE(pciDevice);
    freeDMADescriptors();
    
    DebugLog("free() <===\n");
    
    super::free();
}

static const char *onName = "enabled";
static const char *offName = "disabled";

bool RTL8100::start(IOService *provider)
{
    OSNumber *intrMit;
    OSBoolean *enableEEE;
    OSBoolean *tso4;
    OSBoolean *csoV6;
    bool result;
    
    result = super::start(provider);
    
    if (!result) {
        IOLog("Ethernet [RealtekRTL8100]: IOEthernetController::start failed.\n");
        goto done;
    }
    multicastMode = false;
    promiscusMode = false;
    multicastFilter = 0;
    
    pciDevice = OSDynamicCast(IOPCIDevice, provider);
    
    if (!pciDevice) {
        IOLog("Ethernet [RealtekRTL8100]: No provider.\n");
        goto done;
    }
    pciDevice->retain();
    
    if (!pciDevice->open(this)) {
        IOLog("Ethernet [RealtekRTL8100]: Failed to open provider.\n");
        goto error1;
    }
    
    if (!initPCIConfigSpace(pciDevice)) {
        goto error2;
    }
    
    enableEEE = OSDynamicCast(OSBoolean, getProperty(kEnableEeeName));
    
    if (enableEEE)
        linuxData.eeeEnable = (enableEEE->getValue()) ? 1 : 0;
    else
        linuxData.eeeEnable = 0;
    
    IOLog("Ethernet [RealtekRTL8100]: EEE support %s.\n", linuxData.eeeEnable ? onName : offName);
    
    tso4 = OSDynamicCast(OSBoolean, getProperty(kEnableTSO4Name));
    enableTSO4 = (tso4) ? tso4->getValue() : false;
    
    IOLog("Ethernet [RealtekRTL8100]: TCP/IPv4 segmentation offload %s.\n", enableTSO4 ? onName : offName);
    
    csoV6 = OSDynamicCast(OSBoolean, getProperty(kEnableCSO6Name));
    enableCSO6 = (csoV6) ? csoV6->getValue() : false;
    
    IOLog("Ethernet [RealtekRTL8100]: TCP/IPv6 checksum offload %s.\n", enableCSO6 ? onName : offName);
    
    intrMit = OSDynamicCast(OSNumber, getProperty(kIntrMitigateName));
    
    if (intrMit)
        intrMitigateValue = intrMit->unsigned16BitValue();
    
    IOLog("Ethernet [RealtekRTL8100]: Using interrupt mitigate value 0x%x.\n", intrMitigateValue);
    
    if (!initRTL8100()) {
        goto error2;
    }
    
    if (!setupMediumDict()) {
        IOLog("Ethernet [RealtekRTL8100]: Failed to setup medium dictionary.\n");
        goto error2;
    }
    commandGate = getCommandGate();
    
    if (!commandGate) {
        IOLog("Ethernet [RealtekRTL8100]: getCommandGate() failed.\n");
        goto error3;
    }
    commandGate->retain();
    
    if (!initEventSources(provider)) {
        IOLog("Ethernet [RealtekRTL8100]: initEventSources() failed.\n");
        goto error3;
    }
    
    result = attachInterface(reinterpret_cast<IONetworkInterface**>(&netif));
    
    if (!result) {
        IOLog("Ethernet [RealtekRTL8100]: attachInterface() failed.\n");
        goto error3;
    }
    pciDevice->close(this);
    result = true;
    
done:
    return result;
    
error3:
    RELEASE(commandGate);
    
error2:
    pciDevice->close(this);
    
error1:
    pciDevice->release();
    pciDevice = NULL;
    goto done;
}

void RTL8100::stop(IOService *provider)
{
    UInt32 i;
    
    if (netif) {
        detachInterface(netif);
        netif = NULL;
    }
    if (workLoop) {
        if (interruptSource) {
            workLoop->removeEventSource(interruptSource);
            RELEASE(interruptSource);
        }
        if (timerSource) {
            workLoop->removeEventSource(timerSource);
            RELEASE(timerSource);
        }
        workLoop->release();
        workLoop = NULL;
    }
    RELEASE(commandGate);
    RELEASE(txQueue);
    RELEASE(mediumDict);
    
    for (i = MEDIUM_INDEX_AUTO; i < MEDIUM_INDEX_COUNT; i++)
        mediumTable[i] = NULL;
    
    freeDMADescriptors();
    RELEASE(baseMap);
    baseAddr = NULL;
    linuxData.mmio_addr = NULL;
    
    RELEASE(pciDevice);
    
    super::stop(provider);
}

/* Power Management Support */
static IOPMPowerState powerStateArray[kPowerStateCount] =
{
    {1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {1, kIOPMDeviceUsable, kIOPMPowerOn, kIOPMPowerOn, 0, 0, 0, 0, 0, 0, 0, 0}
};

IOReturn RTL8100::registerWithPolicyMaker(IOService *policyMaker)
{
    DebugLog("registerWithPolicyMaker() ===>\n");
    
    powerState = kPowerStateOn;
    
    DebugLog("registerWithPolicyMaker() <===\n");
    
    return policyMaker->registerPowerDriver(this, powerStateArray, kPowerStateCount);
}

IOReturn RTL8100::setPowerState(unsigned long powerStateOrdinal, IOService *policyMaker)
{
    IOReturn result = IOPMAckImplied;
    
    DebugLog("setPowerState() ===>\n");
    
    if (powerStateOrdinal == powerState) {
        DebugLog("Ethernet [RealtekRTL8100]: Already in power state %lu.\n", powerStateOrdinal);
        goto done;
    }
    DebugLog("Ethernet [RealtekRTL8100]: switching to power state %lu.\n", powerStateOrdinal);
    
    if (powerStateOrdinal == kPowerStateOff)
        commandGate->runAction(setPowerStateSleepAction);
    else
        commandGate->runAction(setPowerStateWakeAction);
    
    powerState = powerStateOrdinal;
    
done:
    DebugLog("setPowerState() <===\n");
    
    return result;
}

void RTL8100::systemWillShutdown(IOOptionBits specifier)
{
    DebugLog("systemWillShutdown() ===>\n");
    
    if ((kIOMessageSystemWillPowerOff | kIOMessageSystemWillRestart) & specifier)
        disable(netif);
    
    DebugLog("systemWillShutdown() <===\n");
    
    /* Must call super shutdown or system will stall. */
    super::systemWillShutdown(specifier);
}

/* IONetworkController methods. */
IOReturn RTL8100::enable(IONetworkInterface *netif)
{
    const IONetworkMedium *selectedMedium;
    IOReturn result = kIOReturnError;
    
    DebugLog("enable() ===>\n");
    
    if (isEnabled) {
        DebugLog("Ethernet [RealtekRTL8100]: Interface already enabled.\n");
        result = kIOReturnSuccess;
        goto done;
    }
    if (!pciDevice || pciDevice->isOpen()) {
        IOLog("Ethernet [RealtekRTL8100]: Unable to open PCI device.\n");
        goto done;
    }
    pciDevice->open(this);
    
    if (!setupDMADescriptors()) {
        IOLog("Ethernet [RealtekRTL8100]: Error allocating DMA descriptors.\n");
        goto done;
    }
    selectedMedium = getSelectedMedium();
    
    if (!selectedMedium) {
        DebugLog("Ethernet [RealtekRTL8100]: No medium selected. Falling back to autonegotiation.\n");
        selectedMedium = mediumTable[MEDIUM_INDEX_AUTO];
    }
    selectMedium(selectedMedium);
    setLinkStatus(kIONetworkLinkValid);
    enableRTL8100();
    
    /* In case we are using an msi the interrupt hasn't been enabled by start(). */
    if (useMSI)
        interruptSource->enable();
    
    txDescDoneCount = txDescDoneLast = 0;
    deadlockWarn = 0;
    needsUpdate = false;
    txQueue->setCapacity(kTransmitQueueCapacity);
    isEnabled = true;
    stalled = false;
    
    timerSource->setTimeoutMS(kTimeoutMS);
    
    result = kIOReturnSuccess;
    
    DebugLog("enable() <===\n");
    
done:
    return result;
}

IOReturn RTL8100::disable(IONetworkInterface *netif)
{
    IOReturn result = kIOReturnSuccess;
    
    DebugLog("disable() ===>\n");
    
    if (!isEnabled)
        goto done;
    
    txQueue->stop();
    txQueue->flush();
    txQueue->setCapacity(0);
    isEnabled = false;
    stalled = false;
    
    timerSource->cancelTimeout();
    needsUpdate = false;
    txDescDoneCount = txDescDoneLast = 0;
    
    /* In case we are using msi disable the interrupt. */
    if (useMSI)
        interruptSource->disable();
    
    disableRTL8100();
    
    setLinkStatus(kIONetworkLinkValid);
    linkUp = false;
    txClearDescriptors(true);
    
    if (pciDevice && pciDevice->isOpen())
        pciDevice->close(this);
    
    freeDMADescriptors();
    
    DebugLog("disable() <===\n");
    
done:
    return result;
}

UInt32 RTL8100::outputPacket(mbuf_t m, void *param)
{
    IOPhysicalSegment txSegments[kMaxSegs];
    RtlDmaDesc *desc, *firstDesc;
    UInt32 result = kIOReturnOutputDropped;
    mbuf_tso_request_flags_t tsoFlags;
    mbuf_csum_request_flags_t checksums;
    UInt32 mssValue;
    UInt32 cmd;
    UInt32 opts1;
    UInt32 opts2;
    UInt32 vlanTag;
    UInt32 csumData;
    UInt32 numSegs;
    UInt32 lastSeg;
    UInt32 index;
    UInt32 i;
    
    //DebugLog("outputPacket() ===>\n");
    
    if (!(isEnabled && linkUp)) {
        DebugLog("Ethernet [RealtekRTL8100]: Interface down. Dropping packet.\n");
        goto error;
    }
    numSegs = txMbufCursor->getPhysicalSegmentsWithCoalesce(m, &txSegments[0], kMaxSegs);
    
    if (!numSegs) {
        DebugLog("Ethernet [RealtekRTL8100]: getPhysicalSegmentsWithCoalesce() failed. Dropping packet.\n");
        etherStats->dot3TxExtraEntry.resourceErrors++;
        goto error;
    }
    if (mbuf_get_tso_requested(m, &tsoFlags, &mssValue)) {
        DebugLog("Ethernet [RealtekRTL8100]: mbuf_get_tso_requested() failed. Dropping packet.\n");
        goto error;
    }
    if (tsoFlags && (mbuf_pkthdr_len(m) <= ETH_FRAME_LEN)) {
        checksums = (tsoFlags & MBUF_TSO_IPV4) ? (kChecksumTCP | kChecksumIP): kChecksumTCPIPv6;
        tsoFlags = 0;
    } else {
        mbuf_get_csum_requested(m, &checksums, &csumData);
    }
    /* Alloc required number of descriptors. As the descriptor which has been freed last must be
     * considered to be still in use we never fill the ring completely but leave at least one
     * unused.
     */
    if ((txNumFreeDesc <= numSegs)) {
        DebugLog("Ethernet [RealtekRTL8100]: Not enough descriptors. Stalling.\n");
        result = kIOReturnOutputStall;
        stalled = true;
        goto done;
    }
    OSAddAtomic(-numSegs, &txNumFreeDesc);
    index = txNextDescIndex;
    txNextDescIndex = (txNextDescIndex + numSegs) & kTxDescMask;
    firstDesc = &txDescArray[index];
    lastSeg = numSegs - 1;
    cmd = 0;
    
    /* First fill in the VLAN tag. */
    opts2 = (getVlanTagDemand(m, &vlanTag)) ? (OSSwapInt16(vlanTag) | TxVlanTag) : 0;
    
    /* Next setup the checksum and TSO command bits. */
    getDescCommand(&cmd, &opts2, checksums, mssValue, tsoFlags);
    
    /* And finally fill in the descriptors. */
    for (i = 0; i < numSegs; i++) {
        desc = &txDescArray[index];
        opts1 = (((UInt32)txSegments[i].length) | cmd);
        opts1 |= (i == 0) ? FirstFrag : DescOwn;
        
        if (i == lastSeg) {
            opts1 |= LastFrag;
            txMbufArray[index] = m;
        } else {
            txMbufArray[index] = NULL;
        }
        if (index == kTxLastDesc)
            opts1 |= RingEnd;
        
        desc->addr = OSSwapHostToLittleInt64(txSegments[i].location);
        desc->opts2 = OSSwapHostToLittleInt32(opts2);
        desc->opts1 = OSSwapHostToLittleInt32(opts1);
        
        //DebugLog("opts1=0x%x, opts2=0x%x, addr=0x%llx, len=0x%llx\n", opts1, opts2, txSegments[i].location, txSegments[i].length);
        ++index &= kTxDescMask;
    }
    firstDesc->opts1 |= DescOwn;
    
    /* Set the polling bit. */
    WriteReg8(TxPoll, NPQ);
    
    result = kIOReturnOutputSuccess;
    
done:
    //DebugLog("outputPacket() <===\n");
    
    return result;
    
error:
    freePacket(m);
    goto done;
}

void RTL8100::getPacketBufferConstraints(IOPacketBufferConstraints *constraints) const
{
    DebugLog("getPacketBufferConstraints() ===>\n");
    
	constraints->alignStart = kIOPacketBufferAlign8;
	constraints->alignLength = kIOPacketBufferAlign8;
    
    DebugLog("getPacketBufferConstraints() <===\n");
}

IOOutputQueue* RTL8100::createOutputQueue()
{
    DebugLog("createOutputQueue() ===>\n");
    
    DebugLog("createOutputQueue() <===\n");
    
    return IOBasicOutputQueue::withTarget(this);
}

const OSString* RTL8100::newVendorString() const
{
    DebugLog("newVendorString() ===>\n");
    
    DebugLog("newVendorString() <===\n");
    
    return OSString::withCString("Realtek");
}

const OSString* RTL8100::newModelString() const
{
    DebugLog("newModelString() ===>\n");
    DebugLog("newModelString() <===\n");
    
    return OSString::withCString(rtl_chip_info[linuxData.chipset].name);
}

bool RTL8100::configureInterface(IONetworkInterface *interface)
{
    char modelName[kNameLenght];
    IONetworkData *data;
    bool result;
    
    DebugLog("configureInterface() ===>\n");
    
    result = super::configureInterface(interface);
    
    if (!result)
        goto done;
	
    /* Get the generic network statistics structure. */
    data = interface->getParameter(kIONetworkStatsKey);
    
    if (data) {
        netStats = (IONetworkStats *)data->getBuffer();
        
        if (!netStats) {
            IOLog("Ethernet [RealtekRTL8100]: Error getting IONetworkStats\n.");
            result = false;
            goto done;
        }
    }
    /* Get the Ethernet statistics structure. */
    data = interface->getParameter(kIOEthernetStatsKey);
    
    if (data) {
        etherStats = (IOEthernetStats *)data->getBuffer();
        
        if (!etherStats) {
            IOLog("Ethernet [RealtekRTL8100]: Error getting IOEthernetStats\n.");
            result = false;
            goto done;
        }
    }
    unitNumber = interface->getUnitNumber();
    snprintf(modelName, kNameLenght, "Realtek %s PCI Express Fast Ethernet", rtl_chip_info[linuxData.chipset].name);
    setProperty("model", modelName);
    
    DebugLog("configureInterface() <===\n");
    
done:
    return result;
}

bool RTL8100::createWorkLoop()
{
    DebugLog("createWorkLoop() ===>\n");
    
    workLoop = IOWorkLoop::workLoop();
    
    DebugLog("createWorkLoop() <===\n");
    
    return workLoop ? true : false;
}

IOWorkLoop* RTL8100::getWorkLoop() const
{
    DebugLog("getWorkLoop() ===>\n");
    
    DebugLog("getWorkLoop() <===\n");
    
    return workLoop;
}

/* Methods inherited from IOEthernetController. */
IOReturn RTL8100::getHardwareAddress(IOEthernetAddress *addr)
{
    IOReturn result = kIOReturnError;
    
    DebugLog("getHardwareAddress() ===>\n");
    
    if (addr) {
        bcopy(&currMacAddr.bytes, addr->bytes, kIOEthernetAddressSize);
        result = kIOReturnSuccess;
    }
    
    DebugLog("getHardwareAddress() <===\n");
    
    return result;
}

IOReturn RTL8100::setPromiscuousMode(bool active)
{
    UInt32 *filterAddr = (UInt32 *)&multicastFilter;
    UInt32 mcFilter[2];
    UInt32 rxMode;
    
    DebugLog("setPromiscuousMode() ===>\n");
    
    if (active) {
        DebugLog("Ethernet [RealtekRTL8100]: Promiscuous mode enabled.\n");
        rxMode = (AcceptBroadcast | AcceptMulticast | AcceptMyPhys | AcceptAllPhys);
        mcFilter[1] = mcFilter[0] = 0xffffffff;
    } else {
        DebugLog("Ethernet [RealtekRTL8100]: Promiscuous mode disabled.\n");
        rxMode = (AcceptBroadcast | AcceptMulticast | AcceptMyPhys);
        mcFilter[0] = *filterAddr++;
        mcFilter[1] = *filterAddr;
    }
    promiscusMode = active;
    rxMode |= rxConfigReg | (ReadReg32(RxConfig) & rxConfigMask);
    WriteReg32(RxConfig, rxMode);
    WriteReg32(MAR0, mcFilter[0]);
    WriteReg32(MAR1, mcFilter[1]);
    
    DebugLog("setPromiscuousMode() <===\n");
    
    return kIOReturnSuccess;
}

IOReturn RTL8100::setMulticastMode(bool active)
{
    UInt32 *filterAddr = (UInt32 *)&multicastFilter;
    UInt32 mcFilter[2];
    UInt32 rxMode;
    
    DebugLog("setMulticastMode() ===>\n");
    
    if (active) {
        rxMode = (AcceptBroadcast | AcceptMulticast | AcceptMyPhys);
        mcFilter[0] = *filterAddr++;
        mcFilter[1] = *filterAddr;
    } else{
        rxMode = (AcceptBroadcast | AcceptMyPhys);
        mcFilter[1] = mcFilter[0] = 0;
    }
    multicastMode = active;
    rxMode |= rxConfigReg | (ReadReg32(RxConfig) & rxConfigMask);
    WriteReg32(RxConfig, rxMode);
    WriteReg32(MAR0, mcFilter[0]);
    WriteReg32(MAR1, mcFilter[1]);
    
    DebugLog("setMulticastMode() <===\n");
    
    return kIOReturnSuccess;
}

IOReturn RTL8100::setMulticastList(IOEthernetAddress *addrs, UInt32 count)
{
    UInt32 *filterAddr = (UInt32 *)&multicastFilter;
    UInt64 filter = 0;
    UInt32 i, bitNumber;
    
    DebugLog("setMulticastList() ===>\n");
    
    if (count <= kMCFilterLimit) {
        for (i = 0; i < count; i++, addrs++) {
            bitNumber = ether_crc(6, reinterpret_cast<unsigned char *>(addrs)) >> 26;
            filter |= (1 << (bitNumber & 0x3f));
        }
        multicastFilter = OSSwapInt64(filter);
    } else {
        multicastFilter = 0xffffffffffffffff;
    }
    WriteReg32(MAR0, *filterAddr++);
    WriteReg32(MAR1, *filterAddr);
    
    DebugLog("setMulticastList() <===\n");
    
    return kIOReturnSuccess;
}

IOReturn RTL8100::getChecksumSupport(UInt32 *checksumMask, UInt32 checksumFamily, bool isOutput)
{
    IOReturn result = kIOReturnUnsupported;
    
    DebugLog("getChecksumSupport() ===>\n");
    
    if ((checksumFamily == kChecksumFamilyTCPIP) && checksumMask) {
        if (isOutput) {
            if (revision2)
                *checksumMask = (enableCSO6) ? (kChecksumTCP | kChecksumUDP | kChecksumIP | kChecksumTCPIPv6 | kChecksumUDPIPv6) : (kChecksumTCP | kChecksumUDP | kChecksumIP);
            else
                *checksumMask = (kChecksumTCP | kChecksumUDP | kChecksumIP);
        } else {
            *checksumMask = (revision2) ? (kChecksumTCP | kChecksumUDP | kChecksumIP | kChecksumTCPIPv6 | kChecksumUDPIPv6) : (kChecksumTCP | kChecksumUDP | kChecksumIP);
        }
        result = kIOReturnSuccess;
    }
    DebugLog("getChecksumSupport() <===\n");
    
    return result;
}

IOReturn RTL8100::setMaxPacketSize (UInt32 maxSize)
{
    IOReturn result = kIOReturnUnsupported;
    
done:
    return result;
}

IOReturn RTL8100::getMaxPacketSize (UInt32 *maxSize) const
{
    IOReturn result = kIOReturnBadArgument;
    
    if (maxSize) {
        *maxSize = mtu + ETHER_HDR_LEN + ETHER_CRC_LEN;
        result = kIOReturnSuccess;
    }
    return result;
}

IOReturn RTL8100::getMinPacketSize (UInt32 *minSize) const
{
    IOReturn result = super::getMinPacketSize(minSize);
    
done:
    return result;
}

IOReturn RTL8100::setWakeOnMagicPacket(bool active)
{
    IOReturn result = kIOReturnUnsupported;
    
    DebugLog("setWakeOnMagicPacket() ===>\n");
    
    if (wolCapable) {
        linuxData.wol_enabled = active ? WOL_ENABLED : WOL_DISABLED;
        wolActive = active;
        result = kIOReturnSuccess;
    }
    
    DebugLog("setWakeOnMagicPacket() <===\n");
    
    return result;
}

IOReturn RTL8100::getPacketFilters(const OSSymbol *group, UInt32 *filters) const
{
    IOReturn result = kIOReturnSuccess;
    
    DebugLog("getPacketFilters() ===>\n");
    
    if ((group == gIOEthernetWakeOnLANFilterGroup) && wolCapable) {
        *filters = kIOEthernetWakeOnMagicPacket;
        DebugLog("Ethernet [RealtekRTL8100]: kIOEthernetWakeOnMagicPacket added to filters.\n");
    } else {
        result = super::getPacketFilters(group, filters);
    }
    
    DebugLog("getPacketFilters() <===\n");
    
    return result;
}


UInt32 RTL8100::getFeatures() const
{
    DebugLog("getFeatures() ===>\n");
    DebugLog("getFeatures() <===\n");
    
    return (enableTSO4) ? (kIONetworkFeatureMultiPages | kIONetworkFeatureHardwareVlan | kIONetworkFeatureTSOIPv4) : (kIONetworkFeatureMultiPages | kIONetworkFeatureHardwareVlan);
}

IOReturn RTL8100::setHardwareAddress(const IOEthernetAddress *addr)
{
    IOReturn result = kIOReturnError;
    
    DebugLog("setHardwareAddress() ===>\n");
    
    if (addr) {
        bcopy(addr->bytes, &currMacAddr.bytes, kIOEthernetAddressSize);
        rtl8101_rar_set(&linuxData, (UInt8 *)&currMacAddr.bytes);
        result = kIOReturnSuccess;
    }
    
    DebugLog("setHardwareAddress() <===\n");
    
    return result;
}

IOReturn RTL8100::selectMedium(const IONetworkMedium *medium)
{
    IOReturn result = kIOReturnSuccess;
    
    DebugLog("selectMedium() ===>\n");
    
    if (medium) {
        switch (medium->getIndex()) {
            case MEDIUM_INDEX_AUTO:
                autoneg = AUTONEG_ENABLE;
                speed = SPEED_1000;
                duplex = DUPLEX_FULL;
                break;
                
            case MEDIUM_INDEX_10HD:
                autoneg = AUTONEG_DISABLE;
                speed = SPEED_10;
                duplex = DUPLEX_HALF;
                break;
                
            case MEDIUM_INDEX_10FD:
                autoneg = AUTONEG_DISABLE;
                speed = SPEED_10;
                duplex = DUPLEX_FULL;
                break;
                
            case MEDIUM_INDEX_100HD:
                autoneg = AUTONEG_DISABLE;
                speed = SPEED_100;
                duplex = DUPLEX_HALF;
                break;
                
            case MEDIUM_INDEX_100FD:
                autoneg = AUTONEG_DISABLE;
                speed = SPEED_100;
                duplex = DUPLEX_FULL;
                break;
                
        }
        rtl8101_set_speed(&linuxData, autoneg, speed, duplex);
        setCurrentMedium(medium);
    }
    
    DebugLog("selectMedium() <===\n");
    
done:
    return result;
}

#pragma mark --- data structure initialization methods ---

static IOMediumType mediumTypeArray[MEDIUM_INDEX_COUNT] = {
    kIOMediumEthernetAuto,
    (kIOMediumEthernet10BaseT | kIOMediumOptionHalfDuplex),
    (kIOMediumEthernet10BaseT | kIOMediumOptionFullDuplex),
    (kIOMediumEthernet100BaseTX | kIOMediumOptionHalfDuplex),
    (kIOMediumEthernet100BaseTX | kIOMediumOptionFullDuplex),
};

static UInt32 mediumSpeedArray[MEDIUM_INDEX_COUNT] = {
    0,
    10 * MBit,
    10 * MBit,
    100 * MBit,
    100 * MBit,
};

bool RTL8100::setupMediumDict()
{
	IONetworkMedium *medium;
    UInt32 i;
    bool result = false;
    
    mediumDict = OSDictionary::withCapacity(MEDIUM_INDEX_COUNT + 1);
    
    if (mediumDict) {
        for (i = MEDIUM_INDEX_AUTO; i < MEDIUM_INDEX_COUNT; i++) {
            medium = IONetworkMedium::medium(mediumTypeArray[i], mediumSpeedArray[i], 0, i);
            
            if (!medium)
                goto error1;
            
            result = IONetworkMedium::addMedium(mediumDict, medium);
            medium->release();
            
            if (!result)
                goto error1;
            
            mediumTable[i] = medium;
        }
    }
    result = publishMediumDictionary(mediumDict);
    
    if (!result)
        goto error1;
    
done:
    return result;
    
error1:
    IOLog("Ethernet [RealtekRTL8100]: Error creating medium dictionary.\n");
    mediumDict->release();
    
    for (i = MEDIUM_INDEX_AUTO; i < MEDIUM_INDEX_COUNT; i++)
        mediumTable[i] = NULL;
    
    goto done;
}

bool RTL8100::initEventSources(IOService *provider)
{
    IOReturn intrResult;
    int msiIndex = -1;
    int intrIndex = 0;
    int intrType = 0;
    bool result = false;
    
    txQueue = reinterpret_cast<IOBasicOutputQueue *>(getOutputQueue());
    
    if (txQueue == NULL) {
        IOLog("Ethernet [RealtekRTL8100]: Failed to get output queue.\n");
        goto done;
    }
    txQueue->retain();
    
    while ((intrResult = pciDevice->getInterruptType(intrIndex, &intrType)) == kIOReturnSuccess) {
        if (intrType & kIOInterruptTypePCIMessaged){
            msiIndex = intrIndex;
            break;
        }
        intrIndex++;
    }
    if (msiIndex != -1) {
        DebugLog("Ethernet [RealtekRTL8100]: MSI interrupt index: %d\n", msiIndex);
        
        interruptSource = IOInterruptEventSource::interruptEventSource(this, OSMemberFunctionCast(IOInterruptEventSource::Action, this, &RTL8100::interruptOccurred), provider, msiIndex);
    }
    if (!interruptSource) {
        DebugLog("Ethernet [RealtekRTL8100]: Warning: MSI index was not found or MSI interrupt could not be enabled.\n");
        
        interruptSource = IOInterruptEventSource::interruptEventSource(this, OSMemberFunctionCast(IOInterruptEventSource::Action, this, &RTL8100::interruptOccurred), provider);
        
        useMSI = false;
    } else {
        useMSI = true;
    }
    if (!interruptSource)
        goto error1;
    
    workLoop->addEventSource(interruptSource);
    
    /*
     * This is important. If the interrupt line is shared with other devices,
	 * then the interrupt vector will be enabled only if all corresponding
	 * interrupt event sources are enabled. To avoid masking interrupts for
	 * other devices that are sharing the interrupt line, the event source
	 * is enabled immediately.
     */
    if (!useMSI)
        interruptSource->enable();
    
    timerSource = IOTimerEventSource::timerEventSource(this, OSMemberFunctionCast(IOTimerEventSource::Action, this, &RTL8100::timerActionRTL8100));
    
    if (!timerSource) {
        IOLog("Ethernet [RealtekRTL8100]: Failed to create IOTimerEventSource.\n");
        goto error2;
    }
    workLoop->addEventSource(timerSource);
    
    result = true;
    
done:
    return result;
    
error2:
    workLoop->removeEventSource(interruptSource);
    RELEASE(interruptSource);
    
error1:
    IOLog("Ethernet [RealtekRTL8100]: Error initializing event sources.\n");
    txQueue->release();
    txQueue = NULL;
    goto done;
}

bool RTL8100::setupDMADescriptors()
{
    IOPhysicalSegment rxSegment;
    mbuf_t spareMbuf[kRxNumSpareMbufs];
    mbuf_t m;
    UInt32 i;
    UInt32 opts1;
    bool result = false;
    
    /* Create transmitter descriptor array. */
    txBufDesc = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task, (kIODirectionInOut | kIOMemoryPhysicallyContiguous | kIOMapInhibitCache), kTxDescSize, 0xFFFFFFFFFFFFFF00ULL);
    
    if (!txBufDesc) {
        IOLog("Ethernet [RealtekRTL8100]: Couldn't alloc txBufDesc.\n");
        goto done;
    }
    if (txBufDesc->prepare() != kIOReturnSuccess) {
        IOLog("Ethernet [RealtekRTL8100]: txBufDesc->prepare() failed.\n");
        goto error1;
    }
    txDescArray = (RtlDmaDesc *)txBufDesc->getBytesNoCopy();
    txPhyAddr = OSSwapHostToLittleInt64(txBufDesc->getPhysicalAddress());
    
    /* Initialize txDescArray. */
    bzero(txDescArray, kTxDescSize);
    txDescArray[kTxLastDesc].opts1 = OSSwapHostToLittleInt32(RingEnd);
    
    for (i = 0; i < kNumTxDesc; i++) {
        txMbufArray[i] = NULL;
    }
    txNextDescIndex = txDirtyDescIndex = 0;
    txNumFreeDesc = kNumTxDesc;
    txMbufCursor = IOMbufNaturalMemoryCursor::withSpecification(0x4000, kMaxSegs);
    
    if (!txMbufCursor) {
        IOLog("Ethernet [RealtekRTL8100]: Couldn't create txMbufCursor.\n");
        goto error2;
    }
    
    /* Create receiver descriptor array. */
    rxBufDesc = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task, (kIODirectionInOut | kIOMemoryPhysicallyContiguous | kIOMapInhibitCache), kRxDescSize, 0xFFFFFFFFFFFFFF00ULL);
    
    if (!rxBufDesc) {
        IOLog("Ethernet [RealtekRTL8100]: Couldn't alloc rxBufDesc.\n");
        goto error3;
    }
    
    if (rxBufDesc->prepare() != kIOReturnSuccess) {
        IOLog("Ethernet [RealtekRTL8100]: rxBufDesc->prepare() failed.\n");
        goto error4;
    }
    rxDescArray = (RtlDmaDesc *)rxBufDesc->getBytesNoCopy();
    rxPhyAddr = OSSwapHostToLittleInt64(rxBufDesc->getPhysicalAddress());
    
    /* Initialize rxDescArray. */
    bzero(rxDescArray, kRxDescSize);
    rxDescArray[kRxLastDesc].opts1 = OSSwapHostToLittleInt32(RingEnd);
    
    for (i = 0; i < kNumRxDesc; i++) {
        rxMbufArray[i] = NULL;
    }
    rxNextDescIndex = 0;
    
    rxMbufCursor = IOMbufNaturalMemoryCursor::withSpecification(PAGE_SIZE, 1);
    
    if (!rxMbufCursor) {
        IOLog("Ethernet [RealtekRTL8100]: Couldn't create rxMbufCursor.\n");
        goto error5;
    }
    /* Alloc receive buffers. */
    for (i = 0; i < kNumRxDesc; i++) {
        m = allocatePacket(kRxBufferPktSize);
        
        if (!m) {
            IOLog("Ethernet [RealtekRTL8100]: Couldn't alloc receive buffer.\n");
            goto error6;
        }
        rxMbufArray[i] = m;
        
        if (rxMbufCursor->getPhysicalSegmentsWithCoalesce(m, &rxSegment, 1) != 1) {
            IOLog("Ethernet [RealtekRTL8100]: getPhysicalSegmentsWithCoalesce() for receive buffer failed.\n");
            goto error6;
        }
        opts1 = (UInt32)rxSegment.length;
        opts1 |= (i == kRxLastDesc) ? (RingEnd | DescOwn) : DescOwn;
        rxDescArray[i].opts1 = OSSwapHostToLittleInt32(opts1);
        rxDescArray[i].opts2 = 0;
        rxDescArray[i].addr = OSSwapHostToLittleInt64(rxSegment.location);
    }
    /* Create statistics dump buffer. */
    statBufDesc = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task, (kIODirectionIn | kIOMemoryPhysicallyContiguous | kIOMapInhibitCache), sizeof(RtlStatData), 0xFFFFFFFFFFFFFF00ULL);
    
    if (!statBufDesc) {
        IOLog("Ethernet [RealtekRTL8100]: Couldn't alloc statBufDesc.\n");
        goto error6;
    }
    
    if (statBufDesc->prepare() != kIOReturnSuccess) {
        IOLog("Ethernet [RealtekRTL8100]: statBufDesc->prepare() failed.\n");
        goto error7;
    }
    statData = (RtlStatData *)statBufDesc->getBytesNoCopy();
    statPhyAddr = OSSwapHostToLittleInt64(statBufDesc->getPhysicalAddress());
    
    /* Initialize statData. */
    bzero(statData, sizeof(RtlStatData));
    
    /* Allocate some spare mbufs and free them in order to increase the buffer pool.
     * This seems to avoid the replaceOrCopyPacket() errors under heavy load.
     */
    for (i = 0; i < kRxNumSpareMbufs; i++)
        spareMbuf[i] = allocatePacket(kRxBufferPktSize);
    
    for (i = 0; i < kRxNumSpareMbufs; i++) {
        if (spareMbuf[i])
            freePacket(spareMbuf[i]);
    }
    result = true;
    
done:
    return result;
    
error7:
    statBufDesc->release();
    statBufDesc = NULL;
    
error6:
    for (i = 0; i < kNumRxDesc; i++) {
        if (rxMbufArray[i]) {
            freePacket(rxMbufArray[i]);
            rxMbufArray[i] = NULL;
        }
    }
    RELEASE(rxMbufCursor);
    
error5:
    rxBufDesc->complete();
    
error4:
    rxBufDesc->release();
    rxBufDesc = NULL;
    
error3:
    RELEASE(txMbufCursor);
    
error2:
    txBufDesc->complete();
    
error1:
    txBufDesc->release();
    txBufDesc = NULL;
    goto done;
}

void RTL8100::freeDMADescriptors()
{
    UInt32 i;
    
    if (txBufDesc) {
        txBufDesc->complete();
        txBufDesc->release();
        txBufDesc = NULL;
        txPhyAddr = NULL;
    }
    RELEASE(txMbufCursor);
    
    if (rxBufDesc) {
        rxBufDesc->complete();
        rxBufDesc->release();
        rxBufDesc = NULL;
        rxPhyAddr = NULL;
    }
    RELEASE(rxMbufCursor);
    
    for (i = 0; i < kNumRxDesc; i++) {
        if (rxMbufArray[i]) {
            freePacket(rxMbufArray[i]);
            rxMbufArray[i] = NULL;
        }
    }
    if (statBufDesc) {
        statBufDesc->complete();
        statBufDesc->release();
        statBufDesc = NULL;
        statPhyAddr = NULL;
        statData = NULL;
    }
}

void RTL8100::txClearDescriptors(bool withReset)
{
    mbuf_t m;
    UInt32 lastIndex = kTxLastDesc;
    UInt32 i;
    
    DebugLog("txClearDescriptors() ===>\n");
    
    if (txNext2FreeMbuf) {
        freePacket(txNext2FreeMbuf);
        txNext2FreeMbuf = NULL;
    }
    for (i = 0; i < kNumTxDesc; i++) {
        txDescArray[i].opts1 = OSSwapHostToLittleInt32((i != lastIndex) ? 0 : RingEnd);
        m = txMbufArray[i];
        
        if (m) {
            freePacket(m);
            txMbufArray[i] = NULL;
        }
    }
    if (withReset)
        txDirtyDescIndex = txNextDescIndex = 0;
    else
        txDirtyDescIndex = txNextDescIndex;
    
    txNumFreeDesc = kNumTxDesc;
    
    DebugLog("txClearDescriptors() <===\n");
}

#pragma mark --- common interrupt methods ---

void RTL8100::pciErrorInterrupt()
{
    UInt16 cmdReg = pciDevice->configRead16(kIOPCIConfigCommand);
    UInt16 statusReg = pciDevice->configRead16(kIOPCIConfigStatus);
    
    DebugLog("Ethernet [RealtekRTL8100]: PCI error: cmdReg=0x%x, statusReg=0x%x\n", cmdReg, statusReg);
    
    cmdReg |= (kIOPCICommandSERR | kIOPCICommandParityError);
    statusReg &= (kIOPCIStatusParityErrActive | kIOPCIStatusSERRActive | kIOPCIStatusMasterAbortActive | kIOPCIStatusTargetAbortActive | kIOPCIStatusTargetAbortCapable);
    pciDevice->configWrite16(kIOPCIConfigCommand, cmdReg);
    pciDevice->configWrite16(kIOPCIConfigStatus, statusReg);
    
    /* Reset the NIC in order to resume operation. */
    restartRTL8100();
}

/* Some (all?) of the RTL8100 family members don't handle descriptors properly.
 * They randomly release control of descriptors pointing to certain packets
 * before the request has been completed and reclaim them later.
 *
 * As a workaround we should:
 * - leave returned descriptors untouched until they get reused.
 * - never reuse the descriptor which has been returned last, i.e. leave at
 *   least one of the descriptors in txDescArray unused.
 * - delay freeing packets until the next descriptor has been finished or a
 *   small period of time has passed (as these packets are really small a
 *   few µ secs should be enough).
 */

void RTL8100::txInterrupt()
{
    SInt32 numDirty = kNumTxDesc - txNumFreeDesc;
    UInt32 oldDirtyIndex = txDirtyDescIndex;
    UInt32 descStatus;
    
    while (numDirty-- > 0) {
        descStatus = OSSwapLittleToHostInt32(txDescArray[txDirtyDescIndex].opts1);
        
        if (descStatus & DescOwn)
            break;
        
        /* Now it's time to free the last mbuf as we can be sure it's not in use anymore. */
        if (txNext2FreeMbuf)
            freePacket(txNext2FreeMbuf);
        
        txNext2FreeMbuf = txMbufArray[txDirtyDescIndex];
        txMbufArray[txDirtyDescIndex] = NULL;
        txDescDoneCount++;
        OSIncrementAtomic(&txNumFreeDesc);
        ++txDirtyDescIndex &= kTxDescMask;
    }
    if (stalled && (txNumFreeDesc > kMaxSegs)) {
        DebugLog("Ethernet [RealtekRTL8100]: Restart stalled queue!\n");
        txQueue->service(IOBasicOutputQueue::kServiceAsync);
        stalled = false;
    }
    if (oldDirtyIndex != txDirtyDescIndex)
        WriteReg8(TxPoll, NPQ);
    
    etherStats->dot3TxExtraEntry.interrupts++;
}

void RTL8100::rxInterrupt()
{
    IOPhysicalSegment rxSegment;
    RtlDmaDesc *desc = &rxDescArray[rxNextDescIndex];
    mbuf_t bufPkt, newPkt;
    UInt64 addr;
    UInt32 opts1, opts2;
    UInt32 descStatus1, descStatus2;
    UInt32 pktSize;
    UInt16 vlanTag;
    UInt16 goodPkts = 0;
    bool replaced;
    
    while (!((descStatus1 = OSSwapLittleToHostInt32(desc->opts1)) & DescOwn)) {
        opts1 = (rxNextDescIndex == kRxLastDesc) ? (RingEnd | DescOwn) : DescOwn;
        opts2 = 0;
        addr = 0;
        
        /* As we don't support jumbo frames we consider fragmented packets as errors. */
        if ((descStatus1 & (FirstFrag|LastFrag)) != (FirstFrag|LastFrag)) {
            DebugLog("Ethernet [RealtekRTL8100]: Fragmented packet.\n");
            etherStats->dot3StatsEntry.frameTooLongs++;
            opts1 |= kRxBufferPktSize;
            goto nextDesc;
        }
        
        descStatus2 = OSSwapLittleToHostInt32(desc->opts2);
        pktSize = (descStatus1 & 0x1fff) - kIOEthernetCRCSize;
        bufPkt = rxMbufArray[rxNextDescIndex];
        vlanTag = (descStatus2 & RxVlanTag) ? OSSwapInt16(descStatus2 & 0xffff) : 0;
        //DebugLog("rxInterrupt(): descStatus1=0x%x, descStatus2=0x%x, pktSize=%u\n", descStatus1, descStatus2, pktSize);
        
        newPkt = replaceOrCopyPacket(&bufPkt, pktSize, &replaced);
        
        if (!newPkt) {
            /* Allocation of a new packet failed so that we must leave the original packet in place. */
            DebugLog("Ethernet [RealtekRTL8100]: replaceOrCopyPacket() failed.\n");
            etherStats->dot3RxExtraEntry.resourceErrors++;
            opts1 |= kRxBufferPktSize;
            goto nextDesc;
        }
        
        /* If the packet was replaced we have to update the descriptor's buffer address. */
        if (replaced) {
            if (rxMbufCursor->getPhysicalSegmentsWithCoalesce(bufPkt, &rxSegment, 1) != 1) {
                DebugLog("Ethernet [RealtekRTL8100]: getPhysicalSegmentsWithCoalesce() failed.\n");
                etherStats->dot3RxExtraEntry.resourceErrors++;
                freePacket(bufPkt);
                opts1 |= kRxBufferPktSize;
                goto nextDesc;
            }
            opts1 |= ((UInt32)rxSegment.length & 0x0000ffff);
            addr = rxSegment.location;
            rxMbufArray[rxNextDescIndex] = bufPkt;
        } else {
            opts1 |= kRxBufferPktSize;
        }
        getChecksumResult(newPkt, descStatus1, descStatus2);
        
        /* Also get the VLAN tag if there is any. */
        if (vlanTag)
            setVlanTag(newPkt, vlanTag);
        
        netif->inputPacket(newPkt, pktSize, IONetworkInterface::kInputOptionQueuePacket);
        goodPkts++;
        
        /* Finally update the descriptor and get the next one to examine. */
    nextDesc:
        if (addr)
            desc->addr = OSSwapHostToLittleInt64(addr);
        
        desc->opts2 = OSSwapHostToLittleInt32(opts2);
        desc->opts1 = OSSwapHostToLittleInt32(opts1);
        
        ++rxNextDescIndex &= kRxDescMask;
        desc = &rxDescArray[rxNextDescIndex];
    }
    if (goodPkts)
        netif->flushInputQueue();
    
    //etherStats->dot3RxExtraEntry.interrupts++;
}

void RTL8100::interruptOccurred(OSObject *client, IOInterruptEventSource *src, int count)
{
	UInt16 status;
    
	WriteReg16(IntrMask, 0x0000);
    status = ReadReg16(IntrStatus);
    
    /* hotplug/major error/no more work/shared irq */
    if ((status == 0xFFFF) || !status)
        goto done;
        
    if (status & SYSErr)
        pciErrorInterrupt();
    
    /* Rx interrupt */
    if (status & (RxOK | RxDescUnavail | RxFIFOOver))
        rxInterrupt();
    
    /* Tx interrupt */
    if (status & (TxOK | TxErr | TxDescUnavail))
        txInterrupt();
        
    /* Check if a statistics dump has been completed. */
    if (needsUpdate && !(ReadReg32(CounterAddrLow) & CounterDump))
        updateStatitics();
    
done:
    WriteReg16(IntrStatus, status);
	WriteReg16(IntrMask, intrMask);
}

bool RTL8100::checkForDeadlock()
{
    bool deadlock = false;
    
    if ((txDescDoneCount == txDescDoneLast) && (txNumFreeDesc < kNumTxDesc)) {
        if (++deadlockWarn == kTxCheckTreshhold) {
            /* Some members of the RTL8100 family seem to be prone to lose transmitter rinterrupts.
             * In order to avoid false positives when trying to detect transmitter deadlocks, check
             * the transmitter ring once for completed descriptors before we assume a deadlock.
             */
            IOLog("Ethernet [RealtekRTL8100]: Tx timeout. Lost interrupt?\n");
            etherStats->dot3TxExtraEntry.timeouts++;
            txInterrupt();
        } else if (deadlockWarn >= kTxDeadlockTreshhold) {
#ifdef DEBUG
            UInt32 i, index;
            
            for (i = 0; i < 10; i++) {
                index = ((txDirtyDescIndex - 1 + i) & kTxDescMask);
                IOLog("Ethernet [RealtekRTL8100]: desc[%u]: opts1=0x%x, opts2=0x%x, addr=0x%llx.\n", index, txDescArray[index].opts1, txDescArray[index].opts2, txDescArray[index].addr);
            }
#endif
            IOLog("Ethernet [RealtekRTL8100]: Tx stalled? Resetting chipset. ISR=0x%x, IMR=0x%x.\n", ReadReg16(IntrStatus), ReadReg16(IntrMask));
            etherStats->dot3TxExtraEntry.resets++;
            restartRTL8100();
            deadlock = true;
        }
    } else {
        deadlockWarn = 0;
    }
    return deadlock;
}

#pragma mark --- hardware specific methods ---

void RTL8100::getDescCommand(UInt32 *cmd1, UInt32 *cmd2, mbuf_csum_request_flags_t checksums, UInt32 mssValue, mbuf_tso_request_flags_t tsoFlags)
{
    if (revision2) {
        if (tsoFlags & MBUF_TSO_IPV4) {
            *cmd2 |= (((mssValue & MSSMask) << MSSShift_C) | TxIPCS_C | TxTCPCS_C);
            *cmd1 = LargeSend;
        } else {
            if (checksums & kChecksumTCP)
                *cmd2 |= (TxIPCS_C | TxTCPCS_C);
            else if (checksums & kChecksumUDP)
                *cmd2 |= (TxIPCS_C | TxUDPCS_C);
            else if (checksums & kChecksumIP)
                *cmd2 |= TxIPCS_C;
            else if (checksums & kChecksumTCPIPv6)
                *cmd2 |= (TxTCPCS_C | TxIPV6_C | ((kMinL4HdrOffset & L4OffMask) << MSSShift_C));
            else if (checksums & kChecksumUDPIPv6)
                *cmd2 |= (TxUDPCS_C | TxIPV6_C | ((kMinL4HdrOffset & L4OffMask) << MSSShift_C));
        }
    } else {
        if (tsoFlags & MBUF_TSO_IPV4) {
            /* This is a TSO operation so that there are no checksum command bits. */
            *cmd1 = (LargeSend |((mssValue & MSSMask) << MSSShift));
        } else {
            /* Setup the checksum command bits. */
            if (checksums & kChecksumTCP)
                *cmd1 = (TxIPCS | TxTCPCS);
            else if (checksums & kChecksumUDP)
                *cmd1 = (TxIPCS | TxUDPCS);
            else if (checksums & kChecksumIP)
                *cmd1 = TxIPCS;
        }
    }
}

#ifdef DEBUG

void RTL8100::getChecksumResult(mbuf_t m, UInt32 status1, UInt32 status2)
{
    UInt32 resultMask = 0;
    UInt32 validMask = 0;
    UInt32 pktType = (status1 & RxProtoMask);
    
    /* Get the result of the checksum calculation and store it in the packet. */
    if (revision2) {
        if (pktType == RxTCPT) {
            /* TCP packet */
            if (status2 & RxV4F) {
                resultMask = (kChecksumTCP | kChecksumIP);
                validMask = (status1 & RxTCPF) ? 0 : (kChecksumTCP | kChecksumIP);
            } else if (status2 & RxV6F) {
                resultMask = kChecksumTCPIPv6;
                validMask = (status1 & RxTCPF) ? 0 : kChecksumTCPIPv6;
            }
        } else if (pktType == RxUDPT) {
            /* UDP packet */
            if (status2 & RxV4F) {
                resultMask = (kChecksumUDP | kChecksumIP);
                validMask = (status1 & RxUDPF) ? 0 : (kChecksumUDP | kChecksumIP);
            } else if (status2 & RxV6F) {
                resultMask = kChecksumUDPIPv6;
                validMask = (status1 & RxUDPF) ? 0 : kChecksumUDPIPv6;
            }
        } else if ((pktType == 0) && (status2 & RxV4F)) {
            /* IP packet */
            resultMask = kChecksumIP;
            validMask = (status1 & RxIPF) ? 0 : kChecksumIP;
        }
    } else {
        if (pktType == RxProtoTCP) {
            /* TCP packet */
            resultMask = (kChecksumTCP | kChecksumIP);
            validMask = (status1 & RxTCPF) ? 0 : (kChecksumTCP | kChecksumIP);
        } else if (pktType == RxProtoUDP) {
            /* UDP packet */
            resultMask = (kChecksumUDP | kChecksumIP);
            validMask = (status1 & RxUDPF) ? 0 : (kChecksumUDP | kChecksumIP);
        } else if (pktType == RxProtoIP) {
            /* IP packet */
            resultMask = kChecksumIP;
            validMask = (status1 & RxIPF) ? 0 : kChecksumIP;
        }
    }
    if (validMask != resultMask)
        IOLog("Ethernet [RealtekRTL8100]: checksums applied: 0x%x, checksums valid: 0x%x\n", resultMask, validMask);
    
    if (validMask)
        setChecksumResult(m, kChecksumFamilyTCPIP, resultMask, validMask);
}

#else

void RTL8100::getChecksumResult(mbuf_t m, UInt32 status1, UInt32 status2)
{
    UInt32 resultMask = 0;
    UInt32 pktType = (status1 & RxProtoMask);
    
    if (revision2) {
        /* Get the result of the checksum calculation and store it in the packet. */
        if (pktType == RxTCPT) {
            /* TCP packet */
            if (status2 & RxV4F)
                resultMask = (status1 & RxTCPF) ? 0 : (kChecksumTCP | kChecksumIP);
            else if (status2 & RxV6F)
                resultMask = (status1 & RxTCPF) ? 0 : kChecksumTCPIPv6;
        } else if (pktType == RxUDPT) {
            /* UDP packet */
            if (status2 & RxV4F)
                resultMask = (status1 & RxUDPF) ? 0 : (kChecksumUDP | kChecksumIP);
            else if (status2 & RxV6F)
                resultMask = (status1 & RxUDPF) ? 0 : kChecksumUDPIPv6;
        } else if ((pktType == 0) && (status2 & RxV4F)) {
            /* IP packet */
            resultMask = (status1 & RxIPF) ? 0 : kChecksumIP;
        }
    } else {
        if (pktType == RxProtoTCP)
            resultMask = (status1 & RxTCPF) ? 0 : (kChecksumTCP | kChecksumIP);  /* TCP packet */
        else if (pktType == RxProtoUDP)
            resultMask = (status1 & RxUDPF) ? 0 : (kChecksumUDP | kChecksumIP);  /* UDP packet */
        else if (pktType == RxProtoIP)
            resultMask = (status1 & RxIPF) ? 0 : kChecksumIP;                    /* IP packet */
    }
    if (resultMask)
        setChecksumResult(m, kChecksumFamilyTCPIP, resultMask, resultMask);
}

#endif

static const char *speed100MName = "100-Megabit";
static const char *speed10MName = "10-Megabit";
static const char *duplexFullName = "Full-duplex";
static const char *duplexHalfName = "Half-duplex";
static const char *offFlowName = "No flow-control";
static const char *onFlowName = "flow-control";

void RTL8100::setLinkUp(UInt8 linkState)
{
    UInt64 mediumSpeed;
    UInt32 mediumIndex = MEDIUM_INDEX_AUTO;
    const char *speedName;
    const char *duplexName;
    const char *flowName;
    
    /* Get link speed, duplex and flow-control mode. */
    if (linkState & _100bps) {
        mediumSpeed = kSpeed100MBit;
        speed = SPEED_100;
        speedName = speed100MName;
        
        if (linkState & FullDup) {
            mediumIndex = MEDIUM_INDEX_100FD;
            duplexName = duplexFullName;
        } else {
            mediumIndex = MEDIUM_INDEX_100HD;
            duplexName = duplexHalfName;
        }
    } else {
        mediumSpeed = kSpeed10MBit;
        speed = SPEED_10;
        speedName = speed10MName;
        
        if (linkState & FullDup) {
            mediumIndex = MEDIUM_INDEX_10FD;
            duplexName = duplexFullName;
        } else {
            mediumIndex = MEDIUM_INDEX_10HD;
            duplexName = duplexHalfName;
        }
    }
    if (linkState &	(TxFlowCtrl | RxFlowCtrl))
        flowName = onFlowName;
    else
        flowName = offFlowName;
    
    linkUp = true;
    setLinkStatus(kIONetworkLinkValid | kIONetworkLinkActive, mediumTable[mediumIndex], mediumSpeed, NULL);
    
    /* Restart txQueue, statistics updates and watchdog. */
    txQueue->start();
    
    if (stalled) {
        txQueue->service();
        stalled = false;
        DebugLog("Ethernet [RealtekRTL8100]: Restart stalled queue!\n");
    }
    IOLog("Ethernet [RealtekRTL8100]: Link up on en%u, %s, %s, %s\n", unitNumber, speedName, duplexName, flowName);
}

void RTL8100::setLinkDown()
{
    deadlockWarn = 0;
    needsUpdate = false;
    
    /* Stop txQueue. */
    txQueue->stop();
    txQueue->flush();
    
    /* Update link status. */
    linkUp = false;
    setLinkStatus(kIONetworkLinkValid);
    
    /* Cleanup descriptor ring. */
    txClearDescriptors(false);
    IOLog("Ethernet [RealtekRTL8100]: Link down on en%u\n", unitNumber);
}

void RTL8100::dumpTallyCounter()
{
    UInt32 cmd;
    
    /* Some chips are unable to dump the tally counter while the receiver is disabled. */
    if (ReadReg8(ChipCmd) & CmdRxEnb) {
        WriteReg32(CounterAddrHigh, (UInt32)(statPhyAddr >> 32));
        cmd = (UInt32)(statPhyAddr & 0x00000000ffffffff);
        WriteReg32(CounterAddrLow, cmd);
        WriteReg32(CounterAddrLow, cmd | CounterDump);
        needsUpdate = true;
    }
}

void RTL8100::updateStatitics()
{
    UInt32 sgColl, mlColl;
    
    needsUpdate = false;
    netStats->inputPackets = (UInt32)OSSwapLittleToHostInt64(statData->rxPackets) & 0x00000000ffffffff;
    netStats->inputErrors = OSSwapLittleToHostInt32(statData->rxErrors);
    netStats->outputPackets = (UInt32)OSSwapLittleToHostInt64(statData->txPackets) & 0x00000000ffffffff;
    netStats->outputErrors = OSSwapLittleToHostInt32(statData->txErrors);
    
    sgColl = OSSwapLittleToHostInt32(statData->txOneCollision);
    mlColl = OSSwapLittleToHostInt32(statData->txMultiCollision);
    netStats->collisions = sgColl + mlColl;
    
    etherStats->dot3StatsEntry.singleCollisionFrames = sgColl;
    etherStats->dot3StatsEntry.multipleCollisionFrames = mlColl;
    etherStats->dot3StatsEntry.alignmentErrors = OSSwapLittleToHostInt16(statData->alignErrors);
    etherStats->dot3StatsEntry.missedFrames = OSSwapLittleToHostInt16(statData->rxMissed);
    etherStats->dot3TxExtraEntry.underruns = OSSwapLittleToHostInt16(statData->txUnderun);
}

#pragma mark --- hardware initialization methods ---

bool RTL8100::initPCIConfigSpace(IOPCIDevice *provider)
{
    UInt32 pcieLinkCap;
    UInt16 pcieLinkCtl;
    UInt16 cmdReg;
    UInt16 pmCap;
    UInt8 pmCapOffset;
    UInt8 pcieCapOffset;
    bool result = false;
    
    /* Get vendor and device info. */
    pciDeviceData.vendor = provider->configRead16(kIOPCIConfigVendorID);
    pciDeviceData.device = provider->configRead16(kIOPCIConfigDeviceID);
    pciDeviceData.subsystem_vendor = provider->configRead16(kIOPCIConfigSubSystemVendorID);
    pciDeviceData.subsystem_device = provider->configRead16(kIOPCIConfigSubSystemID);
    
    /* Setup power management. */
    if (provider->findPCICapability(kIOPCIPowerManagementCapability, &pmCapOffset)) {
        pmCap = provider->configRead16(pmCapOffset + kIOPCIPMCapability);
        DebugLog("Ethernet [RealtekRTL8100]: PCI power management capabilities: 0x%x.\n", pmCap);
        
        if (pmCap & kPCIPMCPMESupportFromD3Cold) {
            wolCapable = true;
            DebugLog("Ethernet [RealtekRTL8100]: PME# from D3 (cold) supported.\n");
        }
    } else {
        IOLog("Ethernet [RealtekRTL8100]: PCI power management unsupported.\n");
    }
    provider->enablePCIPowerManagement(kPCIPMCSPowerStateD0);
    
    /* Get PCIe link information. */
    if (provider->findPCICapability(kIOPCIPCIExpressCapability, &pcieCapOffset)) {
        pcieLinkCap = provider->configRead32(pcieCapOffset + kIOPCIELinkCapability);
        pcieLinkCtl = provider->configRead16(pcieCapOffset + kIOPCIELinkControl);
        DebugLog("Ethernet [RealtekRTL8100]: PCIe link capabilities: 0x%08x, link control: 0x%04x.\n", pcieLinkCap, pcieLinkCtl);
        
        if (pcieLinkCtl & (kIOPCIELinkCtlASPM | kIOPCIELinkCtlClkReqEn)) {
            IOLog("Ethernet [RealtekRTL8100]: Warning: PCIe ASPM enabled.\n");
            linuxData.aspm = 1;
        }
    }
    /* Enable the device. */
    cmdReg	= provider->configRead16(kIOPCIConfigCommand);
    cmdReg  &= ~kIOPCICommandIOSpace;
    cmdReg	|= (kIOPCICommandBusMaster | kIOPCICommandMemorySpace | kIOPCICommandMemWrInvalidate);
	provider->configWrite16(kIOPCIConfigCommand, cmdReg);
    provider->configWrite8(kIOPCIConfigLatencyTimer, 0x40);
    provider->configWrite32(0x30, 0);

    baseMap = provider->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress2);
    
    if (!baseMap) {
        IOLog("Ethernet [RealtekRTL8100]: region #2 not an MMIO resource, aborting.\n");
        goto done;
    }
    baseAddr = reinterpret_cast<volatile void *>(baseMap->getVirtualAddress());
    linuxData.mmio_addr = baseAddr;
    result = true;
    
done:
    return result;
}

IOReturn RTL8100::setPowerStateWakeAction(OSObject *owner, void *arg1, void *arg2, void *arg3, void *arg4)
{
    RTL8100 *ethCtlr = OSDynamicCast(RTL8100, owner);
    
    if (ethCtlr)
        ethCtlr->pciDevice->enablePCIPowerManagement(kPCIPMCSPowerStateD0);
    
    return kIOReturnSuccess;
}

IOReturn RTL8100::setPowerStateSleepAction(OSObject *owner, void *arg1, void *arg2, void *arg3, void *arg4)
{
    RTL8100 *ethCtlr = OSDynamicCast(RTL8100, owner);
    IOPCIDevice *dev;
    
    if (ethCtlr) {
        dev = ethCtlr->pciDevice;
        
        if (ethCtlr->wolActive)
            dev->enablePCIPowerManagement(kPCIPMCSPMEStatus | kPCIPMCSPMEEnable | kPCIPMCSPowerStateD3);
        else
            dev->enablePCIPowerManagement(kPCIPMCSPowerStateD3);
    }
    return kIOReturnSuccess;
}

bool RTL8100::initRTL8100()
{
    struct rtl8101_private *tp = &linuxData;
    UInt32 i;
    UInt16 mac_addr[4];
    bool result = false;
    
    /* Soft reset the chip. */
    WriteReg8(ChipCmd, CmdReset);
    
    /* Check that the chip has finished the reset. */
    for (i = 1000; i > 0; i--) {
        if ((ReadReg8(ChipCmd) & CmdReset) == 0)
            break;
        IODelay(10);
    }
    /* Identify chip attached to board */
	rtl8101_get_mac_version(tp, baseAddr);
    
    /* Assume original RTL-8101E in case of unkown chipset. */
    tp->chipset = (tp->mcfg <= CFG_METHOD_17) ? tp->mcfg : CFG_METHOD_1;

    /* Select the chip revision. */
    revision2 = ((tp->chipset == CFG_METHOD_1) || (tp->chipset == CFG_METHOD_2) || (tp->chipset == CFG_METHOD_3)) ? false : true;
    
    tp->set_speed = rtl8101_set_speed_xmii;
    tp->get_settings = rtl8101_gset_xmii;
    tp->phy_reset_enable = rtl8101_xmii_reset_enable;
    tp->phy_reset_pending = rtl8101_xmii_reset_pending;
    tp->link_ok = rtl8101_xmii_link_ok;
    
    tp->cp_cmd = ReadReg16(CPlusCmd);
    intrMask = (revision2) ? (SYSErr | RxDescUnavail | TxOK | RxOK) : (SYSErr | RxDescUnavail | TxErr | TxOK | RxOK);

    rtl8101_get_bios_setting(tp);
    rtl8101_exit_oob(tp);
    rtl8101_hw_init(tp);
    rtl8101_nic_reset(tp);
    
    /* Get production from EEPROM */
    if (tp->mcfg == CFG_METHOD_17 && (mac_ocp_read(tp, 0xDC00) & BIT_3))
        tp->eeprom_type = EEPROM_TYPE_NONE;
    else
        rtl_eeprom_type(tp);
    
    if (tp->eeprom_type == EEPROM_TYPE_93C46 || tp->eeprom_type == EEPROM_TYPE_93C56)
        rtl_set_eeprom_sel_low(baseAddr);
    
    if (tp->mcfg == CFG_METHOD_14 ||
        tp->mcfg == CFG_METHOD_17) {
        *(u32*)&mac_addr[0] = rtl8101_eri_read(baseAddr, 0xE0, 4, ERIAR_ExGMAC);
        *(u16*)&mac_addr[2] = rtl8101_eri_read(baseAddr, 0xE4, 2, ERIAR_ExGMAC);
    } else {
        if (tp->eeprom_type != EEPROM_TYPE_NONE) {
            /* Get MAC address from EEPROM */
            mac_addr[0] = rtl_eeprom_read_sc(tp, 7);
            mac_addr[1] = rtl_eeprom_read_sc(tp, 8);
            mac_addr[2] = rtl_eeprom_read_sc(tp, 9);
            WriteReg8(Cfg9346, Cfg9346_Unlock);
            WriteReg32(MAC0, (mac_addr[1] << 16) | mac_addr[0]);
            WriteReg16(MAC4, mac_addr[2]);
            WriteReg8(Cfg9346, Cfg9346_Lock);
        }
    }
	for (i = 0; i < MAC_ADDR_LEN; i++) {
		currMacAddr.bytes[i] = ReadReg8(MAC0 + i);
		origMacAddr.bytes[i] = currMacAddr.bytes[i]; /* keep the original MAC address */
	}
    IOLog("Ethernet [RealtekRTL8100]: %s: (Chipset %d) at 0x%lx, %2.2x:%2.2x:%2.2x:%2.2x:%2.2x:%2.2x\n",
          rtl_chip_info[tp->chipset].name, tp->chipset, (unsigned long)baseAddr,
          origMacAddr.bytes[0], origMacAddr.bytes[1],
          origMacAddr.bytes[2], origMacAddr.bytes[3],
          origMacAddr.bytes[4], origMacAddr.bytes[5]);
    
    tp->cp_cmd = ReadReg16(CPlusCmd);
    
    intrMask = (revision2) ? (SYSErr | LinkChg | RxDescUnavail | TxErr | TxOK | RxErr | RxOK) : (SYSErr | RxDescUnavail | TxErr | TxOK | RxErr | RxOK);
    
    /* Get the RxConfig parameters. */
    rxConfigMask = rtl_chip_info[tp->chipset].RxConfigMask;
    result = true;
    
done:
    return result;
}

void RTL8100::enableRTL8100()
{
    struct rtl8101_private *tp = &linuxData;
    
    rtl8101_exit_oob(tp);
    rtl8101_hw_init(tp);
    rtl8101_nic_reset(tp);
    rtl8101_powerup_pll(tp);
    rtl8101_hw_ephy_config(tp);
    rtl8101_hw_phy_config(tp);
	startRTL8100();
	rtl8101_dsm(tp, DSM_IF_UP);
	rtl8101_set_speed(tp, autoneg, speed, duplex);
}

void RTL8100::disableRTL8100()
{
    struct rtl8101_private *tp = &linuxData;
    
	rtl8101_dsm(tp, DSM_IF_DOWN);
    
    /* Disable all interrupts by clearing the interrupt mask. */
    WriteReg16(IntrMask, 0);
    WriteReg16(IntrStatus, ReadReg16(IntrStatus));

    rtl8101_nic_reset(tp);
    rtl8101_hw_d3_para(tp);
	powerdownPLL();
    rtl8101_set_bios_setting(tp);
}

/* Reset the NIC in case a tx deadlock or a pci error occurred. timerSource and txQueue
 * are stopped immediately but will be restarted by checkLinkStatus() when the link has
 * been reestablished.
 */

void RTL8100::restartRTL8100()
{
    /* Stop and cleanup txQueue. Also set the link status to down. */
    txQueue->stop();
    txQueue->flush();
    linkUp = false;
    setLinkStatus(kIONetworkLinkValid);
    
    /* Reset NIC and cleanup both descriptor rings. */
    rtl8101_nic_reset(&linuxData);
    txClearDescriptors(true);
    rxInterrupt();
    rxNextDescIndex = 0;
    deadlockWarn = 0;
    
    /* Reinitialize NIC. */
    enableRTL8100();
}

/* This is a rewrite of the linux driver's rtl8101_hw_start() routine. */

void RTL8100::startRTL8100()
{
    struct rtl8101_private *tp = &linuxData;
    UInt32 csi_tmp;
    UInt8 link_control, options1, options2;
    bool wol;
    
    WriteReg32(RxConfig, (RX_DMA_BURST << RxCfgDMAShift));
    
    rtl8101_nic_reset(tp);
    
    WriteReg8(Cfg9346, Cfg9346_Unlock);
    switch (tp->mcfg) {
        case CFG_METHOD_10:
        case CFG_METHOD_11:
        case CFG_METHOD_12:
        case CFG_METHOD_13:
        case CFG_METHOD_14:
        case CFG_METHOD_15:
        case CFG_METHOD_16:
        case CFG_METHOD_17:
            WriteReg8(Config5, ReadReg8(Config5) & ~BIT_0);
            WriteReg8(Config2, ReadReg8(Config2) & ~BIT_7);
            WriteReg8(0xF1, ReadReg8(0xF1) & ~BIT_7);
            break;
    }
    WriteReg8(MTPS, Reserved1_data);
    
    /* Set DMA burst size and Interframe Gap Time */
    WriteReg32(TxConfig, (TX_DMA_BURST << TxDMAShift) |
            (InterFrameGap << TxInterFrameGapShift));
    
    tp->cp_cmd &= 0x2063;
    
    WriteReg16(IntrMitigate, intrMitigateValue);
        
    fillDescriptorAddr(baseAddr, txPhyAddr, rxPhyAddr);
    
    if (tp->mcfg == CFG_METHOD_4) {
        set_offset70F(tp, 0x17);
        setOffset79(0x50);
        
        link_control = pciDevice->configRead8(0x81);
        
        if (link_control == 1) {
            pciDevice->configWrite8(0x81, 0);
            
            WriteReg8(DBG_reg, 0x98);
            WriteReg8(Config2, ReadReg8(Config2) | BIT_7);
            WriteReg8(Config4, ReadReg8(Config4) | BIT_2);
            
            pciDevice->configWrite8(0x81, 1);
        }
        WriteReg8(Config1, 0x0f);
        WriteReg8(Config3, ReadReg8(Config3) & ~Beacon_en);
        
    } else if (tp->mcfg == CFG_METHOD_5) {
        link_control = pciDevice->configRead8(0x81);
        
        if (link_control == 1) {
            pciDevice->configWrite8(0x81, 0);
            
            WriteReg8(DBG_reg, 0x98);
            WriteReg8(Config2, ReadReg8(Config2) | BIT_7);
            WriteReg8(Config4, ReadReg8(Config4) | BIT_2);
            
            pciDevice->configWrite8(0x81, 1);
        }
        setOffset79(0x50);
        WriteReg8(Config1, 0x0f);
        WriteReg8(Config3, ReadReg8(Config3) & ~Beacon_en);
        
    } else if (tp->mcfg == CFG_METHOD_6) {
        link_control = pciDevice->configRead8(0x81);
        
        if (link_control == 1) {
            pciDevice->configWrite8(0x81, 0);
            
            WriteReg8(DBG_reg, 0x98);
            WriteReg8(Config2, ReadReg8(Config2) | BIT_7);
            WriteReg8(Config4, ReadReg8(Config4) | BIT_2);
            
            pciDevice->configWrite8(0x81, 1);
        }
        setOffset79(0x50);
        WriteReg8(0xF4, 0x01);
        WriteReg8(Config3, ReadReg8(Config3) & ~Beacon_en);
        
    } else if (tp->mcfg == CFG_METHOD_7) {
        link_control = pciDevice->configRead8(0x81);

        if (link_control == 1) {
            pciDevice->configWrite8(0x81, 0);
            
            WriteReg8(DBG_reg, 0x98);
            WriteReg8(Config2, ReadReg8(Config2) | BIT_7);
            WriteReg8(Config4, ReadReg8(Config4) | BIT_2);
            
            pciDevice->configWrite8(0x81, 1);
        }
        setOffset79(0x50);
        WriteReg8(0xF4, 0x01);
        WriteReg8(Config3, ReadReg8(Config3) & ~Beacon_en);
        WriteReg8(0xF5, ReadReg8(0xF5) | BIT_2);
        
    } else if (tp->mcfg == CFG_METHOD_8) {
        link_control = pciDevice->configRead8(0x81);

        if (link_control == 1) {
            pciDevice->configWrite8(0x81, 0);
            
            WriteReg8(DBG_reg, 0x98);
            WriteReg8(Config2, ReadReg8(Config2) | BIT_7);
            WriteReg8(Config4, ReadReg8(Config4) | BIT_2);
            WriteReg8(0xF4, ReadReg8(0xF4) | BIT_3);
            WriteReg8(0xF5, ReadReg8(0xF5) | BIT_2);
            
            pciDevice->configWrite8(0x81, 1);
            
            if (rtl8101_ephy_read(baseAddr, 0x10)==0x0008) {
                rtl8101_ephy_write(baseAddr, 0x10, 0x000C);
            }
        }
        
        link_control = pciDevice->configRead8(0x80);
        
        if (link_control & 3)
            rtl8101_ephy_write(baseAddr, 0x02, 0x011F);
        
        setOffset79(0x50);
        WriteReg8(0xF4, ReadReg8(0xF4) | BIT_0);
        WriteReg8(Config3, ReadReg8(Config3) & ~Beacon_en);
        
    } else if (tp->mcfg == CFG_METHOD_9) {
        link_control = pciDevice->configRead8(0x81);
        
        if (link_control == 1) {
            pciDevice->configWrite8(0x81, 0);
            
            WriteReg8(DBG_reg, 0x98);
            WriteReg8(Config2, ReadReg8(Config2) | BIT_7);
            WriteReg8(Config4, ReadReg8(Config4) | BIT_2);
            
            pciDevice->configWrite8(0x81, 1);
        }
        setOffset79(0x50);
        WriteReg8(0xF4, 0x01);
        WriteReg8(Config3, ReadReg8(Config3) & ~Beacon_en);
        
    } else if (tp->mcfg == CFG_METHOD_10) {
        set_offset70F(tp, 0x27);
        setOffset79(0x50);
        
        WriteReg8(0xF3, ReadReg8(0xF3) | BIT_5);
        WriteReg8(0xF3, ReadReg8(0xF3) & ~BIT_5);
        WriteReg8(0xD0, ReadReg8(0xD0) | BIT_7 | BIT_6);
        WriteReg8(0xF1, ReadReg8(0xF1) | BIT_6 | BIT_5 | BIT_4 | BIT_2 | BIT_1);
        
        if (tp->aspm)
            WriteReg8(0xF1, ReadReg8(0xF1) | BIT_7);
        
        WriteReg8(Config5, (ReadReg8(Config5)&~0x08) | BIT_0);
        WriteReg8(Config2, ReadReg8(Config2) | BIT_7);
        WriteReg8(Config3, ReadReg8(Config3) & ~Beacon_en);
        
    } else if (tp->mcfg == CFG_METHOD_11 || tp->mcfg == CFG_METHOD_12 ||
               tp->mcfg == CFG_METHOD_13) {
        u8	pci_config;
        
        tp->cp_cmd &= 0x2063;
                
        pci_config = pciDevice->configRead8(0x80);
        
        if (pci_config & 0x03) {
            WriteReg8(Config5, ReadReg8(Config5) | BIT_0);
            WriteReg8(0xF2, ReadReg8(0xF2) | BIT_7);
            
            if (tp->aspm)
                WriteReg8(0xF1, ReadReg8(0xF1) | BIT_7);
            
            WriteReg8(Config2, ReadReg8(Config2) | BIT_7);
        }
        WriteReg8(0xF1, ReadReg8(0xF1) | BIT_5 | BIT_3);
        WriteReg8(0xF2, ReadReg8(0xF2) & ~BIT_0);
        WriteReg8(0xD3, ReadReg8(0xD3) | BIT_3 | BIT_2);
        WriteReg8(0xD0, ReadReg8(0xD0) | BIT_6);
        WriteReg16(0xE0, ReadReg16(0xE0) & ~0xDF9C);
        
        if (tp->mcfg == CFG_METHOD_11)
            WriteReg8(Config5, ReadReg8(Config5) & ~BIT_0);
        
    } else if (tp->mcfg == CFG_METHOD_14) {
        set_offset70F(tp, 0x27);
        setOffset79(0x50);
        
        rtl8101_eri_write(baseAddr, 0xC8, 4, 0x00000002, ERIAR_ExGMAC);
        rtl8101_eri_write(baseAddr, 0xE8, 4, 0x00000006, ERIAR_ExGMAC);
        WriteReg32(TxConfig, ReadReg32(TxConfig) | BIT_7);
        WriteReg8(0xD3, ReadReg8(0xD3) & ~BIT_7);
        csi_tmp = rtl8101_eri_read(baseAddr, 0xDC, 1, ERIAR_ExGMAC);
        csi_tmp &= ~BIT_0;
        rtl8101_eri_write( baseAddr, 0xDC, 1, csi_tmp, ERIAR_ExGMAC);
        csi_tmp |= BIT_0;
        rtl8101_eri_write( baseAddr, 0xDC, 1, csi_tmp, ERIAR_ExGMAC);
        
        rtl8101_ephy_write(baseAddr, 0x19, 0xff64);
        
        WriteReg8(Config5, ReadReg8(Config5) | BIT_0);
        WriteReg8(Config2, ReadReg8(Config2) | BIT_7);
        
        rtl8101_eri_write(baseAddr, 0xC0, 2, 0x00000000, ERIAR_ExGMAC);
        rtl8101_eri_write(baseAddr, 0xB8, 2, 0x00000000, ERIAR_ExGMAC);
        rtl8101_eri_write(baseAddr, 0xD5, 1, 0x0000000E, ERIAR_ExGMAC);
    } else if (tp->mcfg == CFG_METHOD_15 || tp->mcfg == CFG_METHOD_16) {
        u8	pci_config;
        
        tp->cp_cmd &= 0x2063;
                
        pci_config = pciDevice->configRead8(0x80);
        
        if (pci_config & 0x03) {
            WriteReg8(Config5, ReadReg8(Config5) | BIT_0);
            WriteReg8(0xF2, ReadReg8(0xF2) | BIT_7);
            
            if (tp->aspm)
                WriteReg8(0xF1, ReadReg8(0xF1) | BIT_7);
            
            WriteReg8(Config2, ReadReg8(Config2) | BIT_7);
        }
        WriteReg8(0xF1, ReadReg8(0xF1) | BIT_5 | BIT_3);
        WriteReg8(0xF2, ReadReg8(0xF2) & ~BIT_0);
        WriteReg8(0xD3, ReadReg8(0xD3) | BIT_3 | BIT_2);
        WriteReg8(0xD0, ReadReg8(0xD0) & ~BIT_6);
        WriteReg16(0xE0, ReadReg16(0xE0) & ~0xDF9C);
        
    } else if (tp->mcfg == CFG_METHOD_17) {
        u8 data8;
        
        set_offset70F(tp, 0x17);
        setOffset79(0x50);
        
        rtl8101_eri_write(baseAddr, 0xC8, 4, 0x00080002, ERIAR_ExGMAC);
        rtl8101_eri_write(baseAddr, 0xCC, 1, 0x38, ERIAR_ExGMAC);
        rtl8101_eri_write(baseAddr, 0xD0, 1, 0x48, ERIAR_ExGMAC);
        rtl8101_eri_write(baseAddr, 0xE8, 4, 0x00100006, ERIAR_ExGMAC);
        
        WriteReg32(TxConfig, ReadReg32(TxConfig) | BIT_7);
        
        csi_tmp = rtl8101_eri_read(baseAddr, 0xDC, 1, ERIAR_ExGMAC);
        csi_tmp &= ~BIT_0;
        rtl8101_eri_write(baseAddr, 0xDC, 1, csi_tmp, ERIAR_ExGMAC);
        csi_tmp |= BIT_0;
        rtl8101_eri_write(baseAddr, 0xDC, 1, csi_tmp, ERIAR_ExGMAC);
        
        WriteReg8(Config3, ReadReg8(Config3) & ~Beacon_en);
        
        tp->cp_cmd = ReadReg16(CPlusCmd) &
        ~(EnableBist | Macdbgo_oe | Force_halfdup |
          Force_rxflow_en | Force_txflow_en |
          Cxpl_dbg_sel | ASF | PktCntrDisable |
          Macdbgo_sel);
        
        WriteReg8(0x1B, ReadReg8(0x1B) & ~0x07);
        
        WriteReg8(TDFNR, 0x4);
        
        if (tp->aspm)
            WriteReg8(0xF1, ReadReg8(0xF1) | BIT_7);
                
        WriteReg8(0xD0, ReadReg8(0xD0) | BIT_6);
        WriteReg8(0xF2, ReadReg8(0xF2) | BIT_6);
        
        WriteReg8(0xD0, ReadReg8(0xD0) | BIT_7);
        
        rtl8101_eri_write(baseAddr, 0xC0, 2, 0x0000, ERIAR_ExGMAC);
        rtl8101_eri_write(baseAddr, 0xB8, 4, 0x00000000, ERIAR_ExGMAC);
        
        rtl8101_eri_write(baseAddr, 0x5F0, 2, 0x4f87, ERIAR_ExGMAC);
        
        csi_tmp = rtl8101_eri_read(baseAddr, 0xD4, 4, ERIAR_ExGMAC);
        csi_tmp  |= ( BIT_7 | BIT_8 | BIT_9 | BIT_10 | BIT_11 | BIT_12 );
        rtl8101_eri_write(baseAddr, 0xD4, 4, csi_tmp, ERIAR_ExGMAC);
        
        csi_tmp = rtl8101_eri_read(baseAddr, 0x1B0, 4, ERIAR_ExGMAC);
        csi_tmp &= ~BIT_12;
        rtl8101_eri_write(baseAddr, 0x1B0, 4, csi_tmp, ERIAR_ExGMAC);
        
        csi_tmp = rtl8101_eri_read(baseAddr, 0x2FC, 1, ERIAR_ExGMAC);
        csi_tmp &= ~(BIT_0 | BIT_1 | BIT_2);
        csi_tmp |= BIT_0;
        rtl8101_eri_write(baseAddr, 0x2FC, 1, csi_tmp, ERIAR_ExGMAC);
        
        csi_tmp = rtl8101_eri_read(baseAddr, 0x1D0, 1, ERIAR_ExGMAC);
        csi_tmp |= BIT_1;
        rtl8101_eri_write(baseAddr, 0x1D0, 1, csi_tmp, ERIAR_ExGMAC);
        
        if (tp->aspm) {
            csi_tmp = rtl8101_eri_read(baseAddr, 0x3F2, 2, ERIAR_ExGMAC);
            csi_tmp &= ~( BIT_8 | BIT_9  | BIT_10 | BIT_11  | BIT_12  | BIT_13  | BIT_14 | BIT_15 );
            csi_tmp |= ( BIT_9 | BIT_10 | BIT_13  | BIT_14 | BIT_15 );
            rtl8101_eri_write(baseAddr, 0x3F2, 2, csi_tmp, ERIAR_ExGMAC);
            csi_tmp = rtl8101_eri_read(baseAddr, 0x3F5, 1, ERIAR_ExGMAC);
            csi_tmp |= BIT_6 | BIT_7;
            rtl8101_eri_write(baseAddr, 0x3F5, 1, csi_tmp, ERIAR_ExGMAC);
            mac_ocp_write(tp, 0xE02C, 0x1880);
            mac_ocp_write(tp, 0xE02E, 0x4880);
            rtl8101_eri_write(baseAddr, 0x2E8, 2, 0x9003, ERIAR_ExGMAC);
            rtl8101_eri_write(baseAddr, 0x2EA, 2, 0x9003, ERIAR_ExGMAC);
            rtl8101_eri_write(baseAddr, 0x2EC, 2, 0x9003, ERIAR_ExGMAC);
            rtl8101_eri_write(baseAddr, 0x2E2, 2, 0x883C, ERIAR_ExGMAC);
            rtl8101_eri_write(baseAddr, 0x2E4, 2, 0x8C12, ERIAR_ExGMAC);
            rtl8101_eri_write(baseAddr, 0x2E6, 2, 0x9003, ERIAR_ExGMAC);
            csi_tmp = rtl8101_eri_read(baseAddr, 0x3FA, 2, ERIAR_ExGMAC);
            csi_tmp |= BIT_14;
            rtl8101_eri_write(baseAddr, 0x3FA, 2, csi_tmp, ERIAR_ExGMAC);
            csi_tmp = rtl8101_eri_read(baseAddr, 0x3F2, 2, ERIAR_ExGMAC);
            csi_tmp &= ~(BIT_0 | BIT_1);
            csi_tmp |= BIT_0;
            data8 = pciDevice->configRead8(0x99);
            
            if (!(data8 & (BIT_5 | BIT_6)))
                csi_tmp &= ~(BIT_1);
            
            if (!(data8 & BIT_2))
                csi_tmp &= ~(BIT_0 );
            
            rtl8101_eri_write(baseAddr, 0x3F2, 2, csi_tmp, ERIAR_ExGMAC);
            
            data8 = pciDevice->extendedConfigRead8(0x180);
            
            if (data8 & (BIT_0|BIT_1)) {
                csi_tmp = rtl8101_eri_read(baseAddr, 0x1E2, 1, ERIAR_ExGMAC);
                csi_tmp |= BIT_2;
                rtl8101_eri_write(baseAddr, 0x1E2, 1, csi_tmp, ERIAR_ExGMAC);
            } else {
                csi_tmp = rtl8101_eri_read(baseAddr, 0x1E2, 1, ERIAR_ExGMAC);
                csi_tmp &= ~BIT_2;
                rtl8101_eri_write(baseAddr, 0x1E2, 1, csi_tmp, ERIAR_ExGMAC);
            }
        }
    }
    //other hw parameretrs
    if (tp->mcfg == CFG_METHOD_17)
        rtl8101_eri_write(baseAddr, 0x2F8, 2, 0x1D8F, ERIAR_ExGMAC);
    
    if (tp->bios_setting & BIT_28) {
        if (tp->mcfg == CFG_METHOD_13) {
            if (ReadReg8(0xEF) & BIT_2) {
                u32 gphy_val;
                
                spin_lock_irqsave(&tp->phy_lock, flags);
                mdio_write(tp, 0x1F, 0x0001);
                gphy_val = mdio_read(tp, 0x1B);
                gphy_val |= BIT_2;
                mdio_write(tp, 0x1B, gphy_val);
                mdio_write(tp, 0x1F, 0x0000);
                spin_unlock_irqrestore(&tp->phy_lock, flags);
            }
        }
        
        if (tp->mcfg == CFG_METHOD_14) {
            u32 gphy_val;
            
            spin_lock_irqsave(&tp->phy_lock, flags);
            mdio_write(tp, 0x1F, 0x0001);
            gphy_val = mdio_read(tp, 0x13);
            gphy_val |= BIT_15;
            mdio_write(tp, 0x13, gphy_val);
            mdio_write(tp, 0x1F, 0x0000);
            spin_unlock_irqrestore(&tp->phy_lock, flags);
        }
    }
    tp->cp_cmd |= (RxChkSum | RxVlan);
    WriteReg16(CPlusCmd, tp->cp_cmd);
    ReadReg16(CPlusCmd);

    switch (tp->mcfg) {
        case CFG_METHOD_17: {
            int timeout;
            for (timeout = 0; timeout < 10; timeout++) {
                if ((rtl8101_eri_read(baseAddr, 0x1AE, 2, ERIAR_ExGMAC) & BIT_13)==0)
                    break;
                mdelay(1);
            }
        }
            break;
    }
    switch (tp->mcfg) {
        case CFG_METHOD_11:
        case CFG_METHOD_12:
        case CFG_METHOD_13:
        case CFG_METHOD_14:
        case CFG_METHOD_15:
        case CFG_METHOD_16:
        case CFG_METHOD_17:
            WriteReg16(RxMaxSize, 0x05F3);
            break;
            
        default:
            WriteReg16(RxMaxSize, 0x05EF);
            break;
    }
    rtl8101_disable_rxdvgate(tp);
    rtl8101_dsm(tp, DSM_MAC_INIT);
    
    /*
     * Determine the chips WoL capabilities. Most of the code is
     * taken from the linux driver's rtl8101_get_wol() routine.
     */
    options1 = ReadReg8(Config3);
    options2 = ReadReg8(Config5);
        
    if (options1 & LinkUp)
        tp->wol_opts |= WAKE_PHY;
    
    switch (tp->mcfg) {
        case CFG_METHOD_14:
        case CFG_METHOD_17:
            csi_tmp = rtl8101_eri_read(baseAddr, 0xDE, 1, ERIAR_ExGMAC);
            
            if (csi_tmp & BIT_0)
                tp->wol_opts |= WAKE_MAGIC;
            break;
            
        default:
            if (options1 & MagicPacket)
                tp->wol_opts |= WAKE_MAGIC;
            break;
    }
    if (options2 & UWF)
        tp->wol_opts |= WAKE_UCAST;
    
    if (options2 & BWF)
        tp->wol_opts |= WAKE_BCAST;
    
    if (options2 & MWF)
        tp->wol_opts |= WAKE_MCAST;
    
    wol = ((options1 & (LinkUp | MagicPacket)) || (options2 & (UWF | BWF | MWF))) ? true : false;
    
    /* Set wake on LAN support and status. */
    wolCapable = wolCapable && wol;
    tp->wol_enabled = (wolCapable && wolActive) ? WOL_ENABLED : WOL_DISABLED;

    /* Set receiver mode. */
    setMulticastMode(multicastMode);
    
    switch (tp->mcfg) {
        case CFG_METHOD_10:
        case CFG_METHOD_11:
        case CFG_METHOD_12:
        case CFG_METHOD_13:
        case CFG_METHOD_14:
        case CFG_METHOD_15:
        case CFG_METHOD_16:
        case CFG_METHOD_17:
            if (tp->aspm) {
                WriteReg8(Config5, ReadReg8(Config5) | BIT_0);
                WriteReg8(Config2, ReadReg8(Config2) | BIT_7);
            } else {
                WriteReg8(Config5, ReadReg8(Config5) & ~BIT_0);
                WriteReg8(Config2, ReadReg8(Config2) & ~BIT_7);
            }
            break;
    }
    WriteReg8(Cfg9346, Cfg9346_Lock);
    WriteReg8(ChipCmd, CmdTxEnb | CmdRxEnb);
    
    /* Enable all known interrupts by setting the interrupt mask. */
    WriteReg16(IntrMask, intrMask);

    IODelay(10);
}

/* Set PCI configuration space offset 0x79 to setting. */

void RTL8100::setOffset79(UInt8 setting)
{
    UInt8 deviceControl;
    
    DebugLog("setOffset79() ===>\n");
    
    deviceControl = pciDevice->configRead8(0x79);
    deviceControl &= ~0x70;
    deviceControl |= setting;
    pciDevice->configWrite8(0x79, deviceControl);
    
    DebugLog("setOffset79() <===\n");
}

#define WAKE_ANY (WAKE_PHY | WAKE_MAGIC | WAKE_UCAST | WAKE_BCAST | WAKE_MCAST)

static struct {
    u32 opt;
    u16 reg;
    u8  mask;
} cfg[] = {
    { WAKE_ANY,   Config1, PMEnable },
    { WAKE_PHY,   Config3, LinkUp },
    { WAKE_UCAST, Config5, UWF },
    { WAKE_BCAST, Config5, BWF },
    { WAKE_MCAST, Config5, MWF },
    { WAKE_ANY,   Config5, LanWake },
    { WAKE_MAGIC, Config3, MagicPacket }
};

/* This is a rewrite of the linux driver's rtl8101_powerdown_pll() routine. */

void RTL8100::powerdownPLL()
{
    struct rtl8101_private *tp = &linuxData;
    
    if (tp->wol_enabled == WOL_ENABLED) {
        int i,tmp;
        u32 csi_tmp;
        int auto_nego;
        u16 val;
        
        /* The next few lines are from rtl8101_set_wol() of the linux driver... */
        WriteReg8(Cfg9346, Cfg9346_Unlock);
        
        switch (tp->mcfg) {
            case CFG_METHOD_14:
            case CFG_METHOD_17:
                tmp = ARRAY_SIZE(cfg) - 1;
                
                csi_tmp = rtl8101_eri_read(baseAddr, 0xDE, 1, ERIAR_ExGMAC);
                
                if (tp->wol_opts & WAKE_MAGIC)
                    csi_tmp |= BIT_0;
                else
                    csi_tmp &= ~BIT_0;
                rtl8101_eri_write(baseAddr, 0xDE, 1, csi_tmp, ERIAR_ExGMAC);
                break;
                
            default:
                tmp = ARRAY_SIZE(cfg);
                break;
        }
        for (i = 0; i < tmp; i++) {
            u8 options = ReadReg8(cfg[i].reg) & ~cfg[i].mask;
            
            if (tp->wol_opts & cfg[i].opt)
                options |= cfg[i].mask;
            
            WriteReg8(cfg[i].reg, options);
        }
        WriteReg8(Cfg9346, Cfg9346_Lock);

        /* ...up to this point. */
        
        if (tp->mcfg == CFG_METHOD_17) {
            WriteReg8(Cfg9346, Cfg9346_Unlock);
            WriteReg8(Config2, ReadReg8(Config2) | PMSTS_En);
            WriteReg8(Cfg9346, Cfg9346_Lock);
        }
        mdio_write(tp, 0x1F, 0x0000);
        auto_nego = mdio_read(tp, MII_ADVERTISE);
        auto_nego &= ~(ADVERTISE_10HALF | ADVERTISE_10FULL
                       | ADVERTISE_100HALF | ADVERTISE_100FULL);
        
        val = mdio_read(tp, MII_LPA);
        
        if (val & (LPA_10HALF | LPA_10FULL))
            auto_nego |= (ADVERTISE_10HALF | ADVERTISE_10FULL);
        else
            auto_nego |= (ADVERTISE_100FULL | ADVERTISE_100HALF | ADVERTISE_10HALF | ADVERTISE_10FULL);
        
        mdio_write(tp, MII_ADVERTISE, auto_nego);
        mdio_write(tp, MII_BMCR, BMCR_RESET | BMCR_ANENABLE | BMCR_ANRESTART);
        
        switch (tp->mcfg) {
            case CFG_METHOD_1:
            case CFG_METHOD_2:
            case CFG_METHOD_3:
            case CFG_METHOD_4:
            case CFG_METHOD_5:
            case CFG_METHOD_6:
            case CFG_METHOD_7:
            case CFG_METHOD_8:
            case CFG_METHOD_9:
                break;
                
            default:
                WriteReg32(RxConfig, ReadReg32(RxConfig) | AcceptBroadcast | AcceptMulticast | AcceptMyPhys);
                break;
        }
        return;
    }
    rtl8101_phy_power_down(tp);
    
    switch (tp->mcfg) {
        case CFG_METHOD_6:
        case CFG_METHOD_9:
            WriteReg8(DBG_reg, ReadReg8(DBG_reg) | BIT_3);
            WriteReg8(PMCH, ReadReg8(PMCH) & ~BIT_7);
            break;
            
        case CFG_METHOD_8:
            pciDevice->configWrite8(0x81, 0);
            WriteReg8(PMCH, ReadReg8(PMCH) & ~BIT_7);
            break;
            
        case CFG_METHOD_7:
        case CFG_METHOD_10:
        case CFG_METHOD_11:
        case CFG_METHOD_12:
        case CFG_METHOD_13:
        case CFG_METHOD_14:
        case CFG_METHOD_15:
        case CFG_METHOD_16:
        case CFG_METHOD_17:
            WriteReg8(PMCH, ReadReg8(PMCH) & ~BIT_7);
            break;
            
        default:
            break;
    }
}

#pragma mark --- RTL8100 specific methods ---

/* 
 * This is the timer action routine. Its basic tasks are to:
 *  - check for link status changes.
 *  - check for transmitter deadlocks.
 *  - trigger statistics dumps.
 */

void RTL8100::timerActionRTL8100(IOTimerEventSource *timer)
{
    struct rtl8101_private *tp = &linuxData;
    UInt32 data32;
	UInt8 currLinkState;
    bool newLinkState;
    
    //DebugLog("timerActionRTL8100() ===>\n");

    /* 
     * As the link status change interrupt of some family members is broken,
     * we have to check for link changes periodically.
     * 
     * Most of the code here is taken as is from the linux driver's
     * rtl8101_check_link_status() routine. 
     */
    
    currLinkState = ReadReg8(PHYstatus);
	newLinkState = (currLinkState & LinkStatus) ? true : false;
    
    if (newLinkState != linkUp) {
        if (newLinkState) {
            if (tp->mcfg == CFG_METHOD_5 || tp->mcfg == CFG_METHOD_6 ||
                tp->mcfg == CFG_METHOD_7 || tp->mcfg == CFG_METHOD_8) {
                set_offset70F(tp, 0x3F);
            
            } else if (tp->mcfg == CFG_METHOD_11 || tp->mcfg == CFG_METHOD_12 ||
                tp->mcfg == CFG_METHOD_13) {
                if ((currLinkState & FullDup) == 0 && eee_enable == 1)
                    rtl8101_disable_EEE(tp);
                
                if (currLinkState & _10bps) {
                    rtl8101_eri_write(baseAddr, 0x1D0, 2, 0x4D02, ERIAR_ExGMAC);
                    rtl8101_eri_write(baseAddr, 0x1DC, 2, 0x0060, ERIAR_ExGMAC);
                    
                    rtl8101_eri_write(baseAddr, 0x1B0, 2, 0, ERIAR_ExGMAC);
                    mdio_write( tp, 0x1F, 0x0004);
                    data32 = mdio_read( tp, 0x10);
                    data32 |= 0x0400;
                    data32 &= ~0x0800;
                    mdio_write(tp, 0x10, data32);
                    mdio_write(tp, 0x1F, 0x0000);
                } else {
                    rtl8101_eri_write(baseAddr, 0x1D0, 2, 0, ERIAR_ExGMAC);
                    if ( eee_enable == 1 && (ReadReg8(0xEF) & BIT_0) == 0)
                        rtl8101_eri_write(baseAddr, 0x1B0, 2, 0xED03, ERIAR_ExGMAC);
                }
            } else if (tp->mcfg == CFG_METHOD_14 || tp->mcfg == CFG_METHOD_15 ||
                       tp->mcfg == CFG_METHOD_16) {
                if (currLinkState & _10bps) {
                    rtl8101_eri_write(baseAddr, 0x1D0, 2, 0x4d02, ERIAR_ExGMAC);
                    rtl8101_eri_write(baseAddr, 0x1DC, 2, 0x0060, ERIAR_ExGMAC);
                } else {
                    rtl8101_eri_write(baseAddr, 0x1D0, 2, 0, ERIAR_ExGMAC);
                }
            }
            setLinkUp(currLinkState);            
            WriteReg8(ChipCmd, CmdRxEnb | CmdTxEnb);
        } else {
            if (tp->mcfg == CFG_METHOD_11 || tp->mcfg == CFG_METHOD_12 ||
                tp->mcfg == CFG_METHOD_13) {
                mdio_write(tp, 0x1F, 0x0004);
                data32 = mdio_read( tp, 0x10);
                data32 &= ~0x0C00;
                mdio_write(tp, 0x1F, 0x0000);
            }
            setLinkDown();
        }
    }
    /*
     * The name suggests it has something to do with ASPM but what does it do excatly?
     * Probably someone at Realtek knows but is unwilling to share his wisdom with us.
     */
    switch (tp->mcfg) {
        case CFG_METHOD_4:
            rtl8101_aspm_fix1(tp);
            break;
    }
    /* Check for tx deadlock. */
    if (linkUp) {
        if (checkForDeadlock())
            goto done;
        
        dumpTallyCounter();
    }
    /*
     * We can savely free the mbuf here because the timer action gets called
     * synchronized to the workloop. See txInterrupt() for the details.
     */
    if (txNext2FreeMbuf) {
        freePacket(txNext2FreeMbuf);
        txNext2FreeMbuf = NULL;
    }
    
done:
    timerSource->setTimeoutMS(kTimeoutMS);
    txDescDoneLast = txDescDoneCount;
    
    //DebugLog("timerActionRTL8100() <===\n");
}

#pragma mark --- miscellaneous functions ---

static inline void fillDescriptorAddr(volatile void *baseAddr, IOPhysicalAddress64 txPhyAddr, IOPhysicalAddress64 rxPhyAddr)
{
    WriteReg32(TxDescStartAddrLow, (UInt32)(txPhyAddr & 0x00000000ffffffff));
    WriteReg32(TxDescStartAddrHigh, (UInt32)(txPhyAddr >> 32));
    WriteReg32(RxDescAddrLow, (UInt32)(rxPhyAddr & 0x00000000ffffffff));
    WriteReg32(RxDescAddrHigh, (UInt32)(rxPhyAddr >> 32));
}

static unsigned const ethernet_polynomial = 0x04c11db7U;

static inline u32 ether_crc(int length, unsigned char *data)
{
    int crc = -1;
    
    while(--length >= 0) {
        unsigned char current_octet = *data++;
        int bit;
        for (bit = 0; bit < 8; bit++, current_octet >>= 1) {
            crc = (crc << 1) ^
            ((crc < 0) ^ (current_octet & 1) ? ethernet_polynomial : 0);
        }
    }
    return crc;
}


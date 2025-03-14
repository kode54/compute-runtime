/*
 * Copyright (C) 2020-2023 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 */

#include "level_zero/core/source/context/context_imp.h"

#include "shared/source/command_container/implicit_scaling.h"
#include "shared/source/command_stream/command_stream_receiver.h"
#include "shared/source/execution_environment/root_device_environment.h"
#include "shared/source/helpers/basic_math.h"
#include "shared/source/helpers/gfx_core_helper.h"
#include "shared/source/memory_manager/allocation_properties.h"
#include "shared/source/memory_manager/memory_operations_handler.h"
#include "shared/source/memory_manager/unified_memory_manager.h"

#include "level_zero/api/driver_experimental/public/zex_memory.h"
#include "level_zero/core/source/cmdlist/cmdlist.h"
#include "level_zero/core/source/device/device_imp.h"
#include "level_zero/core/source/driver/driver_handle_imp.h"
#include "level_zero/core/source/event/event.h"
#include "level_zero/core/source/gfx_core_helpers/l0_gfx_core_helper.h"
#include "level_zero/core/source/helpers/properties_parser.h"
#include "level_zero/core/source/image/image.h"
#include "level_zero/core/source/memory/memory_operations_helper.h"
#include "level_zero/core/source/module/module.h"

namespace L0 {

ze_result_t ContextImp::destroy() {
    while (driverHandle->svmAllocsManager->getNumDeferFreeAllocs() > 0) {
        this->driverHandle->svmAllocsManager->freeSVMAllocDeferImpl();
    }
    delete this;

    return ZE_RESULT_SUCCESS;
}

ze_result_t ContextImp::getStatus() {
    DriverHandleImp *driverHandleImp = static_cast<DriverHandleImp *>(this->driverHandle);
    for (auto device : driverHandleImp->devices) {
        DeviceImp *deviceImp = static_cast<DeviceImp *>(device);
        if (deviceImp->resourcesReleased) {
            return ZE_RESULT_ERROR_DEVICE_LOST;
        }
    }
    return ZE_RESULT_SUCCESS;
}

DriverHandle *ContextImp::getDriverHandle() {
    return this->driverHandle;
}

ContextImp::ContextImp(DriverHandle *driverHandle) {
    this->driverHandle = static_cast<DriverHandleImp *>(driverHandle);
}

ze_result_t ContextImp::allocHostMem(const ze_host_mem_alloc_desc_t *hostDesc,
                                     size_t size,
                                     size_t alignment,
                                     void **ptr) {
    if (NEO::DebugManager.flags.ForceExtendedUSMBufferSize.get() >= 1) {
        size += (MemoryConstants::pageSize * NEO::DebugManager.flags.ForceExtendedUSMBufferSize.get());
    }

    bool relaxedSizeAllowed = NEO::DebugManager.flags.AllowUnrestrictedSize.get();
    if (hostDesc->pNext) {
        const ze_base_desc_t *extendedDesc = reinterpret_cast<const ze_base_desc_t *>(hostDesc->pNext);
        if (extendedDesc->stype == ZE_STRUCTURE_TYPE_RELAXED_ALLOCATION_LIMITS_EXP_DESC) {
            const ze_relaxed_allocation_limits_exp_desc_t *relaxedLimitsDesc =
                reinterpret_cast<const ze_relaxed_allocation_limits_exp_desc_t *>(extendedDesc);
            if (!(relaxedLimitsDesc->flags & ZE_RELAXED_ALLOCATION_LIMITS_EXP_FLAG_MAX_SIZE)) {
                return ZE_RESULT_ERROR_INVALID_ARGUMENT;
            }
            relaxedSizeAllowed = true;
        }
    }

    if (relaxedSizeAllowed == false &&
        (size > this->driverHandle->devices[0]->getNEODevice()->getDeviceInfo().maxMemAllocSize)) {
        *ptr = nullptr;
        return ZE_RESULT_ERROR_UNSUPPORTED_SIZE;
    }

    StructuresLookupTable lookupTable = {};

    lookupTable.relaxedSizeAllowed = NEO::DebugManager.flags.AllowUnrestrictedSize.get();
    auto parseResult = prepareL0StructuresLookupTable(lookupTable, hostDesc->pNext);

    if (parseResult != ZE_RESULT_SUCCESS) {
        return parseResult;
    }

    if (lookupTable.isSharedHandle) {
        if (lookupTable.sharedHandleType.isDMABUFHandle) {
            ze_ipc_memory_flags_t flags = {};
            *ptr = getMemHandlePtr(this->devices.begin()->second,
                                   lookupTable.sharedHandleType.fd,
                                   NEO::AllocationType::BUFFER_HOST_MEMORY,
                                   flags);
            if (nullptr == *ptr) {
                return ZE_RESULT_ERROR_INVALID_ARGUMENT;
            }
        } else {
            UNRECOVERABLE_IF(!lookupTable.sharedHandleType.isNTHandle);
            *ptr = this->driverHandle->importNTHandle(this->devices.begin()->second,
                                                      lookupTable.sharedHandleType.ntHnadle,
                                                      NEO::AllocationType::BUFFER_HOST_MEMORY);
            if (*ptr == nullptr) {
                return ZE_RESULT_ERROR_INVALID_ARGUMENT;
            }
        }
        return ZE_RESULT_SUCCESS;
    }

    NEO::SVMAllocsManager::UnifiedMemoryProperties unifiedMemoryProperties(InternalMemoryType::HOST_UNIFIED_MEMORY,
                                                                           alignment,
                                                                           this->rootDeviceIndices,
                                                                           this->deviceBitfields);

    if (hostDesc->flags & ZE_HOST_MEM_ALLOC_FLAG_BIAS_UNCACHED) {
        unifiedMemoryProperties.allocationFlags.flags.locallyUncachedResource = 1;
    }

    if (hostDesc->flags & ZEX_HOST_MEM_ALLOC_FLAG_USE_HOST_PTR) {
        unifiedMemoryProperties.allocationFlags.hostptr = reinterpret_cast<uintptr_t>(*ptr);
    }

    auto usmPtr = this->driverHandle->svmAllocsManager->createHostUnifiedMemoryAllocation(size,
                                                                                          unifiedMemoryProperties);
    if (usmPtr == nullptr) {
        if (driverHandle->svmAllocsManager->getNumDeferFreeAllocs() > 0) {
            this->driverHandle->svmAllocsManager->freeSVMAllocDeferImpl();
            usmPtr = this->driverHandle->svmAllocsManager->createHostUnifiedMemoryAllocation(size,
                                                                                             unifiedMemoryProperties);
            if (usmPtr) {
                *ptr = usmPtr;
                return ZE_RESULT_SUCCESS;
            }
        }
        return ZE_RESULT_ERROR_OUT_OF_HOST_MEMORY;
    }

    *ptr = usmPtr;

    return ZE_RESULT_SUCCESS;
}

bool ContextImp::isDeviceDefinedForThisContext(Device *inDevice) {
    uint32_t deviceIndex = inDevice->getRootDeviceIndex();
    return (this->getDevices().find(deviceIndex) != this->getDevices().end());
}

ze_result_t ContextImp::allocDeviceMem(ze_device_handle_t hDevice,
                                       const ze_device_mem_alloc_desc_t *deviceDesc,
                                       size_t size,
                                       size_t alignment, void **ptr) {
    if (NEO::DebugManager.flags.ForceExtendedUSMBufferSize.get() >= 1) {
        size += (MemoryConstants::pageSize * NEO::DebugManager.flags.ForceExtendedUSMBufferSize.get());
    }

    auto device = Device::fromHandle(hDevice);
    if (isDeviceDefinedForThisContext(device) == false) {
        return ZE_RESULT_ERROR_DEVICE_LOST;
    }

    StructuresLookupTable lookupTable = {};

    lookupTable.relaxedSizeAllowed = NEO::DebugManager.flags.AllowUnrestrictedSize.get();
    auto parseResult = prepareL0StructuresLookupTable(lookupTable, deviceDesc->pNext);

    if (parseResult != ZE_RESULT_SUCCESS) {
        return parseResult;
    }

    auto neoDevice = device->getNEODevice();
    auto rootDeviceIndex = neoDevice->getRootDeviceIndex();
    auto deviceBitfields = this->driverHandle->deviceBitfields;

    deviceBitfields[rootDeviceIndex] = neoDevice->getDeviceBitfield();

    if (lookupTable.isSharedHandle) {
        if (lookupTable.sharedHandleType.isDMABUFHandle) {
            ze_ipc_memory_flags_t flags = {};
            *ptr = getMemHandlePtr(hDevice,
                                   lookupTable.sharedHandleType.fd,
                                   NEO::AllocationType::BUFFER,
                                   flags);
            if (nullptr == *ptr) {
                return ZE_RESULT_ERROR_INVALID_ARGUMENT;
            }
        } else {
            UNRECOVERABLE_IF(!lookupTable.sharedHandleType.isNTHandle);
            *ptr = this->driverHandle->importNTHandle(hDevice,
                                                      lookupTable.sharedHandleType.ntHnadle,
                                                      NEO::AllocationType::BUFFER);
            if (*ptr == nullptr) {
                return ZE_RESULT_ERROR_INVALID_ARGUMENT;
            }
        }
        return ZE_RESULT_SUCCESS;
    }

    if (lookupTable.relaxedSizeAllowed == false &&
        (size > neoDevice->getDeviceInfo().maxMemAllocSize)) {
        *ptr = nullptr;
        return ZE_RESULT_ERROR_UNSUPPORTED_SIZE;
    }

    uint64_t globalMemSize = neoDevice->getDeviceInfo().globalMemSize;

    uint32_t numSubDevices = neoDevice->getNumGenericSubDevices();
    if ((!device->isImplicitScalingCapable()) && (numSubDevices > 1)) {
        globalMemSize = globalMemSize / numSubDevices;
    }
    if (lookupTable.relaxedSizeAllowed && (size > globalMemSize)) {
        *ptr = nullptr;
        return ZE_RESULT_ERROR_UNSUPPORTED_SIZE;
    }

    deviceBitfields[rootDeviceIndex] = neoDevice->getDeviceBitfield();
    NEO::SVMAllocsManager::UnifiedMemoryProperties unifiedMemoryProperties(InternalMemoryType::DEVICE_UNIFIED_MEMORY, alignment, this->driverHandle->rootDeviceIndices, deviceBitfields);
    unifiedMemoryProperties.allocationFlags.flags.shareable = isShareableMemory(deviceDesc->pNext, static_cast<uint32_t>(lookupTable.exportMemory), neoDevice);
    unifiedMemoryProperties.device = neoDevice;
    unifiedMemoryProperties.allocationFlags.flags.compressedHint = isAllocationSuitableForCompression(lookupTable, *device, size);

    if (deviceDesc->flags & ZE_DEVICE_MEM_ALLOC_FLAG_BIAS_UNCACHED) {
        unifiedMemoryProperties.allocationFlags.flags.locallyUncachedResource = 1;
    }

    if (lookupTable.rayTracingMemory == true) {
        auto &productHelper = neoDevice->getProductHelper();
        unifiedMemoryProperties.allocationFlags.flags.resource48Bit = productHelper.is48bResourceNeededForRayTracing();
    }

    void *usmPtr =
        this->driverHandle->svmAllocsManager->createUnifiedMemoryAllocation(size, unifiedMemoryProperties);
    if (usmPtr == nullptr) {
        if (driverHandle->svmAllocsManager->getNumDeferFreeAllocs() > 0) {
            this->driverHandle->svmAllocsManager->freeSVMAllocDeferImpl();
            usmPtr =
                this->driverHandle->svmAllocsManager->createUnifiedMemoryAllocation(size, unifiedMemoryProperties);
            if (usmPtr) {
                *ptr = usmPtr;
                return ZE_RESULT_SUCCESS;
            }
        }
        return ZE_RESULT_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    *ptr = usmPtr;

    return ZE_RESULT_SUCCESS;
}

ze_result_t ContextImp::allocSharedMem(ze_device_handle_t hDevice,
                                       const ze_device_mem_alloc_desc_t *deviceDesc,
                                       const ze_host_mem_alloc_desc_t *hostDesc,
                                       size_t size,
                                       size_t alignment,
                                       void **ptr) {
    if (NEO::DebugManager.flags.ForceExtendedUSMBufferSize.get() >= 1) {
        size += (MemoryConstants::pageSize * NEO::DebugManager.flags.ForceExtendedUSMBufferSize.get());
    }

    auto device = Device::fromHandle(this->devices.begin()->second);
    if (hDevice != nullptr) {
        device = Device::fromHandle(hDevice);
    }
    auto neoDevice = device->getNEODevice();

    bool relaxedSizeAllowed = NEO::DebugManager.flags.AllowUnrestrictedSize.get();
    bool rayTracingAllocation = false;

    if (deviceDesc->pNext) {
        const ze_base_desc_t *extendedDesc = reinterpret_cast<const ze_base_desc_t *>(deviceDesc->pNext);
        if (extendedDesc->stype == ZE_STRUCTURE_TYPE_RELAXED_ALLOCATION_LIMITS_EXP_DESC) {
            const ze_relaxed_allocation_limits_exp_desc_t *relaxedLimitsDesc =
                reinterpret_cast<const ze_relaxed_allocation_limits_exp_desc_t *>(extendedDesc);
            if (!(relaxedLimitsDesc->flags & ZE_RELAXED_ALLOCATION_LIMITS_EXP_FLAG_MAX_SIZE)) {
                return ZE_RESULT_ERROR_INVALID_ARGUMENT;
            }
            relaxedSizeAllowed = true;
        } else if (extendedDesc->stype == ZE_STRUCTURE_TYPE_RAYTRACING_MEM_ALLOC_EXT_DESC) {
            rayTracingAllocation = true;
        }
    }

    if (relaxedSizeAllowed == false &&
        (size > neoDevice->getDeviceInfo().maxMemAllocSize)) {
        *ptr = nullptr;
        return ZE_RESULT_ERROR_UNSUPPORTED_SIZE;
    }

    uint64_t globalMemSize = neoDevice->getDeviceInfo().globalMemSize;

    uint32_t numSubDevices = neoDevice->getNumGenericSubDevices();
    if ((!device->isImplicitScalingCapable()) && (numSubDevices > 1)) {
        globalMemSize = globalMemSize / numSubDevices;
    }
    if (relaxedSizeAllowed &&
        (size > globalMemSize)) {
        *ptr = nullptr;
        return ZE_RESULT_ERROR_UNSUPPORTED_SIZE;
    }

    auto deviceBitfields = this->deviceBitfields;
    NEO::Device *unifiedMemoryPropertiesDevice = nullptr;
    if (hDevice) {
        device = Device::fromHandle(hDevice);
        if (isDeviceDefinedForThisContext(device) == false) {
            return ZE_RESULT_ERROR_DEVICE_LOST;
        }

        neoDevice = device->getNEODevice();
        auto rootDeviceIndex = neoDevice->getRootDeviceIndex();
        unifiedMemoryPropertiesDevice = neoDevice;
        deviceBitfields[rootDeviceIndex] = neoDevice->getDeviceBitfield();
    }

    NEO::SVMAllocsManager::UnifiedMemoryProperties unifiedMemoryProperties(InternalMemoryType::SHARED_UNIFIED_MEMORY,
                                                                           alignment,
                                                                           this->rootDeviceIndices,
                                                                           deviceBitfields);
    unifiedMemoryProperties.device = unifiedMemoryPropertiesDevice;

    if (deviceDesc->flags & ZE_DEVICE_MEM_ALLOC_FLAG_BIAS_UNCACHED) {
        unifiedMemoryProperties.allocationFlags.flags.locallyUncachedResource = 1;
    }

    if (deviceDesc->flags & ZE_DEVICE_MEM_ALLOC_FLAG_BIAS_INITIAL_PLACEMENT) {
        unifiedMemoryProperties.allocationFlags.allocFlags.usmInitialPlacementGpu = 1;
    }

    if (hostDesc->flags & ZE_HOST_MEM_ALLOC_FLAG_BIAS_INITIAL_PLACEMENT) {
        unifiedMemoryProperties.allocationFlags.allocFlags.usmInitialPlacementCpu = 1;
    }

    if (rayTracingAllocation) {
        auto &productHelper = neoDevice->getProductHelper();
        unifiedMemoryProperties.allocationFlags.flags.resource48Bit = productHelper.is48bResourceNeededForRayTracing();
    }

    if (hostDesc->flags & ZEX_HOST_MEM_ALLOC_FLAG_USE_HOST_PTR) {
        unifiedMemoryProperties.allocationFlags.hostptr = reinterpret_cast<uintptr_t>(*ptr);
    }

    auto usmPtr = this->driverHandle->svmAllocsManager->createSharedUnifiedMemoryAllocation(size,
                                                                                            unifiedMemoryProperties,
                                                                                            static_cast<void *>(neoDevice->getSpecializedDevice<L0::Device>()));
    if (usmPtr == nullptr) {
        if (driverHandle->svmAllocsManager->getNumDeferFreeAllocs() > 0) {
            this->driverHandle->svmAllocsManager->freeSVMAllocDeferImpl();
            usmPtr = this->driverHandle->svmAllocsManager->createSharedUnifiedMemoryAllocation(size,
                                                                                               unifiedMemoryProperties,
                                                                                               static_cast<void *>(neoDevice->getSpecializedDevice<L0::Device>()));
            if (usmPtr) {
                *ptr = usmPtr;
                return ZE_RESULT_SUCCESS;
            }
        }
        return ZE_RESULT_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    *ptr = usmPtr;

    return ZE_RESULT_SUCCESS;
}

void ContextImp::freePeerAllocations(const void *ptr, bool blocking, Device *device) {
    DeviceImp *deviceImp = static_cast<DeviceImp *>(device);

    std::unique_lock<NEO::SpinLock> lock(deviceImp->peerAllocationsMutex);

    auto iter = deviceImp->peerAllocations.allocations.find(ptr);
    if (iter != deviceImp->peerAllocations.allocations.end()) {
        auto peerAllocData = &iter->second;
        auto peerAlloc = peerAllocData->gpuAllocations.getDefaultGraphicsAllocation();
        auto peerPtr = reinterpret_cast<void *>(peerAlloc->getGpuAddress());
        if (peerAllocData->mappedAllocData) {
            auto gpuAllocations = peerAllocData->gpuAllocations;
            for (const auto &graphicsAllocation : gpuAllocations.getGraphicsAllocations()) {
                this->driverHandle->getMemoryManager()->freeGraphicsMemory(graphicsAllocation);
            }
        } else {
            this->driverHandle->svmAllocsManager->freeSVMAlloc(peerPtr, blocking);
        }
        deviceImp->peerAllocations.allocations.erase(iter);
    }

    for (auto subDevice : deviceImp->subDevices) {
        this->freePeerAllocations(ptr, blocking, subDevice);
    }
}

ze_result_t ContextImp::freeMem(const void *ptr) {
    return this->freeMem(ptr, false);
}

ze_result_t ContextImp::freeMem(const void *ptr, bool blocking) {
    auto allocation = this->driverHandle->svmAllocsManager->getSVMAlloc(ptr);
    if (allocation == nullptr) {
        return ZE_RESULT_ERROR_INVALID_ARGUMENT;
    }

    for (auto pairDevice : this->devices) {
        this->freePeerAllocations(ptr, blocking, Device::fromHandle(pairDevice.second));
    }
    this->driverHandle->svmAllocsManager->freeSVMAlloc(const_cast<void *>(ptr), blocking);

    std::map<uint64_t, IpcHandleTracking *>::iterator ipcHandleIterator;
    auto lockIPC = this->lockIPCHandleMap();
    ipcHandleIterator = this->getIPCHandleMap().begin();
    while (ipcHandleIterator != this->getIPCHandleMap().end()) {
        if (ipcHandleIterator->second->ptr == reinterpret_cast<uint64_t>(ptr)) {
            delete ipcHandleIterator->second;
            this->getIPCHandleMap().erase(ipcHandleIterator->first);
            break;
        }
        ipcHandleIterator++;
    }
    return ZE_RESULT_SUCCESS;
}

ze_result_t ContextImp::freeMemExt(const ze_memory_free_ext_desc_t *pMemFreeDesc,
                                   void *ptr) {

    if (pMemFreeDesc->freePolicy == ZE_DRIVER_MEMORY_FREE_POLICY_EXT_FLAG_BLOCKING_FREE) {
        return this->freeMem(ptr, true);
    }
    if (pMemFreeDesc->freePolicy == ZE_DRIVER_MEMORY_FREE_POLICY_EXT_FLAG_DEFER_FREE) {
        auto allocation = this->driverHandle->svmAllocsManager->getSVMAlloc(ptr);
        if (allocation == nullptr) {
            return ZE_RESULT_ERROR_INVALID_ARGUMENT;
        }

        for (auto pairDevice : this->devices) {
            this->freePeerAllocations(ptr, false, Device::fromHandle(pairDevice.second));
        }

        this->driverHandle->svmAllocsManager->freeSVMAllocDefer(const_cast<void *>(ptr));
        return ZE_RESULT_SUCCESS;
    }
    return this->freeMem(ptr, false);
}

ze_result_t ContextImp::makeMemoryResident(ze_device_handle_t hDevice, void *ptr, size_t size) {
    Device *device = L0::Device::fromHandle(hDevice);
    NEO::Device *neoDevice = device->getNEODevice();
    auto allocation = device->getDriverHandle()->getDriverSystemMemoryAllocation(
        ptr,
        size,
        neoDevice->getRootDeviceIndex(),
        nullptr);
    if (allocation == nullptr) {
        return ZE_RESULT_ERROR_INVALID_ARGUMENT;
    }

    NEO::MemoryOperationsHandler *memoryOperationsIface = neoDevice->getRootDeviceEnvironment().memoryOperationsInterface.get();
    auto success = memoryOperationsIface->makeResident(neoDevice, ArrayRef<NEO::GraphicsAllocation *>(&allocation, 1));
    ze_result_t res = changeMemoryOperationStatusToL0ResultType(success);

    if (ZE_RESULT_SUCCESS == res) {
        auto allocData = device->getDriverHandle()->getSvmAllocsManager()->getSVMAlloc(ptr);
        if (allocData && allocData->memoryType == InternalMemoryType::SHARED_UNIFIED_MEMORY) {
            DriverHandleImp *driverHandleImp = static_cast<DriverHandleImp *>(device->getDriverHandle());
            std::lock_guard<std::mutex> lock(driverHandleImp->sharedMakeResidentAllocationsLock);
            driverHandleImp->sharedMakeResidentAllocations.insert({ptr, allocation});
        }
    }

    return res;
}

ze_result_t ContextImp::evictMemory(ze_device_handle_t hDevice, void *ptr, size_t size) {
    Device *device = L0::Device::fromHandle(hDevice);
    NEO::Device *neoDevice = device->getNEODevice();
    auto allocation = device->getDriverHandle()->getDriverSystemMemoryAllocation(
        ptr,
        size,
        neoDevice->getRootDeviceIndex(),
        nullptr);
    if (allocation == nullptr) {
        return ZE_RESULT_ERROR_INVALID_ARGUMENT;
    }

    {
        DriverHandleImp *driverHandleImp = static_cast<DriverHandleImp *>(device->getDriverHandle());
        std::lock_guard<std::mutex> lock(driverHandleImp->sharedMakeResidentAllocationsLock);
        driverHandleImp->sharedMakeResidentAllocations.erase(ptr);
    }

    NEO::MemoryOperationsHandler *memoryOperationsIface = neoDevice->getRootDeviceEnvironment().memoryOperationsInterface.get();
    auto success = memoryOperationsIface->evict(neoDevice, *allocation);
    return changeMemoryOperationStatusToL0ResultType(success);
}

ze_result_t ContextImp::makeImageResident(ze_device_handle_t hDevice, ze_image_handle_t hImage) {
    auto alloc = Image::fromHandle(hImage)->getAllocation();

    NEO::Device *neoDevice = L0::Device::fromHandle(hDevice)->getNEODevice();
    NEO::MemoryOperationsHandler *memoryOperationsIface = neoDevice->getRootDeviceEnvironment().memoryOperationsInterface.get();
    auto success = memoryOperationsIface->makeResident(neoDevice, ArrayRef<NEO::GraphicsAllocation *>(&alloc, 1));
    return changeMemoryOperationStatusToL0ResultType(success);
}
ze_result_t ContextImp::evictImage(ze_device_handle_t hDevice, ze_image_handle_t hImage) {
    auto alloc = Image::fromHandle(hImage)->getAllocation();

    NEO::Device *neoDevice = L0::Device::fromHandle(hDevice)->getNEODevice();
    NEO::MemoryOperationsHandler *memoryOperationsIface = neoDevice->getRootDeviceEnvironment().memoryOperationsInterface.get();
    auto success = memoryOperationsIface->evict(neoDevice, *alloc);
    return changeMemoryOperationStatusToL0ResultType(success);
}

ze_result_t ContextImp::getMemAddressRange(const void *ptr,
                                           void **pBase,
                                           size_t *pSize) {
    NEO::SvmAllocationData *allocData = this->driverHandle->svmAllocsManager->getSVMAlloc(ptr);
    if (allocData) {
        NEO::GraphicsAllocation *alloc;
        alloc = allocData->gpuAllocations.getDefaultGraphicsAllocation();
        if (pBase) {
            uint64_t *allocBase = reinterpret_cast<uint64_t *>(pBase);
            *allocBase = alloc->getGpuAddress();
        }

        if (pSize) {
            *pSize = allocData->size;
        }

        return ZE_RESULT_SUCCESS;
    }
    return ZE_RESULT_ERROR_UNKNOWN;
}

ze_result_t ContextImp::closeIpcMemHandle(const void *ptr) {
    return this->freeMem(ptr);
}

ze_result_t ContextImp::putIpcMemHandle(ze_ipc_mem_handle_t ipcHandle) {
    IpcMemoryData &ipcData = *reinterpret_cast<IpcMemoryData *>(ipcHandle.data);
    std::map<uint64_t, IpcHandleTracking *>::iterator ipcHandleIterator;
    auto lock = this->lockIPCHandleMap();
    ipcHandleIterator = this->getIPCHandleMap().find(ipcData.handle);
    if (ipcHandleIterator != this->getIPCHandleMap().end()) {
        ipcHandleIterator->second->refcnt -= 1;
        if (ipcHandleIterator->second->refcnt == 0) {
            auto *memoryManager = driverHandle->getMemoryManager();
            memoryManager->closeInternalHandle(ipcData.handle, ipcHandleIterator->second->handleId, ipcHandleIterator->second->alloc);
            delete ipcHandleIterator->second;
            this->getIPCHandleMap().erase(ipcData.handle);
        }
    }
    return ZE_RESULT_SUCCESS;
}

void ContextImp::setIPCHandleData(NEO::GraphicsAllocation *graphicsAllocation, uint64_t handle, IpcMemoryData &ipcData, uint64_t ptrAddress, uint8_t type) {
    std::map<uint64_t, IpcHandleTracking *>::iterator ipcHandleIterator;

    ipcData = {};
    ipcData.handle = handle;
    ipcData.type = type;

    auto lock = this->lockIPCHandleMap();
    ipcHandleIterator = this->getIPCHandleMap().find(handle);
    if (ipcHandleIterator != this->getIPCHandleMap().end()) {
        ipcHandleIterator->second->refcnt += 1;
    } else {
        IpcHandleTracking *handleTracking = new IpcHandleTracking;
        handleTracking->alloc = graphicsAllocation;
        handleTracking->refcnt = 1;
        handleTracking->ptr = ptrAddress;
        handleTracking->ipcData = ipcData;
        this->getIPCHandleMap().insert(std::pair<uint64_t, IpcHandleTracking *>(handle, handleTracking));
    }
}

ze_result_t ContextImp::getIpcMemHandle(const void *ptr,
                                        ze_ipc_mem_handle_t *pIpcHandle) {
    NEO::SvmAllocationData *allocData = this->driverHandle->svmAllocsManager->getSVMAlloc(ptr);
    if (allocData) {
        auto *memoryManager = driverHandle->getMemoryManager();
        auto *graphicsAllocation = allocData->gpuAllocations.getDefaultGraphicsAllocation();

        uint64_t handle = 0;
        int ret = graphicsAllocation->createInternalHandle(memoryManager, 0u, handle);
        if (ret < 0) {
            return ZE_RESULT_ERROR_OUT_OF_HOST_MEMORY;
        }

        memoryManager->registerIpcExportedAllocation(graphicsAllocation);

        IpcMemoryData &ipcData = *reinterpret_cast<IpcMemoryData *>(pIpcHandle->data);
        auto type = allocData->memoryType;
        uint8_t ipcType = 0;
        if (type == HOST_UNIFIED_MEMORY) {
            ipcType = static_cast<uint8_t>(InternalIpcMemoryType::IPC_HOST_UNIFIED_MEMORY);
        }
        setIPCHandleData(graphicsAllocation, handle, ipcData, reinterpret_cast<uint64_t>(ptr), ipcType);

        return ZE_RESULT_SUCCESS;
    }
    return ZE_RESULT_ERROR_INVALID_ARGUMENT;
}

ze_result_t ContextImp::getIpcHandleFromFd(uint64_t handle, ze_ipc_mem_handle_t *pIpcHandle) {
    std::map<uint64_t, IpcHandleTracking *>::iterator ipcHandleIterator;
    auto lock = this->lockIPCHandleMap();
    ipcHandleIterator = this->getIPCHandleMap().find(handle);
    if (ipcHandleIterator != this->getIPCHandleMap().end()) {
        IpcMemoryData &ipcData = *reinterpret_cast<IpcMemoryData *>(pIpcHandle->data);
        ipcData = ipcHandleIterator->second->ipcData;
    } else {
        return ZE_RESULT_ERROR_OUT_OF_HOST_MEMORY;
    }
    return ZE_RESULT_SUCCESS;
}

ze_result_t ContextImp::getFdFromIpcHandle(ze_ipc_mem_handle_t ipcHandle, uint64_t *pHandle) {
    IpcMemoryData &ipcData = *reinterpret_cast<IpcMemoryData *>(ipcHandle.data);
    std::map<uint64_t, IpcHandleTracking *>::iterator ipcHandleIterator;
    auto lock = this->lockIPCHandleMap();
    ipcHandleIterator = this->getIPCHandleMap().find(ipcData.handle);
    if (ipcHandleIterator != this->getIPCHandleMap().end()) {
        *pHandle = ipcHandleIterator->first;
    } else {
        return ZE_RESULT_ERROR_OUT_OF_HOST_MEMORY;
    }
    return ZE_RESULT_SUCCESS;
}

ze_result_t ContextImp::getIpcMemHandles(const void *ptr,
                                         uint32_t *numIpcHandles,
                                         ze_ipc_mem_handle_t *pIpcHandles) {
    NEO::SvmAllocationData *allocData = this->driverHandle->svmAllocsManager->getSVMAlloc(ptr);
    if (allocData) {
        auto alloc = allocData->gpuAllocations.getDefaultGraphicsAllocation();
        uint32_t numHandles = alloc->getNumHandles();
        UNRECOVERABLE_IF(numIpcHandles == nullptr);

        if (*numIpcHandles == 0 || *numIpcHandles > numHandles) {
            *numIpcHandles = numHandles;
        }

        if (pIpcHandles == nullptr) {
            return ZE_RESULT_SUCCESS;
        }

        auto type = allocData->memoryType;
        auto ipcType = InternalIpcMemoryType::IPC_DEVICE_UNIFIED_MEMORY;
        if (type == HOST_UNIFIED_MEMORY) {
            ipcType = InternalIpcMemoryType::IPC_HOST_UNIFIED_MEMORY;
        }

        for (uint32_t i = 0; i < *numIpcHandles; i++) {
            uint64_t handle = 0;
            int ret = allocData->gpuAllocations.getDefaultGraphicsAllocation()->createInternalHandle(this->driverHandle->getMemoryManager(), i, handle);
            if (ret < 0) {
                return ZE_RESULT_ERROR_OUT_OF_HOST_MEMORY;
            }

            IpcMemoryData &ipcData = *reinterpret_cast<IpcMemoryData *>(pIpcHandles[i].data);
            setIPCHandleData(alloc, handle, ipcData, reinterpret_cast<uint64_t>(ptr), static_cast<uint8_t>(ipcType));
        }

        return ZE_RESULT_SUCCESS;
    }
    return ZE_RESULT_ERROR_INVALID_ARGUMENT;
}

ze_result_t ContextImp::openIpcMemHandle(ze_device_handle_t hDevice,
                                         const ze_ipc_mem_handle_t &pIpcHandle,
                                         ze_ipc_memory_flags_t flags,
                                         void **ptr) {
    const IpcMemoryData &ipcData = *reinterpret_cast<const IpcMemoryData *>(pIpcHandle.data);

    uint64_t handle = ipcData.handle;
    uint8_t type = ipcData.type;

    NEO::AllocationType allocationType = NEO::AllocationType::UNKNOWN;
    if (type == static_cast<uint8_t>(InternalIpcMemoryType::IPC_DEVICE_UNIFIED_MEMORY)) {
        allocationType = NEO::AllocationType::BUFFER;
    } else if (type == static_cast<uint8_t>(InternalIpcMemoryType::IPC_HOST_UNIFIED_MEMORY)) {
        allocationType = NEO::AllocationType::BUFFER_HOST_MEMORY;
    } else {
        return ZE_RESULT_ERROR_INVALID_ARGUMENT;
    }

    *ptr = getMemHandlePtr(hDevice,
                           handle,
                           allocationType,
                           flags);
    if (nullptr == *ptr) {
        return ZE_RESULT_ERROR_INVALID_ARGUMENT;
    }

    return ZE_RESULT_SUCCESS;
}

ze_result_t ContextImp::openIpcMemHandles(ze_device_handle_t hDevice,
                                          uint32_t numIpcHandles,
                                          ze_ipc_mem_handle_t *pIpcHandles,
                                          ze_ipc_memory_flags_t flags,
                                          void **pptr) {
    std::vector<NEO::osHandle> handles;
    handles.reserve(numIpcHandles);

    for (uint32_t i = 0; i < numIpcHandles; i++) {
        const IpcMemoryData &ipcData = *reinterpret_cast<const IpcMemoryData *>(pIpcHandles[i].data);
        uint64_t handle = ipcData.handle;

        if (ipcData.type != static_cast<uint8_t>(InternalIpcMemoryType::IPC_DEVICE_UNIFIED_MEMORY)) {
            return ZE_RESULT_ERROR_INVALID_ARGUMENT;
        }

        handles.push_back(static_cast<NEO::osHandle>(handle));
    }
    auto neoDevice = Device::fromHandle(hDevice)->getNEODevice()->getRootDevice();
    NEO::SvmAllocationData allocDataInternal(neoDevice->getRootDeviceIndex());
    *pptr = this->driverHandle->importFdHandles(neoDevice, flags, handles, nullptr, nullptr, allocDataInternal);
    if (nullptr == *pptr) {
        return ZE_RESULT_ERROR_INVALID_ARGUMENT;
    }

    return ZE_RESULT_SUCCESS;
}

ze_result_t ContextImp::openEventPoolIpcHandle(const ze_ipc_event_pool_handle_t &ipcEventPoolHandle,
                                               ze_event_pool_handle_t *eventPoolHandle) {
    return EventPool::openEventPoolIpcHandle(ipcEventPoolHandle, eventPoolHandle, driverHandle, this, this->numDevices, this->deviceHandles.data());
}

ze_result_t ContextImp::handleAllocationExtensions(NEO::GraphicsAllocation *alloc, ze_memory_type_t type, void *pNext, struct DriverHandleImp *driverHandle) {
    if (pNext != nullptr) {
        ze_base_properties_t *extendedProperties =
            reinterpret_cast<ze_base_properties_t *>(pNext);
        if (extendedProperties->stype == ZE_STRUCTURE_TYPE_EXTERNAL_MEMORY_EXPORT_FD) {
            ze_external_memory_export_fd_t *extendedMemoryExportProperties =
                reinterpret_cast<ze_external_memory_export_fd_t *>(extendedProperties);
            if (extendedMemoryExportProperties->flags & ZE_EXTERNAL_MEMORY_TYPE_FLAG_OPAQUE_FD) {
                return ZE_RESULT_ERROR_UNSUPPORTED_ENUMERATION;
            }
            if (type == ZE_MEMORY_TYPE_SHARED) {
                return ZE_RESULT_ERROR_UNSUPPORTED_FEATURE;
            }
            ze_ipc_mem_handle_t ipcHandle;
            uint64_t handle = 0;
            auto result = getIpcMemHandle(reinterpret_cast<void *>(alloc->getGpuAddress()), &ipcHandle);
            if (result != ZE_RESULT_SUCCESS) {
                // If this memory is not an SVM Allocation like Images, then retrieve only the handle untracked.
                auto ret = alloc->peekInternalHandle(driverHandle->getMemoryManager(), handle);
                if (ret < 0) {
                    return ZE_RESULT_ERROR_OUT_OF_HOST_MEMORY;
                }
            } else {
                IpcMemoryData &ipcData = *reinterpret_cast<IpcMemoryData *>(ipcHandle.data);
                handle = ipcData.handle;
            }
            extendedMemoryExportProperties->fd = static_cast<int>(handle);
        } else if (extendedProperties->stype == ZE_STRUCTURE_TYPE_EXTERNAL_MEMORY_EXPORT_WIN32) {
            ze_external_memory_export_win32_handle_t *exportStructure = reinterpret_cast<ze_external_memory_export_win32_handle_t *>(extendedProperties);
            if (exportStructure->flags != ZE_EXTERNAL_MEMORY_TYPE_FLAG_OPAQUE_WIN32) {
                return ZE_RESULT_ERROR_UNSUPPORTED_ENUMERATION;
            }
            uint64_t handle = 0;
            int ret = alloc->peekInternalHandle(driverHandle->getMemoryManager(), handle);
            if (ret < 0) {
                return ZE_RESULT_ERROR_OUT_OF_HOST_MEMORY;
            }
            exportStructure->handle = reinterpret_cast<void *>(handle);
        } else if (extendedProperties->stype == ZE_STRUCTURE_TYPE_MEMORY_SUB_ALLOCATIONS_EXP_PROPERTIES) {
            if (alloc->getNumHandles()) {
                ze_memory_sub_allocations_exp_properties_t *extendedSubAllocProperties =
                    reinterpret_cast<ze_memory_sub_allocations_exp_properties_t *>(extendedProperties);
                if (extendedSubAllocProperties->pCount) {
                    *extendedSubAllocProperties->pCount = alloc->getNumHandles();
                } else {
                    // pCount cannot be nullptr
                    return ZE_RESULT_ERROR_INVALID_NULL_POINTER;
                }
                if (extendedSubAllocProperties->pSubAllocations) {
                    for (uint32_t i = 0; i < *extendedSubAllocProperties->pCount; i++) {
                        extendedSubAllocProperties->pSubAllocations[i].base = reinterpret_cast<void *>(alloc->getHandleAddressBase(i));
                        extendedSubAllocProperties->pSubAllocations[i].size = alloc->getHandleSize(i);
                    }
                    // If pSubAllocations nullptr, then user getting Count first and calling second time
                }
                return ZE_RESULT_SUCCESS;
            }
            return ZE_RESULT_ERROR_UNSUPPORTED_ENUMERATION;
        } else {
            return ZE_RESULT_ERROR_INVALID_ENUMERATION;
        }
    }

    return ZE_RESULT_SUCCESS;
}

ze_result_t ContextImp::getMemAllocProperties(const void *ptr,
                                              ze_memory_allocation_properties_t *pMemAllocProperties,
                                              ze_device_handle_t *phDevice) {
    const auto alloc = driverHandle->svmAllocsManager->getSVMAlloc(ptr);
    if (nullptr == alloc) {
        pMemAllocProperties->type = ZE_MEMORY_TYPE_UNKNOWN;
        return ZE_RESULT_SUCCESS;
    }

    pMemAllocProperties->type = Context::parseUSMType(alloc->memoryType);
    pMemAllocProperties->pageSize = alloc->pageSizeForAlignment;
    pMemAllocProperties->id = alloc->getAllocId();

    if (phDevice != nullptr) {
        if (alloc->device == nullptr) {
            *phDevice = nullptr;
        } else {
            auto device = static_cast<NEO::Device *>(alloc->device)->getSpecializedDevice<DeviceImp>();
            DEBUG_BREAK_IF(device == nullptr);
            *phDevice = device->toHandle();
        }
    }
    if (pMemAllocProperties->pNext == nullptr) {
        return ZE_RESULT_SUCCESS;
    }
    return handleAllocationExtensions(alloc->gpuAllocations.getDefaultGraphicsAllocation(),
                                      pMemAllocProperties->type,
                                      pMemAllocProperties->pNext,
                                      driverHandle);
}

ze_result_t ContextImp::getImageAllocProperties(Image *image, ze_image_allocation_ext_properties_t *pAllocProperties) {
    NEO::GraphicsAllocation *alloc = image->getAllocation();

    if (alloc == nullptr) {
        return ZE_RESULT_ERROR_UNKNOWN;
    }

    pAllocProperties->id = 0;

    return handleAllocationExtensions(alloc, ZE_MEMORY_TYPE_DEVICE, pAllocProperties->pNext, driverHandle);
}

ze_result_t ContextImp::createModule(ze_device_handle_t hDevice,
                                     const ze_module_desc_t *desc,
                                     ze_module_handle_t *phModule,
                                     ze_module_build_log_handle_t *phBuildLog) {
    return L0::Device::fromHandle(hDevice)->createModule(desc, phModule, phBuildLog, ModuleType::User);
}

ze_result_t ContextImp::createSampler(ze_device_handle_t hDevice,
                                      const ze_sampler_desc_t *pDesc,
                                      ze_sampler_handle_t *phSampler) {
    return L0::Device::fromHandle(hDevice)->createSampler(pDesc, phSampler);
}

ze_result_t ContextImp::createCommandQueue(ze_device_handle_t hDevice,
                                           const ze_command_queue_desc_t *desc,
                                           ze_command_queue_handle_t *commandQueue) {
    return L0::Device::fromHandle(hDevice)->createCommandQueue(desc, commandQueue);
}

ze_result_t ContextImp::createCommandList(ze_device_handle_t hDevice,
                                          const ze_command_list_desc_t *desc,
                                          ze_command_list_handle_t *commandList) {
    auto ret = L0::Device::fromHandle(hDevice)->createCommandList(desc, commandList);
    if (*commandList) {
        L0::CommandList::fromHandle(*commandList)->setCmdListContext(this->toHandle());
    }
    return ret;
}

ze_result_t ContextImp::createCommandListImmediate(ze_device_handle_t hDevice,
                                                   const ze_command_queue_desc_t *desc,
                                                   ze_command_list_handle_t *commandList) {
    auto ret = L0::Device::fromHandle(hDevice)->createCommandListImmediate(desc, commandList);
    if (*commandList) {
        L0::CommandList::fromHandle(*commandList)->setCmdListContext(this->toHandle());
    }
    return ret;
}

ze_result_t ContextImp::activateMetricGroups(zet_device_handle_t hDevice,
                                             uint32_t count,
                                             zet_metric_group_handle_t *phMetricGroups) {
    return L0::Device::fromHandle(hDevice)->activateMetricGroupsDeferred(count, phMetricGroups);
}

NEO::VirtualMemoryReservation *ContextImp::findSupportedVirtualReservation(const void *ptr, size_t size) {
    void *address = const_cast<void *>(ptr);
    auto allocation = this->driverHandle->getMemoryManager()->getVirtualMemoryReservationMap().lower_bound(address);
    if (allocation != this->driverHandle->getMemoryManager()->getVirtualMemoryReservationMap().end()) {
        if (ptr == allocation->first && ptrOffset(reinterpret_cast<uint64_t>(allocation->first), allocation->second->virtualAddressRange.size) >= ptrOffset(reinterpret_cast<uint64_t>(address), size)) {
            return allocation->second;
        }
    }
    if (allocation != this->driverHandle->getMemoryManager()->getVirtualMemoryReservationMap().begin()) {
        allocation--;
        if (ptrOffset(allocation->first, allocation->second->virtualAddressRange.size) >= ptrOffset(address, size)) {
            return allocation->second;
        }
    }
    return nullptr;
}

ze_result_t ContextImp::reserveVirtualMem(const void *pStart,
                                          size_t size,
                                          void **pptr) {
    if ((getPageSizeRequired(size) != size)) {
        return ZE_RESULT_ERROR_UNSUPPORTED_SIZE;
    }
    NEO::VirtualMemoryReservation *virtualMemoryReservation = new NEO::VirtualMemoryReservation;
    virtualMemoryReservation->virtualAddressRange = this->driverHandle->getMemoryManager()->reserveGpuAddress(reinterpret_cast<uint64_t>(pStart), size, this->driverHandle->rootDeviceIndices, &virtualMemoryReservation->rootDeviceIndex);
    if (virtualMemoryReservation->virtualAddressRange.address == 0) {
        delete virtualMemoryReservation;
        return ZE_RESULT_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    virtualMemoryReservation->flags.readWrite = false;
    virtualMemoryReservation->flags.readOnly = false;
    virtualMemoryReservation->flags.noAccess = true;
    auto lock = this->driverHandle->getMemoryManager()->lockVirtualMemoryReservationMap();
    this->driverHandle->getMemoryManager()->getVirtualMemoryReservationMap().insert(std::pair<void *, NEO::VirtualMemoryReservation *>(reinterpret_cast<void *>(virtualMemoryReservation->virtualAddressRange.address), virtualMemoryReservation));
    *pptr = reinterpret_cast<void *>(virtualMemoryReservation->virtualAddressRange.address);
    return ZE_RESULT_SUCCESS;
}

ze_result_t ContextImp::freeVirtualMem(const void *ptr,
                                       size_t size) {
    std::map<void *, NEO::VirtualMemoryReservation *>::iterator it;
    auto lock = this->driverHandle->getMemoryManager()->lockVirtualMemoryReservationMap();
    it = this->driverHandle->getMemoryManager()->getVirtualMemoryReservationMap().find(const_cast<void *>(ptr));
    if (it != this->driverHandle->getMemoryManager()->getVirtualMemoryReservationMap().end()) {
        for (auto &pairDevice : this->devices) {
            this->freePeerAllocations(ptr, false, Device::fromHandle(pairDevice.second));
        }

        NEO::VirtualMemoryReservation *virtualMemoryReservation = it->second;
        if (virtualMemoryReservation->virtualAddressRange.size != size) {
            return ZE_RESULT_ERROR_INVALID_ARGUMENT;
        }
        this->driverHandle->getMemoryManager()->freeGpuAddress(virtualMemoryReservation->virtualAddressRange, virtualMemoryReservation->rootDeviceIndex);
        delete virtualMemoryReservation;
        this->driverHandle->getMemoryManager()->getVirtualMemoryReservationMap().erase(it);
        virtualMemoryReservation = nullptr;
        return ZE_RESULT_SUCCESS;
    } else {
        return ZE_RESULT_ERROR_INVALID_ARGUMENT;
    }
}

size_t ContextImp::getPageSizeRequired(size_t size) {
    return std::max(Math::prevPowerOfTwo(size), MemoryConstants::pageSize64k);
}

ze_result_t ContextImp::queryVirtualMemPageSize(ze_device_handle_t hDevice,
                                                size_t size,
                                                size_t *pagesize) {
    *pagesize = getPageSizeRequired(size);
    return ZE_RESULT_SUCCESS;
}

ze_result_t ContextImp::createPhysicalMem(ze_device_handle_t hDevice,
                                          ze_physical_mem_desc_t *desc,
                                          ze_physical_mem_handle_t *phPhysicalMemory) {
    if ((getPageSizeRequired(desc->size) != desc->size)) {
        return ZE_RESULT_ERROR_UNSUPPORTED_SIZE;
    }
    auto device = Device::fromHandle(hDevice);
    auto neoDevice = device->getNEODevice();

    NEO::AllocationProperties physicalDeviceMemoryProperties{neoDevice->getRootDeviceIndex(),
                                                             true,
                                                             desc->size,
                                                             NEO::AllocationType::BUFFER,
                                                             false,
                                                             false,
                                                             device->getNEODevice()->getDeviceBitfield()};
    physicalDeviceMemoryProperties.flags.isUSMDeviceAllocation = true;
    physicalDeviceMemoryProperties.flags.shareable = 1;

    NEO::GraphicsAllocation *physicalDeviceMemoryAllocation = this->driverHandle->getMemoryManager()->allocatePhysicalGraphicsMemory(physicalDeviceMemoryProperties);
    if (!physicalDeviceMemoryAllocation) {
        return ZE_RESULT_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    NEO::PhysicalMemoryAllocation *physicalMemoryAllocation = new NEO::PhysicalMemoryAllocation;
    physicalMemoryAllocation->allocation = physicalDeviceMemoryAllocation;
    physicalMemoryAllocation->device = neoDevice;
    auto lock = this->driverHandle->getMemoryManager()->lockPhysicalMemoryAllocationMap();
    this->driverHandle->getMemoryManager()->getPhysicalMemoryAllocationMap().insert(std::pair<void *, NEO::PhysicalMemoryAllocation *>(reinterpret_cast<void *>(physicalDeviceMemoryAllocation), physicalMemoryAllocation));
    *phPhysicalMemory = reinterpret_cast<ze_physical_mem_handle_t>(physicalDeviceMemoryAllocation);
    return ZE_RESULT_SUCCESS;
}

ze_result_t ContextImp::destroyPhysicalMem(ze_physical_mem_handle_t hPhysicalMemory) {
    std::map<void *, NEO::PhysicalMemoryAllocation *>::iterator it;
    auto lock = this->driverHandle->getMemoryManager()->lockPhysicalMemoryAllocationMap();
    it = this->driverHandle->getMemoryManager()->getPhysicalMemoryAllocationMap().find(static_cast<void *>(hPhysicalMemory));
    if (it != this->driverHandle->getMemoryManager()->getPhysicalMemoryAllocationMap().end()) {
        NEO::PhysicalMemoryAllocation *allocationNode = it->second;
        this->driverHandle->getMemoryManager()->freeGraphicsMemoryImpl(allocationNode->allocation);
        this->driverHandle->getMemoryManager()->getPhysicalMemoryAllocationMap().erase(it);
        delete allocationNode;
    }
    return ZE_RESULT_SUCCESS;
}

ze_result_t ContextImp::mapVirtualMem(const void *ptr,
                                      size_t size,
                                      ze_physical_mem_handle_t hPhysicalMemory,
                                      size_t offset,
                                      ze_memory_access_attribute_t access) {
    std::map<void *, NEO::PhysicalMemoryAllocation *>::iterator physicalIt;
    NEO::PhysicalMemoryAllocation *allocationNode = nullptr;

    if ((getPageSizeRequired(size) != size)) {
        return ZE_RESULT_ERROR_UNSUPPORTED_ALIGNMENT;
    }
    auto lockPhysical = this->driverHandle->getMemoryManager()->lockPhysicalMemoryAllocationMap();
    physicalIt = this->driverHandle->getMemoryManager()->getPhysicalMemoryAllocationMap().find(static_cast<void *>(hPhysicalMemory));
    if (physicalIt != this->driverHandle->getMemoryManager()->getPhysicalMemoryAllocationMap().end()) {
        allocationNode = physicalIt->second;
    } else {
        return ZE_RESULT_ERROR_INVALID_ARGUMENT;
    }

    NEO::VirtualMemoryReservation *virtualMemoryReservation = nullptr;
    auto lockVirtual = this->driverHandle->getMemoryManager()->lockVirtualMemoryReservationMap();
    virtualMemoryReservation = findSupportedVirtualReservation(ptr, size);
    if (virtualMemoryReservation) {
        switch (access) {
        case ZE_MEMORY_ACCESS_ATTRIBUTE_NONE:
            virtualMemoryReservation->flags.readOnly = false;
            virtualMemoryReservation->flags.noAccess = true;
            virtualMemoryReservation->flags.readWrite = false;
            break;
        case ZE_MEMORY_ACCESS_ATTRIBUTE_READWRITE:
            virtualMemoryReservation->flags.readOnly = false;
            virtualMemoryReservation->flags.noAccess = false;
            virtualMemoryReservation->flags.readWrite = true;
            break;
        case ZE_MEMORY_ACCESS_ATTRIBUTE_READONLY:
            virtualMemoryReservation->flags.readWrite = false;
            virtualMemoryReservation->flags.noAccess = false;
            virtualMemoryReservation->flags.readOnly = true;
            break;
        default:
            return ZE_RESULT_ERROR_INVALID_ENUMERATION;
        }
        if (virtualMemoryReservation->mappedAllocations.size() > 0) {
            std::map<void *, NEO::MemoryMappedRange *>::iterator physicalMapIt;
            physicalMapIt = virtualMemoryReservation->mappedAllocations.find(const_cast<void *>(ptr));
            if (physicalMapIt != virtualMemoryReservation->mappedAllocations.end()) {
                return ZE_RESULT_ERROR_INVALID_ARGUMENT;
            }
        }
    } else {
        return ZE_RESULT_ERROR_INVALID_ARGUMENT;
    }

    if (this->driverHandle->getMemoryManager()->mapPhysicalToVirtualMemory(allocationNode->allocation, reinterpret_cast<uint64_t>(ptr), size) == true) {
        NEO::SvmAllocationData allocData(allocationNode->allocation->getRootDeviceIndex());
        allocData.gpuAllocations.addAllocation(allocationNode->allocation);
        allocData.cpuAllocation = nullptr;
        allocData.device = allocationNode->device;
        allocData.size = size;
        allocData.pageSizeForAlignment = MemoryConstants::pageSize64k;
        allocData.setAllocId(this->driverHandle->svmAllocsManager->allocationsCounter++);
        allocData.memoryType = InternalMemoryType::RESERVED_DEVICE_MEMORY;
        NEO::MemoryMappedRange *mappedRange = new NEO::MemoryMappedRange;
        mappedRange->ptr = ptr;
        mappedRange->size = size;
        mappedRange->mappedAllocation = allocationNode;
        virtualMemoryReservation->mappedAllocations.insert(std::pair<void *, NEO::MemoryMappedRange *>(const_cast<void *>(ptr), mappedRange));
        this->driverHandle->getSvmAllocsManager()->insertSVMAlloc(allocData);
        NEO::MemoryOperationsHandler *memoryOperationsIface = allocationNode->device->getRootDeviceEnvironment().memoryOperationsInterface.get();
        auto success = memoryOperationsIface->makeResident(allocationNode->device, ArrayRef<NEO::GraphicsAllocation *>(&allocationNode->allocation, 1));
        ze_result_t res = changeMemoryOperationStatusToL0ResultType(success);
        return res;
    }
    return ZE_RESULT_ERROR_OUT_OF_DEVICE_MEMORY;
}

ze_result_t ContextImp::unMapVirtualMem(const void *ptr,
                                        size_t size) {

    NEO::VirtualMemoryReservation *virtualMemoryReservation = nullptr;
    auto lockVirtual = this->driverHandle->getMemoryManager()->lockVirtualMemoryReservationMap();
    virtualMemoryReservation = findSupportedVirtualReservation(ptr, size);
    if (virtualMemoryReservation) {
        std::map<void *, NEO::MemoryMappedRange *>::iterator physicalMapIt;
        physicalMapIt = virtualMemoryReservation->mappedAllocations.find(const_cast<void *>(ptr));
        if (physicalMapIt != virtualMemoryReservation->mappedAllocations.end()) {
            NEO::PhysicalMemoryAllocation *physicalAllocation = physicalMapIt->second->mappedAllocation;
            NEO::SvmAllocationData *allocData = this->driverHandle->getSvmAllocsManager()->getSVMAlloc(reinterpret_cast<void *>(physicalAllocation->allocation->getGpuAddress()));
            this->driverHandle->getSvmAllocsManager()->removeSVMAlloc(*allocData);
            NEO::Device *device = physicalAllocation->device;
            NEO::CommandStreamReceiver *csr = device->getDefaultEngine().commandStreamReceiver;
            NEO::OsContext *osContext = &csr->getOsContext();
            this->driverHandle->getMemoryManager()->unMapPhysicalToVirtualMemory(physicalAllocation->allocation, reinterpret_cast<uint64_t>(ptr), size, osContext, virtualMemoryReservation->rootDeviceIndex);
            delete physicalMapIt->second;
            virtualMemoryReservation->mappedAllocations.erase(physicalMapIt);
        }
    }
    return ZE_RESULT_SUCCESS;
}

ze_result_t ContextImp::setVirtualMemAccessAttribute(const void *ptr,
                                                     size_t size,
                                                     ze_memory_access_attribute_t access) {
    NEO::VirtualMemoryReservation *virtualMemoryReservation = nullptr;
    auto lockVirtual = this->driverHandle->getMemoryManager()->lockVirtualMemoryReservationMap();
    virtualMemoryReservation = findSupportedVirtualReservation(ptr, size);
    if (virtualMemoryReservation) {
        switch (access) {
        case ZE_MEMORY_ACCESS_ATTRIBUTE_NONE:
            virtualMemoryReservation->flags.readOnly = false;
            virtualMemoryReservation->flags.noAccess = true;
            virtualMemoryReservation->flags.readWrite = false;
            break;
        case ZE_MEMORY_ACCESS_ATTRIBUTE_READWRITE:
            virtualMemoryReservation->flags.readOnly = false;
            virtualMemoryReservation->flags.noAccess = false;
            virtualMemoryReservation->flags.readWrite = true;
            break;
        case ZE_MEMORY_ACCESS_ATTRIBUTE_READONLY:
            virtualMemoryReservation->flags.readWrite = false;
            virtualMemoryReservation->flags.noAccess = false;
            virtualMemoryReservation->flags.readOnly = true;
            break;
        default:
            return ZE_RESULT_ERROR_INVALID_ENUMERATION;
        }
        return ZE_RESULT_SUCCESS;
    } else {
        return ZE_RESULT_ERROR_INVALID_ARGUMENT;
    }
}

ze_result_t ContextImp::getVirtualMemAccessAttribute(const void *ptr,
                                                     size_t size,
                                                     ze_memory_access_attribute_t *access,
                                                     size_t *outSize) {
    NEO::VirtualMemoryReservation *virtualMemoryReservation = nullptr;
    auto lockVirtual = this->driverHandle->getMemoryManager()->lockVirtualMemoryReservationMap();
    virtualMemoryReservation = findSupportedVirtualReservation(ptr, size);
    if (virtualMemoryReservation) {
        if (virtualMemoryReservation->flags.readWrite) {
            *access = ZE_MEMORY_ACCESS_ATTRIBUTE_READWRITE;
        } else if (virtualMemoryReservation->flags.readOnly) {
            *access = ZE_MEMORY_ACCESS_ATTRIBUTE_READONLY;
        } else {
            *access = ZE_MEMORY_ACCESS_ATTRIBUTE_NONE;
        }
        *outSize = virtualMemoryReservation->virtualAddressRange.size;
        return ZE_RESULT_SUCCESS;
    } else {
        return ZE_RESULT_ERROR_INVALID_ARGUMENT;
    }
}

ze_result_t ContextImp::createEventPool(const ze_event_pool_desc_t *desc,
                                        uint32_t numDevices,
                                        ze_device_handle_t *phDevices,
                                        ze_event_pool_handle_t *phEventPool) {
    ze_result_t result;
    EventPool *eventPool = EventPool::create(this->driverHandle, this, numDevices, phDevices, desc, result);

    if (eventPool == nullptr) {
        return result;
    }

    *phEventPool = eventPool->toHandle();

    return ZE_RESULT_SUCCESS;
}

ze_result_t ContextImp::createImage(ze_device_handle_t hDevice,
                                    const ze_image_desc_t *desc,
                                    ze_image_handle_t *phImage) {
    return L0::Device::fromHandle(hDevice)->createImage(desc, phImage);
}

bool ContextImp::isAllocationSuitableForCompression(const StructuresLookupTable &structuresLookupTable, Device &device, size_t allocSize) {
    auto &hwInfo = device.getHwInfo();
    auto &gfxCoreHelper = device.getGfxCoreHelper();
    auto &l0GfxCoreHelper = device.getNEODevice()->getRootDeviceEnvironment().getHelper<L0GfxCoreHelper>();

    if (!l0GfxCoreHelper.usmCompressionSupported(hwInfo) || !gfxCoreHelper.isBufferSizeSuitableForCompression(allocSize) || structuresLookupTable.uncompressedHint) {
        return false;
    }

    if (l0GfxCoreHelper.forceDefaultUsmCompressionSupport()) {
        return true;
    }

    return structuresLookupTable.compressedHint;
}

} // namespace L0

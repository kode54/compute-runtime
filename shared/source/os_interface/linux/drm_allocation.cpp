/*
 * Copyright (C) 2020-2023 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 */

#include "shared/source/os_interface/linux/drm_allocation.h"

#include "shared/source/debug_settings/debug_settings_manager.h"
#include "shared/source/execution_environment/root_device_environment.h"
#include "shared/source/helpers/basic_math.h"
#include "shared/source/memory_manager/residency.h"
#include "shared/source/os_interface/linux/cache_info.h"
#include "shared/source/os_interface/linux/drm_buffer_object.h"
#include "shared/source/os_interface/linux/drm_memory_manager.h"
#include "shared/source/os_interface/linux/drm_neo.h"
#include "shared/source/os_interface/linux/ioctl_helper.h"
#include "shared/source/os_interface/linux/os_context_linux.h"
#include "shared/source/os_interface/os_context.h"
#include "shared/source/os_interface/product_helper.h"

#include <sstream>

namespace NEO {

DrmAllocation::~DrmAllocation() {
    [[maybe_unused]] int retCode;
    for (auto &memory : this->memoryToUnmap) {
        retCode = memory.unmapFunction(memory.pointer, memory.size);
        DEBUG_BREAK_IF(retCode != 0);
    }
}

std::string DrmAllocation::getAllocationInfoString() const {
    std::stringstream ss;
    for (auto bo : bufferObjects) {
        if (bo != nullptr) {
            ss << " Handle: " << bo->peekHandle();
        }
    }
    return ss.str();
}

void DrmAllocation::clearInternalHandle(uint32_t handleId) {
    handles[handleId] = std::numeric_limits<uint64_t>::max();
}

int DrmAllocation::createInternalHandle(MemoryManager *memoryManager, uint32_t handleId, uint64_t &handle) {
    return peekInternalHandle(memoryManager, handleId, handle);
}

int DrmAllocation::peekInternalHandle(MemoryManager *memoryManager, uint64_t &handle) {
    return peekInternalHandle(memoryManager, 0u, handle);
}

int DrmAllocation::peekInternalHandle(MemoryManager *memoryManager, uint32_t handleId, uint64_t &handle) {
    if (handles[handleId] != std::numeric_limits<uint64_t>::max()) {
        handle = handles[handleId];
        return 0;
    }

    int64_t ret = static_cast<int64_t>((static_cast<DrmMemoryManager *>(memoryManager))->obtainFdFromHandle(getBufferObjectToModify(handleId)->peekHandle(), this->rootDeviceIndex));
    if (ret < 0) {
        return -1;
    }

    handle = handles[handleId] = ret;

    return 0;
}

void DrmAllocation::setCachePolicy(CachePolicy memType) {
    for (auto bo : bufferObjects) {
        if (bo != nullptr) {
            bo->setCachePolicy(memType);
        }
    }
}

bool DrmAllocation::setPreferredLocation(Drm *drm, PreferredLocation memoryLocation) {
    auto ioctlHelper = drm->getIoctlHelper();
    auto remainingMemoryBanks = storageInfo.memoryBanks;
    bool success = true;

    for (uint8_t handleId = 0u; handleId < numHandles; handleId++) {
        auto memoryInstance = Math::getMinLsbSet(static_cast<uint32_t>(remainingMemoryBanks.to_ulong()));

        std::optional<MemoryClassInstance> region = ioctlHelper->getPreferredLocationRegion(memoryLocation, memoryInstance);
        if (region != std::nullopt) {
            auto bo = this->getBOs()[handleId];
            success &= ioctlHelper->setVmBoAdvise(bo->peekHandle(), ioctlHelper->getPreferredLocationAdvise(), &region);
        }

        remainingMemoryBanks.reset(memoryInstance);
    }

    return success;
}

bool DrmAllocation::setCacheRegion(Drm *drm, CacheRegion regionIndex) {
    if (regionIndex == CacheRegion::Default) {
        return true;
    }

    auto cacheInfo = drm->getCacheInfo();
    if (cacheInfo == nullptr) {
        return false;
    }

    auto regionSize = (cacheInfo->getMaxReservationNumCacheRegions() > 0) ? cacheInfo->getMaxReservationCacheSize() / cacheInfo->getMaxReservationNumCacheRegions() : 0;
    if (regionSize == 0) {
        return false;
    }

    return setCacheAdvice(drm, regionSize, regionIndex);
}

bool DrmAllocation::setCacheAdvice(Drm *drm, size_t regionSize, CacheRegion regionIndex) {
    if (!drm->getCacheInfo()->getCacheRegion(regionSize, regionIndex)) {
        return false;
    }

    auto patIndex = drm->getPatIndex(getDefaultGmm(), allocationType, regionIndex, CachePolicy::WriteBack, true);

    if (fragmentsStorage.fragmentCount > 0) {
        for (uint32_t i = 0; i < fragmentsStorage.fragmentCount; i++) {
            auto bo = static_cast<OsHandleLinux *>(fragmentsStorage.fragmentStorageData[i].osHandleStorage)->bo;
            bo->setCacheRegion(regionIndex);
            bo->setPatIndex(patIndex);
        }
        return true;
    }

    for (auto bo : bufferObjects) {
        if (bo != nullptr) {
            bo->setCacheRegion(regionIndex);
            bo->setPatIndex(patIndex);
        }
    }
    return true;
}

int DrmAllocation::makeBOsResident(OsContext *osContext, uint32_t vmHandleId, std::vector<BufferObject *> *bufferObjects, bool bind) {
    if (this->fragmentsStorage.fragmentCount) {
        for (unsigned int f = 0; f < this->fragmentsStorage.fragmentCount; f++) {
            if (!this->fragmentsStorage.fragmentStorageData[f].residency->resident[osContext->getContextId()]) {
                int retVal = bindBO(static_cast<OsHandleLinux *>(this->fragmentsStorage.fragmentStorageData[f].osHandleStorage)->bo, osContext, vmHandleId, bufferObjects, bind);
                if (retVal) {
                    return retVal;
                }
                this->fragmentsStorage.fragmentStorageData[f].residency->resident[osContext->getContextId()] = true;
            }
        }
    } else {
        int retVal = bindBOs(osContext, vmHandleId, bufferObjects, bind);
        if (retVal) {
            return retVal;
        }
    }

    return 0;
}

int DrmAllocation::bindBO(BufferObject *bo, OsContext *osContext, uint32_t vmHandleId, std::vector<BufferObject *> *bufferObjects, bool bind) {
    auto retVal = 0;
    if (bo) {
        bo->requireExplicitResidency(bo->peekDrm()->hasPageFaultSupport() && !shouldAllocationPageFault(bo->peekDrm()));
        if (bufferObjects) {
            if (bo->peekIsReusableAllocation()) {
                for (auto bufferObject : *bufferObjects) {
                    if (bufferObject == bo) {
                        return 0;
                    }
                }
            }

            bufferObjects->push_back(bo);

        } else {
            if (bind) {
                retVal = bo->bind(osContext, vmHandleId);
            } else {
                retVal = bo->unbind(osContext, vmHandleId);
            }
        }
    }
    return retVal;
}

int DrmAllocation::bindBOs(OsContext *osContext, uint32_t vmHandleId, std::vector<BufferObject *> *bufferObjects, bool bind) {
    int retVal = 0;
    if (this->storageInfo.getNumBanks() > 1) {
        auto &bos = this->getBOs();
        if (this->storageInfo.tileInstanced) {
            auto bo = bos[vmHandleId];
            retVal = bindBO(bo, osContext, vmHandleId, bufferObjects, bind);
            if (retVal) {
                return retVal;
            }
        } else {
            for (auto bo : bos) {
                retVal = bindBO(bo, osContext, vmHandleId, bufferObjects, bind);
                if (retVal) {
                    return retVal;
                }
            }
        }
    } else {
        auto bo = this->getBO();
        retVal = bindBO(bo, osContext, vmHandleId, bufferObjects, bind);
        if (retVal) {
            return retVal;
        }
    }
    return 0;
}

bool DrmAllocation::prefetchBO(BufferObject *bo, uint32_t vmHandleId, uint32_t subDeviceId) {
    auto drm = bo->peekDrm();
    auto ioctlHelper = drm->getIoctlHelper();
    auto memoryClassDevice = ioctlHelper->getDrmParamValue(DrmParam::MemoryClassDevice);
    auto region = static_cast<uint32_t>((memoryClassDevice << 16u) | subDeviceId);
    auto vmId = drm->getVirtualMemoryAddressSpace(vmHandleId);

    auto result = ioctlHelper->setVmPrefetch(bo->peekAddress(), bo->peekSize(), region, vmId);

    PRINT_DEBUG_STRING(DebugManager.flags.PrintBOPrefetchingResult.get(), stdout,
                       "prefetch BO=%d to VM %u, drmVmId=%u, range: %llx - %llx, size: %lld, region: %x, result: %d\n",
                       bo->peekHandle(), vmId, vmHandleId, bo->peekAddress(), ptrOffset(bo->peekAddress(), bo->peekSize()), bo->peekSize(), region, result);
    return result;
}

void DrmAllocation::registerBOBindExtHandle(Drm *drm) {
    if (!drm->resourceRegistrationEnabled()) {
        return;
    }

    DrmResourceClass resourceClass = DrmResourceClass::MaxSize;

    switch (this->allocationType) {
    case AllocationType::DEBUG_CONTEXT_SAVE_AREA:
        resourceClass = DrmResourceClass::ContextSaveArea;
        break;
    case AllocationType::DEBUG_SBA_TRACKING_BUFFER:
        resourceClass = DrmResourceClass::SbaTrackingBuffer;
        break;
    case AllocationType::KERNEL_ISA:
        resourceClass = DrmResourceClass::Isa;
        break;
    case AllocationType::DEBUG_MODULE_AREA:
        resourceClass = DrmResourceClass::ModuleHeapDebugArea;
        break;
    default:
        break;
    }

    if (resourceClass != DrmResourceClass::MaxSize) {
        auto handle = 0;
        if (resourceClass == DrmResourceClass::Isa) {
            auto deviceBitfiled = static_cast<uint32_t>(this->storageInfo.subDeviceBitfield.to_ulong());
            handle = drm->registerResource(resourceClass, &deviceBitfiled, sizeof(deviceBitfiled));
        } else {
            uint64_t gpuAddress = getGpuAddress();
            handle = drm->registerResource(resourceClass, &gpuAddress, sizeof(gpuAddress));
        }

        registeredBoBindHandles.push_back(handle);

        auto &bos = getBOs();
        uint32_t boIndex = 0u;

        for (auto bo : bos) {
            if (bo) {
                bo->addBindExtHandle(handle);
                bo->markForCapture();
                if (resourceClass == DrmResourceClass::Isa && storageInfo.tileInstanced == true) {
                    auto cookieHandle = drm->registerIsaCookie(handle);
                    bo->addBindExtHandle(cookieHandle);
                    registeredBoBindHandles.push_back(cookieHandle);
                }

                if (resourceClass == DrmResourceClass::SbaTrackingBuffer && getOsContext()) {
                    auto deviceIndex = [=]() -> uint32_t {
                        if (storageInfo.tileInstanced == true) {
                            return boIndex;
                        }
                        auto deviceBitfield = this->storageInfo.subDeviceBitfield;
                        return deviceBitfield.any() ? static_cast<uint32_t>(Math::log2(static_cast<uint32_t>(deviceBitfield.to_ulong()))) : 0u;
                    }();

                    auto contextId = getOsContext()->getOfflineDumpContextId(deviceIndex);
                    auto externalHandle = drm->registerResource(resourceClass, &contextId, sizeof(uint64_t));

                    bo->addBindExtHandle(externalHandle);
                    registeredBoBindHandles.push_back(externalHandle);
                }

                bo->requireImmediateBinding(true);
            }
            boIndex++;
        }
    }
}

void DrmAllocation::linkWithRegisteredHandle(uint32_t handle) {
    auto &bos = getBOs();
    for (auto bo : bos) {
        if (bo) {
            bo->addBindExtHandle(handle);
        }
    }
}

void DrmAllocation::freeRegisteredBOBindExtHandles(Drm *drm) {
    for (auto it = registeredBoBindHandles.rbegin(); it != registeredBoBindHandles.rend(); ++it) {
        drm->unregisterResource(*it);
    }
}

void DrmAllocation::markForCapture() {
    auto &bos = getBOs();
    for (auto bo : bos) {
        if (bo) {
            bo->markForCapture();
        }
    }
}

bool DrmAllocation::shouldAllocationPageFault(const Drm *drm) {
    if (!drm->hasPageFaultSupport()) {
        return false;
    }

    if (DebugManager.flags.EnableImplicitMigrationOnFaultableHardware.get() != -1) {
        return DebugManager.flags.EnableImplicitMigrationOnFaultableHardware.get();
    }

    switch (this->allocationType) {
    case AllocationType::UNIFIED_SHARED_MEMORY:
        return drm->hasKmdMigrationSupport();
    case AllocationType::BUFFER:
        return DebugManager.flags.UseKmdMigrationForBuffers.get() > 0;
    default:
        return false;
    }
}

bool DrmAllocation::setMemAdvise(Drm *drm, MemAdviseFlags flags) {
    bool success = true;

    if (flags.cachedMemory != enabledMemAdviseFlags.cachedMemory) {
        CachePolicy memType = flags.cachedMemory ? CachePolicy::WriteBack : CachePolicy::Uncached;
        setCachePolicy(memType);
    }

    auto ioctlHelper = drm->getIoctlHelper();
    if (flags.nonAtomic != enabledMemAdviseFlags.nonAtomic) {
        for (auto bo : bufferObjects) {
            if (bo != nullptr) {
                success &= ioctlHelper->setVmBoAdvise(bo->peekHandle(), ioctlHelper->getAtomicAdvise(flags.nonAtomic), nullptr);
            }
        }
    }

    if (flags.devicePreferredLocation != enabledMemAdviseFlags.devicePreferredLocation) {
        success &= setPreferredLocation(drm, flags.devicePreferredLocation ? PreferredLocation::Device : PreferredLocation::Clear);
    }

    if (success) {
        enabledMemAdviseFlags = flags;
    }

    return success;
}

bool DrmAllocation::setMemPrefetch(Drm *drm, SubDeviceIdsVec &subDeviceIds) {
    UNRECOVERABLE_IF(subDeviceIds.size() == 0);

    bool success = true;

    if (numHandles > 1) {
        for (uint8_t handleId = 0u; handleId < numHandles; handleId++) {
            auto bo = this->getBOs()[handleId];
            auto subDeviceId = handleId;
            if (DebugManager.flags.KMDSupportForCrossTileMigrationPolicy.get() > 0) {
                subDeviceId = subDeviceIds[handleId % subDeviceIds.size()];
            }
            for (auto vmHandleId : subDeviceIds) {
                success &= prefetchBO(bo, vmHandleId, subDeviceId);
            }
        }
    } else {
        auto bo = this->getBO();
        success = prefetchBO(bo, subDeviceIds[0], subDeviceIds[0]);
    }

    return success;
}

void DrmAllocation::registerMemoryToUnmap(void *pointer, size_t size, DrmAllocation::MemoryUnmapFunction unmapFunction) {
    this->memoryToUnmap.push_back({pointer, size, unmapFunction});
}

uint64_t DrmAllocation::getHandleAddressBase(uint32_t handleIndex) {
    return bufferObjects[handleIndex]->peekAddress();
}

size_t DrmAllocation::getHandleSize(uint32_t handleIndex) {
    return bufferObjects[handleIndex]->peekSize();
}

} // namespace NEO

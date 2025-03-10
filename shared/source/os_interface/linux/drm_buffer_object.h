/*
 * Copyright (C) 2018-2023 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 */

#pragma once

#include "shared/source/command_stream/task_count_helper.h"
#include "shared/source/helpers/common_types.h"
#include "shared/source/helpers/constants.h"
#include "shared/source/memory_manager/definitions/engine_limits.h"
#include "shared/source/memory_manager/memory_operations_status.h"
#include "shared/source/os_interface/linux/cache_info.h"
#include "shared/source/utilities/stackvec.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <mutex>
#include <stdint.h>
#include <utility>
#include <vector>

namespace NEO {

struct ExecBuffer;
struct ExecObject;
class DrmMemoryManager;
class Drm;
class OsContext;

class BufferObjectHandleWrapper {
  private:
    struct ControlBlock {
        int refCount{0};
        int weakRefCount{0};
        std::mutex blockMutex{};
    };

    enum class Ownership : std::uint8_t {
        Weak = 0,
        Strong = 1,
    };

  public:
    explicit BufferObjectHandleWrapper(int boHandle) noexcept
        : boHandle{boHandle} {}

    BufferObjectHandleWrapper(BufferObjectHandleWrapper &&other) noexcept
        : boHandle(std::exchange(other.boHandle, -1)), ownership(other.ownership), controlBlock(std::exchange(other.controlBlock, nullptr)) {}

    ~BufferObjectHandleWrapper();

    BufferObjectHandleWrapper(const BufferObjectHandleWrapper &) = delete;
    BufferObjectHandleWrapper &operator=(const BufferObjectHandleWrapper &) = delete;
    BufferObjectHandleWrapper &operator=(BufferObjectHandleWrapper &&) = delete;

    BufferObjectHandleWrapper acquireSharedOwnership();
    BufferObjectHandleWrapper acquireWeakOwnership();

    bool canCloseBoHandle();

    int getBoHandle() const {
        return boHandle;
    }

    void setBoHandle(int handle) {
        boHandle = handle;
    }

  protected:
    BufferObjectHandleWrapper(int boHandle, Ownership ownership, ControlBlock *controlBlock)
        : boHandle{boHandle}, ownership{ownership}, controlBlock{controlBlock} {}

    int boHandle{};
    Ownership ownership{Ownership::Strong};
    ControlBlock *controlBlock{};
};

class BufferObject {
  public:
    BufferObject(uint32_t rootDeviceIndex, Drm *drm, uint64_t patIndex, int handle, size_t size, size_t maxOsContextCount);
    BufferObject(uint32_t rootDeviceIndex, Drm *drm, uint64_t patIndex, BufferObjectHandleWrapper &&handle, size_t size, size_t maxOsContextCount);

    MOCKABLE_VIRTUAL ~BufferObject() = default;

    struct Deleter {
        void operator()(BufferObject *bo) {
            bo->close();
            delete bo;
        }
    };

    bool setTiling(uint32_t mode, uint32_t stride);

    int pin(BufferObject *const boToPin[], size_t numberOfBos, OsContext *osContext, uint32_t vmHandleId, uint32_t drmContextId);
    MOCKABLE_VIRTUAL int validateHostPtr(BufferObject *const boToPin[], size_t numberOfBos, OsContext *osContext, uint32_t vmHandleId, uint32_t drmContextId);

    MOCKABLE_VIRTUAL int exec(uint32_t used, size_t startOffset, unsigned int flags, bool requiresCoherency, OsContext *osContext, uint32_t vmHandleId, uint32_t drmContextId,
                              BufferObject *const residency[], size_t residencyCount, ExecObject *execObjectsStorage, uint64_t completionGpuAddress, TaskCountType completionValue);

    int bind(OsContext *osContext, uint32_t vmHandleId);
    int unbind(OsContext *osContext, uint32_t vmHandleId);

    void printExecutionBuffer(ExecBuffer &execbuf, const size_t &residencyCount, ExecObject *execObjectsStorage, BufferObject *const residency[]);

    int wait(int64_t timeoutNs);
    bool close();

    inline void reference() {
        this->refCount++;
    }
    inline uint32_t unreference() {
        return this->refCount.fetch_sub(1);
    }
    uint32_t getRefCount() const;

    bool isBoHandleShared() const {
        return boHandleShared;
    }

    void markAsSharedBoHandle() {
        boHandleShared = true;
    }

    BufferObjectHandleWrapper acquireSharedOwnershipOfBoHandle() {
        markAsSharedBoHandle();
        return handle.acquireSharedOwnership();
    }

    BufferObjectHandleWrapper acquireWeakOwnershipOfBoHandle() {
        markAsSharedBoHandle();
        return handle.acquireWeakOwnership();
    }

    size_t peekSize() const { return size; }
    int peekHandle() const { return handle.getBoHandle(); }
    const Drm *peekDrm() const { return drm; }
    uint64_t peekAddress() const { return gpuAddress; }
    void setAddress(uint64_t address);
    void *peekLockedAddress() const { return lockedAddress; }
    void setLockedAddress(void *cpuAddress) { this->lockedAddress = cpuAddress; }
    void setUnmapSize(uint64_t unmapSize) { this->unmapSize = unmapSize; }
    uint64_t peekUnmapSize() const { return unmapSize; }
    bool peekIsReusableAllocation() const { return this->isReused; }
    void markAsReusableAllocation() { this->isReused = true; }
    void addBindExtHandle(uint32_t handle);
    const StackVec<uint32_t, 2> &getBindExtHandles() const { return bindExtHandles; }
    void markForCapture() {
        allowCapture = true;
    }
    bool isMarkedForCapture() {
        return allowCapture;
    }

    bool isImmediateBindingRequired() {
        return requiresImmediateBinding;
    }
    void requireImmediateBinding(bool required) {
        requiresImmediateBinding = required;
    }

    bool isExplicitResidencyRequired() {
        return requiresExplicitResidency;
    }
    void requireExplicitResidency(bool required) {
        requiresExplicitResidency = required;
    }
    uint32_t getRootDeviceIndex() {
        return rootDeviceIndex;
    }
    int getHandle() {
        return handle.getBoHandle();
    }

    void setCacheRegion(CacheRegion regionIndex) { cacheRegion = regionIndex; }
    CacheRegion peekCacheRegion() const { return cacheRegion; }

    void setCachePolicy(CachePolicy memType) { cachePolicy = memType; }
    CachePolicy peekCachePolicy() const { return cachePolicy; }

    void setColourWithBind() {
        this->colourWithBind = true;
    }
    void setColourChunk(size_t size) {
        this->colourChunk = size;
    }
    void addColouringAddress(uint64_t address) {
        this->bindAddresses.push_back(address);
    }
    void reserveAddressVector(size_t size) {
        this->bindAddresses.reserve(size);
    }

    bool getColourWithBind() {
        return this->colourWithBind;
    }
    size_t getColourChunk() {
        return this->colourChunk;
    }
    std::vector<uint64_t> &getColourAddresses() {
        return this->bindAddresses;
    }
    uint64_t peekPatIndex() const { return patIndex; }
    void setPatIndex(uint64_t newPatIndex) { this->patIndex = newPatIndex; }

    static constexpr int gpuHangDetected{-7171};

    uint32_t getOsContextId(OsContext *osContext);
    std::vector<std::array<bool, EngineLimits::maxHandleCount>> bindInfo;

  protected:
    MOCKABLE_VIRTUAL MemoryOperationsStatus evictUnusedAllocations(bool waitForCompletion, bool isLockNeeded);

    Drm *drm = nullptr;
    bool perContextVmsUsed = false;
    std::atomic<uint32_t> refCount;

    uint32_t rootDeviceIndex = std::numeric_limits<uint32_t>::max();
    BufferObjectHandleWrapper handle; // i915 gem object handle
    uint64_t size;
    bool isReused = false;
    bool boHandleShared = false;

    uint32_t tilingMode;
    bool allowCapture = false;
    bool requiresImmediateBinding = false;
    bool requiresExplicitResidency = false;

    MOCKABLE_VIRTUAL void fillExecObject(ExecObject &execObject, OsContext *osContext, uint32_t vmHandleId, uint32_t drmContextId);
    void printBOBindingResult(OsContext *osContext, uint32_t vmHandleId, bool bind, int retVal);

    void *lockedAddress; // CPU side virtual address

    uint64_t unmapSize = 0;
    uint64_t patIndex = CommonConstants::unsupportedPatIndex;

    CacheRegion cacheRegion = CacheRegion::Default;
    CachePolicy cachePolicy = CachePolicy::WriteBack;

    StackVec<uint32_t, 2> bindExtHandles;

    bool colourWithBind = false;
    size_t colourChunk = 0;
    std::vector<uint64_t> bindAddresses;

  private:
    uint64_t gpuAddress = 0llu;
};
} // namespace NEO

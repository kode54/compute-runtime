/*
 * Copyright (C) 2018-2023 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 */

#pragma once

#include "shared/source/helpers/mt_helpers.h"
#include "shared/source/memory_manager/definitions/engine_limits.h"
#include "shared/source/os_interface/os_context.h"

#include <array>
#include <atomic>
#include <vector>

namespace NEO {
class Drm;

class OsContextLinux : public OsContext {
  public:
    OsContextLinux() = delete;
    ~OsContextLinux() override;
    OsContextLinux(Drm &drm, uint32_t rootDeviceIndex, uint32_t contextId, const EngineDescriptor &engineDescriptor);

    unsigned int getEngineFlag() const { return engineFlag; }
    void setEngineFlag(unsigned int engineFlag) { this->engineFlag = engineFlag; }
    const std::vector<uint32_t> &getDrmContextIds() const { return drmContextIds; }
    const std::vector<uint32_t> &getDrmVmIds() const { return drmVmIds; }
    void setNewResourceBound() {
        tlbFlushCounter++;
    };

    uint32_t peekTlbFlushCounter() const { return tlbFlushCounter.load(); }

    void setTlbFlushed(uint32_t newCounter) {
        NEO::MultiThreadHelpers::interlockedMax(lastFlushedTlbFlushCounter, newCounter);
    };
    bool isTlbFlushRequired() const {
        return (tlbFlushCounter.load() > lastFlushedTlbFlushCounter.load());
    };
    bool isDirectSubmissionSupported(const HardwareInfo &hwInfo) const override;
    Drm &getDrm() const;
    void waitForPagingFence();
    static OsContext *create(OSInterface *osInterface, uint32_t rootDeviceIndex, uint32_t contextId, const EngineDescriptor &engineDescriptor);
    void reInitializeContext() override;
    void setHangDetected() {
        contextHangDetected = true;
    }
    bool isHangDetected() const {
        return contextHangDetected;
    }

    uint64_t getOfflineDumpContextId(uint32_t deviceIndex) const override;

    uint64_t getNextFenceVal(uint32_t deviceIndex) { return fenceVal[deviceIndex] + 1; }
    void incFenceVal(uint32_t deviceIndex) { fenceVal[deviceIndex]++; }
    uint64_t *getFenceAddr(uint32_t deviceIndex) { return &pagingFence[deviceIndex]; }
    void waitForBind(uint32_t drmIterator);

  protected:
    bool initializeContext() override;

    std::atomic<uint32_t> tlbFlushCounter{0};
    std::atomic<uint32_t> lastFlushedTlbFlushCounter{0};
    unsigned int engineFlag = 0;
    std::vector<uint32_t> drmContextIds;
    std::vector<uint32_t> drmVmIds;

    std::array<uint64_t, EngineLimits::maxHandleCount> pagingFence;
    std::array<uint64_t, EngineLimits::maxHandleCount> fenceVal;

    Drm &drm;
    bool contextHangDetected = false;
};
} // namespace NEO

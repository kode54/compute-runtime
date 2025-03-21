/*
 * Copyright (C) 2020-2023 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 */

#pragma once
#include "shared/source/helpers/non_copyable_or_moveable.h"

#include "level_zero/sysman/source/memory/memory.h"
#include "level_zero/sysman/source/memory/os_memory.h"
#include <level_zero/zes_api.h>

namespace L0 {
namespace Sysman {

class MemoryImp : public Memory, NEO::NonCopyableOrMovableClass {
  public:
    ze_result_t memoryGetProperties(zes_mem_properties_t *pProperties) override;
    ze_result_t memoryGetBandwidth(zes_mem_bandwidth_t *pBandwidth) override;
    ze_result_t memoryGetState(zes_mem_state_t *pState) override;
    ze_result_t memoryGetBandwidthEx(uint64_t *pReadCounters, uint64_t *pWriteCounters, uint64_t *pMaxBandwidth, uint64_t timeout) override;

    MemoryImp(OsSysman *pOsSysman, bool onSubdevice, uint32_t subDeviceId);
    ~MemoryImp() override;

    MemoryImp() = default;
    void init();
    std::unique_ptr<OsMemory> pOsMemory;

  private:
    zes_mem_properties_t memoryProperties = {};
};

} // namespace Sysman
} // namespace L0

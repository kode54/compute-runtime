/*
 * Copyright (C) 2023 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 */

#pragma once
#include "shared/source/helpers/non_copyable_or_moveable.h"

#include "level_zero/sysman/source/global_operations/global_operations.h"
#include "level_zero/sysman/source/global_operations/os_global_operations.h"

#include <mutex>

namespace L0 {
namespace Sysman {

class GlobalOperationsImp : public GlobalOperations, NEO::NonCopyableOrMovableClass {
  public:
    void init() override;
    ze_result_t reset(ze_bool_t force) override;
    ze_result_t deviceGetProperties(zes_device_properties_t *pProperties) override;
    ze_result_t processesGetState(uint32_t *pCount, zes_process_state_t *pProcesses) override;
    ze_result_t deviceGetState(zes_device_state_t *pState) override;
    OsGlobalOperations *pOsGlobalOperations = nullptr;

    GlobalOperationsImp() = default;
    GlobalOperationsImp(OsSysman *pOsSysman) : pOsSysman(pOsSysman){};
    ~GlobalOperationsImp() override;

  private:
    OsSysman *pOsSysman = nullptr;
    zes_device_properties_t sysmanProperties = {};
    std::once_flag initGlobalOpOnce;
    void initGlobalOperations();
};

} // namespace Sysman
} // namespace L0

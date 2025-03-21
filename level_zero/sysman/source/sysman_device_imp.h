/*
 * Copyright (C) 2023 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 */

#pragma once
#include "shared/source/execution_environment/execution_environment.h"
#include "shared/source/execution_environment/root_device_environment.h"
#include "shared/source/helpers/hw_info.h"
#include "shared/source/helpers/non_copyable_or_moveable.h"

#include "level_zero/sysman/source/sysman_device.h"

#include <unordered_map>

namespace L0 {
namespace Sysman {
struct OsSysman;

struct SysmanDeviceImp : SysmanDevice, NEO::NonCopyableOrMovableClass {

    SysmanDeviceImp(NEO::ExecutionEnvironment *executionEnvironment, const uint32_t rootDeviceIndex);
    ~SysmanDeviceImp() override;

    SysmanDeviceImp() = delete;
    ze_result_t init();

    OsSysman *pOsSysman = nullptr;

    const NEO::RootDeviceEnvironment &getRootDeviceEnvironment() const {
        return *executionEnvironment->rootDeviceEnvironments[rootDeviceIndex];
    }
    const NEO::HardwareInfo &getHardwareInfo() const override { return *getRootDeviceEnvironment().getHardwareInfo(); }
    PRODUCT_FAMILY getProductFamily() const { return getHardwareInfo().platform.eProductFamily; }
    NEO::ExecutionEnvironment *getExecutionEnvironment() const { return executionEnvironment; }
    uint32_t getRootDeviceIndex() const { return rootDeviceIndex; }

    GlobalOperations *pGlobalOperations = nullptr;
    PowerHandleContext *pPowerHandleContext = nullptr;
    FabricPortHandleContext *pFabricPortHandleContext = nullptr;
    MemoryHandleContext *pMemoryHandleContext = nullptr;
    EngineHandleContext *pEngineHandleContext = nullptr;
    SchedulerHandleContext *pSchedulerHandleContext = nullptr;
    FirmwareHandleContext *pFirmwareHandleContext = nullptr;
    RasHandleContext *pRasHandleContext = nullptr;
    DiagnosticsHandleContext *pDiagnosticsHandleContext = nullptr;
    FrequencyHandleContext *pFrequencyHandleContext = nullptr;
    StandbyHandleContext *pStandbyHandleContext = nullptr;
    PerformanceHandleContext *pPerformanceHandleContext = nullptr;
    Ecc *pEcc = nullptr;
    TemperatureHandleContext *pTempHandleContext = nullptr;
    Pci *pPci = nullptr;

    ze_result_t powerGet(uint32_t *pCount, zes_pwr_handle_t *phPower) override;
    ze_result_t powerGetCardDomain(zes_pwr_handle_t *phPower) override;
    ze_result_t memoryGet(uint32_t *pCount, zes_mem_handle_t *phMemory) override;
    ze_result_t fabricPortGet(uint32_t *pCount, zes_fabric_port_handle_t *phPort) override;
    ze_result_t engineGet(uint32_t *pCount, zes_engine_handle_t *phEngine) override;
    ze_result_t schedulerGet(uint32_t *pCount, zes_sched_handle_t *phScheduler) override;
    ze_result_t frequencyGet(uint32_t *pCount, zes_freq_handle_t *phFrequency) override;
    ze_result_t firmwareGet(uint32_t *pCount, zes_firmware_handle_t *phFirmware) override;
    ze_result_t rasGet(uint32_t *pCount, zes_ras_handle_t *phRas) override;
    ze_result_t diagnosticsGet(uint32_t *pCount, zes_diag_handle_t *phFirmware) override;
    ze_result_t deviceGetProperties(zes_device_properties_t *pProperties) override;
    ze_result_t processesGetState(uint32_t *pCount, zes_process_state_t *pProcesses) override;
    ze_result_t deviceReset(ze_bool_t force) override;
    ze_result_t deviceGetState(zes_device_state_t *pState) override;
    ze_result_t standbyGet(uint32_t *pCount, zes_standby_handle_t *phStandby) override;
    ze_result_t deviceEccAvailable(ze_bool_t *pAvailable) override;
    ze_result_t deviceEccConfigurable(ze_bool_t *pConfigurable) override;
    ze_result_t deviceGetEccState(zes_device_ecc_properties_t *pState) override;
    ze_result_t deviceSetEccState(const zes_device_ecc_desc_t *newState, zes_device_ecc_properties_t *pState) override;
    ze_result_t temperatureGet(uint32_t *pCount, zes_temp_handle_t *phTemperature) override;
    ze_result_t performanceGet(uint32_t *pCount, zes_perf_handle_t *phPerformance) override;
    ze_result_t pciGetProperties(zes_pci_properties_t *pProperties) override;
    ze_result_t pciGetState(zes_pci_state_t *pState) override;
    ze_result_t pciGetBars(uint32_t *pCount, zes_pci_bar_properties_t *pProperties) override;
    ze_result_t pciGetStats(zes_pci_stats_t *pStats) override;

  private:
    NEO::ExecutionEnvironment *executionEnvironment = nullptr;
    const uint32_t rootDeviceIndex;
    template <typename T>
    void inline freeResource(T *&resource) {
        if (resource) {
            delete resource;
            resource = nullptr;
        }
    }
};

} // namespace Sysman
} // namespace L0

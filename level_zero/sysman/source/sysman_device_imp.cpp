/*
 * Copyright (C) 2023 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 */

#include "level_zero/sysman/source/sysman_device_imp.h"

#include "shared/source/helpers/debug_helpers.h"

#include "level_zero/sysman/source/ecc/ecc_imp.h"
#include "level_zero/sysman/source/global_operations/global_operations_imp.h"
#include "level_zero/sysman/source/os_sysman.h"
#include "level_zero/sysman/source/pci/pci_imp.h"

#include <vector>

namespace L0 {
namespace Sysman {

SysmanDeviceImp::SysmanDeviceImp(NEO::ExecutionEnvironment *executionEnvironment, const uint32_t rootDeviceIndex)
    : executionEnvironment(executionEnvironment), rootDeviceIndex(rootDeviceIndex) {
    this->executionEnvironment->incRefInternal();
    pOsSysman = OsSysman::create(this);
    UNRECOVERABLE_IF(nullptr == pOsSysman);
    pFabricPortHandleContext = new FabricPortHandleContext(pOsSysman);
    pMemoryHandleContext = new MemoryHandleContext(pOsSysman);
    pPowerHandleContext = new PowerHandleContext(pOsSysman);
    pEngineHandleContext = new EngineHandleContext(pOsSysman);
    pFrequencyHandleContext = new FrequencyHandleContext(pOsSysman);
    pSchedulerHandleContext = new SchedulerHandleContext(pOsSysman);
    pFirmwareHandleContext = new FirmwareHandleContext(pOsSysman);
    pRasHandleContext = new RasHandleContext(pOsSysman);
    pDiagnosticsHandleContext = new DiagnosticsHandleContext(pOsSysman);
    pGlobalOperations = new GlobalOperationsImp(pOsSysman);
    pStandbyHandleContext = new StandbyHandleContext(pOsSysman);
    pPerformanceHandleContext = new PerformanceHandleContext(pOsSysman);
    pEcc = new EccImp(pOsSysman);
    pTempHandleContext = new TemperatureHandleContext(pOsSysman);
    pPci = new PciImp(pOsSysman);
}

SysmanDeviceImp::~SysmanDeviceImp() {
    freeResource(pGlobalOperations);
    freeResource(pDiagnosticsHandleContext);
    freeResource(pRasHandleContext);
    freeResource(pFirmwareHandleContext);
    freeResource(pSchedulerHandleContext);
    freeResource(pFrequencyHandleContext);
    freeResource(pEngineHandleContext);
    freeResource(pPowerHandleContext);
    freeResource(pMemoryHandleContext);
    freeResource(pFabricPortHandleContext);
    freeResource(pStandbyHandleContext);
    freeResource(pPerformanceHandleContext);
    freeResource(pEcc);
    freeResource(pTempHandleContext);
    freeResource(pPci);
    freeResource(pOsSysman);
    executionEnvironment->decRefInternal();
}

ze_result_t SysmanDeviceImp::init() {
    auto result = pOsSysman->init();
    if (ZE_RESULT_SUCCESS != result) {
        return result;
    }
    return result;
}

ze_result_t SysmanDeviceImp::deviceGetProperties(zes_device_properties_t *pProperties) {
    return pGlobalOperations->deviceGetProperties(pProperties);
}

ze_result_t SysmanDeviceImp::processesGetState(uint32_t *pCount, zes_process_state_t *pProcesses) {
    return pGlobalOperations->processesGetState(pCount, pProcesses);
}

ze_result_t SysmanDeviceImp::deviceReset(ze_bool_t force) {
    return pGlobalOperations->reset(force);
}

ze_result_t SysmanDeviceImp::deviceGetState(zes_device_state_t *pState) {
    return pGlobalOperations->deviceGetState(pState);
}

ze_result_t SysmanDeviceImp::fabricPortGet(uint32_t *pCount, zes_fabric_port_handle_t *phPort) {
    return pFabricPortHandleContext->fabricPortGet(pCount, phPort);
}

ze_result_t SysmanDeviceImp::memoryGet(uint32_t *pCount, zes_mem_handle_t *phMemory) {
    return pMemoryHandleContext->memoryGet(pCount, phMemory);
}

ze_result_t SysmanDeviceImp::powerGetCardDomain(zes_pwr_handle_t *phPower) {
    return pPowerHandleContext->powerGetCardDomain(phPower);
}

ze_result_t SysmanDeviceImp::powerGet(uint32_t *pCount, zes_pwr_handle_t *phPower) {
    return pPowerHandleContext->powerGet(pCount, phPower);
}

ze_result_t SysmanDeviceImp::engineGet(uint32_t *pCount, zes_engine_handle_t *phEngine) {
    return pEngineHandleContext->engineGet(pCount, phEngine);
}

ze_result_t SysmanDeviceImp::frequencyGet(uint32_t *pCount, zes_freq_handle_t *phFrequency) {
    return pFrequencyHandleContext->frequencyGet(pCount, phFrequency);
}

ze_result_t SysmanDeviceImp::schedulerGet(uint32_t *pCount, zes_sched_handle_t *phScheduler) {
    return pSchedulerHandleContext->schedulerGet(pCount, phScheduler);
}

ze_result_t SysmanDeviceImp::rasGet(uint32_t *pCount, zes_ras_handle_t *phRas) {
    return pRasHandleContext->rasGet(pCount, phRas);
}

ze_result_t SysmanDeviceImp::firmwareGet(uint32_t *pCount, zes_firmware_handle_t *phFirmware) {
    return pFirmwareHandleContext->firmwareGet(pCount, phFirmware);
}

ze_result_t SysmanDeviceImp::diagnosticsGet(uint32_t *pCount, zes_diag_handle_t *phDiagnostics) {
    return pDiagnosticsHandleContext->diagnosticsGet(pCount, phDiagnostics);
}

ze_result_t SysmanDeviceImp::deviceEccAvailable(ze_bool_t *pAvailable) {
    return pEcc->deviceEccAvailable(pAvailable);
}

ze_result_t SysmanDeviceImp::deviceEccConfigurable(ze_bool_t *pConfigurable) {
    return pEcc->deviceEccConfigurable(pConfigurable);
}

ze_result_t SysmanDeviceImp::deviceGetEccState(zes_device_ecc_properties_t *pState) {
    return pEcc->getEccState(pState);
}

ze_result_t SysmanDeviceImp::deviceSetEccState(const zes_device_ecc_desc_t *newState, zes_device_ecc_properties_t *pState) {
    return pEcc->setEccState(newState, pState);
}

ze_result_t SysmanDeviceImp::standbyGet(uint32_t *pCount, zes_standby_handle_t *phStandby) {
    return pStandbyHandleContext->standbyGet(pCount, phStandby);
}

ze_result_t SysmanDeviceImp::temperatureGet(uint32_t *pCount, zes_temp_handle_t *phTemperature) {
    return pTempHandleContext->temperatureGet(pCount, phTemperature);
}

ze_result_t SysmanDeviceImp::performanceGet(uint32_t *pCount, zes_perf_handle_t *phPerformance) {
    return pPerformanceHandleContext->performanceGet(pCount, phPerformance);
}

ze_result_t SysmanDeviceImp::pciGetProperties(zes_pci_properties_t *pProperties) {
    return pPci->pciStaticProperties(pProperties);
}

ze_result_t SysmanDeviceImp::pciGetState(zes_pci_state_t *pState) {
    return pPci->pciGetState(pState);
}

ze_result_t SysmanDeviceImp::pciGetBars(uint32_t *pCount, zes_pci_bar_properties_t *pProperties) {
    return pPci->pciGetInitializedBars(pCount, pProperties);
}

ze_result_t SysmanDeviceImp::pciGetStats(zes_pci_stats_t *pStats) {
    return ZE_RESULT_ERROR_UNSUPPORTED_FEATURE;
}

} // namespace Sysman
} // namespace L0

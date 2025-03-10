/*
 * Copyright (C) 2018-2023 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 */

#include "shared/source/execution_environment/execution_environment.h"

#include "shared/source/built_ins/built_ins.h"
#include "shared/source/built_ins/sip.h"
#include "shared/source/debug_settings/debug_settings_manager.h"
#include "shared/source/direct_submission/direct_submission_controller.h"
#include "shared/source/execution_environment/root_device_environment.h"
#include "shared/source/helpers/affinity_mask.h"
#include "shared/source/helpers/driver_model_type.h"
#include "shared/source/helpers/gfx_core_helper.h"
#include "shared/source/helpers/hw_info.h"
#include "shared/source/helpers/string_helpers.h"
#include "shared/source/memory_manager/memory_manager.h"
#include "shared/source/memory_manager/os_agnostic_memory_manager.h"
#include "shared/source/os_interface/os_environment.h"
#include "shared/source/os_interface/os_interface.h"
#include "shared/source/os_interface/product_helper.h"
#include "shared/source/utilities/wait_util.h"

namespace NEO {
ExecutionEnvironment::ExecutionEnvironment() {
    WaitUtils::init();
    this->configureNeoEnvironment();
}

void ExecutionEnvironment::releaseRootDeviceEnvironmentResources(RootDeviceEnvironment *rootDeviceEnvironment) {
    if (rootDeviceEnvironment == nullptr) {
        return;
    }
    SipKernel::freeSipKernels(rootDeviceEnvironment, memoryManager.get());
    if (rootDeviceEnvironment->builtins.get()) {
        rootDeviceEnvironment->builtins->freeSipKernels(memoryManager.get());
    }
}

ExecutionEnvironment::~ExecutionEnvironment() {
    if (memoryManager) {
        memoryManager->commonCleanup();
        for (const auto &rootDeviceEnvironment : this->rootDeviceEnvironments) {
            releaseRootDeviceEnvironmentResources(rootDeviceEnvironment.get());
        }
    }
    rootDeviceEnvironments.clear();
}

bool ExecutionEnvironment::initializeMemoryManager() {
    if (this->memoryManager) {
        return memoryManager->isInitialized();
    }

    int32_t setCommandStreamReceiverType = CommandStreamReceiverType::CSR_HW;
    if (DebugManager.flags.SetCommandStreamReceiver.get() >= 0) {
        setCommandStreamReceiverType = DebugManager.flags.SetCommandStreamReceiver.get();
    }

    switch (setCommandStreamReceiverType) {
    case CommandStreamReceiverType::CSR_TBX:
    case CommandStreamReceiverType::CSR_TBX_WITH_AUB:
    case CommandStreamReceiverType::CSR_AUB:
        memoryManager = std::make_unique<OsAgnosticMemoryManager>(*this);
        break;
    case CommandStreamReceiverType::CSR_HW:
    case CommandStreamReceiverType::CSR_HW_WITH_AUB:
    default: {
        auto driverModelType = DriverModelType::UNKNOWN;
        if (this->rootDeviceEnvironments[0]->osInterface && this->rootDeviceEnvironments[0]->osInterface->getDriverModel()) {
            driverModelType = this->rootDeviceEnvironments[0]->osInterface->getDriverModel()->getDriverModelType();
        }
        memoryManager = MemoryManager::createMemoryManager(*this, driverModelType);
    } break;
    }

    return memoryManager->isInitialized();
}

void ExecutionEnvironment::calculateMaxOsContextCount() {
    MemoryManager::maxOsContextCount = 0u;
    for (const auto &rootDeviceEnvironment : this->rootDeviceEnvironments) {
        auto hwInfo = rootDeviceEnvironment->getHardwareInfo();
        auto &gfxCoreHelper = rootDeviceEnvironment->getHelper<GfxCoreHelper>();
        auto osContextCount = static_cast<uint32_t>(gfxCoreHelper.getGpgpuEngineInstances(*rootDeviceEnvironment).size());
        auto subDevicesCount = GfxCoreHelper::getSubDevicesCount(hwInfo);
        auto ccsCount = hwInfo->gtSystemInfo.CCSInfo.NumberOfCCSEnabled;
        bool hasRootCsr = subDevicesCount > 1;

        MemoryManager::maxOsContextCount += osContextCount * subDevicesCount + hasRootCsr;

        if (ccsCount > 1 && DebugManager.flags.EngineInstancedSubDevices.get()) {
            MemoryManager::maxOsContextCount += ccsCount * subDevicesCount;
        }
    }
}

DirectSubmissionController *ExecutionEnvironment::initializeDirectSubmissionController() {
    auto initializeDirectSubmissionController = DirectSubmissionController::isSupported();

    if (DebugManager.flags.SetCommandStreamReceiver.get() > 0) {
        initializeDirectSubmissionController = false;
    }

    if (DebugManager.flags.EnableDirectSubmissionController.get() != -1) {
        initializeDirectSubmissionController = DebugManager.flags.EnableDirectSubmissionController.get();
    }

    if (initializeDirectSubmissionController && this->directSubmissionController == nullptr) {
        this->directSubmissionController = std::make_unique<DirectSubmissionController>();
    }

    return directSubmissionController.get();
}

void ExecutionEnvironment::prepareRootDeviceEnvironments(uint32_t numRootDevices) {
    if (rootDeviceEnvironments.size() < numRootDevices) {
        rootDeviceEnvironments.resize(numRootDevices);
    }
    for (auto rootDeviceIndex = 0u; rootDeviceIndex < numRootDevices; rootDeviceIndex++) {
        if (!rootDeviceEnvironments[rootDeviceIndex]) {
            rootDeviceEnvironments[rootDeviceIndex] = std::make_unique<RootDeviceEnvironment>(*this);
        }
    }
}

void ExecutionEnvironment::prepareForCleanup() const {
    for (auto &rootDeviceEnvironment : rootDeviceEnvironments) {
        if (rootDeviceEnvironment) {
            rootDeviceEnvironment->prepareForCleanup();
        }
    }
}

void ExecutionEnvironment::prepareRootDeviceEnvironment(const uint32_t rootDeviceIndexForReInit) {
    rootDeviceEnvironments[rootDeviceIndexForReInit] = std::make_unique<RootDeviceEnvironment>(*this);
}

void ExecutionEnvironment::parseAffinityMask() {
    const auto &affinityMaskString = DebugManager.flags.ZE_AFFINITY_MASK.get();

    if (affinityMaskString.compare("default") == 0 ||
        affinityMaskString.empty()) {
        return;
    }

    bool exposeSubDevicesAsApiDevices = false;
    if (NEO::DebugManager.flags.ReturnSubDevicesAsApiDevices.get() != -1) {
        exposeSubDevicesAsApiDevices = NEO::DebugManager.flags.ReturnSubDevicesAsApiDevices.get();
    }

    uint32_t numRootDevices = static_cast<uint32_t>(rootDeviceEnvironments.size());

    RootDeviceIndicesMap mapOfIndexes;
    // Reserve at least for a size equal to rootDeviceEnvironments.size() times four,
    // which is enough for typical configurations
    size_t reservedSizeForIndices = numRootDevices * 4;
    mapOfIndexes.reserve(reservedSizeForIndices);
    if (exposeSubDevicesAsApiDevices) {
        uint32_t currentDeviceIndex = 0;
        for (uint32_t currentRootDevice = 0u; currentRootDevice < static_cast<uint32_t>(rootDeviceEnvironments.size()); currentRootDevice++) {
            auto hwInfo = rootDeviceEnvironments[currentRootDevice]->getHardwareInfo();
            auto subDevicesCount = GfxCoreHelper::getSubDevicesCount(hwInfo);
            uint32_t currentSubDevice = 0;
            mapOfIndexes[currentDeviceIndex++] = std::make_tuple(currentRootDevice, currentSubDevice);
            for (currentSubDevice = 1; currentSubDevice < subDevicesCount; currentSubDevice++) {
                mapOfIndexes[currentDeviceIndex++] = std::make_tuple(currentRootDevice, currentSubDevice);
            }
        }

        numRootDevices = currentDeviceIndex;
        UNRECOVERABLE_IF(numRootDevices > reservedSizeForIndices);
    }

    std::vector<AffinityMaskHelper> affinityMaskHelper(numRootDevices);

    auto affinityMaskEntries = StringHelpers::split(affinityMaskString, ",");

    for (const auto &entry : affinityMaskEntries) {
        auto subEntries = StringHelpers::split(entry, ".");
        uint32_t rootDeviceIndex = StringHelpers::toUint32t(subEntries[0]);

        // tiles as devices
        if (exposeSubDevicesAsApiDevices) {
            if (rootDeviceIndex > numRootDevices) {
                continue;
            }

            // ReturnSubDevicesAsApiDevices not supported with AllowSingleTileEngineInstancedSubDevices
            // so ignore X.Y
            if (subEntries.size() > 1) {
                continue;
            }

            std::tuple<uint32_t, uint32_t> indexKey = mapOfIndexes[rootDeviceIndex];
            auto deviceIndex = std::get<0>(indexKey);
            auto tileIndex = std::get<1>(indexKey);
            affinityMaskHelper[deviceIndex].enableGenericSubDevice(tileIndex);

            continue;
        }

        // cards as devices
        if (rootDeviceIndex < numRootDevices) {
            auto hwInfo = rootDeviceEnvironments[rootDeviceIndex]->getHardwareInfo();
            auto subDevicesCount = GfxCoreHelper::getSubDevicesCount(hwInfo);

            if (subEntries.size() > 1) {
                uint32_t subDeviceIndex = StringHelpers::toUint32t(subEntries[1]);

                bool enableSecondLevelEngineInstanced = ((subDevicesCount == 1) &&
                                                         (hwInfo->gtSystemInfo.CCSInfo.NumberOfCCSEnabled > 1) &&
                                                         DebugManager.flags.AllowSingleTileEngineInstancedSubDevices.get());

                if (enableSecondLevelEngineInstanced) {
                    UNRECOVERABLE_IF(subEntries.size() != 2);

                    if (subDeviceIndex < hwInfo->gtSystemInfo.CCSInfo.NumberOfCCSEnabled) {
                        affinityMaskHelper[rootDeviceIndex].enableEngineInstancedSubDevice(0, subDeviceIndex); // Mask: X.Y
                    }
                } else if (subDeviceIndex < subDevicesCount) {
                    if (subEntries.size() == 2) {
                        affinityMaskHelper[rootDeviceIndex].enableGenericSubDevice(subDeviceIndex); // Mask: X.Y
                    } else {
                        UNRECOVERABLE_IF(subEntries.size() != 3);
                        uint32_t ccsIndex = StringHelpers::toUint32t(subEntries[2]);

                        if (ccsIndex < hwInfo->gtSystemInfo.CCSInfo.NumberOfCCSEnabled) {
                            affinityMaskHelper[rootDeviceIndex].enableEngineInstancedSubDevice(subDeviceIndex, ccsIndex); // Mask: X.Y.Z
                        }
                    }
                }
            } else {
                affinityMaskHelper[rootDeviceIndex].enableAllGenericSubDevices(subDevicesCount); // Mask: X
            }
        }
    }

    std::vector<std::unique_ptr<RootDeviceEnvironment>> filteredEnvironments;
    for (uint32_t i = 0u; i < numRootDevices; i++) {
        if (!affinityMaskHelper[i].isDeviceEnabled()) {
            continue;
        }

        rootDeviceEnvironments[i]->deviceAffinityMask = affinityMaskHelper[i];
        filteredEnvironments.emplace_back(rootDeviceEnvironments[i].release());
    }

    rootDeviceEnvironments.swap(filteredEnvironments);
}

void ExecutionEnvironment::adjustCcsCountImpl(RootDeviceEnvironment *rootDeviceEnvironment) const {
    auto hwInfo = rootDeviceEnvironment->getMutableHardwareInfo();
    auto &productHelper = rootDeviceEnvironment->getHelper<ProductHelper>();
    productHelper.adjustNumberOfCcs(*hwInfo);
}

void ExecutionEnvironment::adjustCcsCount() {
    parseCcsCountLimitations();

    for (auto rootDeviceIndex = 0u; rootDeviceIndex < rootDeviceEnvironments.size(); rootDeviceIndex++) {
        auto &rootDeviceEnvironment = rootDeviceEnvironments[rootDeviceIndex];
        UNRECOVERABLE_IF(!rootDeviceEnvironment);
        if (!rootDeviceEnvironment->isNumberOfCcsLimited()) {
            adjustCcsCountImpl(rootDeviceEnvironment.get());
        }
    }
}

void ExecutionEnvironment::adjustCcsCount(const uint32_t rootDeviceIndex) const {
    auto &rootDeviceEnvironment = rootDeviceEnvironments[rootDeviceIndex];
    UNRECOVERABLE_IF(!rootDeviceEnvironment);
    if (rootDeviceNumCcsMap.find(rootDeviceIndex) != rootDeviceNumCcsMap.end()) {
        rootDeviceEnvironment->limitNumberOfCcs(rootDeviceNumCcsMap.at(rootDeviceIndex));
    } else {
        adjustCcsCountImpl(rootDeviceEnvironment.get());
    }
}

void ExecutionEnvironment::parseCcsCountLimitations() {
    const auto &numberOfCcsString = DebugManager.flags.ZEX_NUMBER_OF_CCS.get();

    if (numberOfCcsString.compare("default") == 0 ||
        numberOfCcsString.empty()) {
        return;
    }

    const uint32_t numRootDevices = static_cast<uint32_t>(rootDeviceEnvironments.size());

    auto numberOfCcsEntries = StringHelpers::split(numberOfCcsString, ",");

    for (const auto &entry : numberOfCcsEntries) {
        auto subEntries = StringHelpers::split(entry, ":");
        uint32_t rootDeviceIndex = StringHelpers::toUint32t(subEntries[0]);

        if (rootDeviceIndex < numRootDevices) {
            if (subEntries.size() > 1) {
                uint32_t maxCcsCount = StringHelpers::toUint32t(subEntries[1]);
                rootDeviceNumCcsMap.insert({rootDeviceIndex, maxCcsCount});
                rootDeviceEnvironments[rootDeviceIndex]->limitNumberOfCcs(maxCcsCount);
            }
        }
    }
}

void ExecutionEnvironment::configureNeoEnvironment() {
    if (DebugManager.flags.NEO_CAL_ENABLED.get()) {
        DebugManager.flags.UseKmdMigration.setIfDefault(0);
        DebugManager.flags.SplitBcsSize.setIfDefault(256);
    }
}
} // namespace NEO

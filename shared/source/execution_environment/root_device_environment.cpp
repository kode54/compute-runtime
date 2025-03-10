/*
 * Copyright (C) 2019-2023 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 */

#include "shared/source/execution_environment/root_device_environment.h"

#include "shared/source/ail/ail_configuration.h"
#include "shared/source/assert_handler/assert_handler.h"
#include "shared/source/aub/aub_center.h"
#include "shared/source/built_ins/built_ins.h"
#include "shared/source/built_ins/sip.h"
#include "shared/source/compiler_interface/compiler_interface.h"
#include "shared/source/compiler_interface/default_cache_config.h"
#include "shared/source/debugger/debugger.h"
#include "shared/source/debugger/debugger_l0.h"
#include "shared/source/device/device.h"
#include "shared/source/execution_environment/execution_environment.h"
#include "shared/source/gmm_helper/gmm_helper.h"
#include "shared/source/gmm_helper/page_table_mngr.h"
#include "shared/source/helpers/api_gfx_core_helper.h"
#include "shared/source/helpers/api_specific_config.h"
#include "shared/source/helpers/bindless_heaps_helper.h"
#include "shared/source/helpers/compiler_product_helper.h"
#include "shared/source/helpers/gfx_core_helper.h"
#include "shared/source/helpers/hw_info.h"
#include "shared/source/memory_manager/allocation_properties.h"
#include "shared/source/memory_manager/graphics_allocation.h"
#include "shared/source/memory_manager/memory_manager.h"
#include "shared/source/memory_manager/memory_operations_handler.h"
#include "shared/source/os_interface/os_interface.h"
#include "shared/source/os_interface/os_time.h"
#include "shared/source/os_interface/product_helper.h"
#include "shared/source/release_helper/release_helper.h"
#include "shared/source/utilities/software_tags_manager.h"

namespace NEO {

RootDeviceEnvironment::RootDeviceEnvironment(ExecutionEnvironment &executionEnvironment) : executionEnvironment(executionEnvironment) {
    hwInfo = std::make_unique<HardwareInfo>();

    if (DebugManager.flags.EnableSWTags.get()) {
        tagsManager = std::make_unique<SWTagsManager>();
    }
}

RootDeviceEnvironment::~RootDeviceEnvironment() = default;

void RootDeviceEnvironment::initAubCenter(bool localMemoryEnabled, const std::string &aubFileName, CommandStreamReceiverType csrType) {
    if (!aubCenter) {
        UNRECOVERABLE_IF(!getGmmHelper());
        aubCenter.reset(new AubCenter(*this, localMemoryEnabled, aubFileName, csrType));
    }
}

void RootDeviceEnvironment::initDebugger() {
    debugger = Debugger::create(*this);
}

void RootDeviceEnvironment::initDebuggerL0(Device *neoDevice) {
    if (this->debugger.get() != nullptr) {
        NEO::printDebugString(NEO::DebugManager.flags.PrintDebugMessages.get(), stderr,
                              "%s", "Source Level Debugger cannot be used with Environment Variable enabling program debugging.\n");
        UNRECOVERABLE_IF(this->debugger.get() != nullptr);
    }

    this->getMutableHardwareInfo()->capabilityTable.fusedEuEnabled = false;
    this->getMutableHardwareInfo()->capabilityTable.ftrRenderCompressedBuffers = false;
    this->getMutableHardwareInfo()->capabilityTable.ftrRenderCompressedImages = false;

    this->debugger = DebuggerL0::create(neoDevice);
}

const HardwareInfo *RootDeviceEnvironment::getHardwareInfo() const {
    return hwInfo.get();
}

HardwareInfo *RootDeviceEnvironment::getMutableHardwareInfo() const {
    return hwInfo.get();
}

void RootDeviceEnvironment::setHwInfoAndInitHelpers(const HardwareInfo *hwInfo) {
    *this->hwInfo = *hwInfo;
    initHelpers();
}

bool RootDeviceEnvironment::isFullRangeSvm() const {
    return hwInfo->capabilityTable.gpuAddressSpace >= maxNBitValue(47);
}

GmmHelper *RootDeviceEnvironment::getGmmHelper() const {
    return gmmHelper.get();
}
GmmClientContext *RootDeviceEnvironment::getGmmClientContext() const {
    return gmmHelper->getClientContext();
}

void RootDeviceEnvironment::prepareForCleanup() const {
    if (osInterface && osInterface->getDriverModel()) {
        osInterface->getDriverModel()->isDriverAvailable();
    }
}

bool RootDeviceEnvironment::initAilConfiguration() {
    auto ailConfiguration = AILConfiguration::get(hwInfo->platform.eProductFamily);

    if (ailConfiguration == nullptr) {
        return true;
    }

    auto result = ailConfiguration->initProcessExecutableName();
    if (result != true) {
        return false;
    }

    ailConfiguration->apply(hwInfo->capabilityTable);

    return true;
}

void RootDeviceEnvironment::initGmm() {
    if (!gmmHelper) {
        gmmHelper.reset(new GmmHelper(*this));
    }
}

void RootDeviceEnvironment::initOsTime() {
    if (!osTime) {
        osTime = OSTime::create(osInterface.get());
    }
}

BindlessHeapsHelper *RootDeviceEnvironment::getBindlessHeapsHelper() const {
    return bindlessHeapsHelper.get();
}

const ProductHelper &RootDeviceEnvironment::getProductHelper() const {
    return *productHelper;
}

void RootDeviceEnvironment::createBindlessHeapsHelper(MemoryManager *memoryManager, bool availableDevices, uint32_t rootDeviceIndex, DeviceBitfield deviceBitfield) {
    bindlessHeapsHelper = std::make_unique<BindlessHeapsHelper>(memoryManager, availableDevices, rootDeviceIndex, deviceBitfield);
}

CompilerInterface *RootDeviceEnvironment::getCompilerInterface() {
    if (this->compilerInterface.get() == nullptr) {
        std::lock_guard<std::mutex> autolock(this->mtx);
        if (this->compilerInterface.get() == nullptr) {
            auto cache = std::make_unique<CompilerCache>(getDefaultCompilerCacheConfig());
            this->compilerInterface.reset(CompilerInterface::createInstance(std::move(cache), ApiSpecificConfig::getApiType() == ApiSpecificConfig::ApiType::OCL));
        }
    }
    return this->compilerInterface.get();
}

void RootDeviceEnvironment::initHelpers() {
    initProductHelper();
    initGfxCoreHelper();
    initApiGfxCoreHelper();
    initCompilerProductHelper();
    initReleaseHelper();
}

void RootDeviceEnvironment::initGfxCoreHelper() {
    if (gfxCoreHelper == nullptr) {
        gfxCoreHelper = GfxCoreHelper::create(this->getHardwareInfo()->platform.eRenderCoreFamily);
    }
}

void RootDeviceEnvironment::initProductHelper() {
    if (productHelper == nullptr) {
        productHelper = ProductHelper::create(this->getHardwareInfo()->platform.eProductFamily);
    }
}
void RootDeviceEnvironment::initCompilerProductHelper() {
    if (compilerProductHelper == nullptr) {
        compilerProductHelper = CompilerProductHelper::create(this->getHardwareInfo()->platform.eProductFamily);
    }
}

void RootDeviceEnvironment::initReleaseHelper() {
    if (releaseHelper == nullptr) {
        releaseHelper = ReleaseHelper::create(this->getHardwareInfo()->ipVersion);
    }
}

ReleaseHelper *RootDeviceEnvironment::getReleaseHelper() const {
    return releaseHelper.get();
}

BuiltIns *RootDeviceEnvironment::getBuiltIns() {
    if (this->builtins.get() == nullptr) {
        std::lock_guard<std::mutex> autolock(this->mtx);
        if (this->builtins.get() == nullptr) {
            this->builtins = std::make_unique<BuiltIns>();
        }
    }
    return this->builtins.get();
}

void RootDeviceEnvironment::limitNumberOfCcs(uint32_t numberOfCcs) {

    hwInfo->gtSystemInfo.CCSInfo.NumberOfCCSEnabled = std::min(hwInfo->gtSystemInfo.CCSInfo.NumberOfCCSEnabled, numberOfCcs);
    limitedNumberOfCcs = true;
}

bool RootDeviceEnvironment::isNumberOfCcsLimited() const {
    return limitedNumberOfCcs;
}

void RootDeviceEnvironment::initDummyAllocation() {
    std::call_once(isDummyAllocationInitialized, [this]() {
        auto customDeleter = [this](GraphicsAllocation *dummyAllocation) {
            this->executionEnvironment.memoryManager->freeGraphicsMemory(dummyAllocation);
        };
        auto dummyBlitAllocation = this->executionEnvironment.memoryManager->allocateGraphicsMemoryWithProperties(
            *this->dummyBlitProperties.get());
        this->dummyAllocation = GraphicsAllocationUniquePtrType(dummyBlitAllocation, customDeleter);
    });
}

void RootDeviceEnvironment::setDummyBlitProperties(uint32_t rootDeviceIndex) {
    size_t size = 4 * 4096u;
    this->dummyBlitProperties = std::make_unique<AllocationProperties>(
        rootDeviceIndex,
        true,
        size,
        NEO::AllocationType::BUFFER,
        false,
        false,
        systemMemoryBitfield);
}

GraphicsAllocation *RootDeviceEnvironment::getDummyAllocation() const {
    return dummyAllocation.get();
}

AssertHandler *RootDeviceEnvironment::getAssertHandler(Device *neoDevice) {
    if (this->assertHandler.get() == nullptr) {
        std::lock_guard<std::mutex> autolock(this->mtx);
        if (this->assertHandler.get() == nullptr) {
            this->assertHandler = std::make_unique<AssertHandler>(neoDevice);
        }
    }
    return this->assertHandler.get();
}

template <typename HelperType>
HelperType &RootDeviceEnvironment::getHelper() const {
    if constexpr (std::is_same_v<HelperType, CompilerProductHelper>) {
        UNRECOVERABLE_IF(compilerProductHelper == nullptr);
        return *compilerProductHelper;
    } else if constexpr (std::is_same_v<HelperType, ProductHelper>) {
        UNRECOVERABLE_IF(productHelper == nullptr);
        return *productHelper;
    } else {
        static_assert(std::is_same_v<HelperType, GfxCoreHelper>, "Only CompilerProductHelper, ProductHelper and GfxCoreHelper are supported");
        UNRECOVERABLE_IF(gfxCoreHelper == nullptr);
        return *gfxCoreHelper;
    }
}

template ProductHelper &RootDeviceEnvironment::getHelper() const;
template CompilerProductHelper &RootDeviceEnvironment::getHelper() const;
template GfxCoreHelper &RootDeviceEnvironment::getHelper() const;

} // namespace NEO

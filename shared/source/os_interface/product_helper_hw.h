/*
 * Copyright (C) 2023 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 */

#pragma once
#include "shared/source/os_interface/product_helper.h"

namespace NEO {

template <PRODUCT_FAMILY gfxProduct>
class ProductHelperHw : public ProductHelper {
  public:
    static std::unique_ptr<ProductHelper> create() {
        auto productHelper = std::unique_ptr<ProductHelper>(new ProductHelperHw());

        return productHelper;
    }

    int configureHardwareCustom(HardwareInfo *hwInfo, OSInterface *osIface) const override;
    void adjustPlatformForProductFamily(HardwareInfo *hwInfo) override;
    void adjustSamplerState(void *sampler, const HardwareInfo &hwInfo) const override;
    void disableRcsExposure(HardwareInfo *hwInfo) const override;
    uint64_t getHostMemCapabilities(const HardwareInfo *hwInfo) const override;
    uint64_t getDeviceMemCapabilities() const override;
    uint64_t getSingleDeviceSharedMemCapabilities() const override;
    uint64_t getCrossDeviceSharedMemCapabilities() const override;
    uint64_t getSharedSystemMemCapabilities(const HardwareInfo *hwInfo) const override;
    void getKernelExtendedProperties(uint32_t *fp16, uint32_t *fp32, uint32_t *fp64) const override;
    std::vector<int32_t> getKernelSupportedThreadArbitrationPolicies() const override;
    uint32_t getDeviceMemoryMaxClkRate(const HardwareInfo &hwInfo, const OSInterface *osIface, uint32_t subDeviceIndex) const override;
    uint64_t getDeviceMemoryPhysicalSizeInBytes(const OSInterface *osIface, uint32_t subDeviceIndex) const override;
    uint64_t getDeviceMemoryMaxBandWidthInBytesPerSecond(const HardwareInfo &hwInfo, const OSInterface *osIface, uint32_t subDeviceIndex) const override;
    bool isAdditionalStateBaseAddressWARequired(const HardwareInfo &hwInfo) const override;
    bool isMaxThreadsForWorkgroupWARequired(const HardwareInfo &hwInfo) const override;
    uint32_t getMaxThreadsForWorkgroupInDSSOrSS(const HardwareInfo &hwInfo, uint32_t maxNumEUsPerSubSlice, uint32_t maxNumEUsPerDualSubSlice) const override;
    uint32_t getMaxThreadsForWorkgroup(const HardwareInfo &hwInfo, uint32_t maxNumEUsPerSubSlice) const override;
    void setForceNonCoherent(void *const commandPtr, const StateComputeModeProperties &properties) const override;
    void updateScmCommand(void *const commandPtr, const StateComputeModeProperties &properties) const override;
    void updateIddCommand(void *const commandPtr, uint32_t numGrf, int32_t threadArbitrationPolicy) const override;
    bool obtainBlitterPreference(const HardwareInfo &hwInfo) const override;
    bool isBlitterFullySupported(const HardwareInfo &hwInfo) const override;
    bool isPageTableManagerSupported(const HardwareInfo &hwInfo) const override;
    bool overrideGfxPartitionLayoutForWsl() const override;
    uint32_t getHwIpVersion(const HardwareInfo &hwInfo) const override;
    uint32_t getHwRevIdFromStepping(uint32_t stepping, const HardwareInfo &hwInfo) const override;
    uint32_t getSteppingFromHwRevId(const HardwareInfo &hwInfo) const override;
    uint32_t getAubStreamSteppingFromHwRevId(const HardwareInfo &hwInfo) const override;
    std::optional<aub_stream::ProductFamily> getAubStreamProductFamily() const override;
    bool isDefaultEngineTypeAdjustmentRequired(const HardwareInfo &hwInfo) const override;
    std::string getDeviceMemoryName() const override;
    bool isDisableOverdispatchAvailable(const HardwareInfo &hwInfo) const override;
    bool allowCompression(const HardwareInfo &hwInfo) const override;
    LocalMemoryAccessMode getLocalMemoryAccessMode(const HardwareInfo &hwInfo) const override;
    bool isAllocationSizeAdjustmentRequired(const HardwareInfo &hwInfo) const override;
    int getProductMaxPreferredSlmSize(const HardwareInfo &hwInfo, int preferredEnumValue) const override;
    bool isNewResidencyModelSupported() const override;
    bool isDirectSubmissionSupported(const HardwareInfo &hwInfo) const override;
    std::pair<bool, bool> isPipeControlPriorToNonPipelinedStateCommandsWARequired(const HardwareInfo &hwInfo, bool isRcs, const ReleaseHelper *releaseHelper) const override;
    bool heapInLocalMem(const HardwareInfo &hwInfo) const override;
    void setCapabilityCoherencyFlag(const HardwareInfo &hwInfo, bool &coherencyFlag) override;
    bool isAdditionalMediaSamplerProgrammingRequired() const override;
    bool isInitialFlagsProgrammingRequired() const override;
    bool isReturnedCmdSizeForMediaSamplerAdjustmentRequired() const override;
    bool extraParametersInvalid(const HardwareInfo &hwInfo) const override;
    bool pipeControlWARequired(const HardwareInfo &hwInfo) const override;
    bool imagePitchAlignmentWARequired(const HardwareInfo &hwInfo) const override;
    bool isForceEmuInt32DivRemSPWARequired(const HardwareInfo &hwInfo) const override;
    bool is3DPipelineSelectWARequired() const override;
    bool isStorageInfoAdjustmentRequired() const override;
    bool isBlitterForImagesSupported() const override;
    bool isPageFaultSupported() const override;
    bool isKmdMigrationSupported() const override;
    bool isTile64With3DSurfaceOnBCSSupported(const HardwareInfo &hwInfo) const override;
    bool isDcFlushAllowed() const override;
    uint32_t computeMaxNeededSubSliceSpace(const HardwareInfo &hwInfo) const override;
    bool getUuid(Device *device, std::array<uint8_t, ProductHelper::uuidSize> &uuid) const override;
    bool isFlushTaskAllowed() const override;
    bool programAllStateComputeCommandFields() const override;
    bool isSystolicModeConfigurable(const HardwareInfo &hwInfo) const override;
    bool isInitBuiltinAsyncSupported(const HardwareInfo &hwInfo) const override;
    bool isComputeDispatchAllWalkerEnableInComputeWalkerRequired(const HardwareInfo &hwInfo) const override;
    bool isCopyEngineSelectorEnabled(const HardwareInfo &hwInfo) const override;
    bool isGlobalFenceInCommandStreamRequired(const HardwareInfo &hwInfo) const override;
    bool isGlobalFenceInDirectSubmissionRequired(const HardwareInfo &hwInfo) const override;
    bool isAdjustProgrammableIdPreferredSlmSizeRequired(const HardwareInfo &hwInfo) const override;
    uint32_t getThreadEuRatioForScratch(const HardwareInfo &hwInfo) const override;
    size_t getSvmCpuAlignment() const override;
    bool isComputeDispatchAllWalkerEnableInCfeStateRequired(const HardwareInfo &hwInfo) const override;
    bool isVmBindPatIndexProgrammingSupported() const override;
    bool isIpSamplingSupported(const HardwareInfo &hwInfo) const override;
    bool isGrfNumReportedWithScm() const override;
    bool isThreadArbitrationPolicyReportedWithScm() const override;
    bool isCooperativeEngineSupported(const HardwareInfo &hwInfo) const override;
    bool isTimestampWaitSupportedForEvents() const override;
    bool isTilePlacementResourceWaRequired(const HardwareInfo &hwInfo) const override;
    bool isBlitSplitEnqueueWARequired(const HardwareInfo &hwInfo) const override;
    bool isInitDeviceWithFirstSubmissionRequired(const HardwareInfo &hwInfo) const override;
    bool allowMemoryPrefetch(const HardwareInfo &hwInfo) const override;
    bool isBcsReportWaRequired(const HardwareInfo &hwInfo) const override;
    bool isBlitCopyRequiredForLocalMemory(const RootDeviceEnvironment &rootDeviceEnvironment, const GraphicsAllocation &allocation) const override;
    bool isImplicitScalingSupported(const HardwareInfo &hwInfo) const override;
    bool isCpuCopyNecessary(const void *ptr, MemoryManager *memoryManager) const override;
    bool isUnlockingLockedPtrNecessary(const HardwareInfo &hwInfo) const override;
    bool isAdjustWalkOrderAvailable(const ReleaseHelper *releaseHelper) const override;
    bool isAssignEngineRoundRobinSupported() const override;
    uint32_t getL1CachePolicy(bool isDebuggerActive) const override;
    bool isEvictionIfNecessaryFlagSupported() const override;
    void adjustNumberOfCcs(HardwareInfo &hwInfo) const override;
    bool isPrefetcherDisablingInDirectSubmissionRequired() const override;
    bool isStatefulAddressingModeSupported() const override;
    uint32_t getNumberOfPartsInTileForConcurrentKernel() const override;
    bool isPlatformQuerySupported() const override;
    bool isNonBlockingGpuSubmissionSupported() const override;
    bool isResolveDependenciesByPipeControlsSupported(const HardwareInfo &hwInfo, bool isOOQ, TaskCountType queueTaskCount, const CommandStreamReceiver &queueCsr) const override;
    bool isMidThreadPreemptionDisallowedForRayTracingKernels() const override;
    bool isBufferPoolAllocatorSupported() const override;
    uint64_t overridePatIndex(AllocationType allocationType, uint64_t patIndex) const override;
    bool isTlbFlushRequired() const override;
    bool isDummyBlitWaRequired() const override;
    bool isDetectIndirectAccessInKernelSupported(const KernelDescriptor &kernelDescriptor) const override;
    bool isLinearStoragePreferred(bool isSharedContext, bool isImage1d, bool forceLinearStorage) const override;
    bool isTranslationExceptionSupported() const override;
    uint32_t getMaxNumSamplers() const override;

    bool getFrontEndPropertyScratchSizeSupport() const override;
    bool getFrontEndPropertyPrivateScratchSizeSupport() const override;
    bool getFrontEndPropertyComputeDispatchAllWalkerSupport() const override;
    bool getFrontEndPropertyDisableEuFusionSupport() const override;
    bool getFrontEndPropertyDisableOverDispatchSupport() const override;
    bool getFrontEndPropertySingleSliceDispatchCcsModeSupport() const override;

    bool getScmPropertyThreadArbitrationPolicySupport() const override;
    bool getScmPropertyCoherencyRequiredSupport() const override;
    bool getScmPropertyZPassAsyncComputeThreadLimitSupport() const override;
    bool getScmPropertyPixelAsyncComputeThreadLimitSupport() const override;
    bool getScmPropertyLargeGrfModeSupport() const override;
    bool getScmPropertyDevicePreemptionModeSupport() const override;

    bool getStateBaseAddressPropertyGlobalAtomicsSupport() const override;
    bool getStateBaseAddressPropertyBindingTablePoolBaseAddressSupport() const override;

    bool getPreemptionDbgPropertyPreemptionModeSupport() const override;
    bool getPreemptionDbgPropertyStateSipSupport() const override;
    bool getPreemptionDbgPropertyCsrSurfaceSupport() const override;

    bool getPipelineSelectPropertyMediaSamplerDopClockGateSupport() const override;
    bool getPipelineSelectPropertySystolicModeSupport() const override;

    void fillScmPropertiesSupportStructure(StateComputeModePropertiesSupport &propertiesSupport) const override;
    void fillScmPropertiesSupportStructureExtra(StateComputeModePropertiesSupport &propertiesSupport, const RootDeviceEnvironment &rootDeviceEnvironment) const override;
    void fillFrontEndPropertiesSupportStructure(FrontEndPropertiesSupport &propertiesSupport, const HardwareInfo &hwInfo) const override;
    void fillPipelineSelectPropertiesSupportStructure(PipelineSelectPropertiesSupport &propertiesSupport, const HardwareInfo &hwInfo) const override;
    void fillStateBaseAddressPropertiesSupportStructure(StateBaseAddressPropertiesSupport &propertiesSupport) const override;
    uint32_t getDefaultRevisionId() const override;

    bool isFusedEuDisabledForDpas(bool kernelHasDpasInstructions, const uint32_t *lws, const uint32_t *groupCount, const HardwareInfo &hwInfo) const override;
    bool isCalculationForDisablingEuFusionWithDpasNeeded(const HardwareInfo &hwInfo) const override;
    bool is48bResourceNeededForRayTracing() const override;

    ~ProductHelperHw() override = default;

  protected:
    ProductHelperHw() = default;

    void enableCompression(HardwareInfo *hwInfo) const;
    void enableBlitterOperationsSupport(HardwareInfo *hwInfo) const;
    bool getConcurrentAccessMemCapabilitiesSupported(UsmAccessCapabilities capability) const;
    uint32_t getProductConfigFromHwInfo(const HardwareInfo &hwInfo) const override;
    uint64_t getHostMemCapabilitiesValue() const;
    bool getHostMemCapabilitiesSupported(const HardwareInfo *hwInfo) const;
    LocalMemoryAccessMode getDefaultLocalMemoryAccessMode(const HardwareInfo &hwInfo) const override;
    void fillScmPropertiesSupportStructureBase(StateComputeModePropertiesSupport &propertiesSupport) const override;
};

template <PRODUCT_FAMILY gfxProduct>
struct EnableProductHelper {
    EnableProductHelper() {
        auto productHelperCreateFunction = ProductHelperHw<gfxProduct>::create;
        productHelperFactory[gfxProduct] = productHelperCreateFunction;
    }
};

} // namespace NEO

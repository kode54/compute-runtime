/*
 * Copyright (C) 2021-2023 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 */

#include "shared/source/command_stream/stream_property.h"

namespace NEO {
enum PreemptionMode : uint32_t;
struct HardwareInfo;
struct RootDeviceEnvironment;

struct StateComputeModePropertiesSupport {
    bool coherencyRequired = false;
    bool largeGrfMode = false;
    bool zPassAsyncComputeThreadLimit = false;
    bool pixelAsyncComputeThreadLimit = false;
    bool threadArbitrationPolicy = false;
    bool devicePreemptionMode = false;
};

struct StateComputeModeProperties {
    StreamProperty isCoherencyRequired{};
    StreamProperty largeGrfMode{};
    StreamProperty zPassAsyncComputeThreadLimit{};
    StreamProperty pixelAsyncComputeThreadLimit{};
    StreamProperty threadArbitrationPolicy{};
    StreamProperty devicePreemptionMode{};

    void initSupport(const RootDeviceEnvironment &rootDeviceEnvironment);
    void resetState();

    void setPropertiesAll(bool requiresCoherency, uint32_t numGrfRequired, int32_t threadArbitrationPolicy, PreemptionMode devicePreemptionMode);
    void setPropertiesGrfNumberThreadArbitration(uint32_t numGrfRequired, int32_t threadArbitrationPolicy);
    void setPropertiesCoherencyDevicePreemption(bool requiresCoherency, PreemptionMode devicePreemptionMode, bool clearDirtyState);

    void copyPropertiesAll(const StateComputeModeProperties &properties);
    void copyPropertiesGrfNumberThreadArbitration(const StateComputeModeProperties &properties);

    bool isDirty() const;
    void clearIsDirty();

  protected:
    void clearIsDirtyExtraPerContext();
    void clearIsDirtyExtraPerKernel();
    bool isDirtyExtra() const;
    void resetStateExtra();

    void setPropertiesExtraPerContext();
    void setPropertiesExtraPerKernel();

    void copyPropertiesExtra(const StateComputeModeProperties &properties);

    void setCoherencyProperty(bool requiresCoherency);
    void setDevicePreemptionProperty(PreemptionMode devicePreemptionMode);
    void setGrfNumberProperty(uint32_t numGrfRequired);
    void setThreadArbitrationProperty(int32_t threadArbitrationPolicy);

    StateComputeModePropertiesSupport scmPropertiesSupport = {};
    int32_t defaultThreadArbitrationPolicy = 0;
    bool propertiesSupportLoaded = false;
};

struct FrontEndPropertiesSupport {
    bool computeDispatchAllWalker = false;
    bool disableEuFusion = false;
    bool disableOverdispatch = false;
    bool singleSliceDispatchCcsMode = false;
};

struct FrontEndProperties {
    StreamProperty computeDispatchAllWalkerEnable{};
    StreamProperty disableEUFusion{};
    StreamProperty disableOverdispatch{};
    StreamProperty singleSliceDispatchCcsMode{};

    void initSupport(const RootDeviceEnvironment &rootDeviceEnvironment);
    void resetState();

    void setPropertiesAll(bool isCooperativeKernel, bool disableEuFusion, bool disableOverdispatch, int32_t engineInstancedDevice);
    void setPropertySingleSliceDispatchCcsMode(int32_t engineInstancedDevice);
    void setPropertiesDisableOverdispatchEngineInstanced(bool disableOverdispatch, int32_t engineInstancedDevice, bool clearDirtyState);
    void setPropertiesComputeDispatchAllWalkerEnableDisableEuFusion(bool isCooperativeKernel, bool disableEuFusion);

    void copyPropertiesAll(const FrontEndProperties &properties);
    void copyPropertiesComputeDispatchAllWalkerEnableDisableEuFusion(const FrontEndProperties &properties);

    bool isDirty() const;
    void clearIsDirty();

  protected:
    FrontEndPropertiesSupport frontEndPropertiesSupport = {};
    bool propertiesSupportLoaded = false;
};

struct PipelineSelectPropertiesSupport {
    bool mediaSamplerDopClockGate = false;
    bool systolicMode = false;
};

struct PipelineSelectProperties {
    StreamProperty modeSelected{};
    StreamProperty mediaSamplerDopClockGate{};
    StreamProperty systolicMode{};

    void initSupport(const RootDeviceEnvironment &rootDeviceEnvironment);
    void resetState();

    void setPropertiesAll(bool modeSelected, bool mediaSamplerDopClockGate, bool systolicMode);
    void setPropertiesModeSelectedMediaSamplerClockGate(bool modeSelected, bool mediaSamplerDopClockGate, bool clearDirtyState);
    void setPropertySystolicMode(bool systolicMode);

    void copyPropertiesAll(const PipelineSelectProperties &properties);
    void copyPropertiesSystolicMode(const PipelineSelectProperties &properties);

    bool isDirty() const;
    void clearIsDirty();

  protected:
    PipelineSelectPropertiesSupport pipelineSelectPropertiesSupport = {};
    bool propertiesSupportLoaded = false;
};

struct StateBaseAddressPropertiesSupport {
    bool globalAtomics = false;
    bool bindingTablePoolBaseAddress = false;
};

struct StateBaseAddressProperties {
    StreamProperty64 bindingTablePoolBaseAddress{};
    StreamProperty64 surfaceStateBaseAddress{};
    StreamProperty64 dynamicStateBaseAddress{};
    StreamProperty64 indirectObjectBaseAddress{};
    StreamPropertySizeT bindingTablePoolSize{};
    StreamPropertySizeT surfaceStateSize{};
    StreamPropertySizeT dynamicStateSize{};
    StreamPropertySizeT indirectObjectSize{};
    StreamProperty globalAtomics{};
    StreamProperty statelessMocs{};

    void initSupport(const RootDeviceEnvironment &rootDeviceEnvironment);
    void resetState();

    void setPropertiesAll(bool globalAtomics, int32_t statelessMocs,
                          int64_t bindingTablePoolBaseAddress, size_t bindingTablePoolSize,
                          int64_t surfaceStateBaseAddress, size_t surfaceStateSize,
                          int64_t dynamicStateBaseAddress, size_t dynamicStateSize,
                          int64_t indirectObjectBaseAddress, size_t indirectObjectSize);
    void setPropertiesBindingTableSurfaceState(int64_t bindingTablePoolBaseAddress, size_t bindingTablePoolSize,
                                               int64_t surfaceStateBaseAddress, size_t surfaceStateSize);
    void setPropertiesSurfaceState(int64_t surfaceStateBaseAddress, size_t surfaceStateSize);
    void setPropertiesDynamicState(int64_t dynamicStateBaseAddress, size_t dynamicStateSize);
    void setPropertiesIndirectState(int64_t indirectObjectBaseAddress, size_t indirectObjectSize);
    void setPropertyStatelessMocs(int32_t statelessMocs);
    void setPropertyGlobalAtomics(bool globalAtomics, bool clearDirtyState);

    void copyPropertiesAll(const StateBaseAddressProperties &properties);
    void copyPropertiesStatelessMocs(const StateBaseAddressProperties &properties);
    void copyPropertiesStatelessMocsIndirectState(const StateBaseAddressProperties &properties);

    bool isDirty() const;
    void clearIsDirty();

  protected:
    StateBaseAddressPropertiesSupport stateBaseAddressPropertiesSupport = {};
    bool propertiesSupportLoaded = false;
};

} // namespace NEO

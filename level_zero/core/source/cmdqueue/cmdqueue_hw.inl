/*
 * Copyright (C) 2020-2023 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 */

#pragma once

#include "shared/source/built_ins/built_ins.h"
#include "shared/source/built_ins/sip.h"
#include "shared/source/command_container/command_encoder.h"
#include "shared/source/command_stream/command_stream_receiver_hw.h"
#include "shared/source/command_stream/linear_stream.h"
#include "shared/source/command_stream/preemption.h"
#include "shared/source/command_stream/preemption_mode.h"
#include "shared/source/command_stream/scratch_space_controller.h"
#include "shared/source/command_stream/wait_status.h"
#include "shared/source/debugger/debugger_l0.h"
#include "shared/source/device/device.h"
#include "shared/source/execution_environment/root_device_environment.h"
#include "shared/source/helpers/definitions/command_encoder_args.h"
#include "shared/source/helpers/gfx_core_helper.h"
#include "shared/source/helpers/heap_base_address_model.h"
#include "shared/source/helpers/logical_state_helper.h"
#include "shared/source/helpers/pause_on_gpu_properties.h"
#include "shared/source/helpers/pipe_control_args.h"
#include "shared/source/helpers/preamble.h"
#include "shared/source/memory_manager/graphics_allocation.h"
#include "shared/source/memory_manager/memory_manager.h"
#include "shared/source/memory_manager/residency_container.h"
#include "shared/source/memory_manager/unified_memory_manager.h"
#include "shared/source/os_interface/os_context.h"
#include "shared/source/unified_memory/unified_memory.h"
#include "shared/source/utilities/software_tags_manager.h"

#include "level_zero/core/source/cmdlist/cmdlist.h"
#include "level_zero/core/source/cmdlist/cmdlist_hw.h"
#include "level_zero/core/source/cmdqueue/cmdqueue_hw.h"
#include "level_zero/core/source/device/device.h"
#include "level_zero/core/source/driver/driver_handle_imp.h"
#include "level_zero/core/source/fence/fence.h"
#include "level_zero/core/source/helpers/error_code_helper_l0.h"

#include <algorithm>
#include <limits>

namespace L0 {
template <GFXCORE_FAMILY gfxCoreFamily>
ze_result_t CommandQueueHw<gfxCoreFamily>::createFence(const ze_fence_desc_t *desc,
                                                       ze_fence_handle_t *phFence) {
    *phFence = Fence::create(this, desc);
    return ZE_RESULT_SUCCESS;
}

template <GFXCORE_FAMILY gfxCoreFamily>
ze_result_t CommandQueueHw<gfxCoreFamily>::executeCommandLists(
    uint32_t numCommandLists,
    ze_command_list_handle_t *phCommandLists,
    ze_fence_handle_t hFence,
    bool performMigration) {

    auto ret = validateCommandListsParams(phCommandLists, numCommandLists);
    if (ret != ZE_RESULT_SUCCESS) {
        return ret;
    }

    auto lockCSR = this->csr->obtainUniqueOwnership();

    if (NEO::DebugManager.flags.ForceMemoryPrefetchForKmdMigratedSharedAllocations.get()) {
        auto svmAllocMgr = device->getDriverHandle()->getSvmAllocsManager();
        svmAllocMgr->prefetchSVMAllocs(*device->getNEODevice(), *csr);
    }

    if (this->clientId == CommandQueue::clientNotRegistered) {
        this->clientId = this->csr->registerClient();
    }

    auto neoDevice = device->getNEODevice();
    auto ctx = CommandListExecutionContext{phCommandLists,
                                           numCommandLists,
                                           this->isCopyOnlyCommandQueue ? NEO::PreemptionMode::Disabled : csr->getPreemptionMode(),
                                           device,
                                           NEO::Debugger::isDebugEnabled(internalUsage),
                                           csr->isProgramActivePartitionConfigRequired(),
                                           performMigration};
    ctx.globalInit |= ctx.isDebugEnabled && !this->commandQueueDebugCmdsProgrammed && (neoDevice->getSourceLevelDebugger() || device->getL0Debugger());

    this->startingCmdBuffer = &this->commandStream;
    this->device->activateMetricGroups();

    if (this->isCopyOnlyCommandQueue) {
        ret = this->executeCommandListsCopyOnly(ctx, numCommandLists, phCommandLists, hFence);
    } else {
        ret = this->executeCommandListsRegular(ctx, numCommandLists, phCommandLists, hFence);
    }

    if (NEO::DebugManager.flags.PauseOnEnqueue.get() != -1) {
        neoDevice->debugExecutionCounter++;
    }

    return ret;
}

template <GFXCORE_FAMILY gfxCoreFamily>
ze_result_t CommandQueueHw<gfxCoreFamily>::executeCommandListsRegular(
    CommandListExecutionContext &ctx,
    uint32_t numCommandLists,
    ze_command_list_handle_t *commandListHandles,
    ze_fence_handle_t hFence) {

    this->setupCmdListsAndContextParams(ctx, commandListHandles, numCommandLists, hFence);
    ctx.isDirectSubmissionEnabled = this->csr->isDirectSubmissionEnabled();

    std::unique_lock<std::mutex> lockForIndirect;
    if (ctx.hasIndirectAccess) {
        handleIndirectAllocationResidency(ctx.unifiedMemoryControls, lockForIndirect, ctx.isMigrationRequested);
    }

    size_t linearStreamSizeEstimate = this->estimateLinearStreamSizeInitial(ctx);

    this->handleScratchSpaceAndUpdateGSBAStateDirtyFlag(ctx);
    this->setFrontEndStateProperties(ctx);

    linearStreamSizeEstimate += this->estimateLinearStreamSizeComplementary(ctx, commandListHandles, numCommandLists);
    linearStreamSizeEstimate += this->computeDebuggerCmdsSize(ctx);

    auto neoDevice = this->device->getNEODevice();

    if (ctx.isDispatchTaskCountPostSyncRequired) {
        linearStreamSizeEstimate += NEO::MemorySynchronizationCommands<GfxFamily>::getSizeForBarrierWithPostSyncOperation(neoDevice->getRootDeviceEnvironment(), false);
    }

    this->csr->getResidencyAllocations().reserve(ctx.spaceForResidency);

    NEO::LinearStream child(nullptr);
    if (const auto ret = this->makeAlignedChildStreamAndSetGpuBase(child, linearStreamSizeEstimate); ret != ZE_RESULT_SUCCESS) {
        return ret;
    }

    this->getGlobalFenceAndMakeItResident();
    this->getWorkPartitionAndMakeItResident();
    this->getGlobalStatelessHeapAndMakeItResident();
    this->makePreemptionAllocationResidentForModeMidThread(ctx.isDevicePreemptionModeMidThread);
    this->makeSipIsaResidentIfSipKernelUsed(ctx);
    this->makeDebugSurfaceResidentIfNEODebuggerActive(ctx.isNEODebuggerActive(this->device));
    this->makeRayTracingBufferResident(neoDevice->getRTMemoryBackedBuffer());
    this->makeSbaTrackingBufferResidentIfL0DebuggerEnabled(ctx.isDebugEnabled);
    this->makeCsrTagAllocationResident();
    this->encodeKernelArgsBufferAndMakeItResident();

    if (ctx.globalInit) {
        this->getTagsManagerHeapsAndMakeThemResidentIfSWTagsEnabled(child);
        this->csr->programHardwareContext(child);

        if (!this->pipelineSelectStateTracking) {
            this->programPipelineSelectIfGpgpuDisabled(child);
        } else {
            // Setting systolic/pipeline select here for 1st command list is to preserve dispatch order of hw commands
            if (this->stateChanges.size() > 0) {
                auto &firstCmdListWithStateChange = this->stateChanges[0];
                // check first required state change is for the first command list
                if (firstCmdListWithStateChange.cmdListIndex == 0 && firstCmdListWithStateChange.flags.propertyPsDirty) {
                    this->programOneCmdListPipelineSelect(child, firstCmdListWithStateChange);
                    firstCmdListWithStateChange.flags.propertyPsDirty = false;
                }
            }
        }
        this->programCommandQueueDebugCmdsForSourceLevelOrL0DebuggerIfEnabled(ctx.isDebugEnabled, child);
        if (!this->stateBaseAddressTracking) {
            this->programStateBaseAddressWithGsbaIfDirty(ctx, ctx.firstCommandList, child);
        }
        this->programCsrBaseAddressIfPreemptionModeInitial(ctx.isPreemptionModeInitial, child);
        this->programStateSip(ctx.stateSipRequired, child);
        this->programActivePartitionConfig(ctx.isProgramActivePartitionConfigRequired, child);
        bool shouldProgramVfe = (this->csr->getLogicalStateHelper() || !frontEndTrackingEnabled()) && ctx.frontEndStateDirty;
        this->programFrontEndAndClearDirtyFlag(shouldProgramVfe, ctx, child, csr->getStreamProperties());

        if (ctx.rtDispatchRequired) {
            auto csrHw = static_cast<NEO::CommandStreamReceiverHw<GfxFamily> *>(this->csr);
            csrHw->dispatchRayTracingStateCommand(child, *neoDevice);
        }
    }

    this->writeCsrStreamInlineIfLogicalStateHelperAvailable(child);

    for (auto i = 0u; i < numCommandLists; ++i) {
        auto commandList = CommandList::fromHandle(commandListHandles[i]);

        ctx.childGpuAddressPositionBeforeDynamicPreamble = child.getCurrentGpuAddressPosition();

        if (this->stateChanges.size() > this->currentStateChangeIndex) {
            auto &stateChange = this->stateChanges[this->currentStateChangeIndex];
            if (stateChange.cmdListIndex == i) {
                DEBUG_BREAK_IF(commandList != stateChange.commandList);
                this->updateOneCmdListPreemptionModeAndCtxStatePreemption(child, stateChange);
                this->programOneCmdListPipelineSelect(child, stateChange);
                this->programOneCmdListFrontEndIfDirty(ctx, child, stateChange);
                this->programRequiredStateComputeModeForCommandList(child, stateChange);
                this->programRequiredStateBaseAddressForCommandList(ctx, child, stateChange);

                this->currentStateChangeIndex++;
            }
        }

        this->patchCommands(*commandList, this->csr->getScratchSpaceController()->getScratchPatchAddress());
        this->programOneCmdListBatchBufferStart(commandList, child, ctx);
        this->mergeOneCmdListPipelinedState(commandList);

        this->prefetchMemoryToDeviceAssociatedWithCmdList(commandList);
        if (commandList->hasKernelWithAssert()) {
            cmdListWithAssertExecuted.exchange(true);
        }

        this->collectPrintfContentsFromCommandsList(commandList);
    }

    this->updateBaseAddressState(ctx.lastCommandList);
    this->migrateSharedAllocationsIfRequested(ctx.isMigrationRequested, ctx.firstCommandList);

    this->programLastCommandListReturnBbStart(child, ctx);
    this->programStateSipEndWA(ctx.stateSipRequired, child);
    this->assignCsrTaskCountToFenceIfAvailable(hFence);
    this->dispatchTaskCountPostSyncRegular(ctx.isDispatchTaskCountPostSyncRequired, child);
    auto submitResult = this->prepareAndSubmitBatchBuffer(ctx, child);

    this->csr->setPreemptionMode(ctx.statePreemption);
    this->updateTaskCountAndPostSync(ctx.isDispatchTaskCountPostSyncRequired);

    this->csr->makeSurfacePackNonResident(this->csr->getResidencyAllocations(), false);

    auto completionResult = this->waitForCommandQueueCompletionAndCleanHeapContainer();
    ze_result_t retVal = this->handleSubmissionAndCompletionResults(submitResult, completionResult);

    this->csr->getResidencyAllocations().clear();
    this->stateChanges.clear();
    this->currentStateChangeIndex = 0;
    return retVal;
}

template <GFXCORE_FAMILY gfxCoreFamily>
ze_result_t CommandQueueHw<gfxCoreFamily>::executeCommandListsCopyOnly(
    CommandListExecutionContext &ctx,
    uint32_t numCommandLists,
    ze_command_list_handle_t *phCommandLists,
    ze_fence_handle_t hFence) {

    this->setupCmdListsAndContextParams(ctx, phCommandLists, numCommandLists, hFence);
    ctx.isDirectSubmissionEnabled = this->csr->isBlitterDirectSubmissionEnabled();

    size_t linearStreamSizeEstimate = this->estimateLinearStreamSizeInitial(ctx);
    for (auto i = 0u; i < numCommandLists; i++) {
        auto commandList = CommandList::fromHandle(phCommandLists[i]);
        linearStreamSizeEstimate += estimateCommandListSecondaryStart(commandList);
        ctx.spaceForResidency += estimateCommandListResidencySize(commandList);
    }

    linearStreamSizeEstimate += this->estimateCommandListPrimaryStart(ctx.globalInit);

    this->csr->getResidencyAllocations().reserve(ctx.spaceForResidency);

    NEO::EncodeDummyBlitWaArgs waArgs{false, &(this->device->getNEODevice()->getRootDeviceEnvironmentRef())};
    linearStreamSizeEstimate += NEO::EncodeMiFlushDW<GfxFamily>::getCommandSizeWithWa(waArgs);

    NEO::LinearStream child(nullptr);
    if (const auto ret = this->makeAlignedChildStreamAndSetGpuBase(child, linearStreamSizeEstimate); ret != ZE_RESULT_SUCCESS) {
        return ret;
    }

    this->getGlobalFenceAndMakeItResident();
    this->getTagsManagerHeapsAndMakeThemResidentIfSWTagsEnabled(child);
    this->csr->programHardwareContext(child);

    this->encodeKernelArgsBufferAndMakeItResident();

    this->writeCsrStreamInlineIfLogicalStateHelperAvailable(child);

    for (auto i = 0u; i < numCommandLists; ++i) {
        auto commandList = CommandList::fromHandle(phCommandLists[i]);
        ctx.childGpuAddressPositionBeforeDynamicPreamble = child.getCurrentGpuAddressPosition();

        this->programOneCmdListBatchBufferStart(commandList, child, ctx);
        this->mergeOneCmdListPipelinedState(commandList);
        this->prefetchMemoryToDeviceAssociatedWithCmdList(commandList);
    }
    this->migrateSharedAllocationsIfRequested(ctx.isMigrationRequested, ctx.firstCommandList);

    this->assignCsrTaskCountToFenceIfAvailable(hFence);

    this->programLastCommandListReturnBbStart(child, ctx);
    this->dispatchTaskCountPostSyncByMiFlushDw(ctx.isDispatchTaskCountPostSyncRequired, child);

    this->makeCsrTagAllocationResident();
    auto submitResult = this->prepareAndSubmitBatchBuffer(ctx, child);
    this->updateTaskCountAndPostSync(ctx.isDispatchTaskCountPostSyncRequired);
    this->csr->makeSurfacePackNonResident(this->csr->getResidencyAllocations(), false);

    auto completionResult = this->waitForCommandQueueCompletionAndCleanHeapContainer();
    ze_result_t retVal = this->handleSubmissionAndCompletionResults(submitResult, completionResult);

    this->csr->getResidencyAllocations().clear();

    return retVal;
}

template <GFXCORE_FAMILY gfxCoreFamily>
ze_result_t CommandQueueHw<gfxCoreFamily>::validateCommandListsParams(
    ze_command_list_handle_t *phCommandLists,
    uint32_t numCommandLists) {

    bool anyCommandListWithCooperativeKernels = false;
    bool anyCommandListWithoutCooperativeKernels = false;

    for (auto i = 0u; i < numCommandLists; i++) {
        auto commandList = CommandList::fromHandle(phCommandLists[i]);
        if (this->peekIsCopyOnlyCommandQueue() != commandList->isCopyOnly()) {
            return ZE_RESULT_ERROR_INVALID_COMMAND_LIST_TYPE;
        }

        if (this->activeSubDevices < commandList->getPartitionCount()) {
            return ZE_RESULT_ERROR_INVALID_COMMAND_LIST_TYPE;
        }

        if (commandList->containsCooperativeKernels()) {
            anyCommandListWithCooperativeKernels = true;
        } else {
            anyCommandListWithoutCooperativeKernels = true;
        }
    }

    if (anyCommandListWithCooperativeKernels &&
        anyCommandListWithoutCooperativeKernels &&
        (!NEO::DebugManager.flags.AllowMixingRegularAndCooperativeKernels.get())) {
        return ZE_RESULT_ERROR_INVALID_COMMAND_LIST_TYPE;
    }

    return ZE_RESULT_SUCCESS;
}

template <GFXCORE_FAMILY gfxCoreFamily>
void CommandQueueHw<gfxCoreFamily>::programOneCmdListFrontEndIfDirty(
    CommandListExecutionContext &ctx,
    NEO::LinearStream &cmdStream,
    CommandListRequiredStateChange &cmdListRequired) {

    if (!frontEndTrackingEnabled()) {
        return;
    }

    if (cmdListRequired.flags.propertyFeDirty) {
        this->programFrontEndAndClearDirtyFlag(cmdListRequired.flags.propertyFeDirty, ctx, cmdStream, cmdListRequired.requiredState);
    }

    if (cmdListRequired.flags.frontEndReturnPoint) {
        ctx.cmdListBeginState.frontEndState.resetState();
        ctx.cmdListBeginState.frontEndState.copyPropertiesAll(cmdListRequired.requiredState.frontEndState);
    }
}

template <GFXCORE_FAMILY gfxCoreFamily>
void CommandQueueHw<gfxCoreFamily>::programFrontEndAndClearDirtyFlag(
    bool shouldFrontEndBeProgrammed,
    CommandListExecutionContext &ctx,
    NEO::LinearStream &cmdStream,
    NEO::StreamProperties &csrState) {

    if (!shouldFrontEndBeProgrammed) {
        return;
    }
    auto scratchSpaceController = this->csr->getScratchSpaceController();
    programFrontEnd(scratchSpaceController->getScratchPatchAddress(),
                    scratchSpaceController->getPerThreadScratchSpaceSize(),
                    cmdStream,
                    csrState);
    ctx.frontEndStateDirty = false;
}

template <GFXCORE_FAMILY gfxCoreFamily>
void CommandQueueHw<gfxCoreFamily>::programFrontEnd(uint64_t scratchAddress, uint32_t perThreadScratchSpaceSize, NEO::LinearStream &cmdStream, NEO::StreamProperties &streamProperties) {
    UNRECOVERABLE_IF(csr == nullptr);
    auto &hwInfo = device->getHwInfo();
    auto &gfxCoreHelper = device->getGfxCoreHelper();
    auto engineGroupType = gfxCoreHelper.getEngineGroupType(csr->getOsContext().getEngineType(),
                                                            csr->getOsContext().getEngineUsage(), hwInfo);
    auto pVfeState = NEO::PreambleHelper<GfxFamily>::getSpaceForVfeState(&cmdStream, hwInfo, engineGroupType);
    NEO::PreambleHelper<GfxFamily>::programVfeState(pVfeState,
                                                    device->getNEODevice()->getRootDeviceEnvironment(),
                                                    perThreadScratchSpaceSize,
                                                    scratchAddress,
                                                    device->getMaxNumHwThreads(),
                                                    streamProperties,
                                                    csr->getLogicalStateHelper());
    csr->setMediaVFEStateDirty(false);
}

template <GFXCORE_FAMILY gfxCoreFamily>
size_t CommandQueueHw<gfxCoreFamily>::estimateFrontEndCmdSize() {
    return NEO::PreambleHelper<GfxFamily>::getVFECommandsSize();
}

template <GFXCORE_FAMILY gfxCoreFamily>
size_t CommandQueueHw<gfxCoreFamily>::estimateFrontEndCmdSize(bool isFrontEndDirty) {
    if (!frontEndTrackingEnabled()) {
        return isFrontEndDirty * estimateFrontEndCmdSize();
    }
    return 0;
}

template <GFXCORE_FAMILY gfxCoreFamily>
size_t CommandQueueHw<gfxCoreFamily>::estimateFrontEndCmdSizeForMultipleCommandLists(
    bool &isFrontEndStateDirty, int32_t engineInstanced, CommandList *commandList,
    NEO::StreamProperties &csrState,
    const NEO::StreamProperties &cmdListRequired,
    const NEO::StreamProperties &cmdListFinal,
    NEO::StreamProperties &requiredState,
    bool &propertyDirty,
    bool &frontEndReturnPoint) {

    if (!frontEndTrackingEnabled()) {
        return 0;
    }

    auto singleFrontEndCmdSize = estimateFrontEndCmdSize();
    size_t estimatedSize = 0;
    bool feCurrentDirty = isFrontEndStateDirty;

    if (isFrontEndStateDirty) {
        csrState.frontEndState.copyPropertiesAll(cmdListRequired.frontEndState);
        csrState.frontEndState.setPropertySingleSliceDispatchCcsMode(engineInstanced);
    } else {
        csrState.frontEndState.copyPropertiesComputeDispatchAllWalkerEnableDisableEuFusion(cmdListRequired.frontEndState);
        feCurrentDirty = csrState.frontEndState.isDirty();
    }

    if (feCurrentDirty) {
        estimatedSize += singleFrontEndCmdSize;

        propertyDirty = true;
    }

    uint32_t frontEndChanges = commandList->getReturnPointsSize();
    if (frontEndChanges > 0) {
        estimatedSize += (frontEndChanges * singleFrontEndCmdSize);
        estimatedSize += (frontEndChanges * NEO::EncodeBatchBufferStartOrEnd<GfxFamily>::getBatchBufferStartSize());
        frontEndReturnPoint = true;
    }

    if (frontEndReturnPoint || propertyDirty) {
        requiredState.frontEndState = csrState.frontEndState;
    }

    if (isFrontEndStateDirty) {
        csrState.frontEndState.copyPropertiesAll(cmdListFinal.frontEndState);
        isFrontEndStateDirty = false;
    } else {
        csrState.frontEndState.copyPropertiesComputeDispatchAllWalkerEnableDisableEuFusion(cmdListFinal.frontEndState);
    }

    return estimatedSize;
}

template <GFXCORE_FAMILY gfxCoreFamily>
void CommandQueueHw<gfxCoreFamily>::programPipelineSelectIfGpgpuDisabled(NEO::LinearStream &cmdStream) {
    bool gpgpuEnabled = this->csr->getPreambleSetFlag();
    if (!gpgpuEnabled) {
        NEO::PipelineSelectArgs args = {false, false, false, false};
        NEO::PreambleHelper<GfxFamily>::programPipelineSelect(&cmdStream, args, device->getNEODevice()->getRootDeviceEnvironment());
        this->csr->setPreambleSetFlag(true);
    }
}

template <GFXCORE_FAMILY gfxCoreFamily>
bool CommandQueueHw<gfxCoreFamily>::isDispatchTaskCountPostSyncRequired(ze_fence_handle_t hFence, bool containsAnyRegularCmdList) const {
    return containsAnyRegularCmdList || !csr->isUpdateTagFromWaitEnabled() || hFence != nullptr || getSynchronousMode() == ZE_COMMAND_QUEUE_MODE_SYNCHRONOUS;
}

template <GFXCORE_FAMILY gfxCoreFamily>
bool CommandQueueHw<gfxCoreFamily>::getPreemptionCmdProgramming() {
    return NEO::PreemptionHelper::getRequiredCmdStreamSize<GfxFamily>(NEO::PreemptionMode::MidThread, NEO::PreemptionMode::Initial) > 0u;
}

template <GFXCORE_FAMILY gfxCoreFamily>
CommandQueueHw<gfxCoreFamily>::CommandListExecutionContext::CommandListExecutionContext(
    ze_command_list_handle_t *commandListHandles,
    uint32_t numCommandLists,
    NEO::PreemptionMode contextPreemptionMode,
    Device *device,
    bool debugEnabled,
    bool programActivePartitionConfig,
    bool performMigration) : preemptionMode{contextPreemptionMode},
                             statePreemption{contextPreemptionMode},
                             isPreemptionModeInitial{contextPreemptionMode == NEO::PreemptionMode::Initial},
                             isDebugEnabled{debugEnabled},
                             isProgramActivePartitionConfigRequired{programActivePartitionConfig},
                             isMigrationRequested{performMigration} {

    constexpr size_t residencyContainerSpaceForPreemption = 2;
    constexpr size_t residencyContainerSpaceForTagWrite = 1;
    constexpr size_t residencyContainerSpaceForBtdAllocation = 1;

    this->firstCommandList = CommandList::fromHandle(commandListHandles[0]);
    this->lastCommandList = CommandList::fromHandle(commandListHandles[numCommandLists - 1]);

    this->isDevicePreemptionModeMidThread = device->getDevicePreemptionMode() == NEO::PreemptionMode::MidThread;
    this->stateSipRequired = (this->isPreemptionModeInitial && this->isDevicePreemptionModeMidThread) ||
                             this->isNEODebuggerActive(device);

    if (this->isDevicePreemptionModeMidThread) {
        this->spaceForResidency += residencyContainerSpaceForPreemption;
    }
    this->spaceForResidency += residencyContainerSpaceForTagWrite;
    if (device->getNEODevice()->getRTMemoryBackedBuffer()) {
        this->spaceForResidency += residencyContainerSpaceForBtdAllocation;
    }

    if (this->isMigrationRequested && device->getDriverHandle()->getMemoryManager()->getPageFaultManager() == nullptr) {
        this->isMigrationRequested = false;
    }

    this->globalInit |= (this->isProgramActivePartitionConfigRequired || this->isPreemptionModeInitial || this->stateSipRequired);
}

template <GFXCORE_FAMILY gfxCoreFamily>
bool CommandQueueHw<gfxCoreFamily>::CommandListExecutionContext::isNEODebuggerActive(Device *device) {
    return device->getNEODevice()->getDebugger() && this->isDebugEnabled;
}

template <GFXCORE_FAMILY gfxCoreFamily>
size_t CommandQueueHw<gfxCoreFamily>::computeDebuggerCmdsSize(const CommandListExecutionContext &ctx) {
    size_t debuggerCmdsSize = 0;

    if (ctx.isDebugEnabled && !this->commandQueueDebugCmdsProgrammed) {
        if (this->device->getNEODevice()->getSourceLevelDebugger()) {
            debuggerCmdsSize += NEO::PreambleHelper<GfxFamily>::getKernelDebuggingCommandsSize(true);
        } else if (this->device->getL0Debugger()) {
            debuggerCmdsSize += device->getL0Debugger()->getSbaAddressLoadCommandsSize();
        }
    }

    return debuggerCmdsSize;
}

template <GFXCORE_FAMILY gfxCoreFamily>
size_t CommandQueueHw<gfxCoreFamily>::computePreemptionSizeForCommandList(
    CommandListExecutionContext &ctx,
    CommandList *commandList,
    bool &dirtyState) {

    size_t preemptionSize = 0u;

    auto commandListPreemption = commandList->getCommandListPreemptionMode();

    if (ctx.statePreemption != commandListPreemption) {
        if (this->preemptionCmdSyncProgramming) {
            preemptionSize += NEO::MemorySynchronizationCommands<GfxFamily>::getSizeForSingleBarrier(false);
        }
        preemptionSize += NEO::PreemptionHelper::getRequiredCmdStreamSize<GfxFamily>(commandListPreemption, ctx.statePreemption);
        ctx.statePreemption = commandListPreemption;
        dirtyState = true;
    }

    return preemptionSize;
}

template <GFXCORE_FAMILY gfxCoreFamily>
void CommandQueueHw<gfxCoreFamily>::setupCmdListsAndContextParams(
    CommandListExecutionContext &ctx,
    ze_command_list_handle_t *phCommandLists,
    uint32_t numCommandLists,
    ze_fence_handle_t hFence) {

    ctx.containsAnyRegularCmdList = ctx.firstCommandList->getCmdListType() == CommandList::CommandListType::TYPE_REGULAR;

    for (auto i = 0u; i < numCommandLists; i++) {
        auto commandList = CommandList::fromHandle(phCommandLists[i]);
        commandList->setCsr(this->csr);

        auto &commandContainer = commandList->getCmdContainer();

        if (!isCopyOnlyCommandQueue) {
            ctx.perThreadScratchSpaceSize = std::max(ctx.perThreadScratchSpaceSize, commandList->getCommandListPerThreadScratchSize());
            ctx.perThreadPrivateScratchSize = std::max(ctx.perThreadPrivateScratchSize, commandList->getCommandListPerThreadPrivateScratchSize());

            if (commandList->getCmdListHeapAddressModel() == NEO::HeapAddressModel::PrivateHeaps) {
                if (commandList->getCommandListPerThreadScratchSize() != 0 || commandList->getCommandListPerThreadPrivateScratchSize() != 0) {
                    if (commandContainer.getIndirectHeap(NEO::HeapType::SURFACE_STATE) != nullptr) {
                        heapContainer.push_back(commandContainer.getIndirectHeap(NEO::HeapType::SURFACE_STATE)->getGraphicsAllocation());
                    }
                    for (auto &element : commandContainer.getSshAllocations()) {
                        heapContainer.push_back(element);
                    }
                }
            }

            if (commandList->containsCooperativeKernels()) {
                ctx.anyCommandListWithCooperativeKernels = true;
            } else {
                ctx.anyCommandListWithoutCooperativeKernels = true;
            }

            if (commandList->getRequiredStreamState().frontEndState.disableEUFusion.value == 1) {
                ctx.anyCommandListRequiresDisabledEUFusion = true;
            }

            // If the Command List has commands that require uncached MOCS, then any changes to the commands in the queue requires the uncached MOCS
            if (commandList->isRequiredQueueUncachedMocs() && ctx.cachedMOCSAllowed == true) {
                ctx.cachedMOCSAllowed = false;
            }

            ctx.hasIndirectAccess |= commandList->hasIndirectAllocationsAllowed();
            if (commandList->hasIndirectAllocationsAllowed()) {
                ctx.unifiedMemoryControls.indirectDeviceAllocationsAllowed |= commandList->getUnifiedMemoryControls().indirectDeviceAllocationsAllowed;
                ctx.unifiedMemoryControls.indirectHostAllocationsAllowed |= commandList->getUnifiedMemoryControls().indirectHostAllocationsAllowed;
                ctx.unifiedMemoryControls.indirectSharedAllocationsAllowed |= commandList->getUnifiedMemoryControls().indirectSharedAllocationsAllowed;
            }

            this->partitionCount = std::max(this->partitionCount, commandList->getPartitionCount());
        }

        makeResidentAndMigrate(ctx.isMigrationRequested, commandContainer.getResidencyContainer());
    }

    ctx.isDispatchTaskCountPostSyncRequired = isDispatchTaskCountPostSyncRequired(hFence, ctx.containsAnyRegularCmdList);
}

template <GFXCORE_FAMILY gfxCoreFamily>
size_t CommandQueueHw<gfxCoreFamily>::estimateLinearStreamSizeInitial(
    CommandListExecutionContext &ctx) {

    using MI_BATCH_BUFFER_START = typename GfxFamily::MI_BATCH_BUFFER_START;
    using MI_BATCH_BUFFER_END = typename GfxFamily::MI_BATCH_BUFFER_END;

    size_t linearStreamSizeEstimate = 0u;

    auto hwContextSizeEstimate = this->csr->getCmdsSizeForHardwareContext();
    if (hwContextSizeEstimate > 0) {
        linearStreamSizeEstimate += hwContextSizeEstimate;
        ctx.globalInit = true;
    }

    if (ctx.isDirectSubmissionEnabled) {
        linearStreamSizeEstimate += NEO::EncodeBatchBufferStartOrEnd<GfxFamily>::getBatchBufferStartSize();
        if (NEO::DebugManager.flags.DirectSubmissionRelaxedOrdering.get() == 1) {
            linearStreamSizeEstimate += 2 * sizeof(typename GfxFamily::MI_LOAD_REGISTER_REG);
        }
    } else {
        linearStreamSizeEstimate += NEO::EncodeBatchBufferStartOrEnd<GfxFamily>::getBatchBufferEndSize();
    }

    auto csrHw = reinterpret_cast<NEO::CommandStreamReceiverHw<GfxFamily> *>(this->csr);
    if (ctx.isProgramActivePartitionConfigRequired) {
        linearStreamSizeEstimate += csrHw->getCmdSizeForActivePartitionConfig();
    }

    if (NEO::DebugManager.flags.EnableSWTags.get()) {
        linearStreamSizeEstimate += NEO::SWTagsManager::estimateSpaceForSWTags<GfxFamily>();
        ctx.globalInit = true;
    }

    linearStreamSizeEstimate += NEO::EncodeKernelArgsBuffer<GfxFamily>::getKernelArgsBufferCmdsSize(this->csr->getKernelArgsBufferAllocation(),
                                                                                                    this->csr->getLogicalStateHelper());

    return linearStreamSizeEstimate;
}

template <GFXCORE_FAMILY gfxCoreFamily>
size_t CommandQueueHw<gfxCoreFamily>::estimateCommandListSecondaryStart(CommandList *commandList) {
    if (!this->dispatchCmdListBatchBufferAsPrimary) {
        return (commandList->getCmdContainer().getCmdBufferAllocations().size() * NEO::EncodeBatchBufferStartOrEnd<GfxFamily>::getBatchBufferStartSize());
    }
    return 0;
}

template <GFXCORE_FAMILY gfxCoreFamily>
size_t CommandQueueHw<gfxCoreFamily>::estimateCommandListPrimaryStart(bool required) {
    if (this->dispatchCmdListBatchBufferAsPrimary && required) {
        return NEO::EncodeBatchBufferStartOrEnd<GfxFamily>::getBatchBufferStartSize();
    }
    return 0;
}

template <GFXCORE_FAMILY gfxCoreFamily>
size_t CommandQueueHw<gfxCoreFamily>::estimateCommandListResidencySize(CommandList *commandList) {
    return commandList->getCmdContainer().getResidencyContainer().size();
}

template <GFXCORE_FAMILY gfxCoreFamily>
void CommandQueueHw<gfxCoreFamily>::setFrontEndStateProperties(CommandListExecutionContext &ctx) {

    auto isEngineInstanced = csr->getOsContext().isEngineInstanced();
    auto &streamProperties = this->csr->getStreamProperties();
    if (!frontEndTrackingEnabled()) {
        streamProperties.frontEndState.setPropertiesAll(ctx.anyCommandListWithCooperativeKernels, ctx.anyCommandListRequiresDisabledEUFusion,
                                                        true, isEngineInstanced);
        ctx.frontEndStateDirty |= (streamProperties.frontEndState.isDirty() && !this->csr->getLogicalStateHelper());
    } else {
        ctx.engineInstanced = isEngineInstanced;
    }
    ctx.frontEndStateDirty |= csr->getMediaVFEStateDirty();
    ctx.globalInit |= ctx.frontEndStateDirty;
}

template <GFXCORE_FAMILY gfxCoreFamily>
void CommandQueueHw<gfxCoreFamily>::handleScratchSpaceAndUpdateGSBAStateDirtyFlag(CommandListExecutionContext &ctx) {
    auto scratchController = this->csr->getScratchSpaceController();
    handleScratchSpace(this->heapContainer,
                       scratchController,
                       ctx.gsbaStateDirty, ctx.frontEndStateDirty,
                       ctx.perThreadScratchSpaceSize, ctx.perThreadPrivateScratchSize);
    ctx.gsbaStateDirty |= this->csr->getGSBAStateDirty();
    ctx.scratchGsba = scratchController->calculateNewGSH();

    ctx.globalInit |= ctx.gsbaStateDirty;
}

template <GFXCORE_FAMILY gfxCoreFamily>
size_t CommandQueueHw<gfxCoreFamily>::estimateLinearStreamSizeComplementary(
    CommandListExecutionContext &ctx,
    ze_command_list_handle_t *phCommandLists,
    uint32_t numCommandLists) {

    size_t linearStreamSizeEstimate = 0u;

    linearStreamSizeEstimate += estimateFrontEndCmdSize(ctx.frontEndStateDirty);
    linearStreamSizeEstimate += estimatePipelineSelectCmdSize();

    auto &streamProperties = this->csr->getStreamProperties();
    bool frontEndStateDirty = ctx.frontEndStateDirty;
    bool gpgpuEnabled = this->csr->getPreambleSetFlag();
    bool baseAdresStateDirty = ctx.gsbaStateDirty;
    bool scmStateDirty = this->csr->getStateComputeModeDirty();

    ctx.globalInit |= !gpgpuEnabled;
    ctx.globalInit |= scmStateDirty;

    CommandListRequiredStateChange cmdListState;

    for (uint32_t i = 0; i < numCommandLists; i++) {
        auto cmdList = CommandList::fromHandle(phCommandLists[i]);
        auto &requiredStreamState = cmdList->getRequiredStreamState();
        auto &finalStreamState = cmdList->getFinalStreamState();

        linearStreamSizeEstimate += estimateFrontEndCmdSizeForMultipleCommandLists(frontEndStateDirty, ctx.engineInstanced, cmdList,
                                                                                   streamProperties, requiredStreamState, finalStreamState,
                                                                                   cmdListState.requiredState,
                                                                                   cmdListState.flags.propertyFeDirty, cmdListState.flags.frontEndReturnPoint);
        linearStreamSizeEstimate += estimatePipelineSelectCmdSizeForMultipleCommandLists(streamProperties, requiredStreamState, finalStreamState, gpgpuEnabled,
                                                                                         cmdListState.requiredState, cmdListState.flags.propertyPsDirty);
        linearStreamSizeEstimate += estimateScmCmdSizeForMultipleCommandLists(streamProperties, scmStateDirty, requiredStreamState, finalStreamState,
                                                                              cmdListState.requiredState, cmdListState.flags.propertyScmDirty);
        linearStreamSizeEstimate += estimateStateBaseAddressCmdSizeForMultipleCommandLists(baseAdresStateDirty, cmdList->getCmdListHeapAddressModel(), streamProperties, requiredStreamState, finalStreamState,
                                                                                           cmdListState.requiredState, cmdListState.flags.propertySbaDirty);
        linearStreamSizeEstimate += computePreemptionSizeForCommandList(ctx, cmdList, cmdListState.flags.preemptionDirty);

        linearStreamSizeEstimate += estimateCommandListSecondaryStart(cmdList);
        ctx.spaceForResidency += estimateCommandListResidencySize(cmdList);

        if (cmdListState.flags.isAnyDirty()) {
            cmdListState.commandList = cmdList;
            cmdListState.cmdListIndex = i;
            cmdListState.newPreemptionMode = ctx.statePreemption;
            this->stateChanges.push_back(cmdListState);

            linearStreamSizeEstimate += this->estimateCommandListPrimaryStart(true);

            cmdListState.requiredState.resetState();
            cmdListState.flags.cleanDirty();
        }
    }

    if (ctx.gsbaStateDirty && !this->stateBaseAddressTracking) {
        linearStreamSizeEstimate += estimateStateBaseAddressCmdSize();
    }

    if (csr->isRayTracingStateProgramingNeeded(*device->getNEODevice())) {
        ctx.rtDispatchRequired = true;
        auto csrHw = static_cast<NEO::CommandStreamReceiverHw<GfxFamily> *>(this->csr);
        linearStreamSizeEstimate += csrHw->getCmdSizeForPerDssBackedBuffer(this->device->getHwInfo());

        ctx.globalInit = true;
    }

    NEO::Device *neoDevice = this->device->getNEODevice();
    if (ctx.isPreemptionModeInitial) {
        linearStreamSizeEstimate += NEO::PreemptionHelper::getRequiredPreambleSize<GfxFamily>(*neoDevice);
    }
    if (ctx.stateSipRequired) {
        linearStreamSizeEstimate += NEO::PreemptionHelper::getRequiredStateSipCmdSize<GfxFamily>(*neoDevice, this->csr->isRcs());
    }

    bool firstCmdlistDynamicPreamble = (this->stateChanges.size() > 0 && this->stateChanges[0].cmdListIndex == 0);
    bool estimateBbStartForGlobalInitOnly = !firstCmdlistDynamicPreamble && ctx.globalInit;
    linearStreamSizeEstimate += this->estimateCommandListPrimaryStart(estimateBbStartForGlobalInitOnly);

    return linearStreamSizeEstimate;
}

template <GFXCORE_FAMILY gfxCoreFamily>
ze_result_t CommandQueueHw<gfxCoreFamily>::makeAlignedChildStreamAndSetGpuBase(NEO::LinearStream &child, size_t requiredSize) {

    size_t alignedSize = alignUp<size_t>(requiredSize, this->minCmdBufferPtrAlign);

    if (const auto waitStatus = this->reserveLinearStreamSize(alignedSize); waitStatus == NEO::WaitStatus::GpuHang) {
        return ZE_RESULT_ERROR_DEVICE_LOST;
    }

    child.replaceBuffer(this->commandStream.getSpace(alignedSize), alignedSize);
    child.setGpuBase(ptrOffset(this->commandStream.getGpuBase(), this->commandStream.getUsed() - alignedSize));
    this->alignedChildStreamPadding = alignedSize - requiredSize;
    return ZE_RESULT_SUCCESS;
}

template <GFXCORE_FAMILY gfxCoreFamily>
void CommandQueueHw<gfxCoreFamily>::getGlobalFenceAndMakeItResident() {
    const auto globalFenceAllocation = this->csr->getGlobalFenceAllocation();
    if (globalFenceAllocation) {
        this->csr->makeResident(*globalFenceAllocation);
    }
}

template <GFXCORE_FAMILY gfxCoreFamily>
void CommandQueueHw<gfxCoreFamily>::getWorkPartitionAndMakeItResident() {
    const auto workPartitionAllocation = this->csr->getWorkPartitionAllocation();
    if (workPartitionAllocation) {
        this->csr->makeResident(*workPartitionAllocation);
    }
}

template <GFXCORE_FAMILY gfxCoreFamily>
void CommandQueueHw<gfxCoreFamily>::getGlobalStatelessHeapAndMakeItResident() {
    const auto globalStatelessAllocation = this->csr->getGlobalStatelessHeapAllocation();
    if (globalStatelessAllocation) {
        this->csr->makeResident(*globalStatelessAllocation);
    }
}

template <GFXCORE_FAMILY gfxCoreFamily>
void CommandQueueHw<gfxCoreFamily>::getTagsManagerHeapsAndMakeThemResidentIfSWTagsEnabled(NEO::LinearStream &cmdStream) {
    if (NEO::DebugManager.flags.EnableSWTags.get()) {
        NEO::Device *neoDevice = this->device->getNEODevice();
        NEO::SWTagsManager *tagsManager = neoDevice->getRootDeviceEnvironment().tagsManager.get();
        UNRECOVERABLE_IF(tagsManager == nullptr);
        this->csr->makeResident(*tagsManager->getBXMLHeapAllocation());
        this->csr->makeResident(*tagsManager->getSWTagHeapAllocation());
        tagsManager->insertBXMLHeapAddress<GfxFamily>(cmdStream);
        tagsManager->insertSWTagHeapAddress<GfxFamily>(cmdStream);
    }
}

template <GFXCORE_FAMILY gfxCoreFamily>
void CommandQueueHw<gfxCoreFamily>::makeSbaTrackingBufferResidentIfL0DebuggerEnabled(bool isDebugEnabled) {
    if (isDebugEnabled && this->device->getL0Debugger()) {
        this->csr->makeResident(*this->device->getL0Debugger()->getSbaTrackingBuffer(this->csr->getOsContext().getContextId()));
    }
}

template <GFXCORE_FAMILY gfxCoreFamily>
void CommandQueueHw<gfxCoreFamily>::programCommandQueueDebugCmdsForSourceLevelOrL0DebuggerIfEnabled(bool isDebugEnabled, NEO::LinearStream &cmdStream) {
    if (isDebugEnabled && !this->commandQueueDebugCmdsProgrammed) {
        NEO::Device *neoDevice = device->getNEODevice();
        if (neoDevice->getSourceLevelDebugger()) {
            NEO::PreambleHelper<GfxFamily>::programKernelDebugging(&cmdStream);
            this->commandQueueDebugCmdsProgrammed = true;
        } else if (this->device->getL0Debugger()) {
            this->device->getL0Debugger()->programSbaAddressLoad(cmdStream,
                                                                 device->getL0Debugger()->getSbaTrackingBuffer(csr->getOsContext().getContextId())->getGpuAddress());
            this->commandQueueDebugCmdsProgrammed = true;
        }
    }
}

template <GFXCORE_FAMILY gfxCoreFamily>
void CommandQueueHw<gfxCoreFamily>::programStateBaseAddressWithGsbaIfDirty(
    CommandListExecutionContext &ctx,
    ze_command_list_handle_t hCommandList,
    NEO::LinearStream &cmdStream) {

    if (!ctx.gsbaStateDirty) {
        return;
    }
    auto indirectHeap = CommandList::fromHandle(hCommandList)->getCmdContainer().getIndirectHeap(NEO::HeapType::INDIRECT_OBJECT);
    programStateBaseAddress(ctx.scratchGsba,
                            indirectHeap->getGraphicsAllocation()->isAllocatedInLocalMemoryPool(),
                            cmdStream,
                            ctx.cachedMOCSAllowed,
                            nullptr);
}

template <GFXCORE_FAMILY gfxCoreFamily>
void CommandQueueHw<gfxCoreFamily>::programCsrBaseAddressIfPreemptionModeInitial(bool isPreemptionModeInitial, NEO::LinearStream &cmdStream) {
    if (!isPreemptionModeInitial) {
        return;
    }
    NEO::Device *neoDevice = this->device->getNEODevice();
    NEO::PreemptionHelper::programCsrBaseAddress<GfxFamily>(cmdStream,
                                                            *neoDevice,
                                                            this->csr->getPreemptionAllocation(),
                                                            this->csr->getLogicalStateHelper());
}

template <GFXCORE_FAMILY gfxCoreFamily>
void CommandQueueHw<gfxCoreFamily>::programStateSip(bool isStateSipRequired, NEO::LinearStream &cmdStream) {
    if (!isStateSipRequired) {
        return;
    }
    NEO::Device *neoDevice = this->device->getNEODevice();
    NEO::PreemptionHelper::programStateSip<GfxFamily>(cmdStream, *neoDevice, this->csr->getLogicalStateHelper(), &this->csr->getOsContext());
}

template <GFXCORE_FAMILY gfxCoreFamily>
void CommandQueueHw<gfxCoreFamily>::programStateSipEndWA(bool isStateSipRequired, NEO::LinearStream &cmdStream) {
    if (!isStateSipRequired) {
        return;
    }
    NEO::Device *neoDevice = this->device->getNEODevice();
    NEO::PreemptionHelper::programStateSipEndWa<GfxFamily>(cmdStream, neoDevice->getRootDeviceEnvironment());
}

template <GFXCORE_FAMILY gfxCoreFamily>
void CommandQueueHw<gfxCoreFamily>::updateOneCmdListPreemptionModeAndCtxStatePreemption(
    NEO::LinearStream &cmdStream,
    CommandListRequiredStateChange &cmdListRequired) {

    if (cmdListRequired.flags.preemptionDirty) {
        if (NEO::DebugManager.flags.EnableSWTags.get()) {
            NEO::Device *neoDevice = this->device->getNEODevice();
            neoDevice->getRootDeviceEnvironment().tagsManager->insertTag<GfxFamily, NEO::SWTags::PipeControlReasonTag>(
                cmdStream,
                *neoDevice,
                "ComandList Preemption Mode update", 0u);
        }
        if (this->preemptionCmdSyncProgramming) {
            NEO::PipeControlArgs args;
            NEO::MemorySynchronizationCommands<GfxFamily>::addSingleBarrier(cmdStream, args);
        }
        NEO::PreemptionHelper::programCmdStream<GfxFamily>(cmdStream,
                                                           cmdListRequired.newPreemptionMode,
                                                           NEO::PreemptionMode::Initial,
                                                           this->csr->getPreemptionAllocation());
    }
}

template <GFXCORE_FAMILY gfxCoreFamily>
void CommandQueueHw<gfxCoreFamily>::makePreemptionAllocationResidentForModeMidThread(bool isDevicePreemptionModeMidThread) {
    if (isDevicePreemptionModeMidThread) {
        this->csr->makeResident(*this->csr->getPreemptionAllocation());
    }
}

template <GFXCORE_FAMILY gfxCoreFamily>
void CommandQueueHw<gfxCoreFamily>::makeSipIsaResidentIfSipKernelUsed(CommandListExecutionContext &ctx) {
    NEO::Device *neoDevice = this->device->getNEODevice();
    if (ctx.isDevicePreemptionModeMidThread || ctx.isNEODebuggerActive(this->device)) {
        auto sipIsa = NEO::SipKernel::getSipKernel(*neoDevice).getSipAllocation();
        this->csr->makeResident(*sipIsa);
    }
}

template <GFXCORE_FAMILY gfxCoreFamily>
void CommandQueueHw<gfxCoreFamily>::makeDebugSurfaceResidentIfNEODebuggerActive(bool isNEODebuggerActive) {
    if (!isNEODebuggerActive) {
        return;
    }
    UNRECOVERABLE_IF(this->device->getDebugSurface() == nullptr);
    this->csr->makeResident(*this->device->getDebugSurface());
}

template <GFXCORE_FAMILY gfxCoreFamily>
void CommandQueueHw<gfxCoreFamily>::programActivePartitionConfig(
    bool isProgramActivePartitionConfigRequired,
    NEO::LinearStream &cmdStream) {

    if (!isProgramActivePartitionConfigRequired) {
        return;
    }
    auto csrHw = reinterpret_cast<NEO::CommandStreamReceiverHw<GfxFamily> *>(this->csr);
    csrHw->programActivePartitionConfig(cmdStream);
}

template <GFXCORE_FAMILY gfxCoreFamily>
void CommandQueueHw<gfxCoreFamily>::encodeKernelArgsBufferAndMakeItResident() {
    NEO::EncodeKernelArgsBuffer<GfxFamily>::encodeKernelArgsBufferCmds(this->csr->getKernelArgsBufferAllocation(),
                                                                       this->csr->getLogicalStateHelper());
    if (this->csr->getKernelArgsBufferAllocation()) {
        this->csr->makeResident(*this->csr->getKernelArgsBufferAllocation());
    }
}

template <GFXCORE_FAMILY gfxCoreFamily>
void CommandQueueHw<gfxCoreFamily>::writeCsrStreamInlineIfLogicalStateHelperAvailable(NEO::LinearStream &cmdStream) {
    if (this->csr->getLogicalStateHelper()) {
        this->csr->getLogicalStateHelper()->writeStreamInline(cmdStream, false);
    }
}

template <GFXCORE_FAMILY gfxCoreFamily>
void CommandQueueHw<gfxCoreFamily>::programOneCmdListBatchBufferStart(CommandList *commandList, NEO::LinearStream &commandStream, CommandListExecutionContext &ctx) {
    if (this->dispatchCmdListBatchBufferAsPrimary) {
        programOneCmdListBatchBufferStartPrimaryBatchBuffer(commandList, commandStream, ctx);
    } else {
        programOneCmdListBatchBufferStartSecondaryBatchBuffer(commandList, commandStream, ctx);
    }
}

template <GFXCORE_FAMILY gfxCoreFamily>
void CommandQueueHw<gfxCoreFamily>::programOneCmdListBatchBufferStartPrimaryBatchBuffer(CommandList *commandList, NEO::LinearStream &commandStream, CommandListExecutionContext &ctx) {
    using MI_BATCH_BUFFER_START = typename GfxFamily::MI_BATCH_BUFFER_START;

    NEO::CommandContainer &cmdListContainer = commandList->getCmdContainer();
    NEO::GraphicsAllocation *cmdListFirstCmdBuffer = cmdListContainer.getCmdBufferAllocations()[0];
    auto bbStartPatchLocation = reinterpret_cast<MI_BATCH_BUFFER_START *>(ctx.currentPatchForChainedBbStart);

    bool dynamicPreamble = ctx.childGpuAddressPositionBeforeDynamicPreamble != commandStream.getCurrentGpuAddressPosition();
    if (ctx.globalInit || dynamicPreamble) {
        if (ctx.currentPatchForChainedBbStart) {
            // dynamic preamble, 2nd or later command list
            // jump from previous command list to the position before dynamic preamble
            NEO::EncodeBatchBufferStartOrEnd<GfxFamily>::programBatchBufferStart(
                bbStartPatchLocation,
                ctx.childGpuAddressPositionBeforeDynamicPreamble,
                false, false, false);
        }
        // dynamic preamble, jump from current position, after dynamic preamble to the current command list
        NEO::EncodeBatchBufferStartOrEnd<GfxFamily>::programBatchBufferStart(&commandStream, cmdListFirstCmdBuffer->getGpuAddress(), false, false, false);

        ctx.globalInit = false;
    } else {
        if (ctx.currentPatchForChainedBbStart == nullptr) {
            // nothing to dispatch from queue, first command list will be used as submitting batch buffer to KMD or ULLS
            size_t firstCmdBufferAlignedSize = cmdListContainer.getAlignedPrimarySize();
            this->firstCmdListStream.replaceGraphicsAllocation(cmdListFirstCmdBuffer);
            this->firstCmdListStream.replaceBuffer(cmdListFirstCmdBuffer->getUnderlyingBuffer(), firstCmdBufferAlignedSize);
            this->firstCmdListStream.getSpace(firstCmdBufferAlignedSize);
            this->startingCmdBuffer = &this->firstCmdListStream;
        } else {
            // chain between command lists when no dynamic preamble required between 2nd and next command list
            NEO::EncodeBatchBufferStartOrEnd<GfxFamily>::programBatchBufferStart(
                bbStartPatchLocation,
                cmdListFirstCmdBuffer->getGpuAddress(),
                false, false, false);
        }
    }

    ctx.currentPatchForChainedBbStart = cmdListContainer.getEndCmdPtr();
}

template <GFXCORE_FAMILY gfxCoreFamily>
void CommandQueueHw<gfxCoreFamily>::programOneCmdListBatchBufferStartSecondaryBatchBuffer(CommandList *commandList, NEO::LinearStream &commandStream, CommandListExecutionContext &ctx) {
    auto &commandContainer = commandList->getCmdContainer();

    auto &cmdBufferAllocations = commandContainer.getCmdBufferAllocations();
    auto cmdBufferCount = cmdBufferAllocations.size();
    bool isCommandListImmediate = !ctx.containsAnyRegularCmdList;

    auto &returnPoints = commandList->getReturnPoints();
    uint32_t returnPointsSize = commandList->getReturnPointsSize();
    uint32_t returnPointIdx = 0;

    for (size_t iter = 0; iter < cmdBufferCount; iter++) {
        auto allocation = cmdBufferAllocations[iter];
        uint64_t startOffset = allocation->getGpuAddress();
        if (isCommandListImmediate && (iter == (cmdBufferCount - 1))) {
            startOffset = ptrOffset(allocation->getGpuAddress(), commandContainer.currentLinearStreamStartOffsetRef());
        }
        NEO::EncodeBatchBufferStartOrEnd<GfxFamily>::programBatchBufferStart(&commandStream, startOffset, true, false, false);
        if (returnPointsSize > 0) {
            bool cmdBufferHasRestarts = std::find_if(
                                            std::next(returnPoints.begin(), returnPointIdx),
                                            returnPoints.end(),
                                            [allocation](CmdListReturnPoint &retPt) {
                                                return retPt.currentCmdBuffer == allocation;
                                            }) != returnPoints.end();
            if (cmdBufferHasRestarts) {
                while (returnPointIdx < returnPointsSize && allocation == returnPoints[returnPointIdx].currentCmdBuffer) {
                    auto scratchSpaceController = this->csr->getScratchSpaceController();
                    ctx.cmdListBeginState.frontEndState.copyPropertiesComputeDispatchAllWalkerEnableDisableEuFusion(returnPoints[returnPointIdx].configSnapshot.frontEndState);
                    programFrontEnd(scratchSpaceController->getScratchPatchAddress(),
                                    scratchSpaceController->getPerThreadScratchSpaceSize(),
                                    commandStream,
                                    ctx.cmdListBeginState);
                    NEO::EncodeBatchBufferStartOrEnd<GfxFamily>::programBatchBufferStart(&commandStream,
                                                                                         returnPoints[returnPointIdx].gpuAddress,
                                                                                         true, false, false);
                    returnPointIdx++;
                }
            }
        }
    }
}

template <GFXCORE_FAMILY gfxCoreFamily>
void CommandQueueHw<gfxCoreFamily>::programLastCommandListReturnBbStart(
    NEO::LinearStream &commandStream,
    CommandListExecutionContext &ctx) {
    using MI_BATCH_BUFFER_START = typename GfxFamily::MI_BATCH_BUFFER_START;
    if (this->dispatchCmdListBatchBufferAsPrimary) {
        auto finalReturnPosition = commandStream.getCurrentGpuAddressPosition();
        auto bbStartCmd = reinterpret_cast<MI_BATCH_BUFFER_START *>(ctx.currentPatchForChainedBbStart);
        NEO::EncodeBatchBufferStartOrEnd<GfxFamily>::programBatchBufferStart(bbStartCmd,
                                                                             finalReturnPosition,
                                                                             false, false, false);
    }
}

template <GFXCORE_FAMILY gfxCoreFamily>
void CommandQueueHw<gfxCoreFamily>::mergeOneCmdListPipelinedState(CommandList *commandList) {

    bool isCommandListImmediate = (commandList->getCmdListType() == CommandList::CommandListType::TYPE_IMMEDIATE) ? true : false;
    auto commandListImp = static_cast<CommandListImp *>(commandList);
    if (!isCommandListImmediate && commandListImp->getLogicalStateHelper()) {
        this->csr->getLogicalStateHelper()->mergePipelinedState(*commandListImp->getLogicalStateHelper());
    }
}

template <GFXCORE_FAMILY gfxCoreFamily>
void CommandQueueHw<gfxCoreFamily>::collectPrintfContentsFromCommandsList(
    CommandList *commandList) {

    this->printfKernelContainer.insert(this->printfKernelContainer.end(),
                                       commandList->getPrintfKernelContainer().begin(),
                                       commandList->getPrintfKernelContainer().end());
}

template <GFXCORE_FAMILY gfxCoreFamily>
void CommandQueueHw<gfxCoreFamily>::migrateSharedAllocationsIfRequested(
    bool isMigrationRequested,
    CommandList *commandList) {

    if (isMigrationRequested) {
        commandList->migrateSharedAllocations();
    }
}

template <GFXCORE_FAMILY gfxCoreFamily>
void CommandQueueHw<gfxCoreFamily>::prefetchMemoryToDeviceAssociatedWithCmdList(CommandList *commandList) {
    if (commandList->isMemoryPrefetchRequested()) {
        auto prefetchManager = this->device->getDriverHandle()->getMemoryManager()->getPrefetchManager();
        prefetchManager->migrateAllocationsToGpu(commandList->getPrefetchContext(),
                                                 *this->device->getDriverHandle()->getSvmAllocsManager(),
                                                 *this->device->getNEODevice(),
                                                 *this->csr);
    }
}

template <GFXCORE_FAMILY gfxCoreFamily>
void CommandQueueHw<gfxCoreFamily>::assignCsrTaskCountToFenceIfAvailable(ze_fence_handle_t hFence) {
    if (hFence) {
        Fence::fromHandle(hFence)->assignTaskCountFromCsr();
    }
}

template <GFXCORE_FAMILY gfxCoreFamily>
void CommandQueueHw<gfxCoreFamily>::dispatchTaskCountPostSyncByMiFlushDw(
    bool isDispatchTaskCountPostSyncRequired,
    NEO::LinearStream &cmdStream) {

    if (!isDispatchTaskCountPostSyncRequired) {
        return;
    }

    uint64_t postSyncAddress = this->csr->getTagAllocation()->getGpuAddress();
    TaskCountType postSyncData = this->csr->peekTaskCount() + 1;

    NEO::EncodeDummyBlitWaArgs waArgs{false, &(this->device->getNEODevice()->getRootDeviceEnvironmentRef())};
    NEO::MiFlushArgs args{waArgs};
    args.commandWithPostSync = true;
    args.notifyEnable = this->csr->isUsedNotifyEnableForPostSync();

    NEO::EncodeMiFlushDW<GfxFamily>::programWithWa(cmdStream, postSyncAddress, postSyncData, args);
}

template <GFXCORE_FAMILY gfxCoreFamily>
void CommandQueueHw<gfxCoreFamily>::dispatchTaskCountPostSyncRegular(
    bool isDispatchTaskCountPostSyncRequired,
    NEO::LinearStream &cmdStream) {

    if (!isDispatchTaskCountPostSyncRequired) {
        return;
    }

    uint64_t postSyncAddress = this->csr->getTagAllocation()->getGpuAddress();
    TaskCountType postSyncData = this->csr->peekTaskCount() + 1;

    NEO::PipeControlArgs args;
    args.dcFlushEnable = this->csr->getDcFlushSupport();
    args.workloadPartitionOffset = this->partitionCount > 1;
    args.notifyEnable = this->csr->isUsedNotifyEnableForPostSync();
    NEO::MemorySynchronizationCommands<GfxFamily>::addBarrierWithPostSyncOperation(
        cmdStream,
        NEO::PostSyncMode::ImmediateData,
        postSyncAddress,
        postSyncData,
        device->getNEODevice()->getRootDeviceEnvironment(),
        args);
}

template <GFXCORE_FAMILY gfxCoreFamily>
void CommandQueueHw<gfxCoreFamily>::makeCsrTagAllocationResident() {
    this->csr->makeResident(*this->csr->getTagAllocation());
}

template <GFXCORE_FAMILY gfxCoreFamily>
void CommandQueueHw<gfxCoreFamily>::makeRayTracingBufferResident(NEO::GraphicsAllocation *rtBuffer) {
    if (rtBuffer) {
        this->csr->makeResident(*rtBuffer);
    }
}

template <GFXCORE_FAMILY gfxCoreFamily>
NEO::SubmissionStatus CommandQueueHw<gfxCoreFamily>::prepareAndSubmitBatchBuffer(
    CommandListExecutionContext &ctx,
    NEO::LinearStream &innerCommandStream) {

    using MI_BATCH_BUFFER_END = typename GfxFamily::MI_BATCH_BUFFER_END;

    auto &outerCommandStream = this->commandStream;

    void *endingCmd = nullptr;
    if (ctx.isDirectSubmissionEnabled) {
        auto offset = ptrDiff(innerCommandStream.getCpuBase(), outerCommandStream.getCpuBase()) + innerCommandStream.getUsed();
        uint64_t startAddress = outerCommandStream.getGraphicsAllocation()->getGpuAddress() + offset;
        if (NEO::DebugManager.flags.BatchBufferStartPrepatchingWaEnabled.get() == 0) {
            startAddress = 0;
        }

        endingCmd = innerCommandStream.getSpace(0);
        NEO::EncodeBatchBufferStartOrEnd<GfxFamily>::programBatchBufferStart(&innerCommandStream, startAddress, false, false, false);
    } else {
        auto buffer = innerCommandStream.getSpaceForCmd<MI_BATCH_BUFFER_END>();
        *(MI_BATCH_BUFFER_END *)buffer = GfxFamily::cmdInitBatchBufferEnd;
    }

    if (ctx.isNEODebuggerActive(this->device) || NEO::DebugManager.flags.EnableSWTags.get() || csr->getLogicalStateHelper()) {
        cleanLeftoverMemory(outerCommandStream, innerCommandStream);
    } else if (this->alignedChildStreamPadding) {
        void *paddingPtr = innerCommandStream.getSpace(this->alignedChildStreamPadding);
        memset(paddingPtr, 0, this->alignedChildStreamPadding);
    }
    size_t startOffset = (this->startingCmdBuffer == &this->firstCmdListStream)
                             ? 0
                             : ptrDiff(innerCommandStream.getCpuBase(), outerCommandStream.getCpuBase());

    return submitBatchBuffer(startOffset,
                             csr->getResidencyAllocations(),
                             endingCmd,
                             ctx.anyCommandListWithCooperativeKernels);
}

template <GFXCORE_FAMILY gfxCoreFamily>
void CommandQueueHw<gfxCoreFamily>::cleanLeftoverMemory(NEO::LinearStream &outerCommandStream, NEO::LinearStream &innerCommandStream) {

    auto leftoverSpace = outerCommandStream.getUsed() - innerCommandStream.getUsed();
    leftoverSpace -= ptrDiff(innerCommandStream.getCpuBase(), outerCommandStream.getCpuBase());
    if (leftoverSpace > 0) {
        auto memory = innerCommandStream.getSpace(leftoverSpace);
        memset(memory, 0, leftoverSpace);
    }
}

template <GFXCORE_FAMILY gfxCoreFamily>
void CommandQueueHw<gfxCoreFamily>::updateTaskCountAndPostSync(bool isDispatchTaskCountPostSyncRequired) {

    if (!isDispatchTaskCountPostSyncRequired) {
        return;
    }
    this->taskCount = this->csr->peekTaskCount();
    this->csr->setLatestFlushedTaskCount(this->taskCount);
}

template <GFXCORE_FAMILY gfxCoreFamily>
ze_result_t CommandQueueHw<gfxCoreFamily>::waitForCommandQueueCompletionAndCleanHeapContainer() {

    ze_result_t ret = ZE_RESULT_SUCCESS;

    if (this->getSynchronousMode() == ZE_COMMAND_QUEUE_MODE_SYNCHRONOUS) {
        if (const auto syncRet = this->synchronize(std::numeric_limits<uint64_t>::max()); syncRet == ZE_RESULT_ERROR_DEVICE_LOST) {
            ret = syncRet;
        }
    } else {
        this->csr->pollForCompletion();
    }
    this->heapContainer.clear();

    return ret;
}

template <GFXCORE_FAMILY gfxCoreFamily>
ze_result_t CommandQueueHw<gfxCoreFamily>::handleSubmissionAndCompletionResults(
    NEO::SubmissionStatus submitRet,
    ze_result_t completionRet) {

    if ((submitRet != NEO::SubmissionStatus::SUCCESS) || (completionRet == ZE_RESULT_ERROR_DEVICE_LOST)) {
        for (auto &gfx : this->csr->getResidencyAllocations()) {
            if (this->csr->peekLatestFlushedTaskCount() == 0) {
                gfx->releaseUsageInOsContext(this->csr->getOsContext().getContextId());
            } else {
                gfx->updateTaskCount(this->csr->peekLatestFlushedTaskCount(), this->csr->getOsContext().getContextId());
            }
        }
        if (completionRet != ZE_RESULT_ERROR_DEVICE_LOST) {
            completionRet = getErrorCodeForSubmissionStatus(submitRet);
        }
    }

    return completionRet;
}

template <GFXCORE_FAMILY gfxCoreFamily>
size_t CommandQueueHw<gfxCoreFamily>::estimatePipelineSelectCmdSize() {
    if (!this->pipelineSelectStateTracking) {
        bool gpgpuEnabled = csr->getPreambleSetFlag();
        return !gpgpuEnabled * NEO::PreambleHelper<GfxFamily>::getCmdSizeForPipelineSelect(device->getNEODevice()->getRootDeviceEnvironment());
    }
    return 0;
}

template <GFXCORE_FAMILY gfxCoreFamily>
size_t CommandQueueHw<gfxCoreFamily>::estimatePipelineSelectCmdSizeForMultipleCommandLists(NEO::StreamProperties &csrState,
                                                                                           const NEO::StreamProperties &cmdListRequired,
                                                                                           const NEO::StreamProperties &cmdListFinal,
                                                                                           bool &gpgpuEnabled,
                                                                                           NEO::StreamProperties &requiredState,
                                                                                           bool &propertyDirty) {
    if (!this->pipelineSelectStateTracking) {
        return 0;
    }

    size_t estimatedSize = 0;
    bool psCurrentDirty = !gpgpuEnabled;
    if (psCurrentDirty) {
        csrState.pipelineSelect.copyPropertiesAll(cmdListRequired.pipelineSelect);
    } else {
        csrState.pipelineSelect.copyPropertiesSystolicMode(cmdListRequired.pipelineSelect);
        psCurrentDirty = csrState.pipelineSelect.isDirty();
    }

    if (psCurrentDirty) {
        estimatedSize += NEO::PreambleHelper<GfxFamily>::getCmdSizeForPipelineSelect(device->getNEODevice()->getRootDeviceEnvironment());

        propertyDirty = true;
        requiredState.pipelineSelect = csrState.pipelineSelect;
    }

    if (!gpgpuEnabled) {
        csrState.pipelineSelect.copyPropertiesAll(cmdListFinal.pipelineSelect);
        gpgpuEnabled = true;
    } else {
        csrState.pipelineSelect.copyPropertiesSystolicMode(cmdListFinal.pipelineSelect);
    }

    return estimatedSize;
}

template <GFXCORE_FAMILY gfxCoreFamily>
void CommandQueueHw<gfxCoreFamily>::programOneCmdListPipelineSelect(NEO::LinearStream &commandStream,
                                                                    CommandListRequiredStateChange &cmdListRequired) {
    if (!this->pipelineSelectStateTracking) {
        return;
    }

    if (cmdListRequired.flags.propertyPsDirty) {
        bool systolic = cmdListRequired.requiredState.pipelineSelect.systolicMode.value == 1;
        NEO::PipelineSelectArgs args = {
            systolic,
            false,
            false,
            cmdListRequired.commandList->getSystolicModeSupport()};

        NEO::PreambleHelper<GfxFamily>::programPipelineSelect(&commandStream, args, device->getNEODevice()->getRootDeviceEnvironment());
        csr->setPreambleSetFlag(true);
    }
}

template <GFXCORE_FAMILY gfxCoreFamily>
size_t CommandQueueHw<gfxCoreFamily>::estimateScmCmdSizeForMultipleCommandLists(NEO::StreamProperties &csrState,
                                                                                bool &scmStateDirty,
                                                                                const NEO::StreamProperties &cmdListRequired,
                                                                                const NEO::StreamProperties &cmdListFinal,
                                                                                NEO::StreamProperties &requiredState,
                                                                                bool &propertyDirty) {
    if (!this->stateComputeModeTracking) {
        return 0;
    }

    size_t estimatedSize = 0;
    bool scmCurrentDirty = scmStateDirty;
    if (scmStateDirty) {
        csrState.stateComputeMode.copyPropertiesAll(cmdListRequired.stateComputeMode);
    } else {
        csrState.stateComputeMode.copyPropertiesGrfNumberThreadArbitration(cmdListRequired.stateComputeMode);
        scmCurrentDirty = csrState.stateComputeMode.isDirty();
    }

    if (scmCurrentDirty) {
        bool isRcs = this->getCsr()->isRcs();
        estimatedSize = NEO::EncodeComputeMode<GfxFamily>::getCmdSizeForComputeMode(device->getNEODevice()->getRootDeviceEnvironment(), false, isRcs);

        propertyDirty = true;
        requiredState.stateComputeMode = csrState.stateComputeMode;
        requiredState.pipelineSelect = csrState.pipelineSelect;
    }

    if (scmStateDirty) {
        csrState.stateComputeMode.copyPropertiesAll(cmdListFinal.stateComputeMode);
        scmStateDirty = false;
    } else {
        csrState.stateComputeMode.copyPropertiesGrfNumberThreadArbitration(cmdListFinal.stateComputeMode);
    }

    return estimatedSize;
}

template <GFXCORE_FAMILY gfxCoreFamily>
void CommandQueueHw<gfxCoreFamily>::programRequiredStateComputeModeForCommandList(NEO::LinearStream &commandStream,
                                                                                  CommandListRequiredStateChange &cmdListRequired) {
    if (!this->stateComputeModeTracking) {
        return;
    }

    if (cmdListRequired.flags.propertyScmDirty) {
        NEO::PipelineSelectArgs pipelineSelectArgs = {
            cmdListRequired.requiredState.pipelineSelect.systolicMode.value == 1,
            false,
            false,
            cmdListRequired.commandList->getSystolicModeSupport()};

        NEO::EncodeComputeMode<GfxFamily>::programComputeModeCommandWithSynchronization(commandStream, cmdListRequired.requiredState.stateComputeMode, pipelineSelectArgs,
                                                                                        false, device->getNEODevice()->getRootDeviceEnvironment(), this->csr->isRcs(),
                                                                                        this->csr->getDcFlushSupport(), nullptr);
        this->csr->setStateComputeModeDirty(false);
    }
}

template <GFXCORE_FAMILY gfxCoreFamily>
void CommandQueueHw<gfxCoreFamily>::programRequiredStateBaseAddressForCommandList(CommandListExecutionContext &ctx,
                                                                                  NEO::LinearStream &commandStream,
                                                                                  CommandListRequiredStateChange &cmdListRequired) {

    if (!this->stateBaseAddressTracking) {
        return;
    }

    if (cmdListRequired.flags.propertySbaDirty) {
        bool indirectHeapInLocalMemory = cmdListRequired.commandList->getCmdContainer().isIndirectHeapInLocalMemory();
        programStateBaseAddress(ctx.scratchGsba,
                                indirectHeapInLocalMemory,
                                commandStream,
                                ctx.cachedMOCSAllowed,
                                &cmdListRequired.requiredState);

        ctx.gsbaStateDirty = false;
    }
}

template <GFXCORE_FAMILY gfxCoreFamily>
void CommandQueueHw<gfxCoreFamily>::updateBaseAddressState(CommandList *lastCommandList) {
    auto csrHw = static_cast<NEO::CommandStreamReceiverHw<GfxFamily> *>(csr);
    auto &commandContainer = lastCommandList->getCmdContainer();

    if (lastCommandList->getCmdListHeapAddressModel() == NEO::HeapAddressModel::GlobalStateless) {
        csrHw->getSshState().updateAndCheck(csr->getGlobalStatelessHeap());
    } else {
        auto dsh = commandContainer.getIndirectHeap(NEO::HeapType::DYNAMIC_STATE);
        if (dsh != nullptr) {
            csrHw->getDshState().updateAndCheck(dsh);
        }

        auto ssh = commandContainer.getIndirectHeap(NEO::HeapType::SURFACE_STATE);
        if (ssh != nullptr) {
            csrHw->getSshState().updateAndCheck(ssh);
        }
    }

    auto ioh = commandContainer.getIndirectHeap(NEO::HeapType::INDIRECT_OBJECT);
    csrHw->getIohState().updateAndCheck(ioh);
}

template <GFXCORE_FAMILY gfxCoreFamily>
size_t CommandQueueHw<gfxCoreFamily>::estimateStateBaseAddressCmdSizeForMultipleCommandLists(bool &baseAddressStateDirty,
                                                                                             NEO::HeapAddressModel commandListHeapAddressModel,
                                                                                             NEO::StreamProperties &csrState,
                                                                                             const NEO::StreamProperties &cmdListRequired,
                                                                                             const NEO::StreamProperties &cmdListFinal,
                                                                                             NEO::StreamProperties &requiredState,
                                                                                             bool &propertyDirty) {
    if (!this->stateBaseAddressTracking) {
        return 0;
    }

    size_t estimatedSize = 0;

    if (commandListHeapAddressModel == NEO::HeapAddressModel::GlobalStateless) {
        estimatedSize = estimateStateBaseAddressCmdSizeForGlobalStatelessCommandList(baseAddressStateDirty, csrState, cmdListRequired, cmdListFinal, requiredState, propertyDirty);
    } else {
        estimatedSize = estimateStateBaseAddressCmdSizeForPrivateHeapCommandList(baseAddressStateDirty, csrState, cmdListRequired, cmdListFinal, requiredState, propertyDirty);
    }

    return estimatedSize;
}

template <GFXCORE_FAMILY gfxCoreFamily>
size_t CommandQueueHw<gfxCoreFamily>::estimateStateBaseAddressCmdSizeForGlobalStatelessCommandList(bool &baseAddressStateDirty,
                                                                                                   NEO::StreamProperties &csrState,
                                                                                                   const NEO::StreamProperties &cmdListRequired,
                                                                                                   const NEO::StreamProperties &cmdListFinal,
                                                                                                   NEO::StreamProperties &requiredState,
                                                                                                   bool &propertyDirty) {
    auto globalStatelessHeap = this->csr->getGlobalStatelessHeap();

    size_t estimatedSize = 0;

    if (baseAddressStateDirty) {
        csrState.stateBaseAddress.copyPropertiesAll(cmdListRequired.stateBaseAddress);
    } else {
        csrState.stateBaseAddress.copyPropertiesStatelessMocs(cmdListRequired.stateBaseAddress);
    }
    csrState.stateBaseAddress.setPropertiesSurfaceState(globalStatelessHeap->getHeapGpuBase(), globalStatelessHeap->getHeapSizeInPages());

    if (baseAddressStateDirty || csrState.stateBaseAddress.isDirty()) {
        bool useBtiCommand = csrState.stateBaseAddress.bindingTablePoolBaseAddress.value != NEO::StreamProperty64::initValue;
        estimatedSize = estimateStateBaseAddressCmdDispatchSize(useBtiCommand);

        propertyDirty = true;
        requiredState.stateBaseAddress = csrState.stateBaseAddress;
    }

    if (baseAddressStateDirty) {
        csrState.stateBaseAddress.copyPropertiesAll(cmdListFinal.stateBaseAddress);
        baseAddressStateDirty = false;
    } else {
        csrState.stateBaseAddress.copyPropertiesStatelessMocs(cmdListFinal.stateBaseAddress);
    }

    return estimatedSize;
}

template <GFXCORE_FAMILY gfxCoreFamily>
size_t CommandQueueHw<gfxCoreFamily>::estimateStateBaseAddressCmdSizeForPrivateHeapCommandList(bool &baseAddressStateDirty,
                                                                                               NEO::StreamProperties &csrState,
                                                                                               const NEO::StreamProperties &cmdListRequired,
                                                                                               const NEO::StreamProperties &cmdListFinal,
                                                                                               NEO::StreamProperties &requiredState,
                                                                                               bool &propertyDirty) {
    size_t estimatedSize = 0;

    csrState.stateBaseAddress.copyPropertiesAll(cmdListRequired.stateBaseAddress);
    if (baseAddressStateDirty || csrState.stateBaseAddress.isDirty()) {
        bool useBtiCommand = csrState.stateBaseAddress.bindingTablePoolBaseAddress.value != NEO::StreamProperty64::initValue;
        estimatedSize = estimateStateBaseAddressCmdDispatchSize(useBtiCommand);

        baseAddressStateDirty = false;
        propertyDirty = true;
        requiredState.stateBaseAddress = csrState.stateBaseAddress;
    }
    csrState.stateBaseAddress.copyPropertiesAll(cmdListFinal.stateBaseAddress);

    return estimatedSize;
}
template <GFXCORE_FAMILY gfxCoreFamily>
size_t CommandQueueHw<gfxCoreFamily>::estimateStateBaseAddressDebugTracking() {
    size_t size = 0;
    if (NEO::Debugger::isDebugEnabled(this->internalUsage) && this->device->getL0Debugger() != nullptr) {
        const size_t trackedAddressesCount = 6;
        size = this->device->getL0Debugger()->getSbaTrackingCommandsSize(trackedAddressesCount);
    }
    return size;
}

} // namespace L0

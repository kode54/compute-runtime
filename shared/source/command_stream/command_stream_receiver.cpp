/*
 * Copyright (C) 2018-2023 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 */

#include "shared/source/command_stream/command_stream_receiver.h"

#include "shared/source/built_ins/built_ins.h"
#include "shared/source/command_container/implicit_scaling.h"
#include "shared/source/command_stream/aub_subcapture_status.h"
#include "shared/source/command_stream/experimental_command_buffer.h"
#include "shared/source/command_stream/preemption.h"
#include "shared/source/command_stream/scratch_space_controller.h"
#include "shared/source/command_stream/submission_status.h"
#include "shared/source/command_stream/submissions_aggregator.h"
#include "shared/source/command_stream/tag_allocation_layout.h"
#include "shared/source/debug_settings/debug_settings_manager.h"
#include "shared/source/device/device.h"
#include "shared/source/direct_submission/direct_submission_controller.h"
#include "shared/source/execution_environment/execution_environment.h"
#include "shared/source/execution_environment/root_device_environment.h"
#include "shared/source/gmm_helper/cache_settings_helper.h"
#include "shared/source/gmm_helper/page_table_mngr.h"
#include "shared/source/helpers/api_specific_config.h"
#include "shared/source/helpers/array_count.h"
#include "shared/source/helpers/flat_batch_buffer_helper.h"
#include "shared/source/helpers/flush_stamp.h"
#include "shared/source/helpers/gfx_core_helper.h"
#include "shared/source/helpers/logical_state_helper.h"
#include "shared/source/helpers/pause_on_gpu_properties.h"
#include "shared/source/helpers/ray_tracing_helper.h"
#include "shared/source/helpers/string.h"
#include "shared/source/helpers/timestamp_packet.h"
#include "shared/source/memory_manager/allocation_properties.h"
#include "shared/source/memory_manager/internal_allocation_storage.h"
#include "shared/source/memory_manager/memory_manager.h"
#include "shared/source/memory_manager/surface.h"
#include "shared/source/os_interface/os_context.h"
#include "shared/source/os_interface/os_interface.h"
#include "shared/source/os_interface/os_thread.h"
#include "shared/source/os_interface/product_helper.h"
#include "shared/source/os_interface/sys_calls_common.h"
#include "shared/source/utilities/hw_timestamps.h"
#include "shared/source/utilities/perf_counter.h"
#include "shared/source/utilities/tag_allocator.h"
#include "shared/source/utilities/wait_util.h"

#include <iostream>

namespace AubMemDump {
#include "aub_services.h"
}

namespace NEO {

// Global table of CommandStreamReceiver factories for HW and tests
CommandStreamReceiverCreateFunc commandStreamReceiverFactory[2 * IGFX_MAX_CORE] = {};

CommandStreamReceiver::CommandStreamReceiver(ExecutionEnvironment &executionEnvironment,
                                             uint32_t rootDeviceIndex,
                                             const DeviceBitfield deviceBitfield)
    : executionEnvironment(executionEnvironment), rootDeviceIndex(rootDeviceIndex), deviceBitfield(deviceBitfield) {
    residencyAllocations.reserve(startingResidencyContainerSize);

    latestSentStatelessMocsConfig = CacheSettings::unknownMocs;
    submissionAggregator.reset(new SubmissionAggregator());
    if (ApiSpecificConfig::getApiType() == ApiSpecificConfig::L0) {
        this->dispatchMode = DispatchMode::ImmediateDispatch;
    }
    if (DebugManager.flags.CsrDispatchMode.get()) {
        this->dispatchMode = (DispatchMode)DebugManager.flags.CsrDispatchMode.get();
    }
    flushStamp.reset(new FlushStampTracker(true));
    for (int i = 0; i < IndirectHeap::Type::NUM_TYPES; ++i) {
        indirectHeap[i] = nullptr;
    }
    internalAllocationStorage = std::make_unique<InternalAllocationStorage>(*this);
    const auto &hwInfo = peekHwInfo();
    uint32_t subDeviceCount = static_cast<uint32_t>(deviceBitfield.count());
    auto &gfxCoreHelper = getGfxCoreHelper();
    auto &rootDeviceEnvironment = peekRootDeviceEnvironment();
    bool platformImplicitScaling = gfxCoreHelper.platformSupportsImplicitScaling(rootDeviceEnvironment);
    if (NEO::ImplicitScalingHelper::isImplicitScalingEnabled(deviceBitfield, platformImplicitScaling) &&
        subDeviceCount > 1 &&
        DebugManager.flags.EnableStaticPartitioning.get() != 0) {
        this->activePartitions = subDeviceCount;
        this->staticWorkPartitioningEnabled = true;
    }
    this->streamProperties.initSupport(rootDeviceEnvironment);
    auto &productHelper = getProductHelper();
    productHelper.fillFrontEndPropertiesSupportStructure(feSupportFlags, hwInfo);
    productHelper.fillPipelineSelectPropertiesSupportStructure(pipelineSupportFlags, hwInfo);
    productHelper.fillStateBaseAddressPropertiesSupportStructure(sbaSupportFlags);
    this->doubleSbaWa = productHelper.isAdditionalStateBaseAddressWARequired(hwInfo);
    this->l1CachePolicyData.init(productHelper);
}

CommandStreamReceiver::~CommandStreamReceiver() {
    if (userPauseConfirmation) {
        {
            std::unique_lock<SpinLock> lock{debugPauseStateLock};
            *debugPauseStateAddress = DebugPauseState::terminate;
        }

        userPauseConfirmation->join();
    }

    for (int i = 0; i < IndirectHeap::Type::NUM_TYPES; ++i) {
        if (indirectHeap[i] != nullptr) {
            auto allocation = indirectHeap[i]->getGraphicsAllocation();
            if (allocation != nullptr) {
                internalAllocationStorage->storeAllocation(std::unique_ptr<GraphicsAllocation>(allocation), REUSABLE_ALLOCATION);
            }
            delete indirectHeap[i];
        }
    }
    cleanupResources();

    internalAllocationStorage->cleanAllocationList(-1, REUSABLE_ALLOCATION);
    internalAllocationStorage->cleanAllocationList(-1, TEMPORARY_ALLOCATION);
    getMemoryManager()->unregisterEngineForCsr(this);
}

SubmissionStatus CommandStreamReceiver::submitBatchBuffer(BatchBuffer &batchBuffer, ResidencyContainer &allocationsForResidency) {
    this->latestSentTaskCount = taskCount + 1;

    SubmissionStatus retVal = this->flush(batchBuffer, allocationsForResidency);

    if (retVal != NEO::SubmissionStatus::SUCCESS) {
        return retVal;
    }
    if (!isUpdateTagFromWaitEnabled()) {
        this->latestFlushedTaskCount = taskCount + 1;
    }
    taskCount++;

    return retVal;
}

void CommandStreamReceiver::makeResident(MultiGraphicsAllocation &gfxAllocation) {
    makeResident(*gfxAllocation.getGraphicsAllocation(rootDeviceIndex));
}

void CommandStreamReceiver::makeResident(GraphicsAllocation &gfxAllocation) {
    auto submissionTaskCount = this->taskCount + 1;
    if (gfxAllocation.isResidencyTaskCountBelow(submissionTaskCount, osContext->getContextId())) {
        auto pushAllocations = true;

        if (DebugManager.flags.MakeEachAllocationResident.get() != -1) {
            pushAllocations = !DebugManager.flags.MakeEachAllocationResident.get();
        }

        if (pushAllocations) {
            this->getResidencyAllocations().push_back(&gfxAllocation);
        }

        gfxAllocation.updateTaskCount(submissionTaskCount, osContext->getContextId());
        if (this->dispatchMode == DispatchMode::BatchedDispatch) {
            checkForNewResources(submissionTaskCount, gfxAllocation.getTaskCount(osContext->getContextId()), gfxAllocation);
            if (!gfxAllocation.isResident(osContext->getContextId())) {
                this->totalMemoryUsed += gfxAllocation.getUnderlyingBufferSize();
            }
        }
    }
    gfxAllocation.updateResidencyTaskCount(submissionTaskCount, osContext->getContextId());
}

void CommandStreamReceiver::processEviction() {
    this->getEvictionAllocations().clear();
}

void CommandStreamReceiver::makeNonResident(GraphicsAllocation &gfxAllocation) {
    if (gfxAllocation.isResident(osContext->getContextId())) {
        if (gfxAllocation.peekEvictable()) {
            this->getEvictionAllocations().push_back(&gfxAllocation);
        } else {
            gfxAllocation.setEvictable(true);
        }
    }

    if (!gfxAllocation.isAlwaysResident(this->osContext->getContextId())) {
        gfxAllocation.releaseResidencyInOsContext(this->osContext->getContextId());
    }
}

void CommandStreamReceiver::makeSurfacePackNonResident(ResidencyContainer &allocationsForResidency, bool clearAllocations) {
    for (auto &surface : allocationsForResidency) {
        this->makeNonResident(*surface);
    }
    if (clearAllocations) {
        allocationsForResidency.clear();
    }
    this->processEviction();
}

SubmissionStatus CommandStreamReceiver::processResidency(const ResidencyContainer &allocationsForResidency, uint32_t handleId) {
    return SubmissionStatus::SUCCESS;
}

void CommandStreamReceiver::makeResidentHostPtrAllocation(GraphicsAllocation *gfxAllocation) {
    makeResident(*gfxAllocation);
}

WaitStatus CommandStreamReceiver::waitForTaskCount(TaskCountType requiredTaskCount) {
    auto address = getTagAddress();
    if (!skipResourceCleanup() && address) {
        this->downloadTagAllocation(requiredTaskCount);
        return baseWaitFunction(address, WaitParams{false, false, 0}, requiredTaskCount);
    }

    return WaitStatus::Ready;
}

WaitStatus CommandStreamReceiver::waitForTaskCountAndCleanAllocationList(TaskCountType requiredTaskCount, uint32_t allocationUsage) {
    WaitStatus waitStatus{WaitStatus::Ready};
    auto &list = allocationUsage == TEMPORARY_ALLOCATION ? internalAllocationStorage->getTemporaryAllocations() : internalAllocationStorage->getAllocationsForReuse();
    if (!list.peekIsEmpty()) {
        waitStatus = this->CommandStreamReceiver::waitForTaskCount(requiredTaskCount);
    }
    internalAllocationStorage->cleanAllocationList(requiredTaskCount, allocationUsage);

    return waitStatus;
}

WaitStatus CommandStreamReceiver::waitForTaskCountAndCleanTemporaryAllocationList(TaskCountType requiredTaskCount) {
    return waitForTaskCountAndCleanAllocationList(requiredTaskCount, TEMPORARY_ALLOCATION);
}

void CommandStreamReceiver::ensureCommandBufferAllocation(LinearStream &commandStream, size_t minimumRequiredSize, size_t additionalAllocationSize) {
    if (commandStream.getAvailableSpace() >= minimumRequiredSize) {
        return;
    }

    auto alignment = MemoryConstants::pageSize64k;

    if (DebugManager.flags.ForceCommandBufferAlignment.get() != -1) {
        alignment = DebugManager.flags.ForceCommandBufferAlignment.get() * MemoryConstants::kiloByte;
    }

    const auto allocationSize = alignUp(minimumRequiredSize + additionalAllocationSize, alignment);
    constexpr static auto allocationType = AllocationType::COMMAND_BUFFER;
    auto allocation = this->getInternalAllocationStorage()->obtainReusableAllocation(allocationSize, allocationType).release();
    if (allocation == nullptr) {
        const AllocationProperties commandStreamAllocationProperties{rootDeviceIndex, true, allocationSize, allocationType,
                                                                     isMultiOsContextCapable(), false, osContext->getDeviceBitfield()};
        allocation = this->getMemoryManager()->allocateGraphicsMemoryWithProperties(commandStreamAllocationProperties);
    }
    DEBUG_BREAK_IF(allocation == nullptr);

    if (commandStream.getGraphicsAllocation() != nullptr) {
        getInternalAllocationStorage()->storeAllocation(std::unique_ptr<GraphicsAllocation>(commandStream.getGraphicsAllocation()), REUSABLE_ALLOCATION);
    }

    commandStream.replaceBuffer(allocation->getUnderlyingBuffer(), allocationSize - additionalAllocationSize);
    commandStream.replaceGraphicsAllocation(allocation);
}

void CommandStreamReceiver::fillReusableAllocationsList() {
    auto &gfxCoreHelper = getGfxCoreHelper();
    auto amountToFill = gfxCoreHelper.getAmountOfAllocationsToFill();
    for (auto i = 0u; i < amountToFill; i++) {
        const AllocationProperties commandStreamAllocationProperties{rootDeviceIndex, true, MemoryConstants::pageSize64k, AllocationType::COMMAND_BUFFER,
                                                                     isMultiOsContextCapable(), false, deviceBitfield};
        auto allocation = this->getMemoryManager()->allocateGraphicsMemoryWithProperties(commandStreamAllocationProperties);
        getInternalAllocationStorage()->storeAllocation(std::unique_ptr<GraphicsAllocation>(allocation), REUSABLE_ALLOCATION);
        this->makeResident(*allocation);
    }
}

bool CommandStreamReceiver::initializeResources() {
    if (!resourcesInitialized) {
        auto lock = obtainUniqueOwnership();
        if (!resourcesInitialized) {
            if (!osContext->ensureContextInitialized()) {
                return false;
            }
            this->fillReusableAllocationsList();
            this->resourcesInitialized = true;
        }
    }

    return true;
}

MemoryManager *CommandStreamReceiver::getMemoryManager() const {
    DEBUG_BREAK_IF(!executionEnvironment.memoryManager);
    return executionEnvironment.memoryManager.get();
}

LinearStream &CommandStreamReceiver::getCS(size_t minRequiredSize) {
    constexpr static auto additionalAllocationSize = MemoryConstants::cacheLineSize + CSRequirements::csOverfetchSize;
    ensureCommandBufferAllocation(this->commandStream, minRequiredSize, additionalAllocationSize);
    return commandStream;
}

OSInterface *CommandStreamReceiver::getOSInterface() const {
    return executionEnvironment.rootDeviceEnvironments[rootDeviceIndex]->osInterface.get();
}

GmmHelper *CommandStreamReceiver::peekGmmHelper() const {
    return executionEnvironment.rootDeviceEnvironments[rootDeviceIndex]->gmmHelper.get();
}

uint64_t CommandStreamReceiver::getWorkPartitionAllocationGpuAddress() const {
    if (isStaticWorkPartitioningEnabled()) {
        return getWorkPartitionAllocation()->getGpuAddress();
    }
    return 0;
}

bool CommandStreamReceiver::isRcs() const {
    return this->osContext->getEngineType() == aub_stream::ENGINE_RCS;
}

bool CommandStreamReceiver::skipResourceCleanup() const {
    return ((this->getOSInterface() && this->getOSInterface()->getDriverModel() && this->getOSInterface()->getDriverModel()->skipResourceCleanup()) || forceSkipResourceCleanupRequired);
}

bool CommandStreamReceiver::isGpuHangDetected() const {
    if (DebugManager.flags.DisableGpuHangDetection.get()) {
        return false;
    }

    return this->osContext && this->getOSInterface() && this->getOSInterface()->getDriverModel() && this->getOSInterface()->getDriverModel()->isGpuHangDetected(*osContext);
}

void CommandStreamReceiver::cleanupResources() {
    waitForTaskCountAndCleanAllocationList(this->latestFlushedTaskCount, TEMPORARY_ALLOCATION);
    waitForTaskCountAndCleanAllocationList(this->latestFlushedTaskCount, REUSABLE_ALLOCATION);

    if (debugSurface) {
        getMemoryManager()->freeGraphicsMemory(debugSurface);
        debugSurface = nullptr;
    }

    if (commandStream.getCpuBase()) {
        getMemoryManager()->freeGraphicsMemory(commandStream.getGraphicsAllocation());
        commandStream.replaceGraphicsAllocation(nullptr);
        commandStream.replaceBuffer(nullptr, 0);
    }

    if (tagsMultiAllocation) {
        // Null tag address to prevent waiting for tag update when freeing it
        tagAllocation = nullptr;
        tagAddress = nullptr;
        DEBUG_BREAK_IF(tagAllocation != nullptr);
        DEBUG_BREAK_IF(tagAddress != nullptr);

        for (auto graphicsAllocation : tagsMultiAllocation->getGraphicsAllocations()) {
            getMemoryManager()->freeGraphicsMemory(graphicsAllocation);
        }
        delete tagsMultiAllocation;
        tagsMultiAllocation = nullptr;
    }

    if (globalFenceAllocation) {
        getMemoryManager()->freeGraphicsMemory(globalFenceAllocation);
        globalFenceAllocation = nullptr;
    }

    if (preemptionAllocation) {
        getMemoryManager()->freeGraphicsMemory(preemptionAllocation);
        preemptionAllocation = nullptr;
    }

    if (perDssBackedBuffer) {
        getMemoryManager()->freeGraphicsMemory(perDssBackedBuffer);
        perDssBackedBuffer = nullptr;
    }

    if (clearColorAllocation) {
        getMemoryManager()->freeGraphicsMemory(clearColorAllocation);
        clearColorAllocation = nullptr;
    }

    if (workPartitionAllocation) {
        getMemoryManager()->freeGraphicsMemory(workPartitionAllocation);
        workPartitionAllocation = nullptr;
    }

    if (kernelArgsBufferAllocation) {
        getMemoryManager()->freeGraphicsMemory(kernelArgsBufferAllocation);
        kernelArgsBufferAllocation = nullptr;
    }

    if (globalStatelessHeapAllocation) {
        getMemoryManager()->freeGraphicsMemory(globalStatelessHeapAllocation);
        globalStatelessHeapAllocation = nullptr;
    }
}

WaitStatus CommandStreamReceiver::waitForCompletionWithTimeout(const WaitParams &params, TaskCountType taskCountToWait) {
    bool printWaitForCompletion = DebugManager.flags.LogWaitingForCompletion.get();
    if (printWaitForCompletion) {
        printTagAddressContent(taskCountToWait, params.waitTimeout, true);
    }

    TaskCountType latestSentTaskCount = this->latestFlushedTaskCount;
    if (latestSentTaskCount < taskCountToWait) {
        if (!this->flushBatchedSubmissions()) {
            const auto isGpuHang{isGpuHangDetected()};
            return isGpuHang ? WaitStatus::GpuHang : WaitStatus::NotReady;
        }
    }

    auto retCode = baseWaitFunction(getTagAddress(), params, taskCountToWait);
    if (printWaitForCompletion) {
        printTagAddressContent(taskCountToWait, params.waitTimeout, false);
    }
    return retCode;
}

bool CommandStreamReceiver::checkGpuHangDetected(TimeType currentTime, TimeType &lastHangCheckTime) const {
    std::chrono::microseconds elapsedTimeSinceGpuHangCheck = std::chrono::duration_cast<std::chrono::microseconds>(currentTime - lastHangCheckTime);

    if (elapsedTimeSinceGpuHangCheck.count() >= gpuHangCheckPeriod.count()) {
        lastHangCheckTime = currentTime;
        if (isGpuHangDetected()) {
            return true;
        }
    }
    return false;
}

WaitStatus CommandStreamReceiver::baseWaitFunction(volatile TagAddressType *pollAddress, const WaitParams &params, TaskCountType taskCountToWait) {
    std::chrono::high_resolution_clock::time_point waitStartTime, lastHangCheckTime, currentTime;
    int64_t timeDiff = 0;

    TaskCountType latestSentTaskCount = this->latestFlushedTaskCount;
    if (latestSentTaskCount < taskCountToWait) {
        if (this->flushTagUpdate() != NEO::SubmissionStatus::SUCCESS) {
            return WaitStatus::NotReady;
        }
    }
    volatile TagAddressType *partitionAddress = pollAddress;

    waitStartTime = std::chrono::high_resolution_clock::now();
    lastHangCheckTime = waitStartTime;
    for (uint32_t i = 0; i < activePartitions; i++) {
        while (*partitionAddress < taskCountToWait && timeDiff <= params.waitTimeout) {
            this->downloadTagAllocation(taskCountToWait);

            if (!params.indefinitelyPoll && WaitUtils::waitFunction(partitionAddress, taskCountToWait)) {
                break;
            }

            currentTime = std::chrono::high_resolution_clock::now();
            if (checkGpuHangDetected(currentTime, lastHangCheckTime)) {
                return WaitStatus::GpuHang;
            }

            if (params.enableTimeout) {
                timeDiff = std::chrono::duration_cast<std::chrono::microseconds>(currentTime - waitStartTime).count();
            }
        }

        partitionAddress = ptrOffset(partitionAddress, this->postSyncWriteOffset);
    }

    partitionAddress = pollAddress;
    for (uint32_t i = 0; i < activePartitions; i++) {
        if (*partitionAddress < taskCountToWait) {
            return WaitStatus::NotReady;
        }
        partitionAddress = ptrOffset(partitionAddress, this->postSyncWriteOffset);
    }

    return WaitStatus::Ready;
}

void CommandStreamReceiver::setTagAllocation(GraphicsAllocation *allocation) {
    this->tagAllocation = allocation;
    UNRECOVERABLE_IF(allocation == nullptr);
    this->tagAddress = reinterpret_cast<TagAddressType *>(allocation->getUnderlyingBuffer());
    this->debugPauseStateAddress = reinterpret_cast<DebugPauseState *>(
        reinterpret_cast<uint8_t *>(allocation->getUnderlyingBuffer()) + TagAllocationLayout::debugPauseStateAddressOffset);
}

MultiGraphicsAllocation &CommandStreamReceiver::createTagsMultiAllocation() {
    RootDeviceIndicesContainer rootDeviceIndices;

    rootDeviceIndices.push_back(rootDeviceIndex);

    auto maxRootDeviceIndex = static_cast<uint32_t>(this->executionEnvironment.rootDeviceEnvironments.size() - 1);
    auto allocations = new MultiGraphicsAllocation(maxRootDeviceIndex);

    AllocationProperties unifiedMemoryProperties{rootDeviceIndex, MemoryConstants::pageSize, AllocationType::TAG_BUFFER, systemMemoryBitfield};

    this->getMemoryManager()->createMultiGraphicsAllocationInSystemMemoryPool(rootDeviceIndices, unifiedMemoryProperties, *allocations);
    return *allocations;
}
bool CommandStreamReceiver::ensureTagAllocationForRootDeviceIndex(uint32_t rootDeviceIndex) {
    UNRECOVERABLE_IF(!tagsMultiAllocation);
    if (rootDeviceIndex >= tagsMultiAllocation->getGraphicsAllocations().size()) {
        return false;
    }
    if (tagsMultiAllocation->getGraphicsAllocation(rootDeviceIndex)) {
        return true;
    }
    AllocationProperties allocationProperties{rootDeviceIndex, MemoryConstants::pageSize, AllocationType::TAG_BUFFER, systemMemoryBitfield};
    allocationProperties.flags.allocateMemory = false;
    auto graphicsAllocation = this->getMemoryManager()->createGraphicsAllocationFromExistingStorage(allocationProperties, tagAllocation->getUnderlyingBuffer(), *tagsMultiAllocation);
    if (!graphicsAllocation) {
        return false;
    }
    tagsMultiAllocation->addAllocation(graphicsAllocation);
    return true;
}

FlushStamp CommandStreamReceiver::obtainCurrentFlushStamp() const {
    return flushStamp->peekStamp();
}

void CommandStreamReceiver::setRequiredScratchSizes(uint32_t newRequiredScratchSize, uint32_t newRequiredPrivateScratchSize) {
    if (newRequiredScratchSize > requiredScratchSize) {
        requiredScratchSize = newRequiredScratchSize;
    }
    if (newRequiredPrivateScratchSize > requiredPrivateScratchSize) {
        requiredPrivateScratchSize = newRequiredPrivateScratchSize;
    }
}

GraphicsAllocation *CommandStreamReceiver::getScratchAllocation() {
    return scratchSpaceController->getScratchSpaceAllocation();
}

void CommandStreamReceiver::overwriteFlatBatchBufferHelper(FlatBatchBufferHelper *newHelper) {
    flatBatchBufferHelper.reset(newHelper);
}

void CommandStreamReceiver::initProgrammingFlags() {
    isPreambleSent = false;
    gsbaFor32BitProgrammed = false;
    bindingTableBaseAddressRequired = true;
    mediaVfeStateDirty = true;
    lastVmeSubslicesConfig = false;
    stateComputeModeDirty = true;

    lastSentL3Config = 0;
    lastMediaSamplerConfig = -1;
    lastPreemptionMode = PreemptionMode::Initial;

    latestSentStatelessMocsConfig = CacheSettings::unknownMocs;
    this->streamProperties.stateBaseAddress.statelessMocs = {};

    lastSentUseGlobalAtomics = false;
    this->streamProperties.stateBaseAddress.globalAtomics = {};
}

void CommandStreamReceiver::programForAubSubCapture(bool wasActiveInPreviousEnqueue, bool isActive) {
    if (!wasActiveInPreviousEnqueue && isActive) {
        // force CSR reprogramming upon subcapture activation
        this->initProgrammingFlags();
    }
    if (wasActiveInPreviousEnqueue && !isActive) {
        // flush BB upon subcapture deactivation
        this->flushBatchedSubmissions();
    }
}

ResidencyContainer &CommandStreamReceiver::getResidencyAllocations() {
    return this->residencyAllocations;
}

ResidencyContainer &CommandStreamReceiver::getEvictionAllocations() {
    return this->evictionAllocations;
}

AubSubCaptureStatus CommandStreamReceiver::checkAndActivateAubSubCapture(const std::string &kernelName) { return {false, false}; }

void CommandStreamReceiver::addAubComment(const char *comment) {}

void CommandStreamReceiver::downloadAllocation(GraphicsAllocation &gfxAllocation) {
    if (this->downloadAllocationImpl) {
        this->downloadAllocationImpl(gfxAllocation);
    }
}

void CommandStreamReceiver::startControllingDirectSubmissions() {
    auto controller = this->executionEnvironment.directSubmissionController.get();
    if (controller) {
        controller->startControlling();
    }
}

GraphicsAllocation *CommandStreamReceiver::allocateDebugSurface(size_t size) {
    UNRECOVERABLE_IF(debugSurface != nullptr);
    debugSurface = getMemoryManager()->allocateGraphicsMemoryWithProperties({rootDeviceIndex, size, AllocationType::DEBUG_CONTEXT_SAVE_AREA, getOsContext().getDeviceBitfield()});
    return debugSurface;
}

void *CommandStreamReceiver::getIndirectHeapCurrentPtr(IndirectHeapType heapType) const {
    auto heap = indirectHeap[heapType];
    if (heap) {
        return heap->getSpace(0);
    }
    return nullptr;
}

IndirectHeap &CommandStreamReceiver::getIndirectHeap(IndirectHeap::Type heapType,
                                                     size_t minRequiredSize) {
    DEBUG_BREAK_IF(static_cast<uint32_t>(heapType) >= arrayCount(indirectHeap));
    auto &heap = indirectHeap[heapType];
    GraphicsAllocation *heapMemory = nullptr;

    if (heap)
        heapMemory = heap->getGraphicsAllocation();

    if (heap && heap->getAvailableSpace() < minRequiredSize && heapMemory) {
        internalAllocationStorage->storeAllocation(std::unique_ptr<GraphicsAllocation>(heapMemory), REUSABLE_ALLOCATION);
        heapMemory = nullptr;
        this->heapStorageRequiresRecyclingTag = true;
    }

    if (!heapMemory) {
        allocateHeapMemory(heapType, minRequiredSize, heap);
    }

    return *heap;
}

void CommandStreamReceiver::allocateHeapMemory(IndirectHeap::Type heapType,
                                               size_t minRequiredSize, IndirectHeap *&indirectHeap) {
    size_t reservedSize = 0;
    auto finalHeapSize = HeapSize::getDefaultHeapSize(HeapSize::defaultHeapSize);
    if (IndirectHeap::Type::SURFACE_STATE == heapType) {
        finalHeapSize = defaultSshSize;
    }
    bool requireInternalHeap = IndirectHeap::Type::INDIRECT_OBJECT == heapType ? canUse4GbHeaps : false;

    if (DebugManager.flags.AddPatchInfoCommentsForAUBDump.get()) {
        requireInternalHeap = false;
    }

    minRequiredSize += reservedSize;

    finalHeapSize = alignUp(std::max(finalHeapSize, minRequiredSize), MemoryConstants::pageSize);
    auto allocationType = AllocationType::LINEAR_STREAM;
    if (requireInternalHeap) {
        allocationType = AllocationType::INTERNAL_HEAP;
    }
    auto heapMemory = internalAllocationStorage->obtainReusableAllocation(finalHeapSize, allocationType).release();

    if (!heapMemory) {
        heapMemory = getMemoryManager()->allocateGraphicsMemoryWithProperties({rootDeviceIndex, true, finalHeapSize, allocationType,
                                                                               isMultiOsContextCapable(), false, osContext->getDeviceBitfield()});
    } else {
        finalHeapSize = std::max(heapMemory->getUnderlyingBufferSize(), finalHeapSize);
    }

    if (IndirectHeap::Type::SURFACE_STATE == heapType) {
        DEBUG_BREAK_IF(minRequiredSize > defaultSshSize - MemoryConstants::pageSize);
        finalHeapSize = defaultSshSize - MemoryConstants::pageSize;
    }

    if (indirectHeap) {
        indirectHeap->replaceBuffer(heapMemory->getUnderlyingBuffer(), finalHeapSize);
        indirectHeap->replaceGraphicsAllocation(heapMemory);
    } else {
        indirectHeap = new IndirectHeap(heapMemory, requireInternalHeap);
        indirectHeap->overrideMaxSize(finalHeapSize);
    }
    scratchSpaceController->reserveHeap(heapType, indirectHeap);
}

void CommandStreamReceiver::releaseIndirectHeap(IndirectHeap::Type heapType) {
    DEBUG_BREAK_IF(static_cast<uint32_t>(heapType) >= arrayCount(indirectHeap));
    auto &heap = indirectHeap[heapType];

    if (heap) {
        auto heapMemory = heap->getGraphicsAllocation();
        if (heapMemory != nullptr)
            internalAllocationStorage->storeAllocation(std::unique_ptr<GraphicsAllocation>(heapMemory), REUSABLE_ALLOCATION);
        heap->replaceBuffer(nullptr, 0);
        heap->replaceGraphicsAllocation(nullptr);
    }
}

void CommandStreamReceiver::setExperimentalCmdBuffer(std::unique_ptr<ExperimentalCommandBuffer> &&cmdBuffer) {
    experimentalCmdBuffer = std::move(cmdBuffer);
}

void *CommandStreamReceiver::asyncDebugBreakConfirmation(void *arg) {
    auto self = reinterpret_cast<CommandStreamReceiver *>(arg);

    do {
        auto debugPauseStateValue = DebugPauseState::waitingForUserStartConfirmation;
        if (DebugManager.flags.PauseOnGpuMode.get() != PauseOnGpuProperties::PauseMode::AfterWorkload) {
            do {
                {
                    std::unique_lock<SpinLock> lock{self->debugPauseStateLock};
                    debugPauseStateValue = *self->debugPauseStateAddress;
                }

                if (debugPauseStateValue == DebugPauseState::terminate) {
                    return nullptr;
                }
                std::this_thread::yield();
            } while (debugPauseStateValue != DebugPauseState::waitingForUserStartConfirmation);
            std::cout << "Debug break: Press enter to start workload" << std::endl;
            self->debugConfirmationFunction();
            debugPauseStateValue = DebugPauseState::hasUserStartConfirmation;
            {
                std::unique_lock<SpinLock> lock{self->debugPauseStateLock};
                *self->debugPauseStateAddress = debugPauseStateValue;
            }
        }

        if (DebugManager.flags.PauseOnGpuMode.get() != PauseOnGpuProperties::PauseMode::BeforeWorkload) {
            do {
                {
                    std::unique_lock<SpinLock> lock{self->debugPauseStateLock};
                    debugPauseStateValue = *self->debugPauseStateAddress;
                }
                if (debugPauseStateValue == DebugPauseState::terminate) {
                    return nullptr;
                }
                std::this_thread::yield();
            } while (debugPauseStateValue != DebugPauseState::waitingForUserEndConfirmation);

            std::cout << "Debug break: Workload ended, press enter to continue" << std::endl;
            self->debugConfirmationFunction();

            {
                std::unique_lock<SpinLock> lock{self->debugPauseStateLock};
                *self->debugPauseStateAddress = DebugPauseState::hasUserEndConfirmation;
            }
        }
    } while (DebugManager.flags.PauseOnEnqueue.get() == PauseOnGpuProperties::DebugFlagValues::OnEachEnqueue || DebugManager.flags.PauseOnBlitCopy.get() == PauseOnGpuProperties::DebugFlagValues::OnEachEnqueue);
    return nullptr;
}

bool CommandStreamReceiver::initializeTagAllocation() {
    this->tagsMultiAllocation = &this->createTagsMultiAllocation();

    auto tagAllocation = tagsMultiAllocation->getGraphicsAllocation(rootDeviceIndex);
    if (!tagAllocation) {
        return false;
    }

    this->setTagAllocation(tagAllocation);
    auto initValue = DebugManager.flags.EnableNullHardware.get() ? static_cast<uint32_t>(-1) : initialHardwareTag;
    auto tagAddress = this->tagAddress;
    auto completionFence = reinterpret_cast<TaskCountType *>(getCompletionAddress());
    UNRECOVERABLE_IF(!completionFence);
    uint32_t subDevices = static_cast<uint32_t>(this->deviceBitfield.count());
    for (uint32_t i = 0; i < subDevices; i++) {
        *tagAddress = initValue;
        tagAddress = ptrOffset(tagAddress, this->postSyncWriteOffset);
        *completionFence = 0;
        completionFence = ptrOffset(completionFence, this->postSyncWriteOffset);
    }
    *this->debugPauseStateAddress = DebugManager.flags.EnableNullHardware.get() ? DebugPauseState::disabled : DebugPauseState::waitingForFirstSemaphore;

    PRINT_DEBUG_STRING(DebugManager.flags.PrintTagAllocationAddress.get(), stdout,
                       "\nCreated tag allocation %p for engine %u\n",
                       this->tagAddress, static_cast<uint32_t>(osContext->getEngineType()));

    if (DebugManager.flags.PauseOnEnqueue.get() != -1 || DebugManager.flags.PauseOnBlitCopy.get() != -1) {
        userPauseConfirmation = Thread::create(CommandStreamReceiver::asyncDebugBreakConfirmation, reinterpret_cast<void *>(this));
    }

    this->barrierCountTagAddress = ptrOffset(this->tagAddress, TagAllocationLayout::barrierCountOffset);

    return true;
}

bool CommandStreamReceiver::createWorkPartitionAllocation(const Device &device) {
    if (!staticWorkPartitioningEnabled) {
        return false;
    }
    UNRECOVERABLE_IF(device.getNumGenericSubDevices() < 2);

    AllocationProperties properties{this->rootDeviceIndex, true, 4096u, AllocationType::WORK_PARTITION_SURFACE, true, false, deviceBitfield};
    this->workPartitionAllocation = getMemoryManager()->allocateGraphicsMemoryWithProperties(properties);
    if (this->workPartitionAllocation == nullptr) {
        return false;
    }

    uint32_t logicalId = 0;
    for (uint32_t deviceIndex = 0; deviceIndex < deviceBitfield.size(); deviceIndex++) {
        if (!deviceBitfield.test(deviceIndex)) {
            continue;
        }

        const uint32_t copySrc[2] = {logicalId++, deviceIndex};
        DeviceBitfield copyBitfield{};
        copyBitfield.set(deviceIndex);
        auto copySuccess = MemoryTransferHelper::transferMemoryToAllocationBanks(device, workPartitionAllocation, 0, copySrc, sizeof(copySrc), copyBitfield);

        if (!copySuccess) {
            return false;
        }
    }

    return true;
}

bool CommandStreamReceiver::createGlobalFenceAllocation() {
    auto &gfxCoreHelper = getGfxCoreHelper();
    auto &hwInfo = peekHwInfo();
    if (!gfxCoreHelper.isFenceAllocationRequired(hwInfo)) {
        return true;
    }

    DEBUG_BREAK_IF(this->globalFenceAllocation != nullptr);
    this->globalFenceAllocation = getMemoryManager()->allocateGraphicsMemoryWithProperties({rootDeviceIndex, MemoryConstants::pageSize, AllocationType::GLOBAL_FENCE, osContext->getDeviceBitfield()});
    return this->globalFenceAllocation != nullptr;
}

bool CommandStreamReceiver::createPreemptionAllocation() {
    auto hwInfo = executionEnvironment.rootDeviceEnvironments[rootDeviceIndex]->getHardwareInfo();
    auto &gfxCoreHelper = getGfxCoreHelper();
    size_t preemptionSurfaceSize = hwInfo->capabilityTable.requiredPreemptionSurfaceSize;
    if (DebugManager.flags.OverrideCsrAllocationSize.get() > 0) {
        preemptionSurfaceSize = DebugManager.flags.OverrideCsrAllocationSize.get();
    }
    AllocationProperties properties{rootDeviceIndex, true, preemptionSurfaceSize, AllocationType::PREEMPTION, isMultiOsContextCapable(), false, deviceBitfield};
    properties.flags.uncacheable = hwInfo->workaroundTable.flags.waCSRUncachable;
    properties.alignment = gfxCoreHelper.getPreemptionAllocationAlignment();
    this->preemptionAllocation = getMemoryManager()->allocateGraphicsMemoryWithProperties(properties);
    return this->preemptionAllocation != nullptr;
}

std::unique_lock<CommandStreamReceiver::MutexType> CommandStreamReceiver::obtainUniqueOwnership() {
    return std::unique_lock<CommandStreamReceiver::MutexType>(this->ownershipMutex);
}
std::unique_lock<CommandStreamReceiver::MutexType> CommandStreamReceiver::obtainHostPtrSurfaceCreationLock() {
    return std::unique_lock<CommandStreamReceiver::MutexType>(this->hostPtrSurfaceCreationMutex);
}
AllocationsList &CommandStreamReceiver::getTemporaryAllocations() { return internalAllocationStorage->getTemporaryAllocations(); }
AllocationsList &CommandStreamReceiver::getAllocationsForReuse() { return internalAllocationStorage->getAllocationsForReuse(); }
AllocationsList &CommandStreamReceiver::getDeferredAllocations() { return internalAllocationStorage->getDeferredAllocations(); }

bool CommandStreamReceiver::createAllocationForHostSurface(HostPtrSurface &surface, bool requiresL3Flush) {
    std::unique_lock<decltype(hostPtrSurfaceCreationMutex)> lock = this->obtainHostPtrSurfaceCreationLock();
    auto allocation = internalAllocationStorage->obtainTemporaryAllocationWithPtr(surface.getSurfaceSize(), surface.getMemoryPointer(), AllocationType::EXTERNAL_HOST_PTR);

    if (allocation == nullptr) {
        auto memoryManager = getMemoryManager();
        AllocationProperties properties{rootDeviceIndex,
                                        false, // allocateMemory
                                        surface.getSurfaceSize(), AllocationType::EXTERNAL_HOST_PTR,
                                        false, // isMultiStorageAllocation
                                        osContext->getDeviceBitfield()};
        properties.flags.flushL3RequiredForRead = properties.flags.flushL3RequiredForWrite = requiresL3Flush;
        allocation.reset(memoryManager->allocateGraphicsMemoryWithProperties(properties, surface.getMemoryPointer()));
        if (allocation == nullptr && surface.peekIsPtrCopyAllowed()) {
            // Try with no host pointer allocation and copy
            allocation.reset(memoryManager->allocateInternalGraphicsMemoryWithHostCopy(rootDeviceIndex,
                                                                                       internalAllocationStorage->getDeviceBitfield(),
                                                                                       surface.getMemoryPointer(),
                                                                                       surface.getSurfaceSize()));
        }
    }

    if (allocation == nullptr) {
        return false;
    }
    allocation->hostPtrTaskCountAssignment++;
    allocation->updateTaskCount(0u, osContext->getContextId());
    surface.setAllocation(allocation.get());
    internalAllocationStorage->storeAllocation(std::move(allocation), TEMPORARY_ALLOCATION);
    return true;
}

TagAllocatorBase *CommandStreamReceiver::getEventTsAllocator() {
    if (profilingTimeStampAllocator.get() == nullptr) {
        RootDeviceIndicesContainer rootDeviceIndices = {rootDeviceIndex};
        profilingTimeStampAllocator = std::make_unique<TagAllocator<HwTimeStamps>>(rootDeviceIndices, getMemoryManager(), getPreferredTagPoolSize(), MemoryConstants::cacheLineSize,
                                                                                   sizeof(HwTimeStamps), false, osContext->getDeviceBitfield());
    }
    return profilingTimeStampAllocator.get();
}

TagAllocatorBase *CommandStreamReceiver::getEventPerfCountAllocator(const uint32_t tagSize) {
    if (perfCounterAllocator.get() == nullptr) {
        RootDeviceIndicesContainer rootDeviceIndices = {rootDeviceIndex};
        perfCounterAllocator = std::make_unique<TagAllocator<HwPerfCounter>>(
            rootDeviceIndices, getMemoryManager(), getPreferredTagPoolSize(), MemoryConstants::cacheLineSize, tagSize, false, osContext->getDeviceBitfield());
    }
    return perfCounterAllocator.get();
}

size_t CommandStreamReceiver::getPreferredTagPoolSize() const {
    if (DebugManager.flags.DisableTimestampPacketOptimizations.get()) {
        return 1;
    }

    return 2048;
}

bool CommandStreamReceiver::expectMemory(const void *gfxAddress, const void *srcAddress,
                                         size_t length, uint32_t compareOperation) {
    auto isMemoryEqual = (memcmp(gfxAddress, srcAddress, length) == 0);
    auto isEqualMemoryExpected = (compareOperation == AubMemDump::CmdServicesMemTraceMemoryCompare::CompareOperationValues::CompareEqual);

    return (isMemoryEqual == isEqualMemoryExpected);
}

bool CommandStreamReceiver::needsPageTableManager() const {
    auto hwInfo = executionEnvironment.rootDeviceEnvironments[rootDeviceIndex]->getHardwareInfo();
    auto &productHelper = getProductHelper();

    if (pageTableManager.get() != nullptr) {
        return false;
    }
    return productHelper.isPageTableManagerSupported(*hwInfo);
}

void CommandStreamReceiver::printDeviceIndex() {
    if (DebugManager.flags.PrintDeviceAndEngineIdOnSubmission.get()) {
        printf("%u: Submission to RootDevice Index: %u, Sub-Devices Mask: %lu, EngineId: %u (%s, %s)\n",
               SysCalls::getProcessId(),
               this->getRootDeviceIndex(),
               this->osContext->getDeviceBitfield().to_ulong(),
               this->osContext->getEngineType(),
               EngineHelpers::engineTypeToString(this->osContext->getEngineType()).c_str(),
               EngineHelpers::engineUsageToString(this->osContext->getEngineUsage()).c_str());
    }
}

void CommandStreamReceiver::checkForNewResources(TaskCountType submittedTaskCount, TaskCountType allocationTaskCount, GraphicsAllocation &gfxAllocation) {
    if (useNewResourceImplicitFlush) {
        if (allocationTaskCount == GraphicsAllocation::objectNotUsed && !GraphicsAllocation::isIsaAllocationType(gfxAllocation.getAllocationType())) {
            newResources = true;
            if (DebugManager.flags.ProvideVerboseImplicitFlush.get()) {
                printf("New resource detected of type %llu\n", static_cast<unsigned long long>(gfxAllocation.getAllocationType()));
            }
        }
    }
}

bool CommandStreamReceiver::checkImplicitFlushForGpuIdle() {
    if (useGpuIdleImplicitFlush) {
        if (this->taskCount == *getTagAddress()) {
            return true;
        }
    }
    return false;
}

void CommandStreamReceiver::downloadTagAllocation(TaskCountType taskCountToWait) {
    if (this->getTagAllocation()) {
        if (taskCountToWait && taskCountToWait <= this->peekLatestFlushedTaskCount()) {
            this->downloadAllocation(*this->getTagAllocation());
        }
    }
}

bool CommandStreamReceiver::testTaskCountReady(volatile TagAddressType *pollAddress, TaskCountType taskCountToWait) {
    this->downloadTagAllocation(taskCountToWait);
    for (uint32_t i = 0; i < activePartitions; i++) {
        if (!WaitUtils::waitFunction(pollAddress, taskCountToWait)) {
            return false;
        }

        pollAddress = ptrOffset(pollAddress, this->postSyncWriteOffset);
    }
    return true;
}

const HardwareInfo &CommandStreamReceiver::peekHwInfo() const {
    return *peekRootDeviceEnvironment().getHardwareInfo();
}

const RootDeviceEnvironment &CommandStreamReceiver::peekRootDeviceEnvironment() const {
    return *executionEnvironment.rootDeviceEnvironments[rootDeviceIndex];
}

const GfxCoreHelper &CommandStreamReceiver::getGfxCoreHelper() const {
    return peekRootDeviceEnvironment().getHelper<GfxCoreHelper>();
}

const ProductHelper &CommandStreamReceiver::getProductHelper() const {
    return peekRootDeviceEnvironment().getHelper<ProductHelper>();
}

const ReleaseHelper *CommandStreamReceiver::getReleaseHelper() const {
    return peekRootDeviceEnvironment().getReleaseHelper();
}

TaskCountType CommandStreamReceiver::getCompletionValue(const GraphicsAllocation &gfxAllocation) {
    if (completionFenceValuePointer) {
        return *completionFenceValuePointer;
    }
    auto osContextId = osContext->getContextId();
    return gfxAllocation.getTaskCount(osContextId);
}

bool CommandStreamReceiver::createPerDssBackedBuffer(Device &device) {
    UNRECOVERABLE_IF(perDssBackedBuffer != nullptr);
    auto size = RayTracingHelper::getTotalMemoryBackedFifoSize(device);

    perDssBackedBuffer = getMemoryManager()->allocateGraphicsMemoryWithProperties({rootDeviceIndex, size, AllocationType::BUFFER, device.getDeviceBitfield()});

    return perDssBackedBuffer != nullptr;
}

void CommandStreamReceiver::printTagAddressContent(TaskCountType taskCountToWait, int64_t waitTimeout, bool start) {
    auto postSyncAddress = getTagAddress();
    if (start) {
        PRINT_DEBUG_STRING(true, stdout,
                           "\nWaiting for task count %llu at location %p with timeout %llx. Current value:",
                           taskCountToWait, postSyncAddress, waitTimeout);
    } else {
        PRINT_DEBUG_STRING(true, stdout,
                           "%s", "\nWaiting completed. Current value:");
    }
    for (uint32_t i = 0; i < activePartitions; i++) {
        PRINT_DEBUG_STRING(true, stdout, " %u", *postSyncAddress);
        postSyncAddress = ptrOffset(postSyncAddress, this->postSyncWriteOffset);
    }
    PRINT_DEBUG_STRING(true, stdout, "%s", "\n");
}

LogicalStateHelper *CommandStreamReceiver::getLogicalStateHelper() const {
    return logicalStateHelper.get();
}

bool CommandStreamReceiver::isTbxMode() const {
    return (getType() == NEO::CommandStreamReceiverType::CSR_TBX || getType() == NEO::CommandStreamReceiverType::CSR_TBX_WITH_AUB);
}

TaskCountType CompletionStamp::getTaskCountFromSubmissionStatusError(SubmissionStatus status) {
    switch (status) {
    case SubmissionStatus::OUT_OF_HOST_MEMORY:
        return CompletionStamp::outOfHostMemory;
    case SubmissionStatus::OUT_OF_MEMORY:
        return CompletionStamp::outOfDeviceMemory;
    case SubmissionStatus::FAILED:
        return CompletionStamp::failed;
    default:
        return 0;
    }
}
uint64_t CommandStreamReceiver::getBarrierCountGpuAddress() const { return ptrOffset(this->tagAllocation->getGpuAddress(), TagAllocationLayout::barrierCountOffset); }
uint64_t CommandStreamReceiver::getDebugPauseStateGPUAddress() const { return tagAllocation->getGpuAddress() + TagAllocationLayout::debugPauseStateAddressOffset; }
uint64_t CommandStreamReceiver::getCompletionAddress() const {
    uint64_t completionFenceAddress = castToUint64(const_cast<TagAddressType *>(tagAddress));
    if (completionFenceAddress == 0) {
        return 0;
    }
    completionFenceAddress += TagAllocationLayout::completionFenceOffset;
    return completionFenceAddress;
}

void CommandStreamReceiver::createGlobalStatelessHeap() {
    if (this->globalStatelessHeapAllocation == nullptr) {
        auto lock = obtainUniqueOwnership();
        if (this->globalStatelessHeapAllocation == nullptr) {
            constexpr size_t heapSize = 16 * MemoryConstants::kiloByte;
            constexpr AllocationType allocationType = AllocationType::LINEAR_STREAM;

            AllocationProperties properties{rootDeviceIndex, true, heapSize, allocationType,
                                            isMultiOsContextCapable(), false, osContext->getDeviceBitfield()};

            this->globalStatelessHeapAllocation = getMemoryManager()->allocateGraphicsMemoryWithProperties(properties);

            this->globalStatelessHeap = std::make_unique<IndirectHeap>(this->globalStatelessHeapAllocation);
        }
    }
}

bool CommandStreamReceiver::isRayTracingStateProgramingNeeded(Device &device) const {
    return device.getRTMemoryBackedBuffer() && getBtdCommandDirty();
}

std::function<void()> CommandStreamReceiver::debugConfirmationFunction = []() { std::cin.get(); };
} // namespace NEO

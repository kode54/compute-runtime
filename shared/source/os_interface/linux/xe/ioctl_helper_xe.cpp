/*
 * Copyright (C) 2023 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 */

#include "shared/source/os_interface/linux/xe/ioctl_helper_xe.h"

#include "shared/source/command_stream/csr_definitions.h"
#include "shared/source/debug_settings/debug_settings_manager.h"
#include "shared/source/execution_environment/root_device_environment.h"
#include "shared/source/helpers/basic_math.h"
#include "shared/source/helpers/bit_helpers.h"
#include "shared/source/helpers/common_types.h"
#include "shared/source/helpers/constants.h"
#include "shared/source/helpers/hw_info.h"
#include "shared/source/helpers/ptr_math.h"
#include "shared/source/helpers/register_offsets.h"
#include "shared/source/helpers/string.h"
#include "shared/source/os_interface/linux/drm_neo.h"
#include "shared/source/os_interface/linux/engine_info.h"
#include "shared/source/os_interface/linux/memory_info.h"
#include "shared/source/os_interface/linux/os_context_linux.h"

#include "drm/i915_drm_prelim.h"
#include "drm/xe_drm.h"

#include <iostream>

#define XE_FIND_INVALID_INSTANCE 16

#define STRINGIFY_ME(X) return #X
#define RETURN_ME(X) return X

#define XE_USERPTR_FAKE_FLAG 0x800000
#define XE_USERPTR_FAKE_MASK 0x7FFFFF

#define USER_FENCE_VALUE 0xc0ffee0000000000ull

namespace NEO {

static_assert(DRM_XE_ENGINE_CLASS_RENDER == I915_ENGINE_CLASS_RENDER);
static_assert(DRM_XE_ENGINE_CLASS_COPY == I915_ENGINE_CLASS_COPY);
static_assert(DRM_XE_ENGINE_CLASS_VIDEO_DECODE == I915_ENGINE_CLASS_VIDEO);
static_assert(DRM_XE_ENGINE_CLASS_VIDEO_ENHANCE == I915_ENGINE_CLASS_VIDEO_ENHANCE);
static_assert(DRM_XE_ENGINE_CLASS_COMPUTE == I915_ENGINE_CLASS_COMPUTE);
static_assert(XE_MEM_REGION_CLASS_VRAM == I915_MEMORY_CLASS_DEVICE);
static_assert(XE_MEM_REGION_CLASS_SYSMEM == I915_MEMORY_CLASS_SYSTEM);

int IoctlHelperXe::xeGetQuery(Query *data) {
    if (data->numItems == 1) {
        QueryItem *queryItem = (QueryItem *)data->itemsPtr;
        std::vector<uint8_t> *queryData = nullptr;
        switch (queryItem->queryId) {
        case static_cast<int>(DrmParam::QueryHwconfigTable):
            queryData = &hwconfigFakei915;
            break;
        case static_cast<int>(DrmParam::QueryTopologyInfo):
            queryData = &topologyFakei915;
            break;
        default:
            xeLog("error: bad query 0x%x\n", queryItem->queryId);
            return -1;
        }
        if (queryData != nullptr) {
            if (queryItem->length == 0) {
                queryItem->length = static_cast<int32_t>(queryData->size());
                return 0;
            }
            if (queryItem->length != static_cast<int32_t>(queryData->size())) {
                xeLog("error: incorrect length 0x%x 0x%lx\n", queryItem->length, queryData->size());
                return -1;
            }
            memcpy_s(reinterpret_cast<void *>(queryItem->dataPtr),
                     queryItem->length, queryData->data(), queryItem->length);
            return 0;
        }
    }
    return -1;
}

const char *IoctlHelperXe::xeGetClassName(int className) {
    switch (className) {
    case DRM_XE_ENGINE_CLASS_RENDER:
        return "rcs";
    case DRM_XE_ENGINE_CLASS_COPY:
        return "bcs";
    case DRM_XE_ENGINE_CLASS_VIDEO_DECODE:
        return "vcs";
    case DRM_XE_ENGINE_CLASS_VIDEO_ENHANCE:
        return "vecs";
    case DRM_XE_ENGINE_CLASS_COMPUTE:
        return "ccs";
    }
    return "???";
}

const char *IoctlHelperXe::xeGetBindOpName(int bindOp) {
    switch (bindOp) {
    case XE_VM_BIND_OP_MAP:
        return "MAP";
    case XE_VM_BIND_OP_UNMAP:
        return "UNMAP";
    case XE_VM_BIND_OP_MAP_USERPTR:
        return "MAP_USERPTR";
    case XE_VM_BIND_OP_MAP | XE_VM_BIND_FLAG_ASYNC:
        return "AS_MAP";
    case XE_VM_BIND_OP_UNMAP | XE_VM_BIND_FLAG_ASYNC:
        return "AS_UNMAP";
    case XE_VM_BIND_OP_MAP_USERPTR | XE_VM_BIND_FLAG_ASYNC:
        return "AS_MAP_USERPTR";
    }
    return "unknown_OP";
}

const char *IoctlHelperXe::xeGetengineClassName(uint32_t engineClass) {
    switch (engineClass) {
    case DRM_XE_ENGINE_CLASS_RENDER:
        return "DRM_XE_ENGINE_CLASS_RENDER";
    case DRM_XE_ENGINE_CLASS_COPY:
        return "DRM_XE_ENGINE_CLASS_COPY";
    case DRM_XE_ENGINE_CLASS_VIDEO_DECODE:
        return "DRM_XE_ENGINE_CLASS_VIDEO_DECODE";
    case DRM_XE_ENGINE_CLASS_VIDEO_ENHANCE:
        return "DRM_XE_ENGINE_CLASS_VIDEO_ENHANCE";
    case DRM_XE_ENGINE_CLASS_COMPUTE:
        return "DRM_XE_ENGINE_CLASS_COMPUTE";
    default:
        return "?";
    }
}

IoctlHelperXe::IoctlHelperXe(Drm &drmArg) : IoctlHelper(drmArg) {
    xeLog("IoctlHelperXe::IoctlHelperXe\n", "");
}

bool IoctlHelperXe::initialize() {
    bool ret = false;
    xeLog("IoctlHelperXe::initialize\n", "");
    DebugManager.flags.ForceUserptrAlignment.set(64);
    DebugManager.flags.UseVmBind.set(1);
    DebugManager.flags.EnableImmediateVmBindExt.set(1);
    DebugManager.flags.UseNewQueryTopoIoctl.set(0);
    DebugManager.flags.RenderCompressedBuffersEnabled.set(0);

    struct drm_xe_device_query queryConfig = {};
    queryConfig.query = DRM_XE_DEVICE_QUERY_CONFIG;

    auto retVal = IoctlHelper::ioctl(DrmIoctl::Query, &queryConfig);
    if (retVal != 0 || queryConfig.size == 0) {
        return false;
    }
    auto data = std::vector<uint8_t>(sizeof(drm_xe_query_config) + sizeof(uint64_t) * queryConfig.size, 0);
    struct drm_xe_query_config *config = reinterpret_cast<struct drm_xe_query_config *>(data.data());
    queryConfig.data = castToUint64(config);
    IoctlHelper::ioctl(DrmIoctl::Query, &queryConfig);
    xeLog("XE_QUERY_CONFIG_REV_AND_DEVICE_ID\t%#llx\n",
          config->info[XE_QUERY_CONFIG_REV_AND_DEVICE_ID]);
    xeLog("  REV_ID\t\t\t\t%#llx\n",
          config->info[XE_QUERY_CONFIG_REV_AND_DEVICE_ID] >> 16);
    xeLog("  DEVICE_ID\t\t\t\t%#llx\n",
          config->info[XE_QUERY_CONFIG_REV_AND_DEVICE_ID] & 0xffff);
    xeLog("XE_QUERY_CONFIG_FLAGS\t\t\t%#llx\n",
          config->info[XE_QUERY_CONFIG_FLAGS]);
    xeLog("  XE_QUERY_CONFIG_FLAGS_HAS_VRAM\t%s\n",
          config->info[XE_QUERY_CONFIG_FLAGS] &
                  XE_QUERY_CONFIG_FLAGS_HAS_VRAM
              ? "ON"
              : "OFF");
    xeLog("  XE_QUERY_CONFIG_FLAGS_USE_GUC\t\t%s\n",
          config->info[XE_QUERY_CONFIG_FLAGS] &
                  XE_QUERY_CONFIG_FLAGS_USE_GUC
              ? "ON"
              : "OFF");
    xeLog("XE_QUERY_CONFIG_MIN_ALIGNEMENT\t\t%#llx\n",
          config->info[XE_QUERY_CONFIG_MIN_ALIGNEMENT]);
    xeLog("XE_QUERY_CONFIG_VA_BITS\t\t%#llx\n",
          config->info[XE_QUERY_CONFIG_VA_BITS]);
    xeLog("XE_QUERY_CONFIG_GT_COUNT\t\t%llu\n",
          config->info[XE_QUERY_CONFIG_GT_COUNT]);
    xeLog("XE_QUERY_CONFIG_MEM_REGION_COUNT\t%llu\n",
          config->info[XE_QUERY_CONFIG_MEM_REGION_COUNT]);

    chipsetId = config->info[XE_QUERY_CONFIG_REV_AND_DEVICE_ID] & 0xffff;
    revId = static_cast<int>(config->info[XE_QUERY_CONFIG_REV_AND_DEVICE_ID] >> 16);
    hasVram = config->info[XE_QUERY_CONFIG_FLAGS] & XE_QUERY_CONFIG_FLAGS_HAS_VRAM ? 1 : 0;
    addressWidth = static_cast<uint32_t>(config->info[XE_QUERY_CONFIG_VA_BITS]);

    memset(&queryConfig, 0, sizeof(queryConfig));
    queryConfig.query = DRM_XE_DEVICE_QUERY_HWCONFIG;
    IoctlHelper::ioctl(DrmIoctl::Query, &queryConfig);
    hwconfigFakei915.resize(queryConfig.size);
    queryConfig.data = castToUint64(hwconfigFakei915.data());
    IoctlHelper::ioctl(DrmIoctl::Query, &queryConfig);

    memset(&queryConfig, 0, sizeof(queryConfig));
    queryConfig.query = DRM_XE_DEVICE_QUERY_GT_TOPOLOGY;
    IoctlHelper::ioctl(DrmIoctl::Query, &queryConfig);
    std::vector<uint8_t> topology(queryConfig.size);
    queryConfig.data = castToUint64(topology.data());
    IoctlHelper::ioctl(DrmIoctl::Query, &queryConfig);
    std::vector<uint8_t> geomDss;
    std::vector<uint8_t> computeDss;
    std::vector<uint8_t> euDss;
    uint32_t topologySize = queryConfig.size;
    uint8_t *dataPtr = reinterpret_cast<uint8_t *>(topology.data());
    while (topologySize >= sizeof(struct drm_xe_query_topology_mask)) {
        struct drm_xe_query_topology_mask *topo = reinterpret_cast<struct drm_xe_query_topology_mask *>(dataPtr);
        uint32_t itemSize = sizeof(struct drm_xe_query_topology_mask) + topo->num_bytes;
        std::vector<uint8_t> *toFill = nullptr;

        switch (topo->type) {
        case XE_TOPO_DSS_GEOMETRY:
            toFill = &geomDss;
            break;
        case XE_TOPO_DSS_COMPUTE:
            toFill = &computeDss;
            break;
        case XE_TOPO_EU_PER_DSS:
            toFill = &euDss;
            break;
        default:
            xeLog("Un handle GT Topo type: %d\n", topo->type);
            break;
        }
        if (toFill != nullptr) {
            for (uint32_t j = 0; j < topo->num_bytes; j++)
                toFill->push_back(topo->mask[j]);
        }
        topologySize -= itemSize;
        dataPtr += itemSize;
    }
    topologyFakei915 = xeRebuildi915Topology(&geomDss, &computeDss, &euDss);
    if (topologyFakei915.size()) {
        ret = true;
    } else {
        xeLog("can't get i915 topology\n", "");
        UNRECOVERABLE_IF(true);
    }
    auto hwInfo = this->drm.getRootDeviceEnvironment().getMutableHardwareInfo();
    hwInfo->platform.usDeviceID = chipsetId;
    hwInfo->platform.usRevId = revId;
    return ret;
}

IoctlHelperXe::~IoctlHelperXe() {
    xeLog("IoctlHelperXe::~IoctlHelperXe\n", "");
}

bool IoctlHelperXe::isSetPairAvailable() {
    return false;
}

bool IoctlHelperXe::isVmBindAvailable() {
    return true;
}

std::vector<uint8_t> IoctlHelperXe::queryData(uint32_t queryId) {
    struct drm_xe_device_query deviceQuery = {};
    deviceQuery.query = queryId;

    IoctlHelper::ioctl(DrmIoctl::Query, &deviceQuery);

    std::vector<uint8_t> retVal(deviceQuery.size);

    deviceQuery.data = castToUint64(retVal.data());
    IoctlHelper::ioctl(DrmIoctl::Query, &deviceQuery);

    return retVal;
}

std::unique_ptr<EngineInfo> IoctlHelperXe::createEngineInfo(bool isSysmanEnabled) {
    auto enginesData = queryData(DRM_XE_DEVICE_QUERY_ENGINES);

    auto numberHwEngines = enginesData.size() /
                           sizeof(struct drm_xe_engine_class_instance);

    xeLog("numberHwEngines=%d\n", numberHwEngines);

    if (enginesData.empty()) {
        return {};
    }

    auto queriedEngines = reinterpret_cast<struct drm_xe_engine_class_instance *>(enginesData.data());

    StackVec<std::vector<EngineClassInstance>, 2> enginesPerTile{};
    std::bitset<8> multiTileMask{};

    for (auto i = 0u; i < numberHwEngines; i++) {
        auto tile = queriedEngines[i].gt_id;
        multiTileMask.set(tile);
        EngineClassInstance engineClassInstance{};
        engineClassInstance.engineClass = queriedEngines[i].engine_class;
        engineClassInstance.engineInstance = queriedEngines[i].engine_instance;
        xeLog("\t%s:%d\n", xeGetClassName(engineClassInstance.engineClass), engineClassInstance.engineInstance);

        if (engineClassInstance.engineClass == getDrmParamValue(DrmParam::EngineClassCompute) ||
            engineClassInstance.engineClass == getDrmParamValue(DrmParam::EngineClassRender) ||
            engineClassInstance.engineClass == getDrmParamValue(DrmParam::EngineClassCopy) ||
            (isSysmanEnabled && (engineClassInstance.engineClass == getDrmParamValue(DrmParam::EngineClassVideo) ||
                                 engineClassInstance.engineClass == getDrmParamValue(DrmParam::EngineClassVideoEnhance)))) {

            if (enginesPerTile.size() <= tile) {
                enginesPerTile.resize(tile + 1);
            }
            enginesPerTile[tile].push_back(engineClassInstance);
            allEngines.push_back(queriedEngines[i]);
        }
    }

    auto hwInfo = drm.getRootDeviceEnvironment().getMutableHardwareInfo();
    if (hwInfo->featureTable.flags.ftrMultiTileArch) {
        auto &multiTileArchInfo = hwInfo->gtSystemInfo.MultiTileArchInfo;
        multiTileArchInfo.IsValid = true;
        multiTileArchInfo.TileCount = multiTileMask.count();
        multiTileArchInfo.TileMask = static_cast<uint8_t>(multiTileMask.to_ulong());
    }
    return std::make_unique<EngineInfo>(&drm, enginesPerTile);
}

inline MemoryRegion createMemoryRegionFromXeMemRegion(const drm_xe_query_mem_usage::drm_xe_query_mem_region &xeMemRegion) {
    MemoryRegion memoryRegion{};
    memoryRegion.region.memoryInstance = xeMemRegion.instance;
    memoryRegion.region.memoryClass = xeMemRegion.mem_class;
    memoryRegion.probedSize = xeMemRegion.total_size;
    memoryRegion.unallocatedSize = xeMemRegion.total_size - xeMemRegion.used;
    return memoryRegion;
}

std::unique_ptr<MemoryInfo> IoctlHelperXe::createMemoryInfo() {
    auto memUsageData = queryData(DRM_XE_DEVICE_QUERY_MEM_USAGE);
    auto gtsData = queryData(DRM_XE_DEVICE_QUERY_GTS);

    if (memUsageData.empty() || gtsData.empty()) {
        return {};
    }

    MemoryInfo::RegionContainer regionsContainer{};
    auto xeMemUsageData = reinterpret_cast<drm_xe_query_mem_usage *>(memUsageData.data());
    auto xeGtsData = reinterpret_cast<drm_xe_query_gts *>(gtsData.data());

    std::array<drm_xe_query_mem_usage::drm_xe_query_mem_region *, 64> memoryRegionInstances{};

    for (auto i = 0u; i < xeMemUsageData->num_regions; i++) {
        auto &region = xeMemUsageData->regions[i];
        memoryRegionInstances[region.instance] = &region;
        if (region.mem_class == XE_MEM_REGION_CLASS_SYSMEM) {
            regionsContainer.push_back(createMemoryRegionFromXeMemRegion(region));
        }
    }

    if (regionsContainer.empty()) {
        return {};
    }

    for (auto i = 0u; i < xeGtsData->num_gt; i++) {
        uint64_t nativeMemRegions = xeGtsData->gts[i].native_mem_regions;
        auto regionIndex = Math::log2(nativeMemRegions);
        UNRECOVERABLE_IF(!memoryRegionInstances[regionIndex]);
        regionsContainer.push_back(createMemoryRegionFromXeMemRegion(*memoryRegionInstances[regionIndex]));

        xeTimestampFrequency = xeGtsData->gts[i].clock_freq;
    }
    return std::make_unique<MemoryInfo>(regionsContainer, drm);
}

int IoctlHelperXe::createGemExt(const MemRegionsVec &memClassInstances, size_t allocSize, uint32_t &handle, std::optional<uint32_t> vmId, int32_t pairHandle) {
    struct drm_xe_gem_create create = {};
    uint32_t regionsSize = static_cast<uint32_t>(memClassInstances.size());

    if (!regionsSize) {
        xeLog("memClassInstances empty !\n", "");
        return -1;
    }

    if (vmId != std::nullopt) {
        create.vm_id = vmId.value();
    }

    create.size = allocSize;
    MemoryClassInstance mem = memClassInstances[regionsSize - 1];
    std::bitset<32> memoryInstances{};
    for (const auto &memoryClassInstance : memClassInstances) {
        memoryInstances.set(memoryClassInstance.memoryInstance);
    }
    create.flags = static_cast<uint32_t>(memoryInstances.to_ulong());

    auto ret = IoctlHelper::ioctl(DrmIoctl::GemCreate, &create);
    handle = create.handle;

    xeLog(" -> IoctlHelperXe::%s [%d,%d] vmid=0x%x s=0x%lx f=0x%x h=0x%x r=%d\n", __FUNCTION__,
          mem.memoryClass, mem.memoryInstance,
          create.vm_id, create.size, create.flags, handle, ret);

    {
        std::unique_lock<std::mutex> lock(xeLock);
        BindInfo b = {create.handle, 0, 0, create.size};
        bindInfo.push_back(b);
    }

    return ret;
}

CacheRegion IoctlHelperXe::closAlloc() {
    xeLog(" -> IoctlHelperXe::%s\n", __FUNCTION__);
    return CacheRegion::None;
}

uint16_t IoctlHelperXe::closAllocWays(CacheRegion closIndex, uint16_t cacheLevel, uint16_t numWays) {
    xeLog(" -> IoctlHelperXe::%s\n", __FUNCTION__);
    return 0;
}

CacheRegion IoctlHelperXe::closFree(CacheRegion closIndex) {
    xeLog(" -> IoctlHelperXe::%s\n", __FUNCTION__);
    return CacheRegion::None;
}

int IoctlHelperXe::xeWaitUserFence(uint64_t mask, uint16_t op, uint64_t addr, uint64_t value,
                                   struct drm_xe_engine_class_instance *eci,
                                   int64_t timeout) {
    struct drm_xe_wait_user_fence wait = {};
    wait.addr = addr;
    wait.op = op;
    wait.flags = !eci ? DRM_XE_UFENCE_WAIT_SOFT_OP : 0;
    wait.value = value;
    wait.mask = mask;
    wait.timeout = timeout;
    wait.num_engines = eci ? 1 : 0;
    wait.instances = eci ? castToUint64(eci) : 0;
    auto retVal = IoctlHelper::ioctl(DrmIoctl::GemWaitUserFence, &wait);
    xeLog(" -> IoctlHelperXe::%s a=0x%llx v=0x%llx engine=[0x%x, 0x%x] T=0x%llx F=0x%x retVal=0x%x\n", __FUNCTION__, addr, value,
          eci ? eci->engine_class : -1, eci ? eci->engine_instance : -1,
          timeout, wait.flags, retVal);
    return retVal;
}

int IoctlHelperXe::waitUserFence(uint32_t ctxId, uint64_t address,
                                 uint64_t value, uint32_t dataWidth, int64_t timeout, uint16_t flags) {
    xeLog(" -> IoctlHelperXe::%s a=0x%llx v=0x%llx w=0x%x T=0x%llx F=0x%x\n", __FUNCTION__, address, value, dataWidth, timeout, flags);
    uint64_t mask;
    switch (dataWidth) {
    case static_cast<uint32_t>(Drm::ValueWidth::U64):
        mask = DRM_XE_UFENCE_WAIT_U64;
        break;
    case static_cast<uint32_t>(Drm::ValueWidth::U32):
        mask = DRM_XE_UFENCE_WAIT_U32;
        break;
    case static_cast<uint32_t>(Drm::ValueWidth::U16):
        mask = DRM_XE_UFENCE_WAIT_U16;
        break;
    default:
        mask = DRM_XE_UFENCE_WAIT_U8;
        break;
    }
    if (timeout == -1) {
        /* expected in i915 but not in xe where timeout is an unsigned long */
        timeout = TimeoutControls::maxTimeout;
    }
    if (address) {
        return xeWaitUserFence(mask, DRM_XE_UFENCE_WAIT_GTE, address, value, NULL, timeout);
    }
    return 0;
}

uint32_t IoctlHelperXe::getAtomicAdvise(bool isNonAtomic) {
    xeLog(" -> IoctlHelperXe::%s\n", __FUNCTION__);
    return 0;
}

uint32_t IoctlHelperXe::getPreferredLocationAdvise() {
    xeLog(" -> IoctlHelperXe::%s\n", __FUNCTION__);
    return 0;
}

std::optional<MemoryClassInstance> IoctlHelperXe::getPreferredLocationRegion(PreferredLocation memoryLocation, uint32_t memoryInstance) {
    return std::nullopt;
}

bool IoctlHelperXe::setVmBoAdvise(int32_t handle, uint32_t attribute, void *region) {
    xeLog(" -> IoctlHelperXe::%s\n", __FUNCTION__);
    return false;
}

bool IoctlHelperXe::setVmPrefetch(uint64_t start, uint64_t length, uint32_t region, uint32_t vmId) {
    xeLog(" -> IoctlHelperXe::%s\n", __FUNCTION__);
    return false;
}

uint32_t IoctlHelperXe::getDirectSubmissionFlag() {
    xeLog(" -> IoctlHelperXe::%s\n", __FUNCTION__);
    return 0;
}

uint16_t IoctlHelperXe::getWaitUserFenceSoftFlag() {
    xeLog(" -> IoctlHelperXe::%s\n", __FUNCTION__);
    return PRELIM_I915_UFENCE_WAIT_SOFT;
    ;
};

int IoctlHelperXe::execBuffer(ExecBuffer *execBuffer, uint64_t completionGpuAddress, TaskCountType counterValue) {
    xeLog(" -> IoctlHelperXe::%s\n", __FUNCTION__);
    int ret = 0;
    if (execBuffer) {
        drm_i915_gem_execbuffer2 *d = reinterpret_cast<drm_i915_gem_execbuffer2 *>(execBuffer->data);
        if (d) {
            drm_i915_gem_exec_object2 *obj = reinterpret_cast<drm_i915_gem_exec_object2
                                                                  *>(d->buffers_ptr);
            uint32_t engine = static_cast<uint32_t>(d->rsvd1);
            if (obj) {

                xeLog("EXEC bc=%d ofs=%d len=%d f=0x%llx ctx=0x%x ptr=0x%llx r=0x%x\n",
                      d->buffer_count, d->batch_start_offset, d->batch_len,
                      d->flags, engine, obj->offset, ret);

                xeLog(" -> IoctlHelperXe::%s CA=0x%llx v=0x%x ctx=0x%x\n", __FUNCTION__,
                      completionGpuAddress, counterValue, engine);

                struct drm_xe_sync sync[1] = {};
                sync[0].flags = DRM_XE_SYNC_USER_FENCE | DRM_XE_SYNC_SIGNAL;
                sync[0].addr = completionGpuAddress;
                sync[0].timeline_value = counterValue;
                struct drm_xe_exec exec = {};

                exec.engine_id = engine;
                exec.num_syncs = 1;
                exec.syncs = reinterpret_cast<uintptr_t>(&sync);
                exec.address = obj->offset + d->batch_start_offset;
                exec.num_batch_buffer = 1;

                ret = IoctlHelper::ioctl(DrmIoctl::GemExecbuffer2, &exec);
                xeLog("r=0x%x batch=0x%lx\n", ret, exec.address);

                if (DebugManager.flags.PrintCompletionFenceUsage.get()) {
                    std::cout << "Completion fence submitted."
                              << " GPU address: " << std::hex << completionGpuAddress << std::dec
                              << ", value: " << counterValue << std::endl;
                }
            }
        }
    }
    return ret;
}

bool IoctlHelperXe::completionFenceExtensionSupported(const bool isVmBindAvailable) {
    xeLog(" -> IoctlHelperXe::%s\n", __FUNCTION__);
    return isVmBindAvailable;
}

std::unique_ptr<uint8_t[]> IoctlHelperXe::prepareVmBindExt(const StackVec<uint32_t, 2> &bindExtHandles) {
    xeLog(" -> IoctlHelperXe::%s\n", __FUNCTION__);
    return {};
}

uint64_t IoctlHelperXe::getFlagsForVmBind(bool bindCapture, bool bindImmediate, bool bindMakeResident) {
    uint64_t ret = 0;
    xeLog(" -> IoctlHelperXe::%s %d %d %d\n", __FUNCTION__, bindCapture, bindImmediate, bindMakeResident);
    if (bindCapture) {
        ret |= XE_NEO_BIND_CAPTURE_FLAG;
    }
    if (bindImmediate) {
        ret |= XE_NEO_BIND_IMMEDIATE_FLAG;
    }
    if (bindMakeResident) {
        ret |= XE_NEO_BIND_MAKERESIDENT_FLAG;
    }
    return ret;
}

int IoctlHelperXe::queryDistances(std::vector<QueryItem> &queryItems, std::vector<DistanceInfo> &distanceInfos) {
    xeLog(" -> IoctlHelperXe::%s\n", __FUNCTION__);
    return 0;
}

std::optional<DrmParam> IoctlHelperXe::getHasPageFaultParamId() {
    xeLog(" -> IoctlHelperXe::%s\n", __FUNCTION__);
    return {};
};

bool IoctlHelperXe::getEuStallProperties(std::array<uint64_t, 12u> &properties, uint64_t dssBufferSize, uint64_t samplingRate,
                                         uint64_t pollPeriod, uint64_t engineInstance, uint64_t notifyNReports) {
    xeLog(" -> IoctlHelperXe::%s\n", __FUNCTION__);
    return false;
}

uint32_t IoctlHelperXe::getEuStallFdParameter() {
    xeLog(" -> IoctlHelperXe::%s\n", __FUNCTION__);
    return 0;
}

std::unique_ptr<uint8_t[]> IoctlHelperXe::createVmControlExtRegion(const std::optional<MemoryClassInstance> &regionInstanceClass) {
    xeLog(" -> IoctlHelperXe::%s\n", __FUNCTION__);
    return {};
}

uint32_t IoctlHelperXe::getFlagsForVmCreate(bool disableScratch, bool enablePageFault, bool useVmBind) {
    xeLog(" -> IoctlHelperXe::%s %d,%d,%d\n", __FUNCTION__, disableScratch, enablePageFault, useVmBind);
    uint32_t flags = 0u;
    if (disableScratch) {
        flags |= XE_NEO_VMCREATE_DISABLESCRATCH_FLAG;
    }
    if (enablePageFault) {
        flags |= XE_NEO_VMCREATE_ENABLEPAGEFAULT_FLAG;
    }
    if (useVmBind) {
        flags |= XE_NEO_VMCREATE_USEVMBIND_FLAG;
    }
    return flags;
}

uint32_t IoctlHelperXe::createContextWithAccessCounters(GemContextCreateExt &gcc) {
    xeLog(" -> IoctlHelperXe::%s\n", __FUNCTION__);
    return 0;
}

uint32_t IoctlHelperXe::createCooperativeContext(GemContextCreateExt &gcc) {
    xeLog(" -> IoctlHelperXe::%s\n", __FUNCTION__);
    return 0;
}

void IoctlHelperXe::fillVmBindExtSetPat(VmBindExtSetPatT &vmBindExtSetPat, uint64_t patIndex, uint64_t nextExtension) {
    xeLog(" -> IoctlHelperXe::%s\n", __FUNCTION__);
}

void IoctlHelperXe::fillVmBindExtUserFence(VmBindExtUserFenceT &vmBindExtUserFence, uint64_t fenceAddress, uint64_t fenceValue, uint64_t nextExtension) {
    xeLog(" -> IoctlHelperXe::%s 0x%lx 0x%lx\n", __FUNCTION__, fenceAddress, fenceValue);
    auto xeBindExtUserFence = reinterpret_cast<UserFenceExtension *>(vmBindExtUserFence);
    UNRECOVERABLE_IF(!xeBindExtUserFence);
    xeBindExtUserFence->tag = UserFenceExtension::tagValue;
    xeBindExtUserFence->addr = fenceAddress;
    xeBindExtUserFence->value = fenceValue;
}

std::optional<uint64_t> IoctlHelperXe::getCopyClassSaturatePCIECapability() {
    xeLog(" -> IoctlHelperXe::%s\n", __FUNCTION__);
    return {};
}

std::optional<uint64_t> IoctlHelperXe::getCopyClassSaturateLinkCapability() {
    xeLog(" -> IoctlHelperXe::%s\n", __FUNCTION__);
    return {};
}

uint32_t IoctlHelperXe::getVmAdviseAtomicAttribute() {
    xeLog(" -> IoctlHelperXe::%s\n", __FUNCTION__);
    return 0;
}

int IoctlHelperXe::vmBind(const VmBindParams &vmBindParams) {
    return xeVmBind(vmBindParams, true);
}

int IoctlHelperXe::vmUnbind(const VmBindParams &vmBindParams) {
    return xeVmBind(vmBindParams, false);
}

UuidRegisterResult IoctlHelperXe::registerUuid(const std::string &uuid, uint32_t uuidClass, uint64_t ptr, uint64_t size) {
    xeLog(" -> IoctlHelperXe::%s\n", __FUNCTION__);
    return {};
}

UuidRegisterResult IoctlHelperXe::registerStringClassUuid(const std::string &uuid, uint64_t ptr, uint64_t size) {
    xeLog(" -> IoctlHelperXe::%s\n", __FUNCTION__);
    return {};
}

int IoctlHelperXe::unregisterUuid(uint32_t handle) {
    xeLog(" -> IoctlHelperXe::%s\n", __FUNCTION__);
    return 0;
}

bool IoctlHelperXe::isContextDebugSupported() {
    return false;
}

int IoctlHelperXe::setContextDebugFlag(uint32_t drmContextId) {
    return 0;
}

bool IoctlHelperXe::isDebugAttachAvailable() {
    return false;
}

unsigned int IoctlHelperXe::getIoctlRequestValue(DrmIoctl ioctlRequest) const {
    xeLog(" -> IoctlHelperXe::%s 0x%x\n", __FUNCTION__, ioctlRequest);
    switch (ioctlRequest) {
    case DrmIoctl::SyncobjCreate:
        RETURN_ME(DRM_IOCTL_SYNCOBJ_CREATE);
    case DrmIoctl::SyncobjWait:
        RETURN_ME(DRM_IOCTL_SYNCOBJ_WAIT);
    case DrmIoctl::SyncobjDestroy:
        RETURN_ME(DRM_IOCTL_SYNCOBJ_DESTROY);
    case DrmIoctl::GemClose:
        RETURN_ME(DRM_IOCTL_GEM_CLOSE);
    case DrmIoctl::GemVmCreate:
        RETURN_ME(DRM_IOCTL_XE_VM_CREATE);
    case DrmIoctl::GemVmDestroy:
        RETURN_ME(DRM_IOCTL_XE_VM_DESTROY);
    case DrmIoctl::GemMmapOffset:
        RETURN_ME(DRM_IOCTL_XE_GEM_MMAP_OFFSET);
    case DrmIoctl::GemCreate:
        RETURN_ME(DRM_IOCTL_XE_GEM_CREATE);
    case DrmIoctl::GemExecbuffer2:
        RETURN_ME(DRM_IOCTL_XE_EXEC);
    case DrmIoctl::GemVmBind:
        RETURN_ME(DRM_IOCTL_XE_VM_BIND);
    case DrmIoctl::Query:
        RETURN_ME(DRM_IOCTL_XE_DEVICE_QUERY);
    case DrmIoctl::GemContextCreateExt:
        RETURN_ME(DRM_IOCTL_XE_ENGINE_CREATE);
    case DrmIoctl::GemContextDestroy:
        RETURN_ME(DRM_IOCTL_XE_ENGINE_DESTROY);
    case DrmIoctl::GemWaitUserFence:
        RETURN_ME(DRM_IOCTL_XE_WAIT_USER_FENCE);
    case DrmIoctl::PrimeFdToHandle:
        RETURN_ME(DRM_IOCTL_PRIME_FD_TO_HANDLE);
    case DrmIoctl::PrimeHandleToFd:
        RETURN_ME(DRM_IOCTL_PRIME_HANDLE_TO_FD);
    case DrmIoctl::RegRead:
        RETURN_ME(DRM_IOCTL_XE_MMIO);
    default:
        UNRECOVERABLE_IF(true);
        return 0;
    }
}

int IoctlHelperXe::getDrmParamValue(DrmParam drmParam) const {
    xeLog(" -> IoctlHelperXe::%s 0x%x %s\n", __FUNCTION__, drmParam, getDrmParamString(drmParam).c_str());

    switch (drmParam) {
    case DrmParam::MemoryClassDevice:
        return I915_MEMORY_CLASS_DEVICE;
    case DrmParam::MemoryClassSystem:
        return I915_MEMORY_CLASS_SYSTEM;

    case DrmParam::EngineClassRender:
        return drm_i915_gem_engine_class::I915_ENGINE_CLASS_RENDER;
    case DrmParam::EngineClassCopy:
        return drm_i915_gem_engine_class::I915_ENGINE_CLASS_COPY;
    case DrmParam::EngineClassVideo:
        return drm_i915_gem_engine_class::I915_ENGINE_CLASS_VIDEO;
    case DrmParam::EngineClassVideoEnhance:
        return drm_i915_gem_engine_class::I915_ENGINE_CLASS_VIDEO_ENHANCE;
    case DrmParam::EngineClassCompute:
        return prelim_drm_i915_gem_engine_class::PRELIM_I915_ENGINE_CLASS_COMPUTE;
    case DrmParam::EngineClassInvalid:
        return drm_i915_gem_engine_class::I915_ENGINE_CLASS_INVALID;

    default:
        return getDrmParamValueBase(drmParam);
    }
}

int IoctlHelperXe::getDrmParamValueBase(DrmParam drmParam) const {
    return static_cast<int>(drmParam);
}

template <typename... XeLogArgs>
void IoctlHelperXe::xeLog(XeLogArgs &&...args) const {
    PRINT_DEBUG_STRING(DebugManager.flags.PrintDebugMessages.get(), stderr, args...);
}

std::string IoctlHelperXe::getIoctlString(DrmIoctl ioctlRequest) const {
    switch (ioctlRequest) {
    case DrmIoctl::SyncobjCreate:
        STRINGIFY_ME(DRM_IOCTL_SYNCOBJ_CREATE);
    case DrmIoctl::SyncobjWait:
        STRINGIFY_ME(DRM_IOCTL_SYNCOBJ_WAIT);
    case DrmIoctl::SyncobjDestroy:
        STRINGIFY_ME(DRM_IOCTL_SYNCOBJ_DESTROY);
    case DrmIoctl::GemClose:
        STRINGIFY_ME(DRM_IOCTL_GEM_CLOSE);
    case DrmIoctl::GemVmCreate:
        STRINGIFY_ME(DRM_IOCTL_XE_VM_CREATE);
    case DrmIoctl::GemVmDestroy:
        STRINGIFY_ME(DRM_IOCTL_XE_VM_DESTROY);
    case DrmIoctl::GemMmapOffset:
        STRINGIFY_ME(DRM_IOCTL_XE_GEM_MMAP_OFFSET);
    case DrmIoctl::GemCreate:
        STRINGIFY_ME(DRM_IOCTL_XE_GEM_CREATE);
    case DrmIoctl::GemExecbuffer2:
        STRINGIFY_ME(DRM_IOCTL_XE_EXEC);
    case DrmIoctl::GemVmBind:
        STRINGIFY_ME(DRM_IOCTL_XE_VM_BIND);
    case DrmIoctl::Query:
        STRINGIFY_ME(DRM_IOCTL_XE_DEVICE_QUERY);
    case DrmIoctl::GemContextCreateExt:
        STRINGIFY_ME(DRM_IOCTL_XE_ENGINE_CREATE);
    case DrmIoctl::GemContextDestroy:
        STRINGIFY_ME(DRM_IOCTL_XE_ENGINE_DESTROY);
    case DrmIoctl::GemWaitUserFence:
        STRINGIFY_ME(DRM_IOCTL_XE_WAIT_USER_FENCE);
    case DrmIoctl::PrimeFdToHandle:
        STRINGIFY_ME(DRM_IOCTL_PRIME_FD_TO_HANDLE);
    case DrmIoctl::PrimeHandleToFd:
        STRINGIFY_ME(DRM_IOCTL_PRIME_HANDLE_TO_FD);
    case DrmIoctl::RegRead:
        STRINGIFY_ME(DRM_IOCTL_XE_MMIO);
    default:
        return "???";
    }
}

uint32_t IoctlHelperXe::xeSyncObjCreate(uint32_t flags) {
    int ret;
    struct drm_syncobj_create create = {};
    create.flags = flags;
    ret = IoctlHelper::ioctl(DrmIoctl::SyncobjCreate, &create);
    UNRECOVERABLE_IF(ret);
    return create.handle;
}

bool IoctlHelperXe::xeSyncObjWait(uint32_t *handles, uint32_t count,
                                  uint64_t absTimeoutNsec, uint32_t flags,
                                  uint32_t *firstSignaled) {
    int ret;
    struct drm_syncobj_wait wait = {};
    wait.handles = castToUint64(handles);
    wait.timeout_nsec = absTimeoutNsec;
    wait.count_handles = count;
    wait.flags = flags;
    ret = IoctlHelper::ioctl(DrmIoctl::SyncobjWait, &wait);
    if (ret && errno == ETIME) {
        return false;
    }
    UNRECOVERABLE_IF(ret);
    if (firstSignaled)
        *firstSignaled = wait.first_signaled;
    return true;
}

void IoctlHelperXe::xeSyncObjDestroy(uint32_t handle) {
    int ret;
    struct drm_syncobj_destroy destroy = {};
    destroy.handle = handle;
    ret = IoctlHelper::ioctl(DrmIoctl::SyncobjDestroy, &destroy);
    UNRECOVERABLE_IF(ret);
}

uint64_t IoctlHelperXe::xeDecanonize(uint64_t address) {
    return (address & maxNBitValue(addressWidth));
}

int IoctlHelperXe::ioctl(DrmIoctl request, void *arg) {
    int ret = -1;
    xeLog(" => IoctlHelperXe::%s 0x%x\n", __FUNCTION__, request);
    switch (request) {
    case DrmIoctl::Getparam: {
        struct GetParam *d = (struct GetParam *)arg;
        ret = 0;
        switch (d->param) {
        case static_cast<int>(DrmParam::ParamChipsetId):
            *d->value = chipsetId;
            break;
        case static_cast<int>(DrmParam::ParamRevision):
            *d->value = revId;
            break;
        case static_cast<int>(DrmParam::ParamHasPageFault):
            *d->value = 0;
            break;
        case static_cast<int>(DrmParam::ParamHasExecSoftpin):
            *d->value = 1;
            break;
        case static_cast<int>(DrmParam::ParamHasScheduler):
            *d->value = static_cast<int>(0x80000037);
            break;
        case static_cast<int>(DrmParam::ParamCsTimestampFrequency):
            *d->value = static_cast<int>(xeTimestampFrequency);
            break;
        default:
            ret = -1;
        }
        xeLog(" -> IoctlHelperXe::ioctl Getparam 0x%x/0x%x r=%d\n", d->param, *d->value, ret);
    } break;

    case DrmIoctl::Query: {

        Query *q = static_cast<Query *>(arg);
        ret = xeGetQuery(q);
        if (ret == 0) {
            QueryItem *queryItem = reinterpret_cast<QueryItem *>(q->itemsPtr);
            xeLog(" -> IoctlHelperXe::ioctl Query id=0x%x f=0x%x len=%d r=%d\n",
                  static_cast<int>(queryItem->queryId), static_cast<int>(queryItem->flags), queryItem->length, ret);

        } else {
            xeLog(" -> IoctlHelperXe::ioctl Query r=%d\n", ret);
        }
    } break;
    case DrmIoctl::GemUserptr: {
        GemUserPtr *d = static_cast<GemUserPtr *>(arg);
        d->handle = userPtrHandle++ | XE_USERPTR_FAKE_FLAG;
        {
            std::unique_lock<std::mutex> lock(xeLock);
            BindInfo b = {d->handle, d->userPtr, 0, d->userSize};
            bindInfo.push_back(b);
        }
        ret = 0;
        xeLog(" -> IoctlHelperXe::ioctl GemUserptrGemUserptr p=0x%llx s=0x%llx f=0x%x h=0x%x r=%d\n", d->userPtr,
              d->userSize, d->flags, d->handle, ret);
        xeShowBindTable();
    } break;
    case DrmIoctl::GemContextCreateExt: {
        UNRECOVERABLE_IF(true);
    } break;
    case DrmIoctl::GemContextDestroy: {
        GemContextDestroy *d = static_cast<GemContextDestroy *>(arg);
        struct drm_xe_engine_destroy destroy = {};
        destroy.engine_id = d->contextId;
        if (d->contextId != 0xffffffff)
            ret = IoctlHelper::ioctl(request, &destroy);
        else
            ret = 0;
        xeLog(" -> IoctlHelperXe::ioctl GemContextDestroryExt ctx=0x%x r=%d\n",
              d->contextId, ret);
    } break;
    case DrmIoctl::GemContextGetparam: {
        GemContextParam *d = static_cast<GemContextParam *>(arg);

        ret = 0;
        switch (d->param) {
        case static_cast<int>(DrmParam::ContextParamGttSize):
            d->value = 0x1ull << addressWidth;
            break;
        case static_cast<int>(DrmParam::ContextParamSseu):
            d->value = 0x55fdd94d4e40;
            break;
        case static_cast<int>(DrmParam::ContextParamPersistence):
            d->value = 0x1;
            break;
        default:
            ret = -1;
            break;
        }
        xeLog(" -> IoctlHelperXe::ioctl GemContextGetparam r=%d\n", ret);
    } break;
    case DrmIoctl::GemContextSetparam: {
        GemContextParam *d = static_cast<GemContextParam *>(arg);
        switch (d->param) {
        case static_cast<int>(DrmParam::ContextParamPersistence):
            if (d->value == 0)
                ret = 0;
            break;
        case static_cast<int>(DrmParam::ContextParamEngines): {
            i915_context_param_engines *contextEngine = reinterpret_cast<i915_context_param_engines *>(d->value);
            int items = (d->size - sizeof(uint64_t)) / sizeof(uint32_t);
            contextParamEngine.clear();
            if (items < 11) {
                for (int i = 0; i < items; i++) {
                    drm_xe_engine_class_instance engine = {
                        contextEngine->engines[i].engine_class,
                        contextEngine->engines[i].engine_instance,
                        0};
                    if (engine.engine_class != 65535)
                        contextParamEngine.push_back(engine);
                }
            }
            if (contextParamEngine.size())
                ret = 0;
        } break;
        case contextPrivateParamBoost:
            ret = 0;
            break;
        default:
            ret = -1;
            break;
        }
        xeLog(" -> IoctlHelperXe::ioctl GemContextSetparam r=%d\n", ret);
    } break;
    case DrmIoctl::GemClose: {
        struct GemClose *d = static_cast<struct GemClose *>(arg);
        int found = -1;
        xeShowBindTable();
        for (unsigned int i = 0; i < bindInfo.size(); i++) {
            if (d->handle == bindInfo[i].handle) {
                found = i;
                break;
            }
        }
        if (found != -1) {
            xeLog(" removing %d: 0x%x 0x%lx 0x%lx\n",
                  found,
                  bindInfo[found].handle,
                  bindInfo[found].userptr,
                  bindInfo[found].addr);
            {
                std::unique_lock<std::mutex> lock(xeLock);
                bindInfo.erase(bindInfo.begin() + found);
            }
            if (d->handle & XE_USERPTR_FAKE_FLAG) {
                // nothing to do under XE
                ret = 0;
            } else {
                ret = IoctlHelper::ioctl(request, arg);
            }
        } else {
            ret = 0; // let it pass trough for now
        }
        xeLog(" -> IoctlHelperXe::ioctl GemClose found=%d h=0x%x r=%d\n", found, d->handle, ret);
    } break;
    case DrmIoctl::RegRead: {
        struct drm_xe_mmio mmio = {};
        RegisterRead *reg = static_cast<RegisterRead *>(arg);
        mmio.addr = static_cast<uint32_t>(reg->offset);
        if (reg->offset == (REG_GLOBAL_TIMESTAMP_LDW | 1)) {
            mmio.addr = REG_GLOBAL_TIMESTAMP_LDW;
        }
        mmio.flags = DRM_XE_MMIO_READ | DRM_XE_MMIO_64BIT;
        ret = IoctlHelper::ioctl(request, &mmio);
        reg->value = mmio.value;
        xeLog(" -> IoctlHelperXe::ioctl RegRead 0x%lx/0x%lx r=%d\n",
              reg->offset, reg->value, ret);
    } break;
    case DrmIoctl::GemVmCreate: {
        GemVmControl *d = static_cast<GemVmControl *>(arg);
        struct drm_xe_vm_create args = {};
        args.flags = DRM_XE_VM_CREATE_ASYNC_BIND_OPS |
                     DRM_XE_VM_CREATE_COMPUTE_MODE;
        ret = IoctlHelper::ioctl(request, &args);
        d->vmId = ret ? 0 : args.vm_id;
        xeVmId = d->vmId;
        xeLog(" -> IoctlHelperXe::ioctl GemVmCreate vmid=0x%x r=%d\n", d->vmId, ret);

    } break;
    case DrmIoctl::GemVmDestroy: {
        GemVmControl *d = static_cast<GemVmControl *>(arg);
        struct drm_xe_vm_destroy args = {};
        args.vm_id = d->vmId;
        ret = IoctlHelper::ioctl(request, &args);
        xeLog(" -> IoctlHelperXe::ioctl GemVmDestroy vmid=0x%x r=%d\n", d->vmId, ret);

    } break;

    case DrmIoctl::GemMmapOffset: {
        GemMmapOffset *d = static_cast<GemMmapOffset *>(arg);
        struct drm_xe_gem_mmap_offset mmo = {};
        mmo.handle = d->handle;
        ret = IoctlHelper::ioctl(request, &mmo);
        d->offset = mmo.offset;
        xeLog(" -> IoctlHelperXe::ioctl GemMmapOffset h=0x%x o=0x%x r=%d\n",
              d->handle, d->offset, ret);
    } break;
    case DrmIoctl::GetResetStats: {
        ResetStats *d = static_cast<ResetStats *>(arg);
        //    d->batchActive = 1; // fake gpu hang
        ret = 0;
        xeLog(" -> IoctlHelperXe::ioctl GetResetStats ctx=0x%x r=%d\n",
              d->contextId, ret);
    } break;
    case DrmIoctl::PrimeFdToHandle: {
        PrimeHandle *prime = static_cast<PrimeHandle *>(arg);
        ret = IoctlHelper::ioctl(request, arg);
        xeLog(" ->PrimeFdToHandle  h=0x%x f=0x%x d=0x%x r=%d\n",
              prime->handle, prime->flags, prime->fileDescriptor, ret);
    } break;
    case DrmIoctl::PrimeHandleToFd: {
        PrimeHandle *prime = static_cast<PrimeHandle *>(arg);
        xeLog(" ->PrimeHandleToFd h=0x%x f=0x%x d=0x%x r=%d\n",
              prime->handle, prime->flags, prime->fileDescriptor, ret);
        ret = IoctlHelper::ioctl(request, arg);
    } break;
    case DrmIoctl::GemCreate: {
        GemCreate *gemCreate = static_cast<GemCreate *>(arg);
        ret = IoctlHelper::ioctl(request, arg);
        xeLog(" -> IoctlHelperXe::ioctl GemCreate h=0x%x s=0x%llx r=%d\n",
              gemCreate->handle, gemCreate->size, ret);
    } break;
    default:
        xeLog("Not handled 0x%x\n", request);
        UNRECOVERABLE_IF(true);
    }

    return ret;
}

void IoctlHelperXe::xeShowBindTable() {
#if 1
    std::unique_lock<std::mutex> lock(xeLock);
    xeLog("show bind: (<index> <handle> <userptr> <addr> <size>)\n", "");
    for (unsigned int i = 0; i < bindInfo.size(); i++) {
        xeLog(" %3d x%08x x%016lx x%016lx x%016lx\n", i,
              bindInfo[i].handle,
              bindInfo[i].userptr,
              bindInfo[i].addr,
              bindInfo[i].size);
    }
#endif
}

int IoctlHelperXe::createDrmContext(Drm &drm, OsContextLinux &osContext, uint32_t drmVmId, uint32_t deviceIndex) {
    struct drm_xe_engine_create create = {};
    uint32_t drmContextId = 0;
    struct drm_xe_engine_class_instance *currentEngine = nullptr;
    std::vector<struct drm_xe_engine_class_instance> engine;
    int requestClass = 0;

    xeLog("createDrmContext VM=0x%x\n", drmVmId);
    auto engineFlag = drm.bindDrmContext(drmContextId, deviceIndex, osContext.getEngineType(), osContext.isEngineInstanced());
    switch (engineFlag) {
    case static_cast<int>(DrmParam::ExecRender):
        requestClass = DRM_XE_ENGINE_CLASS_RENDER;
        break;
    case static_cast<int>(DrmParam::ExecBlt):
        requestClass = DRM_XE_ENGINE_CLASS_COPY;
        break;
    case static_cast<int>(DrmParam::ExecDefault):
        requestClass = DRM_XE_ENGINE_CLASS_COMPUTE;
        break;
    default:
        xeLog("unexpected engineFlag=0x%x\n", engineFlag);
        UNRECOVERABLE_IF(true);
    }
    size_t n = contextParamEngine.size();
    create.vm_id = drmVmId;
    create.width = 1;
    if (n == 0) {
        currentEngine = xeFindMatchingEngine(requestClass, XE_FIND_INVALID_INSTANCE);
        if (currentEngine == nullptr) {
            xeLog("Unable to find engine %d\n", requestClass);
            UNRECOVERABLE_IF(true);
            return 0;
        }
        engine.push_back(*currentEngine);
    } else {
        for (size_t i = 0; i < n; i++) {
            currentEngine = xeFindMatchingEngine(contextParamEngine[i].engine_class,
                                                 contextParamEngine[i].engine_instance);
            if (currentEngine == nullptr) {
                xeLog("Unable to find engine %d:%d\n",
                      contextParamEngine[i].engine_class,
                      contextParamEngine[i].engine_instance);
                UNRECOVERABLE_IF(true);
                return 0;
            }
            engine.push_back(*currentEngine);
        }
    }
    if (engine.size() > 9) {
        xeLog("Too much instances...\n", "");
        UNRECOVERABLE_IF(true);
        return 0;
    }
    create.instances = castToUint64(engine.data());
    create.num_placements = engine.size();
    struct drm_xe_ext_engine_set_property ext = {};

    ext.base.name = XE_ENGINE_EXTENSION_SET_PROPERTY;
    ext.property = XE_ENGINE_PROPERTY_COMPUTE_MODE;
    ext.value = 1;
    create.extensions = castToUint64(&ext);
    int ret = IoctlHelper::ioctl(DrmIoctl::GemContextCreateExt, &create);
    drmContextId = create.engine_id;
    xeLog("%s:%d (%d) vmid=0x%x ctx=0x%x r=0x%x\n", xeGetClassName(engine[0].engine_class),
          engine[0].engine_instance, create.num_placements, drmVmId, drmContextId, ret);
    if (ret != 0) {
        UNRECOVERABLE_IF(true);
    }
    return drmContextId;
}

int IoctlHelperXe::xeVmBind(const VmBindParams &vmBindParams, bool bindOp) {
    int ret = -1;
    const char *operation = "unbind";
    if (bindOp) {
        operation = "bind";
    }
    int found = -1;
    if (bindOp) {
        for (unsigned int i = 0; i < bindInfo.size(); i++) {
            if (vmBindParams.handle == bindInfo[i].handle) {
                found = i;
                break;
            }
        }
    } else {
        uint64_t ad = xeDecanonize(vmBindParams.start);
        for (unsigned int i = 0; i < bindInfo.size(); i++) {
            if (ad == bindInfo[i].addr) {
                found = i;
                break;
            }
        }
    }
    if (found != -1) {
        uint32_t extraBindFlag = 0;
        struct drm_xe_sync sync[1] = {};
        sync[0].flags = DRM_XE_SYNC_USER_FENCE | DRM_XE_SYNC_SIGNAL;
        extraBindFlag = XE_VM_BIND_FLAG_ASYNC;
        auto xeBindExtUserFence = reinterpret_cast<UserFenceExtension *>(vmBindParams.extensions);
        UNRECOVERABLE_IF(!xeBindExtUserFence);
        UNRECOVERABLE_IF(xeBindExtUserFence->tag != UserFenceExtension::tagValue);
        sync[0].addr = xeBindExtUserFence->addr;
        sync[0].timeline_value = xeBindExtUserFence->value;

        struct drm_xe_vm_bind bind = {};
        bind.vm_id = vmBindParams.vmId;
        bind.num_binds = 1;
        bind.bind.obj = vmBindParams.handle;
        bind.bind.obj_offset = vmBindParams.offset;
        bind.bind.range = vmBindParams.length;
        bind.bind.addr = xeDecanonize(vmBindParams.start);
        bind.bind.op = XE_VM_BIND_OP_MAP;
        bind.num_syncs = 1;
        bind.syncs = reinterpret_cast<uintptr_t>(&sync);
        if (vmBindParams.handle & XE_USERPTR_FAKE_FLAG) {
            bind.bind.obj = 0;
            bind.bind.obj_offset = bindInfo[found].userptr;
            bind.bind.op = XE_VM_BIND_OP_MAP_USERPTR;
        }
        if (!bindOp) {
            bind.bind.op = XE_VM_BIND_OP_UNMAP;
            bind.bind.obj = 0;
            if (bindInfo[found].handle & XE_USERPTR_FAKE_FLAG) {
                bind.bind.obj_offset = bindInfo[found].userptr;
            }
        }
        bind.bind.op |= extraBindFlag;

        bindInfo[found].addr = bind.bind.addr;
        xeLog(" vm=%d obj=0x%x off=0x%llx range=0x%llx addr=0x%llx op=%d(%s) nsy=%d\n",
              bind.vm_id,
              bind.bind.obj,
              bind.bind.obj_offset,
              bind.bind.range,
              bind.bind.addr,
              bind.bind.op,
              xeGetBindOpName(bind.bind.op),
              bind.num_syncs);
        ret = IoctlHelper::ioctl(DrmIoctl::GemVmBind, &bind);
        if (ret != 0) {
            return ret;
        }

        return xeWaitUserFence(DRM_XE_UFENCE_WAIT_U64, DRM_XE_UFENCE_WAIT_EQ,
                               sync[0].addr,
                               sync[0].timeline_value, NULL, XE_ONE_SEC);
    }

    xeLog(" -> IoctlHelperXe::%s %s found=%d vmid=0x%x h=0x%x s=0x%llx o=0x%llx l=0x%llx f=0x%llx r=%d\n",
          __FUNCTION__, operation, found, vmBindParams.vmId,
          vmBindParams.handle, vmBindParams.start, vmBindParams.offset,
          vmBindParams.length, vmBindParams.flags, ret);

    return ret;
}

std::string IoctlHelperXe::getDrmParamString(DrmParam drmParam) const {
    switch (drmParam) {
    case DrmParam::ContextCreateExtSetparam:
        return "ContextCreateExtSetparam";
    case DrmParam::ContextCreateFlagsUseExtensions:
        return "ContextCreateFlagsUseExtensions";
    case DrmParam::ContextEnginesExtLoadBalance:
        return "ContextEnginesExtLoadBalance";
    case DrmParam::ContextParamEngines:
        return "ContextParamEngines";
    case DrmParam::ContextParamGttSize:
        return "ContextParamGttSize";
    case DrmParam::ContextParamPersistence:
        return "ContextParamPersistence";
    case DrmParam::ContextParamPriority:
        return "ContextParamPriority";
    case DrmParam::ContextParamRecoverable:
        return "ContextParamRecoverable";
    case DrmParam::ContextParamSseu:
        return "ContextParamSseu";
    case DrmParam::ContextParamVm:
        return "ContextParamVm";
    case DrmParam::EngineClassRender:
        return "EngineClassRender";
    case DrmParam::EngineClassCompute:
        return "EngineClassCompute";
    case DrmParam::EngineClassCopy:
        return "EngineClassCopy";
    case DrmParam::EngineClassVideo:
        return "EngineClassVideo";
    case DrmParam::EngineClassVideoEnhance:
        return "EngineClassVideoEnhance";
    case DrmParam::EngineClassInvalid:
        return "EngineClassInvalid";
    case DrmParam::EngineClassInvalidNone:
        return "EngineClassInvalidNone";
    case DrmParam::ExecBlt:
        return "ExecBlt";
    case DrmParam::ExecDefault:
        return "ExecDefault";
    case DrmParam::ExecNoReloc:
        return "ExecNoReloc";
    case DrmParam::ExecRender:
        return "ExecRender";
    case DrmParam::MemoryClassDevice:
        return "MemoryClassDevice";
    case DrmParam::MemoryClassSystem:
        return "MemoryClassSystem";
    case DrmParam::MmapOffsetWb:
        return "MmapOffsetWb";
    case DrmParam::MmapOffsetWc:
        return "MmapOffsetWc";
    case DrmParam::ParamChipsetId:
        return "ParamChipsetId";
    case DrmParam::ParamRevision:
        return "ParamRevision";
    case DrmParam::ParamHasExecSoftpin:
        return "ParamHasExecSoftpin";
    case DrmParam::ParamHasPooledEu:
        return "ParamHasPooledEu";
    case DrmParam::ParamHasScheduler:
        return "ParamHasScheduler";
    case DrmParam::ParamEuTotal:
        return "ParamEuTotal";
    case DrmParam::ParamSubsliceTotal:
        return "ParamSubsliceTotal";
    case DrmParam::ParamMinEuInPool:
        return "ParamMinEuInPool";
    case DrmParam::ParamCsTimestampFrequency:
        return "ParamCsTimestampFrequency";
    case DrmParam::ParamHasVmBind:
        return "ParamHasVmBind";
    case DrmParam::ParamHasPageFault:
        return "ParamHasPageFault";
    case DrmParam::QueryEngineInfo:
        return "QueryEngineInfo";
    case DrmParam::QueryHwconfigTable:
        return "QueryHwconfigTable";
    case DrmParam::QueryComputeSlices:
        return "QueryComputeSlices";
    case DrmParam::QueryMemoryRegions:
        return "QueryMemoryRegions";
    case DrmParam::QueryTopologyInfo:
        return "QueryTopologyInfo";
    case DrmParam::SchedulerCapPreemption:
        return "SchedulerCapPreemption";
    case DrmParam::TilingNone:
        return "TilingNone";
    case DrmParam::TilingY:
        return "TilingY";
    default:
        return "DrmParam::<missing>";
    }
}

std::string IoctlHelperXe::getFileForMaxGpuFrequency() const {
    return "/device/gt0/freq_max";
}

std::string IoctlHelperXe::getFileForMaxGpuFrequencyOfSubDevice(int subDeviceId) const {
    return "/device/gt" + std::to_string(subDeviceId) + "/freq_max";
}

std::string IoctlHelperXe::getFileForMaxMemoryFrequencyOfSubDevice(int subDeviceId) const {
    return "/device/gt" + std::to_string(subDeviceId) + "/freq_rp0";
}

struct drm_xe_engine_class_instance *
IoctlHelperXe::xeFindMatchingEngine(uint16_t engineClass, uint16_t engineInstance) {
    for (auto &engine : allEngines) {
        if (engine.engine_class == engineClass &&
            (engineInstance == XE_FIND_INVALID_INSTANCE || engine.engine_instance == engineInstance)) {
            xeLog("\t select: %s:%d (%d)\n", xeGetClassName(engine.engine_class),
                  engine.engine_instance, engineInstance);
            return &engine;
        }
    }
    return nullptr;
}

bool IoctlHelperXe::getFabricLatency(uint32_t fabricId, uint32_t &latency, uint32_t &bandwidth) {
    return false;
}

bool IoctlHelperXe::isWaitBeforeBindRequired(bool bind) const {
    return true;
}

static uint32_t getVectorGetMax(std::vector<uint8_t> *data) {
    uint32_t ret = 0;
    for (uint32_t i = 0; i < static_cast<uint32_t>(data->size()); i++) {
        for (uint32_t j = 0; j < 8; j++) {
            if ((*data)[i] & (1 << j)) {
                ret = (j + 1 + (i * 8));
            }
        }
    }
    return ret;
}

static uint32_t isBitOn(std::vector<uint8_t> *data, uint32_t n) {
    if (n / 8 < static_cast<uint32_t>(data->size())) {
        return isBitSet((*data)[n / 8], n % 8);
    }
    return 0;
}

std::vector<uint8_t> IoctlHelperXe::xeRebuildi915Topology(std::vector<uint8_t> *geomDss,
                                                          std::vector<uint8_t> *computeDss,
                                                          std::vector<uint8_t> *euDss) {
    std::vector<uint8_t> ret;
    xeLog("GeomDss %ld %d\n", geomDss->size(), getVectorGetMax(geomDss));
    xeLog("ComputeDss %ld %d\n", computeDss->size(), getVectorGetMax(computeDss));
    xeLog("EuDss %ld %d\n", euDss->size(), getVectorGetMax(euDss));
    uint32_t maxEuPerDss = getVectorGetMax(euDss);
    uint32_t maxSubslice = getVectorGetMax(geomDss);
    std::vector<uint8_t> *currentDss = geomDss;
    if (!maxSubslice) {
        maxSubslice = getVectorGetMax(computeDss);
        currentDss = computeDss;
        if (!maxSubslice) {
            xeLog("incorrect number of slices !\n", "");
            return {};
        }
    }
    uint32_t ssStride = static_cast<uint32_t>(Math::divideAndRoundUp(maxSubslice, 8));
    uint32_t euStride = static_cast<uint32_t>(Math::divideAndRoundUp(maxEuPerDss, 8));
    uint32_t maxSlice = 1;
    uint32_t sliceLength = 1;
    uint32_t subsliceLength = maxSlice * ssStride;
    uint32_t euLength = maxSlice * maxSubslice * euStride;
    uint32_t totalLength = sizeof(struct drm_i915_query_topology_info) + sliceLength + subsliceLength + euLength;

    xeLog("maxSlice:%d maxSubslice:%d maxEuPerDss:%d euStride:%d ssStride:%d\n",
          maxSlice, maxSubslice, maxEuPerDss, euStride, ssStride);
    xeLog("subsliceLength:%d euLength:%d totalLength:%d total_eu:%d\n",
          subsliceLength, euLength, totalLength, maxEuPerDss * maxSubslice);

    {
        ret.resize(sizeof(struct drm_i915_query_topology_info));
        auto topology = reinterpret_cast<struct drm_i915_query_topology_info *>(ret.data());
        topology->max_slices = maxSlice;
        topology->max_subslices = maxSubslice;
        topology->max_eus_per_subslice = maxEuPerDss;
        topology->subslice_offset = sliceLength;
        topology->subslice_stride = ssStride;
        topology->eu_offset = sliceLength + subsliceLength;
        topology->eu_stride = euStride;
    }
    ret.push_back(maxSlice);
    for (uint32_t i = 0; i < ssStride; i++) {
        ret.push_back((*currentDss)[i]);
    }
    for (uint32_t i = 0; i < maxSubslice; i++) {
        if (isBitOn(currentDss, i)) {
            for (uint32_t j = 0; j < euStride; j++)
                ret.push_back((*euDss)[j]);
        } else {
            for (uint32_t j = 0; j < euStride; j++)
                ret.push_back(0);
        }
    }
    if (ret.size() != totalLength) {
        xeLog("Error while rebuilding i915 topology %ld %d\n", ret.size(), totalLength);
        return {};
    }
    return ret;
}

} // namespace NEO

/*
 * Copyright (C) 2018-2023 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 */

#pragma once
#include "shared/source/gmm_helper/gmm_lib.h"
#include "shared/source/os_interface/windows/hw_device_id.h"
#include "shared/source/os_interface/windows/sharedata_wrapper.h"
#include "shared/source/os_interface/windows/wddm/wddm_defs.h"

#include <atomic>

struct _SYSTEM_INFO;
typedef struct _SYSTEM_INFO SYSTEM_INFO;

namespace NEO {
enum PreemptionMode : uint32_t;
class Gdi;
class GfxPartition;
class Gmm;
class GmmMemory;
class OsContextWin;
class ProductHelper;
class SettingsReader;
class WddmAllocation;
class WddmInterface;
class WddmResidencyController;
class WddmResidencyLogger;
class WddmResidentAllocationsContainer;

struct AllocationStorageData;
struct FeatureTable;
struct HardwareInfo;
struct KmDafListener;
struct MonitoredFence;
struct OsHandleStorage;
struct OSMemory;
struct RootDeviceEnvironment;
struct WorkaroundTable;

enum class HeapIndex : uint32_t;

unsigned int readEnablePreemptionRegKey();
unsigned int getPid();
bool isShutdownInProgress();
CREATECONTEXT_PVTDATA initPrivateData(OsContextWin &osContext);

class Wddm : public DriverModel {
  public:
    static constexpr DriverModelType driverModelType = DriverModelType::WDDM;
    static constexpr std::uint64_t gpuHangIndication{std::numeric_limits<std::uint64_t>::max()};

    typedef HRESULT(WINAPI *CreateDXGIFactoryFcn)(REFIID riid, void **ppFactory);
    typedef HRESULT(WINAPI *DXCoreCreateAdapterFactoryFcn)(REFIID riid, void **ppFactory);
    typedef void(WINAPI *GetSystemInfoFcn)(SYSTEM_INFO *pSystemInfo);

    ~Wddm() override;

    static Wddm *createWddm(std::unique_ptr<HwDeviceIdWddm> &&hwDeviceId, RootDeviceEnvironment &rootDeviceEnvironment);
    bool init();

    MOCKABLE_VIRTUAL bool evict(const D3DKMT_HANDLE *handleList, uint32_t numOfHandles, uint64_t &sizeToTrim, bool evictNeeded);
    MOCKABLE_VIRTUAL bool makeResident(const D3DKMT_HANDLE *handles, uint32_t count, bool cantTrimFurther, uint64_t *numberOfBytesToTrim, size_t totalSize);
    MOCKABLE_VIRTUAL bool mapGpuVirtualAddress(Gmm *gmm, D3DKMT_HANDLE handle, D3DGPU_VIRTUAL_ADDRESS minimumAddress, D3DGPU_VIRTUAL_ADDRESS maximumAddress, D3DGPU_VIRTUAL_ADDRESS preferredAddress, D3DGPU_VIRTUAL_ADDRESS &gpuPtr);
    bool mapGpuVirtualAddress(AllocationStorageData *allocationStorageData);
    MOCKABLE_VIRTUAL NTSTATUS reserveGpuVirtualAddress(D3DGPU_VIRTUAL_ADDRESS baseAddress, D3DGPU_VIRTUAL_ADDRESS minimumAddress, D3DGPU_VIRTUAL_ADDRESS maximumAddress, D3DGPU_SIZE_T size, D3DGPU_VIRTUAL_ADDRESS *reservedAddress);
    MOCKABLE_VIRTUAL bool createContext(OsContextWin &osContext);
    MOCKABLE_VIRTUAL void applyAdditionalContextFlags(CREATECONTEXT_PVTDATA &privateData, OsContextWin &osContext);
    MOCKABLE_VIRTUAL void applyAdditionalMapGPUVAFields(D3DDDI_MAPGPUVIRTUALADDRESS &mapGPUVA, Gmm *gmm);
    MOCKABLE_VIRTUAL bool freeGpuVirtualAddress(D3DGPU_VIRTUAL_ADDRESS &gpuPtr, uint64_t size);
    MOCKABLE_VIRTUAL NTSTATUS createAllocation(const void *alignedCpuPtr, const Gmm *gmm, D3DKMT_HANDLE &outHandle, D3DKMT_HANDLE &outResourceHandle, uint64_t *outSharedHandle);
    MOCKABLE_VIRTUAL bool createAllocation(const Gmm *gmm, D3DKMT_HANDLE &outHandle);
    MOCKABLE_VIRTUAL NTSTATUS createAllocationsAndMapGpuVa(OsHandleStorage &osHandles);
    MOCKABLE_VIRTUAL bool destroyAllocations(const D3DKMT_HANDLE *handles, uint32_t allocationCount, D3DKMT_HANDLE resourceHandle);
    MOCKABLE_VIRTUAL bool verifySharedHandle(D3DKMT_HANDLE osHandle);
    MOCKABLE_VIRTUAL bool openSharedHandle(D3DKMT_HANDLE handle, WddmAllocation *alloc);
    MOCKABLE_VIRTUAL bool verifyNTHandle(HANDLE handle);
    bool openNTHandle(HANDLE handle, WddmAllocation *alloc);
    MOCKABLE_VIRTUAL void *lockResource(const D3DKMT_HANDLE &handle, bool applyMakeResidentPriorToLock, size_t size);
    MOCKABLE_VIRTUAL void unlockResource(const D3DKMT_HANDLE &handle);
    MOCKABLE_VIRTUAL void kmDafLock(D3DKMT_HANDLE handle);
    MOCKABLE_VIRTUAL bool isKmDafEnabled() const;

    MOCKABLE_VIRTUAL bool setAllocationPriority(const D3DKMT_HANDLE *handles, uint32_t allocationCount, uint32_t priority);

    MOCKABLE_VIRTUAL bool destroyContext(D3DKMT_HANDLE context);
    MOCKABLE_VIRTUAL bool queryAdapterInfo();
    MOCKABLE_VIRTUAL NTSTATUS createNTHandle(const D3DKMT_HANDLE *resourceHandle, HANDLE *ntHandle);

    MOCKABLE_VIRTUAL bool submit(uint64_t commandBuffer, size_t size, void *commandHeader, WddmSubmitArguments &submitArguments);
    MOCKABLE_VIRTUAL bool waitFromCpu(uint64_t lastFenceValue, const MonitoredFence &monitoredFence);

    MOCKABLE_VIRTUAL NTSTATUS escape(D3DKMT_ESCAPE &escapeCommand);
    MOCKABLE_VIRTUAL VOID *registerTrimCallback(PFND3DKMT_TRIMNOTIFICATIONCALLBACK callback, WddmResidencyController &residencyController);
    void unregisterTrimCallback(PFND3DKMT_TRIMNOTIFICATIONCALLBACK callback, VOID *trimCallbackHandle);
    MOCKABLE_VIRTUAL void releaseReservedAddress(void *reservedAddress);
    MOCKABLE_VIRTUAL bool reserveValidAddressRange(size_t size, void *&reservedMem);

    MOCKABLE_VIRTUAL void *virtualAlloc(void *inPtr, size_t size, bool topDownHint);
    MOCKABLE_VIRTUAL void virtualFree(void *ptr, size_t size);

    MOCKABLE_VIRTUAL bool isShutdownInProgress();
    MOCKABLE_VIRTUAL bool isDebugAttachAvailable();

    bool isGpuHangDetected(OsContext &osContext) override;

    bool configureDeviceAddressSpace();
    const FeatureTable &getFeatureTable() const {
        return *featureTable;
    }

    GT_SYSTEM_INFO *getGtSysInfo() const {
        DEBUG_BREAK_IF(!gtSystemInfo);
        return gtSystemInfo.get();
    }

    const GMM_GFX_PARTITIONING &getGfxPartition() const {
        return gfxPartition;
    }

    void initGfxPartition(GfxPartition &outGfxPartition, uint32_t rootDeviceIndex, size_t numRootDevices, bool useFrontWindowPool) const;

    const std::string &getDeviceRegistryPath() const {
        return deviceRegistryPath;
    }

    uint64_t getSystemSharedMemory() const;
    uint64_t getDedicatedVideoMemory() const;

    uint64_t getMaxApplicationAddress() const;

    HwDeviceIdWddm *getHwDeviceId() const {
        return hwDeviceId.get();
    }
    D3DKMT_HANDLE getAdapter() const { return hwDeviceId->getAdapter(); }
    D3DKMT_HANDLE getDeviceHandle() const override { return device; }
    D3DKMT_HANDLE getPagingQueue() const { return pagingQueue; }
    D3DKMT_HANDLE getPagingQueueSyncObject() const { return pagingQueueSyncObject; }
    Gdi *getGdi() const { return hwDeviceId->getGdi(); }
    MOCKABLE_VIRTUAL bool verifyAdapterLuid(LUID adapterLuid) const;
    LUID getAdapterLuid() const;

    PFND3DKMT_ESCAPE getEscapeHandle() const;

    uint32_t getHwContextId() const {
        return static_cast<uint32_t>(hwContextId);
    }

    uintptr_t getWddmMinAddress() const {
        return this->minAddress;
    }
    WddmInterface *getWddmInterface() const {
        return wddmInterface.get();
    }

    unsigned int getEnablePreemptionRegValue();
    MOCKABLE_VIRTUAL uint64_t *getPagingFenceAddress() {
        return pagingFenceAddress;
    }
    WddmResidentAllocationsContainer *getTemporaryResourcesContainer() {
        return temporaryResources.get();
    }
    void updatePagingFenceValue(uint64_t newPagingFenceValue);
    GmmMemory *getGmmMemory() const {
        return gmmMemory.get();
    }
    MOCKABLE_VIRTUAL void waitOnPagingFenceFromCpu();

    void setGmmInputArgs(void *args) override;

    WddmVersion getWddmVersion();
    static CreateDXGIFactoryFcn createDxgiFactory;
    static DXCoreCreateAdapterFactoryFcn dXCoreCreateAdapterFactory;

    uint32_t getRequestedEUCount() const;

    WddmResidencyLogger *getResidencyLogger() {
        return residencyLogger.get();
    }

    const RootDeviceEnvironment &getRootDeviceEnvironment() const { return rootDeviceEnvironment; }

    uint32_t getTimestampFrequency() const { return timestampFrequency; }

    PhysicalDevicePciBusInfo getPciBusInfo() const override;

    size_t getMaxMemAllocSize() const override;
    bool isDriverAvailable() override;

    static std::vector<std::unique_ptr<HwDeviceId>> discoverDevices(ExecutionEnvironment &executionEnvironment);

    ADAPTER_BDF getAdapterBDF() const {
        return adapterBDF;
    }

    PhysicalDevicePciSpeedInfo getPciSpeedInfo() const override;
    bool buildTopologyMapping();

    uint32_t getAdditionalAdapterInfoOptions() const {
        return additionalAdapterInfoOptions;
    }

  protected:
    bool translateTopologyInfo(TopologyMapping &mapping);

    Wddm(std::unique_ptr<HwDeviceIdWddm> &&hwDeviceId, RootDeviceEnvironment &rootDeviceEnvironment);
    MOCKABLE_VIRTUAL bool waitOnGPU(D3DKMT_HANDLE context);
    bool createDevice(PreemptionMode preemptionMode);
    bool createPagingQueue();
    bool destroyPagingQueue();
    bool destroyDevice();
    void getDeviceState();
    MOCKABLE_VIRTUAL void createPagingFenceLogger();
    bool setLowPriorityContextParam(D3DKMT_HANDLE contextHandle);
    bool adjustEvictNeededParameter(bool evictNeeded) {
        if (evictNeeded == false && platformSupportsEvictIfNecessary == false) {
            evictNeeded = true;
        }
        if (forceEvictOnlyIfNecessary != -1) {
            evictNeeded = !forceEvictOnlyIfNecessary;
        }
        return evictNeeded;
    }
    void setPlatformSupportEvictIfNecessaryFlag(const ProductHelper &productHelper);
    void populateAdditionalAdapterInfoOptions(const ADAPTER_INFO_KMD &adapterInfo);
    void populateIpVersion(HardwareInfo &hwInfo);

    GMM_GFX_PARTITIONING gfxPartition{};
    ADAPTER_BDF adapterBDF{};

    std::string deviceRegistryPath;

    std::atomic<std::uint64_t> currentPagingFenceValue{0};

    uint64_t systemSharedMemory = 0;
    uint64_t dedicatedVideoMemory = 0;

    // Adapter information
    std::unique_ptr<PLATFORM> gfxPlatform;
    std::unique_ptr<GT_SYSTEM_INFO> gtSystemInfo;
    std::unique_ptr<FeatureTable> featureTable;
    std::unique_ptr<WorkaroundTable> workaroundTable;

    std::unique_ptr<HwDeviceIdWddm> hwDeviceId;
    std::unique_ptr<GmmMemory> gmmMemory;
    std::unique_ptr<KmDafListener> kmDafListener;
    std::unique_ptr<WddmInterface> wddmInterface;
    std::unique_ptr<WddmResidentAllocationsContainer> temporaryResources;
    std::unique_ptr<WddmResidencyLogger> residencyLogger;
    std::unique_ptr<OSMemory> osMemory;

    static GetSystemInfoFcn getSystemInfo;
    RootDeviceEnvironment &rootDeviceEnvironment;

    uint64_t *pagingFenceAddress = nullptr;

    uintptr_t maximumApplicationAddress = 0;
    uintptr_t minAddress = 0;

    unsigned long hwContextId = 0;

    D3DKMT_HANDLE device = 0;
    D3DKMT_HANDLE pagingQueue = 0;
    D3DKMT_HANDLE pagingQueueSyncObject = 0;

    uint32_t maxRenderFrequency = 0;
    uint32_t timestampFrequency = 0u;
    uint32_t additionalAdapterInfoOptions = 0u;
    int32_t forceEvictOnlyIfNecessary = -1;

    unsigned int enablePreemptionRegValue = 1;

    bool platformSupportsEvictIfNecessary = false;
    bool instrumentationEnabled = false;
};
} // namespace NEO

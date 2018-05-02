//
// Copyright (c) Microsoft. All rights reserved.
// This code is licensed under the MIT License (MIT).
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
// PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
//
// Developed by Minigraph
//
// Author:  James Stanard 
//

#include "pch.h"
#include "DynamicDescriptorHeap.h"
#include "CommandContext.h"
#include "GraphicsCore.h"
#include "CommandListManager.h"
#include "RootSignature.h"

using namespace Graphics;

//
// DynamicDescriptorHeap Implementation
//

namespace {
  struct DefaultResources {
    void CreateDefaultResources()
    {
      using namespace Graphics;

      D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
      uavDesc.Format = DXGI_FORMAT_R8_UINT;
      uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE1D;

      mUavNullDescriptor = AllocateDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
      g_Device->CreateUnorderedAccessView(nullptr, nullptr, &uavDesc, mUavNullDescriptor);

      D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
      srvDesc.Format = DXGI_FORMAT_R8_UINT;
      srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1D;
      srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

      mSrvNullDescriptor = AllocateDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
      g_Device->CreateShaderResourceView(nullptr, &srvDesc, mSrvNullDescriptor);

      mCbvNullDescriptor = AllocateDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
      g_Device->CreateConstantBufferView(nullptr, mCbvNullDescriptor);

      D3D12_SAMPLER_DESC samplerDesc = {};
      samplerDesc.AddressU = samplerDesc.AddressV = samplerDesc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
      mSamplerNullDescriptor = AllocateDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
      g_Device->CreateSampler(&samplerDesc, mSamplerNullDescriptor);
    }

    const D3D12_CPU_DESCRIPTOR_HANDLE& GetType(const D3D12_DESCRIPTOR_RANGE_TYPE& type) {
      if (type == D3D12_DESCRIPTOR_RANGE_TYPE_SRV)
        return mSrvNullDescriptor;
      else if (type == D3D12_DESCRIPTOR_RANGE_TYPE_UAV)
        return mUavNullDescriptor;
      else if (type == D3D12_DESCRIPTOR_RANGE_TYPE_CBV)
        return mCbvNullDescriptor;
      else
        return mSamplerNullDescriptor;
    }

  private:
    D3D12_CPU_DESCRIPTOR_HANDLE mSrvNullDescriptor;
    D3D12_CPU_DESCRIPTOR_HANDLE mUavNullDescriptor;
    D3D12_CPU_DESCRIPTOR_HANDLE mCbvNullDescriptor;
    D3D12_CPU_DESCRIPTOR_HANDLE mSamplerNullDescriptor;
  };

  DefaultResources sm_DefaultResources;

}

std::mutex DynamicDescriptorHeap::sm_Mutex;
std::vector<Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>> DynamicDescriptorHeap::sm_DescriptorHeapPool[2];
std::queue<std::pair<uint64_t, ID3D12DescriptorHeap*>> DynamicDescriptorHeap::sm_RetiredDescriptorHeaps[2];
std::queue<ID3D12DescriptorHeap*> DynamicDescriptorHeap::sm_AvailableDescriptorHeaps[2];

ID3D12DescriptorHeap* DynamicDescriptorHeap::RequestDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE HeapType)
{
    std::lock_guard<std::mutex> LockGuard(sm_Mutex);

    uint32_t idx = HeapType == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER ? 1 : 0;

    while (!sm_RetiredDescriptorHeaps[idx].empty() && g_CommandManager.IsFenceComplete(sm_RetiredDescriptorHeaps[idx].front().first))
    {
        sm_AvailableDescriptorHeaps[idx].push(sm_RetiredDescriptorHeaps[idx].front().second);
        sm_RetiredDescriptorHeaps[idx].pop();
    }

    if (!sm_AvailableDescriptorHeaps[idx].empty())
    {
        ID3D12DescriptorHeap* HeapPtr = sm_AvailableDescriptorHeaps[idx].front();
        sm_AvailableDescriptorHeaps[idx].pop();
        return HeapPtr;
    }
    else
    {
        D3D12_DESCRIPTOR_HEAP_DESC HeapDesc = {};
        HeapDesc.Type = HeapType;
        HeapDesc.NumDescriptors = kNumDescriptorsPerHeap;
        HeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        HeapDesc.NodeMask = 1;
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> HeapPtr;
        ASSERT_SUCCEEDED(g_Device->CreateDescriptorHeap(&HeapDesc, MY_IID_PPV_ARGS(&HeapPtr)));
        sm_DescriptorHeapPool[idx].emplace_back(HeapPtr);
        return HeapPtr.Get();
    }
}

void DynamicDescriptorHeap::DiscardDescriptorHeaps( D3D12_DESCRIPTOR_HEAP_TYPE HeapType, uint64_t FenceValue, const std::vector<ID3D12DescriptorHeap*>& UsedHeaps )
{
    uint32_t idx = HeapType == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER ? 1 : 0;
    std::lock_guard<std::mutex> LockGuard(sm_Mutex);
    for (auto iter = UsedHeaps.begin(); iter != UsedHeaps.end(); ++iter)
        sm_RetiredDescriptorHeaps[idx].push(std::make_pair(FenceValue, *iter));
}

void DynamicDescriptorHeap::RetireCurrentHeap( void )
{
    // Don't retire unused heaps.
    if (m_CurrentOffset == 0)
    {
        ASSERT(m_CurrentHeapPtr == nullptr);
        return;
    }

    ASSERT(m_CurrentHeapPtr != nullptr);
    m_RetiredHeaps.push_back(m_CurrentHeapPtr);
    m_CurrentHeapPtr = nullptr;
    m_CurrentOffset = 0;
}

void DynamicDescriptorHeap::RetireUsedHeaps( uint64_t fenceValue )
{
    DiscardDescriptorHeaps(m_DescriptorType, fenceValue, m_RetiredHeaps);
    m_RetiredHeaps.clear();
}

DynamicDescriptorHeap::DynamicDescriptorHeap(CommandContext& OwningContext, D3D12_DESCRIPTOR_HEAP_TYPE HeapType)
    : m_OwningContext(OwningContext), m_DescriptorType(HeapType)
{
    m_CurrentHeapPtr = nullptr;
    m_CurrentOffset = 0;
    m_DescriptorSize = Graphics::g_Device->GetDescriptorHandleIncrementSize(HeapType);

    static std::once_flag init;
    std::call_once(init, [&]() { sm_DefaultResources.CreateDefaultResources(); });
}

DynamicDescriptorHeap::~DynamicDescriptorHeap()
{
}

void DynamicDescriptorHeap::CleanupUsedHeaps( uint64_t fenceValue )
{
    RetireCurrentHeap();
    RetireUsedHeaps(fenceValue);
    m_GraphicsHandleCache.ClearCache();
    m_ComputeHandleCache.ClearCache();
}

inline ID3D12DescriptorHeap* DynamicDescriptorHeap::GetHeapPointer()
{
    if (m_CurrentHeapPtr == nullptr)
    {
        ASSERT(m_CurrentOffset == 0);
        m_CurrentHeapPtr = RequestDescriptorHeap(m_DescriptorType);
        m_FirstDescriptor = DescriptorHandle(
            m_CurrentHeapPtr->GetCPUDescriptorHandleForHeapStart(),
            m_CurrentHeapPtr->GetGPUDescriptorHandleForHeapStart());
    }

    return m_CurrentHeapPtr;
}

uint32_t DynamicDescriptorHeap::DescriptorHandleCache::ComputeStagedSize()
{
    // Sum the maximum assigned offsets of stale descriptor tables to determine total needed space.
    uint32_t NeededSpace = 0;
    uint32_t RootIndex;
    uint32_t StaleParams = m_StaleRootParamsBitMap;
    while (_BitScanForward((unsigned long*)&RootIndex, StaleParams))
    {
        StaleParams ^= (1 << RootIndex);

        uint32_t MaxSetHandle;
        ASSERT(TRUE == _BitScanReverse((unsigned long*)&MaxSetHandle, m_RootDescriptorTable[RootIndex].AssignedHandlesBitMap),
            "Root entry marked as stale but has no stale descriptors");

        NeededSpace += MaxSetHandle + 1;
    }
    return NeededSpace;
}

void DynamicDescriptorHeap::DescriptorHandleCache::CopyAndBindStaleTables(
    D3D12_DESCRIPTOR_HEAP_TYPE Type, uint32_t DescriptorSize,
    DescriptorHandle DestHandleStart, ID3D12GraphicsCommandList* CmdList,
    void (STDMETHODCALLTYPE ID3D12GraphicsCommandList::*SetFunc)(UINT, D3D12_GPU_DESCRIPTOR_HANDLE))
{
    uint32_t StaleParamCount = 0;
    uint32_t TableSize[DescriptorHandleCache::kMaxNumDescriptorTables];
    uint32_t RootIndices[DescriptorHandleCache::kMaxNumDescriptorTables];
    uint32_t NeededSpace = 0;
    uint32_t RootIndex;

    // Sum the maximum assigned offsets of stale descriptor tables to determine total needed space.
    uint32_t StaleParams = m_StaleRootParamsBitMap;
    while (_BitScanForward((unsigned long*)&RootIndex, StaleParams))
    {
        RootIndices[StaleParamCount] = RootIndex;
        StaleParams ^= (1 << RootIndex);

        uint32_t MaxSetHandle;
        ASSERT(TRUE == _BitScanReverse((unsigned long*)&MaxSetHandle, m_RootDescriptorTable[RootIndex].AssignedHandlesBitMap),
            "Root entry marked as stale but has no stale descriptors");

        NeededSpace += MaxSetHandle + 1;
        TableSize[StaleParamCount] = MaxSetHandle + 1;

        ++StaleParamCount;
    }

    ASSERT(StaleParamCount <= DescriptorHandleCache::kMaxNumDescriptorTables,
        "We're only equipped to handle so many descriptor tables");

    m_StaleRootParamsBitMap = 0;

    static const uint32_t kMaxDescriptorsPerCopy = 16;
    UINT NumDestDescriptorRanges = 0;
    D3D12_CPU_DESCRIPTOR_HANDLE pDestDescriptorRangeStarts[kMaxDescriptorsPerCopy];
    UINT pDestDescriptorRangeSizes[kMaxDescriptorsPerCopy];

    UINT NumSrcDescriptorRanges = 0;
    D3D12_CPU_DESCRIPTOR_HANDLE pSrcDescriptorRangeStarts[kMaxDescriptorsPerCopy];
    UINT pSrcDescriptorRangeSizes[kMaxDescriptorsPerCopy];

    for (uint32_t i = 0; i < StaleParamCount; ++i)
    {
        RootIndex = RootIndices[i];
        (CmdList->*SetFunc)(RootIndex, DestHandleStart.GetGpuHandle());

        DescriptorTableCache& RootDescTable = m_RootDescriptorTable[RootIndex];

        D3D12_CPU_DESCRIPTOR_HANDLE* SrcHandles = RootDescTable.TableStart;
        uint64_t SetHandles = (uint64_t)RootDescTable.AssignedHandlesBitMap;
        D3D12_CPU_DESCRIPTOR_HANDLE CurDest = DestHandleStart.GetCpuHandle();
        DestHandleStart += TableSize[i] * DescriptorSize;

        unsigned long SkipCount;
        while (_BitScanForward64(&SkipCount, SetHandles))
        {
            // Skip over unset descriptor handles
            SetHandles >>= SkipCount;
            SrcHandles += SkipCount;
            CurDest.ptr += SkipCount * DescriptorSize;

            unsigned long DescriptorCount;
            _BitScanForward64(&DescriptorCount, ~SetHandles);
            SetHandles >>= DescriptorCount;

            // If we run out of temp room, copy what we've got so far
            if (NumSrcDescriptorRanges + DescriptorCount > kMaxDescriptorsPerCopy)
            {
                g_Device->CopyDescriptors(
                    NumDestDescriptorRanges, pDestDescriptorRangeStarts, pDestDescriptorRangeSizes,
                    NumSrcDescriptorRanges, pSrcDescriptorRangeStarts, pSrcDescriptorRangeSizes,
                    Type);

                NumSrcDescriptorRanges = 0;
                NumDestDescriptorRanges = 0;
            }

            // Setup destination range
            pDestDescriptorRangeStarts[NumDestDescriptorRanges] = CurDest;
            pDestDescriptorRangeSizes[NumDestDescriptorRanges] = DescriptorCount;
            ++NumDestDescriptorRanges;

            // Setup source ranges (one descriptor each because we don't assume they are contiguous)
            for (uint32_t j = 0; j < DescriptorCount; ++j)
            {
                pSrcDescriptorRangeStarts[NumSrcDescriptorRanges] = SrcHandles[j];
                pSrcDescriptorRangeSizes[NumSrcDescriptorRanges] = 1;
                ++NumSrcDescriptorRanges;
            }

            // Move the destination pointer forward by the number of descriptors we will copy
            SrcHandles += DescriptorCount;
            CurDest.ptr += DescriptorCount * DescriptorSize;
        }
    }

    g_Device->CopyDescriptors(
        NumDestDescriptorRanges, pDestDescriptorRangeStarts, pDestDescriptorRangeSizes,
        NumSrcDescriptorRanges, pSrcDescriptorRangeStarts, pSrcDescriptorRangeSizes,
        Type);
}
    
void DynamicDescriptorHeap::CopyAndBindStagedTables( DescriptorHandleCache& HandleCache, ID3D12GraphicsCommandList* CmdList,
    void (STDMETHODCALLTYPE ID3D12GraphicsCommandList::*SetFunc)(UINT, D3D12_GPU_DESCRIPTOR_HANDLE))
{
    uint32_t NeededSize = HandleCache.ComputeStagedSize();
    if (!HasSpace(NeededSize))
    {
        RetireCurrentHeap();
        UnbindAllValid();
        NeededSize = HandleCache.ComputeStagedSize();
    }

    // This can trigger the creation of a new heap
    m_OwningContext.SetDescriptorHeap(m_DescriptorType, GetHeapPointer());
    HandleCache.CopyAndBindStaleTables(m_DescriptorType, m_DescriptorSize, Allocate(NeededSize), CmdList, SetFunc);
}

void DynamicDescriptorHeap::DescriptorHandleCache::SetDefaults() {
  unsigned long TableParams = m_RootDescriptorTablesBitMap;
  unsigned long RootIndex;
  while (_BitScanForward(&RootIndex, TableParams)) {
    TableParams ^= (1 << RootIndex);
    DescriptorTableCache &RootDescTable = m_RootDescriptorTable[RootIndex];

    if (RootDescTable.TableSize == 0u)
      continue;

    uint32_t TableMask = ((1 << RootDescTable.TableSize) - 1);
    if (RootDescTable.AssignedHandlesBitMap == TableMask)
      continue;

    if (RootDescTable.Type != D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE)
      continue;

    uint64_t SetHandles = (uint64_t)m_RootDescriptorTable[RootIndex].AssignedHandlesBitMap;
    auto SrcHandles = RootDescTable.TableStart;

    if (RootDescTable.RangeType >= D3D12_DESCRIPTOR_RANGE_TYPE_SRV && RootDescTable.RangeType <= D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER)
    {
      auto& NullResource = sm_DefaultResources.GetType(RootDescTable.RangeType);
      uint32_t InverseTableMask = ~TableMask;

      unsigned long UnsetCount;
      uint32_t Offset = 0;
      while (_BitScanForward64(&UnsetCount, SetHandles ^ InverseTableMask) && Offset < RootDescTable.TableSize) {
        SetHandles >>= UnsetCount;
        InverseTableMask >>= UnsetCount;

        if (UnsetCount > 0) {
          std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> null_resources(UnsetCount, NullResource);
          StageDescriptorHandles(RootIndex, Offset, UnsetCount, null_resources.data());
          Offset += UnsetCount;
        }

        // Skip already set ones
        unsigned long DescriptorCount;
        _BitScanForward64(&DescriptorCount, ~SetHandles);
        if (DescriptorCount > 0) {
          SetHandles >>= DescriptorCount;
          InverseTableMask >>= DescriptorCount;
          Offset += DescriptorCount;
        }
      }

    }
  }
}

void DynamicDescriptorHeap::UnbindAllValid( void )
{
    m_GraphicsHandleCache.UnbindAllValid();
    m_ComputeHandleCache.UnbindAllValid();
}

D3D12_GPU_DESCRIPTOR_HANDLE DynamicDescriptorHeap::UploadDirect( D3D12_CPU_DESCRIPTOR_HANDLE Handle )
{
    if (!HasSpace(1))
    {
        RetireCurrentHeap();
        UnbindAllValid();
    }

    m_OwningContext.SetDescriptorHeap(m_DescriptorType, GetHeapPointer());

    DescriptorHandle DestHandle = m_FirstDescriptor + m_CurrentOffset * m_DescriptorSize;
    m_CurrentOffset += 1;

    g_Device->CopyDescriptorsSimple(1, DestHandle.GetCpuHandle(), Handle, m_DescriptorType);

    return DestHandle.GetGpuHandle();
}

void DynamicDescriptorHeap::DescriptorHandleCache::UnbindAllValid()
{
    m_StaleRootParamsBitMap = 0;

    unsigned long TableParams = m_RootDescriptorTablesBitMap;
    unsigned long RootIndex;
    while (_BitScanForward(&RootIndex, TableParams))
    {
        TableParams ^= (1 << RootIndex);
        if (m_RootDescriptorTable[RootIndex].AssignedHandlesBitMap != 0)
            m_StaleRootParamsBitMap |= (1 << RootIndex);
    }
}

void DynamicDescriptorHeap::DescriptorHandleCache::StageDescriptorHandles( UINT RootIndex, UINT Offset, UINT NumHandles, const D3D12_CPU_DESCRIPTOR_HANDLE Handles[] )
{
    ASSERT(((1 << RootIndex) & m_RootDescriptorTablesBitMap) != 0, "Root parameter is not a CBV_SRV_UAV descriptor table");
    ASSERT(Offset + NumHandles <= m_RootDescriptorTable[RootIndex].TableSize);

    DescriptorTableCache& TableCache = m_RootDescriptorTable[RootIndex];
    D3D12_CPU_DESCRIPTOR_HANDLE* CopyDest = TableCache.TableStart + Offset;
    for (UINT i = 0; i < NumHandles; ++i)
        CopyDest[i] = Handles[i];
    TableCache.AssignedHandlesBitMap |= ((1 << NumHandles) - 1) << Offset;
    m_StaleRootParamsBitMap |= (1 << RootIndex);
}

void DynamicDescriptorHeap::DescriptorHandleCache::ParseRootSignature( D3D12_DESCRIPTOR_HEAP_TYPE Type, const RootSignature& RootSig )
{
    UINT CurrentOffset = 0;

    ASSERT(RootSig.m_NumParameters <= 16, "Maybe we need to support something greater");

    m_StaleRootParamsBitMap = 0;
    m_RootDescriptorTablesBitMap = (Type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER ?
        RootSig.m_SamplerTableBitMap : RootSig.m_DescriptorTableBitMap);

    unsigned long TableParams = m_RootDescriptorTablesBitMap;
    unsigned long RootIndex;
    while (_BitScanForward(&RootIndex, TableParams))
    {
        TableParams ^= (1 << RootIndex);

        UINT TableSize = RootSig.m_DescriptorTableSize[RootIndex];
        ASSERT(TableSize > 0);

        DescriptorTableCache& RootDescriptorTable = m_RootDescriptorTable[RootIndex];
        RootDescriptorTable.AssignedHandlesBitMap = 0;
        RootDescriptorTable.TableStart = m_HandleCache + CurrentOffset;
        RootDescriptorTable.TableSize = TableSize;
        RootDescriptorTable.Type = RootSig[RootIndex].GetType();
        RootDescriptorTable.RangeType = RootSig[RootIndex].GetRangeType();
        CurrentOffset += TableSize;
    }

    m_MaxCachedDescriptors = CurrentOffset;

    ASSERT(m_MaxCachedDescriptors <= kMaxNumDescriptors, "Exceeded user-supplied maximum cache size");
}

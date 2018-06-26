//*********************************************************
//
// Copyright (c) Microsoft. All rights reserved.
// This code is licensed under the MIT License (MIT).
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
// PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
//
//*********************************************************

// Using the Karras/Aila paper on treelet reoordering:
// "Fast Parallel Construction of High-Quality Bounding Volume
// Hierarchies"

#define HLSL
#include "TreeletReorderBindings.h"
#include "RayTracingHelper.hlsli"

AABB ComputeLeafAABB(uint triangleIndex)
{
    uint2 unused;
    Primitive primitive = InputBuffer[triangleIndex];
    if (primitive.PrimitiveType == TRIANGLE_TYPE)
    {
        Triangle tri = GetTriangle(primitive);
        return BoundingBoxToAABB(GetBoxDataFromTriangle(tri.v0, tri.v1, tri.v2, triangleIndex, unused));
    }
    else // if(primitiveType == PROCEDURAL_PRIMITIVE_TYPE)
    {
        return GetProceduralPrimitiveAABB(primitive);
    }
}

AABB CombineAABB(AABB aabb0, AABB aabb1)
{
    AABB parentAABB;
    parentAABB.min = min(aabb0.min, aabb1.min);
    parentAABB.max = max(aabb0.max, aabb1.max);
    return parentAABB;
}

[numthreads(THREAD_GROUP_1D_WIDTH, 1, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    if (DTid.x >= Constants.NumberOfElements) return;

    const uint NumberOfInternalNodes = GetNumInternalNodes(Constants.NumberOfElements);
    const uint NumberOfAABBs = NumberOfInternalNodes + Constants.NumberOfElements;

    // Start from the leaf nodes and go bottom-up
    uint nodeIndex = NumberOfAABBs - DTid.x - 1;
    uint numTriangles = 1;
    bool isLeaf = true;
    do
    {
        AABB nodeAABB;
        if (isLeaf)
        {
            uint leafIndex = nodeIndex - NumberOfInternalNodes;
            nodeAABB = ComputeLeafAABB(leafIndex);
        }
        else
        {
            AABB leftAABB = AABBBuffer[hierarchyBuffer[nodeIndex].LeftChildIndex];
            AABB rightAABB = AABBBuffer[hierarchyBuffer[nodeIndex].RightChildIndex];
            nodeAABB = CombineAABB(leftAABB, rightAABB);
        }

        AABBBuffer[nodeIndex] = nodeAABB;
        DeviceMemoryBarrier(); // Ensure AABBs have been written out and are visible to all waves

        if (numTriangles >= Constants.MinTrianglesPerTreelet)
        {
            uint previousCount;
            BaseTreeletsCountBuffer.InterlockedAdd(0, 1, previousCount);
            BaseTreeletsIndexBuffer[previousCount] = nodeIndex;
            return;
        }

        if (nodeIndex == rootNodeIndex) return;

        uint parentNodeIndex = hierarchyBuffer[nodeIndex].ParentIndex;

        uint numTrianglesFromOtherNode = 0;
        NumTrianglesBuffer.InterlockedAdd(parentNodeIndex * SizeOfUINT32, numTriangles, numTrianglesFromOtherNode);

        // Wait for sibling in tree
        if (numTrianglesFromOtherNode == 0)
        {
            return;
        }

        numTriangles = numTrianglesFromOtherNode + numTriangles;
        nodeIndex = parentNodeIndex;
        isLeaf = false;

    } while (true);
}

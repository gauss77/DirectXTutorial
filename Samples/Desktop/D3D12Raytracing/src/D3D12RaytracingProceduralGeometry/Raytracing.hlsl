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

#ifndef RAYTRACING_HLSL
#define RAYTRACING_HLSL

#define HLSL
#include "RaytracingHlslCompat.h"
#include "ProceduralPrimitivesLibrary.h"
#include "RaytracingShaderHelper.h"

// ToDo:
// - handle RayFlags in intersection tests
// - enable shadows on AABBs
// - specify traceRay args in a shared header

RaytracingAccelerationStructure Scene : register(t0, space0);
RWTexture2D<float4> RenderTarget : register(u0);
ByteAddressBuffer Indices : register(t1, space0);
StructuredBuffer<Vertex> Vertices : register(t2, space0);
StructuredBuffer<AABBPrimitiveAttributes> g_AABBPrimitiveAttributes : register(t3, space0);

ConstantBuffer<SceneConstantBuffer> g_sceneCB : register(b0);
ConstantBuffer<MaterialConstantBuffer> g_materialCB : register(b1);
ConstantBuffer<AABBConstantBuffer> g_aabbCB : register(b2);

struct MyAttributes
{
    float2 barycentrics;
    float4 normal;
};

enum ProceduralPrimitives
{
    Box = 0,
    Spheres,
    Sphere,
    Count
};

struct ShadowPayload
{
    bool hit;
};


struct HitData
{
    float4 color;
};

// Retrieve hit world position.
float3 HitWorldPosition()
{
    return WorldRayOrigin() + RayTCurrent() * WorldRayDirection();
}

// Retrieve attribute at a hit position interpolated from vertex attributes using the hit's barycentrics.
float3 HitAttribute(float3 vertexAttribute[3], float2 barycentrics)
{
    return vertexAttribute[0] +
        barycentrics.x * (vertexAttribute[1] - vertexAttribute[0]) +
        barycentrics.y * (vertexAttribute[2] - vertexAttribute[0]);
}

// Generate a ray in world space for a camera pixel corresponding to an index from the dispatched 2D grid.
inline void GenerateCameraRay(uint2 index, out float3 origin, out float3 direction)
{
    float2 xy = index + 0.5f; // center in the middle of the pixel.
    float2 screenPos = xy / DispatchRaysDimensions() * 2.0 - 1.0;

    // Invert Y for DirectX-style coordinates.
    screenPos.y = -screenPos.y;

    // Unproject the pixel coordinate into a ray.
    float4 world = mul(float4(screenPos, 0, 1), g_sceneCB.projectionToWorld);

    world.xyz /= world.w;
    origin = g_sceneCB.cameraPosition.xyz;
    direction = normalize(world - origin);
}

// ToDo is pixelToLight correct?
// Diffuse lighting calculation.
float4 CalculateDiffuseLighting(float3 hitPosition, float3 normal)
{
    float3 pixelToLight = normalize(g_sceneCB.lightPosition - hitPosition);

    // Diffuse contribution.
    float fNDotL = max(0.0f, dot(pixelToLight, normal));

    return g_sceneCB.lightDiffuseColor * fNDotL;
}

[shader("raygeneration")]
void MyRaygenShader()
{
    float3 rayDir;
    float3 origin;

    // Generate a ray for a camera pixel corresponding to an index from the dispatched 2D grid.
    GenerateCameraRay(DispatchRaysIndex(), origin, rayDir);

    // Trace the ray.
    // Set the ray's extents.
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = rayDir;
    // Set TMin to a non-zero small value to avoid aliasing issues due to floating - point errors.
    // TMin should be kept small to prevent missing geometry at close contact areas.
    ray.TMin = 0;
    ray.TMax = 10000.0;
    HitData payload = { float4(0, 0, 0, 0) };
    TraceRay(
        Scene, // Raytracing acceleration structure
        RAY_FLAG_CULL_BACK_FACING_TRIANGLES, /* RayFlags */
        ~0, /* InstanceInclusionMask*/
         0, /* RayContributionToHitGroupIndex */
         2, /* MultiplierForGeometryContributionToHitGroupIndex */
         0, /* MissShaderIndex */
        ray, payload);

    // Write the raytraced color to the output texture.
    RenderTarget[DispatchRaysIndex()] = payload.color;
}

// Get ray in AABB's local space
Ray GetRayInAABBPrimitiveLocalSpace()
{
    // Should PrimitiveIndex be passed as arg?
    // ToDo improve desc
    // Retrieve ray origin position and direction in bottom level AS space 
    // and transform them into the AABB primitive's local space.
    AABBPrimitiveAttributes aabbAttribute = g_AABBPrimitiveAttributes[g_aabbCB.geometryIndex];
    Ray ray;
    ray.origin = mul(float4(ObjectRayOrigin(), 1), aabbAttribute.bottomLevelASToLocalSpace).xyz;
    ray.direction = mul(ObjectRayDirection(), (float3x3) aabbAttribute.bottomLevelASToLocalSpace).xyz;
    return ray;
}

[shader("intersection")]
void MyIntersectionShader_Spheres()
{
    ProceduralPrimitiveAttributes attr;
    float tHit;
    Ray localRay = GetRayInAABBPrimitiveLocalSpace();
    if (RaySpheresIntersectionTest(localRay, RayTMin(), RayTCurrent(), tHit, attr))
    {
        AABBPrimitiveAttributes aabbAttribute = g_AABBPrimitiveAttributes[g_aabbCB.geometryIndex];
        attr.normal = mul(attr.normal, (float3x3) aabbAttribute.localSpaceToBottomLevelAS).xyz;

        // ReportHit will reject any tHits outside a valid tHit range: <RayTMin(), RayTCurrent()>.
        ReportHit(tHit, /*hitKind*/ 0, attr);
    }
}

[shader("intersection")]
void MyIntersectionShader_Sphere()
{
    ProceduralPrimitiveAttributes attr;
    float tHit;
    Ray localRay = GetRayInAABBPrimitiveLocalSpace();
    if (RaySphereIntersectionTest(localRay, RayTMin(), tHit, attr))
    {
        AABBPrimitiveAttributes aabbAttribute = g_AABBPrimitiveAttributes[g_aabbCB.geometryIndex];
        attr.normal = mul(attr.normal, (float3x3) aabbAttribute.localSpaceToBottomLevelAS).xyz;

        // ReportHit will reject any tHits outside a valid tHit range: <RayTMin(), RayTCurrent()>.
        ReportHit(tHit, /*hitKind*/ 0, attr);       
    }
}

[shader("intersection")]
void MyIntersectionShader_AABB()
{
    ProceduralPrimitiveAttributes attr;
    float tHit;
    Ray localRay = GetRayInAABBPrimitiveLocalSpace(); 
    if (RayAABBIntersectionTest(localRay, tHit, attr))
    {
        AABBPrimitiveAttributes aabbAttribute = g_AABBPrimitiveAttributes[g_aabbCB.geometryIndex];
        attr.normal = mul(attr.normal, (float3x3) aabbAttribute.localSpaceToBottomLevelAS).xyz;

        // ReportHit will reject any tHits outside a valid tHit range: <RayTMin(), RayTCurrent()>.
        ReportHit(tHit, /*hitKind*/ 0, attr);
    }
}

[shader("closesthit")]
void MyClosestHitShader_Triangle(inout HitData payload : SV_RayPayload, in BuiltInTriangleIntersectionAttributes attr : SV_IntersectionAttributes)
{
    float3 hitPosition = HitWorldPosition();

    // Get the base index of the triangle's first 16 bit index.
    uint indexSizeInBytes = 2;
    uint indicesPerTriangle = 3;
    uint triangleIndexStride = indicesPerTriangle * indexSizeInBytes;
    uint baseIndex = PrimitiveIndex() * triangleIndexStride;

    // Load up 3 16 bit indices for the triangle.
    const uint3 indices = Load3x16BitIndices(baseIndex, Indices);

    // Retrieve corresponding vertex normals for the triangle vertices.
    float3 vertexNormals[3] = {
        Vertices[indices[0]].normal,
        Vertices[indices[1]].normal,
        Vertices[indices[2]].normal
    };

    // Compute the triangle's normal.
    // This is redundant and done for illustration purposes 
    // as all the per-vertex normals are the same and match triangle's normal in this sample. 
    float3 triangleNormal = HitAttribute(vertexNormals, attr.barycentrics);

    // Trace a shadow ray. 
    // Set the ray's extents.
    RayDesc ray;
    ray.Origin = hitPosition;
    ray.Direction = normalize(g_sceneCB.lightPosition - hitPosition);
    // Set TMin to a non-zero small value to avoid aliasing issues due to floating - point errors.
    // TMin should be kept small to prevent missing geometry at close contact areas.
    // For shadow ray this will be extremely small to avoid aliasing at contact areas.
    ray.TMin = 0;
    ray.TMax = 10000.0;
    ShadowPayload shadowPayload;
    // ToDo use hit/miss indices from a header
    // ToDo place ShadowHitGroup right after Closest hitgroup?
    // ToDo review hit group indexing
    // ToDo - improve wording, reformat: Offset by 1 as AABB  BLAS offsets by 1 => 2
    TraceRay(Scene,
        RAY_FLAG_CULL_BACK_FACING_TRIANGLES | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, /* RayFlags */
        ~0,/* InstanceInclusionMask*/
        1, /* RayContributionToHitGroupIndex */
        2, /* MultiplierForGeometryContributionToHitGroupIndex */
        1, /* MissShaderIndex */ 
        ray, shadowPayload);

    float shadowFactor = shadowPayload.hit ? 0.1 : 1.0;

    float4 diffuseColor = shadowFactor * g_materialCB.albedo * CalculateDiffuseLighting(hitPosition, triangleNormal);
    float4 color = g_sceneCB.lightAmbientColor + diffuseColor;

    payload.color = color;
}

[shader("closesthit")]
void MyClosestHitShader_AABB(inout HitData payload : SV_RayPayload, in ProceduralPrimitiveAttributes attr : SV_IntersectionAttributes)
{
    float3 hitPosition = HitWorldPosition();

#if 1 // ToDo doesn't work properly
    // Trace a shadow ray. 
    // Set the ray's extents.
    RayDesc ray;
    ray.Origin = hitPosition;
    ray.Direction = normalize(g_sceneCB.lightPosition - hitPosition);
    // Set TMin to a non-zero small value to avoid aliasing issues due to floating - point errors.
    // TMin should be kept small to prevent missing geometry at close contact areas.
    // ToDo explain use of 0, should it be everywhere? - its ok due to CULL back facing
    ray.TMin = 0;
    ray.TMax = 10000.0;
    ShadowPayload shadowPayload;
    // ToDo use hit/miss indices from a header
    TraceRay(Scene,
        RAY_FLAG_CULL_BACK_FACING_TRIANGLES | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, /* RayFlags */
        ~0,/* InstanceInclusionMask*/
        1, /* RayContributionToHitGroupIndex */
        2, /* MultiplierForGeometryContributionToHitGroupIndex */
        1, /* MissShaderIndex */
        ray, shadowPayload);

    float shadowFactor = shadowPayload.hit ? 0.1 : 1.0;
#else
    float shadowFactor = 1.0;
#endif

    float3 triangleNormal = attr.normal;
    float4 albedo = g_materialCB.albedo;
    float4 diffuseColor = shadowFactor * albedo * CalculateDiffuseLighting(hitPosition, triangleNormal);
    float4 color = g_sceneCB.lightAmbientColor + diffuseColor;

    payload.color = color;
}

[shader("miss")]
void MyMissShader(inout HitData payload : SV_RayPayload)
{
    float4 background = float4(0.0f, 0.2f, 0.4f, 1.0f);
    payload.color = background;
}

[shader("closesthit")]
void MyClosestHitShader_ShadowAABB(inout ShadowPayload payload : SV_RayPayload, in ProceduralPrimitiveAttributes attr : SV_IntersectionAttributes)
{
    payload.hit = true;
}

[shader("miss")]
void MyMissShader_Shadow(inout ShadowPayload payload : SV_RayPayload)
{
    payload.hit = false;
}

#endif // RAYTRACING_HLSL
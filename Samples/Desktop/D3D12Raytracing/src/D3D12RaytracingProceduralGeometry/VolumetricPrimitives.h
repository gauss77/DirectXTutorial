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

#ifndef VOLUMETRICPRIMITIVESLIBRARY_H
#define VOLUMETRICPRIMITIVESLIBRARY_H

#include "RaytracingShaderHelper.h"

// Calculate a magnitude of an influence from a Metaball charge.
// mbRadius - largest possible area of metaball contribution - AKA its bounding sphere.
// Ref: https://www.scratchapixel.com/lessons/advanced-rendering/rendering-distance-fields/blobbies
float CalculateMetaballPotential(in float3 position, in float3 mbCenter, in float mbRadius)
{
    float d = length(position - mbCenter);

    if (d <= mbRadius)
    {
        return 2 * (d * d * d) / (mbRadius * mbRadius * mbRadius)
            - 3 * (d * d) / (mbRadius * mbRadius)
            + 1;
        float dR = d / mbRadius;
        return (2 * dR - 3) * (dR * dR) + 1;
    }
    return 0;
}

// Test if a ray with RayFlags and segment <RayTMin(), RayTCurrent()> intersect a metaball field.
// Ref: http://www.geisswerks.com/ryan/BLOBS/blobs.html
bool RayMetaballsIntersectionTest(in Ray ray, out float thit, out ProceduralPrimitiveAttributes attr, in float totalTime)
{
    const UINT N = 3;
    // ToDo Pass in from the app?
    // Metaball centers at t0 and t1 key frames.
    float3 keyFrameCenters[N][2] =
    {
        { float3(-0.5, -0.3, -0.4),float3(0.5,-0.3,-0.0) },
        { float3(0.0, -0.4, 0.5), float3(0.0, 0.4, 0.5) },
        { float3(0.5,0.5, 0.4), float3(-0.5, 0.2, -0.4) }
    };

    // Calculate animated metaball center positions.
    float  tAnimate = CalculateAnimationInterpolant(totalTime, 8.0f);
    float3 centers[N];
    centers[0] = lerp(keyFrameCenters[0][0], keyFrameCenters[0][1], tAnimate);
    centers[1] = lerp(keyFrameCenters[1][0], keyFrameCenters[1][1], tAnimate);
    centers[2] = lerp(keyFrameCenters[2][0], keyFrameCenters[2][1], tAnimate);

    // Metaball field radii of max influence
    float radii[N] = { 0.50, 0.65, 0.50 };    // ToDo Compare perf with precomputed invRadius
    
    // Set bounds for ray march to in and out intersection 
    // against max influence of all metaballs. 
    float tmin, tmax;
    tmin = RayTCurrent();
    tmax = RayTMin();
#if 1
    float _thit, _tmax;
#if 0 // DO_NOT_USE_DYNAMIC_LOOPS
    // ToDo remove
    if (RaySolidSphereIntersectionTest(ray, _thit, _tmax, centers[0], radii[0]))
    {
        tmin = min(_thit, tmin);
        tmax = max(_tmax, tmax);
    }
    if (RaySolidSphereIntersectionTest(ray, _thit, _tmax, centers[1], radii[1]))
    {
        tmin = min(_thit, tmin);
        tmax = max(_tmax, tmax);
    }
    if (RaySolidSphereIntersectionTest(ray, _thit, _tmax, centers[2], radii[2]))
    {
        tmin = min(_thit, tmin);
        tmax = max(_tmax, tmax);
    }
#else
    for (UINT j = 0; j < N; j++)
    {
        if (RaySolidSphereIntersectionTest(ray, _thit, _tmax, centers[j], radii[j]))
        {
            tmin = min(_thit, tmin);
            tmax = max(_tmax, tmax);
        }
    }
#endif
#else
    float3 aabb[2] = {
        float3(-1,-1,-1),
        float3(1,1,1)
    };
    if (!RayAABBIntersectionTest(ray, aabb, tmin, tmax))
    {
        return false;
    }
#endif
    tmin = max(tmin, RayTMin());
    tmax = min(tmax, RayTCurrent());

    UINT MAX_STEPS = 128;
    float tstep = (tmax - tmin) / (MAX_STEPS - 1);

    // ToDo lipchshitz ray marcher

    // Step along the ray calculating field potentials from all metaballs.
    for (UINT i = 0; i < MAX_STEPS; i++)
    {
        float t = tmin + i * tstep;
        float3 position = ray.origin + t * ray.direction;
        float fieldPotentials[N];
        float fieldPotential = 0;

        for (UINT j = 0; j < N; j++)
        {
            fieldPotentials[j] = CalculateMetaballPotential(position, centers[j], radii[j]);
            fieldPotential = fieldPotential + fieldPotentials[j];
        }

        // ToDo revise threshold range
        // ToDo pass threshold from app
        // Threshold - valid range is (0, 0.1>, the larger the threshold the smaller the blob.
        float threshold = 0.25f;
        if (fieldPotential >= threshold)
        {
            // Calculate normal as a weighted average of sphere normals from contributing metaballs.
            float3 normal = float3(0, 0, 0);

            for (UINT j = 0; j < N; j++)
            {
                normal += fieldPotentials[j] * CalculateNormalForARaySphereHit(ray, t, centers[j]);
            }

            normal = normalize(normal/fieldPotential);
            if (IsAValidHit(ray, t, normal))
            {
                thit = t;
                attr.normal = normal;
                return true;
            }
        }
    }

    return false;
}


#endif // VOLUMETRICPRIMITIVESLIBRARY_H
/// @file    RayShape.hlsli
/// @brief   World-space emissive sphere/capsule intersection and uniform surface sampling.
/// @author  Hasegawa Jin
/// @date    2026-10-01
#ifndef FBZZ_RAY_SHAPE_HLSLI
#define FBZZ_RAY_SHAPE_HLSLI

/// @note Matches RayPathShapeRecord80B. Tube axis is unit length; halfLength excludes the hemispheres.
struct RayPathShapeRecord {
    float3 position; float radius;
    float3 axis; float halfLength;
    float3 emission; float area;
    float selectionPdf; float selectionCdf; float range; uint type;
    uint objectIndex; uint objectGeneration; uint flags; float shadowStrength;
};
void RayShapeBasis(float3 normal, out float3 tangent, out float3 bitangent)
{
    tangent = normalize(cross(abs(normal.z) < 0.999f ? float3(0, 0, 1) : float3(0, 1, 0), normal));
    bitangent = cross(normal, tangent);
}
float RayShapeLength(float3 value)
{
    float scale = max(abs(value.x), max(abs(value.y), abs(value.z)));
    return scale == 0 ? 0 : scale * sqrt(dot(value / scale, value / scale));
}
/// @note Closest-line discriminant preserves a far small radius; q-form roots separately avoid near-surface cancellation.
/// @see https://pbr-book.org/4ed/Shapes/Spheres Sphere quadratic intersection
bool RayShapeRoots(float3 origin, float3 direction, float radius, out float2 roots,
    out float3 closest, out float span)
{
    roots = 0; closest = 0; span = 0;
    float a = dot(direction, direction);
    if (a <= 0) return false;
    float b = dot(direction, origin);
    closest = cross(direction, cross(origin, direction)) / a;
    float closestLength = RayShapeLength(closest);
    if (closestLength > radius) return false;
    span = sqrt(max(0, radius - closestLength)) * sqrt(radius + closestLength) / sqrt(a);
    float originLength = RayShapeLength(origin);
    float c = (originLength - radius) * (originLength + radius);
    float q = -b - (b >= 0 ? a * span : -a * span);
    if (q == 0) roots = (-b / a).xx;
    else roots = float2(q / a, c / q);
    if (roots.x > roots.y) roots = roots.yx;
    return true;
}
/// @return 0=miss, 1=hit, 2=unrepresentable finite geometry arithmetic.
uint IntersectRayShape(RayPathShapeRecord shape, RayDesc ray, out float distance, out float3 normal)
{
    distance = ray.TMax; normal = 0;
    float3 relative = ray.Origin - shape.position;
    if (!all(isfinite(relative))) return 2u;
    float scale = max(shape.radius, max(shape.halfLength, max(abs(relative.x), max(abs(relative.y), abs(relative.z)))));
    float3 origin = relative / scale;
    float radius = shape.radius / scale, halfLength = shape.halfLength / scale;
    if (!isfinite(scale) || scale <= 0 || radius <= 0) return 2u;
    float minT = ray.TMin / scale, maxT = ray.TMax / scale;
    float best = maxT;
    float3 bestNormal = 0;
    float axialOrigin = dot(origin, shape.axis), axialDirection = dot(ray.Direction, shape.axis);
    float3 radialOrigin = origin - shape.axis * axialOrigin;
    float3 radialDirection = ray.Direction - shape.axis * axialDirection;
    float2 roots;
    float3 closest; float span;
    if (shape.type == 1u && halfLength > 0 && RayShapeRoots(radialOrigin, radialDirection, radius, roots, closest, span)) {
        if (!all(isfinite(roots)) || !all(isfinite(closest)) || !isfinite(span)) return 2u;
        for (uint i = 0; i < 2; ++i) {
            float t = roots[i], height = axialOrigin + t * axialDirection;
            if (t >= minT && t <= best && abs(height) <= halfLength) {
                best = t; bestNormal = normalize(closest + (i == 0 ? -span : span) * radialDirection);
            }
        }
    }
    uint caps = shape.type == 1u && halfLength > 0 ? 2u : 1u;
    for (uint cap = 0; cap < caps; ++cap) {
        float sign = cap == 0 ? -1 : 1;
        float3 center = caps == 2u ? shape.axis * (sign * halfLength) : 0;
        float3 offset = origin - center;
        if (!RayShapeRoots(offset, ray.Direction, radius, roots, closest, span)) continue;
        if (!all(isfinite(roots)) || !all(isfinite(closest)) || !isfinite(span)) return 2u;
        for (uint i = 0; i < 2; ++i) {
            float t = roots[i];
            float3 candidate = closest + (i == 0 ? -span : span) * ray.Direction;
            if (t < minT || t > best || (caps == 2u && sign * dot(candidate, shape.axis) < 0)) continue;
            best = t; bestNormal = normalize(candidate);
        }
    }
    if (dot(bestNormal, bestNormal) == 0) return 0u;
    distance = best * scale; normal = bestNormal;
    return isfinite(distance) && all(isfinite(normal)) ? 1u : 2u;
}
/// @note Uniform area on cylinder and hemispheres; their selection probabilities are their exact surface-area fractions.
/// @see https://pbr-book.org/4ed/Sampling_Algorithms/Sampling_Multidimensional_Functions Uniform sphere sampling
void SampleRayShape(RayPathShapeRecord shape, float3 random, out float3 position, out float3 normal)
{
    float3 tangent, bitangent;
    RayShapeBasis(shape.axis, tangent, bitangent);
    float phi = 2 * PI * random.z;
    float cylinderProbability = shape.type == 1u ? shape.halfLength / (shape.radius + shape.halfLength) : 0;
    if (random.x < cylinderProbability) {
        normal = cos(phi) * tangent + sin(phi) * bitangent;
        position = shape.position + shape.axis * ((2 * random.y - 1) * shape.halfLength) + shape.radius * normal;
    } else {
        float z = 1 - 2 * random.y, radial = sqrt(max(0, 1 - z * z));
        normal = radial * (cos(phi) * tangent + sin(phi) * bitangent) + z * shape.axis;
        position = shape.position + shape.radius * normal
            + (shape.type == 1u ? (z >= 0 ? shape.halfLength : -shape.halfLength) * shape.axis : 0);
    }
}
float RayShapeSolidAnglePdf(RayPathShapeRecord shape, float3 origin, float3 position, float3 normal)
{
    float3 delta = position - origin;
    float distanceSquared = dot(delta, delta);
    if (!all(isfinite(delta)) || !isfinite(distanceSquared)) return asfloat(0x7FC00000u);
    if (distanceSquared <= 0 || shape.selectionPdf <= 0) return 0;
    float cosine = dot(normal, -normalize(delta));
    if (cosine <= 0) return 0;
    return shape.selectionPdf * distanceSquared / (shape.area * cosine);
}
#endif

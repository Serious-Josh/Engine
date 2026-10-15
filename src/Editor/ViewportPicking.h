#pragma once

#include "Renderer/Mesh.h"
#include "assets/ImportedMesh.h"
#include "Math/Matrix4.h"
#include "Scene/Camera.h"
#include <algorithm>
#include <cmath>
#include <span>
#include <limits>

namespace ViewportPicking
{
    struct Ray
    {
        Vector3 origin;
        Vector3 direction;
        float maxDistance = 0.0f;
    };

    inline bool UnprojectPoint(
        const Matrix4& inverseViewProjection,
        float x,
        float y,
        float z,
        Vector3& result)
    {
        const float w = inverseViewProjection.m[3][0] * x + inverseViewProjection.m[3][1] * y +
                        inverseViewProjection.m[3][2] * z + inverseViewProjection.m[3][3];

        if (!std::isfinite(w) || std::abs(w) < 0.000001f)
        {
            return false;
        }

        const float inverseW = 1.0f / w;

        result = {(inverseViewProjection.m[0][0] * x + inverseViewProjection.m[0][1] * y +
                   inverseViewProjection.m[0][2] * z + inverseViewProjection.m[0][3]) *
                      inverseW,
                  (inverseViewProjection.m[1][0] * x + inverseViewProjection.m[1][1] * y +
                   inverseViewProjection.m[1][2] * z + inverseViewProjection.m[1][3]) *
                      inverseW,
                  (inverseViewProjection.m[2][0] * x + inverseViewProjection.m[2][1] * y +
                   inverseViewProjection.m[2][2] * z + inverseViewProjection.m[2][3]) *
                      inverseW};

        return std::isfinite(result.x) && std::isfinite(result.y) && std::isfinite(result.z);
    }

    inline bool BuildPickingRay(
        const Camera& camera,
        float mouseX,
        float mouseY,
        float viewportWidth,
        float viewportHeight,
        Ray& ray)
    {
        if (viewportWidth <= 0.0f || viewportHeight <= 0.0f || !std::isfinite(mouseX) || !std::isfinite(mouseY))
        {
            return false;
        }

        const auto forward = camera.GetTarget() - camera.GetPosition();
        const auto right = Vector3::Cross({0.0f, 1.0f, 0.0f}, forward.Normalized());
        if(!std::isfinite(forward.Length()) || forward.Length() <= 0.000001f || right.Length() <= 0.000001f)
            return false;

        const float x = 2.0f * mouseX / viewportWidth - 1.0f;
        const float y = 1.0f - 2.0f * mouseY / viewportHeight;

        const Matrix4 viewProjection = camera.GetProjectionMatrix() * camera.GetViewMatrix();
        const Matrix4 inverseViewProjection = viewProjection.Inverse();

        Vector3 nearPoint;
        Vector3 farPoint;

        if (!UnprojectPoint(inverseViewProjection, x, y, 0.0f, nearPoint) ||
            !UnprojectPoint(inverseViewProjection, x, y, 1.0f, farPoint))
        {
            return false;
        }

        const Vector3 difference = farPoint - nearPoint;
        const float distance = difference.Length();

        if (!std::isfinite(distance) || distance <= 0.000001f)
        {
            return false;
        }

        ray.origin = nearPoint;
        ray.direction = difference * (1.0f / distance);
        ray.maxDistance = distance;

        return true;
    }

    inline bool Finite(const Vector3& value)
    {
        return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
    }

    inline Vector3 Point(const Matrix4& matrix, const Vector3& point)
    {
        return {matrix.m[0][0] * point.x + matrix.m[0][1] * point.y + matrix.m[0][2] * point.z + matrix.m[0][3],
            matrix.m[1][0] * point.x + matrix.m[1][1] * point.y + matrix.m[1][2] * point.z + matrix.m[1][3],
            matrix.m[2][0] * point.x + matrix.m[2][1] * point.y + matrix.m[2][2] * point.z + matrix.m[2][3]};
    }

    inline bool IntersectBounds(const Ray& ray, const MeshBounds& bounds, double limit)
    {
        if(!bounds.valid || !Finite(bounds.min) || !Finite(bounds.max)) return false;
        double nearDistance = 0.0, farDistance = (std::min)(double(ray.maxDistance), limit);
        const double origins[] = {ray.origin.x, ray.origin.y, ray.origin.z};
        const double directions[] = {ray.direction.x, ray.direction.y, ray.direction.z};
        const double minimums[] = {bounds.min.x, bounds.min.y, bounds.min.z};
        const double maximums[] = {bounds.max.x, bounds.max.y, bounds.max.z};
        for(size_t axis = 0; axis < 3; ++axis)
        {
            if(minimums[axis] > maximums[axis]) return false;
            if(directions[axis] == 0.0)
            {
                if(origins[axis] < minimums[axis] || origins[axis] > maximums[axis]) return false;
                continue;
            }
            double first = (minimums[axis] - origins[axis]) / directions[axis];
            double last = (maximums[axis] - origins[axis]) / directions[axis];
            if(first > last) std::swap(first, last);
            nearDistance = (std::max)(nearDistance, first);
            farDistance = (std::min)(farDistance, last);
            if(nearDistance > farDistance) return false;
        }
        return farDistance >= 0.0;
    }

    struct DoubleVector
    {
        double x, y, z;
        explicit DoubleVector(const Vector3& value) : x(value.x), y(value.y), z(value.z) {}
        DoubleVector(double a, double b, double c) : x(a), y(b), z(c) {}
        DoubleVector operator-(const DoubleVector& value) const { return {x - value.x, y - value.y, z - value.z}; }
        DoubleVector Cross(const DoubleVector& value) const
        {
            return {y * value.z - z * value.y, z * value.x - x * value.z, x * value.y - y * value.x};
        }
        double Dot(const DoubleVector& value) const { return x * value.x + y * value.y + z * value.z; }
    };

    inline bool IntersectTriangle(const Ray& ray, const Vector3& a, const Vector3& b,
        const Vector3& c, double& nearest)
    {
        if(!Finite(a) || !Finite(b) || !Finite(c)) return false;
        const auto edge1 = DoubleVector(b) - DoubleVector(a);
        const auto edge2 = DoubleVector(c) - DoubleVector(a);
        const DoubleVector direction(ray.direction);
        const auto cross = direction.Cross(edge2);
        const double determinant = edge1.Dot(cross);
        const auto normal = edge1.Cross(edge2);
        const double area = std::sqrt(normal.Dot(normal));
        // Match the existing placement raycast: two-sided Moller-Trumbore.
        if(!std::isfinite(area) || area == 0.0 || !std::isfinite(determinant) ||
            std::abs(determinant) <= area * 1e-12) return false;
        const auto offset = DoubleVector(ray.origin) - DoubleVector(a);
        const double u = offset.Dot(cross) / determinant;
        if(!std::isfinite(u) || u < 0.0 || u > 1.0) return false;
        const auto offsetCross = offset.Cross(edge1);
        const double v = direction.Dot(offsetCross) / determinant;
        const double distance = edge2.Dot(offsetCross) / determinant;
        if(!std::isfinite(v) || v < 0.0 || u + v > 1.0 || !std::isfinite(distance) ||
            distance <= 0.0 || distance > nearest || distance > ray.maxDistance) return false;
        nearest = distance;
        return true;
    }

    inline bool IntersectMesh(const Ray& ray, const Matrix4& model, const MeshBounds& localBounds,
        const ImportedMesh& geometry, bool indexed, std::span<const MeshSubmesh> ranges, double& nearest)
    {
        if(!Finite(ray.origin) || !Finite(ray.direction) || !std::isfinite(ray.maxDistance) ||
            ray.maxDistance <= 0.0f || !localBounds.valid || geometry.vertices.empty() ||
            !Finite(localBounds.min) || !Finite(localBounds.max) || localBounds.min.x > localBounds.max.x ||
            localBounds.min.y > localBounds.max.y || localBounds.min.z > localBounds.max.z || std::isnan(nearest)) return false;
        for(const auto& row : model.m)
            for(const float value : row)
                if(!std::isfinite(value)) return false;
        MeshBounds worldBounds;
        for(unsigned int corner = 0; corner < 8; ++corner)
        {
            const auto world = Point(model, {corner & 1 ? localBounds.max.x : localBounds.min.x,
                corner & 2 ? localBounds.max.y : localBounds.min.y,
                corner & 4 ? localBounds.max.z : localBounds.min.z});
            if(!Finite(world)) return false;
            if(!worldBounds.valid) { worldBounds = {world, world, true}; continue; }
            worldBounds.min = {(std::min)(worldBounds.min.x, world.x), (std::min)(worldBounds.min.y, world.y), (std::min)(worldBounds.min.z, world.z)};
            worldBounds.max = {(std::max)(worldBounds.max.x, world.x), (std::max)(worldBounds.max.y, world.y), (std::max)(worldBounds.max.z, world.z)};
        }
        if(!IntersectBounds(ray, worldBounds, nearest)) return false;

        bool hit = false;
        const auto triangle = [&](size_t first)
        {
            const size_t a = indexed ? geometry.indices[first] : first;
            const size_t b = indexed ? geometry.indices[first + 1] : first + 1;
            const size_t c = indexed ? geometry.indices[first + 2] : first + 2;
            if(a >= geometry.vertices.size() || b >= geometry.vertices.size() || c >= geometry.vertices.size()) return;
            const auto position = [&](size_t index)
            {
                const auto& value = geometry.vertices[index].position;
                return Point(model, {value[0], value[1], value[2]});
            };
            // World-space triangles preserve the original ray parameter under
            // nonuniform/mirrored scales and avoid singular inverse transforms.
            hit = IntersectTriangle(ray, position(a), position(b), position(c), nearest) || hit;
        };
        if(!indexed)
        {
            for(size_t first = 0; first + 2 < geometry.vertices.size(); first += 3) triangle(first);
        }
        else
        {
            for(const auto& range : ranges)
            {
                if(range.firstIndex > geometry.indices.size() || range.indexCount > geometry.indices.size() - range.firstIndex ||
                    range.firstIndex % 3 != 0 || range.indexCount % 3 != 0) continue;
                const size_t end = size_t(range.firstIndex) + range.indexCount;
                for(size_t first = range.firstIndex; first < end; first += 3) triangle(first);
            }
        }
        return hit;
    }

    // No candidate array or per-click allocation. The callback borrows current scene geometry.
    template<class IntersectCandidate>
    size_t PickNearest(size_t count, double limit, IntersectCandidate intersect)
    {
        size_t selected = (std::numeric_limits<size_t>::max)();
        double nearest = limit;
        for(size_t index = 0; index < count; ++index)
        {
            double distance = nearest;
            if(intersect(index, distance) && std::isfinite(distance) && distance > 0.0 && distance <= nearest &&
                (selected == (std::numeric_limits<size_t>::max)() || distance < nearest))
            {
                nearest = distance;
                selected = index;
            }
        }
        return selected;
    }
}

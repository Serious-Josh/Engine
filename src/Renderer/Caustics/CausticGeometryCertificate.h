#pragma once

#include "Renderer/Vertex.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <span>

namespace CausticContracts::Geometry
{
    static_assert(std::numeric_limits<float>::is_iec559 && std::numeric_limits<float>::digits == 24);
    static_assert(std::numeric_limits<double>::is_iec559 && std::numeric_limits<double>::digits == 53);
    inline constexpr std::uint32_t MaximumTriangles = 4096;
    inline constexpr std::uint32_t MaximumElements = 12288;
    inline constexpr std::uint32_t NoPrimitive = UINT32_MAX;

    enum class Topology { NonIndexedTriangles, IndexedUInt32Triangles };
    struct FixtureTransform
    {
        int scaleExponent = 0;
        std::array<float, 3> translation{};
        bool operator==(const FixtureTransform&) const = default;
    };
    struct FixturePolicy
    {
        bool smoothed = false;
        bool normalMapped = false;
        bool dynamicGeometry = false;
    };
    struct FixtureInput
    {
        std::span<const Vertex> vertices;
        std::span<const std::uint32_t> indices;
        Topology topology = Topology::NonIndexedTriangles;
        std::uint32_t firstPrimitive = 0;
        std::uint32_t primitiveCount = 0;
        FixtureTransform transform{};
        FixturePolicy policy{};
        std::uint64_t fixtureID = 0;
    };
    enum class Status { Success, InvalidInput, LimitExceeded, Unsupported, Degenerate, NormalMismatch };
    struct Result { Status status; std::uint32_t offendingPrimitive = NoPrimitive; };
    struct Double3
    {
        std::array<double, 3> components{};
        bool operator==(const Double3&) const = default;
    };
    // Fixture summary only. No live publication, GPU validity flags or source identity hash.
    // Source bytes remain caller-owned and immutable; equal summaries do not prove equal sources.
    struct FixtureCertificate
    {
        std::uint64_t fixtureID = 0;
        Topology topology = Topology::NonIndexedTriangles;
        std::uint32_t vertexCount = 0, indexCount = 0;
        std::uint32_t firstPrimitive = 0, primitiveCount = 0;
        FixtureTransform transform{};
        std::uint32_t policyVersion = 1; // static, unsmoothed, unmapped axis/grid policy
        Double3 worldMinimum{}, worldMaximum{}, sphereCenter{};
        double sphereRadius = 0;
        bool operator==(const FixtureCertificate&) const = default;
    };

    namespace Detail
    {
        inline bool DecodeGrid(float value, std::int64_t& grid) noexcept
        {
            const double scaled = double(value) * 1024.0;
            if(!std::isfinite(scaled) || scaled < -1048576.0 || scaled > 1048576.0 ||
                std::trunc(scaled) != scaled)
                return false;
            grid = static_cast<std::int64_t>(scaled);
            return double(grid) / 1024.0 == double(value);
        }

        inline bool Determinant(std::int64_t a, std::int64_t b, std::int64_t c,
            std::int64_t d, std::int64_t& result) noexcept
        {
            // Checked operand bounds imply products <= 2^42 and difference <= 2^43.
            constexpr std::int64_t edgeLimit = 2097152;
            for(const auto value : {a, b, c, d})
                if(value < -edgeLimit || value > edgeLimit) return false;
            result = a * b - c * d;
            return true;
        }
    }

    // Validation order: topology/ID, span limits/shape, range, policy, transform,
    // then domain primitives in order: indices, positions/normals finite, degeneracy,
    // axis plane, exact normals. Finally bounds. No output writes before success.
    // Spans must satisfy the C++ span lifetime/storage preconditions; dangling memory
    // cannot be detected. Only requested-domain attributes and indices are inspected.
    inline Result BuildFixtureCertificate(const FixtureInput& input, FixtureCertificate& output) noexcept
    {
        if((input.topology != Topology::NonIndexedTriangles &&
            input.topology != Topology::IndexedUInt32Triangles) || input.fixtureID == 0)
            return {Status::InvalidInput};
        if(input.vertices.size() > MaximumElements || input.indices.size() > MaximumElements)
            return {Status::LimitExceeded};
        const bool indexed = input.topology == Topology::IndexedUInt32Triangles;
        const auto elements = indexed ? input.indices.size() : input.vertices.size();
        if(input.vertices.empty() || elements == 0 || elements % 3 != 0 ||
            (!indexed && !input.indices.empty()))
            return {Status::InvalidInput};
        const auto triangles = static_cast<std::uint32_t>(elements / 3);
        if(input.primitiveCount == 0 || input.firstPrimitive >= triangles ||
            input.primitiveCount > triangles - input.firstPrimitive)
            return {Status::InvalidInput};
        if(input.policy.smoothed || input.policy.normalMapped || input.policy.dynamicGeometry)
            return {Status::Unsupported};
        if(input.transform.scaleExponent < -8 || input.transform.scaleExponent > 8)
            return {Status::Unsupported};
        FixtureCertificate candidate{};
        candidate.fixtureID = input.fixtureID;
        candidate.topology = input.topology;
        candidate.vertexCount = static_cast<std::uint32_t>(input.vertices.size());
        candidate.indexCount = static_cast<std::uint32_t>(input.indices.size());
        candidate.firstPrimitive = input.firstPrimitive;
        candidate.primitiveCount = input.primitiveCount;
        candidate.transform.scaleExponent = input.transform.scaleExponent;
        for(std::size_t axis = 0; axis < 3; ++axis)
        {
            std::int64_t translation = 0;
            if(!Detail::DecodeGrid(input.transform.translation[axis], translation))
                return {Status::InvalidInput};
            candidate.transform.translation[axis] = static_cast<float>(double(translation) / 1024.0);
            candidate.worldMinimum.components[axis] = std::numeric_limits<double>::infinity();
            candidate.worldMaximum.components[axis] = -std::numeric_limits<double>::infinity();
        }
        for(std::uint32_t primitive = input.firstPrimitive;
            primitive < input.firstPrimitive + input.primitiveCount; ++primitive)
        {
            std::array<std::uint32_t, 3> indices{};
            for(std::uint32_t corner = 0; corner < 3; ++corner)
            {
                const auto offset = primitive * 3 + corner;
                indices[corner] = indexed ? input.indices[offset] : offset;
                if(indices[corner] >= input.vertices.size()) return {Status::InvalidInput, primitive};
            }
            std::array<std::array<std::int64_t, 3>, 3> grid{};
            for(std::size_t corner = 0; corner < 3; ++corner)
                for(std::size_t axis = 0; axis < 3; ++axis)
                {
                    const auto& vertex = input.vertices[indices[corner]];
                    if(!Detail::DecodeGrid(vertex.position[axis], grid[corner][axis]) ||
                        !std::isfinite(vertex.normal[axis])) return {Status::InvalidInput, primitive};
                }
            std::array<std::int64_t, 3> cross{};
            unsigned int constantAxes = 0;
            for(std::size_t axis = 0; axis < 3; ++axis)
            {
                const auto j = (axis + 1) % 3, k = (axis + 2) % 3;
                if(!Detail::Determinant(grid[1][j] - grid[0][j], grid[2][k] - grid[0][k],
                    grid[1][k] - grid[0][k], grid[2][j] - grid[0][j], cross[axis]))
                    return {Status::LimitExceeded, primitive};
                if(grid[0][axis] == grid[1][axis] && grid[0][axis] == grid[2][axis]) ++constantAxes;
            }
            if(cross[0] == 0 && cross[1] == 0 && cross[2] == 0) return {Status::Degenerate, primitive};
            if(constantAxes != 1) return {Status::Unsupported, primitive};
            for(std::size_t corner = 0; corner < 3; ++corner)
                for(std::size_t axis = 0; axis < 3; ++axis)
                {
                    const float expected = cross[axis] == 0 ? 0.0f : (cross[axis] > 0 ? 1.0f : -1.0f);
                    if(input.vertices[indices[corner]].normal[axis] != expected)
                        return {Status::NormalMismatch, primitive};
                    // Dyadic coordinates within the documented range are exact in binary64.
                    const double world = std::ldexp(double(grid[corner][axis]) / 1024.0,
                        input.transform.scaleExponent) + double(candidate.transform.translation[axis]);
                    auto& minimum = candidate.worldMinimum.components[axis];
                    auto& maximum = candidate.worldMaximum.components[axis];
                    if(world < minimum) minimum = world;
                    if(world > maximum) maximum = world;
                }
        }
        double l1Radius = 0;
        for(std::size_t axis = 0; axis < 3; ++axis)
        {
            const double minimum = candidate.worldMinimum.components[axis];
            const double maximum = candidate.worldMaximum.components[axis];
            const double center = (minimum + maximum) / 2.0;
            candidate.sphereCenter.components[axis] = center;
            const double low = center - minimum, high = maximum - center;
            l1Radius += low > high ? low : high;
        }
        candidate.sphereRadius = std::nextafter(l1Radius, std::numeric_limits<double>::infinity());
        if(!(candidate.sphereRadius > 0) || !std::isfinite(candidate.sphereRadius))
            return {Status::InvalidInput};
        output = candidate;
        return {Status::Success};
    }
}

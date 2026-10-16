#include "ViewportMarquee.h"
#include <array>
#include <cmath>
#include <limits>

namespace ViewportMarquee
{
Rectangle Normalize(ImVec2 anchor, ImVec2 cursor)
{
    return {{(std::min)(anchor.x, cursor.x), (std::min)(anchor.y, cursor.y)},
        {(std::max)(anchor.x, cursor.x), (std::max)(anchor.y, cursor.y)}};
}
bool Overlaps(const Rectangle& first, const Rectangle& second)
{
    return first.minimum.x <= second.maximum.x && first.maximum.x >= second.minimum.x &&
        first.minimum.y <= second.maximum.y && first.maximum.y >= second.minimum.y;
}

namespace
{
struct HomogeneousPoint { double coordinates[4]; };
HomogeneousPoint Transform(const Matrix4& matrix, const std::array<double, 3>& point)
{
    HomogeneousPoint result{};
    for(unsigned int row = 0; row < 4; ++row)
    {
        result.coordinates[row] = matrix[row][0] * point[0] + matrix[row][1] * point[1] +
            matrix[row][2] * point[2] + matrix[row][3];
    }
    return result;
}
bool ClipInterval(double firstDistance, double secondDistance, double& first, double& last)
{
    if(firstDistance < 0 && secondDistance < 0) return false;
    if(firstDistance >= 0 && secondDistance >= 0) return true;
    const double crossing = firstDistance / (firstDistance - secondDistance);
    if(firstDistance < 0) first = (std::max)(first, crossing);
    else last = (std::min)(last, crossing);
    return first <= last;
}
bool ClipFrustum(const HomogeneousPoint& start, const HomogeneousPoint& end, double& first, double& last)
{
    const auto distances = [](const HomogeneousPoint& point)
    {
        const auto& c = point.coordinates;
        return std::array<double, 7>{c[0]+c[3], c[3]-c[0], c[1]+c[3], c[3]-c[1], c[2], c[3]-c[2], c[3]-1e-7};
    };
    const auto startDistances = distances(start);
    const auto endDistances = distances(end);
    for(unsigned int plane = 0; plane < startDistances.size(); ++plane)
    {
        if(!ClipInterval(startDistances[plane], endDistances[plane], first, last)) return false;
    }
    return true;
}
bool FiniteMatrix(const Matrix4& matrix)
{
    for(const auto& row : matrix.m)
        for(float coordinate : row)
            if(!std::isfinite(coordinate)) return false;
    return true;
}
}

bool ProjectBounds(const MeshBounds& bounds, const Matrix4& clipFromLocal, ImVec2 imageSize, Rectangle& output)
{
    if(!bounds.valid || imageSize.x <= 0 || imageSize.y <= 0 || !std::isfinite(imageSize.x) ||
        !std::isfinite(imageSize.y) || !FiniteMatrix(clipFromLocal)) return false;
    const std::array<double, 3> minimum{bounds.min.x, bounds.min.y, bounds.min.z};
    const std::array<double, 3> maximum{bounds.max.x, bounds.max.y, bounds.max.z};
    for(unsigned int axis = 0; axis < 3; ++axis)
        if(!std::isfinite(minimum[axis]) || !std::isfinite(maximum[axis]) || minimum[axis] > maximum[axis]) return false;

    const Matrix4 localFromClip = clipFromLocal.Inverse();
    if(!FiniteMatrix(localFromClip)) return false;
    const Matrix4 inverseCheck = clipFromLocal * localFromClip;
    for(unsigned int row = 0; row < 4; ++row)
        for(unsigned int column = 0; column < 4; ++column)
            if(!std::isfinite(inverseCheck[row][column]) ||
                std::abs(inverseCheck[row][column] - (row == column ? 1.0f : 0.0f)) > 0.01f) return false;

    Rectangle projected{{imageSize.x, imageSize.y}, {0, 0}};
    bool hasVisiblePoint = false;
    const auto include = [&](const HomogeneousPoint& point)
    {
        const auto& c = point.coordinates;
        if(c[3] < 1e-7) return;
        const float x = static_cast<float>((std::clamp)(c[0]/c[3], -1.0, 1.0));
        const float y = static_cast<float>((std::clamp)(c[1]/c[3], -1.0, 1.0));
        const ImVec2 screen((x+1)*0.5f*imageSize.x, (1-y)*0.5f*imageSize.y);
        projected.minimum.x = (std::min)(projected.minimum.x, screen.x);
        projected.minimum.y = (std::min)(projected.minimum.y, screen.y);
        projected.maximum.x = (std::max)(projected.maximum.x, screen.x);
        projected.maximum.y = (std::max)(projected.maximum.y, screen.y);
        hasVisiblePoint = true;
    };
    const auto includeSegment = [&](const HomogeneousPoint& start, const HomogeneousPoint& end, double first, double last)
    {
        for(double parameter : {first, last})
        {
            HomogeneousPoint point{};
            for(unsigned int coordinate = 0; coordinate < 4; ++coordinate)
                point.coordinates[coordinate] = start.coordinates[coordinate] +
                    (end.coordinates[coordinate] - start.coordinates[coordinate]) * parameter;
            include(point);
        }
    };

    std::array<std::array<double, 3>, 8> boxCorners{}, frustumCorners{};
    for(unsigned int corner = 0; corner < 8; ++corner)
    {
        for(unsigned int axis = 0; axis < 3; ++axis)
            boxCorners[corner][axis] = (corner & (1u << axis)) ? maximum[axis] : minimum[axis];
        const auto local = Transform(localFromClip, {(corner & 1) ? 1.0 : -1.0,
            (corner & 2) ? 1.0 : -1.0, (corner & 4) ? 1.0 : 0.0});
        if(!std::isfinite(local.coordinates[3]) || std::abs(local.coordinates[3]) < 1e-12) return false;
        for(unsigned int axis = 0; axis < 3; ++axis)
        {
            frustumCorners[corner][axis] = local.coordinates[axis] / local.coordinates[3];
            if(!std::isfinite(frustumCorners[corner][axis])) return false;
        }
    }

    // Intersection vertices can lie on edges of either convex volume. Clipping
    // both sets handles near-plane crossings AND a box enclosing the entire frustum.
    for(unsigned int corner = 0; corner < 8; ++corner)
    {
        for(unsigned int axis = 0; axis < 3; ++axis)
        {
            const unsigned int other = corner | (1u << axis);
            if(other == corner) continue;
            const auto start = Transform(clipFromLocal, boxCorners[corner]);
            const auto end = Transform(clipFromLocal, boxCorners[other]);
            double first = 0, last = 1;
            if(ClipFrustum(start, end, first, last)) includeSegment(start, end, first, last);

            first = 0; last = 1;
            bool intersectsBox = true;
            for(unsigned int boxAxis = 0; boxAxis < 3; ++boxAxis)
            {
                const double startCoordinate = frustumCorners[corner][boxAxis];
                const double endCoordinate = frustumCorners[other][boxAxis];
                if(!ClipInterval(startCoordinate-minimum[boxAxis], endCoordinate-minimum[boxAxis], first, last) ||
                    !ClipInterval(maximum[boxAxis]-startCoordinate, maximum[boxAxis]-endCoordinate, first, last))
                {
                    intersectsBox = false;
                    break;
                }
            }
            if(intersectsBox)
                includeSegment(Transform(clipFromLocal, frustumCorners[corner]), Transform(clipFromLocal, frustumCorners[other]), first, last);
        }
    }
    if(hasVisiblePoint) output = projected;
    return hasVisiblePoint;
}

void CombineSelection(std::span<const SceneSelection::Identity> original,
    std::span<const SceneSelection::Identity> candidates, SelectionMode mode, std::vector<SceneSelection::Identity>& output)
{
    output.clear();
    if(mode != SelectionMode::Replace) output.assign(original.begin(), original.end());
    for(const auto& candidate : candidates)
    {
        const auto found = std::find(output.begin(), output.end(), candidate);
        if(found == output.end()) output.push_back(candidate);
        else if(mode == SelectionMode::Toggle) output.erase(found);
    }
}
}

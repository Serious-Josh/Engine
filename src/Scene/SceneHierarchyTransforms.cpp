#include "Scene/SceneHierarchyTransforms.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace SceneHierarchy
{
namespace
{
    using Column = std::array<double, 3>;

    bool Fail(std::string* error, const char* message)
    {
        if(error)
        {
            *error = message;
        }

        return false;
    }

    Column LinearColumn(const Matrix4& matrix, size_t column)
    {
        return {matrix.m[0][column], matrix.m[1][column], matrix.m[2][column]};
    }

    double Dot(const Column& left, const Column& right)
    {
        return left[0] * right[0] + left[1] * right[1] + left[2] * right[2];
    }

    double Length(const Column& column)
    {
        return std::sqrt(Dot(column, column));
    }

    Column Cross(const Column& left, const Column& right)
    {
        return {
            left[1] * right[2] - left[2] * right[1],
            left[2] * right[0] - left[0] * right[2],
            left[0] * right[1] - left[1] * right[0]};
    }

    // Determinant of the upper-left 3x3 linear part, as the scalar triple product.
    double LinearDeterminant(const Matrix4& matrix)
    {
        return Dot(LinearColumn(matrix, 0), Cross(LinearColumn(matrix, 1), LinearColumn(matrix, 2)));
    }

    double MaximumMagnitude(const Matrix4& matrix)
    {
        double magnitude = 0.0;

        for(size_t row = 0; row < 3; ++row)
        {
            for(size_t column = 0; column < 4; ++column)
            {
                magnitude = (std::max)(magnitude, std::abs(static_cast<double>(matrix.m[row][column])));
            }
        }

        return magnitude;
    }

    // Scale-aware comparison: tolerance grows with the larger matrix magnitude, never below 1.
    bool MatricesMatch(const Matrix4& expected, const Matrix4& actual, double tolerance)
    {
        const double scale = (std::max)({1.0, MaximumMagnitude(expected), MaximumMagnitude(actual)});

        for(size_t row = 0; row < 4; ++row)
        {
            for(size_t column = 0; column < 4; ++column)
            {
                const double difference = std::abs(
                    static_cast<double>(expected.m[row][column]) - static_cast<double>(actual.m[row][column]));

                if(!(difference <= tolerance * scale))
                {
                    return false;
                }
            }
        }

        return true;
    }

    bool IsFiniteVector(const Vector3& value)
    {
        return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
    }

    // Double-precision product of two affine matrices, rounded once to float storage.
    Matrix4 MultiplyAffine(const Matrix4& left, const Matrix4& right)
    {
        Matrix4 product = Matrix4::Identity();

        for(size_t row = 0; row < 3; ++row)
        {
            for(size_t column = 0; column < 4; ++column)
            {
                double value = column == 3 ? static_cast<double>(left.m[row][3]) : 0.0;

                for(size_t inner = 0; inner < 3; ++inner)
                {
                    value += static_cast<double>(left.m[row][inner]) * static_cast<double>(right.m[inner][column]);
                }

                product.m[row][column] = static_cast<float>(value);
            }
        }

        return product;
    }

    // Rejects bases outside the connected-branch policy. Shared by inversion and decomposition.
    bool ValidateBasis(const Matrix4& matrix, std::string* error, const NumericalPolicy& policy)
    {
        const Column columns[3] = {LinearColumn(matrix, 0), LinearColumn(matrix, 1), LinearColumn(matrix, 2)};
        double lengthProduct = 1.0;

        for(const auto& column : columns)
        {
            const double length = Length(column);

            if(!(length >= policy.minimumAxisLength) || !(length <= policy.maximumAxisLength))
            {
                return Fail(error, "Hierarchy transform scale is outside the admitted range.");
            }

            lengthProduct *= length;
        }

        const double normalizedVolume = std::abs(LinearDeterminant(matrix)) / lengthProduct;

        if(!(normalizedVolume >= policy.minimumNormalizedVolume))
        {
            return Fail(error, "Hierarchy transform is singular or insufficiently conditioned.");
        }

        return true;
    }
}

bool IsFiniteAffine(const Matrix4& matrix) noexcept
{
    for(size_t row = 0; row < 4; ++row)
    {
        for(size_t column = 0; column < 4; ++column)
        {
            if(!std::isfinite(matrix.m[row][column]))
            {
                return false;
            }
        }
    }

    return matrix.m[3][0] == 0.0f && matrix.m[3][1] == 0.0f && matrix.m[3][2] == 0.0f && matrix.m[3][3] == 1.0f;
}

bool TryMultiplyAffine(const Matrix4& parent, const Matrix4& local, Matrix4& product) noexcept
{
    if(!IsFiniteAffine(parent) || !IsFiniteAffine(local))
    {
        return false;
    }

    const Matrix4 candidate = MultiplyAffine(parent, local);

    if(!IsFiniteAffine(candidate))
    {
        return false;
    }

    product = candidate;
    return true;
}

bool HasValidNormalMatrix(const Matrix4& world) noexcept
{
    const double determinant = LinearDeterminant(world);
    return std::isfinite(determinant) && std::abs(determinant) >= kRendererNormalDeterminant;
}

bool TryInvertAffine(const Matrix4& matrix, Matrix4& inverse, std::string* error, const NumericalPolicy& policy)
{
    if(!IsFiniteAffine(matrix))
    {
        return Fail(error, "Hierarchy transform is not a finite affine matrix.");
    }

    if(!ValidateBasis(matrix, error, policy))
    {
        return false;
    }

    // Inverse linear part = adjugate / determinant; rows of the inverse are the
    // cross products of column pairs. Translation becomes -inverseLinear * t.
    const Column columns[3] = {LinearColumn(matrix, 0), LinearColumn(matrix, 1), LinearColumn(matrix, 2)};
    const Column rows[3] = {Cross(columns[1], columns[2]), Cross(columns[2], columns[0]), Cross(columns[0], columns[1])};
    const double inverseDeterminant = 1.0 / LinearDeterminant(matrix);
    const Column translation = {matrix.m[0][3], matrix.m[1][3], matrix.m[2][3]};
    Matrix4 candidate = Matrix4::Identity();

    for(size_t row = 0; row < 3; ++row)
    {
        for(size_t column = 0; column < 3; ++column)
        {
            candidate.m[row][column] = static_cast<float>(rows[row][column] * inverseDeterminant);
        }

        candidate.m[row][3] = static_cast<float>(-Dot(rows[row], translation) * inverseDeterminant);
    }

    if(!IsFiniteAffine(candidate) || !MatricesMatch(Matrix4::Identity(), MultiplyAffine(matrix, candidate),
        policy.reconstructionTolerance))
    {
        return Fail(error, "Hierarchy transform inversion failed its round-trip check.");
    }

    inverse = candidate;
    return true;
}

bool TryDecomposeLocalTransform(const Matrix4& local, Transform& transform, std::string* error,
    const NumericalPolicy& policy)
{
    if(!IsFiniteAffine(local))
    {
        return Fail(error, "Local transform is not a finite affine matrix.");
    }

    if(!ValidateBasis(local, error, policy))
    {
        return false;
    }

    Column columns[3] = {LinearColumn(local, 0), LinearColumn(local, 1), LinearColumn(local, 2)};
    double scales[3] = {Length(columns[0]), Length(columns[1]), Length(columns[2])};

    // Authored TRS has orthogonal basis columns; anything else is inherited shear.
    for(size_t first = 0; first < 3; ++first)
    {
        for(size_t second = first + 1; second < 3; ++second)
        {
            const double cosine = Dot(columns[first], columns[second]) / (scales[first] * scales[second]);

            if(!(std::abs(cosine) <= policy.maximumLocalShear))
            {
                return Fail(error, "Local transform contains shear that authored TRS cannot represent.");
            }
        }
    }

    // A reflection is represented deterministically as a negative X scale.
    if(LinearDeterminant(local) < 0.0)
    {
        scales[0] = -scales[0];
    }

    double rotation[3][3];

    for(size_t column = 0; column < 3; ++column)
    {
        for(size_t row = 0; row < 3; ++row)
        {
            rotation[row][column] = columns[column][row] / scales[column];
        }
    }

    // R = Rz(z) * Ry(y) * Rx(x): R[2][0] = -sin(y), R[2][1] = cos(y)sin(x), R[2][2] = cos(y)cos(x),
    // R[1][0] = sin(z)cos(y), R[0][0] = cos(z)cos(y).
    const double sinePitch = std::clamp(-rotation[2][0], -1.0, 1.0);
    const double pitch = std::asin(sinePitch);
    double roll = 0.0;
    double yaw = 0.0;
    constexpr double kGimbalCosine = 1.0e-6;

    if(std::cos(pitch) > kGimbalCosine)
    {
        roll = std::atan2(rotation[2][1], rotation[2][2]);
        yaw = std::atan2(rotation[1][0], rotation[0][0]);
    }
    else
    {
        // Gimbal lock: only x - z (or x + z) is observable. Fix z = 0 so that
        // R = Ry(pitch) * Rx(x), whose row 1 is (0, cos(x), -sin(x)).
        roll = std::atan2(-rotation[1][2], rotation[1][1]);
    }

    Transform candidate;
    candidate.position = Vector3{local.m[0][3], local.m[1][3], local.m[2][3]};
    candidate.rotation = Vector3{static_cast<float>(roll), static_cast<float>(pitch), static_cast<float>(yaw)};
    candidate.scale = Vector3{static_cast<float>(scales[0]), static_cast<float>(scales[1]), static_cast<float>(scales[2])};

    if(!IsFiniteVector(candidate.position) || !IsFiniteVector(candidate.rotation) || !IsFiniteVector(candidate.scale))
    {
        return Fail(error, "Local transform factorization produced nonfinite values.");
    }

    if(!MatricesMatch(local, candidate.GetModelMatrix(), policy.reconstructionTolerance))
    {
        return Fail(error, "Local transform failed TRS reconstruction.");
    }

    transform = candidate;
    return true;
}

bool TryConvertWorldToLocal(const Matrix4& parentWorld, const Matrix4& desiredWorld, Transform& local,
    std::string* error, const NumericalPolicy& policy)
{
    if(!IsFiniteAffine(desiredWorld))
    {
        return Fail(error, "Desired world transform is not a finite affine matrix.");
    }

    Matrix4 inverseParent;

    if(!TryInvertAffine(parentWorld, inverseParent, error, policy))
    {
        return false;
    }

    Matrix4 candidateMatrix;

    if(!TryMultiplyAffine(inverseParent, desiredWorld, candidateMatrix))
    {
        return Fail(error, "Candidate local transform overflowed.");
    }

    Transform candidate;

    if(!TryDecomposeLocalTransform(candidateMatrix, candidate, error, policy))
    {
        return false;
    }

    // Check the result the user will see, not only the intermediate local matrix.
    Matrix4 reconstructedWorld;

    if(!TryMultiplyAffine(parentWorld, candidate.GetModelMatrix(), reconstructedWorld) ||
        !MatricesMatch(desiredWorld, reconstructedWorld, policy.reconstructionTolerance))
    {
        return Fail(error, "Converted local transform does not reproduce the desired world transform.");
    }

    local = candidate;
    return true;
}

const char* EvaluateChildWorld(const Matrix4& parentWorld, const Transform& local, Matrix4& world,
    const NumericalPolicy& policy) noexcept
{
    const bool scaleAdmitted =
        std::abs(local.scale.x) >= policy.minimumAxisLength &&
        std::abs(local.scale.y) >= policy.minimumAxisLength &&
        std::abs(local.scale.z) >= policy.minimumAxisLength;

    if(!scaleAdmitted)
    {
        return "A parented object's local scale is too close to zero.";
    }

    if(!TryMultiplyAffine(parentWorld, local.GetModelMatrix(), world))
    {
        return "A hierarchy world transform is nonfinite or not affine.";
    }

    if(!HasValidNormalMatrix(world))
    {
        return "A hierarchy world transform has no valid normal matrix.";
    }

    return nullptr;
}

const char* ValidateParentWorld(const Matrix4& world, const NumericalPolicy& policy) noexcept
{
    // A parent's world must stay invertible so its children can later be converted (Keep World).
    Matrix4 inverse;

    if(!TryInvertAffine(world, inverse, nullptr, policy))
    {
        return "A hierarchy parent world is singular or insufficiently conditioned.";
    }

    return nullptr;
}

bool ValidateConnectedTransforms(std::span<const SceneObjectDocument> objects, const Forest& forest,
    std::string* error)
{
    if(forest.objectIDs.size() != objects.size() || forest.parentBeforeChild.size() != objects.size())
    {
        return Fail(error, "Hierarchy forest does not describe these objects.");
    }

    std::vector<Matrix4> worlds(objects.size());

    for(const size_t index : forest.parentBeforeChild)
    {
        const auto& object = objects[index];
        const char* failure = nullptr;

        if(forest.parents[index])
        {
            failure = EvaluateChildWorld(worlds[*forest.parents[index]], object.transform, worlds[index]);
        }
        else
        {
            worlds[index] = object.transform.GetModelMatrix();
        }

        if(!failure && !forest.children[index].empty())
        {
            failure = ValidateParentWorld(worlds[index]);
        }

        if(failure)
        {
            if(error)
            {
                *error = std::string(failure) + " Object: " + object.id;
            }

            return false;
        }
    }

    return true;
}

std::vector<size_t> NormalizeSelectionRoots(const Forest& forest, std::span<const size_t> selected)
{
    std::vector<bool> isSelected(forest.objectIDs.size(), false);

    for(const size_t index : selected)
    {
        if(index < isSelected.size())
        {
            isSelected[index] = true;
        }
    }

    std::vector<size_t> roots;
    roots.reserve(selected.size());
    std::vector<bool> emitted(forest.objectIDs.size(), false);

    for(const size_t index : selected)
    {
        if(index >= isSelected.size() || emitted[index])
        {
            continue;
        }

        emitted[index] = true;

        // Ancestor walk is bounded by the validated forest's object count.
        bool ancestorSelected = false;
        auto ancestor = forest.parents[index];

        for(size_t steps = 0; ancestor && steps < forest.objectIDs.size(); ++steps)
        {
            if(isSelected[*ancestor])
            {
                ancestorSelected = true;
                break;
            }

            ancestor = forest.parents[*ancestor];
        }

        if(!ancestorSelected)
        {
            roots.push_back(index);
        }
    }

    return roots;
}
}

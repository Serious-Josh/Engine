#pragma once

#include "Scene/SceneHierarchy.h"
#include "Scene/Transform.h"

#include <span>
#include <string>
#include <vector>

// H.3 hierarchy transform math. Matrices use Engine's convention: row-indexed storage,
// column-vector mathematics, translation in [0..2][3], model = T * Rz * Ry * Rx * S.
// Every function is value-only and leaves its outputs untouched when it rejects.
namespace SceneHierarchy
{
    // Thresholds for hierarchy-connected branches only. Historical flat roots keep their
    // existing admission. Reasoning and fixtures: Documentation/Scene/SceneHierarchyWorldTransforms.md.
    struct NumericalPolicy
    {
        // Shortest admitted basis column (scale magnitude) of a connected local or parent world.
        double minimumAxisLength = 1.0e-4;

        // Longest admitted basis column; keeps products of 8192-deep chains finite in float.
        double maximumAxisLength = 1.0e6;

        // |det| / (|c0| * |c1| * |c2|): 1 for orthogonal bases, 0 for degenerate ones.
        double minimumNormalizedVolume = 1.0e-3;

        // Largest |cos| between two local basis columns still representable as TRS.
        double maximumLocalShear = 1.0e-4;

        // Recomposition error relative to the matrix magnitude.
        double reconstructionTolerance = 1.0e-4;
    };

    // Matches Matrix4::Inverse's absolute determinant cutoff, below which the renderer's
    // inverse-transpose normal matrix silently becomes identity.
    inline constexpr double kRendererNormalDeterminant = 1.0e-6;

    bool IsFiniteAffine(const Matrix4& matrix) noexcept;

    // parent * local, rejecting nonfinite or non-affine products.
    bool TryMultiplyAffine(const Matrix4& parent, const Matrix4& local, Matrix4& product) noexcept;

    // True when the renderer's existing normal-matrix computation is valid for this world.
    bool HasValidNormalMatrix(const Matrix4& world) noexcept;

    // Checked affine inverse. Never returns Matrix4::Inverse's identity fallback.
    bool TryInvertAffine(const Matrix4& matrix, Matrix4& inverse, std::string* error = nullptr,
        const NumericalPolicy& policy = {});

    // Exact T * Rz * Ry * Rx * S factorization with recomposition check. Rejects shear.
    // Reflections negate X scale; gimbal lock (|pitch| = 90 degrees) fixes Z rotation to 0.
    bool TryDecomposeLocalTransform(const Matrix4& local, Transform& transform, std::string* error = nullptr,
        const NumericalPolicy& policy = {});

    // CandidateLocal = inverse(parentWorld) * desiredWorld, then factorized and checked so that
    // parentWorld * Local(candidate) reproduces desiredWorld. For a root pass identity.
    bool TryConvertWorldToLocal(const Matrix4& parentWorld, const Matrix4& desiredWorld, Transform& local,
        std::string* error = nullptr, const NumericalPolicy& policy = {});

    // Connected-branch rules shared by document admission and Scene evaluation, so a document
    // that validates can always be installed. Each returns nullptr on success or a static reason.
    const char* EvaluateChildWorld(const Matrix4& parentWorld, const Transform& local, Matrix4& world,
        const NumericalPolicy& policy = {}) noexcept;
    const char* ValidateParentWorld(const Matrix4& world, const NumericalPolicy& policy = {}) noexcept;

    // Evaluates every authored world of a validated forest (no simulation poses) and rejects
    // the first connected node that violates the rules above. Flat forests always pass.
    bool ValidateConnectedTransforms(std::span<const SceneObjectDocument> objects, const Forest& forest,
        std::string* error = nullptr);

    // Removes every selected document index whose ancestor is also selected, so a world-space
    // group operation moves each independent branch exactly once. Preserves input order.
    std::vector<size_t> NormalizeSelectionRoots(const Forest& forest, std::span<const size_t> selected);
}

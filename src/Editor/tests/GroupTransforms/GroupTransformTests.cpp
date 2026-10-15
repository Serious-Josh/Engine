#include "Editor/GroupTransform.h"
#include "Editor/SceneSelection.h"
#include "Scene/Scene.h"
#include <iostream>
#include <limits>
#include <stdexcept>

static void Check(bool condition, const char* message)
{
    if(!condition) throw std::runtime_error(message);
}

static bool Near(const Vector3& a, const Vector3& b)
{
    return (a - b).Length() < 0.0001f;
}

static bool Near(const Matrix4& a, const Matrix4& b)
{
    for(size_t row = 0; row < 4; ++row)
        for(size_t col = 0; col < 4; ++col)
            if(std::abs(a.m[row][col] - b.m[row][col]) > 0.0001f) return false;
    return true;
}

int main()
{
    try
    {
        std::vector<SceneSelection::Identity> selected;
        const SceneSelection::Identity a{"persistent-a"}, b{"persistent-b"}, c{"persistent-c"};
        SceneSelection::Select(selected, a, false);
        SceneSelection::Select(selected, b, true);
        SceneSelection::Select(selected, c, true);
        Check(selected.size() == 3 && selected.back() == c, "Ordered add/active");
        SceneSelection::Select(selected, c, true);
        Check(selected.size() == 2 && selected.back() == b, "Active removal fallback");
        SceneSelection::Select(selected, a, true);
        Check(selected.size() == 1 && selected.back() == b, "Toggle removal without duplicates");
        SceneSelection::Select(selected, std::nullopt, true);
        Check(selected.size() == 1, "Shift-empty preserves selection");
        SceneSelection::Select(selected, c, false);
        Check(selected.size() == 1 && selected.back() == c, "Plain replacement");
        SceneSelection::Select(selected, std::nullopt, false);
        Check(selected.empty(), "Plain-empty clears selection");

        const Transform first{{3.0f, 2.0f, 1.0f}, {0.2f, 0.3f, 0.4f}, {1.0f, 2.0f, 3.0f}};
        const Transform anchor{{1.0f, 2.0f, 1.0f}};
        std::vector<Transform> before{first, anchor}, after;
        std::string error;
        Transform changed = anchor;
        changed.position = anchor.position + Vector3{5.0f, -2.0f, 7.0f};
        Check(GroupTransform::Calculate(GroupTransform::Mode::Translate, anchor, changed, before, after, error), "Translate");
        Check(Near(after[0].position - first.position, after[1].position - anchor.position), "Shared displacement");
        Check(Near(after[0].rotation, first.rotation) && Near(after[0].scale, first.scale), "Translation preserves rotation/scale");

        changed = anchor;
        changed.rotation.z = 1.57079632679f;
        Check(GroupTransform::Calculate(GroupTransform::Mode::Rotate, anchor, changed, before, after, error), "Rotate");
        Check(Near(after[0].position, {1.0f, 4.0f, 1.0f}) && Near(after[1].position, anchor.position), "Active pivot rotation");
        Check(Near(Matrix4::Rotation(after[0].rotation), Matrix4::Rotation(changed.rotation) * Matrix4::Rotation(first.rotation)), "Composed orientation");
        Check(Near(after[0].scale, first.scale), "Rotation preserves scale");
        const auto rotated = after;
        for(int repeat = 0; repeat < 100; ++repeat)
            Check(GroupTransform::Calculate(GroupTransform::Mode::Rotate, anchor, changed, before, after, error), "Repeat snapshot rotation");
        Check(Near(after[0].GetModelMatrix(), rotated[0].GetModelMatrix()), "No accumulated drift");

        changed = anchor;
        changed.scale = {2.0f, 2.0f, 2.0f};
        Check(GroupTransform::Calculate(GroupTransform::Mode::Scale, anchor, changed, before, after, error), "Uniform scale");
        Check(Near(after[0].position, {5.0f, 2.0f, 1.0f}) && Near(after[0].scale, {2.0f, 4.0f, 6.0f}), "Pivot offset and individual scale");
        Check(Near(Matrix4::Rotation(after[0].rotation), Matrix4::Rotation(first.rotation)), "Uniform scale preserves orientation");
        changed.scale = {2.0f, 1.0f, 1.0f};
        Check(!GroupTransform::Calculate(GroupTransform::Mode::Scale, anchor, changed, before, after, error), "Reject shearing nonuniform scale");
        before[0].rotation = {};
        Check(GroupTransform::Calculate(GroupTransform::Mode::Scale, anchor, changed, before, after, error), "Allow representable nonuniform scale");
        Check(Near(after[0].GetModelMatrix(), Matrix4::Translation(anchor.position) * Matrix4::Scale(2.0f, 1.0f, 1.0f) *
            Matrix4::Translation(anchor.position * -1.0f) * before[0].GetModelMatrix()), "Nonuniform matrix equivalence");
        changed.scale = {0.001f, 0.001f, 0.001f};
        Check(!GroupTransform::Calculate(GroupTransform::Mode::Scale, anchor, changed, before, after, error), "Minimum scale rejection");

        // A rotated anchor scales in the existing gizmo's local scale axes.
        Transform rotatedAnchor = anchor;
        rotatedAnchor.rotation = {0.2f, 0.4f, 0.6f};
        before = {first, rotatedAnchor};
        before[0].rotation = rotatedAnchor.rotation;
        changed = rotatedAnchor;
        changed.scale = {2.0f, 1.0f, 1.0f};
        Check(GroupTransform::Calculate(GroupTransform::Mode::Scale, rotatedAnchor, changed, before, after, error), "Rotated anchor scale");
        const auto basis = Matrix4::Rotation(rotatedAnchor.rotation);
        const auto delta = Matrix4::Translation(anchor.position) * basis * Matrix4::Scale(2.0f, 1.0f, 1.0f) *
            basis.Transpose() * Matrix4::Translation(anchor.position * -1.0f);
        Check(Near(after[0].GetModelMatrix(), delta * before[0].GetModelMatrix()), "Rotated-axis scale matrix equivalence");

        // Euler conversion must reproduce the rotation matrix, including gimbal limits.
        for(const float y : {-1.57079632679f, -1.2f, 0.0f, 1.2f, 1.57079632679f})
        {
            const auto rotation = Matrix4::Rotation({0.3f, y, -0.7f});
            Check(Near(rotation, Matrix4::Rotation(GroupTransform::Euler(rotation))), "Euler decomposition");
        }

        Scene scene;
        RenderObject object;
        scene.AddRenderObject(object);
        scene.AddRenderObject(object);
        const auto revision = scene.GetStructureRevision();
        std::vector<RenderObjectTransformUpdate> updates{{0, first}, {1, anchor}};
        Check(scene.SetRenderObjectTransforms(updates), "Atomic apply");
        Check(scene.GetChangedTransforms().size() == 2 && scene.GetStructureRevision() == revision, "Dirty list without structural/residency changes");
        scene.ClearChangedTransforms();
        auto invalid = updates;
        invalid[0].transform.position.x = 99.0f;
        invalid[1].id = 99;
        Check(!scene.SetRenderObjectTransforms(invalid) && Near(scene.GetRenderObjects()[0].transform.position, first.position), "Invalid ID rejects entire batch");
        invalid = updates;
        invalid[1].id = 0;
        Check(!scene.SetRenderObjectTransforms(invalid), "Duplicate ID rejection");
        invalid = updates;
        invalid[1].transform.position.x = std::numeric_limits<float>::infinity();
        Check(!scene.SetRenderObjectTransforms(invalid) && scene.GetChangedTransforms().empty(), "Nonfinite batch has no side effects");
        std::vector<RenderObjectTransformUpdate> undo{{0, Transform{}}, {1, Transform{}}};
        Check(scene.SetRenderObjectTransforms(undo), "Batch undo application");
        Check(scene.SetRenderObjectTransforms(updates), "Batch redo application");
        Check(scene.GetChangedTransforms().size() == 2, "Dirty IDs are deduplicated");
        std::cout << "PASS: selection ordering/toggles, group TRS/pivot math, no drift, shear rejection, Euler limits, atomic validation and dirty updates\n";
        return 0;
    }
    catch(const std::exception& exception)
    {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}

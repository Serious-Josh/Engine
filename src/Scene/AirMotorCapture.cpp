// AIR.3: the scene's Air Motors for the frame's air solver, and their Editor wireframes. Both use each object's accepted
// presentation pose: the world the frame's meshes render with, after physics publication. AIR.4F F2: likewise the scene's Mist
// Emitters and their wireframes.

#include "Scene/FileSceneRuntime.h"
#include "Renderer/AirField.h"
#include "Renderer/EditorColliderOverlay.h"
#include "Renderer/Renderer.h"

#include <algorithm>
#include <cmath>
#include <thread>

namespace
{
// A motor's rigid pose: position and orthonormal axes from the presented world, ignoring scale.
struct MotorPose
{
    Vector3 position;
    Vector3 axes[3];
};

std::optional<MotorPose> PoseOf(const Matrix4& world)
{
    MotorPose pose;
    pose.position = {world.m[0][3], world.m[1][3], world.m[2][3]};

    for(int axis = 0; axis < 3; ++axis)
    {
        const Vector3 column{world.m[0][axis], world.m[1][axis], world.m[2][axis]};
        const float length = std::sqrt(column.x * column.x + column.y * column.y + column.z * column.z);

        if(!std::isfinite(length) || length < 1e-6f)
        {
            return std::nullopt;
        }

        pose.axes[axis] = {column.x / length, column.y / length, column.z / length};
    }

    const bool finite = std::isfinite(pose.position.x) && std::isfinite(pose.position.y) && std::isfinite(pose.position.z);
    return finite ? std::optional<MotorPose>(pose) : std::nullopt;
}

// The rigid pose as a matrix (columns: axes, then position).
Matrix4 RigidMatrix(const MotorPose& pose)
{
    Matrix4 matrix = Matrix4::Identity();

    for(int axis = 0; axis < 3; ++axis)
    {
        matrix.m[0][axis] = pose.axes[axis].x;
        matrix.m[1][axis] = pose.axes[axis].y;
        matrix.m[2][axis] = pose.axes[axis].z;
    }

    matrix.m[0][3] = pose.position.x;
    matrix.m[1][3] = pose.position.y;
    matrix.m[2][3] = pose.position.z;
    return matrix;
}

// Rotates local +Y (the capsule overlay's axis) onto local +Z (the motor's axis).
Matrix4 YToZ()
{
    return Matrix4::RotationX(1.57079632679f);
}

// AIR.4D D2 emission gizmos: the origin marker's radius and the emission arrow's width, in metres.
constexpr float EmissionMarkerRadius = 0.08f;
constexpr float EmissionArrowWidth = 0.15f;

// AIR.4D D2: where the motor sits in world space: its emission origin (object-local, rotated by the presented pose, scale ignored),
// or the object's own position for a Moving motor, which has no emission controls.
Vector3 EmissionOrigin(const MotorPose& pose, const AirMotorComponent& motor)
{
    if(motor.type == AirMotorType::Moving)
    {
        return pose.position;
    }

    const Vector3& offset = motor.emissionOrigin;
    return {pose.position.x + pose.axes[0].x * offset.x + pose.axes[1].x * offset.y + pose.axes[2].x * offset.z,
        pose.position.y + pose.axes[0].y * offset.x + pose.axes[1].y * offset.y + pose.axes[2].y * offset.z,
        pose.position.z + pose.axes[0].z * offset.x + pose.axes[1].z * offset.y + pose.axes[2].z * offset.z};
}

// AIR.4D D2.1: the motor's pose in the solver: its emission origin, and for an Omni or Vortex the object's axes turned by the
// emission rotation (object-local, Rz * Ry * Rx, about the centre). Scale is ignored throughout.
MotorPose EmissionPose(const MotorPose& pose, const AirMotorComponent& motor)
{
    MotorPose emission = pose;
    emission.position = EmissionOrigin(pose, motor);

    if(motor.type != AirMotorType::Omni && motor.type != AirMotorType::Vortex)
    {
        return emission;
    }

    const Matrix4 rotation = Matrix4::Rotation(motor.emissionRotation);

    for(int axis = 0; axis < 3; ++axis)
    {
        Vector3 turned{0.0f, 0.0f, 0.0f};

        for(int source = 0; source < 3; ++source)
        {
            const float weight = rotation.m[source][axis];
            turned.x += pose.axes[source].x * weight;
            turned.y += pose.axes[source].y * weight;
            turned.z += pose.axes[source].z * weight;
        }

        emission.axes[axis] = turned;
    }

    return emission;
}

// AIR.4F F2: a Mist Emitter's pose: its emission origin (object-local, rotated with the object, scale ignored), and the object's axes
// turned by its emission rotation (object-local, Rz * Ry * Rx, about the origin).
MotorPose EmitterPose(const MotorPose& pose, const MistEmitterComponent& emitter)
{
    MotorPose emission = pose;
    const Vector3& offset = emitter.emissionOrigin;
    emission.position = {pose.position.x + pose.axes[0].x * offset.x + pose.axes[1].x * offset.y + pose.axes[2].x * offset.z,
        pose.position.y + pose.axes[0].y * offset.x + pose.axes[1].y * offset.y + pose.axes[2].y * offset.z,
        pose.position.z + pose.axes[0].z * offset.x + pose.axes[1].z * offset.y + pose.axes[2].z * offset.z};
    const Matrix4 rotation = Matrix4::Rotation(emitter.emissionRotation);

    for(int axis = 0; axis < 3; ++axis)
    {
        Vector3 turned{0.0f, 0.0f, 0.0f};

        for(int source = 0; source < 3; ++source)
        {
            const float weight = rotation.m[source][axis];
            turned.x += pose.axes[source].x * weight;
            turned.y += pose.axes[source].y * weight;
            turned.z += pose.axes[source].z * weight;
        }

        emission.axes[axis] = turned;
    }

    return emission;
}
}

void FileSceneRuntime::CaptureMistEmitters(std::vector<MistEmitterInstance>& emitters) const
{
    emitters.clear();

    if(std::this_thread::get_id() != m_ownerThread)
    {
        return;
    }

    const auto& objects = m_scene.GetRenderObjects();
    const auto& mapping = m_objectMapping.idToRuntimeIndex;

    for(const SceneObjectDocument& object : m_fileScene.GetDocument().objects)
    {
        if(!object.mistEmitter || !object.mistEmitter->enabled || !ValidateMistEmitterComponent(*object.mistEmitter))
        {
            continue;
        }

        const auto found = mapping.find(object.id);

        if(found == mapping.end() || found->second >= objects.size())
        {
            continue;
        }

        const auto pose = PoseOf(objects[found->second].GetPresentationTransform().currentModel);

        if(!pose)
        {
            continue;
        }

        const MistEmitterComponent& component = *object.mistEmitter;
        const MotorPose emission = EmitterPose(*pose, component);
        MistEmitterInstance emitter;
        emitter.shape = component.shape;
        emitter.position = emission.position;
        emitter.axisX = emission.axes[0];
        emitter.axisY = emission.axes[1];
        emitter.axisZ = emission.axes[2];
        emitter.radius = component.radius;
        emitter.halfExtents = component.halfExtents;
        emitter.densityRate = component.rate * component.density;
        emitter.color = component.color;
        emitter.tag = MistEmitterTag(object.id);
        emitters.push_back(emitter);
    }
}

void FileSceneRuntime::CaptureAirMotors(double frameSeconds, std::vector<AirMotorInstance>& motors) const
{
    motors.clear();

    if(std::this_thread::get_id() != m_ownerThread)
    {
        return;
    }

    const auto& objects = m_scene.GetRenderObjects();
    const auto& mapping = m_objectMapping.idToRuntimeIndex;

    for(const SceneObjectDocument& object : m_fileScene.GetDocument().objects)
    {
        if(!object.airMotor || !object.airMotor->enabled || !ValidateAirMotorComponent(*object.airMotor))
        {
            continue;
        }

        const auto found = mapping.find(object.id);

        if(found == mapping.end() || found->second >= objects.size())
        {
            continue;
        }

        const PresentationTransform& presented = objects[found->second].GetPresentationTransform();
        const auto pose = PoseOf(presented.currentModel);

        if(!pose)
        {
            continue;
        }

        const AirMotorComponent& component = *object.airMotor;
        const MotorPose emission = EmissionPose(*pose, component);
        AirMotorInstance motor;
        motor.type = component.type;
        motor.position = emission.position;
        motor.axisX = emission.axes[0];
        motor.axisY = emission.axes[1];
        motor.axisZ = emission.axes[2];
        motor.strength = component.strength;
        motor.falloff = component.falloff;
        motor.halfExtents = component.halfExtents;
        motor.radius = component.radius;
        motor.halfLength = component.length * 0.5f;
        motor.inwardPull = component.inwardPull;
        motor.coupling = component.coupling;
        motor.emissionSemiAxes = component.emissionSemiAxes;
        motor.tag = AirMotorTag(object.id);

        // Moving: the presented motion over the frame. A jump faster than any real motion is a teleport and moves no air.
        if(component.type == AirMotorType::Moving && frameSeconds > 0.0)
        {
            const Vector3 previous{presented.previousModel.m[0][3], presented.previousModel.m[1][3], presented.previousModel.m[2][3]};
            const double inverse = 1.0 / frameSeconds;
            const Vector3 velocity{static_cast<float>((pose->position.x - previous.x) * inverse), static_cast<float>((pose->position.y - previous.y) * inverse),
                static_cast<float>((pose->position.z - previous.z) * inverse)};
            const float speed = std::sqrt(velocity.x * velocity.x + velocity.y * velocity.y + velocity.z * velocity.z);
            const bool teleport = !std::isfinite(speed) || speed > AirMotorTeleportSpeed;
            motor.velocity = teleport ? Vector3{0.0f, 0.0f, 0.0f} : velocity;
        }

        motors.push_back(motor);
    }
}

bool FileSceneRuntime::CaptureEditorAirMotorWireframes(const Renderer& renderer, EditorColliderOverlay& overlay,
    std::span<const size_t> selectedRuntimeIndices, std::vector<std::string>& rejected, std::string& error) const
{
    rejected.clear();
    error.clear();

    // Editor-only, owner-thread, current-renderer capture, as for Light wireframes.
    const bool ownsActiveRendererScene = renderer.m_sceneResourceScene == &m_scene && renderer.m_sceneResourceLifetime != 0;

    if(m_archiveScene || std::this_thread::get_id() != m_ownerThread || !ownsActiveRendererScene)
    {
        error = "Air Motor wireframes require the active editable scene.";
        return false;
    }

    if(overlay.scene != nullptr && !overlay.Matches(m_scene, renderer.m_sceneResourceLifetime))
    {
        error = "Air Motor wireframes cannot join an overlay captured for another scene.";
        return false;
    }

    const auto& objects = m_scene.GetRenderObjects();
    const auto& mapping = m_objectMapping.idToRuntimeIndex;
    std::vector<ColliderOverlayMarker> wireframes;
    // Selected wireframes draw last, so with equal depth they stay on top.
    std::vector<ColliderOverlayMarker> selectedWireframes;

    for(const SceneObjectDocument& object : m_fileScene.GetDocument().objects)
    {
        if(!object.airMotor)
        {
            continue;
        }

        const AirMotorComponent& motor = *object.airMotor;
        const auto found = mapping.find(object.id);
        const auto pose = found != mapping.end() && found->second < objects.size() ?
            PoseOf(objects[found->second].GetPresentationTransform().currentModel) : std::nullopt;

        if(!pose || !ValidateAirMotorComponent(motor))
        {
            rejected.push_back(object.name);
            continue;
        }

        const bool selected = std::find(selectedRuntimeIndices.begin(), selectedRuntimeIndices.end(), found->second) !=
            selectedRuntimeIndices.end();
        const ColliderOverlayRole role = !motor.enabled ? ColliderOverlayRole::AirMotorDisabled :
            (selected ? ColliderOverlayRole::AirMotorSelected : ColliderOverlayRole::AirMotor);
        auto& target = selected ? selectedWireframes : wireframes;
        // The region is centred on the emission origin (AIR.4D D2), and an Omni's or Vortex's turned by its emission rotation
        // (D2.1); a Moving motor's is on its object.
        const Matrix4 rigid = RigidMatrix(EmissionPose(*pose, motor));

        const auto add = [&](const Matrix4& local, ColliderOverlayShape shape, float halfCylinderHeightInRadii = 0.0f)
        {
            target.push_back({rigid * local, shape, role, halfCylinderHeightInRadii});
        };

        // The emission geometry, in its own colour (dimmed with the region when the motor is disabled): a small marker at the
        // origin, and the type's emitter. An Omni ellipsoid's or Vortex ring's centre emits nothing (AIR.4D D2.1), so its marker
        // is drawn in the region's colour, as a reference only.
        const ColliderOverlayRole emissionRole = motor.enabled ? ColliderOverlayRole::AirMotorEmission : ColliderOverlayRole::AirMotorDisabled;
        const bool shaped = motor.type == AirMotorType::Vortex || (motor.type == AirMotorType::Omni && !IsPointOmni(motor));
        const Vector3& semiAxes = motor.emissionSemiAxes;

        const auto addEmission = [&](const Matrix4& local, ColliderOverlayShape shape, float halfCylinderHeightInRadii = 0.0f)
        {
            target.push_back({rigid * local, shape, emissionRole, halfCylinderHeightInRadii});
        };

        // A ring in the local XY plane with semi-axes (x, y) at height z: the Cylinder overlay (along +Y, rings in its XZ plane)
        // turned onto +Z with no height. RotationX(90 degrees) maps the overlay's X to X and its Z to -Y, so the scale's Z is the
        // ring's Y semi-axis.
        const auto ring = [](float x, float y, float z)
        {
            return Matrix4::Translation(0.0f, 0.0f, z) * YToZ() * Matrix4::Scale(x, 1.0f, y);
        };

        if(motor.type != AirMotorType::Moving)
        {
            const Matrix4 marker = Matrix4::Scale(EmissionMarkerRadius, EmissionMarkerRadius, EmissionMarkerRadius);

            if(shaped)
            {
                add(marker, ColliderOverlayShape::Sphere);
            }
            else
            {
                addEmission(marker, ColliderOverlayShape::Sphere);
            }
        }

        switch(motor.type)
        {
        case AirMotorType::Directional:
            // The emission ray: one arrow from the origin to the box's +Z face.
            addEmission(Matrix4::Scale(EmissionArrowWidth, EmissionArrowWidth, motor.halfExtents.z), ColliderOverlayShape::Arrows);
            break;
        case AirMotorType::Cylinder:
            // The emitting disc (a cylinder of no height) and its direction.
            addEmission(YToZ() * Matrix4::Scale(motor.radius, motor.radius, motor.radius), ColliderOverlayShape::Cylinder, 0.0f);
            addEmission(Matrix4::Scale(EmissionArrowWidth, EmissionArrowWidth, motor.length * 0.5f), ColliderOverlayShape::Arrows);
            break;
        case AirMotorType::Omni:
            // The emitting ellipsoid (a point Omni has only its origin marker).
            if(shaped)
            {
                addEmission(Matrix4::Scale(semiAxes.x, semiAxes.y, semiAxes.z), ColliderOverlayShape::Sphere);
            }

            break;
        case AirMotorType::Vortex:
            // The emitting ring.
            addEmission(ring(semiAxes.x, semiAxes.y, 0.0f), ColliderOverlayShape::Cylinder);
            break;
        case AirMotorType::Moving:
            break;
        }

        switch(motor.type)
        {
        case AirMotorType::Directional:
            // The box, and arrows through it along +Z.
            add(Matrix4::Scale(motor.halfExtents.x * 2.0f, motor.halfExtents.y * 2.0f, motor.halfExtents.z * 2.0f), ColliderOverlayShape::Box);
            add(Matrix4::Translation(0.0f, 0.0f, -motor.halfExtents.z) * Matrix4::Scale(motor.halfExtents.x, motor.halfExtents.y,
                motor.halfExtents.z * 2.0f), ColliderOverlayShape::Arrows);
            break;
        case AirMotorType::Omni:
        {
            // The influence region's outer bound: exact for a sphere or a point; for an ellipsoid, the ellipsoid with the influence
            // distance added to each semi-axis, which approximates the true offset surface (not itself an ellipsoid).
            const float reach = motor.radius;
            // A point Omni's semi-axes are 0, so this is its sphere of the influence distance.
            add(Matrix4::Scale(semiAxes.x + reach, semiAxes.y + reach, semiAxes.z + reach), ColliderOverlayShape::Sphere);
            break;
        }
        case AirMotorType::Moving:
            add(Matrix4::Scale(motor.radius, motor.radius, motor.radius), ColliderOverlayShape::Sphere);
            break;
        case AirMotorType::Vortex:
        {
            // The influence tube around the ring, outlined: its outer and (where it exists) inner in-plane edges, and the ring's
            // copies at the tube's top and bottom. Approximate for an ellipse, whose offset curves are not ellipses.
            const float reach = motor.radius;
            add(ring(semiAxes.x + reach, semiAxes.y + reach, 0.0f), ColliderOverlayShape::Cylinder);

            if(semiAxes.x > reach && semiAxes.y > reach)
            {
                add(ring(semiAxes.x - reach, semiAxes.y - reach, 0.0f), ColliderOverlayShape::Cylinder);
            }

            add(ring(semiAxes.x, semiAxes.y, reach), ColliderOverlayShape::Cylinder);
            add(ring(semiAxes.x, semiAxes.y, -reach), ColliderOverlayShape::Cylinder);
            break;
        }
        case AirMotorType::Cylinder:
        {
            const float halfLength = motor.length * 0.5f;
            add(YToZ() * Matrix4::Scale(motor.radius, motor.radius, motor.radius), ColliderOverlayShape::Cylinder, halfLength / motor.radius);
            // A Cylinder motor pushes along its axis: arrows through it.
            add(Matrix4::Translation(0.0f, 0.0f, -halfLength) * Matrix4::Scale(motor.radius, motor.radius, motor.length),
                ColliderOverlayShape::Arrows);
            break;
        }
        }
    }

    // AIR.4F F2: each Mist Emitter's shape in the emission colour (the region colour when selected, dim when disabled), and a marker
    // at its origin.
    for(const SceneObjectDocument& object : m_fileScene.GetDocument().objects)
    {
        if(!object.mistEmitter)
        {
            continue;
        }

        const MistEmitterComponent& emitter = *object.mistEmitter;
        const auto found = mapping.find(object.id);
        const auto pose = found != mapping.end() && found->second < objects.size() ?
            PoseOf(objects[found->second].GetPresentationTransform().currentModel) : std::nullopt;

        if(!pose || !ValidateMistEmitterComponent(emitter))
        {
            rejected.push_back(object.name);
            continue;
        }

        const bool selected = std::find(selectedRuntimeIndices.begin(), selectedRuntimeIndices.end(), found->second) !=
            selectedRuntimeIndices.end();
        const ColliderOverlayRole role = !emitter.enabled ? ColliderOverlayRole::AirMotorDisabled :
            (selected ? ColliderOverlayRole::AirMotorSelected : ColliderOverlayRole::AirMotorEmission);
        auto& target = selected ? selectedWireframes : wireframes;
        const Matrix4 rigid = RigidMatrix(EmitterPose(*pose, emitter));
        const Matrix4 shape = emitter.shape == MistEmitterShape::Sphere ? Matrix4::Scale(emitter.radius, emitter.radius, emitter.radius) :
            Matrix4::Scale(emitter.halfExtents.x * 2.0f, emitter.halfExtents.y * 2.0f, emitter.halfExtents.z * 2.0f);
        const ColliderOverlayShape outline = emitter.shape == MistEmitterShape::Sphere ? ColliderOverlayShape::Sphere : ColliderOverlayShape::Box;
        target.push_back({rigid * shape, outline, role, 0.0f});
        target.push_back({rigid * Matrix4::Scale(EmissionMarkerRadius, EmissionMarkerRadius, EmissionMarkerRadius), ColliderOverlayShape::Sphere,
            role, 0.0f});
    }

    if(wireframes.empty() && selectedWireframes.empty())
    {
        return true;
    }

    if(overlay.scene == nullptr)
    {
        overlay.scene = &m_scene;
        overlay.structureRevision = m_scene.GetStructureRevision();
        overlay.rendererLifetime = renderer.m_sceneResourceLifetime;
    }

    if(wireframes.size() + selectedWireframes.size() > EditorColliderOverlay::MaxColliders - (std::min)(overlay.GetColliderCount(),
        EditorColliderOverlay::MaxColliders))
    {
        error = "Air Motor wireframes exceed the Editor overlay capacity.";
        return false;
    }

    overlay.airMotorWireframes.insert(overlay.airMotorWireframes.end(), wireframes.begin(), wireframes.end());
    overlay.airMotorWireframes.insert(overlay.airMotorWireframes.end(), selectedWireframes.begin(), selectedWireframes.end());
    return true;
}

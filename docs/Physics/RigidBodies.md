# Rigid Bodies

Rigid bodies are objects that interact in Engine's physics simulation. Engine uses **Box3D** to simulate their movement and physical interactions.

An object can participate in rigid-body physics through its **Physics** component, an **Inherent Optional** component that defines its rigid-body type, collider shape, and physical properties.

Physics components are configured independently of Generic Optional components, such as Trigger Colliders. A Trigger Collider can detect overlaps without requiring the object to have a Physics component.

## Rigid Body Types

Engine supports three types of rigid bodies: **Static**, **Dynamic**, and **Kinematic**.

### Static

Static rigid bodies are intended for objects that do not move during physics simulation.

They are not affected by gravity or collision forces, but can interact with other rigid bodies.

Common uses include:

- Floors and walls
- Terrain
- Stationary obstacles
- Structural geometry

For example, a floor with a Static rigid body can support a falling Dynamic object without moving.

### Dynamic

Dynamic rigid bodies are controlled by the physics simulation.

They respond to gravity and collision forces, allowing them to fall, collide, and come to rest against other physics objects.

Common uses include:

- Falling objects
- Movable crates
- Physics-driven props
- Objects that should react to collisions

Dynamic bodies use their collider dimensions and density to determine their physical mass.

For example, a Dynamic cube positioned above a Static floor will fall under gravity until it collides with the floor.

### Kinematic

Kinematic rigid bodies are intended for objects whose movement is controlled externally rather than by physics forces.

They are not accelerated by gravity and do not respond to collisions by being pushed like Dynamic bodies.

However, they can interact with Dynamic bodies as they move.

Common uses include:

- Moving platforms
- Mechanically controlled doors
- Elevators
- Script-controlled obstacles

**Note**: Kinematic body support exists in Engine's physics system, but gameplay scripting and general-purpose movement controls are not implemented yet.

## Physics Simulation

Engine uses a fixed timestep to maintain consistent physics behavior.

| Property | Value |
|---|---|
| Physics library | Box3D |
| Simulation frequency | 60 Hz |
| Fixed timestep | 1/60 second |
| Solver substeps | 4 |
| Gravity | (0, -9.81, 0) |
| Unit scale | 1 Engine unit = 1 meter |

Engine uses the Y axis as its vertical axis, gravity therefore acts in the negative Y direction.

The fixed timestep means physics advances in discrete simulation steps rather than directly following the editor's frame rate.

## Authored Transforms and Simulated Transforms

Engine distinguishes between an object's **authored transform** and its **simulated transform**.

The authored transform is the position, rotation, and scale saved in the scene.

The simulated transform is the position and rotation produced by the physics simulation during Play mode.

When Play begins, Engine creates a physics simulation using the scene's current authored state.

While the simulation runs:

- Dynamic bodies can move and rotate in response to physics
- Their simulated positions and rotations are presented in the viewport
- The original authored transforms are preserved

When Play stops, Engine discards the simulated transforms and restores the authored scene.

This allows you to test physics interactions without permanently modifying scene presentation.

## Rigid Bodies and Colliders

Every rigid body requires a collision shape that defines its physical volume.

Engine currently supports:

- Box
- Sphere
- Capsule

The collision shape is configured separately from the object's visual mesh, changing the visual mesh does not automatically redefine its collider dimensions and vice-versa.

For more information see [Colliders](Colliders.md).

## Configuring Rigid Bodies

Rigid-body settings are available through the editor's Inspector when an object has a **Physics** component.

Physics is an **Inherent Optional** component and retains its own Inspector controls rather than being added through **Add Component**.

You can configure its body type, collider shape, and physical properties.

Physics-related editing is disabled while Play mode is active.
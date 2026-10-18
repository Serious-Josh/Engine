# Colliders

Colliders define the physical shape of an object and determine how it interacts with other objects during physics simulation.

In Engine, a collider is configured through an objects **Physics** component in the Inspector. Colliders are used to detect physical contact and produce collision responses, such as preventing objects from passing through one another.

A collider does not need to match the object's visible mesh visually, its dimensions and position are configured independently of the rendered geometry.

## Collider Shapes

Engine currently supports three collider shapes: Box, Sphere, and Capsule.

### Box Collider

A Box Collider represents a rectangular solid.

Its dimensions property specifies the full width, height, and depth of the box, in meters.

For example, dimensions of `(2, 1, 3)` create a collider that is 2 meters wide, 1 meter tall, and 3 meters deep.

The default dimensions are `(1, 1, 1)`.

Box Colliders are suitable for crates, walls, floors, platforms, and other objects with approximately rectangular shapes.

### Sphere Collider

A Sphere Collider represents a spherical solid.

It's radius property specifies the distance from the center of the sphere to its surface, in meters.

The default radius is `0.5`, producing a sphere with a dimension of 1 meters.

Sphere Colliders are suitable for balls and other approximately spherical objects.

### Capsule Collider

A Capsule Collider consists of a cylinder with a hemisphere at each end.

It's properties are:

- Radius - The radius of the cylinder and hemispherical ends
- Cylinder Height - The height of the cylindrical portion, excluding the hemispherical ends
- Local Rotation - The orientation of the capsule relative to the object

The default radius is `0.5` meters, and the default cylinder height is `1.0` meters.

The total height of an upright capsule is: `Total Height = Cylinder Height + (2 * Radius)`

For example, a capsule with a cylinder height of 1 meter and a radius of 0.5 meters has a total height of 2 meters.

Capsule Colliders are useful for characters, pillars, and other elongated objects with rounded ends.

## Collider Position and Orientation

### Local Offset

The offset property moves the collider relative to the object's origin.

An offset of `(0, 0, 0)` centers the collider on the object.

For example, an offset of `(0, 1, 0)` positions the collider 1 meter above the object's origin in local space.

The offset follows the object's rotation, so the collider remains positioned relative to the object as it rotates.

### Capsule Local Rotation

Capsule Colliders also support a **Local Rotation** property, allowing their orientation to differ from the object's rotation.

The Inspector displays rotation values in degrees.

Local Rotation is applied in addition to the object's own rotation.

Box and Sphere Colliders do not expose an independent local rotation setting.

### Collider Scale

Collider dimensions are independent of an object's visual scale.

Scaling of an object's mesh does not automatically resize its collider, instead collider dimensions must be adjusted through the Physics component.

This separation allows physical collision shapes to be configured independently of rendered geometry.

## Physical Properties

The Physics component also provides properties that control how a collider behaves during physical contact.

### Density 

Density determines the amount of mass per unit volume, measured in kilograms per cubic meter (kg/m³).

The default density is `1000 kg/m³`.

For Dynamic bodies, density contributes to the mass and inertia calculated from the collider's shape and dimensions.

Density is a property of Dynamic rigid-bodies.

### Friction

Friction controls resistance to sliding when two colliders are in contact.

The default friction is `0.5`.

Higher friction generally makes surfaces resist sliding more strongly, while lower friction allows them to slide more easily.

### Restitution

Restitution controls how much an object bounces during collisions.

The default restitution is `0.0`.

A restitution of `0.0` represents a non-bouncy collision, while higher values produce more elastic collisions.

## Viewing Colliders in the Editor

Engine provides a wireframe overlay for inspecting collider shapes.

To enable it:

1. Open the **Overlays** menu in the Editor toolbar.
2. Enable **Physics Colliders**

When enabled, colliders appear as green wireframes in the viewport.

These wireframes represent the configured physical shapes rather than the visible meshes.

The overlay is an editor visualization and does not appear in packaged games.

## Colliders and Simulation

Colliders participate in the physics simulation according to their associated rigid body's type.

- **Static bodies** provide collision geometry that does not move under simulation.
- **Dynamic bodies** respond to gravity, forces, and collisions.
- **Kinematic bodies** are moved explicitly rather than by simulated forces.

A collider's shape, dimensions, and physical properties determine how it participates in collision detection and response.

For more information about rigid-body behavior, see [RigidBodies](RigidBodies.md).

## Trigger Colliders

Trigger Colliders are non-solid collision volumes used to detect when other physics objects enter or leave a defined area.

Unlike regular colliders, Trigger Colliders do not block movement or produce physical collision responses. Objects can pass through a Trigger Collider while Engine tracks their overlap.

Common uses for Trigger Colliders include:

- Detecting when a player enters a particular area
- Creating interaction zones around objects
- Detecting objects entering or leaving a doorway
- Defining checkpoints or gameplay regions
- Detecting proximity between physics objects

Trigger Colliders provide the foundation for these interactions, although gameplay scripting callbacks are not yet implemented.

### Adding a Trigger Collider

Trigger Colliders are considered **Generic Optional** components that can be attached to objects through the Inspector.

To add a Trigger Collider:

1. Select a mesh object or Empty Object in the Scene Hierarchy
2. Click **Add Component** at the bottom of the Inspector
3. Search for **Trigger Collider** in the component list
4. Select **Trigger Collider** to attach it to the object

Once added, the Trigger Collider section appears in the Inspector, where its shape, dimensions, local offset, and Capsule local rotation can be configured.

To remove a Trigger Collider, open the management menu (...) in its section header and select **Remove Component**.

Adding and removing Trigger Colliders supports Undo and Redo.

Trigger Colliders are independent of the Physics component, which is an **Inherent Optional** and retains its own Inspector controls.

An object can have:

- A Physics component with a solid collider
- A Trigger Collider without a Physics component
- Both a Physics component and a Trigger Collider

Trigger Colliders can also be attached to Empty Objects, allowing invisible regions to participate in overlap detection without requiring a visible mesh.

When an object has both components, its solid collider continues to produce normal physical collision responses while its Trigger Collider independently detects overlaps.

### Trigger Collider Shapes

Trigger Colliders support the same three shapes as solid colliders:

- Box - A rectangular volume configured using full width, height, and depth
- Sphere - A spherical volume configured using a radius
- Capsule - A cylindrical volume with hemispherical ends, configured using radius and cylinder height

Trigger Colliders also support a local offset, allowing the detection volume to be positioned relative to the object's origin.

Capsule Trigger Colliders support an additional local rotation, displayed in degrees in the Inspector.

The default dimensions are the same as their solid collider equivalents:

| Shape | Default properties |
|---|---|
| Box | Dimensions `(1, 1, 1)` |
| Sphere | Radius `0.5` |
| Capsule | Radius `0.5`, Cylinder Height `1.0` |

Trigger Collider dimensions are independent of visual mesh scale. Scaling an object does not automatically resize its Trigger Collider.

### Trigger Colliders and Rigid Bodies

A Trigger Collider can operate with or without a Physics component.

**With a Physics component**, the Trigger Collider follows the object's existing physics body. For Dynamic bodies, the trigger volume moves with the body during simulation.

**Without a Physics Component**, Engine creates an internal Static sensor body to represent the Trigger Collider during simulation.

This internal body is managed automatically and does not turn the object into a user-configurable rigid body.

Trigger Colliders can detect overlaps with Static, Dynamic, and Kinematic physics colliders, as well as other Trigger Colliders.

An object that has only a visible mesh, without a Physics component or Trigger Collider, does not participate in trigger detection.

### Trigger Enter and Exit Events

Engine tracks two types of Trigger Events:

- Enter - Another physics object begins overlapping a Trigger Collider
- Exit - Another physics object stops overlapping a Trigger Collider

Each Trigger Event identifies the Trigger Collider's owning object, the visiting object, and the physics step in which the transition occurred.

Trigger Events are directional. When two Trigger Colliders overlap, Engine records a separate directed event for each trigger owner.

Engine also combines multiple overlapping native physics shapes into one logical object-level interaction, avoiding duplicate Enter or Exit events while the objects remain overlapping.

Trigger Events are independent of the **Generate Contact Events** checkbox in the Physics component.

Stopping simulation or destroying a physics session clears active trigger states without generating artificial Exit events.

Trigger Events are currently available internally through Engine's physics systems. Gameplay scripting callbacks have not been implemented yet.

For more information about solid contact notifications, see [Contact Events](ContactEvents.md)

### Trigger Collider Visualization

Trigger Colliders can be visualized in the editor using the same wireframe overlay as regular solid colliders.

To enable the visualization:

1. Open the **Overlays** menu in the editor toolbar.
2. Enable **Physics Colliders**

When enabled, colliders appear as colored wireframes in the viewport:

- Green - Solid colliders associated with Physics components
- Cyan - Non-solid Trigger Colliders

Trigger Collider wireframes represent their configured Box, Sphere, or Capsule volumes, including their dimensions, local offsets, and Capsule local rotation.

Trigger Colliders attached to Empty Objects are also visible, allowing invisible detection regions to be inspected without requiring a rendered mesh.

Objects containing both a Physics component and Trigger Collider display both wireframes simultaneously.

During Play mode, Trigger Collider wireframes follow their associated physics bodies, including the simulated movement of Dynamic bodies.

The overlay is intended for editor visualization only. It does not affect detection, Trigger Events, or physics simulation, and does not appear in packaged games.

### Limitations

Trigger detection uses discrete physics simulation steps. An object moving extremely quickly through a thin Trigger Collider may pass through between physics updates without generating an overlap event.

Trigger Events currently provide overlap transitions rather than continuous reports every frame.

Gameplay callbacks and scripting integration are planned for future implementation.
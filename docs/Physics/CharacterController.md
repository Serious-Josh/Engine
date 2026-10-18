# Character Controller

## Overview

The **Character Controller** is an optional component designed to move characters through a three-dimensional environment while respecting physical obstacles, surfaces, and movement constraints.

Unlike a Rigid Body, which participates in the physics simulation as a physical object, a Character Controller uses a capsule-shaped geometric mover to resolve requested movement against the environment.

Character Controllers are intended for player characters, AI-controlled characters, and other objects requiring controlled locomotion.

The character Controller does not determine how an object should be controlled. It provides the physical movement capabilities that gameplay systems can use.

## Adding a Character Controller

1. Select a Mesh Object or Empty Object in the Scene Hierarchy
2. Open the Inspector
3. Select **Add Component**
4. Choose **Character Controller**
5. Configure its capsule and movement properties

Character Controller is a **Generic Optional** component. It is not automatically attached to new objects.

A Character Controller can coexist with a Trigger Collider, but cannot coexist with a Physics component. Adding either conflicting component must be rejected rather than automatically removing the other.

## Capsule Geometry

The Character Controller uses an upright capsule aligned with the world's positive Y axis.

The capsule consists of a cylinder with a hemispherical cap at each end.

| Property | Default | Description |
|---|---|---|
| Capsule Radius | 0.3 m | Radius of the capsule and both hemispherical ends |
| Cylinder Height | 1.2 m | Height of the cylindrical section, excluding the caps |
| Local Offset | (0, 0, 0) m | Offset of the capsule center relative to the object's origin |

The total capsule height is:

**Total Height = Cylinder Height + 2 × Capsule Radius**

With the default values, the capsule is **1.8 meters tall**.

The capsule is centered on the object's origin when Local Offset is zero. The capsule's collision geometry does not inherit the object's visual scale.

The initial controller supports an upright capsule with yaw rotation. Authored pitch and roll are not supported, apart from a small numerical tolerance.

## Movement Properties

| Property | Default | Description |
|---|---|---|
| Maximum Horizontal Speed | 5 m/s | Maximum requested horizontal movement speed |
| Gravity Scale | 1.0 | Multiplier applied to gravity |
| Jump Speed | 5 m/s | Proposed upward launch speed for jumping |
| Maximum Walkable Slope | 45° | Steepest surface considered walkable |
| Ground Snap Distance | 0.1 m | Maximum distance used for intended ground reacquisition |
| Collision Clearance | 0.002 m | Additional clearance used by geometric collision queries |

These properties define movement constraints and physical behavior, not input bindings.

A value of zero is valid for horizontal speed, gravity scale, jump speed, ground snap distance, and collision clearance.

All values must pass the component's validation rules. Invalid dimensions, nonfinite values, and unsupported orientations are rejected rather than silently corrected.

## Movement and Collision

The Character Controller receives movement requests and resolves them against the surrounding environment.

Unlike a Dynamic Rigid Body, its movement is not primarily determined by physical forces or collision impulses. Instead, the controller calculates an allowed movement based on its capsule geometry, requested motion, and surrounding collision shapes.

This allows a character to move in a controlled manner while respecting solid obstacles.

### Horizontal Movement

Horizontal movement takes place along the world's X and Z axes.

The **Maximum Horizontal Speed** property limits the controller's requested horizontal movement speed.

For example, with a Maximum Horizontal Speed of `5 m/s`, the controller can request horizontal movement up to five meters per second.

Movement requests are separate from keyboard, mouse, or gamepad inputs. Gameplay systems are responsible for deciding which direction a character should move and supplying the corresponding requests.

### Collision Response

When a controller moves towards a solid obstacle, its capsule is checked against the surrounding collision geometry.

The controller resolves movement geometrically rather than relying on a Dynamic Rigid Body's collision impulses.

Depending on the obstacle and movement direction, this can prevent the controller from passing through solid geometry or allow movement along an available surface.

For example, when a character moves diagonally towards a wall, the collision response can restrict the movement into the wall while allowing movement parallel to it.

The **Collision Clearance** property provides additional separation during geometric collision queries.

Collision Clearance does not change the visible mesh or the authored dimensions of the controller's capsule.

## Gravity and Jumping

Character Controllers maintain their own vertical movement behavior.

### Gravity

The **Gravity Scale** property determines how strongly gravity affects the controller.

Engine's default gravity is `(0, -9.81, 0)`, with the negative Y axis representing downward movement.

A Gravity Scale of:

- 1.0 applies normal gravity
- 0.5 applies half the normal gravitational acceleration
- 0.0 disables gravity-driven acceleration

Gravity Scale does not affect the controller's horizontal speed limit.

### Jumping

The **Jump Speed** property defines the upward launch speed used for a jump.

The default Jump Speed is `5 m/s`.

Jumping is a movement capability of the controller rather than an automatically assigned keyboard action.

A gameplay system must request a jump. Adding a Character Controller does not automatically make the Space key or another input trigger jumping.

Setting Jump Speed to 0 disables upward launch from a jump request.

## Ground Detection and Slopes

The Character Controller distinguishes between walkable ground and surfaces that are too steep to support normal grounded movement.

### Ground Detection

Ground detection determines whether the controller has suitable supporting geometry beneath its capsule.

This is important for distinguishing grounded movement from movement through the air.

The **Ground Snap Distance** property controls how far the controller may search downward to reacquire suitable ground.

The default value is `0.1 m`.

Ground snapping helps maintain contact with nearby supporting surfaces during appropriate movement conditions. It is not intended to pull the controller onto arbitrarily distant surfaces.

Setting Ground Snap Distance to 0 disables this additional ground-reacquisition distance.

### Walkable Slopes

The **Maximum Walkable Slope** property determines the steepest surface considered suitable ground.

The default is `45°`.

Slope angles are evaluated relative to the world's upward Y axis.

A horizontal floor has a slope angle of approximately `0°`, while a vertical wall ahs a slope angle of approximately `90°`.

With the default setting, surfaces within the walkable-slope limit may support grounded movement. Steeper surfaces are not considered walkable ground.

This distinction prevents the controller from treating walls and excessively steep surfaces as ordinary floors.

## Character Controllers and Rigid Bodies

Character Controllers and Rigid Bodies provide different methods of movement.

| Property | Character Controller | Dynamic Rigid Body |
|---|---|---|
| Movement | Requested and geometrically resolved | Physics simulation |
| Collision shape | Upright capsule | Box, Sphere, or Capsule |
| Gravity | Controller movement behavior | Simulated force |
| Jumping | Requested upward movement | Requires external force or velocity control |
| Physical collision impulses | Not used as primary locomotion | Used |
| Typical purpose | Player and AI characters | Physical objects and props |

An object cannot contain both a Character Controller and a Physics component.

Engine rejects attempts to add one while the other is already present.

For more information about physical bodies, see [Rigid Bodies](RigidBodies.md).

## Character Controllers and Trigger Colliders

A Character Controller can coexist with a Trigger Collider.

The Character Controller handles solid movement and obstacle avoidance, while the Trigger Collider provides a separate non-solid detection volume.

For example, a character may use its controller capsule to move through the environment while carrying an additional Trigger Collider for proximity detection.

For more information, see [Colliders](Colliders.md) and [Contact Events](ContactEvents.md).

## Character Controllers and Scene Hierarchy

Character Controllers are restricted to root-level scene objects.

A root object is an object that does not have a parent in the Scene Hierarchy.

This restriction keeps the controller's authored transform and simulated movement independent of parent-driven transform changes.

An object containing a Character Controller cannot be parented beneath another object while retaining that component.

Likewise, adding a Character Controller to a child object is not supported.

Visual objects without restricted Physics components can still participate in ordinary parent/child hierarchies.

## Character Controllers During Play Mode

Character Controllers participate in Engine's physics simulation during Play mode.

During simulation, controller movement is maintained separately from the scene's saved authored transform.

Character Controller properties cannot be configured while Play mode is running.

## Character Controller Visualization

The Character Controller uses an invisible capsule to determine its physical movement and collision behavior.

This capsule is independent of the object's visible mesh.

Its radius, cylinder height, and local offset determine the space occupied by the controller during collision queries.

Engine's physics collider overlay can be enabled from the Editor's Overlays menu by selecting Physics Colliders.

The overlay is an Editor visualization and does not appear in packaged games.

## Current Limitations

The Character Controller currently has several (some intentional) limitations:

- **No built-in player inputs** - Adding a controller does not automatically provide WASD movement, mouse look, or jump-key bindings
- **No gameplay scripting integration** - Gameplay systems do not yet expose a complete user-facing interface for controlling characters
- **Upright capsule only** - The controller supports yaw orientation, not arbitary pitch or roll
- **No automatic visual scaling** - Changing the object's mesh scale does not resize its controller capsule
- **Root objects only** - Character Controllers cannot be attached to child objects
- **No simultaneous Physics component** - A Character Controller and a rigid-body Physics component are mutually exclusive
- **No crouching or runtime capsule resizing** - The initial controller uses its configured capsule dimensions
- **No automatic gameplay callbacks** - Contact and Trigger Events are available through Engine's physics system, but automatic gameplay event dispatch remains future work

The Character Controller provides the physical foundation for controlled character locomotion. Player input, camera control, AI behavior, and gameplay scripting can be built on top of that foundation without requiring characters to behave like Dynamic Rigid Bodies.
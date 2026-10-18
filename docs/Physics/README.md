# Physics

Engine uses **Box3D**, a three-dimensional rigid-body physics library, to simulate physical interactions between objects.

Engine's physics system supports Static, Dynamic, and Kinematic rigid bodies, configurable solid and Trigger Colliders, contact and trigger events, and collider visualization through the editor.

## Documentation

### [Rigid Bodies](RigidBodies.md)

Learn about the three supported rigid-body types:

- Static
- Dynamic
- Kinematic

Includes information about physics simulation and body behavior.

### [Colliders](Colliders.md)

Learn how collision shapes are configured and how they interact with other physics objects.

Supported collider shapes:

- Box
- Sphere
- Capsule

### [Contact Events](ContactEvents.md)

Learn how Engine records physical contact and non-solid trigger overlaps during physics simulation.

Covers `ContactEvent`, `ContactNotification`, and `TriggerEvent`, including the distinction between solid contact notifications and directional Trigger Enter/Exit events.

### [Character Controller](CharacterController.md)

Learn how Character Controllers provide capsule-based movement and collision handling for player characters and AI-controlled objects.

Covers horizontal movement, gravity, jumping, ground detection, walkable slopes, and collision response.

Includes information about component configuration, interaction with Trigger Colliders, and current movement limitations.

## Current Limitations

Gameplay scripting callbacks, automatic gameplay event dispatch, and built-in player input controls are not yet implemented.

Contact Events and Trigger Events are available internally through Engine's physics system, but gameplay systems do not automatically consume or respond to them yet.

Character Controllers provide the physical foundation for character movement, but gameplay input and scripting integration are planned for future implementation.
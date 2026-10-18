# Contact Events

Contact Events allow Engine to track when two physics objects begin or stop touching.

These events provide information about physical interactions during simulation, such as when a falling box lands on a floor or when two moving objects separate.

Contact Events are separate from physical collision response. Objects can collide, block one another, and respond to gravity without generating gameplay notifications.

## How Contact Events Work

During physics simulation, Engine tracks contact transitions between objects that have physics bodies and colliders.

There are two contact event types:

- Begin - Two objects start touching
- End - Two objects stop touching

For example, when a Dynamic box falls onto a Static floor, a Begin event is generated when contact is established. If the box is subsequently lifted away from the floor, an End event is generated when contact ends.

Contact Events describe changes in contact rather than continuously reporting every frame that two objects remain touching.

Engine processes physics at a fixed timestep of 60 updates per second. Contact transitions are associated with the physics step in which they occur.

## Generate Contact Events

The **Generate Contact Events** checkbox is available in the Physics component section of the Inspector.

It controls whether an object requests contact notifications during simulation.

The setting is disabled by default.

Enabling it does not change the object's physical collision behavior. It only affects which contact events are selected for notification.

### When Neither Object Has It Enabled

The objects still collide normally.

Engine can track the underlying contact transition, but no contact notification is produced for either object.

### When One Object Has It Enabled

A contact notification is produced for the interaction.

Only the object with the Generate Contact Events enabled is marked as requesting the notification.

For example, if a Dynamic box has the setting enabled and a Static floor does not, the box is marked for notification when the two objects begin or stop touching.

### When Both Objects Have It Enabled

A single contact notification is produced for the interaction, with both objects marked as requesting notification.

Engine does not create two duplicate notifications simply because both objects have the setting enabled.

## Contact Events and Contact Notifications

Engine distinguishes between the underlying contact event and the notification selected for gameplay use.

### Contact Events

A Contact Event contains:

- The type of transition: Begin or End
- The stable identifiers of the two participating scene objects
- The fixed physics step associated with the transition

The two objects identifiers are stored in a consistent order to support deterministic processing.

Contact Events are generated independently of the Generate Contact Event setting.

### Contact Notifications

A Contact Notification contains the original Contact Event along with information indicating which participating objects have request notification.

A notification is produced when at least one participating object has Generate Contact Events enabled.

This allows future gameplay systems to distinguish between which object or objects are interested in receiving contact information.

## Multiple Contact Points

Two objects can touch at more than one point or through more than one physical shape.

Engine tracks contact state at the scene-object-pair level, rather than treating every individual contact point as a separate gameplay interaction.

A Begin event represents the first qualifying contact between the objects.

An End event represents the end of the final qualifying contact between them.

This avoids duplicate Begin and End events while the object remains in contact.

## Event Ordering and Object Identity

Contact Events use stable scene-object identifiers rather than temporary physics shape handles.

This allows events to refer consistently to scene objects independently of the underlying physics implementation.

Engine also processes contact transitions in a deterministic order within each physics update.

This is useful for future gameplay systems that need predictable event ordering.

## Contact Events During Play and Stop

Contact Events are generated while physics simulation is running.

When Play begins, Engine starts a new physics simulation session and establishes its contact state as objects interact.

When Stop is selected, the physics session is destroyed and its contact state is cleared.

Stopping simulation does not generate artificial End events for every contact that was active.

Likewise, scene teardown does not produce additional gameplay notifications solely because physics objects are being destroyed.

# Trigger Events

In addition to solid Contact Events, Engine supports **Trigger Events**, which track when physics objects enter or leave non-solid Trigger Collider volumes.

Unlike regular colliders, Trigger Colliders do not block movement or produce physical collision responses. They detect overlaps without affecting the movement of other objects.

Trigger Events are generated independently of regular Contact Events and do not require **Generate Contact Events** to be enabled.

For more information about configuring Trigger Colliders, see [Colliders](Colliders.md).

## How Trigger Events Work

Engine tracks two types of Trigger Events:

- Enter - An object begins overlapping a Trigger Collider
- Exit - An object stops overlapping a Trigger Collider

For example, if a Dynamic object falls through a Trigger Collider attached to an Empty Object, an Enter event is generated when the object begins overlapping with the trigger volume. An exit event is generated when the object leaves it.

Trigger Colliders can detect overlaps with Static, Dynamic, and Kinematic physics colliders, as well as other Trigger Colliders.

Objects containing only a visible mesh without a Physics component or Trigger Collider do not participate in trigger detection.

Like Contact Events, Trigger Events describe transitions rather than continuously reporting overlaps every frame.

## Trigger Event Data

Each Trigger Event contains:

- **Event Type** - Enter or Exit
- **Trigger Owner** - The stable identifier of the scene object containing the Trigger Collider
- **Visiting Object** - The stable identifier of the object entering or leaving the trigger volume
- **Physics Step** - The fixed simulation step in which the transition occurred

Unlike Contact Events, which identify an unordered pair of participating objects, Trigger events are directional.

The Trigger Owner identifies the object whose Trigger Collider detected the overlap, while the Visiting Object identifies the other participant.

## Trigger-to-Trigger Overlaps

Two Trigger Colliders can detect one another.

When two Trigger Colliders begin overlapping, Engine generates two directed Enter events:

- One event identifies the first Trigger Collider's owner as the Trigger Owner and the second object as the visitor
- The other event identifies the second Trigger Collider's owner as the Trigger Owner and the first object as the visitor

When the Trigger Colliders separate, corresponding Exit events are generated for both owners.

This allows each Trigger Collider to independently detect an overlap with the other.

## Multiple Overlapping Shapes

Engine tracks Trigger Events at the scene-object level rather than generating a separate event for every native physics shape involved.

An Enter event is generated when the first qualifying overlap begins between a Trigger Collider and a visiting object.

An Exit event is generated when the final qualifying overlap between those two objects ends.

if multiple shapes remain overlapping, Engine does not generate duplicate Enter or Exit events for the same directed object pair.

This also prevents temporary event transitions caused solely by replacing underlying physics shapes while the logical overlap remains active.

## Trigger Events and Contact Notifications

Trigger Events and Contact Notifications serve different purposes.

| Property | Contact Events | Trigger Events |
|---|---|---|
| Interaction | Solid physical contact | Non-solid overlap |
| Event types | Begin / End | Enter / Exit |
| Object identity | Consistently ordered object pair | Directed Trigger Owner and visitor |
| Collision response | Solid colliders can block movement | Trigger volumes do not block movement |
| Generate Contact Events | Controls notification selection | Not required |
| Multiple shapes | Aggregated by object pair | Aggregated by directed object pair |

An object can contain both a Physics component and a Trigger Collider.

In this case, its solid collider continues participating in physical collisions while its Trigger Collider independently detects overlaps.

Enabled or disabling **Generate Contact Events** does not change Trigger Event generation.

## Event Ordering and Identity

Trigger Events use stable scene-object identifiers rather than temporary physics shape handles.

Engine processes Trigger Events in a deterministic order using:

1. Physics step
2. Trigger Owner identifier
3. Visiting Object identifier
4. Event type, with Exit ordered before Enter when the preceding values are equal

This provides consistent event ordering for systems that consume Trigger Events.

## Trigger Events During Play and Stop

Trigger Events are generated while physics simulation is running.

When Play begins, Engine starts a new physics simulation session and begins tracking qualifying overlaps.

When Stop is selected, any active Trigger Event states are cleared.

Stopping simulation, changing scenes, or destroying a physics session does not generate artificial Exit events for overlaps that were still active.

Ordinary separation during simulation produces Exit events normally.

## Current Limitations

Trigger detection uses discrete physics simulation steps. An object moving extremely quickly through a thin Trigger Collider may pass through the volume between physics updates without generating an Enter or Exit event.

Trigger Events are currently available internally through Engine's physics systems.

Gameplay scripting callbacks and automatic gameplay event dispatch have not yet been implemented.
#pragma once

#include <cstdint>
#include <string>

namespace Physics
{
// Object-level contact transition. Begin: two scene objects started touching (their first
// touching shape contact appeared). End: their last touching shape contact disappeared.
enum class ContactEventType
{
    Begin,
    End
};

// A read-only simulation result, published after the fixed step that produced it. It
// holds stable scene-object IDs only, never Box3D shape/body IDs or pointers.
struct ContactEvent
{
    // 1-based fixed-step number within the physics session that produced the event.
    std::uint64_t fixedStep = 0;
    ContactEventType type = ContactEventType::Begin;

    // Canonical pair: objectA < objectB (std::string ordering), never equal.
    std::string objectA;
    std::string objectB;

    bool operator==(const ContactEvent&) const = default;
};

// Gameplay-facing view of a ContactEvent for objects that opted in with
// PhysicsComponent::generateContactEvents. One notification per raw event, never one per
// interested participant: notifyObjectA/notifyObjectB say which participants should
// receive it; the other object in the pair is the one it touched. At least one is true.
struct ContactNotification
{
    ContactEvent contact;
    bool notifyObjectA = false;
    bool notifyObjectB = false;

    bool operator==(const ContactNotification&) const = default;
};

// Deterministic publication order: fixed step, object A, object B, then End before Begin.
// End first because, when one pair both ends and begins in the same step, the old contact
// ended before the new one began.
inline bool ContactEventPrecedes(const ContactEvent& left, const ContactEvent& right)
{
    if(left.fixedStep != right.fixedStep)
    {
        return left.fixedStep < right.fixedStep;
    }

    if(left.objectA != right.objectA)
    {
        return left.objectA < right.objectA;
    }

    if(left.objectB != right.objectB)
    {
        return left.objectB < right.objectB;
    }

    const bool leftIsEnd = left.type == ContactEventType::End;
    const bool rightIsEnd = right.type == ContactEventType::End;

    return leftIsEnd && !rightIsEnd;
}
}

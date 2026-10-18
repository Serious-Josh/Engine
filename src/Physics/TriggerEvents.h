#pragma once

#include <cstdint>
#include <string>

namespace Physics
{
enum class TriggerEventType
{
    Enter,
    Exit
};

// Directed, value-owned transition. No native handles or simulation ownership escapes.
struct TriggerEvent
{
    std::uint64_t fixedStep = 0;
    TriggerEventType type = TriggerEventType::Enter;
    std::string triggerObjectID;
    std::string otherObjectID;

    bool operator==(const TriggerEvent&) const = default;
};

inline bool TriggerEventPrecedes(const TriggerEvent& left, const TriggerEvent& right)
{
    if(left.fixedStep != right.fixedStep)
    {
        return left.fixedStep < right.fixedStep;
    }

    if(left.triggerObjectID != right.triggerObjectID)
    {
        return left.triggerObjectID < right.triggerObjectID;
    }

    if(left.otherObjectID != right.otherObjectID)
    {
        return left.otherObjectID < right.otherObjectID;
    }

    return left.type == TriggerEventType::Exit && right.type == TriggerEventType::Enter;
}
}

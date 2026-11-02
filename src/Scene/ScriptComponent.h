#pragma once

#include <string>

// Authored script attachment (AS.5): which EAScript class of which registered Script asset the
// object runs during gameplay. Owns no instance, module or execution state; those belong to the
// gameplay runtime of one Play session.
struct ScriptComponent
{
    static constexpr std::size_t MaximumClassNameBytes = 256;

    bool enabled = true;
    // Canonical "asset:<uuid>" reference to a registered Script asset; empty until assigned.
    std::string scriptReference;
    // Qualified EAScript class name ("Name" or "Namespace::Name"); empty until selected.
    std::string className;

    bool operator==(const ScriptComponent&) const = default;
};

// Lexical contract shared by authoring and both codecs. Whether the asset exists and defines the
// class is decided by script validation before gameplay, never by scene loading.
bool ValidateScriptComponent(const ScriptComponent& script, std::string* error = nullptr);

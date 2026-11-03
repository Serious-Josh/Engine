// Audio.1C: event parameters and volume, buses, VCAs and global parameters. Callers (the gameplay audio session)
// decide what may be changed and restore it; AudioSystem only validates values and forwards them to FMOD Studio.
#include "Audio/AudioSystemInternal.h"

#include <cmath>

namespace Audio
{
namespace
{
    constexpr std::string_view NotRunning = "audio is not running";

    bool ValidVolume(float volume, std::string& error)
    {
        if(!std::isfinite(volume) || volume < 0.0f || volume > AudioSystem::MaximumVolume)
        {
            error = "volume must be a finite value between 0 and " + std::to_string(AudioSystem::MaximumVolume);
            return false;
        }

        return true;
    }

    bool ValidParameterValue(float value, std::string& error)
    {
        if(!std::isfinite(value))
        {
            error = "parameter values must be finite";
            return false;
        }

        return true;
    }

    bool ValidAttributes(const Attributes3D& attributes, std::string& error)
    {
        constexpr float Tolerance = 1.0e-3f;
        const auto finite = [](const std::array<float, 3>& vector)
        {
            return std::isfinite(vector[0]) && std::isfinite(vector[1]) && std::isfinite(vector[2]);
        };
        const auto dot = [](const std::array<float, 3>& left, const std::array<float, 3>& right)
        {
            return left[0] * right[0] + left[1] * right[1] + left[2] * right[2];
        };

        if(!finite(attributes.position) || !finite(attributes.velocity) || !finite(attributes.forward) ||
            !finite(attributes.up))
        {
            error = "3D attributes must be finite";
            return false;
        }

        if(std::fabs(dot(attributes.forward, attributes.forward) - 1.0f) > Tolerance ||
            std::fabs(dot(attributes.up, attributes.up) - 1.0f) > Tolerance ||
            std::fabs(dot(attributes.forward, attributes.up)) > Tolerance)
        {
            error = "3D forward and up vectors must be unit length and perpendicular";
            return false;
        }

        return true;
    }

#if defined(ENGINE_HAS_FMOD)
    using Internal::DescribeResult;

    FMOD_VECTOR ToFMOD(const std::array<float, 3>& vector)
    {
        return FMOD_VECTOR{vector[0], vector[1], vector[2]};
    }

    std::array<float, 3> FromFMOD(const FMOD_VECTOR& vector)
    {
        return {vector.x, vector.y, vector.z};
    }

    FMOD_3D_ATTRIBUTES ToFMOD(const Attributes3D& attributes)
    {
        return FMOD_3D_ATTRIBUTES{ToFMOD(attributes.position), ToFMOD(attributes.velocity),
            ToFMOD(attributes.forward), ToFMOD(attributes.up)};
    }

    Attributes3D FromFMOD(const FMOD_3D_ATTRIBUTES& attributes)
    {
        return Attributes3D{FromFMOD(attributes.position), FromFMOD(attributes.velocity),
            FromFMOD(attributes.forward), FromFMOD(attributes.up)};
    }

    // FMOD needs NUL-terminated names; Engine passes views.
    std::string Text(std::string_view text)
    {
        return std::string(text);
    }

    template<typename Object>
    Object* Find(FMOD::Studio::System* studio, FMOD_RESULT (FMOD::Studio::System::*lookup)(const char*, Object**) const,
        std::string_view path)
    {
        Object* object = nullptr;

        if(!studio || path.empty() || (studio->*lookup)(Text(path).c_str(), &object) != FMOD_OK)
        {
            return nullptr;
        }

        return object;
    }

    template<typename Object>
    std::optional<float> GetVolume(Object* object)
    {
        float volume = 0.0f;

        if(!object || object->getVolume(&volume) != FMOD_OK)
        {
            return std::nullopt;
        }

        return volume;
    }

    template<typename Object>
    bool SetVolume(Object* object, std::string_view path, float volume, std::string& error)
    {
        if(!object)
        {
            error = "unknown bus or VCA '" + Text(path) + "'";
            return false;
        }

        const FMOD_RESULT result = object->setVolume(volume);

        if(result != FMOD_OK)
        {
            error = DescribeResult(result);
            return false;
        }

        return true;
    }
#endif
}

std::optional<Guid> AudioSystem::ResolveEventReference(std::string_view reference) const
{
    if(reference.starts_with('{'))
    {
        const auto guid = ParseGuid(reference);
        return guid && HasEvent(*guid) ? guid : std::nullopt;
    }

    return FindEvent(reference);
}

bool AudioSystem::SetEventParameter(EventHandle handle, std::string_view name, float value, std::string& error)
{
    error.clear();

    if(!ValidParameterValue(value, error))
    {
        return false;
    }

#if defined(ENGINE_HAS_FMOD)
    FMOD::Studio::EventInstance* instance = m_impl->ResolveEvent(handle);

    if(!instance)
    {
        error = "the event handle is not valid";
        return false;
    }

    const FMOD_RESULT result = instance->setParameterByName(Text(name).c_str(), value);

    if(result != FMOD_OK)
    {
        error = "cannot set parameter '" + Text(name) + "': " + DescribeResult(result);
        return false;
    }

    return true;
#else
    static_cast<void>(handle);
    static_cast<void>(name);
    error = NotRunning;
    return false;
#endif
}

bool AudioSystem::SetEventVolume(EventHandle handle, float volume, std::string& error)
{
    error.clear();

    if(!ValidVolume(volume, error))
    {
        return false;
    }

#if defined(ENGINE_HAS_FMOD)
    FMOD::Studio::EventInstance* instance = m_impl->ResolveEvent(handle);

    if(!instance)
    {
        error = "the event handle is not valid";
        return false;
    }

    const FMOD_RESULT result = instance->setVolume(volume);

    if(result != FMOD_OK)
    {
        error = DescribeResult(result);
        return false;
    }

    return true;
#else
    static_cast<void>(handle);
    error = NotRunning;
    return false;
#endif
}

std::optional<float> AudioSystem::GetBusVolume(std::string_view bus) const
{
#if defined(ENGINE_HAS_FMOD)
    return m_impl->IsRunning() ? GetVolume(Find(m_impl->studio, &FMOD::Studio::System::getBus, bus)) : std::nullopt;
#else
    static_cast<void>(bus);
    return std::nullopt;
#endif
}

bool AudioSystem::SetBusVolume(std::string_view bus, float volume, std::string& error)
{
    error.clear();

    if(!m_impl->IsRunning())
    {
        error = NotRunning;
        return false;
    }

    if(!ValidVolume(volume, error))
    {
        return false;
    }

#if defined(ENGINE_HAS_FMOD)
    return SetVolume(Find(m_impl->studio, &FMOD::Studio::System::getBus, bus), bus, volume, error);
#else
    static_cast<void>(bus);
    return false;
#endif
}

std::optional<float> AudioSystem::GetVCAVolume(std::string_view vca) const
{
#if defined(ENGINE_HAS_FMOD)
    return m_impl->IsRunning() ? GetVolume(Find(m_impl->studio, &FMOD::Studio::System::getVCA, vca)) : std::nullopt;
#else
    static_cast<void>(vca);
    return std::nullopt;
#endif
}

bool AudioSystem::SetVCAVolume(std::string_view vca, float volume, std::string& error)
{
    error.clear();

    if(!m_impl->IsRunning())
    {
        error = NotRunning;
        return false;
    }

    if(!ValidVolume(volume, error))
    {
        return false;
    }

#if defined(ENGINE_HAS_FMOD)
    return SetVolume(Find(m_impl->studio, &FMOD::Studio::System::getVCA, vca), vca, volume, error);
#else
    static_cast<void>(vca);
    return false;
#endif
}

std::optional<float> AudioSystem::GetGlobalParameter(std::string_view name) const
{
#if defined(ENGINE_HAS_FMOD)
    float value = 0.0f;

    if(!m_impl->IsRunning() || name.empty() ||
        m_impl->studio->getParameterByName(Text(name).c_str(), &value) != FMOD_OK)
    {
        return std::nullopt;
    }

    return value;
#else
    static_cast<void>(name);
    return std::nullopt;
#endif
}

bool AudioSystem::SetGlobalParameter(std::string_view name, float value, std::string& error)
{
    error.clear();

    if(!m_impl->IsRunning())
    {
        error = NotRunning;
        return false;
    }

    if(!ValidParameterValue(value, error))
    {
        return false;
    }

#if defined(ENGINE_HAS_FMOD)
    const FMOD_RESULT result = m_impl->studio->setParameterByName(Text(name).c_str(), value);

    if(result != FMOD_OK)
    {
        error = "cannot set global parameter '" + Text(name) + "': " + DescribeResult(result);
        return false;
    }

    return true;
#else
    static_cast<void>(name);
    return false;
#endif
}
bool AudioSystem::IsEvent3D(const Guid& event) const
{
#if defined(ENGINE_HAS_FMOD)
    if(!m_impl->IsRunning())
    {
        return false;
    }

    const FMOD_GUID id{event.data1, event.data2, event.data3,
        {event.data4[0], event.data4[1], event.data4[2], event.data4[3], event.data4[4], event.data4[5],
            event.data4[6], event.data4[7]}};
    FMOD::Studio::EventDescription* description = nullptr;
    bool is3D = false;
    return m_impl->studio->getEventByID(&id, &description) == FMOD_OK && description->is3D(&is3D) == FMOD_OK && is3D;
#else
    static_cast<void>(event);
    return false;
#endif
}

bool AudioSystem::SetEventAttributes(EventHandle handle, const Attributes3D& attributes, std::string& error)
{
    error.clear();

    if(!ValidAttributes(attributes, error))
    {
        return false;
    }

#if defined(ENGINE_HAS_FMOD)
    FMOD::Studio::EventInstance* instance = m_impl->ResolveEvent(handle);

    if(!instance)
    {
        error = "the event handle is not valid";
        return false;
    }

    const FMOD_3D_ATTRIBUTES fmodAttributes = ToFMOD(attributes);
    const FMOD_RESULT result = instance->set3DAttributes(&fmodAttributes);

    if(result != FMOD_OK)
    {
        error = DescribeResult(result);
        return false;
    }

    return true;
#else
    static_cast<void>(handle);
    error = NotRunning;
    return false;
#endif
}

std::optional<Attributes3D> AudioSystem::GetEventAttributes(EventHandle handle) const
{
#if defined(ENGINE_HAS_FMOD)
    FMOD::Studio::EventInstance* instance = m_impl->ResolveEvent(handle);
    FMOD_3D_ATTRIBUTES attributes{};

    if(!instance || instance->get3DAttributes(&attributes) != FMOD_OK)
    {
        return std::nullopt;
    }

    return FromFMOD(attributes);
#else
    static_cast<void>(handle);
    return std::nullopt;
#endif
}

bool AudioSystem::SetListenerAttributes(const Attributes3D& attributes, std::string& error)
{
    error.clear();

    if(!m_impl->IsRunning())
    {
        error = NotRunning;
        return false;
    }

    if(!ValidAttributes(attributes, error))
    {
        return false;
    }

#if defined(ENGINE_HAS_FMOD)
    const FMOD_3D_ATTRIBUTES fmodAttributes = ToFMOD(attributes);
    const FMOD_RESULT result = m_impl->studio->setListenerAttributes(0, &fmodAttributes);

    if(result != FMOD_OK)
    {
        error = DescribeResult(result);
        return false;
    }

    return true;
#else
    return false;
#endif
}

std::optional<Attributes3D> AudioSystem::GetListenerAttributes() const
{
#if defined(ENGINE_HAS_FMOD)
    FMOD_3D_ATTRIBUTES attributes{};

    if(!m_impl->IsRunning() || m_impl->studio->getListenerAttributes(0, &attributes) != FMOD_OK)
    {
        return std::nullopt;
    }

    return FromFMOD(attributes);
#else
    return std::nullopt;
#endif
}
}

// Audio.1B: FMOD Studio banks, event lookup and the minimal event-instance ownership AudioSystem needs to validate
// them. Script-facing playback (sessions, handles per Play) is Audio.1C.
#include "Audio/AudioSystemInternal.h"
#include "Core/Log.h"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <limits>

namespace Audio
{
namespace
{
    bool ParseHex(std::string_view text, std::uint64_t& value)
    {
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value, 16);
        return error == std::errc{} && end == text.data() + text.size();
    }

    constexpr std::string_view NotRunning = "audio is not running";

#if defined(ENGINE_HAS_FMOD)
    using Internal::DescribeResult;

    FMOD_GUID ToFMOD(const Guid& guid)
    {
        FMOD_GUID result{};
        result.Data1 = guid.data1;
        result.Data2 = guid.data2;
        result.Data3 = guid.data3;
        std::copy(guid.data4.begin(), guid.data4.end(), result.Data4);
        return result;
    }

    Guid FromFMOD(const FMOD_GUID& guid)
    {
        Guid result;
        result.data1 = guid.Data1;
        result.data2 = guid.Data2;
        result.data3 = guid.Data3;
        std::copy(std::begin(guid.Data4), std::end(guid.Data4), result.data4.begin());
        return result;
    }

    // An object's path, or empty when no loaded strings bank names it (FMOD_ERR_EVENT_NOTFOUND).
    template<typename Object>
    std::string GetPath(const Object& object)
    {
        char path[512] = {};
        int retrieved = 0;

        if(object.getPath(path, sizeof(path), &retrieved) != FMOD_OK)
        {
            return {};
        }

        return path;
    }

    PlaybackState ToPlaybackState(FMOD_STUDIO_PLAYBACK_STATE state)
    {
        switch(state)
        {
        case FMOD_STUDIO_PLAYBACK_STARTING:
            return PlaybackState::Starting;
        case FMOD_STUDIO_PLAYBACK_PLAYING:
            return PlaybackState::Playing;
        case FMOD_STUDIO_PLAYBACK_SUSTAINING:
            return PlaybackState::Sustaining;
        case FMOD_STUDIO_PLAYBACK_STOPPING:
            return PlaybackState::Stopping;
        case FMOD_STUDIO_PLAYBACK_STOPPED:
            return PlaybackState::Stopped;
        default:
            return PlaybackState::Invalid;
        }
    }
#endif
}

std::optional<Guid> ParseGuid(std::string_view text)
{
    // {xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx}
    if(text.size() != 38 || text.front() != '{' || text.back() != '}' || text[9] != '-' || text[14] != '-' ||
        text[19] != '-' || text[24] != '-')
    {
        return std::nullopt;
    }

    std::uint64_t data1 = 0;
    std::uint64_t data2 = 0;
    std::uint64_t data3 = 0;
    std::uint64_t data4High = 0;
    std::uint64_t data4Low = 0;

    if(!ParseHex(text.substr(1, 8), data1) || !ParseHex(text.substr(10, 4), data2) ||
        !ParseHex(text.substr(15, 4), data3) || !ParseHex(text.substr(20, 4), data4High) ||
        !ParseHex(text.substr(25, 12), data4Low))
    {
        return std::nullopt;
    }

    Guid guid;
    guid.data1 = static_cast<std::uint32_t>(data1);
    guid.data2 = static_cast<std::uint16_t>(data2);
    guid.data3 = static_cast<std::uint16_t>(data3);
    guid.data4[0] = static_cast<std::uint8_t>(data4High >> 8);
    guid.data4[1] = static_cast<std::uint8_t>(data4High);

    for(int index = 0; index < 6; ++index)
    {
        guid.data4[2 + index] = static_cast<std::uint8_t>(data4Low >> (8 * (5 - index)));
    }

    return guid;
}

std::string ToString(const Guid& guid)
{
    char text[39] = {};
    std::snprintf(text, sizeof(text), "{%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x}", guid.data1, guid.data2,
        guid.data3, guid.data4[0], guid.data4[1], guid.data4[2], guid.data4[3], guid.data4[4], guid.data4[5],
        guid.data4[6], guid.data4[7]);
    return text;
}

#if defined(ENGINE_HAS_FMOD)
AudioSystem::Impl::BankEntry* AudioSystem::Impl::FindBank(std::string_view key)
{
    const auto entry = std::find_if(banks.begin(), banks.end(), [key](const BankEntry& bank)
    {
        return bank.key == key;
    });

    return entry == banks.end() ? nullptr : &*entry;
}

const AudioSystem::Impl::BankEntry* AudioSystem::Impl::FindBank(std::string_view key) const
{
    return const_cast<Impl*>(this)->FindBank(key);
}

// Records a finished synchronous load. FMOD guarantees that a failed load leaves no bank behind.
bool AudioSystem::Impl::AddLoadedBank(std::string_view key, FMOD_RESULT result, FMOD::Studio::Bank* bank,
    std::string& error)
{
    if(result == FMOD_ERR_EVENT_ALREADY_LOADED)
    {
        error = "this bank is already loaded under another key";
        return false;
    }

    if(result != FMOD_OK)
    {
        error = DescribeResult(result);
        return false;
    }

    banks.push_back({std::string(key), bank, 1});
    return true;
}

FMOD::Studio::EventInstance* AudioSystem::Impl::ResolveEvent(EventHandle handle) const
{
    if(handle.generation == 0 || handle.index >= eventSlots.size())
    {
        return nullptr;
    }

    const EventSlot& slot = eventSlots[handle.index];

    if(slot.generation != handle.generation || !slot.instance || !slot.instance->isValid())
    {
        return nullptr;
    }

    return slot.instance;
}

void AudioSystem::Impl::FreeEventSlot(std::uint32_t index) noexcept
{
    EventSlot& slot = eventSlots[index];
    slot.instance = nullptr;

    // Skip 0 on wrap-around: it marks the default (always invalid) handle.
    ++slot.generation;

    if(slot.generation == 0)
    {
        slot.generation = 1;
    }

    freeEventSlots.push_back(index);
}

void AudioSystem::Impl::ReapEventSlots() noexcept
{
    for(std::uint32_t index = 0; index < eventSlots.size(); ++index)
    {
        const EventSlot& slot = eventSlots[index];

        if(slot.instance && !slot.instance->isValid())
        {
            FreeEventSlot(index);
        }
    }
}

void AudioSystem::Impl::ReleaseAllEvents() noexcept
{
    for(std::uint32_t index = 0; index < eventSlots.size(); ++index)
    {
        EventSlot& slot = eventSlots[index];

        if(!slot.instance)
        {
            continue;
        }

        if(slot.instance->isValid())
        {
            slot.instance->stop(FMOD_STUDIO_STOP_IMMEDIATE);
            slot.instance->release();
        }

        FreeEventSlot(index);
    }
}

void AudioSystem::Impl::UnloadAllBanks() noexcept
{
    // Reverse load order, so dependent banks unload before the banks loaded ahead of them (master, strings).
    for(auto entry = banks.rbegin(); entry != banks.rend(); ++entry)
    {
        const FMOD_RESULT result = entry->bank->unload();

        if(result != FMOD_OK && result != FMOD_ERR_INVALID_HANDLE)
        {
            Log::Warning("Audio: unloading bank " + entry->key + " reported " + DescribeResult(result) + ".");
        }
    }

    banks.clear();
}
#endif

bool AudioSystem::LoadBankFile(std::string_view key, const std::filesystem::path& path, std::string& error)
{
    error.clear();

    if(!m_impl->IsRunning())
    {
        error = NotRunning;
        return false;
    }

#if defined(ENGINE_HAS_FMOD)
    if(Impl::BankEntry* loaded = m_impl->FindBank(key))
    {
        ++loaded->references;
        return true;
    }

    // FMOD takes UTF-8 file names on Windows; messages use the same spelling.
    const auto utf8 = path.u8string();
    const std::string fileName(utf8.begin(), utf8.end());
    std::error_code status;

    if(!std::filesystem::is_regular_file(path, status))
    {
        error = "bank file not found: " + fileName;
        return false;
    }

    FMOD::Studio::Bank* bank = nullptr;
    const FMOD_RESULT result = m_impl->studio->loadBankFile(fileName.c_str(), FMOD_STUDIO_LOAD_BANK_NORMAL, &bank);

    if(!m_impl->AddLoadedBank(key, result, bank, error))
    {
        error = "cannot load bank " + fileName + ": " + error;
        return false;
    }

    return true;
#else
    static_cast<void>(key);
    static_cast<void>(path);
    return false;
#endif
}

bool AudioSystem::LoadBankMemory(std::string_view key, std::span<const std::byte> bytes, std::string& error)
{
    error.clear();

    if(!m_impl->IsRunning())
    {
        error = NotRunning;
        return false;
    }

#if defined(ENGINE_HAS_FMOD)
    if(Impl::BankEntry* loaded = m_impl->FindBank(key))
    {
        ++loaded->references;
        return true;
    }

    if(bytes.empty() || bytes.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)()))
    {
        error = "bank data is empty or larger than FMOD accepts";
        return false;
    }

    // FMOD_STUDIO_LOAD_MEMORY copies the data before returning; the caller keeps ownership of `bytes`.
    FMOD::Studio::Bank* bank = nullptr;
    const FMOD_RESULT result = m_impl->studio->loadBankMemory(reinterpret_cast<const char*>(bytes.data()),
        static_cast<int>(bytes.size()), FMOD_STUDIO_LOAD_MEMORY, FMOD_STUDIO_LOAD_BANK_NORMAL, &bank);

    if(!m_impl->AddLoadedBank(key, result, bank, error))
    {
        error = "cannot load bank from memory: " + error;
        return false;
    }

    return true;
#else
    static_cast<void>(key);
    static_cast<void>(bytes);
    return false;
#endif
}

bool AudioSystem::UnloadBank(std::string_view key)
{
#if defined(ENGINE_HAS_FMOD)
    Impl::BankEntry* loaded = m_impl->FindBank(key);

    if(!loaded)
    {
        return false;
    }

    if(--loaded->references > 0)
    {
        return true;
    }

    // Unloading destroys the bank's events and event instances. Studio queues the unload for its own thread, so the
    // queue is flushed (a blocking wait for that thread) to make the bank's events and handles invalid on return.
    FMOD_RESULT result = loaded->bank->unload();

    if(result == FMOD_OK)
    {
        result = m_impl->studio->flushCommands();
    }

    if(result != FMOD_OK)
    {
        Log::Warning("Audio: unloading bank " + loaded->key + " reported " + DescribeResult(result) + ".");
    }

    m_impl->banks.erase(m_impl->banks.begin() + (loaded - m_impl->banks.data()));
    m_impl->ReapEventSlots();
    return true;
#else
    static_cast<void>(key);
    return false;
#endif
}

bool AudioSystem::IsBankLoaded(std::string_view key) const
{
#if defined(ENGINE_HAS_FMOD)
    return m_impl->FindBank(key) != nullptr;
#else
    static_cast<void>(key);
    return false;
#endif
}

std::size_t AudioSystem::GetLoadedBankCount() const noexcept
{
#if defined(ENGINE_HAS_FMOD)
    return m_impl->banks.size();
#else
    return 0;
#endif
}

std::vector<EventInfo> AudioSystem::GetBankEvents(std::string_view key) const
{
    std::vector<EventInfo> events;

#if defined(ENGINE_HAS_FMOD)
    const Impl::BankEntry* loaded = m_impl->FindBank(key);
    int count = 0;

    if(!loaded || loaded->bank->getEventCount(&count) != FMOD_OK || count <= 0)
    {
        return events;
    }

    std::vector<FMOD::Studio::EventDescription*> descriptions(static_cast<std::size_t>(count));

    if(loaded->bank->getEventList(descriptions.data(), count, &count) != FMOD_OK)
    {
        return events;
    }

    for(int index = 0; index < count; ++index)
    {
        FMOD_GUID id{};

        if(descriptions[index]->getID(&id) == FMOD_OK)
        {
            events.push_back({FromFMOD(id), GetPath(*descriptions[index])});
        }
    }
#else
    static_cast<void>(key);
#endif

    return events;
}

std::vector<EventInfo> AudioSystem::GetLoadedEvents() const
{
    std::vector<EventInfo> events;

#if defined(ENGINE_HAS_FMOD)
    for(const Impl::BankEntry& bank : m_impl->banks)
    {
        for(EventInfo& event : GetBankEvents(bank.key))
        {
            const bool known = std::any_of(events.begin(), events.end(), [&event](const EventInfo& listed)
            {
                return listed.id == event.id;
            });

            if(!known)
            {
                events.push_back(std::move(event));
            }
        }
    }

    std::sort(events.begin(), events.end(), [](const EventInfo& left, const EventInfo& right)
    {
        if(left.path.empty() != right.path.empty())
        {
            return !left.path.empty();
        }

        return left.path.empty() ? ToString(left.id) < ToString(right.id) : left.path < right.path;
    });
#endif

    return events;
}

std::optional<Guid> AudioSystem::FindEvent(std::string_view path) const
{
#if defined(ENGINE_HAS_FMOD)
    if(!m_impl->IsRunning() || !path.starts_with("event:/"))
    {
        return std::nullopt;
    }

    FMOD_GUID id{};

    if(m_impl->studio->lookupID(std::string(path).c_str(), &id) != FMOD_OK)
    {
        return std::nullopt;
    }

    return FromFMOD(id);
#else
    static_cast<void>(path);
    return std::nullopt;
#endif
}

bool AudioSystem::HasEvent(const Guid& event) const
{
#if defined(ENGINE_HAS_FMOD)
    if(!m_impl->IsRunning())
    {
        return false;
    }

    const FMOD_GUID id = ToFMOD(event);
    FMOD::Studio::EventDescription* description = nullptr;
    return m_impl->studio->getEventByID(&id, &description) == FMOD_OK;
#else
    static_cast<void>(event);
    return false;
#endif
}

std::string AudioSystem::GetEventPath(const Guid& event) const
{
#if defined(ENGINE_HAS_FMOD)
    if(!m_impl->IsRunning())
    {
        return {};
    }

    const FMOD_GUID id = ToFMOD(event);
    FMOD::Studio::EventDescription* description = nullptr;

    if(m_impl->studio->getEventByID(&id, &description) != FMOD_OK)
    {
        return {};
    }

    return GetPath(*description);
#else
    static_cast<void>(event);
    return {};
#endif
}

std::optional<EventHandle> AudioSystem::CreateEventInstance(const Guid& event, std::string& error)
{
    error.clear();

    if(!m_impl->IsRunning())
    {
        error = NotRunning;
        return std::nullopt;
    }

#if defined(ENGINE_HAS_FMOD)
    const FMOD_GUID id = ToFMOD(event);
    FMOD::Studio::EventDescription* description = nullptr;
    FMOD_RESULT result = m_impl->studio->getEventByID(&id, &description);

    if(result != FMOD_OK)
    {
        error = "event " + ToString(event) + " is not in a loaded bank: " + DescribeResult(result);
        return std::nullopt;
    }

    FMOD::Studio::EventInstance* instance = nullptr;
    result = description->createInstance(&instance);

    if(result != FMOD_OK)
    {
        error = "cannot create an instance of " + ToString(event) + ": " + DescribeResult(result);
        return std::nullopt;
    }

    std::uint32_t index = 0;

    if(m_impl->freeEventSlots.empty())
    {
        index = static_cast<std::uint32_t>(m_impl->eventSlots.size());
        m_impl->eventSlots.push_back({nullptr, 1});
    }
    else
    {
        index = m_impl->freeEventSlots.back();
        m_impl->freeEventSlots.pop_back();
    }

    Impl::EventSlot& slot = m_impl->eventSlots[index];
    slot.instance = instance;
    return EventHandle{index, slot.generation};
#else
    static_cast<void>(event);
    return std::nullopt;
#endif
}

bool AudioSystem::StartEvent(EventHandle handle)
{
#if defined(ENGINE_HAS_FMOD)
    FMOD::Studio::EventInstance* instance = m_impl->ResolveEvent(handle);
    return instance && instance->start() == FMOD_OK;
#else
    static_cast<void>(handle);
    return false;
#endif
}

bool AudioSystem::StopEvent(EventHandle handle, bool allowFadeOut)
{
#if defined(ENGINE_HAS_FMOD)
    FMOD::Studio::EventInstance* instance = m_impl->ResolveEvent(handle);
    const FMOD_STUDIO_STOP_MODE mode = allowFadeOut ? FMOD_STUDIO_STOP_ALLOWFADEOUT : FMOD_STUDIO_STOP_IMMEDIATE;
    return instance && instance->stop(mode) == FMOD_OK;
#else
    static_cast<void>(handle);
    static_cast<void>(allowFadeOut);
    return false;
#endif
}

bool AudioSystem::ReleaseEvent(EventHandle handle)
{
#if defined(ENGINE_HAS_FMOD)
    FMOD::Studio::EventInstance* instance = m_impl->ResolveEvent(handle);

    if(!instance)
    {
        return false;
    }

    // FMOD keeps a playing instance alive until it stops, then destroys it; Engine forgets it now.
    instance->release();
    m_impl->FreeEventSlot(handle.index);
    return true;
#else
    static_cast<void>(handle);
    return false;
#endif
}

PlaybackState AudioSystem::GetPlaybackState(EventHandle handle) const
{
#if defined(ENGINE_HAS_FMOD)
    FMOD::Studio::EventInstance* instance = m_impl->ResolveEvent(handle);
    FMOD_STUDIO_PLAYBACK_STATE state = FMOD_STUDIO_PLAYBACK_STOPPED;

    if(!instance || instance->getPlaybackState(&state) != FMOD_OK)
    {
        return PlaybackState::Invalid;
    }

    return ToPlaybackState(state);
#else
    static_cast<void>(handle);
    return PlaybackState::Invalid;
#endif
}

std::size_t AudioSystem::GetEventInstanceCount() const
{
#if defined(ENGINE_HAS_FMOD)
    return static_cast<std::size_t>(std::count_if(m_impl->eventSlots.begin(), m_impl->eventSlots.end(),
        [](const Impl::EventSlot& slot)
        {
            return slot.instance && slot.instance->isValid();
        }));
#else
    return 0;
#endif
}
}

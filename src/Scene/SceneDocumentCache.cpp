#include "Scene/SceneDocumentCache.h"

#include "Core/Log.h"
#include "Debug/StreamingTelemetry.h"
#include "Scene/SceneSerializer.h"

#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <cwctype>
#include <fstream>
#include <iterator>
#include <mutex>
#include <unordered_map>

namespace SceneDocumentCache
{
    namespace
    {
        namespace fs = std::filesystem;
        using Clock = std::chrono::steady_clock;
        using StreamingTelemetry::Metric;

        // A project rarely has more than a handful of large scenes; the byte bound keeps the retained documents (several times
        // their file size in memory) proportionate. Eviction only costs a reparse.
        constexpr std::size_t MaximumEntries = 32;
        constexpr std::size_t MaximumCachedFileBytes = 256ull * 1024 * 1024;

        struct Entry
        {
            std::string bytes;
            std::shared_ptr<const SceneDocument> document;
            std::uint64_t storedSchemaVersion = 0;
            std::uint64_t lastUse = 0;
        };

        struct State
        {
            std::mutex mutex;
            std::unordered_map<std::wstring, Entry> entries;
            std::uint64_t useClock = 0;
            std::size_t cachedFileBytes = 0;
            Statistics statistics;
        };

        State& GetState()
        {
            static State state;
            return state;
        }

        bool Enabled()
        {
            static const bool enabled = []
            {
                char value[8]{};
                const DWORD length = GetEnvironmentVariableA("ENGINE_SCENE_DOCUMENT_CACHE", value, sizeof(value));
                return !(length == 1 && value[0] == '0');
            }();

            return enabled;
        }

        bool Tracing()
        {
            static const bool tracing = GetEnvironmentVariableA("ENGINE_SCENE_DOCUMENT_TRACE", nullptr, 0) != 0;
            return tracing;
        }

        double Milliseconds(Clock::time_point start)
        {
            return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        }

        // Windows paths are case-insensitive; a key mismatch only costs a reparse, never a wrong document.
        std::wstring Key(const fs::path& path)
        {
            std::error_code error;
            const fs::path absolute = fs::absolute(path, error);
            std::wstring key = (error ? path : absolute).lexically_normal().native();
            std::ranges::transform(key, key.begin(), [](wchar_t character) { return static_cast<wchar_t>(std::towlower(character)); });
            return key;
        }

        void Trace(std::string_view consumer, const fs::path& path, std::size_t bytes, const char* outcome, double milliseconds)
        {
            if(Tracing())
            {
                Log::Info("[SceneDocs] " + std::string(consumer) + " " + path.filename().string() + " bytes=" + std::to_string(bytes) +
                    " " + outcome + " " + std::to_string(milliseconds) + " ms");
            }
        }

        // Caller holds the lock.
        void EvictToBounds(State& state)
        {
            while(!state.entries.empty() && (state.entries.size() > MaximumEntries || state.cachedFileBytes > MaximumCachedFileBytes))
            {
                const auto oldest = std::ranges::min_element(state.entries, {}, [](const auto& entry) { return entry.second.lastUse; });
                state.cachedFileBytes -= oldest->second.bytes.size();
                state.entries.erase(oldest);
                ++state.statistics.evictions;
            }
        }

        void Store(const fs::path& path, std::string_view bytes, std::shared_ptr<const SceneDocument> document,
            std::uint64_t storedSchemaVersion)
        {
            if(bytes.size() > MaximumCachedFileBytes)
            {
                return;
            }

            Entry entry{std::string(bytes), std::move(document), storedSchemaVersion, 0};
            const std::wstring key = Key(path);
            auto& state = GetState();
            const std::lock_guard lock(state.mutex);
            entry.lastUse = ++state.useClock;

            if(const auto found = state.entries.find(key); found != state.entries.end())
            {
                state.cachedFileBytes -= found->second.bytes.size();
                state.entries.erase(found);
            }

            state.cachedFileBytes += entry.bytes.size();
            state.entries.emplace(key, std::move(entry));
            EvictToBounds(state);
        }
    }

    bool Parse(const fs::path& path, std::string_view bytes, Parsed& parsed, std::string* errorMessage, std::string_view consumer)
    {
        StreamingTelemetry::Add(Metric::SceneDocumentReads, 1);
        StreamingTelemetry::Add(Metric::SceneDocumentReadBytes, bytes.size());
        parsed = {};

        if(Enabled())
        {
            const auto compareStart = Clock::now();
            const std::wstring key = Key(path);
            auto& state = GetState();
            const std::lock_guard lock(state.mutex);
            const auto found = state.entries.find(key);

            // Exact content: any differing byte (whatever the file's size or timestamp) is a miss.
            if(found != state.entries.end() && std::string_view(found->second.bytes) == bytes)
            {
                found->second.lastUse = ++state.useClock;
                ++state.statistics.hits;
                parsed = {found->second.document, found->second.storedSchemaVersion};
                StreamingTelemetry::Add(Metric::SceneDocumentCacheHits, 1);
                StreamingTelemetry::Add(Metric::SceneDocumentCompareNs, StreamingTelemetry::ElapsedNanoseconds(compareStart));
                Trace(consumer, path, bytes.size(), "hit", Milliseconds(compareStart));

                if(errorMessage)
                {
                    errorMessage->clear();
                }

                return true;
            }

            StreamingTelemetry::Add(Metric::SceneDocumentCompareNs, StreamingTelemetry::ElapsedNanoseconds(compareStart));
        }

        const auto parseStart = Clock::now();
        auto document = std::make_shared<SceneDocument>();
        std::uint64_t storedSchemaVersion = 0;
        const bool valid = ParseSceneDocument(bytes, *document, errorMessage, &storedSchemaVersion);
        parsed.storedSchemaVersion = storedSchemaVersion;
        Trace(consumer, path, bytes.size(), valid ? "parse" : "parse-failed", Milliseconds(parseStart));

        {
            auto& state = GetState();
            const std::lock_guard lock(state.mutex);
            ++state.statistics.parses;
        }

        if(!valid)
        {
            return false;
        }

        parsed.document = document;

        if(Enabled())
        {
            Store(path, bytes, std::move(document), storedSchemaVersion);
        }

        return true;
    }

    bool Load(const fs::path& path, Parsed& parsed, std::string* errorMessage, std::string_view consumer, std::string* bytes)
    {
        parsed = {};
        std::ifstream file(path, std::ios::binary);

        // The same refusal as LoadSceneDocument.
        if(!file)
        {
            if(errorMessage)
            {
                *errorMessage = "Failed to open scene file.";
            }

            return false;
        }

        std::string contents((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

        if(file.bad())
        {
            if(errorMessage)
            {
                *errorMessage = "Failed to read scene file.";
            }

            return false;
        }

        const bool valid = Parse(path, contents, parsed, errorMessage, consumer);

        if(bytes)
        {
            *bytes = std::move(contents);
        }

        return valid;
    }

    void Seed(const fs::path& path, std::string_view bytes, SceneDocument document, std::uint64_t storedSchemaVersion)
    {
        if(!Enabled())
        {
            return;
        }

        Store(path, bytes, std::make_shared<const SceneDocument>(std::move(document)), storedSchemaVersion);
        auto& state = GetState();
        const std::lock_guard lock(state.mutex);
        ++state.statistics.seeds;
    }

    SceneDocument Copy(const SceneDocument& document)
    {
        const StreamingTelemetry::Scope copyTiming(Metric::SceneDocumentCopyNs, Metric::SceneDocumentCopies);
        return document;
    }

    void Clear()
    {
        auto& state = GetState();
        const std::lock_guard lock(state.mutex);
        state.entries.clear();
        state.cachedFileBytes = 0;
    }

    Statistics GetStatistics()
    {
        auto& state = GetState();
        const std::lock_guard lock(state.mutex);
        Statistics statistics = state.statistics;
        statistics.entries = state.entries.size();
        statistics.cachedFileBytes = state.cachedFileBytes;
        return statistics;
    }
}

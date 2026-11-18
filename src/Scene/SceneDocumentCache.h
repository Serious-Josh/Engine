#pragma once

#include "Scene/SceneDocument.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

// W6.0 parse-once scene documents. Parsing a large scene is expensive (about 10 s for 8,192 objects in Debug), and several
// consumers read the same unchanged file during one load (project scan, World membership, composition, open checks).
//
// Ownership and validity:
// - The cache maps one file's exact bytes to the immutable document ParseSceneDocument produced from them. Parsing is a pure
//   function of the bytes, so a cached document is exactly what parsing those bytes again would return.
// - A consumer always reads the file itself and presents the bytes; a hit requires the cached bytes to equal them byte for
//   byte. File timestamps and sizes are never trusted: an edit that keeps both still differs in content and is reparsed.
// - Only on-disk content is cached. Unsaved editor state lives in the scene runtime and is never stored or returned here.
// - Documents are shared and read-only; a consumer that edits takes a copy (Copy).
// - Bounded LRU (entries and cached file bytes); eviction only costs a later reparse. Clear drops everything.
// - Thread-safe; parsing runs outside the lock.
// - ENGINE_SCENE_DOCUMENT_CACHE=0 bypasses the cache (every request parses, nothing is stored).
// - ENGINE_SCENE_DOCUMENT_TRACE logs every request with its consumer, hit or parse, and timings.
namespace SceneDocumentCache
{
    struct Parsed
    {
        std::shared_ptr<const SceneDocument> document;

        // The file's stored schema version, reported even when the document is refused (as ParseSceneDocument does).
        std::uint64_t storedSchemaVersion = 0;
    };

    // `bytes` are the exact contents of `path` as the caller just read them. Returns the document they parse to, reusing an
    // earlier parse of identical bytes. On failure `parsed.document` is null; failures are not cached.
    bool Parse(const std::filesystem::path& path, std::string_view bytes, Parsed& parsed, std::string* errorMessage,
        std::string_view consumer);

    // Reads `path` and parses it as Parse does. `bytes` receives the file contents when given.
    bool Load(const std::filesystem::path& path, Parsed& parsed, std::string* errorMessage, std::string_view consumer,
        std::string* bytes = nullptr);

    // A parse of `bytes` the caller already holds (a write's read-back verification): the next reader of the same bytes reuses
    // it. `document` must be ParseSceneDocument's result for exactly these bytes.
    void Seed(const std::filesystem::path& path, std::string_view bytes, SceneDocument document, std::uint64_t storedSchemaVersion);

    // A private, editable copy of a shared document.
    SceneDocument Copy(const SceneDocument& document);

    void Clear();

    struct Statistics
    {
        std::uint64_t hits = 0;
        std::uint64_t parses = 0;
        std::uint64_t seeds = 0;
        std::uint64_t evictions = 0;
        std::size_t entries = 0;
        std::size_t cachedFileBytes = 0;
    };

    Statistics GetStatistics();
}

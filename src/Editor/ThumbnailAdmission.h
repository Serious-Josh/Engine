#pragma once

#include <chrono>
#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <vector>

// Fair, bounded admission of visible Asset Browser thumbnail tiles (B.2).
//
// Keys are stable asset references; tile position and folder order never matter. At most
// `budget` visible tiles are admitted (requested and displayed) at once. Waiting tiles are
// served oldest-first. When tiles are waiting and the budget is full, an admitted tile that
// has displayed its thumbnail for at least kMinimumResidency yields its place and rejoins the
// back of the queue, so a visible set larger than the budget cannot starve.
//
// This class owns no GPU state. Releasing a key only means "stop requesting and displaying";
// the renderer still retires the image's resources through its fence and pin rules.
class ThumbnailAdmission
{
public:
    using Clock = std::chrono::steady_clock;

    static constexpr std::chrono::milliseconds kMinimumResidency{2000};
    static constexpr std::chrono::milliseconds kGenerationTimeout{15000};
    static constexpr std::chrono::milliseconds kFailureBackoff{5000};

    enum class TileState
    {
        Queued,
        Admitted,
        CoolingDown
    };

    // Start of a browser frame: every entry is invisible until marked.
    void BeginFrame();

    // Records that the tile is visible this frame and returns its admission state.
    TileState MarkVisible(const std::string& key, Clock::time_point now);

    // The tile displayed a completed thumbnail this frame.
    void MarkShown(const std::string& key, Clock::time_point now);

    // The tile's request failed; it leaves admission and is not retried before the backoff.
    void MarkFailed(const std::string& key, Clock::time_point now);

    // End of a browser frame. Forgets invisible entries, times out stuck generations, rotates at
    // most one long-shown tile when others wait, then admits waiting tiles oldest-first. Every key
    // that lost admission this frame (including failures) is appended to `released`.
    void Schedule(std::size_t budget, Clock::time_point now, std::vector<std::string>& released);

    void Clear();

    std::size_t GetAdmittedCount() const;
    std::size_t GetWaitingCount(Clock::time_point now) const;

private:
#if defined(ENGINE_HAS_VERIFICATION)
    friend class ThumbnailSchedulingVerification;
#endif
    struct Entry
    {
        bool visible = false;
        bool admitted = false;
        Clock::time_point waitingSince{};
        Clock::time_point admittedAt{};
        Clock::time_point retryAfter{};
        std::optional<Clock::time_point> shownSince;
    };

    void Release(const std::string& key, Entry& entry, Clock::time_point requeueAt, std::vector<std::string>& released);

    std::map<std::string, Entry> m_entries;
    std::vector<std::string> m_failed;
};

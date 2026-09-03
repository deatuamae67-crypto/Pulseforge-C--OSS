#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace pulseforge {

// PULSEFORGE_P1_5_0G1_PROVIDER_NEUTRAL_MEDIA_HUB_V1
// A bounded media catalogue/queue model inspired by Metrolist's separation of
// media metadata, queue state and playback state. Network/provider policy lives
// outside this class so the deterministic engine never depends on a remote API.
enum class MediaSourceKind : std::uint8_t {
    local_file,
    external_provider,
};

enum class MediaPlaybackStatus : std::uint8_t {
    stopped,
    buffering,
    playing,
    paused,
    failed,
};

struct MediaItem {
    std::string provider_id;
    std::string id;
    std::string title;
    std::string artist;
    std::string album;
    std::string uri;
    std::string art_url;
    double duration_ms{};
    MediaSourceKind source{MediaSourceKind::local_file};

    [[nodiscard]] bool operator==(const MediaItem&) const = default;
};

struct MediaNowPlaying {
    std::optional<MediaItem> item;
    MediaPlaybackStatus status{MediaPlaybackStatus::stopped};
    double position_ms{};
    double duration_ms{};
};

class MediaHub final {
public:
    static constexpr std::size_t maximum_items = 50'000U;
    static constexpr std::size_t maximum_queue_entries = 4'096U;
    static constexpr std::size_t maximum_search_results = 100U;

    [[nodiscard]] bool upsert(MediaItem item, std::string* error = nullptr);
    [[nodiscard]] bool erase(
        std::string_view provider_id,
        std::string_view id
    ) noexcept;
    void clear() noexcept;

    [[nodiscard]] std::size_t item_count() const noexcept;
    [[nodiscard]] std::optional<MediaItem> find(
        std::string_view provider_id,
        std::string_view id
    ) const;
    [[nodiscard]] std::vector<MediaItem> search(
        std::string_view query,
        std::size_t limit = 25U
    ) const;

    [[nodiscard]] bool enqueue(
        std::string_view provider_id,
        std::string_view id,
        std::string* error = nullptr
    );
    [[nodiscard]] bool select_queue_index(std::size_t index) noexcept;
    [[nodiscard]] bool next() noexcept;
    [[nodiscard]] bool previous() noexcept;
    void clear_queue() noexcept;
    [[nodiscard]] std::size_t queue_size() const noexcept;
    [[nodiscard]] std::optional<std::size_t> queue_index() const noexcept;

    void update_playback(
        MediaPlaybackStatus status,
        double position_ms,
        double duration_ms
    ) noexcept;
    [[nodiscard]] MediaNowPlaying now_playing() const;

private:
    [[nodiscard]] static std::string key(
        std::string_view provider_id,
        std::string_view id
    );
    void rebuild_index() noexcept;

    std::vector<MediaItem> items_;
    std::unordered_map<std::string, std::size_t> index_;
    std::vector<std::string> queue_;
    std::optional<std::size_t> queue_index_;
    MediaPlaybackStatus playback_status_{MediaPlaybackStatus::stopped};
    double playback_position_ms_{};
    double playback_duration_ms_{};
};

}  // namespace pulseforge

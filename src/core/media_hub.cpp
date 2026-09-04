#include "pulseforge/media_hub.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace pulseforge {
namespace {

[[nodiscard]] char ascii_lower(const char value) noexcept {
    return value >= 'A' && value <= 'Z'
        ? static_cast<char>(value - 'A' + 'a')
        : value;
}

[[nodiscard]] std::string lower_ascii(std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(), ascii_lower);
    return result;
}

[[nodiscard]] bool media_item_is_bounded(const MediaItem& item) noexcept {
    return !item.provider_id.empty() && item.provider_id.size() <= 64U
        && !item.id.empty() && item.id.size() <= 256U
        && !item.title.empty() && item.title.size() <= 512U
        && item.artist.size() <= 512U
        && item.album.size() <= 512U
        && item.uri.size() <= 2'048U
        && item.art_url.size() <= 2'048U
        && std::isfinite(item.duration_ms)
        && item.duration_ms >= 0.0;
}

}  // namespace

std::string MediaHub::key(
    const std::string_view provider_id,
    const std::string_view id
) {
    std::string result;
    result.reserve(provider_id.size() + id.size() + 1U);
    result.append(provider_id);
    result.push_back('\x1f');
    result.append(id);
    return result;
}

bool MediaHub::upsert(MediaItem item, std::string* const error) {
    if (!media_item_is_bounded(item)) {
        if (error != nullptr) *error = "media item is empty, non-finite, or exceeds bounded metadata limits";
        return false;
    }
    const auto item_key = key(item.provider_id, item.id);
    if (const auto found = index_.find(item_key); found != index_.end()) {
        items_[found->second] = std::move(item);
        return true;
    }
    if (items_.size() >= maximum_items) {
        if (error != nullptr) *error = "media catalogue reached its bounded item capacity";
        return false;
    }
    index_.emplace(item_key, items_.size());
    items_.push_back(std::move(item));
    return true;
}

bool MediaHub::erase(
    const std::string_view provider_id,
    const std::string_view id
) noexcept {
    try {
        const auto item_key = key(provider_id, id);
        const auto found = index_.find(item_key);
        if (found == index_.end()) return false;
        const auto removed_index = found->second;
        items_.erase(items_.begin() + static_cast<std::ptrdiff_t>(removed_index));
        queue_.erase(
            std::remove(queue_.begin(), queue_.end(), item_key),
            queue_.end()
        );
        if (queue_.empty()) {
            queue_index_.reset();
            playback_status_ = MediaPlaybackStatus::stopped;
        } else if (queue_index_.has_value() && *queue_index_ >= queue_.size()) {
            queue_index_ = queue_.size() - 1U;
        }
        rebuild_index();
        return true;
    } catch (...) {
        return false;
    }
}

void MediaHub::clear() noexcept {
    items_.clear();
    index_.clear();
    clear_queue();
}

std::size_t MediaHub::item_count() const noexcept {
    return items_.size();
}

std::optional<MediaItem> MediaHub::find(
    const std::string_view provider_id,
    const std::string_view id
) const {
    const auto found = index_.find(key(provider_id, id));
    return found == index_.end()
        ? std::nullopt
        : std::optional<MediaItem>{items_[found->second]};
}

std::vector<MediaItem> MediaHub::search(
    const std::string_view query,
    const std::size_t limit
) const {
    const auto bounded_limit = std::min(limit, maximum_search_results);
    std::vector<MediaItem> result;
    result.reserve(std::min(bounded_limit, items_.size()));
    if (bounded_limit == 0U) return result;

    const auto needle = lower_ascii(query);
    for (const auto& item : items_) {
        if (!needle.empty()) {
            const auto matches = [&](const std::string& value) {
                return lower_ascii(value).find(needle) != std::string::npos;
            };
            if (!matches(item.title) && !matches(item.artist)
                && !matches(item.album) && !matches(item.provider_id)) {
                continue;
            }
        }
        result.push_back(item);
        if (result.size() >= bounded_limit) break;
    }
    return result;
}

bool MediaHub::enqueue(
    const std::string_view provider_id,
    const std::string_view id,
    std::string* const error
) {
    const auto item_key = key(provider_id, id);
    if (index_.find(item_key) == index_.end()) {
        if (error != nullptr) *error = "cannot enqueue media that is not in the catalogue";
        return false;
    }
    if (queue_.size() >= maximum_queue_entries) {
        if (error != nullptr) *error = "media queue reached its bounded capacity";
        return false;
    }
    queue_.push_back(item_key);
    if (!queue_index_.has_value()) queue_index_ = 0U;
    return true;
}

bool MediaHub::select_queue_index(const std::size_t index) noexcept {
    if (index >= queue_.size()) return false;
    queue_index_ = index;
    playback_position_ms_ = 0.0;
    playback_duration_ms_ = 0.0;
    playback_status_ = MediaPlaybackStatus::stopped;
    return true;
}

bool MediaHub::next() noexcept {
    if (!queue_index_.has_value() || *queue_index_ + 1U >= queue_.size()) return false;
    return select_queue_index(*queue_index_ + 1U);
}

bool MediaHub::previous() noexcept {
    if (!queue_index_.has_value() || *queue_index_ == 0U) return false;
    return select_queue_index(*queue_index_ - 1U);
}

void MediaHub::clear_queue() noexcept {
    queue_.clear();
    queue_index_.reset();
    playback_status_ = MediaPlaybackStatus::stopped;
    playback_position_ms_ = 0.0;
    playback_duration_ms_ = 0.0;
}

std::size_t MediaHub::queue_size() const noexcept {
    return queue_.size();
}

std::optional<std::size_t> MediaHub::queue_index() const noexcept {
    return queue_index_;
}

void MediaHub::update_playback(
    const MediaPlaybackStatus status,
    const double position_ms,
    const double duration_ms
) noexcept {
    playback_status_ = status;
    playback_duration_ms_ = std::isfinite(duration_ms) && duration_ms >= 0.0
        ? duration_ms
        : 0.0;
    playback_position_ms_ = std::clamp(
        std::isfinite(position_ms) && position_ms >= 0.0 ? position_ms : 0.0,
        0.0,
        playback_duration_ms_ > 0.0
            ? playback_duration_ms_
            : std::numeric_limits<double>::max()
    );
}

MediaNowPlaying MediaHub::now_playing() const {
    MediaNowPlaying result;
    result.status = playback_status_;
    result.position_ms = playback_position_ms_;
    result.duration_ms = playback_duration_ms_;
    if (!queue_index_.has_value() || *queue_index_ >= queue_.size()) return result;
    const auto found = index_.find(queue_[*queue_index_]);
    if (found != index_.end()) result.item = items_[found->second];
    return result;
}

void MediaHub::rebuild_index() noexcept {
    try {
        index_.clear();
        index_.reserve(items_.size());
        for (std::size_t index = 0U; index < items_.size(); ++index) {
            index_.emplace(key(items_[index].provider_id, items_[index].id), index);
        }
    } catch (...) {
        // If allocation fails, fail closed: lookups will miss instead of using
        // stale indices after an erase.
        index_.clear();
    }
}

}  // namespace pulseforge

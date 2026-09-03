#include "pulseforge/media_hub.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

pulseforge::MediaItem item(
    std::string id,
    std::string title,
    std::string artist,
    pulseforge::MediaSourceKind source = pulseforge::MediaSourceKind::local_file
) {
    pulseforge::MediaItem result;
    result.provider_id = source == pulseforge::MediaSourceKind::local_file
        ? "local"
        : "external";
    result.id = std::move(id);
    result.title = std::move(title);
    result.artist = std::move(artist);
    result.album = "PulseForge Media";
    result.uri = source == pulseforge::MediaSourceKind::local_file
        ? "C:/music/track.ogg"
        : "https://example.test/media";
    result.duration_ms = 180'000.0;
    result.source = source;
    return result;
}

void test_catalog_search_and_upsert() {
    pulseforge::MediaHub hub;
    require(hub.upsert(item("a", "Launcher Theme", "Alice")), "local item inserts");
    require(hub.upsert(item("b", "Boss Theme", "Bob")), "second item inserts");
    require(
        hub.upsert(item("remote", "Remote Track", "Carol", pulseforge::MediaSourceKind::external_provider)),
        "provider-neutral external metadata inserts without networking in the core"
    );
    require(hub.item_count() == 3U, "catalogue counts stable provider/id identities");
    const auto matches = hub.search("theme", 20U);
    require(matches.size() == 2U, "search matches title fields case-insensitively");

    auto replacement = item("a", "Launcher Theme Remastered", "Alice");
    require(hub.upsert(replacement), "same provider/id performs an upsert");
    require(hub.item_count() == 3U, "upsert never duplicates stable identity");
    const auto found = hub.find("local", "a");
    require(found.has_value() && found->title == "Launcher Theme Remastered", "upsert replaces metadata");
}

void test_queue_and_now_playing() {
    pulseforge::MediaHub hub;
    require(hub.upsert(item("a", "A", "Artist")), "A inserts");
    require(hub.upsert(item("b", "B", "Artist")), "B inserts");
    require(hub.enqueue("local", "a"), "A queues");
    require(hub.enqueue("local", "b"), "B queues");
    require(hub.queue_size() == 2U && hub.queue_index() == 0U, "first enqueue selects queue head");
    hub.update_playback(pulseforge::MediaPlaybackStatus::playing, 12'500.0, 180'000.0);
    auto now = hub.now_playing();
    require(now.item.has_value() && now.item->id == "a", "Now Playing resolves current queue item");
    require(now.position_ms == 12'500.0, "Now Playing carries bounded playback position");
    require(hub.next(), "queue advances");
    now = hub.now_playing();
    require(now.item.has_value() && now.item->id == "b", "next selects following item");
    require(hub.previous(), "queue moves backwards");
    require(!hub.previous(), "queue never wraps implicitly past its first item");
}

void test_bounds_and_erase() {
    pulseforge::MediaHub hub;
    auto invalid = item("bad", "", "Nobody");
    std::string error;
    require(!hub.upsert(invalid, &error) && !error.empty(), "empty media title is rejected");
    require(hub.upsert(item("a", "A", "Artist")), "valid item inserts");
    require(hub.enqueue("local", "a"), "valid item queues");
    require(hub.erase("local", "a"), "erase removes item");
    require(hub.item_count() == 0U && hub.queue_size() == 0U, "erase also removes stale queue references");
    require(!hub.enqueue("local", "missing"), "unknown media cannot enter the queue");
}

}  // namespace

int main() {
    try {
        test_catalog_search_and_upsert();
        test_queue_and_now_playing();
        test_bounds_and_erase();
        std::cout << "3/3 media hub tests passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& exception) {
        std::cerr << "[FAIL] " << exception.what() << '\n';
        return EXIT_FAILURE;
    }
}

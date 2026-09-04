#include "psych_content_roots.hpp"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

std::filesystem::path temp_root() {
    const auto nonce =
        std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path()
        / ("pulseforge-psych-roots-" + std::to_string(nonce));
}

std::string key(const std::filesystem::path& path) {
    return pulseforge::detail::psych_content_roots_detail::path_key(path);
}

void mkdir(const std::filesystem::path& path) {
    std::error_code error;
    std::filesystem::create_directories(path, error);
    if (error) throw std::runtime_error("mkdir failed");
}

void remove_tree(const std::filesystem::path& root) {
    std::error_code error;
    std::filesystem::remove_all(root, error);
}

void test_mod_overrides_all_discovered_base_layers() {
    const auto root = temp_root();
    const auto bin = root / "bin";
    const auto assets = bin / "assets";
    const auto preload = assets / "preload";
    const auto preload_data = preload / "data";
    const auto shared = assets / "shared";
    const auto shared_data = shared / "data";
    const auto mod = bin / "mods" / "mods";

    for (const auto& path : {
             assets, preload, preload_data, shared, shared_data, mod
         }) {
        mkdir(path);
    }

    const std::vector explicit_roots{mod};
    const auto resolved = pulseforge::detail::resolve_psych_content_roots(
        explicit_roots
    );

    require(resolved.roots.size() == 6U, "expected 5 fallbacks + mod");
    require(key(resolved.roots[0]) == key(assets), "assets lowest");
    require(key(resolved.roots[1]) == key(preload), "preload order");
    require(key(resolved.roots[2]) == key(preload_data), "preload/data order");
    require(key(resolved.roots[3]) == key(shared), "shared order");
    require(key(resolved.roots[4]) == key(shared_data), "shared/data order");
    require(key(resolved.roots[5]) == key(mod), "mod highest");
    remove_tree(root);
}

void test_explicit_root_is_promoted() {
    const auto root = temp_root();
    const auto bin = root / "bin";
    const auto assets = bin / "assets";
    const auto shared = assets / "shared";
    const auto mod = bin / "mods" / "mods";
    mkdir(shared);
    mkdir(mod);

    const std::vector explicit_roots{shared, mod};
    const auto resolved = pulseforge::detail::resolve_psych_content_roots(
        explicit_roots
    );

    require(resolved.roots.size() == 3U, "assets + explicit shared + mod");
    require(key(resolved.roots[0]) == key(assets), "assets fallback");
    require(key(resolved.roots[1]) == key(shared), "explicit shared promoted");
    require(key(resolved.roots[2]) == key(mod), "mod highest");
    remove_tree(root);
}

void test_unrelated_tree_is_not_mounted() {
    const auto root = temp_root();
    const auto explicit_root = root / "project" / "content";
    mkdir(explicit_root);
    mkdir(root / "other" / "assets");

    const std::vector roots{explicit_root};
    const auto resolved =
        pulseforge::detail::resolve_psych_content_roots(roots);

    require(resolved.roots.size() == 1U, "unrelated tree stays isolated");
    require(key(resolved.roots[0]) == key(explicit_root), "explicit preserved");
    require(resolved.discovered_fallback_roots.empty(), "no false discovery");
    remove_tree(root);
}

void test_distribution_root_itself() {
    const auto root = temp_root();
    const auto game = root / "game";
    const auto assets = game / "assets";
    const auto shared = assets / "shared";
    mkdir(shared);

    const std::vector roots{game};
    const auto resolved =
        pulseforge::detail::resolve_psych_content_roots(roots);

    require(resolved.roots.size() == 3U, "assets/shared + explicit game");
    require(key(resolved.roots[0]) == key(assets), "assets fallback");
    require(key(resolved.roots[1]) == key(shared), "shared fallback");
    require(key(resolved.roots[2]) == key(game), "game explicit highest");
    remove_tree(root);
}

void test_unicode_distribution_path() {
    const auto root = temp_root();
    const auto unicode_component = std::filesystem::path{
        std::u8string{u8"música-漢字-Δ"}
    };
    const auto game = root / unicode_component;
    const auto assets = game / "assets";
    const auto shared = assets / "shared";
    const auto mod = game / "mods" / "mods";

    mkdir(shared);
    mkdir(mod);

    const std::vector roots{mod};
    const auto resolved =
        pulseforge::detail::resolve_psych_content_roots(roots);

    require(!resolved.roots.empty(), "Unicode resolver returned no roots");
    require(key(resolved.roots.back()) == key(mod), "Unicode mod not highest");
    require(key(resolved.roots.front()) == key(assets),
            "Unicode assets fallback was not discovered");
    remove_tree(root);
}

void test_root_cap_keeps_explicit_mod() {
    const auto root = temp_root();
    const auto bin = root / "bin";
    mkdir(bin / "assets" / "preload" / "data");
    mkdir(bin / "assets" / "shared" / "data");
    const auto mod = bin / "mods" / "mods";
    mkdir(mod);

    const std::vector roots{mod};
    const auto resolved =
        pulseforge::detail::resolve_psych_content_roots(roots, 3U);

    require(resolved.roots.size() == 3U, "bounded to 3");
    require(key(resolved.roots.back()) == key(mod), "mod survives cap");
    require(resolved.omitted_due_to_limit > 0U, "omission reported");
    remove_tree(root);
}


void create_stock_provider_fixture(
    const std::filesystem::path& assets
) {
    for (const auto* song : {"tutorial", "bopeebo", "fresh", "dad-battle"}) {
        mkdir(assets / "songs" / song);
    }
    mkdir(assets / "images");
    mkdir(assets / "sounds");
    mkdir(assets / "music");
    mkdir(assets / "shared");
    mkdir(assets / "data" / "characters");
}

void test_assets_data_layer_is_mounted() {
    const auto root = temp_root();
    const auto game = root / "game";
    const auto assets = game / "assets";
    const auto data = assets / "data";
    const auto mod = game / "mods" / "current";
    mkdir(data / "characters");
    mkdir(mod);

    const std::vector roots{mod};
    const auto resolved =
        pulseforge::detail::resolve_psych_content_roots(roots, 64U, false);

    require(resolved.roots.size() == 3U, "assets/data + explicit root");
    require(key(resolved.roots[0]) == key(assets), "assets base first");
    require(key(resolved.roots[1]) == key(data), "assets/data discovered");
    require(key(resolved.roots[2]) == key(mod), "mod remains highest");
    remove_tree(root);
}

void test_complete_sibling_stock_provider_is_lowest() {
    const auto root = temp_root();
    const auto game = root / "game";
    const auto engine_assets = game / "assets";
    const auto mod = game / "mods" / "current" / "content";
    const auto provider_assets = game / "mods" / "stock-engine" / "assets";
    mkdir(engine_assets);
    mkdir(mod);
    create_stock_provider_fixture(provider_assets);

    const std::vector roots{mod};
    const auto resolved = pulseforge::detail::resolve_psych_content_roots(roots);

    require(resolved.stock_provider_assets_root.has_value(),
            "stock provider discovered");
    require(key(*resolved.stock_provider_assets_root) == key(provider_assets),
            "expected complete provider selected");
    require(!resolved.stock_fallback_roots.empty(),
            "provider contributes fallback roots");
    require(key(resolved.roots.front()) == key(provider_assets),
            "provider is lower precedence than local distribution");
    require(key(resolved.roots.back()) == key(mod),
            "active mod remains authoritative");
    remove_tree(root);
}

void test_stock_provider_can_be_disabled_for_script_discovery() {
    const auto root = temp_root();
    const auto game = root / "game";
    const auto mod = game / "mods" / "current";
    mkdir(game / "assets");
    mkdir(mod);
    create_stock_provider_fixture(game / "mods" / "stock" / "assets");

    const std::vector roots{mod};
    const auto resolved =
        pulseforge::detail::resolve_psych_content_roots(roots, 64U, false);
    require(!resolved.stock_provider_assets_root.has_value(),
            "script-code mode does not discover sibling provider");
    for (const auto& path : resolved.roots) {
        require(key(path).find("stock") == std::string::npos,
                "stock provider is absent from script-code roots");
    }
    remove_tree(root);
}

void test_stock_provider_tie_break_is_deterministic() {
    const auto root = temp_root();
    const auto game = root / "game";
    const auto mod = game / "mods" / "current";
    mkdir(game / "assets");
    mkdir(mod);
    const auto provider_a = game / "mods" / "aaa-provider" / "assets";
    const auto provider_b = game / "mods" / "bbb-provider" / "assets";
    create_stock_provider_fixture(provider_b);
    create_stock_provider_fixture(provider_a);

    const std::vector roots{mod};
    const auto resolved = pulseforge::detail::resolve_psych_content_roots(roots);
    require(resolved.stock_provider_assets_root.has_value(),
            "tied provider selected");
    require(key(*resolved.stock_provider_assets_root) == key(provider_a),
            "lexicographically first equal provider wins");
    remove_tree(root);
}

}  // namespace

int main() {
    try {
        test_mod_overrides_all_discovered_base_layers();
        test_explicit_root_is_promoted();
        test_unrelated_tree_is_not_mounted();
        test_distribution_root_itself();
        test_unicode_distribution_path();
        test_root_cap_keeps_explicit_mod();
        test_assets_data_layer_is_mounted();
        test_complete_sibling_stock_provider_is_lowest();
        test_stock_provider_can_be_disabled_for_script_discovery();
        test_stock_provider_tie_break_is_deterministic();
        std::cout << "[PASS] Generic Psych content-root + stock-provider resolver\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "[FAIL] " << error.what() << '\n';
        return 1;
    }
}

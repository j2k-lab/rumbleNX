#include "utils/updater.hpp"

#include "api/rumble_settings.hpp"
#include "utils/config.hpp"
#include "utils/sha256.hpp"
#include "utils/thread.hpp"

#include <borealis.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <stdexcept>

#ifdef __SWITCH__
#include <switch.h>
#endif

namespace rumblenx::updater {
namespace {

using json = nlohmann::json;

/// 35 MB over a slow connection; GitHub serves assets from a CDN.
constexpr long DOWNLOAD_TIMEOUT_MS = 15 * 60 * 1000;
constexpr int64_t MAX_ASSET_BYTES = 128LL * 1024 * 1024;

std::mutex pathMutex;
std::string runningPath;

bool endsWith(const std::string& value, const std::string& suffix) {
    if (value.size() < suffix.size()) return false;
    return std::equal(suffix.rbegin(), suffix.rend(), value.rbegin(),
        [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); });
}

int64_t fileSize(const std::string& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    return in ? static_cast<int64_t>(in.tellg()) : -1;
}

/// Every NRO has "NRO0" at offset 0x10.
bool hasNroHeader(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    char header[0x14]{};
    if (!in.read(header, sizeof(header))) return false;
    return std::memcmp(header + 0x10, "NRO0", 4) == 0;
}

std::string sha256File(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return in ? sha256Hex(in) : std::string();
}

}  // namespace

void setExecutablePath(const std::string& path) {
    std::lock_guard<std::mutex> lock(pathMutex);
    runningPath = path;
}

std::string executablePath() {
    std::lock_guard<std::mutex> lock(pathMutex);
    return runningPath;
}

bool canInstall() {
#ifdef __SWITCH__
    const std::string path = executablePath();
    return path.rfind("sdmc:/", 0) == 0 && endsWith(path, ".nro") && fileSize(path) > 0;
#else
    return false;
#endif
}

bool checkOnStartup() {
    const json root = rumble::settings::read(rumble::settings::PREFERENCES);
    const auto found = root.find("check_updates_on_start");
    return found == root.end() || !found->is_boolean() || found->get<bool>();
}

void setCheckOnStartup(bool enabled) {
    rumble::settings::update(
        rumble::settings::PREFERENCES, [enabled](json& root) { root["check_updates_on_start"] = enabled; });
}

void checkAsync(std::function<void(std::optional<Release>)> success, std::function<void(std::string)> failure) {
    const std::string repository = AppVersion::getReleaseRepository();
    if (repository.empty()) {
        failure("Update checks are turned off in this build.");
        return;
    }
    ThreadPool::instance().submit([repository, success = std::move(success), failure = std::move(failure)](HTTP&) {
        try {
            const json root = json::parse(HTTP::get("https://api.github.com/repos/" + repository + "/releases/latest",
                                              HTTP::Header{"Accept: application/vnd.github+json"}, HTTP::Timeout{10000}),
                nullptr, false);
            if (root.is_discarded()) throw std::runtime_error("GitHub sent an unreadable answer.");
            Release release = parseRelease(root);
            if (release.version.empty()) throw std::runtime_error("Couldn't read the latest version.");
            std::optional<Release> result;
            if (isNewer(release.version, AppVersion::getVersion())) result = std::move(release);
            brls::sync([success, result]() { success(result); });
        } catch (const std::exception& error) {
            brls::Logger::error("RumbleNX: update check failed: {}", error.what());
            // GitHub answers 404 while a repository has no releases.
            const std::string what = error.what();
            const std::string message = what.find("http status 404") != std::string::npos
                ? "No RumbleNX releases have been published yet."
                : "Couldn't check for updates. Check your internet connection and try again.";
            brls::sync([failure, message]() { failure(message); });
        }
    });
}

void installAsync(const Release& release,
    std::function<void(int64_t, int64_t)> progress,
    std::function<void()> success,
    std::function<void(std::string)> failure,
    HTTP::Cancel cancel) {
    ThreadPool::instance().submit([release, progress = std::move(progress), success = std::move(success),
                                      failure = std::move(failure), cancel](HTTP&) {
        const std::string target = executablePath();
        const std::string partial = target + ".download";
        const std::string backup = target + ".old";
        try {
            if (!canInstall()) throw std::runtime_error("RumbleNX.nro wasn't found on the SD card.");
            if (!release.installable()) throw std::runtime_error("This release has no RumbleNX.nro to install.");
            if (release.assetSize > MAX_ASSET_BYTES) throw std::runtime_error("The update is unexpectedly large.");

            std::remove(partial.c_str());
            auto lastReport = std::chrono::steady_clock::now() - std::chrono::seconds(1);
            HTTP::Progress::Callback onProgress = [&lastReport, progress, &release](curl_off_t total, curl_off_t now) {
                const auto clock = std::chrono::steady_clock::now();
                if (clock - lastReport < std::chrono::milliseconds(200)) return;
                lastReport = clock;
                const int64_t size = total > 0 ? static_cast<int64_t>(total) : release.assetSize;
                const int64_t done = static_cast<int64_t>(now);
                brls::sync([progress, done, size]() { progress(done, size); });
            };
            try {
                // Cancel first: setting it clears progress subscribers.
                HTTP::download(release.assetUrl, partial, HTTP::Timeout{DOWNLOAD_TIMEOUT_MS}, cancel, onProgress);
            } catch (const std::exception&) {
                if (cancel && cancel->load()) throw std::runtime_error("The update was cancelled.");
                throw std::runtime_error("The download failed. Check your internet connection and try again.");
            }
            if (cancel && cancel->load()) throw std::runtime_error("The update was cancelled.");

            if (fileSize(partial) != release.assetSize)
                throw std::runtime_error("The download is incomplete. Check the free space on your SD card.");
            if (!hasNroHeader(partial)) throw std::runtime_error("The download isn't a Switch homebrew app.");
            if (!release.sha256.empty() && sha256File(partial) != release.sha256)
                throw std::runtime_error("The download doesn't match the release's checksum.");

            // The running NRO is already in memory, so its file can be
            // replaced. FAT can't rename over a file: move the old one aside,
            // put the new one in place, and restore the old one on failure.
            std::remove(backup.c_str());
            if (std::rename(target.c_str(), backup.c_str()) != 0)
                throw std::runtime_error("Couldn't replace RumbleNX.nro on the SD card.");
            if (std::rename(partial.c_str(), target.c_str()) != 0) {
                std::rename(backup.c_str(), target.c_str());
                throw std::runtime_error("Couldn't replace RumbleNX.nro on the SD card.");
            }
            std::remove(backup.c_str());
            brls::Logger::info("RumbleNX: installed {} to {}", release.version, target);
            brls::sync([success]() { success(); });
        } catch (const std::exception& error) {
            std::remove(partial.c_str());
            brls::Logger::error("RumbleNX: update failed: {}", error.what());
            const std::string message = error.what();
            brls::sync([failure, message]() { failure(message); });
        }
    });
}

bool restart() {
#ifdef __SWITCH__
    const std::string path = executablePath();
    if (path.empty() || !envHasNextLoad()) return false;
    // hbloader reads both strings after the app exits.
    static std::string loadPath;
    static std::string arguments;
    loadPath = path;
    arguments = "\"" + path + "\"";
    if (R_FAILED(envSetNextLoad(loadPath.c_str(), arguments.c_str()))) return false;
    brls::Application::quit();
    return true;
#else
    return false;
#endif
}

}  // namespace rumblenx::updater

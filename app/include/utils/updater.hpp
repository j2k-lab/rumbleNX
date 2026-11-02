#pragma once

#include "api/http.hpp"
#include "utils/release_info.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

/// Self-update from the GitHub releases of AppVersion::getReleaseRepository():
/// finds the newest release, downloads its RumbleNX.nro asset next to the
/// running NRO, checks it, swaps it in and relaunches through hbloader.
namespace rumblenx::updater {

/// Remembers argv[0], the NRO hbloader started.
void setExecutablePath(const std::string& path);
std::string executablePath();

/// True on Switch when the running NRO is a file on the SD card the
/// updater can replace.
bool canInstall();

/// Startup checks, on by default (settings.json "check_updates_on_start").
bool checkOnStartup();
void setCheckOnStartup(bool enabled);

/// `success` gets the newer release, or nothing when this build is current.
/// A repository without releases fails with "No RumbleNX releases have been
/// published yet." Callbacks run on the UI thread.
void checkAsync(std::function<void(std::optional<Release>)> success, std::function<void(std::string)> failure);

/// Downloads, verifies (size, NRO header, SHA-256 when published) and
/// installs the release over the running NRO. `progress` gets bytes done and
/// total. Callbacks run on the UI thread.
void installAsync(const Release& release,
    std::function<void(int64_t, int64_t)> progress,
    std::function<void()> success,
    std::function<void(std::string)> failure,
    HTTP::Cancel cancel);

/// Asks hbloader to start the installed NRO and quits. False where the
/// environment cannot relaunch; the viewer then restarts by hand.
bool restart();

}  // namespace rumblenx::updater

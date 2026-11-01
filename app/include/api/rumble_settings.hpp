#pragma once

#include <nlohmann/json.hpp>

#include <functional>
#include <string>

/// Small JSON files under sdmc:/config/RumbleNX: playback and chat
/// preferences (settings.json), the signed-in session (session.json) and the
/// followed channels (follows.json). Writes are atomic and serialised.
namespace rumble::settings {

constexpr const char* PREFERENCES = "settings.json";
constexpr const char* SESSION = "session.json";
constexpr const char* FOLLOWS = "follows.json";

std::string directory();
std::string filePath(const std::string& name);

/// The file's JSON object, or an empty object when it is missing or damaged.
nlohmann::json read(const std::string& name);

/// Replaces the file. Returns false when it could not be written.
bool write(const std::string& name, const nlohmann::json& value);

/// Read-modify-write of a JSON object file under one lock.
bool update(const std::string& name, const std::function<void(nlohmann::json&)>& change);

void remove(const std::string& name);

}  // namespace rumble::settings

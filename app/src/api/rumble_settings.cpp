#include "api/rumble_settings.hpp"

#include "utils/config.hpp"

#include <borealis/core/logger.hpp>

#include <filesystem>
#include <fstream>
#include <mutex>

namespace rumble::settings {
namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;

std::mutex& fileMutex() {
    static std::mutex mutex;
    return mutex;
}

json readLocked(const std::string& name) {
    std::ifstream input(filePath(name));
    if (!input.is_open()) return json::object();
    const json root = json::parse(input, nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        brls::Logger::warning("RumbleNX: ignoring damaged {}", name);
        return json::object();
    }
    return root;
}

bool writeLocked(const std::string& name, const json& value) {
    try {
        const fs::path path(filePath(name));
        std::error_code error;
        fs::create_directories(path.parent_path(), error);

        const fs::path temporary = path.string() + ".tmp";
        {
            std::ofstream output(temporary, std::ios::trunc);
            if (!output.is_open()) return false;
            output << value.dump(2) << '\n';
            if (!output.good()) return false;
        }

        // Renaming over an existing file is not reliable on the SD card's
        // FAT filesystem, so the old file is removed first.
        fs::remove(path, error);
        error.clear();
        fs::rename(temporary, path, error);
        if (error) {
            fs::remove(temporary, error);
            return false;
        }
        return true;
    } catch (const std::exception& exception) {
        brls::Logger::warning("RumbleNX: could not write {}: {}", name, exception.what());
        return false;
    }
}

}  // namespace

std::string directory() {
#if defined(__SWITCH__)
    return "sdmc:/config/RumbleNX";
#else
    return AppConfig::instance().configDir();
#endif
}

std::string filePath(const std::string& name) { return directory() + "/" + name; }

json read(const std::string& name) {
    std::lock_guard<std::mutex> lock(fileMutex());
    return readLocked(name);
}

bool write(const std::string& name, const json& value) {
    std::lock_guard<std::mutex> lock(fileMutex());
    return writeLocked(name, value);
}

bool update(const std::string& name, const std::function<void(json&)>& change) {
    std::lock_guard<std::mutex> lock(fileMutex());
    json root = readLocked(name);
    change(root);
    return writeLocked(name, root);
}

void remove(const std::string& name) {
    std::lock_guard<std::mutex> lock(fileMutex());
    std::error_code error;
    fs::remove(filePath(name), error);
}

}  // namespace rumble::settings

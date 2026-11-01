#pragma once

#include "api/rumble_types.hpp"

#include <functional>
#include <string>
#include <vector>

/// Channels the viewer follows in RumbleNX, kept on the SD card
/// (sdmc:/config/RumbleNX/follows.json), newest first. Works without signing
/// in; a signed-in viewer can also import the channels their account follows.
namespace rumble::follows {

std::vector<Channel> load();
bool isFollowing(const std::string& channelPath);
bool follow(const Channel& channel);
bool unfollow(const std::string& channelPath);

/// Refreshes the stored name and picture of channels that are followed.
void refreshDetails(const std::vector<Channel>& channels);

/// Set whenever the list changes, so Home can reload its Following rows.
bool consumeChanged();

/// Adds the channels followed by the signed-in account (from its account
/// page) to the list. Succeeds with how many channels were added.
void importFromAccountAsync(std::function<void(int added, int found)> success, ErrorCallback failure);

}  // namespace rumble::follows

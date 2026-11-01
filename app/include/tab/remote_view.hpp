/*
    Copyright 2023 dragonflylee
*/

#pragma once

#include <client/client.hpp>
#include <api/rumble_types.hpp>

#include <vector>

using DirList = std::vector<remote::DirEntry>;

/// Entry point to the full-screen Rumble player.
class RemoteView {
public:
    /// Asks Rumble how to play `video`, then opens the player: live streams
    /// with chat and live recovery, uploads and past broadcasts as seekable
    /// videos, and a notice for streams that have not started yet.
    static void playVideo(const rumble::Video& video);
};

#pragma once

#include "api/rumble_types.hpp"

#include <functional>
#include <string>
#include <vector>

namespace rumble {

/// A signed-in Rumble session. Only the session cookie and display details
/// are stored (sdmc:/config/RumbleNX/session.json); the password never is.
struct Session {
    /// Value of Rumble's "u_s" session cookie.
    std::string token;
    std::string username;
    std::string avatarUrl;

    bool signedIn() const { return !token.empty(); }
};

Session loadSession();
bool saveSession(const Session& session);
void clearSession();

/// Rumble's password proof for user.login: three comma-separated parts made
/// from the account's salts with salted, stretched MD5 (the same scheme as
/// Rumble's own sign-in page).
std::string passwordHashes(const std::string& password, const std::vector<std::string>& salts);

/// A sign-in waiting for its second factor. Opaque to the UI apart from
/// which delivery methods Rumble offers.
struct SecondFactor {
    std::string username;
    std::string totpId;
    std::string userKey;
    std::string cookies;
    bool authenticator = false;
    bool email = false;
    bool phone = false;
};

struct SignInResult {
    bool needsSecondFactor = false;
    Session session;
    SecondFactor challenge;
};

using SignInCallback = std::function<void(SignInResult)>;
using SessionCallback = std::function<void(Session)>;

/// Signs in with a username (or e-mail address) and password.
void signInAsync(
    const std::string& username,
    const std::string& password,
    SignInCallback success,
    ErrorCallback failure);

/// Asks Rumble to send a one-time code by e-mail (`email`) or text message.
/// Succeeds with where the code was sent, as Rumble describes it.
void requestSecondFactorCodeAsync(
    const SecondFactor& challenge,
    bool email,
    std::function<void(std::string sentTo)> success,
    ErrorCallback failure);

/// Completes a sign-in with the one-time code.
void verifySecondFactorAsync(
    const SecondFactor& challenge,
    const std::string& code,
    SessionCallback success,
    ErrorCallback failure);

/// Ends the session on Rumble (best effort) and forgets it locally.
void signOutAsync(std::function<void()> complete);

}  // namespace rumble

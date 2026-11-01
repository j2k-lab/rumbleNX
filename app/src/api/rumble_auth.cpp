#include "api/rumble_auth.hpp"

#include "api/rumble.hpp"
#include "api/rumble_parse.hpp"
#include "api/rumble_playlist.hpp"
#include "api/rumble_settings.hpp"
#include "utils/config.hpp"
#include "utils/thread.hpp"

#include <borealis.hpp>
#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace rumble {
namespace {

using json = nlohmann::json;
using Form = std::vector<std::pair<std::string, std::string>>;

constexpr const char* SERVICE_URL = "https://rumble.com/service.php?name=";

struct RawResponse {
    long status = 0;
    std::string body;
};

size_t appendBody(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* body = static_cast<std::string*>(userdata);
    body->append(ptr, size * nmemb);
    return size * nmemb;
}

/// One libcurl handle with its own cookie jar, so the cookies Rumble sets
/// during a sign-in carry from step to step.
class AuthClient {
public:
    AuthClient() {
        curl = curl_easy_init();
        if (!curl) throw std::runtime_error("Could not start a network request.");
        curl_easy_setopt(curl, CURLOPT_COOKIEFILE, "");
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 20000L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 15000L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
        curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
        curl_easy_setopt(curl, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, AppVersion::getUserAgent().c_str());
    }

    ~AuthClient() {
        if (curl) curl_easy_cleanup(curl);
    }

    AuthClient(const AuthClient&) = delete;
    AuthClient& operator=(const AuthClient&) = delete;

    RawResponse request(const std::string& url, const std::string* postBody) {
        RawResponse response;
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        if (postBody) {
            curl_easy_setopt(curl, CURLOPT_POST, 1L);
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, postBody->c_str());
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(postBody->size()));
        } else {
            curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
        }

        curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, "Accept: application/json, text/plain, */*");
        if (postBody) headers = curl_slist_append(headers, "Content-Type: application/x-www-form-urlencoded");
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, appendBody);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);

        const CURLcode result = curl_easy_perform(curl);
        if (result == CURLE_OK) {
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status);
        } else {
            response.status = -1;
            response.body = curl_easy_strerror(result);
        }
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, nullptr);
        curl_slist_free_all(headers);
        return response;
    }

    std::string cookie(const std::string& name) const {
        std::string value;
        forEachCookie([&](const std::string& line) {
            std::vector<std::string> columns;
            std::istringstream input(line);
            for (std::string column; std::getline(input, column, '\t');) columns.push_back(column);
            if (columns.size() >= 7 && columns[5] == name) value = columns[6];
        });
        return value;
    }

    std::string exportCookies() const {
        std::string result;
        forEachCookie([&](const std::string& line) { result += line + "\n"; });
        return result;
    }

    void importCookies(const std::string& cookies) {
        std::istringstream input(cookies);
        for (std::string line; std::getline(input, line);) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty()) curl_easy_setopt(curl, CURLOPT_COOKIELIST, line.c_str());
        }
    }

    void setSessionCookie(const std::string& token) {
        curl_easy_setopt(curl, CURLOPT_COOKIE, ("u_s=" + token).c_str());
    }

private:
    template <typename Fn>
    void forEachCookie(Fn fn) const {
        curl_slist* cookies = nullptr;
        if (curl_easy_getinfo(curl, CURLINFO_COOKIELIST, &cookies) != CURLE_OK || !cookies) return;
        for (curl_slist* item = cookies; item; item = item->next) {
            if (item->data) fn(std::string(item->data));
        }
        curl_slist_free_all(cookies);
    }

    CURL* curl = nullptr;
};

std::string formEncode(const Form& form) {
    std::string body;
    for (const auto& [key, value] : form) {
        if (!body.empty()) body += '&';
        body += percentEncode(key) + "=" + percentEncode(value);
    }
    return body;
}

std::string jsonText(const json& obj, const char* key) {
    if (!obj.is_object()) return {};
    const auto it = obj.find(key);
    if (it == obj.end()) return {};
    if (it->is_string()) return it->get<std::string>();
    if (it->is_number_unsigned()) return std::to_string(it->get<unsigned long long>());
    if (it->is_number_integer()) return std::to_string(it->get<long long>());
    return {};
}

bool jsonTruthy(const json& obj, const char* key) {
    if (!obj.is_object()) return false;
    const auto it = obj.find(key);
    if (it == obj.end() || it->is_null()) return false;
    if (it->is_boolean()) return it->get<bool>();
    if (it->is_number()) return it->get<double>() != 0.0;
    if (it->is_string()) return !it->get<std::string>().empty();
    return !it->empty();
}

/// Best readable message from a service.php reply.
std::string serviceMessage(const json& root) {
    if (!root.is_object()) return {};
    if (root.contains("errors")) {
        const json& errors = root["errors"];
        if (errors.is_array()) {
            for (const auto& error : errors) {
                if (error.is_string()) return error.get<std::string>();
                const std::string message = jsonText(error, "message");
                if (!message.empty()) return message;
            }
        } else if (errors.is_object()) {
            for (auto it = errors.begin(); it != errors.end(); ++it) {
                if (it->is_string()) return it->get<std::string>();
            }
        } else if (errors.is_string()) {
            return errors.get<std::string>();
        }
    }
    for (const json* scope : {&root, root.contains("data") ? &root["data"] : &root}) {
        for (const char* key : {"message", "error", "msg"}) {
            const std::string message = jsonText(*scope, key);
            if (!message.empty()) return message;
        }
    }
    return {};
}

struct ServiceReply {
    json root;
    json data;
};

ServiceReply callService(AuthClient& client, const std::string& name, const Form& form,
    const std::string& extraQuery = "", const std::string& fallbackError = "Rumble did not accept the sign-in.") {
    const std::string body = formEncode(form);
    const RawResponse response = client.request(SERVICE_URL + name + extraQuery, &body);
    if (response.status <= 0) throw std::runtime_error("Could not reach Rumble: " + response.body);

    const json root = json::parse(response.body, nullptr, false);
    if (response.status >= 400 || root.is_discarded() || !root.is_object()) {
        const std::string message = root.is_discarded() ? std::string() : serviceMessage(root);
        if (!message.empty()) throw std::runtime_error(parse::cleanText(message));
        if (response.status == 403 || response.status == 429)
            throw std::runtime_error("Rumble is not accepting sign-ins from this network right now (HTTP " +
                                     std::to_string(response.status) + "). Try again later.");
        throw std::runtime_error(fallbackError + " (HTTP " + std::to_string(response.status) + ")");
    }

    ServiceReply reply;
    reply.root = root;
    reply.data = root.contains("data") ? root["data"] : json();
    if (reply.data.is_object() && reply.data.contains("success") && reply.data["success"].is_boolean() &&
        !reply.data["success"].get<bool>()) {
        const std::string message = serviceMessage(root);
        throw std::runtime_error(message.empty() ? fallbackError : parse::cleanText(message));
    }
    return reply;
}

/// Profile picture from the account's public page, if it has one.
std::string fetchAvatar(const std::string& username) {
    if (username.empty() || username.find_first_of("@ /?#") != std::string::npos) return {};
    try {
        const std::string path = "user/" + username;
        const Channel channel = parse::parseChannelHeader(fetchPage("/" + path), path);
        return channel.avatarUrl;
    } catch (const std::exception& error) {
        brls::Logger::info("RumbleNX: no profile picture for {}: {}", username, error.what());
        return {};
    }
}

Session finishSignIn(AuthClient& client, const std::string& username, const std::string& fallbackToken) {
    Session session;
    session.token = client.cookie("u_s");
    if (session.token.empty()) session.token = fallbackToken;
    if (session.token.empty()) throw std::runtime_error("Rumble signed you in but returned no session. Try again.");
    session.username = username;
    session.avatarUrl = fetchAvatar(username);
    if (!saveSession(session)) throw std::runtime_error("Could not save the session to the SD card.");
    return session;
}

}  // namespace

Session loadSession() {
    const json root = settings::read(settings::SESSION);
    Session session;
    session.token = jsonText(root, "token");
    session.username = jsonText(root, "username");
    session.avatarUrl = jsonText(root, "avatar_url");
    return session;
}

bool saveSession(const Session& session) {
    return settings::write(settings::SESSION, json{
        {"token", session.token},
        {"username", session.username},
        {"avatar_url", session.avatarUrl},
    });
}

void clearSession() { settings::remove(settings::SESSION); }

void signInAsync(
    const std::string& username,
    const std::string& password,
    SignInCallback success,
    ErrorCallback failure) {
    ThreadPool::instance().submit([username, password, success = std::move(success),
                                      failure = std::move(failure)](HTTP&) mutable {
        try {
            AuthClient client;

            const ServiceReply salts = callService(client, "user.get_salts", {{"username", username}},
                "&response_type=session", "Rumble did not recognise that account.");
            std::vector<std::string> values;
            if (salts.data.is_object() && salts.data.contains("salts") && salts.data["salts"].is_array()) {
                for (const auto& salt : salts.data["salts"]) {
                    if (salt.is_string()) values.push_back(salt.get<std::string>());
                }
            }
            if (values.size() < 3)
                throw std::runtime_error("Rumble did not return sign-in details for that account. Check the username.");
            const std::string hashes = passwordHashes(password, values);

            const ServiceReply first = callService(client, "user.2fa.first_step",
                {{"legacy_password", "1"}, {"login", username}, {"password", hashes},
                    {"redirect_uri", SITE_URL}},
                "", "Rumble did not accept that username and password.");
            const std::string totpId = jsonText(first.data, "totp_id");
            if (!totpId.empty()) {
                SignInResult result;
                result.needsSecondFactor = true;
                result.challenge.username = username;
                result.challenge.totpId = totpId;
                result.challenge.userKey = jsonText(first.data, "user_key");
                const json& options = first.data.contains("options") ? first.data["options"] : json();
                result.challenge.authenticator = jsonTruthy(options, "authenticator");
                result.challenge.email = jsonTruthy(options, "email");
                result.challenge.phone = jsonTruthy(options, "phone");
                result.challenge.cookies = client.exportCookies();
                brls::sync(std::bind(std::move(success), std::move(result)));
                return;
            }

            const ServiceReply login = callService(client, "user.login",
                {{"username", username}, {"password_hashes", hashes}},
                "", "Rumble did not accept that username and password.");
            if (!jsonTruthy(login.data, "session"))
                throw std::runtime_error("Rumble did not accept that username and password.");

            SignInResult result;
            result.session = finishSignIn(client, username, jsonText(login.data, "session"));
            brls::sync(std::bind(std::move(success), std::move(result)));
        } catch (const std::exception& exception) {
            brls::sync(std::bind(std::move(failure), std::string(exception.what())));
        } catch (...) {
            brls::sync(std::bind(std::move(failure), std::string("Unknown Rumble sign-in error")));
        }
    });
}

void requestSecondFactorCodeAsync(
    const SecondFactor& challenge,
    bool email,
    std::function<void(std::string sentTo)> success,
    ErrorCallback failure) {
    ThreadPool::instance().submit([challenge, email, success = std::move(success),
                                      failure = std::move(failure)](HTTP&) mutable {
        try {
            AuthClient client;
            client.importCookies(challenge.cookies);
            const ServiceReply reply = callService(client,
                email ? "user.2fa.request_email_code" : "user.2fa.request_sms_code",
                {{"totp_id", challenge.totpId}, {"user_key", challenge.userKey}},
                "", "Rumble could not send a sign-in code.");
            brls::sync(std::bind(std::move(success), jsonText(reply.data, "sent_to")));
        } catch (const std::exception& exception) {
            brls::sync(std::bind(std::move(failure), std::string(exception.what())));
        }
    });
}

void verifySecondFactorAsync(
    const SecondFactor& challenge,
    const std::string& code,
    SessionCallback success,
    ErrorCallback failure) {
    ThreadPool::instance().submit([challenge, code, success = std::move(success),
                                      failure = std::move(failure)](HTTP&) mutable {
        try {
            AuthClient client;
            client.importCookies(challenge.cookies);
            callService(client, "user.2fa.verify_totp",
                {{"code", code}, {"redirect_url", SITE_URL}, {"totp_id", challenge.totpId},
                    {"user_key", challenge.userKey}},
                "", "Rumble did not accept that code.");
            Session session = finishSignIn(client, challenge.username, "");
            brls::sync(std::bind(std::move(success), std::move(session)));
        } catch (const std::exception& exception) {
            brls::sync(std::bind(std::move(failure), std::string(exception.what())));
        }
    });
}

void signOutAsync(std::function<void()> complete) {
    const Session session = loadSession();
    clearSession();
    ThreadPool::instance().submit([session, complete = std::move(complete)](HTTP&) mutable {
        if (session.signedIn()) {
            try {
                AuthClient client;
                client.setSessionCookie(session.token);
                client.request(std::string(SERVICE_URL) + "user.logout", nullptr);
            } catch (...) {
                // The local session is already gone; Rumble expires it on its own.
            }
        }
        if (complete) brls::sync(std::move(complete));
    });
}

}  // namespace rumble

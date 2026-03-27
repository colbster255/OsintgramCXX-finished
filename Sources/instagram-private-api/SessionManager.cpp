#include <IGApi/SessionManager.hpp>

#include <iostream>
#include <sstream>
#include <regex>
#include <ctime>
#include <algorithm>
#include <iomanip>
#include <chrono>
#include <cctype>
#include <dev_tools/network/URLParams.hpp>

namespace IG {

    const std::string SessionManager::API_BASE = "https://i.instagram.com/api/v1";
    const std::string SessionManager::WEB_BASE = "https://www.instagram.com";
    const std::string SessionManager::IG_APP_ID = "936619743392459";
    const std::string SessionManager::IG_SIG_KEY_VERSION = "4";

    SessionManager& SessionManager::Instance() {
        static SessionManager instance;
        return instance;
    }

    std::string SessionManager::BuildUserAgent() const {
        return "Instagram 275.0.0.27.98 Android (33/13; 440dpi; 1080x2400; "
               "Google/google; Pixel 7; panther; tensor; en_US; 458229258)";
    }

    Headers SessionManager::BuildCommonHeaders() const {
        Headers headers;
        headers.emplace_back("User-Agent", BuildUserAgent());
        headers.emplace_back("X-IG-App-ID", IG_APP_ID);
        headers.emplace_back("X-IG-App-Locale", "en_US");
        headers.emplace_back("X-IG-Device-Locale", "en_US");
        headers.emplace_back("X-IG-Connection-Type", "WIFI");
        headers.emplace_back("X-IG-Capabilities", "3brTvx0=");
        headers.emplace_back("Accept-Language", "en-US");
        headers.emplace_back("Content-Type", "application/x-www-form-urlencoded; charset=UTF-8");
        headers.emplace_back("Accept", "*/*");
        headers.emplace_back("X-IG-Connection-Speed", "1000kbps");
        headers.emplace_back("X-IG-Bandwidth-Speed-KBPS", "1000.000");
        headers.emplace_back("X-IG-Bandwidth-TotalBytes-B", "0");
        headers.emplace_back("X-IG-Bandwidth-TotalTime-MS", "0");
        return headers;
    }

    std::string SessionManager::ExtractCookieValue(const std::string& cookieHeader, const std::string& name) {
        std::string search = name + "=";
        size_t pos = cookieHeader.find(search);
        if (pos == std::string::npos) return "";

        pos += search.length();
        size_t end = cookieHeader.find(';', pos);
        if (end == std::string::npos)
            return cookieHeader.substr(pos);
        return cookieHeader.substr(pos, end - pos);
    }

    void SessionManager::ParseLoginCookies(const Headers& responseHeaders) {
        for (const auto& [key, value] : responseHeaders) {
            std::string lowerKey = key;
            std::transform(lowerKey.begin(), lowerKey.end(), lowerKey.begin(), ::tolower);
            if (lowerKey != "set-cookie") continue;

            std::string sessionId = ExtractCookieValue(value, "sessionid");
            if (!sessionId.empty()) _currentUser.sessionId = sessionId;

            std::string csrfToken = ExtractCookieValue(value, "csrftoken");
            if (!csrfToken.empty()) _currentUser.csrfToken = csrfToken;

            std::string mid = ExtractCookieValue(value, "mid");
            if (!mid.empty()) _currentUser.mid = mid;

            std::string dsUserId = ExtractCookieValue(value, "ds_user_id");
            if (!dsUserId.empty()) _currentUser.dsUserId = dsUserId;
        }
    }

    ResponseData SessionManager::MakeAuthenticatedRequest(const std::string& url,
                                                           RequestMethod method,
                                                           const std::string& body) {
        RequestData req;
        req.url = url;
        req.method = method;
        req.headers = BuildCommonHeaders();

        // Add auth cookies
        std::string cookies = "sessionid=" + _currentUser.sessionId +
                              "; csrftoken=" + _currentUser.csrfToken +
                              "; ds_user_id=" + _currentUser.dsUserId;
        if (!_currentUser.mid.empty())
            cookies += "; mid=" + _currentUser.mid;

        req.headers.emplace_back("Cookie", cookies);
        req.headers.emplace_back("X-CSRFToken", _currentUser.csrfToken);

        if (!body.empty())
            req.body = ByteData(body);

        req.connTimeoutMillis = 30000;
        req.readTimeoutMillis = 30000;
        req.followRedirects = true;
        req.verifySSL = true;

        return CreateRequest(req);
    }

    ResponseData SessionManager::MakePublicRequest(const std::string& url) {
        RequestData req;
        req.url = url;
        req.method = RequestMethod::REQ_GET;
        req.headers = BuildCommonHeaders();
        req.connTimeoutMillis = 30000;
        req.readTimeoutMillis = 30000;
        req.followRedirects = true;
        req.verifySSL = true;

        return CreateRequest(req);
    }

    bool SessionManager::Login(const std::string& username, const std::string& password) {
        std::lock_guard<std::mutex> lock(_mutex);

        _currentUser = UserSession{};
        _currentUser.username = username;
        _currentUser.userAgent = BuildUserAgent();

        // Step 1: Fetch CSRF token from accounts/login page
        {
            RequestData csrfReq;
            csrfReq.url = API_BASE + "/si/fetch_headers/?challenge_type=signup&guid=generate";
            csrfReq.method = RequestMethod::REQ_GET;
            csrfReq.headers = BuildCommonHeaders();
            csrfReq.connTimeoutMillis = 30000;
            csrfReq.readTimeoutMillis = 30000;
            csrfReq.followRedirects = true;
            csrfReq.verifySSL = true;

            ResponseData csrfResp = CreateRequest(csrfReq);
            ParseLoginCookies(csrfResp.headers);
        }

        // Step 2: Perform login
        const std::string encPassword =
            "#PWD_INSTAGRAM:0:" + std::to_string(std::time(nullptr)) + ":" + password;

        json payload = {
            {"phone_id", "a1b2c3d4-e5f6-7890-abcd-ef1234567890"},
            {"_csrftoken", _currentUser.csrfToken},
            {"username", username},
            {"guid", "a1b2c3d4-e5f6-7890-abcd-ef1234567890"},
            {"device_id", "android-a1b2c3d4e5f67890"},
            {"enc_password", encPassword},
            {"login_attempt_count", "0"}
        };

        std::string loginBody = EncodeURLParams({
            {"signed_body", "SIGNATURE." + payload.dump()},
            {"ig_sig_key_version", IG_SIG_KEY_VERSION}
        });

        RequestData loginReq;
        loginReq.url = API_BASE + "/accounts/login/";
        loginReq.method = RequestMethod::REQ_POST;
        loginReq.headers = BuildCommonHeaders();
        if (!_currentUser.csrfToken.empty()) {
            std::string loginCookies = "csrftoken=" + _currentUser.csrfToken;
            if (!_currentUser.mid.empty())
                loginCookies += "; mid=" + _currentUser.mid;

            loginReq.headers.emplace_back("Cookie", loginCookies);
            loginReq.headers.emplace_back("X-CSRFToken", _currentUser.csrfToken);
        }
        loginReq.body = ByteData(loginBody);
        loginReq.connTimeoutMillis = 30000;
        loginReq.readTimeoutMillis = 30000;
        loginReq.followRedirects = true;
        loginReq.verifySSL = true;

        ResponseData loginResp = CreateRequest(loginReq);

        // Parse cookies from response
        ParseLoginCookies(loginResp.headers);

        // Parse JSON response
        try {
            std::string respBody = std::get<ByteData>(loginResp.body);
            if (respBody.empty()) {
                std::cerr << "[!] Login failed: empty response body";
                if (!loginResp.errorData.empty()) {
                    std::cerr << " (" << loginResp.errorData << ")";
                }
                std::cerr << std::endl;
                return false;
            }

            auto firstNonWs = std::find_if_not(respBody.begin(), respBody.end(), [](unsigned char c) {
                return std::isspace(c) != 0;
            });
            if (firstNonWs == respBody.end() || (*firstNonWs != '{' && *firstNonWs != '['))
                throw json::parse_error::create(101, 1, "non-JSON login response", nullptr);

            json respJson = json::parse(respBody);

            if (respJson.contains("logged_in_user")) {
                auto& user = respJson["logged_in_user"];
                _currentUser.userId = std::to_string(user.value("pk", 0L));
                _currentUser.authenticated = true;

                // Build auth headers for future requests
                _currentUser.authHeaders.emplace_back("Cookie",
                    "sessionid=" + _currentUser.sessionId +
                    "; csrftoken=" + _currentUser.csrfToken +
                    "; ds_user_id=" + _currentUser.dsUserId);
                _currentUser.authHeaders.emplace_back("X-CSRFToken", _currentUser.csrfToken);

                return true;
            }

            if (respJson.contains("two_factor_required") && respJson["two_factor_required"].get<bool>()) {
                std::cerr << "[!] Two-factor authentication required." << std::endl;
                std::cerr << "[!] 2FA support is not yet fully implemented." << std::endl;
                std::cerr << "[!] Please disable 2FA temporarily or use an app password." << std::endl;
                return false;
            }

            if (respJson.contains("message")) {
                std::cerr << "[!] Login failed: " << respJson["message"].get<std::string>() << std::endl;
            }

            if (respJson.contains("error_type")) {
                std::string errorType = respJson["error_type"].get<std::string>();
                if (errorType == "bad_password") {
                    std::cerr << "[!] Incorrect password for user '" << username << "'" << std::endl;
                } else if (errorType == "invalid_user") {
                    std::cerr << "[!] User '" << username << "' not found" << std::endl;
                } else if (errorType == "checkpoint_challenge_required") {
                    std::cerr << "[!] Challenge required. Instagram needs identity verification." << std::endl;
                    std::cerr << "[!] Please log in via the Instagram app first to clear the challenge." << std::endl;
                }
            }

        } catch (const json::exception& e) {
            std::cerr << "[!] Failed to parse login response: " << e.what() << std::endl;
            std::string respBody = std::get<ByteData>(loginResp.body);
            if (!respBody.empty()) {
                std::string preview = respBody.substr(0, std::min<std::size_t>(respBody.size(), 200));
                std::replace(preview.begin(), preview.end(), '\n', ' ');
                std::replace(preview.begin(), preview.end(), '\r', ' ');
                std::cerr << "[!] Login HTTP status: " << loginResp.statusCode << std::endl;
                std::cerr << "[!] Login response preview: " << preview << std::endl;
            }

            // If we got a session ID from cookies, login may have succeeded
            if (!_currentUser.sessionId.empty() && !_currentUser.dsUserId.empty()) {
                _currentUser.userId = _currentUser.dsUserId;
                _currentUser.authenticated = true;
                return true;
            }
        }

        return false;
    }

    void SessionManager::Logout() {
        std::lock_guard<std::mutex> lock(_mutex);

        if (_currentUser.authenticated) {
            // Send logout request
            try {
                MakeAuthenticatedRequest(API_BASE + "/accounts/logout/", RequestMethod::REQ_POST);
            } catch (...) {
                // Best-effort logout
            }
        }

        _currentUser = UserSession{};
        _currentTarget = TargetInfo{};
        _hasTarget = false;
    }

    bool SessionManager::IsLoggedIn() const {
        std::lock_guard<std::mutex> lock(_mutex);
        return _currentUser.authenticated;
    }

    std::string SessionManager::GetCurrentUsername() const {
        std::lock_guard<std::mutex> lock(_mutex);
        return _currentUser.username;
    }

    UserSession SessionManager::GetCurrentSession() const {
        std::lock_guard<std::mutex> lock(_mutex);
        return _currentUser;
    }

    bool SessionManager::SetTarget(const std::string& username) {
        auto info = FetchUserInfo(username);
        if (!info.has_value()) return false;

        std::lock_guard<std::mutex> lock(_mutex);
        _currentTarget = info.value();
        _hasTarget = true;
        return true;
    }

    bool SessionManager::HasTarget() const {
        std::lock_guard<std::mutex> lock(_mutex);
        return _hasTarget;
    }

    TargetInfo SessionManager::GetTarget() const {
        std::lock_guard<std::mutex> lock(_mutex);
        return _currentTarget;
    }

    std::string SessionManager::GetTargetUsername() const {
        std::lock_guard<std::mutex> lock(_mutex);
        return _currentTarget.username;
    }

    std::optional<TargetInfo> SessionManager::FetchUserInfo(const std::string& username) {
        // First search for the user to get their ID
        std::string searchUrl = API_BASE + "/users/search/?q=" + username +
                                "&count=1&timezone_offset=0";

        ResponseData searchResp;
        if (_currentUser.authenticated)
            searchResp = MakeAuthenticatedRequest(searchUrl);
        else
            searchResp = MakePublicRequest(searchUrl);

        if (searchResp.statusCode != 200) {
            std::cerr << "[!] Failed to search for user '" << username
                      << "' (HTTP " << searchResp.statusCode << ")" << std::endl;
            return std::nullopt;
        }

        try {
            std::string body = std::get<ByteData>(searchResp.body);
            json searchJson = json::parse(body);

            if (!searchJson.contains("users") || searchJson["users"].empty()) {
                std::cerr << "[!] User '" << username << "' not found" << std::endl;
                return std::nullopt;
            }

            // Find exact username match
            json targetUser;
            bool found = false;
            for (const auto& u : searchJson["users"]) {
                if (u.value("username", "") == username) {
                    targetUser = u;
                    found = true;
                    break;
                }
            }

            if (!found) {
                // Use first result as fallback
                targetUser = searchJson["users"][0];
            }

            std::string userId = std::to_string(targetUser.value("pk", 0L));

            // Fetch detailed user info
            std::string infoUrl = API_BASE + "/users/" + userId + "/info/";
            ResponseData infoResp;
            if (_currentUser.authenticated)
                infoResp = MakeAuthenticatedRequest(infoUrl);
            else
                infoResp = MakePublicRequest(infoUrl);

            TargetInfo info;
            info.username = username;
            info.userId = userId;

            if (infoResp.statusCode == 200) {
                std::string infoBody = std::get<ByteData>(infoResp.body);
                json infoJson = json::parse(infoBody);

                if (infoJson.contains("user")) {
                    auto& user = infoJson["user"];
                    info.fullName = user.value("full_name", "");
                    info.biography = user.value("biography", "");
                    info.externalUrl = user.value("external_url", "");
                    info.profilePicUrl = user.value("profile_pic_url", "");
                    info.profilePicUrlHD = user.value("hd_profile_pic_url_info",
                                           json::object()).value("url", info.profilePicUrl);
                    info.followerCount = user.value("follower_count", 0);
                    info.followingCount = user.value("following_count", 0);
                    info.mediaCount = user.value("media_count", 0);
                    info.isPrivate = user.value("is_private", false);
                    info.isVerified = user.value("is_verified", false);
                    info.isBusiness = user.value("is_business", false);
                    info.category = user.value("category", "");

                    if (user.contains("public_email"))
                        info.email = user.value("public_email", "");
                    if (user.contains("public_phone_number"))
                        info.phoneNumber = user.value("public_phone_number", "");

                    // Try HD profile pic from nested structure
                    if (user.contains("hd_profile_pic_url_info")) {
                        auto& hdPic = user["hd_profile_pic_url_info"];
                        info.profilePicUrlHD = hdPic.value("url", info.profilePicUrl);
                    }
                }
            } else {
                // Use search result data as fallback
                info.fullName = targetUser.value("full_name", "");
                info.profilePicUrl = targetUser.value("profile_pic_url", "");
                info.isPrivate = targetUser.value("is_private", false);
                info.isVerified = targetUser.value("is_verified", false);
            }

            return info;

        } catch (const json::exception& e) {
            std::cerr << "[!] Failed to parse user info: " << e.what() << std::endl;
            return std::nullopt;
        }
    }

    std::vector<UserEntry> SessionManager::FetchFollowers(const std::string& userId, int maxCount) {
        std::vector<UserEntry> followers;
        std::string nextMaxId;
        int fetched = 0;

        while (fetched < maxCount) {
            std::string url = API_BASE + "/friendships/" + userId + "/followers/"
                              "?count=50&search_surface=follow_list_page";
            if (!nextMaxId.empty())
                url += "&max_id=" + nextMaxId;

            ResponseData resp = MakeAuthenticatedRequest(url);
            if (resp.statusCode != 200) break;

            try {
                std::string body = std::get<ByteData>(resp.body);
                json data = json::parse(body);

                if (!data.contains("users")) break;

                for (const auto& u : data["users"]) {
                    if (fetched >= maxCount) break;
                    UserEntry entry;
                    entry.username = u.value("username", "");
                    entry.userId = std::to_string(u.value("pk", 0L));
                    entry.fullName = u.value("full_name", "");
                    entry.profilePicUrl = u.value("profile_pic_url", "");
                    entry.isPrivate = u.value("is_private", false);
                    entry.isVerified = u.value("is_verified", false);
                    followers.push_back(entry);
                    fetched++;
                }

                if (data.contains("next_max_id") && !data["next_max_id"].is_null())
                    nextMaxId = data["next_max_id"].get<std::string>();
                else
                    break;

            } catch (const json::exception&) {
                break;
            }
        }

        return followers;
    }

    std::vector<UserEntry> SessionManager::FetchFollowing(const std::string& userId, int maxCount) {
        std::vector<UserEntry> following;
        std::string nextMaxId;
        int fetched = 0;

        while (fetched < maxCount) {
            std::string url = API_BASE + "/friendships/" + userId + "/following/"
                              "?count=50";
            if (!nextMaxId.empty())
                url += "&max_id=" + nextMaxId;

            ResponseData resp = MakeAuthenticatedRequest(url);
            if (resp.statusCode != 200) break;

            try {
                std::string body = std::get<ByteData>(resp.body);
                json data = json::parse(body);

                if (!data.contains("users")) break;

                for (const auto& u : data["users"]) {
                    if (fetched >= maxCount) break;
                    UserEntry entry;
                    entry.username = u.value("username", "");
                    entry.userId = std::to_string(u.value("pk", 0L));
                    entry.fullName = u.value("full_name", "");
                    entry.profilePicUrl = u.value("profile_pic_url", "");
                    entry.isPrivate = u.value("is_private", false);
                    entry.isVerified = u.value("is_verified", false);
                    following.push_back(entry);
                    fetched++;
                }

                if (data.contains("next_max_id") && !data["next_max_id"].is_null())
                    nextMaxId = data["next_max_id"].get<std::string>();
                else
                    break;

            } catch (const json::exception&) {
                break;
            }
        }

        return following;
    }

    std::vector<MediaItem> SessionManager::FetchUserFeed(const std::string& userId, int maxCount) {
        std::vector<MediaItem> items;
        std::string nextMaxId;
        int fetched = 0;

        while (fetched < maxCount) {
            std::string url = API_BASE + "/feed/user/" + userId + "/?count=12";
            if (!nextMaxId.empty())
                url += "&max_id=" + nextMaxId;

            ResponseData resp = MakeAuthenticatedRequest(url);
            if (resp.statusCode != 200) break;

            try {
                std::string body = std::get<ByteData>(resp.body);
                json data = json::parse(body);

                if (!data.contains("items")) break;

                for (const auto& item : data["items"]) {
                    if (fetched >= maxCount) break;
                    MediaItem media;
                    media.mediaId = std::to_string(item.value("pk", 0L));
                    media.shortcode = item.value("code", "");
                    media.likeCount = item.value("like_count", 0);
                    media.commentCount = item.value("comment_count", 0);
                    media.takenAt = std::to_string(item.value("taken_at", 0L));

                    // Caption
                    if (item.contains("caption") && !item["caption"].is_null()) {
                        media.caption = item["caption"].value("text", "");
                    }

                    // Media type
                    int mediaType = item.value("media_type", 1);
                    if (mediaType == 1) media.mediaType = "photo";
                    else if (mediaType == 2) media.mediaType = "video";
                    else if (mediaType == 8) media.mediaType = "carousel";

                    // Image URL
                    if (item.contains("image_versions2") &&
                        item["image_versions2"].contains("candidates") &&
                        !item["image_versions2"]["candidates"].empty()) {
                        media.imageUrl = item["image_versions2"]["candidates"][0].value("url", "");
                    }

                    // Video URL
                    if (item.contains("video_versions") && !item["video_versions"].empty()) {
                        media.videoUrl = item["video_versions"][0].value("url", "");
                    }

                    // Location
                    if (item.contains("location") && !item["location"].is_null()) {
                        media.location = item["location"].value("name", "");
                        media.locationAddress = item["location"].value("address", "");
                    }

                    // Tagged users
                    if (item.contains("usertags") && item["usertags"].contains("in")) {
                        for (const auto& tag : item["usertags"]["in"]) {
                            if (tag.contains("user")) {
                                media.taggedUsers.push_back(tag["user"].value("username", ""));
                            }
                        }
                    }

                    // Extract hashtags from caption
                    if (!media.caption.empty()) {
                        std::regex hashtagRegex("#(\\w+)");
                        auto begin = std::sregex_iterator(media.caption.begin(), media.caption.end(), hashtagRegex);
                        auto end = std::sregex_iterator();
                        for (auto it = begin; it != end; ++it) {
                            media.hashtags.push_back(it->str());
                        }
                    }

                    items.push_back(media);
                    fetched++;
                }

                if (data.contains("next_max_id") && !data["next_max_id"].is_null())
                    nextMaxId = std::to_string(data["next_max_id"].get<long long>());
                else if (data.value("more_available", false) == false)
                    break;
                else
                    break;

            } catch (const json::exception&) {
                break;
            }
        }

        return items;
    }

    std::vector<CommentInfo> SessionManager::FetchMediaComments(const std::string& mediaId, int maxCount) {
        std::vector<CommentInfo> comments;
        std::string minId;
        int fetched = 0;

        while (fetched < maxCount) {
            std::string url = API_BASE + "/media/" + mediaId + "/comments/?can_support_threading=true&count=50";
            if (!minId.empty())
                url += "&min_id=" + minId;

            ResponseData resp = MakeAuthenticatedRequest(url);
            if (resp.statusCode != 200) break;

            try {
                std::string body = std::get<ByteData>(resp.body);
                json data = json::parse(body);

                if (!data.contains("comments")) break;

                for (const auto& c : data["comments"]) {
                    if (fetched >= maxCount) break;
                    CommentInfo comment;
                    comment.commentId = std::to_string(c.value("pk", 0L));
                    comment.text = c.value("text", "");
                    comment.likeCount = c.value("comment_like_count", 0);
                    comment.createdAt = std::to_string(c.value("created_at", 0L));

                    if (c.contains("user")) {
                        comment.username = c["user"].value("username", "");
                        comment.userId = std::to_string(c["user"].value("pk", 0L));
                    }

                    comments.push_back(comment);
                    fetched++;
                }

                if (data.contains("next_min_id") && !data["next_min_id"].is_null())
                    minId = data["next_min_id"].get<std::string>();
                else
                    break;

            } catch (const json::exception&) {
                break;
            }
        }

        return comments;
    }

    std::vector<UserEntry> SessionManager::FetchMediaLikers(const std::string& mediaId, int maxCount) {
        std::vector<UserEntry> likers;

        std::string url = API_BASE + "/media/" + mediaId + "/likers/";
        ResponseData resp = MakeAuthenticatedRequest(url);
        if (resp.statusCode != 200) return likers;

        try {
            std::string body = std::get<ByteData>(resp.body);
            json data = json::parse(body);

            if (!data.contains("users")) return likers;

            int count = 0;
            for (const auto& u : data["users"]) {
                if (count >= maxCount) break;
                UserEntry entry;
                entry.username = u.value("username", "");
                entry.userId = std::to_string(u.value("pk", 0L));
                entry.fullName = u.value("full_name", "");
                entry.profilePicUrl = u.value("profile_pic_url", "");
                entry.isPrivate = u.value("is_private", false);
                entry.isVerified = u.value("is_verified", false);
                likers.push_back(entry);
                count++;
            }
        } catch (const json::exception&) {}

        return likers;
    }

    std::optional<std::string> SessionManager::FetchProfilePicHD(const std::string& userId) {
        std::string url = API_BASE + "/users/" + userId + "/info/";
        ResponseData resp;
        if (_currentUser.authenticated)
            resp = MakeAuthenticatedRequest(url);
        else
            resp = MakePublicRequest(url);

        if (resp.statusCode != 200) return std::nullopt;

        try {
            std::string body = std::get<ByteData>(resp.body);
            json data = json::parse(body);

            if (data.contains("user")) {
                auto& user = data["user"];
                if (user.contains("hd_profile_pic_url_info")) {
                    return user["hd_profile_pic_url_info"].value("url", "");
                }
                if (user.contains("hd_profile_pic_versions") &&
                    !user["hd_profile_pic_versions"].empty()) {
                    return user["hd_profile_pic_versions"].back().value("url", "");
                }
                return user.value("profile_pic_url", "");
            }
        } catch (const json::exception&) {}

        return std::nullopt;
    }

    std::vector<MediaItem> SessionManager::FetchStories(const std::string& userId) {
        std::vector<MediaItem> stories;

        std::string url = API_BASE + "/feed/user/" + userId + "/story/";
        ResponseData resp = MakeAuthenticatedRequest(url);
        if (resp.statusCode != 200) return stories;

        try {
            std::string body = std::get<ByteData>(resp.body);
            json data = json::parse(body);

            if (!data.contains("reel") || data["reel"].is_null()) return stories;

            auto& reel = data["reel"];
            if (!reel.contains("items")) return stories;

            for (const auto& item : reel["items"]) {
                MediaItem media;
                media.mediaId = std::to_string(item.value("pk", 0L));
                media.takenAt = std::to_string(item.value("taken_at", 0L));

                int mediaType = item.value("media_type", 1);
                media.mediaType = (mediaType == 2) ? "video" : "photo";

                if (item.contains("image_versions2") &&
                    item["image_versions2"].contains("candidates") &&
                    !item["image_versions2"]["candidates"].empty()) {
                    media.imageUrl = item["image_versions2"]["candidates"][0].value("url", "");
                }

                if (item.contains("video_versions") && !item["video_versions"].empty()) {
                    media.videoUrl = item["video_versions"][0].value("url", "");
                }

                stories.push_back(media);
            }
        } catch (const json::exception&) {}

        return stories;
    }

    std::vector<UserEntry> SessionManager::FetchTaggedUsers(const std::string& userId) {
        std::vector<UserEntry> tagged;

        // Fetch user's feed and collect all tagged users
        auto feed = FetchUserFeed(userId, 50);
        std::map<std::string, UserEntry> uniqueUsers;

        for (const auto& item : feed) {
            for (const auto& taggedUsername : item.taggedUsers) {
                if (uniqueUsers.find(taggedUsername) == uniqueUsers.end()) {
                    UserEntry entry;
                    entry.username = taggedUsername;
                    uniqueUsers[taggedUsername] = entry;
                }
            }
        }

        for (auto& [_, entry] : uniqueUsers) {
            tagged.push_back(entry);
        }

        return tagged;
    }

    std::vector<UserEntry> SessionManager::SearchUsers(const std::string& query, int maxCount) {
        std::vector<UserEntry> results;

        std::string url = API_BASE + "/users/search/?q=" + query +
                          "&count=" + std::to_string(maxCount) +
                          "&timezone_offset=0";

        ResponseData resp;
        if (_currentUser.authenticated)
            resp = MakeAuthenticatedRequest(url);
        else
            resp = MakePublicRequest(url);

        if (resp.statusCode != 200) return results;

        try {
            std::string body = std::get<ByteData>(resp.body);
            json data = json::parse(body);

            if (!data.contains("users")) return results;

            for (const auto& u : data["users"]) {
                UserEntry entry;
                entry.username = u.value("username", "");
                entry.userId = std::to_string(u.value("pk", 0L));
                entry.fullName = u.value("full_name", "");
                entry.profilePicUrl = u.value("profile_pic_url", "");
                entry.isPrivate = u.value("is_private", false);
                entry.isVerified = u.value("is_verified", false);
                results.push_back(entry);
            }
        } catch (const json::exception&) {}

        return results;
    }

}

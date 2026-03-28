#ifndef INSTAGRAM_SESSION_MANAGER_HPP
#define INSTAGRAM_SESSION_MANAGER_HPP

#include <string>
#include <vector>
#include <map>
#include <optional>
#include <mutex>
#include <nlohmann/json.hpp>
#include <dev_tools/network/Network.hpp>

using json = nlohmann::json;
using namespace DevTools;

namespace IG {

    struct UserSession {
        std::string username;
        std::string userId;
        std::string sessionId;
        std::string csrfToken;
        std::string mid;
        std::string dsUserId;
        std::string userAgent;
        bool authenticated = false;
        Headers authHeaders;
    };

    struct TargetInfo {
        std::string username;
        std::string userId;
        std::string fullName;
        std::string biography;
        std::string externalUrl;
        std::string profilePicUrl;
        std::string profilePicUrlHD;
        std::string email;
        std::string phoneNumber;
        std::string category;
        int followerCount = 0;
        int followingCount = 0;
        int mediaCount = 0;
        bool isPrivate = false;
        bool isVerified = false;
        bool isBusiness = false;
        bool isFollowing = false;  // whether logged-in user follows this target
    };

    struct MediaItem {
        std::string mediaId;
        std::string shortcode;
        std::string caption;
        std::string mediaType; // "photo", "video", "carousel"
        std::string imageUrl;
        std::string videoUrl;
        std::string location;
        std::string locationAddress;
        int likeCount = 0;
        int commentCount = 0;
        std::string takenAt;
        std::vector<std::string> taggedUsers;
        std::vector<std::string> hashtags;
    };

    struct CommentInfo {
        std::string commentId;
        std::string text;
        std::string username;
        std::string userId;
        int likeCount = 0;
        std::string createdAt;
    };

    struct UserEntry {
        std::string username;
        std::string userId;
        std::string fullName;
        std::string profilePicUrl;
        bool isPrivate = false;
        bool isVerified = false;
    };

    /**
     * Singleton session manager shared between core and interactive command libraries.
     * Manages Instagram authentication state and provides API request helpers.
     */
    class SessionManager {
    public:
        static SessionManager& Instance();

        // Authentication
        bool Login(const std::string& username, const std::string& password);
        bool TryResumeSession(const std::string& username);
        void Logout();
        bool IsLoggedIn() const;
        std::string GetCurrentUsername() const;
        UserSession GetCurrentSession() const;
        std::string GetLastSavedUsername() const;

        // Target management
        bool SetTarget(const std::string& username);
        bool HasTarget() const;
        TargetInfo GetTarget() const;
        std::string GetTargetUsername() const;
        std::optional<TargetInfo> GetTargetByName(const std::string& username) const;
        std::vector<TargetInfo> ListTargets() const;
        void ClearTarget();

        // Instagram API methods
        std::optional<TargetInfo> FetchUserInfo(const std::string& username);
        std::vector<UserEntry> FetchFollowers(const std::string& userId, int maxCount = 0);
        std::vector<UserEntry> FetchFollowing(const std::string& userId, int maxCount = 0);
        std::vector<MediaItem> FetchUserFeed(const std::string& userId, int maxCount = 0);
        std::vector<CommentInfo> FetchMediaComments(const std::string& mediaId, int maxCount = 0);
        std::vector<UserEntry> FetchMediaLikers(const std::string& mediaId, int maxCount = 0);
        std::optional<std::string> FetchProfilePicHD(const std::string& userId);
        std::vector<MediaItem> FetchStories(const std::string& userId);
        std::vector<UserEntry> FetchTaggedUsers(const std::string& userId);
        std::vector<MediaItem> FetchUsertagsFeed(const std::string& userId, int maxCount = 0);
        std::vector<UserEntry> SearchUsers(const std::string& query, int maxCount = 10);

        ~SessionManager();

    private:
        SessionManager() = default;
        SessionManager(const SessionManager&) = delete;
        SessionManager& operator=(const SessionManager&) = delete;

        // HTTP helpers
        ResponseData MakeAuthenticatedRequest(const std::string& url,
                                              RequestMethod method = RequestMethod::REQ_GET,
                                              const std::string& body = "");
        ResponseData MakePublicRequest(const std::string& url);
        Headers BuildCommonHeaders() const;
        std::string BuildUserAgent() const;

        // 2FA handler
        bool Handle2FA(const json& initialResp, const std::string& username,
                       const std::string& deviceId, const std::string& guid,
                       const std::string& csrfToken);

        // Cookie parsing
        void ParseLoginCookies(const Headers& responseHeaders);
        std::string ExtractCookieValue(const std::string& cookieHeader, const std::string& name);

        // Session persistence
        std::string GetSessionFilePath() const;
        void SaveSessionToFile();
        bool LoadSessionFromFile(const std::string& username);

        // State
        UserSession _currentUser;
        TargetInfo _currentTarget;
        bool _hasTarget = false;
        std::map<std::string, TargetInfo> _targets;  // all loaded targets by username
        mutable std::mutex _mutex;

        // Encryption keys from Instagram
        std::string _encPubKey;
        int _encKeyId = 0;

        static const std::string API_BASE;
        static const std::string WEB_BASE;
        static const std::string IG_APP_ID;
    };

}

#endif //INSTAGRAM_SESSION_MANAGER_HPP

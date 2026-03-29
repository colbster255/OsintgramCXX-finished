#include <IGApi/SessionManager.hpp>

#include <iostream>
#include <sstream>
#include <regex>
#include <ctime>
#include <algorithm>
#include <iomanip>
#include <chrono>
#include <thread>
#include <fstream>
#include <filesystem>
#include <climits>
#include <random>

// OpenSSL for password encryption
#include <openssl/rsa.h>
#include <openssl/pem.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/bio.h>
#include <openssl/buffer.h>
#include <openssl/err.h>

namespace IG {

    // Forward-declared helper (used in Login, LoadSession, FetchUserInfo, etc.)
    static std::string GetResponseBody(const ResponseData& resp) {
        try { return std::get<ByteData>(resp.body); } catch (...) { return ""; }
    }

    static void RateLimitDelay() {
        static std::chrono::steady_clock::time_point lastRequest;
        static std::mt19937 rng(std::random_device{}());
        static int recentRequestCount = 0;

        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastRequest).count();

        // Track burst frequency — reset if idle for >30s
        if (elapsed > 30000) {
            recentRequestCount = 0;
        } else {
            recentRequestCount++;
        }

        int delayMs = 0;

        // Every 15-25 requests, take a longer "human break" (5-12 seconds)
        if (recentRequestCount > 0) {
            std::uniform_int_distribution<int> burstThreshold(15, 25);
            if (recentRequestCount % burstThreshold(rng) == 0) {
                std::uniform_int_distribution<int> longPause(5000, 12000);
                delayMs = longPause(rng);
            }
        }

        if (delayMs == 0) {
            if (elapsed < 1500) {
                // Back-to-back requests — enforce minimum gap with jitter
                std::uniform_int_distribution<int> dist(1500, 4000);
                delayMs = dist(rng);
            } else if (elapsed < 5000) {
                // Recent request — add moderate jitter
                std::uniform_int_distribution<int> dist(500, 2000);
                delayMs = dist(rng);
            }
            // If >5s since last request, no delay needed
        }

        if (delayMs > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
        }

        lastRequest = std::chrono::steady_clock::now();
    }

    const std::string SessionManager::API_BASE = "https://www.instagram.com/api/v1";
    const std::string SessionManager::WEB_BASE  = "https://www.instagram.com";
    const std::string SessionManager::IG_APP_ID = "936619743392459";

    // -------------------------------------------------------------------------
    // Singleton
    // -------------------------------------------------------------------------
    SessionManager& SessionManager::Instance() {
        static SessionManager instance;
        return instance;
    }

    // -------------------------------------------------------------------------
    // Destructor – keep session alive (no logout on exit)
    // -------------------------------------------------------------------------
    SessionManager::~SessionManager() {
        // Session stays valid — will be reused next launch
    }

    // -------------------------------------------------------------------------
    // Device / headers
    // -------------------------------------------------------------------------
    std::string SessionManager::BuildUserAgent() const {
        return "Instagram 275.0.0.27.98 Android (33/13; 440dpi; 1080x2400; "
               "Google/google; Pixel 7; panther; tensor; en_US; 458229258)";
    }

    Headers SessionManager::BuildCommonHeaders() const {
        Headers h;
        h.emplace_back("User-Agent",                  BuildUserAgent());
        h.emplace_back("X-IG-App-ID",                 IG_APP_ID);
        h.emplace_back("X-IG-App-Locale",             "en_US");
        h.emplace_back("X-IG-Device-Locale",          "en_US");
        h.emplace_back("X-IG-Connection-Type",        "WIFI");
        h.emplace_back("X-IG-Capabilities",           "3brTvx0=");
        h.emplace_back("Accept-Language",             "en-US");
        h.emplace_back("Content-Type",                "application/x-www-form-urlencoded; charset=UTF-8");
        h.emplace_back("Accept",                      "*/*");
        h.emplace_back("X-IG-Connection-Speed",       "1000kbps");
        h.emplace_back("X-IG-Bandwidth-Speed-KBPS",  "1000.000");
        h.emplace_back("X-IG-Bandwidth-TotalBytes-B", "0");
        h.emplace_back("X-IG-Bandwidth-TotalTime-MS", "0");
        return h;
    }

    // -------------------------------------------------------------------------
    // Cookie helpers
    // -------------------------------------------------------------------------
    std::string SessionManager::ExtractCookieValue(const std::string& hdr, const std::string& name) {
        std::string search = name + "=";
        size_t pos = hdr.find(search);
        if (pos == std::string::npos) return "";
        pos += search.size();
        size_t end = hdr.find(';', pos);
        return (end == std::string::npos) ? hdr.substr(pos) : hdr.substr(pos, end - pos);
    }

    void SessionManager::ParseLoginCookies(const Headers& respHeaders) {
        for (const auto& [key, value] : respHeaders) {
            std::string lk = key;
            std::transform(lk.begin(), lk.end(), lk.begin(), ::tolower);
            if (lk != "set-cookie") continue;

            auto try_set = [&](const std::string& n, std::string& dst) {
                std::string v = ExtractCookieValue(value, n);
                if (!v.empty()) dst = v;
            };
            try_set("sessionid",  _currentUser.sessionId);
            try_set("csrftoken",  _currentUser.csrfToken);
            try_set("mid",        _currentUser.mid);
            try_set("ds_user_id", _currentUser.dsUserId);
        }
    }

    // -------------------------------------------------------------------------
    // HTTP helpers
    // -------------------------------------------------------------------------
    ResponseData SessionManager::MakeAuthenticatedRequest(const std::string& url,
                                                          RequestMethod method,
                                                          const std::string& body) {
        RequestData req;
        req.url     = url;
        req.method  = method;

        // Use the same header style as the login flow
        req.headers.emplace_back("User-Agent",       _currentUser.userAgent);
        req.headers.emplace_back("Accept",            "*/*");
        req.headers.emplace_back("Accept-Language",   "en-US,en;q=0.9");
        req.headers.emplace_back("Accept-Encoding",   "identity");
        req.headers.emplace_back("X-IG-App-ID",      IG_APP_ID);
        req.headers.emplace_back("X-IG-WWW-Claim",   "0");
        req.headers.emplace_back("X-Requested-With",  "XMLHttpRequest");
        req.headers.emplace_back("Sec-Fetch-Site",    "same-origin");
        req.headers.emplace_back("Sec-Fetch-Mode",    "cors");
        req.headers.emplace_back("Sec-Fetch-Dest",    "empty");
        req.headers.emplace_back("Referer",           "https://www.instagram.com/");
        req.headers.emplace_back("Origin",            "https://www.instagram.com");

        std::string cookies = "sessionid=" + _currentUser.sessionId
                            + "; csrftoken=" + _currentUser.csrfToken
                            + "; ds_user_id=" + _currentUser.dsUserId;
        if (!_currentUser.mid.empty())
            cookies += "; mid=" + _currentUser.mid;

        req.headers.emplace_back("Cookie",      cookies);
        req.headers.emplace_back("X-CSRFToken", _currentUser.csrfToken);
        if (!body.empty()) {
            req.headers.emplace_back("Content-Type",
                "application/x-www-form-urlencoded");
            req.body = ByteData(body);
        }

        req.connTimeoutMillis = 30000;
        req.readTimeoutMillis = 30000;
        req.followRedirects   = true;
        req.verifySSL         = true;
        RateLimitDelay();
        return CreateRequest(req);
    }

    ResponseData SessionManager::MakePublicRequest(const std::string& url) {
        RequestData req;
        req.url             = url;
        req.method          = RequestMethod::REQ_GET;
        req.headers         = BuildCommonHeaders();
        req.connTimeoutMillis = 30000;
        req.readTimeoutMillis = 30000;
        req.followRedirects = true;
        req.verifySSL       = true;
        RateLimitDelay();
        return CreateRequest(req);
    }

    // -------------------------------------------------------------------------
    // Password encryption  (Instagram enc_password format)
    // Returns "#PWD_INSTAGRAM:4:<timestamp>:<base64>"
    // The pubKey and keyId are taken from Instagram's fetch_headers response.
    // -------------------------------------------------------------------------
    static std::string base64Encode(const unsigned char* data, size_t len) {
        BIO* b64 = BIO_new(BIO_f_base64());
        BIO* mem = BIO_new(BIO_s_mem());
        b64 = BIO_push(b64, mem);
        BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
        BIO_write(b64, data, static_cast<int>(len));
        BIO_flush(b64);
        BUF_MEM* bptr = nullptr;
        BIO_get_mem_ptr(b64, &bptr);
        std::string out(bptr->data, bptr->length);
        BIO_free_all(b64);
        return out;
    }

    static std::string encryptPasswordForIG(const std::string& password,
                                             const std::string& pubKeyB64,
                                             int keyId) {
        // Decode the public key (it comes as base64 from Instagram)
        std::string decodedKey;
        {
            BIO* b64 = BIO_new(BIO_f_base64());
            BIO* mem = BIO_new_mem_buf(pubKeyB64.data(), static_cast<int>(pubKeyB64.size()));
            b64 = BIO_push(b64, mem);
            BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
            decodedKey.resize(pubKeyB64.size());
            int len = BIO_read(b64, decodedKey.data(), static_cast<int>(decodedKey.size()));
            decodedKey.resize(std::max(len, 0));
            BIO_free_all(b64);
        }

        // Load public key
        BIO* bio = BIO_new_mem_buf(decodedKey.data(), static_cast<int>(decodedKey.size()));
        EVP_PKEY* pubKey = PEM_read_bio_PUBKEY(bio, nullptr, nullptr, nullptr);
        BIO_free(bio);
        if (!pubKey) throw std::runtime_error("Failed to load Instagram public key");

        // Generate random AES-256 key and 12-byte IV
        unsigned char aesKey[32], iv[12];
        if (!RAND_bytes(aesKey, sizeof(aesKey)) || !RAND_bytes(iv, sizeof(iv))) {
            EVP_PKEY_free(pubKey);
            throw std::runtime_error("Failed to generate random bytes");
        }

        // RSA-encrypt the AES key
        EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new(pubKey, nullptr);
        EVP_PKEY_free(pubKey);
        if (!ctx) throw std::runtime_error("EVP_PKEY_CTX_new failed");

        if (EVP_PKEY_encrypt_init(ctx) <= 0 ||
            EVP_PKEY_CTX_set_rsa_padding(ctx, RSA_PKCS1_PADDING) <= 0) {
            EVP_PKEY_CTX_free(ctx);
            throw std::runtime_error("RSA init failed");
        }

        size_t rsaLen = 0;
        EVP_PKEY_encrypt(ctx, nullptr, &rsaLen, aesKey, sizeof(aesKey));
        std::vector<unsigned char> rsaEncrypted(rsaLen);
        if (EVP_PKEY_encrypt(ctx, rsaEncrypted.data(), &rsaLen, aesKey, sizeof(aesKey)) <= 0) {
            EVP_PKEY_CTX_free(ctx);
            throw std::runtime_error("RSA encryption failed");
        }
        rsaEncrypted.resize(rsaLen);
        EVP_PKEY_CTX_free(ctx);

        // AES-256-GCM encrypt the password using timestamp as AAD
        std::string timestamp = std::to_string(std::time(nullptr));
        EVP_CIPHER_CTX* aesCtx = EVP_CIPHER_CTX_new();
        std::vector<unsigned char> ciphertext(password.size() + 16);
        unsigned char tag[16];
        int len = 0, totalLen = 0;

        if (!EVP_EncryptInit_ex(aesCtx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) ||
            !EVP_CIPHER_CTX_ctrl(aesCtx, EVP_CTRL_GCM_SET_IVLEN, sizeof(iv), nullptr) ||
            !EVP_EncryptInit_ex(aesCtx, nullptr, nullptr, aesKey, iv) ||
            !EVP_EncryptUpdate(aesCtx, nullptr, &len,
                               reinterpret_cast<const unsigned char*>(timestamp.c_str()),
                               static_cast<int>(timestamp.size())) ||
            !EVP_EncryptUpdate(aesCtx, ciphertext.data(), &len,
                               reinterpret_cast<const unsigned char*>(password.c_str()),
                               static_cast<int>(password.size()))) {
            EVP_CIPHER_CTX_free(aesCtx);
            throw std::runtime_error("AES-GCM encryption failed");
        }
        totalLen = len;
        EVP_EncryptFinal_ex(aesCtx, ciphertext.data() + len, &len);
        totalLen += len;
        ciphertext.resize(totalLen);
        EVP_CIPHER_CTX_ctrl(aesCtx, EVP_CTRL_GCM_GET_TAG, sizeof(tag), tag);
        EVP_CIPHER_CTX_free(aesCtx);

        // Build final payload: [1][keyId][iv(12)][rsaLen_lo][rsaLen_hi][rsaEncrypted][tag(16)][ciphertext]
        std::vector<unsigned char> payload;
        payload.push_back(1);
        payload.push_back(static_cast<unsigned char>(keyId));
        payload.insert(payload.end(), iv, iv + sizeof(iv));
        payload.push_back(static_cast<unsigned char>(rsaLen & 0xFF));
        payload.push_back(static_cast<unsigned char>((rsaLen >> 8) & 0xFF));
        payload.insert(payload.end(), rsaEncrypted.begin(), rsaEncrypted.end());
        payload.insert(payload.end(), tag, tag + sizeof(tag));
        payload.insert(payload.end(), ciphertext.begin(), ciphertext.end());

        std::string b64 = base64Encode(payload.data(), payload.size());
        return "#PWD_INSTAGRAM:4:" + timestamp + ":" + b64;
    }

    // -------------------------------------------------------------------------
    // URL-encode a single value
    // -------------------------------------------------------------------------
    static std::string urlEncode(const std::string& s) {
        std::ostringstream out;
        for (unsigned char c : s) {
            if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
                out << c;
            else
                out << '%' << std::uppercase << std::setw(2) << std::setfill('0') << std::hex << (int)c;
        }
        return out.str();
    }

    // -------------------------------------------------------------------------
    // Build a UUID-like device identifier from a seed string
    // -------------------------------------------------------------------------
    static std::string makeDeviceId(const std::string& seed) {
        // Simple deterministic "device id" based on username
        std::hash<std::string> hasher;
        size_t h = hasher(seed + "osintgramcxx");
        std::ostringstream oss;
        oss << "android-" << std::hex << std::setw(16) << std::setfill('0') << h;
        return oss.str().substr(0, 24);
    }

    static std::string makeGuid(const std::string& seed) {
        std::hash<std::string> hasher;
        size_t h1 = hasher(seed + "guid1");
        size_t h2 = hasher(seed + "guid2");
        std::ostringstream oss;
        oss << std::hex << std::setfill('0')
            << std::setw(8) << (h1 & 0xFFFFFFFF) << "-"
            << std::setw(4) << ((h1 >> 32) & 0xFFFF) << "-"
            << std::setw(4) << ((h2 & 0x0FFF) | 0x4000) << "-"
            << std::setw(4) << ((h2 >> 16 & 0x3FFF) | 0x8000) << "-"
            << std::setw(12) << (h2 & 0xFFFFFFFFFFFF);
        return oss.str();
    }

    // -------------------------------------------------------------------------
    // Web login helpers
    // -------------------------------------------------------------------------
    static const std::string WEB_USER_AGENT =
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
        "(KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36";
    static const std::string WEB_IG_APP_ID = "936619743392459";

    static Headers BuildWebHeaders() {
        Headers h;
        h.emplace_back("User-Agent",       WEB_USER_AGENT);
        h.emplace_back("Accept",           "*/*");
        h.emplace_back("Accept-Language",  "en-US,en;q=0.9");
        h.emplace_back("X-IG-App-ID",     WEB_IG_APP_ID);
        h.emplace_back("X-Requested-With","XMLHttpRequest");
        h.emplace_back("Sec-Fetch-Site",   "same-origin");
        h.emplace_back("Sec-Fetch-Mode",   "cors");
        h.emplace_back("Sec-Fetch-Dest",   "empty");
        h.emplace_back("Referer",          "https://www.instagram.com/accounts/login/");
        h.emplace_back("Origin",           "https://www.instagram.com");
        return h;
    }

    // -------------------------------------------------------------------------
    // Login  (web flow via www.instagram.com)
    // -------------------------------------------------------------------------
    bool SessionManager::TryResumeSession(const std::string& username) {
        std::lock_guard<std::mutex> lock(_mutex);
        return LoadSessionFromFile(username);
    }

    std::string SessionManager::GetLastSavedUsername() const {
        namespace fs = std::filesystem;
        std::string homeDir;
#ifdef _WIN32
        const char* userProfile = std::getenv("USERPROFILE");
        homeDir = userProfile ? userProfile : "";
        std::string dir = homeDir + "\\OsintgramCXX\\sessions";
#else
        const char* home = std::getenv("HOME");
        homeDir = home ? home : "";
        std::string dir = homeDir + "/.config/OsintgramCXX/sessions";
#endif
        if (homeDir.empty() || !fs::exists(dir)) return "";

        // Find the most recently modified .session file
        std::string latest;
        std::filesystem::file_time_type latestTime{};
        try {
            for (const auto& entry : fs::directory_iterator(dir)) {
                if (entry.path().extension() == ".session") {
                    if (latest.empty() || entry.last_write_time() > latestTime) {
                        latest = entry.path().stem().string();
                        latestTime = entry.last_write_time();
                    }
                }
            }
        } catch (...) {}
        return latest;
    }

    bool SessionManager::Login(const std::string& username, const std::string& password) {
        std::lock_guard<std::mutex> lock(_mutex);

        _currentUser         = UserSession{};
        _currentUser.username  = username;
        _currentUser.userAgent = WEB_USER_AGENT;

        std::string deviceId = makeDeviceId(username);
        std::string guid     = makeGuid(username);

        // ------------------------------------------------------------------
        // Step 1: GET the login page to grab the csrftoken cookie
        // ------------------------------------------------------------------
        {
            RequestData req;
            req.url     = WEB_BASE + "/accounts/login/";
            req.method  = RequestMethod::REQ_GET;
            req.headers = BuildWebHeaders();
            req.connTimeoutMillis = 15000;
            req.readTimeoutMillis = 15000;
            req.followRedirects   = true;
            req.verifySSL         = true;

            ResponseData resp = CreateRequest(req);
            ParseLoginCookies(resp.headers);

            // Also look for encryption keys in response headers
            std::string encPubKeyHeader, encKeyIdHeader;
            for (const auto& [k, v] : resp.headers) {
                std::string lk = k;
                std::transform(lk.begin(), lk.end(), lk.begin(), ::tolower);
                if (lk == "ig-set-password-encryption-pub-key")  _encPubKey = v;
                if (lk == "ig-set-password-encryption-key-id") {
                    try { _encKeyId = std::stoi(v); } catch (...) {}
                }
            }

            // Try to extract csrftoken from HTML if not in cookie
            if (_currentUser.csrfToken.empty()) {
                std::string body;
                try { body = std::get<ByteData>(resp.body); } catch (...) {}
                std::regex csrfRx("\"csrf_token\"\\s*:\\s*\"([^\"]+)\"");
                std::smatch m;
                if (std::regex_search(body, m, csrfRx))
                    _currentUser.csrfToken = m[1].str();
            }
        }

        if (_currentUser.csrfToken.empty()) {
            std::cerr << "[!] Failed to obtain CSRF token from Instagram." << std::endl;
            return false;
        }

        // ------------------------------------------------------------------
        // Step 2: Build enc_password
        // ------------------------------------------------------------------
        std::string encPassword;
        std::string timestamp = std::to_string(std::time(nullptr));

        if (!_encPubKey.empty() && _encKeyId > 0) {
            try {
                encPassword = encryptPasswordForIG(password, _encPubKey, _encKeyId);
            } catch (const std::exception& e) {
                std::cerr << "[!] Password encryption failed (" << e.what()
                          << "), using timestamp format." << std::endl;
                encPassword = "#PWD_INSTAGRAM_BROWSER:0:" + timestamp + ":" + password;
            }
        } else {
            // Web browser format – unencrypted but properly tagged
            encPassword = "#PWD_INSTAGRAM_BROWSER:0:" + timestamp + ":" + password;
        }

        // ------------------------------------------------------------------
        // Step 3: POST ajax/login
        // ------------------------------------------------------------------
        std::string body =
            "username="           + urlEncode(username)    +
            "&enc_password="      + urlEncode(encPassword) +
            "&queryParams=%7B%7D"                          +
            "&optIntoOneTap=false"                         +
            "&stopDeletionNonce="                          +
            "&trustedDeviceRecords=%7B%7D";

        RequestData loginReq;
        loginReq.url     = WEB_BASE + "/api/v1/web/accounts/login/ajax/";
        loginReq.method  = RequestMethod::REQ_POST;
        loginReq.headers = BuildWebHeaders();
        loginReq.headers.emplace_back("Content-Type",
            "application/x-www-form-urlencoded");
        loginReq.headers.emplace_back("X-CSRFToken", _currentUser.csrfToken);

        // Send cookies we already have
        std::string cookies = "csrftoken=" + _currentUser.csrfToken;
        if (!_currentUser.mid.empty()) cookies += "; mid=" + _currentUser.mid;
        loginReq.headers.emplace_back("Cookie", cookies);

        loginReq.body              = ByteData(body);
        loginReq.connTimeoutMillis = 30000;
        loginReq.readTimeoutMillis = 30000;
        loginReq.followRedirects   = true;
        loginReq.verifySSL         = true;

        ResponseData loginResp = CreateRequest(loginReq);
        ParseLoginCookies(loginResp.headers);

        // ------------------------------------------------------------------
        // Step 4: parse response
        // ------------------------------------------------------------------
        std::string rawBody;
        try { rawBody = std::get<ByteData>(loginResp.body); } catch (...) {}

        if (loginResp.statusCode == 0 || !loginResp.errorData.empty()) {
            std::cerr << "[!] Network error: " << loginResp.errorData << std::endl;
            return false;
        }

        json respJson;
        try {
            respJson = json::parse(rawBody);
        } catch (...) {
            std::cerr << "[!] Non-JSON response (HTTP " << loginResp.statusCode << ")." << std::endl;
            return false;
        }

        // Check authenticated (web returns {"authenticated": true, "userId": "..."})
        if (respJson.value("authenticated", false)) {
            _currentUser.userId        = respJson.value("userId", _currentUser.dsUserId);
            _currentUser.authenticated = true;
            // Update CSRF token if it changed
            ParseLoginCookies(loginResp.headers);

            // Quick verify — test an authenticated endpoint
            ResponseData verifyResp = MakeAuthenticatedRequest(API_BASE + "/accounts/current_user/?edit=true");
            std::string verifyBody = GetResponseBody(verifyResp);
            if (verifyResp.statusCode == 200 && !verifyBody.empty()) {
                std::cerr << "[+] Session fully authenticated." << std::endl;
            } else if (verifyResp.statusCode == 429) {
                std::cerr << "[*] Verify skipped (rate limited), session assumed valid." << std::endl;
            } else {
                std::cerr << "[!] Warning: verify returned HTTP " << verifyResp.statusCode
                          << " — session may have limited access." << std::endl;
            }

            SaveSessionToFile();
            return true;
        }

        // 2FA required  (web returns {"two_factor_required": true, ...})
        if (respJson.value("two_factor_required", false)) {
            return Handle2FA(respJson, username, deviceId, guid, _currentUser.csrfToken);
        }

        // Error messages
        std::string msg     = respJson.value("message", "");
        std::string errType = respJson.value("error_type", "");
        bool showedError = false;

        if (respJson.value("checkpoint_url", "") != "") {
            std::cerr << "[!] Instagram requires a checkpoint challenge." << std::endl;
            std::cerr << "[!] Open https://www.instagram.com" << respJson["checkpoint_url"].get<std::string>()
                      << " in your browser to verify, then retry." << std::endl;
            showedError = true;
        }

        if (!showedError) {
            if (errType == "bad_password" || msg.find("password") != std::string::npos)
                std::cerr << "[!] Incorrect password." << std::endl;
            else if (errType == "invalid_user")
                std::cerr << "[!] User '" << username << "' not found." << std::endl;
            else if (!msg.empty())
                std::cerr << "[!] Login failed: " << msg << std::endl;
            else if (respJson.value("status", "") == "fail")
                std::cerr << "[!] Login failed (Instagram rejected the request)." << std::endl;
            else
                std::cerr << "[!] Login failed (HTTP " << loginResp.statusCode << ")." << std::endl;
        }

        return false;
    }

    // -------------------------------------------------------------------------
    // 2FA handler  (works with both web and mobile API responses)
    // -------------------------------------------------------------------------
    bool SessionManager::Handle2FA(const json& initialResp,
                                   const std::string& username,
                                   const std::string& deviceId,
                                   const std::string& guid,
                                   const std::string& csrfToken) {
        std::string identifier;
        int verificationMethod = 1; // default SMS

        if (initialResp.contains("two_factor_info")) {
            const auto& info = initialResp["two_factor_info"];
            identifier       = info.value("two_factor_identifier", "");
            if (info.value("totp_two_factor_on", false))
                verificationMethod = 3;  // TOTP (authenticator app)
        }

        std::cout << std::endl;
        std::cout << "[*] Two-factor authentication required." << std::endl;
        std::cout << "[*] Method: "
                  << (verificationMethod == 3 ? "Authenticator app (TOTP)" : "SMS")
                  << std::endl;
        std::cout << "[*] Enter your 2FA code: ";
        std::cout.flush();

        std::string code;
        std::getline(std::cin, code);

        // Strip spaces/dashes
        code.erase(std::remove_if(code.begin(), code.end(),
                   [](char c){ return c == ' ' || c == '-'; }), code.end());

        if (code.empty()) {
            std::cerr << "[!] No code entered, login cancelled." << std::endl;
            return false;
        }

        // Build cookies string
        std::string cookies = "csrftoken=" + csrfToken;
        if (!_currentUser.mid.empty()) cookies += "; mid=" + _currentUser.mid;

        // Try web 2FA endpoint first
        {
            std::string body =
                "username="               + urlEncode(username)                        +
                "&verificationCode="      + urlEncode(code)                            +
                "&identifier="            + urlEncode(identifier)                      +
                "&queryParams=%7B%22next%22%3A%22%2F%22%7D"                            +
                "&trust_this_device=1"                                                 +
                "&verification_method="   + std::to_string(verificationMethod);

            RequestData req;
            req.url     = WEB_BASE + "/api/v1/web/accounts/login/ajax/two_factor/";
            req.method  = RequestMethod::REQ_POST;
            req.headers = BuildWebHeaders();
            req.headers.emplace_back("Content-Type",
                "application/x-www-form-urlencoded");
            req.headers.emplace_back("X-CSRFToken", csrfToken);
            req.headers.emplace_back("Cookie", cookies);
            req.body              = ByteData(body);
            req.connTimeoutMillis = 30000;
            req.readTimeoutMillis = 30000;
            req.followRedirects   = true;
            req.verifySSL         = true;

            ResponseData resp = CreateRequest(req);
            ParseLoginCookies(resp.headers);

            std::string rawBody;
            try { rawBody = std::get<ByteData>(resp.body); } catch (...) {}

            json respJson;
            try { respJson = json::parse(rawBody); } catch (...) {
                std::cerr << "[!] Non-JSON response from 2FA endpoint." << std::endl;
                return false;
            }

            // Web returns {"authenticated": true, "userId": "..."}
            if (respJson.value("authenticated", false)) {
                _currentUser.userId        = respJson.value("userId", _currentUser.dsUserId);
                _currentUser.authenticated = true;
                std::cout << "[+] 2FA verified successfully." << std::endl;
                SaveSessionToFile();
                return true;
            }

            // Mobile API format
            if (respJson.contains("logged_in_user")) {
                auto& u = respJson["logged_in_user"];
                _currentUser.userId = std::to_string(u.value("pk", 0LL));
                _currentUser.authenticated = true;
                std::cout << "[+] 2FA verified successfully." << std::endl;
                SaveSessionToFile();
                return true;
            }

            std::string errMsg = respJson.value("message", "");
            if (errMsg.find("check the code") != std::string::npos ||
                errMsg.find("invalid") != std::string::npos)
                std::cerr << "[!] Invalid 2FA code. Please try again." << std::endl;
            else if (!errMsg.empty())
                std::cerr << "[!] 2FA failed: " << errMsg << std::endl;
            else
                std::cerr << "[!] 2FA verification failed." << std::endl;

            return false;
        }
    }

    // -------------------------------------------------------------------------
    // Logout
    // -------------------------------------------------------------------------
    void SessionManager::Logout() {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_currentUser.authenticated) {
            try {
                MakeAuthenticatedRequest(WEB_BASE + "/accounts/logout/ajax/",
                                         RequestMethod::REQ_POST,
                                         "one_click_logout=&user_id=" + _currentUser.dsUserId);
            } catch (...) {}
        }
        // Delete saved session file
        try {
            std::string path = GetSessionFilePath();
            if (!path.empty() && std::filesystem::exists(path))
                std::filesystem::remove(path);
        } catch (...) {}
        _currentUser  = UserSession{};
        _currentTarget = TargetInfo{};
        _hasTarget    = false;
    }

    // -------------------------------------------------------------------------
    // Session persistence — save/load cookies to avoid repeated logins
    // -------------------------------------------------------------------------
    std::string SessionManager::GetSessionFilePath() const {
        namespace fs = std::filesystem;
        std::string homeDir;
#ifdef _WIN32
        const char* userProfile = std::getenv("USERPROFILE");
        homeDir = userProfile ? userProfile : "C:\\Users\\Default";
        std::string dir = homeDir + "\\OsintgramCXX\\sessions";
#else
        const char* home = std::getenv("HOME");
        homeDir = home ? home : "/tmp";
        std::string dir = homeDir + "/.config/OsintgramCXX/sessions";
#endif
        try { fs::create_directories(dir); } catch (...) {}
        return dir + "/" + _currentUser.username + ".session";
    }

    void SessionManager::SaveSessionToFile() {
        std::string path = GetSessionFilePath();
        if (path.empty()) return;
        try {
            json sess;
            sess["username"]  = _currentUser.username;
            sess["userId"]    = _currentUser.userId;
            sess["sessionId"] = _currentUser.sessionId;
            sess["csrfToken"] = _currentUser.csrfToken;
            sess["mid"]       = _currentUser.mid;
            sess["dsUserId"]  = _currentUser.dsUserId;
            sess["userAgent"] = _currentUser.userAgent;
            sess["savedAt"]   = std::time(nullptr);
            std::ofstream out(path);
            out << sess.dump(2);
            std::cerr << "[+] Session saved to " << path << std::endl;
        } catch (const std::exception& e) {
            std::cerr << "[!] Failed to save session: " << e.what() << std::endl;
        }
    }

    bool SessionManager::LoadSessionFromFile(const std::string& username) {
        namespace fs = std::filesystem;
        // Build path manually since _currentUser may not be set yet
        std::string homeDir;
#ifdef _WIN32
        const char* userProfile = std::getenv("USERPROFILE");
        homeDir = userProfile ? userProfile : "C:\\Users\\Default";
        std::string path = homeDir + "\\OsintgramCXX\\sessions\\" + username + ".session";
#else
        const char* home = std::getenv("HOME");
        homeDir = home ? home : "/tmp";
        std::string path = homeDir + "/.config/OsintgramCXX/sessions/" + username + ".session";
#endif
        if (!fs::exists(path)) return false;

        try {
            std::ifstream in(path);
            std::string content((std::istreambuf_iterator<char>(in)),
                                 std::istreambuf_iterator<char>());
            json sess = json::parse(content);

            // Check if session is not too old (max 7 days)
            long long savedAt = sess.value("savedAt", 0LL);
            long long now = std::time(nullptr);
            if (now - savedAt > 7 * 24 * 3600) {
                std::cerr << "[*] Saved session expired, need fresh login." << std::endl;
                fs::remove(path);
                return false;
            }

            _currentUser.username  = sess.value("username",  "");
            _currentUser.userId    = sess.value("userId",    "");
            _currentUser.sessionId = sess.value("sessionId", "");
            _currentUser.csrfToken = sess.value("csrfToken", "");
            _currentUser.mid       = sess.value("mid",       "");
            _currentUser.dsUserId  = sess.value("dsUserId",  "");
            _currentUser.userAgent = sess.value("userAgent", WEB_USER_AGENT);
            _currentUser.authenticated = true;

            // Quick check if session is still valid — but don't invalidate on rate limits
            ResponseData resp = MakeAuthenticatedRequest(API_BASE + "/accounts/current_user/?edit=true");
            if (resp.statusCode == 401 || resp.statusCode == 403) {
                // Definitively unauthorized — session is dead
                _currentUser = UserSession{};
                fs::remove(path);
                std::cerr << "[*] Saved session expired (HTTP " << resp.statusCode << "), need fresh login." << std::endl;
                return false;
            }

            // For any other status (200, 429, etc.), trust the saved session
            return true;
        } catch (...) {
            return false;
        }
    }

    // -------------------------------------------------------------------------
    // Session state accessors
    // -------------------------------------------------------------------------
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

    // -------------------------------------------------------------------------
    // Target management
    // -------------------------------------------------------------------------
    bool SessionManager::SetTarget(const std::string& username) {
        // Check if already loaded
        {
            std::lock_guard<std::mutex> lock(_mutex);
            auto it = _targets.find(username);
            if (it != _targets.end()) {
                _currentTarget = it->second;
                _hasTarget     = true;
                return true;
            }
        }
        // Fetch fresh
        auto info = FetchUserInfo(username);
        if (!info.has_value()) return false;
        std::lock_guard<std::mutex> lock(_mutex);
        _targets[username] = info.value();
        _currentTarget = info.value();
        _hasTarget     = true;
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

    std::optional<TargetInfo> SessionManager::GetTargetByName(const std::string& username) const {
        std::lock_guard<std::mutex> lock(_mutex);
        auto it = _targets.find(username);
        if (it != _targets.end()) return it->second;
        return std::nullopt;
    }

    std::vector<TargetInfo> SessionManager::ListTargets() const {
        std::lock_guard<std::mutex> lock(_mutex);
        std::vector<TargetInfo> result;
        for (const auto& [_, t] : _targets) result.push_back(t);
        return result;
    }

    void SessionManager::ClearTarget() {
        std::lock_guard<std::mutex> lock(_mutex);
        _currentTarget = TargetInfo{};
        _hasTarget = false;
    }

    // -------------------------------------------------------------------------
    // Fetch user info  (web endpoints)
    // -------------------------------------------------------------------------

    std::optional<TargetInfo> SessionManager::FetchUserInfo(const std::string& username) {
        std::string body;
        int status = 0;

        // Attempt 1: web_profile_info endpoint (primary, what Instagram web uses)
        {
            std::string url = WEB_BASE + "/api/v1/users/web_profile_info/?username=" + urlEncode(username);
            ResponseData resp = MakeAuthenticatedRequest(url);
            status = resp.statusCode;
            body = GetResponseBody(resp);

            // Retry with increasing backoff on 429 (rate limit)
            for (int wait : {5, 15, 30}) {
                if (status != 429) break;
                std::cerr << "[*] Rate limited, waiting " << wait << " seconds..." << std::endl;
                std::this_thread::sleep_for(std::chrono::seconds(wait));
                resp = MakeAuthenticatedRequest(url);
                status = resp.statusCode;
                body = GetResponseBody(resp);
            }

            if (!body.empty() && body[0] == '{') {
                std::cerr << "[+] web_profile_info: HTTP " << status
                          << ", body=" << body.size() << "B" << std::endl;
            } else {
                body.clear();
            }
        }

        // Attempt 2: /?__a=1&__d=dis public JSON endpoint
        if (body.empty()) {
            std::string url = WEB_BASE + "/" + username + "/?__a=1&__d=dis";
            ResponseData resp = MakeAuthenticatedRequest(url);
            status = resp.statusCode;
            std::string respBody = GetResponseBody(resp);

            if (!respBody.empty() && respBody[0] == '{') {
                try {
                    json test = json::parse(respBody);
                    // Only accept if it has actual user data
                    bool hasUser = (test.contains("graphql") && test["graphql"].contains("user")) ||
                                   (test.contains("data") && !test["data"].is_null()) ||
                                   test.contains("user");
                    if (hasUser) body = respBody;
                } catch (...) {}
            }
        }

        // Attempt 3: Search for user via /web/search/topsearch/ to get user ID,
        // then fetch full profile via /users/{id}/info/
        if (body.empty()) {
            std::string searchUrl = WEB_BASE + "/web/search/topsearch/?query=" + urlEncode(username)
                                  + "&context=user&include_reel=false";
            ResponseData resp = MakeAuthenticatedRequest(searchUrl);
            status = resp.statusCode;
            std::string searchResp = GetResponseBody(resp);

            std::string discoveredUserId;
            if (!searchResp.empty() && searchResp[0] == '{') {
                try {
                    json searchData = json::parse(searchResp);
                    if (searchData.contains("users")) {
                        for (const auto& entry : searchData["users"]) {
                            json u = entry.contains("user") ? entry["user"] : entry;
                            std::string foundUsername = u.value("username", "");
                            if (foundUsername == username) {
                                if (u.contains("pk"))
                                    discoveredUserId = u["pk"].is_string()
                                        ? u["pk"].get<std::string>()
                                        : std::to_string(u.value("pk", 0LL));
                                else if (u.contains("id"))
                                    discoveredUserId = u["id"].is_string()
                                        ? u["id"].get<std::string>()
                                        : std::to_string(u.value("id", 0LL));
                                break;
                            }
                        }
                    }
                } catch (...) {}
            }

            // If we found the user ID, fetch full profile
            if (!discoveredUserId.empty() && discoveredUserId != "0") {
                std::cerr << "[+] Found user ID via search: " << discoveredUserId << std::endl;

                std::string infoUrl = API_BASE + "/users/" + discoveredUserId + "/info/";
                ResponseData infoResp = MakeAuthenticatedRequest(infoUrl);
                status = infoResp.statusCode;
                std::string infoBody = GetResponseBody(infoResp);

                if (!infoBody.empty() && infoBody[0] == '{') {
                    try {
                        json info = json::parse(infoBody);
                        if (info.contains("user") && info["user"].is_object() &&
                            !info["user"].empty()) {
                            body = infoBody;
                        }
                    } catch (...) {}
                }
            }
        }

        if (body.empty()) {
            std::cerr << "[!] Failed to fetch profile for '" << username
                      << "' — all endpoints exhausted. Try again in a few minutes (rate limit)." << std::endl;
            return std::nullopt;
        }

        try {
            json data = json::parse(body);

            // web_profile_info returns { "data": { "user": { ... } } }
            // public JSON returns { "graphql": { "user": { ... } } }
            json userJson;
            if (data.contains("data") && data["data"].contains("user"))
                userJson = data["data"]["user"];
            else if (data.contains("graphql") && data["graphql"].contains("user"))
                userJson = data["graphql"]["user"];
            else if (data.contains("user"))
                userJson = data["user"];
            else {
                std::cerr << "[!] User '" << username << "' not found." << std::endl;
                return std::nullopt;
            }

            TargetInfo info;
            info.username       = userJson.value("username", username);
            info.fullName       = userJson.value("full_name", "");
            info.biography      = userJson.value("biography", "");
            info.externalUrl    = userJson.value("external_url", "");
            info.profilePicUrl  = userJson.value("profile_pic_url", "");
            info.isPrivate      = userJson.value("is_private", false);
            info.isVerified     = userJson.value("is_verified", false);
            info.isBusiness     = userJson.value("is_business_account", false);
            info.category       = userJson.value("category_name", "");

            // User ID can be in different fields
            if (userJson.contains("id"))
                info.userId = userJson["id"].is_string()
                    ? userJson["id"].get<std::string>()
                    : std::to_string(userJson.value("id", 0LL));
            else if (userJson.contains("pk"))
                info.userId = std::to_string(userJson.value("pk", 0LL));

            // Counts - web format uses edge_* or direct count fields
            if (userJson.contains("edge_followed_by"))
                info.followerCount = userJson["edge_followed_by"].value("count", 0);
            else
                info.followerCount = userJson.value("follower_count", 0);

            if (userJson.contains("edge_follow"))
                info.followingCount = userJson["edge_follow"].value("count", 0);
            else
                info.followingCount = userJson.value("following_count", 0);

            if (userJson.contains("edge_owner_to_timeline_media"))
                info.mediaCount = userJson["edge_owner_to_timeline_media"].value("count", 0);
            else
                info.mediaCount = userJson.value("media_count", 0);

            // Email/phone (business accounts only)
            info.email       = userJson.value("public_email", "");
            info.phoneNumber = userJson.value("public_phone_number", "");
            if (info.email.empty())
                info.email = userJson.value("business_email", "");

            // HD profile pic
            if (userJson.contains("profile_pic_url_hd"))
                info.profilePicUrlHD = userJson.value("profile_pic_url_hd", info.profilePicUrl);
            else if (userJson.contains("hd_profile_pic_url_info"))
                info.profilePicUrlHD = userJson["hd_profile_pic_url_info"].value("url", info.profilePicUrl);
            else
                info.profilePicUrlHD = info.profilePicUrl;

            // Friendship status (from /users/{id}/info/ response)
            if (userJson.contains("friendship_status")) {
                info.isFollowing = userJson["friendship_status"].value("following", false);
            } else {
                // Some responses put it at different levels — search for it
                // Also check the raw parsed data object
                if (data.contains("friendship_status")) {
                    info.isFollowing = data["friendship_status"].value("following", false);
                } else {
                    // Try a friendship API call to check directly
                    if (!info.userId.empty()) {
                        std::string fUrl = API_BASE + "/friendships/show/" + info.userId + "/";
                        ResponseData fResp = MakeAuthenticatedRequest(fUrl);
                        std::string fBody = GetResponseBody(fResp);
                        if (!fBody.empty()) {
                            try {
                                json fData = json::parse(fBody);
                                info.isFollowing = fData.value("following", false);
                            } catch (...) {}
                        }
                    }
                }
            }

            // Validate we got at least a user ID
            if (info.userId.empty() || info.userId == "0") {
                std::cerr << "[!] Profile data for '" << username << "' has no user ID, rejecting." << std::endl;
                return std::nullopt;
            }

            return info;

        } catch (const json::exception& e) {
            std::cerr << "[!] Failed to parse user info: " << e.what() << std::endl;
            return std::nullopt;
        }
    }

    // -------------------------------------------------------------------------
    // Followers / Following
    // -------------------------------------------------------------------------
    // Helper: extract pk/id as string regardless of whether JSON stores it as int or string
    static std::string ExtractPk(const json& obj) {
        if (obj.contains("pk")) {
            const auto& v = obj["pk"];
            if (v.is_string()) return v.get<std::string>();
            if (v.is_number()) return std::to_string(v.get<long long>());
        }
        if (obj.contains("id")) {
            const auto& v = obj["id"];
            if (v.is_string()) return v.get<std::string>();
            if (v.is_number()) return std::to_string(v.get<long long>());
        }
        return "";
    }

    // Helper: extract next_max_id regardless of type
    static std::string ExtractNextMaxId(const json& data) {
        if (!data.contains("next_max_id") || data["next_max_id"].is_null()) return "";
        const auto& v = data["next_max_id"];
        if (v.is_string()) return v.get<std::string>();
        if (v.is_number()) return std::to_string(v.get<long long>());
        return "";
    }

    std::vector<UserEntry> SessionManager::FetchFollowers(const std::string& userId, int maxCount) {
        if (maxCount <= 0) maxCount = INT_MAX;
        std::vector<UserEntry> results;
        std::string nextMaxId;
        int fetched = 0;
        while (fetched < maxCount) {
            std::string url = API_BASE + "/friendships/" + userId + "/followers/?count=50&search_surface=follow_list_page";
            if (!nextMaxId.empty()) url += "&max_id=" + nextMaxId;
            ResponseData resp = MakeAuthenticatedRequest(url);
            std::string body = GetResponseBody(resp);
            if (resp.statusCode < 200 || resp.statusCode >= 300) break;
            try {
                json data = json::parse(body);
                if (!data.contains("users") || !data["users"].is_array()) {
                    break;
                }
                for (const auto& u : data["users"]) {
                    if (fetched >= maxCount) break;
                    UserEntry e;
                    e.username      = u.value("username",       "");
                    e.userId        = ExtractPk(u);
                    e.fullName      = u.value("full_name",       "");
                    e.profilePicUrl = u.value("profile_pic_url","");
                    e.isPrivate     = u.value("is_private",      false);
                    e.isVerified    = u.value("is_verified",     false);
                    results.push_back(e);
                    fetched++;
                }
                nextMaxId = ExtractNextMaxId(data);
                if (nextMaxId.empty()) break;
            } catch (const std::exception& ex) {
                break;
            }
        }
        return results;
    }

    std::vector<UserEntry> SessionManager::FetchFollowing(const std::string& userId, int maxCount) {
        if (maxCount <= 0) maxCount = INT_MAX;
        std::vector<UserEntry> results;
        std::string nextMaxId;
        int fetched = 0;
        while (fetched < maxCount) {
            std::string url = API_BASE + "/friendships/" + userId + "/following/?count=50";
            if (!nextMaxId.empty()) url += "&max_id=" + nextMaxId;
            ResponseData resp = MakeAuthenticatedRequest(url);
            std::string body = GetResponseBody(resp);
            if (resp.statusCode < 200 || resp.statusCode >= 300) break;
            try {
                json data = json::parse(body);
                if (!data.contains("users") || !data["users"].is_array()) {
                    break;
                }
                for (const auto& u : data["users"]) {
                    if (fetched >= maxCount) break;
                    UserEntry e;
                    e.username      = u.value("username",       "");
                    e.userId        = ExtractPk(u);
                    e.fullName      = u.value("full_name",       "");
                    e.profilePicUrl = u.value("profile_pic_url","");
                    e.isPrivate     = u.value("is_private",      false);
                    e.isVerified    = u.value("is_verified",     false);
                    results.push_back(e);
                    fetched++;
                }
                nextMaxId = ExtractNextMaxId(data);
                if (nextMaxId.empty()) break;
            } catch (const std::exception& ex) {
                break;
            }
        }
        return results;
    }

    // -------------------------------------------------------------------------
    // Feed
    // -------------------------------------------------------------------------
    // Helper: parse a single media item from the v1 API format
    // -------------------------------------------------------------------------
    static MediaItem ParseV1MediaItem(const json& item) {
        MediaItem m;
        m.mediaId      = ExtractPk(item);
        m.shortcode    = item.value("code", "");
        m.likeCount    = item.value("like_count",    0);
        m.commentCount = item.value("comment_count", 0);
        if (item.contains("taken_at")) {
            const auto& ta = item["taken_at"];
            m.takenAt = ta.is_string() ? ta.get<std::string>() : std::to_string(ta.get<long long>());
        }
        if (item.contains("caption") && !item["caption"].is_null())
            m.caption = item["caption"].value("text", "");
        int mt = item.value("media_type", 1);
        m.mediaType = (mt == 2) ? "video" : (mt == 8) ? "carousel" : "photo";
        if (item.contains("image_versions2") &&
            item["image_versions2"].contains("candidates") &&
            !item["image_versions2"]["candidates"].empty())
            m.imageUrl = item["image_versions2"]["candidates"][0].value("url","");
        if (item.contains("video_versions") && !item["video_versions"].empty())
            m.videoUrl = item["video_versions"][0].value("url","");
        if (item.contains("location") && !item["location"].is_null()) {
            m.location        = item["location"].value("name","");
            m.locationAddress = item["location"].value("address","");
        }
        if (item.contains("usertags") && item["usertags"].contains("in"))
            for (const auto& tag : item["usertags"]["in"])
                if (tag.contains("user"))
                    m.taggedUsers.push_back(tag["user"].value("username",""));
        if (!m.caption.empty()) {
            std::regex hashRx("#(\\w+)");
            auto beg = std::sregex_iterator(m.caption.begin(), m.caption.end(), hashRx);
            for (auto it = beg; it != std::sregex_iterator(); ++it)
                m.hashtags.push_back(it->str());
        }
        return m;
    }

    // -------------------------------------------------------------------------
    // Helper: parse a single media node from the GraphQL (edge) format
    // -------------------------------------------------------------------------
    static MediaItem ParseGraphQLMediaNode(const json& node) {
        MediaItem m;
        m.mediaId   = node.value("id", "");
        m.shortcode = node.value("shortcode", "");
        if (node.contains("edge_liked_by"))
            m.likeCount = node["edge_liked_by"].value("count", 0);
        else if (node.contains("edge_media_preview_like"))
            m.likeCount = node["edge_media_preview_like"].value("count", 0);
        if (node.contains("edge_media_to_comment"))
            m.commentCount = node["edge_media_to_comment"].value("count", 0);
        if (node.contains("taken_at_timestamp")) {
            const auto& ta = node["taken_at_timestamp"];
            m.takenAt = ta.is_string() ? ta.get<std::string>() : std::to_string(ta.get<long long>());
        }
        if (node.contains("edge_media_to_caption") &&
            node["edge_media_to_caption"].contains("edges") &&
            !node["edge_media_to_caption"]["edges"].empty())
            m.caption = node["edge_media_to_caption"]["edges"][0]["node"].value("text", "");
        bool isVideo = node.value("is_video", false);
        std::string typeName = node.value("__typename", "");
        if (typeName == "GraphSidecar" || typeName == "XDTGraphSidecar") m.mediaType = "carousel";
        else if (isVideo) m.mediaType = "video";
        else m.mediaType = "photo";
        m.imageUrl = node.value("display_url", "");
        if (isVideo) m.videoUrl = node.value("video_url", "");
        if (node.contains("location") && !node["location"].is_null())
            m.location = node["location"].value("name", "");
        if (node.contains("edge_media_to_tagged_user") &&
            node["edge_media_to_tagged_user"].contains("edges"))
            for (const auto& e : node["edge_media_to_tagged_user"]["edges"])
                if (e.contains("node") && e["node"].contains("user"))
                    m.taggedUsers.push_back(e["node"]["user"].value("username",""));
        if (!m.caption.empty()) {
            std::regex hashRx("#(\\w+)");
            auto beg = std::sregex_iterator(m.caption.begin(), m.caption.end(), hashRx);
            for (auto it = beg; it != std::sregex_iterator(); ++it)
                m.hashtags.push_back(it->str());
        }
        return m;
    }

    // -------------------------------------------------------------------------
    // Feed — multi-strategy: v1 API → graphql → web profile scrape
    // -------------------------------------------------------------------------
    std::vector<MediaItem> SessionManager::FetchUserFeed(const std::string& userId, int maxCount) {
        if (maxCount <= 0) maxCount = INT_MAX;
        std::vector<MediaItem> items;
        std::string nextMaxId;
        int fetched = 0;

        // --- Strategy 1: v1 /feed/user/{id}/ (works for mobile sessions) ---
        while (fetched < maxCount) {
            std::string url = API_BASE + "/feed/user/" + userId + "/?count=12";
            if (!nextMaxId.empty()) url += "&max_id=" + nextMaxId;
            ResponseData resp = MakeAuthenticatedRequest(url);
            std::string body = GetResponseBody(resp);
            if (resp.statusCode < 200 || resp.statusCode >= 300) break;
            try {
                json data = json::parse(body);
                if (!data.contains("items") || !data["items"].is_array() || data["items"].empty())
                    break; // not in v1 format or empty — try next strategy
                for (const auto& item : data["items"]) {
                    if (fetched >= maxCount) break;
                    items.push_back(ParseV1MediaItem(item));
                    fetched++;
                }
                if (!data.value("more_available", false)) break;
                nextMaxId = ExtractNextMaxId(data);
                if (nextMaxId.empty()) break;
            } catch (...) { break; }
        }
        if (!items.empty()) return items;

        // --- Strategy 2: GraphQL query (works for web sessions) ---
        // Try multiple query hashes — Instagram rotates these
        std::vector<std::string> queryHashes = {
            "e769aa130647d2354c40ea6a439bfc08",
            "003056d32c2554def87228bc3fd9668a",
            "69cba40317214236af40e7efa697781d",
            "42323d64886122307be10013ad2dcc44"
        };
        std::string endCursor;
        for (const auto& hash : queryHashes) {
            if (fetched > 0) break; // one hash worked, stop trying others
            endCursor.clear();
            while (fetched < maxCount) {
                json vars;
                vars["id"] = userId;
                vars["first"] = std::min(12, maxCount - fetched);
                if (!endCursor.empty()) vars["after"] = endCursor;

                std::string url = WEB_BASE + "/graphql/query/?query_hash=" + hash
                                + "&variables=" + urlEncode(vars.dump());
                ResponseData resp = MakeAuthenticatedRequest(url);
                std::string body = GetResponseBody(resp);
                if (resp.statusCode == 400 || resp.statusCode == 404) break; // bad hash, try next
                if (resp.statusCode < 200 || resp.statusCode >= 300) break;
                try {
                    json data = json::parse(body);
                    // Navigate to edge_owner_to_timeline_media
                    json* media = nullptr;
                    if (data.contains("data") && data["data"].contains("user"))
                        media = &data["data"]["user"]["edge_owner_to_timeline_media"];
                    else if (data.contains("user"))
                        media = &data["user"]["edge_owner_to_timeline_media"];
                    if (!media || !media->contains("edges")) break;

                    for (const auto& edge : (*media)["edges"]) {
                        if (fetched >= maxCount) break;
                        if (!edge.contains("node")) continue;
                        items.push_back(ParseGraphQLMediaNode(edge["node"]));
                        fetched++;
                    }
                    if (!(*media).contains("page_info") ||
                        !(*media)["page_info"].value("has_next_page", false))
                        break;
                    endCursor = (*media)["page_info"].value("end_cursor", "");
                    if (endCursor.empty()) break;
                } catch (...) { break; }
            }
        }
        if (!items.empty()) return items;

        // --- Strategy 3: /{username}/?__a=1&__d=dis scrape ---
        // Need the username — look it up from the target info
        {
            std::string username;
            if (_hasTarget && _currentTarget.userId == userId)
                username = _currentTarget.username;
            if (!username.empty()) {
                std::string url = WEB_BASE + "/" + urlEncode(username) + "/?__a=1&__d=dis";
                ResponseData resp = MakeAuthenticatedRequest(url);
                std::string body = GetResponseBody(resp);
                if (resp.statusCode >= 200 && resp.statusCode < 300 && !body.empty()) {
                    try {
                        json data = json::parse(body);
                        json* media = nullptr;
                        if (data.contains("graphql") && data["graphql"].contains("user"))
                            media = &data["graphql"]["user"]["edge_owner_to_timeline_media"];
                        else if (data.contains("user"))
                            media = &data["user"]["edge_owner_to_timeline_media"];
                        if (media && media->contains("edges")) {
                            for (const auto& edge : (*media)["edges"]) {
                                if (fetched >= maxCount) break;
                                if (!edge.contains("node")) continue;
                                items.push_back(ParseGraphQLMediaNode(edge["node"]));
                                fetched++;
                            }
                        }
                    } catch (...) {}
                }
            }
        }
        return items;
    }

    // -------------------------------------------------------------------------
    // Comments / Likers / Stories / Tags / Search
    // -------------------------------------------------------------------------
    std::vector<CommentInfo> SessionManager::FetchMediaComments(const std::string& mediaId, int maxCount) {
        if (maxCount <= 0) maxCount = INT_MAX;
        std::vector<CommentInfo> results;
        std::string minId;
        int fetched = 0;
        while (fetched < maxCount) {
            std::string url = API_BASE + "/media/" + mediaId + "/comments/?can_support_threading=true&count=50";
            if (!minId.empty()) url += "&min_id=" + minId;
            ResponseData resp = MakeAuthenticatedRequest(url);
            std::string body = GetResponseBody(resp);
            if (resp.statusCode < 200 || resp.statusCode >= 300) break;
            try {
                json data = json::parse(body);
                if (!data.contains("comments") || !data["comments"].is_array()) break;
                for (const auto& c : data["comments"]) {
                    if (fetched >= maxCount) break;
                    CommentInfo ci;
                    ci.commentId = ExtractPk(c);
                    ci.text      = c.value("text",               "");
                    ci.likeCount = c.value("comment_like_count", 0);
                    if (c.contains("created_at")) {
                        const auto& ca = c["created_at"];
                        ci.createdAt = ca.is_string() ? ca.get<std::string>() : std::to_string(ca.get<long long>());
                    }
                    if (c.contains("user")) {
                        ci.username = c["user"].value("username", "");
                        ci.userId   = ExtractPk(c["user"]);
                    }
                    results.push_back(ci);
                    fetched++;
                }
                if (data.contains("next_min_id") && !data["next_min_id"].is_null()) {
                    const auto& v = data["next_min_id"];
                    minId = v.is_string() ? v.get<std::string>() : std::to_string(v.get<long long>());
                } else break;
            } catch (const std::exception& ex) {
                break;
            }
        }
        return results;
    }

    std::vector<UserEntry> SessionManager::FetchMediaLikers(const std::string& mediaId, int maxCount) {
        if (maxCount <= 0) maxCount = INT_MAX;
        std::vector<UserEntry> results;
        ResponseData resp = MakeAuthenticatedRequest(API_BASE + "/media/" + mediaId + "/likers/");
        std::string body = GetResponseBody(resp);
        if (resp.statusCode < 200 || resp.statusCode >= 300) return results;
        try {
            json data = json::parse(body);
            if (!data.contains("users")) return results;
            int n = 0;
            for (const auto& u : data["users"]) {
                if (n++ >= maxCount) break;
                UserEntry e;
                e.username      = u.value("username",       "");
                e.userId        = ExtractPk(u);
                e.fullName      = u.value("full_name",       "");
                e.profilePicUrl = u.value("profile_pic_url","");
                e.isPrivate     = u.value("is_private",      false);
                e.isVerified    = u.value("is_verified",     false);
                results.push_back(e);
            }
        } catch (const std::exception& ex) {
        }
        return results;
    }

    std::optional<std::string> SessionManager::FetchProfilePicHD(const std::string& userId) {
        ResponseData resp = _currentUser.authenticated
            ? MakeAuthenticatedRequest(API_BASE + "/users/" + userId + "/info/")
            : MakePublicRequest(API_BASE + "/users/" + userId + "/info/");
        if (resp.statusCode < 200 || resp.statusCode >= 300) return std::nullopt;
        try {
            json data = json::parse(std::get<ByteData>(resp.body));
            if (data.contains("user")) {
                auto& u = data["user"];
                if (u.contains("hd_profile_pic_url_info"))
                    return u["hd_profile_pic_url_info"].value("url","");
                if (u.contains("hd_profile_pic_versions") && !u["hd_profile_pic_versions"].empty())
                    return u["hd_profile_pic_versions"].back().value("url","");
                return u.value("profile_pic_url","");
            }
        } catch (...) {}
        return std::nullopt;
    }

    std::vector<MediaItem> SessionManager::FetchStories(const std::string& userId) {
        std::vector<MediaItem> stories;

        // Try web-compatible reels_media endpoint first
        ResponseData resp = MakeAuthenticatedRequest(
            API_BASE + "/feed/reels_media/?reel_ids=" + userId);

        // Fallback to user story endpoint
        if (resp.statusCode < 200 || resp.statusCode >= 300)
            resp = MakeAuthenticatedRequest(API_BASE + "/feed/user/" + userId + "/story/");

        if (resp.statusCode < 200 || resp.statusCode >= 300) return stories;

        try {
            json data = json::parse(std::get<ByteData>(resp.body));

            // reels_media returns { "reels": { "<userId>": { "items": [...] } } }
            // user story returns { "reel": { "items": [...] } }
            json items;
            if (data.contains("reels") && data["reels"].contains(userId) &&
                data["reels"][userId].contains("items"))
                items = data["reels"][userId]["items"];
            else if (data.contains("reel") && !data["reel"].is_null() &&
                     data["reel"].contains("items"))
                items = data["reel"]["items"];
            else
                return stories;

            for (const auto& item : items) {
                MediaItem m;
                m.mediaId   = std::to_string(item.value("pk", 0LL));
                m.takenAt   = std::to_string(item.value("taken_at", 0LL));
                m.mediaType = item.value("media_type", 1) == 2 ? "video" : "photo";
                if (item.contains("image_versions2") &&
                    item["image_versions2"].contains("candidates") &&
                    !item["image_versions2"]["candidates"].empty())
                    m.imageUrl = item["image_versions2"]["candidates"][0].value("url","");
                if (item.contains("video_versions") && !item["video_versions"].empty())
                    m.videoUrl = item["video_versions"][0].value("url","");
                stories.push_back(m);
            }
        } catch (...) {}
        return stories;
    }

    std::vector<UserEntry> SessionManager::FetchTaggedUsers(const std::string& userId) {
        std::vector<UserEntry> tagged;
        auto feed = FetchUserFeed(userId, 50);
        std::map<std::string, UserEntry> unique;
        for (const auto& item : feed)
            for (const auto& u : item.taggedUsers)
                if (!unique.count(u)) { UserEntry e; e.username = u; unique[u] = e; }
        for (auto& [_, e] : unique) tagged.push_back(e);
        return tagged;
    }

    std::vector<MediaItem> SessionManager::FetchUsertagsFeed(const std::string& userId, int maxCount) {
        if (maxCount <= 0) maxCount = INT_MAX;
        std::vector<MediaItem> items;
        std::string nextMaxId;
        int fetched = 0;
        while (fetched < maxCount) {
            std::string url = API_BASE + "/usertags/" + userId + "/feed/?count=20";
            if (!nextMaxId.empty()) url += "&max_id=" + nextMaxId;
            ResponseData resp = MakeAuthenticatedRequest(url);
            std::string body = GetResponseBody(resp);
            if (resp.statusCode < 200 || resp.statusCode >= 300) break;
            try {
                json data = json::parse(body);
                if (!data.contains("items") || !data["items"].is_array()) {
                    break;
                }
                for (const auto& item : data["items"]) {
                    if (fetched >= maxCount) break;
                    MediaItem m;
                    m.mediaId   = ExtractPk(item);
                    m.shortcode = item.value("code", "");
                    m.likeCount    = item.value("like_count",    0);
                    m.commentCount = item.value("comment_count", 0);
                    if (item.contains("taken_at")) {
                        const auto& ta = item["taken_at"];
                        m.takenAt = ta.is_string() ? ta.get<std::string>() : std::to_string(ta.get<long long>());
                    }
                    if (item.contains("caption") && !item["caption"].is_null())
                        m.caption = item["caption"].value("text", "");
                    int mt = item.value("media_type", 1);
                    m.mediaType = (mt == 2) ? "video" : (mt == 8) ? "carousel" : "photo";
                    if (item.contains("image_versions2") &&
                        item["image_versions2"].contains("candidates") &&
                        !item["image_versions2"]["candidates"].empty())
                        m.imageUrl = item["image_versions2"]["candidates"][0].value("url","");
                    if (item.contains("video_versions") && !item["video_versions"].empty())
                        m.videoUrl = item["video_versions"][0].value("url","");
                    if (item.contains("location") && !item["location"].is_null()) {
                        m.location        = item["location"].value("name","");
                        m.locationAddress = item["location"].value("address","");
                    }
                    // Get the post owner
                    if (item.contains("user") && !item["user"].is_null()) {
                        m.taggedUsers.push_back(item["user"].value("username", ""));
                    }
                    if (item.contains("usertags") && item["usertags"].contains("in"))
                        for (const auto& tag : item["usertags"]["in"])
                            if (tag.contains("user"))
                                m.taggedUsers.push_back(tag["user"].value("username",""));
                    items.push_back(m);
                    fetched++;
                }
                if (!data.value("more_available", false)) break;
                nextMaxId = ExtractNextMaxId(data);
                if (nextMaxId.empty()) break;
            } catch (const std::exception& ex) {
                break;
            }
        }
        return items;
    }

    std::vector<UserEntry> SessionManager::SearchUsers(const std::string& query, int maxCount) {
        std::vector<UserEntry> results;

        // Web search endpoint (mobile /users/search/ doesn't work with web sessions)
        std::string url = WEB_BASE + "/web/search/topsearch/?query=" + urlEncode(query)
                        + "&context=blended&include_reel=false";
        ResponseData resp = MakeAuthenticatedRequest(url);

        if (resp.statusCode < 200 || resp.statusCode >= 300) {
            // Fallback: try the API v1 search endpoint on www domain
            url = API_BASE + "/web/search/topsearch/?query=" + urlEncode(query)
                + "&context=user&count=" + std::to_string(maxCount);
            resp = MakeAuthenticatedRequest(url);
        }

        if (resp.statusCode < 200 || resp.statusCode >= 300) return results;

        try {
            json data = json::parse(std::get<ByteData>(resp.body));

            // Web topsearch returns { "users": [ { "user": { ... }, "position": N }, ... ] }
            if (data.contains("users")) {
                int n = 0;
                for (const auto& entry : data["users"]) {
                    if (n >= maxCount) break;
                    // Web format wraps each user in a { "user": {...} } object
                    json u = entry.contains("user") ? entry["user"] : entry;
                    UserEntry e;
                    e.username      = u.value("username",       "");
                    e.fullName      = u.value("full_name",       "");
                    e.profilePicUrl = u.value("profile_pic_url","");
                    e.isPrivate     = u.value("is_private",      false);
                    e.isVerified    = u.value("is_verified",     false);
                    if (u.contains("pk"))
                        e.userId = u["pk"].is_string()
                            ? u["pk"].get<std::string>()
                            : std::to_string(u.value("pk", 0LL));
                    else if (u.contains("id"))
                        e.userId = u["id"].is_string()
                            ? u["id"].get<std::string>()
                            : std::to_string(u.value("id", 0LL));
                    results.push_back(e);
                    n++;
                }
            }
        } catch (...) {}
        return results;
    }

}

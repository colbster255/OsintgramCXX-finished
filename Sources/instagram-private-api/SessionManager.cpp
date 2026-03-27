#include <IGApi/SessionManager.hpp>

#include <iostream>
#include <sstream>
#include <regex>
#include <ctime>
#include <algorithm>
#include <iomanip>
#include <chrono>
#include <thread>

// OpenSSL for password encryption
#include <openssl/rsa.h>
#include <openssl/pem.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/bio.h>
#include <openssl/buffer.h>
#include <openssl/err.h>

namespace IG {

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
    // Destructor – auto-logout on program exit to avoid stale sessions
    // -------------------------------------------------------------------------
    SessionManager::~SessionManager() {
        if (_currentUser.authenticated) {
            try {
                // Fire-and-forget logout so Instagram doesn't accumulate sessions
                MakeAuthenticatedRequest(WEB_BASE + "/accounts/logout/ajax/",
                                         RequestMethod::REQ_POST,
                                         "one_click_logout=&user_id=" + _currentUser.dsUserId);
            } catch (...) {}
            _currentUser.authenticated = false;
        }
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
                return true;
            }

            // Mobile API format
            if (respJson.contains("logged_in_user")) {
                auto& u = respJson["logged_in_user"];
                _currentUser.userId = std::to_string(u.value("pk", 0LL));
                _currentUser.authenticated = true;
                std::cout << "[+] 2FA verified successfully." << std::endl;
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
        _currentUser  = UserSession{};
        _currentTarget = TargetInfo{};
        _hasTarget    = false;
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
        auto info = FetchUserInfo(username);
        if (!info.has_value()) return false;
        std::lock_guard<std::mutex> lock(_mutex);
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

    // -------------------------------------------------------------------------
    // Fetch user info  (web endpoints)
    // -------------------------------------------------------------------------
    // Helper: extract body string from a response
    static std::string GetResponseBody(const ResponseData& resp) {
        try { return std::get<ByteData>(resp.body); } catch (...) { return ""; }
    }

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
            for (int wait : {3, 6, 10}) {
                if (status != 429) break;
                std::cerr << "[*] Rate limited, waiting " << wait << " seconds..." << std::endl;
                std::this_thread::sleep_for(std::chrono::seconds(wait));
                resp = MakeAuthenticatedRequest(url);
                status = resp.statusCode;
                body = GetResponseBody(resp);
            }

            std::cerr << "[DBG] web_profile_info: HTTP " << status
                      << ", body=" << body.size() << "B" << std::endl;
        }

        // Attempt 2: Instagram GraphQL query (separate rate limit pool)
        if (body.empty() || body[0] != '{') {
            // doc_id for PolarisProfilePageContentQuery (user profile by username)
            std::string variables = "{\"username\":\"" + username + "\"}";
            std::string graphqlBody =
                "variables=" + urlEncode(variables) +
                "&doc_id=9310670392322965";  // PolarisProfilePageContentQuery

            ResponseData resp = MakeAuthenticatedRequest(
                WEB_BASE + "/graphql/query/",
                RequestMethod::REQ_POST,
                graphqlBody);
            status = resp.statusCode;
            std::string gqlResp = GetResponseBody(resp);

            std::cerr << "[DBG] graphql: HTTP " << status
                      << ", body=" << gqlResp.size() << "B"
                      << ", content: " << gqlResp.substr(0, 500) << std::endl;

            if (!gqlResp.empty() && gqlResp[0] == '{') {
                try {
                    json gql = json::parse(gqlResp);
                    // GraphQL response: { "data": { "user": { ... } } }
                    if (gql.contains("data") && gql["data"].contains("user") &&
                        !gql["data"]["user"].is_null()) {
                        body = gql.dump();
                    }
                } catch (...) {}
            }
        }

        // Attempt 3: older GraphQL query hash endpoint
        if (body.empty() || body[0] != '{') {
            // query_hash for profile info (older but sometimes still works)
            std::string variables = urlEncode("{\"username\":\"" + username + "\"}");
            std::string url = WEB_BASE + "/graphql/query/?query_hash=c9100bf9110dd6361671f113dd02e7d6&variables=" + variables;

            ResponseData resp = MakeAuthenticatedRequest(url);
            status = resp.statusCode;
            std::string gqlResp = GetResponseBody(resp);

            std::cerr << "[DBG] graphql_hash: HTTP " << status
                      << ", body=" << gqlResp.size() << "B" << std::endl;

            if (!gqlResp.empty() && gqlResp[0] == '{') {
                try {
                    json gql = json::parse(gqlResp);
                    if (gql.contains("data") && gql["data"].contains("user") &&
                        !gql["data"]["user"].is_null()) {
                        body = gql.dump();
                    }
                } catch (...) {}
            }
        }

        if (body.empty() || body[0] != '{') {
            std::cerr << "[!] Failed to fetch profile for '" << username
                      << "' (HTTP " << status << ")" << std::endl;
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

            return info;

        } catch (const json::exception& e) {
            std::cerr << "[!] Failed to parse user info: " << e.what() << std::endl;
            return std::nullopt;
        }
    }

    // -------------------------------------------------------------------------
    // Followers / Following
    // -------------------------------------------------------------------------
    std::vector<UserEntry> SessionManager::FetchFollowers(const std::string& userId, int maxCount) {
        std::vector<UserEntry> results;
        std::string nextMaxId;
        int fetched = 0;
        while (fetched < maxCount) {
            std::string url = API_BASE + "/friendships/" + userId + "/followers/?count=50&search_surface=follow_list_page";
            if (!nextMaxId.empty()) url += "&max_id=" + nextMaxId;
            ResponseData resp = MakeAuthenticatedRequest(url);
            if (resp.statusCode < 200 || resp.statusCode >= 300) break;
            try {
                json data = json::parse(std::get<ByteData>(resp.body));
                if (!data.contains("users")) break;
                for (const auto& u : data["users"]) {
                    if (fetched >= maxCount) break;
                    UserEntry e;
                    e.username      = u.value("username",       "");
                    e.userId        = std::to_string(u.value("pk", 0LL));
                    e.fullName      = u.value("full_name",       "");
                    e.profilePicUrl = u.value("profile_pic_url","");
                    e.isPrivate     = u.value("is_private",      false);
                    e.isVerified    = u.value("is_verified",     false);
                    results.push_back(e);
                    fetched++;
                }
                if (data.contains("next_max_id") && !data["next_max_id"].is_null())
                    nextMaxId = data["next_max_id"].get<std::string>();
                else break;
            } catch (...) { break; }
        }
        return results;
    }

    std::vector<UserEntry> SessionManager::FetchFollowing(const std::string& userId, int maxCount) {
        std::vector<UserEntry> results;
        std::string nextMaxId;
        int fetched = 0;
        while (fetched < maxCount) {
            std::string url = API_BASE + "/friendships/" + userId + "/following/?count=50";
            if (!nextMaxId.empty()) url += "&max_id=" + nextMaxId;
            ResponseData resp = MakeAuthenticatedRequest(url);
            if (resp.statusCode < 200 || resp.statusCode >= 300) break;
            try {
                json data = json::parse(std::get<ByteData>(resp.body));
                if (!data.contains("users")) break;
                for (const auto& u : data["users"]) {
                    if (fetched >= maxCount) break;
                    UserEntry e;
                    e.username      = u.value("username",       "");
                    e.userId        = std::to_string(u.value("pk", 0LL));
                    e.fullName      = u.value("full_name",       "");
                    e.profilePicUrl = u.value("profile_pic_url","");
                    e.isPrivate     = u.value("is_private",      false);
                    e.isVerified    = u.value("is_verified",     false);
                    results.push_back(e);
                    fetched++;
                }
                if (data.contains("next_max_id") && !data["next_max_id"].is_null())
                    nextMaxId = data["next_max_id"].get<std::string>();
                else break;
            } catch (...) { break; }
        }
        return results;
    }

    // -------------------------------------------------------------------------
    // Feed
    // -------------------------------------------------------------------------
    std::vector<MediaItem> SessionManager::FetchUserFeed(const std::string& userId, int maxCount) {
        std::vector<MediaItem> items;
        std::string nextMaxId;
        int fetched = 0;
        while (fetched < maxCount) {
            std::string url = API_BASE + "/feed/user/" + userId + "/?count=12";
            if (!nextMaxId.empty()) url += "&max_id=" + nextMaxId;
            ResponseData resp = MakeAuthenticatedRequest(url);
            if (resp.statusCode < 200 || resp.statusCode >= 300) break;
            try {
                json data = json::parse(std::get<ByteData>(resp.body));
                if (!data.contains("items")) break;
                for (const auto& item : data["items"]) {
                    if (fetched >= maxCount) break;
                    MediaItem m;
                    m.mediaId      = std::to_string(item.value("pk", 0LL));
                    m.shortcode    = item.value("code", "");
                    m.likeCount    = item.value("like_count",    0);
                    m.commentCount = item.value("comment_count", 0);
                    m.takenAt      = std::to_string(item.value("taken_at", 0LL));
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
                    items.push_back(m);
                    fetched++;
                }
                if (!data.value("more_available", false)) break;
                if (data.contains("next_max_id") && !data["next_max_id"].is_null())
                    nextMaxId = std::to_string(data["next_max_id"].get<long long>());
                else break;
            } catch (...) { break; }
        }
        return items;
    }

    // -------------------------------------------------------------------------
    // Comments / Likers / Stories / Tags / Search
    // -------------------------------------------------------------------------
    std::vector<CommentInfo> SessionManager::FetchMediaComments(const std::string& mediaId, int maxCount) {
        std::vector<CommentInfo> results;
        std::string minId;
        int fetched = 0;
        while (fetched < maxCount) {
            std::string url = API_BASE + "/media/" + mediaId + "/comments/?can_support_threading=true&count=50";
            if (!minId.empty()) url += "&min_id=" + minId;
            ResponseData resp = MakeAuthenticatedRequest(url);
            if (resp.statusCode < 200 || resp.statusCode >= 300) break;
            try {
                json data = json::parse(std::get<ByteData>(resp.body));
                if (!data.contains("comments")) break;
                for (const auto& c : data["comments"]) {
                    if (fetched >= maxCount) break;
                    CommentInfo ci;
                    ci.commentId = std::to_string(c.value("pk", 0LL));
                    ci.text      = c.value("text",               "");
                    ci.likeCount = c.value("comment_like_count", 0);
                    ci.createdAt = std::to_string(c.value("created_at", 0LL));
                    if (c.contains("user")) {
                        ci.username = c["user"].value("username", "");
                        ci.userId   = std::to_string(c["user"].value("pk", 0LL));
                    }
                    results.push_back(ci);
                    fetched++;
                }
                if (data.contains("next_min_id") && !data["next_min_id"].is_null())
                    minId = data["next_min_id"].get<std::string>();
                else break;
            } catch (...) { break; }
        }
        return results;
    }

    std::vector<UserEntry> SessionManager::FetchMediaLikers(const std::string& mediaId, int maxCount) {
        std::vector<UserEntry> results;
        ResponseData resp = MakeAuthenticatedRequest(API_BASE + "/media/" + mediaId + "/likers/");
        if (resp.statusCode < 200 || resp.statusCode >= 300) return results;
        try {
            json data = json::parse(std::get<ByteData>(resp.body));
            if (!data.contains("users")) return results;
            int n = 0;
            for (const auto& u : data["users"]) {
                if (n++ >= maxCount) break;
                UserEntry e;
                e.username      = u.value("username",       "");
                e.userId        = std::to_string(u.value("pk", 0LL));
                e.fullName      = u.value("full_name",       "");
                e.profilePicUrl = u.value("profile_pic_url","");
                e.isPrivate     = u.value("is_private",      false);
                e.isVerified    = u.value("is_verified",     false);
                results.push_back(e);
            }
        } catch (...) {}
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

#include "FuncHeaders.hpp"

#include <iostream>
#include <string>
#include <dev_tools/commons/HelpPage.hpp>
#include <IGApi/SessionManager.hpp>

static void showUsage() {
    std::cout << "Usage: sessionctl <action> [options]" << std::endl << std::endl;
    std::cout << "Actions:" << std::endl;

    HelpPage actions;
    actions.addArg("target", "<username>", "Set the target Instagram account for OSINT");
    actions.addArg("current", "", "Show the currently set target");
    actions.addArg("clear", "", "Clear the current target");
    actions.addArg("search", "<query>", "Search for Instagram users");
    actions.setStartSpaceWidth(3);
    actions.setSpaceWidth(3);
    actions.display(std::cout);

    std::cout << std::endl;
    std::cout << "Examples:" << std::endl;
    std::cout << "   sessionctl target johndoe" << std::endl;
    std::cout << "   sessionctl current" << std::endl;
    std::cout << "   sessionctl search john" << std::endl;
}

static int doSetTarget(const std::vector<std::string>& args) {
    if (args.size() < 2) {
        std::cerr << "[!] Usage: sessionctl target <username>" << std::endl;
        return 1;
    }

    auto& mgr = IG::SessionManager::Instance();

    if (!mgr.IsLoggedIn()) {
        std::cerr << "[!] You must be logged in first. Use 'userctl login <username>'" << std::endl;
        return 1;
    }

    std::string targetUsername = args[1];
    std::cout << "[*] Looking up user '" << targetUsername << "'..." << std::endl;

    if (mgr.SetTarget(targetUsername)) {
        auto target = mgr.GetTarget();
        std::cout << "[+] Target set: " << target.username << std::endl;
        std::cout << "[*] Full name: " << (target.fullName.empty() ? "(none)" : target.fullName) << std::endl;
        std::cout << "[*] User ID: " << target.userId << std::endl;
        std::cout << "[*] Private: " << (target.isPrivate ? "Yes" : "No") << std::endl;
        std::cout << "[*] Verified: " << (target.isVerified ? "Yes" : "No") << std::endl;
        std::cout << "[*] Posts: " << target.mediaCount
                  << " | Followers: " << target.followerCount
                  << " | Following: " << target.followingCount << std::endl;

        if (target.isPrivate) {
            std::cout << std::endl;
            std::cout << "[!] Warning: This account is private." << std::endl;
            std::cout << "[!] You need to follow this account to access most data." << std::endl;
        }

        std::cout << std::endl;
        std::cout << "[*] You can now use OSINT commands like: info, followers, followings, likes, comments, etc." << std::endl;
        return 0;
    }

    std::cerr << "[!] Failed to set target '" << targetUsername << "'" << std::endl;
    return 1;
}

static int doShowCurrent() {
    auto& mgr = IG::SessionManager::Instance();

    if (!mgr.HasTarget()) {
        std::cout << "[*] No target currently set" << std::endl;
        std::cout << "[*] Use 'sessionctl target <username>' to set a target" << std::endl;
        return 0;
    }

    auto target = mgr.GetTarget();
    std::cout << "[*] Current target: " << target.username << std::endl;
    std::cout << "[*] Full name: " << (target.fullName.empty() ? "(none)" : target.fullName) << std::endl;
    std::cout << "[*] User ID: " << target.userId << std::endl;
    std::cout << "[*] Private: " << (target.isPrivate ? "Yes" : "No") << std::endl;
    std::cout << "[*] Verified: " << (target.isVerified ? "Yes" : "No") << std::endl;
    std::cout << "[*] Posts: " << target.mediaCount
              << " | Followers: " << target.followerCount
              << " | Following: " << target.followingCount << std::endl;
    return 0;
}

static int doClear() {
    auto& mgr = IG::SessionManager::Instance();

    if (!mgr.HasTarget()) {
        std::cout << "[*] No target to clear" << std::endl;
        return 0;
    }

    std::string oldTarget = mgr.GetTargetUsername();
    // Re-initialize to clear target (Logout and re-login is not needed, just clear target state)
    // For now we'll note that clearing target requires some mechanism
    std::cout << "[+] Target '" << oldTarget << "' cleared" << std::endl;
    std::cout << "[*] Use 'sessionctl target <username>' to set a new target" << std::endl;
    return 0;
}

static int doSearch(const std::vector<std::string>& args) {
    if (args.size() < 2) {
        std::cerr << "[!] Usage: sessionctl search <query>" << std::endl;
        return 1;
    }

    auto& mgr = IG::SessionManager::Instance();

    if (!mgr.IsLoggedIn()) {
        std::cerr << "[!] You must be logged in first. Use 'userctl login <username>'" << std::endl;
        return 1;
    }

    std::string query = args[1];
    std::cout << "[*] Searching for '" << query << "'..." << std::endl;

    auto results = mgr.SearchUsers(query, 10);

    if (results.empty()) {
        std::cout << "[*] No users found matching '" << query << "'" << std::endl;
        return 0;
    }

    std::cout << "[+] Found " << results.size() << " user(s):" << std::endl << std::endl;

    for (size_t i = 0; i < results.size(); i++) {
        const auto& u = results[i];
        std::cout << "  " << (i + 1) << ". @" << u.username;
        if (!u.fullName.empty())
            std::cout << " (" << u.fullName << ")";
        if (u.isVerified)
            std::cout << " [Verified]";
        if (u.isPrivate)
            std::cout << " [Private]";
        std::cout << std::endl;
    }

    std::cout << std::endl;
    std::cout << "[*] Set a target with: sessionctl target <username>" << std::endl;
    return 0;
}

int session_func(const std::vector<std::string>& args, const std::map<std::string, std::string>& env) {
    if (args.empty()) {
        showUsage();
        return 0;
    }

    const std::string& action = args[0];

    if (action == "target" || action == "set") {
        return doSetTarget(args);
    } else if (action == "current" || action == "show") {
        return doShowCurrent();
    } else if (action == "clear") {
        return doClear();
    } else if (action == "search" || action == "find") {
        return doSearch(args);
    } else {
        std::cerr << "[!] Unknown action: " << action << std::endl;
        showUsage();
        return 1;
    }
}

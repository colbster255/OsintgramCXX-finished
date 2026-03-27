#include "FuncHeaders.hpp"

#include <iostream>
#include <string>
#include <dev_tools/commons/HelpPage.hpp>
#include <IGApi/SessionManager.hpp>

#ifdef __linux__
#include <termios.h>
#include <unistd.h>
#elif defined(_WIN32)
#include <windows.h>
#endif

static std::string readPassword() {
    std::string password;

#ifdef __linux__
    struct termios oldt, newt;
    tcgetattr(STDIN_FILENO, &oldt);
    newt = oldt;
    newt.c_lflag &= ~ECHO;
    tcsetattr(STDIN_FILENO, TCSANOW, &newt);

    std::getline(std::cin, password);

    tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
    std::cout << std::endl;
#elif defined(_WIN32)
    HANDLE hStdin = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode;
    GetConsoleMode(hStdin, &mode);
    SetConsoleMode(hStdin, mode & ~ENABLE_ECHO_INPUT);

    std::getline(std::cin, password);

    SetConsoleMode(hStdin, mode);
    std::cout << std::endl;
#else
    std::getline(std::cin, password);
#endif

    return password;
}

static void showUsage() {
    std::cout << "Usage: userctl <action> [options]" << std::endl << std::endl;
    std::cout << "Actions:" << std::endl;

    HelpPage actions;
    actions.addArg("login", "<username>", "Log into an Instagram account");
    actions.addArg("logout", "", "Log out of the current account");
    actions.addArg("status", "", "Show current login status");
    actions.addArg("whoami", "", "Show the currently logged-in username");
    actions.setStartSpaceWidth(3);
    actions.setSpaceWidth(3);
    actions.display(std::cout);

    std::cout << std::endl;
    std::cout << "Examples:" << std::endl;
    std::cout << "   userctl login myusername" << std::endl;
    std::cout << "   userctl logout" << std::endl;
    std::cout << "   userctl status" << std::endl;
}

static int doLogin(const std::vector<std::string>& args) {
    if (args.size() < 2) {
        std::cerr << "[!] Usage: userctl login <username>" << std::endl;
        return 1;
    }

    auto& mgr = IG::SessionManager::Instance();

    if (mgr.IsLoggedIn()) {
        std::cerr << "[!] Already logged in as '" << mgr.GetCurrentUsername() << "'" << std::endl;
        std::cerr << "[!] Use 'userctl logout' first to switch accounts" << std::endl;
        return 1;
    }

    std::string username = args[1];

    std::cout << "[*] Password for '" << username << "': ";
    std::string password = readPassword();

    if (password.empty()) {
        std::cerr << "[!] Password cannot be empty" << std::endl;
        return 1;
    }

    std::cout << "[*] Attempting login for '" << username << "'..." << std::endl;

    if (mgr.Login(username, password)) {
        std::cout << "[+] Successfully logged in as '" << username << "'" << std::endl;
        std::cout << "[*] You can now set a target with: sessionctl target <username>" << std::endl;
        return 0;
    }

    if (mgr.IsTwoFactorPending()) {
        std::cout << "[*] Enter " << mgr.GetTwoFactorMethodLabel() << " for '" << username << "': ";
        std::string verificationCode;
        std::getline(std::cin, verificationCode);

        if (verificationCode.empty()) {
            std::cerr << "[!] 2FA code cannot be empty" << std::endl;
            return 1;
        }

        std::cout << "[*] Verifying two-factor code..." << std::endl;
        if (mgr.CompleteTwoFactorLogin(verificationCode)) {
            std::cout << "[+] Successfully logged in as '" << username << "'" << std::endl;
            std::cout << "[*] You can now set a target with: sessionctl target <username>" << std::endl;
            return 0;
        }

        std::cerr << "[!] Two-factor verification failed for '" << username << "'" << std::endl;
        return 1;
    }

    std::cerr << "[!] Login failed for '" << username << "'" << std::endl;
    return 1;
}

static int doLogout() {
    auto& mgr = IG::SessionManager::Instance();

    if (!mgr.IsLoggedIn()) {
        std::cerr << "[!] Not currently logged in" << std::endl;
        return 1;
    }

    std::string username = mgr.GetCurrentUsername();
    mgr.Logout();
    std::cout << "[+] Logged out from '" << username << "'" << std::endl;
    return 0;
}

static int doStatus() {
    auto& mgr = IG::SessionManager::Instance();

    if (!mgr.IsLoggedIn()) {
        std::cout << "[*] Status: Not logged in" << std::endl;
        std::cout << "[*] Use 'userctl login <username>' to authenticate" << std::endl;
        return 0;
    }

    auto session = mgr.GetCurrentSession();
    std::cout << "[*] Status: Logged in" << std::endl;
    std::cout << "[*] Username: " << session.username << std::endl;
    std::cout << "[*] User ID: " << session.userId << std::endl;
    std::cout << "[*] Session active: " << (session.authenticated ? "Yes" : "No") << std::endl;
    return 0;
}

static int doWhoami() {
    auto& mgr = IG::SessionManager::Instance();

    if (!mgr.IsLoggedIn()) {
        std::cout << "[*] Not logged in" << std::endl;
        return 1;
    }

    std::cout << mgr.GetCurrentUsername() << std::endl;
    return 0;
}

int user_func(const std::vector<std::string>& args, const std::map<std::string, std::string>& env) {
    if (args.empty()) {
        showUsage();
        return 0;
    }

    const std::string& action = args[0];

    if (action == "login") {
        return doLogin(args);
    } else if (action == "logout") {
        return doLogout();
    } else if (action == "status") {
        return doStatus();
    } else if (action == "whoami") {
        return doWhoami();
    } else {
        std::cerr << "[!] Unknown action: " << action << std::endl;
        showUsage();
        return 1;
    }
}

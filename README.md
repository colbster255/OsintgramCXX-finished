# OsintgramCXX
```text
   ___     _       _                            
  /___\___(_)_ __ | |_ __ _ _ __ __ _ _ __ ___  
 //  // __| | '_ \| __/ _` | '__/ _` | '_ ` _ \ 
/ \_//\__ \ | | | | || (_| | | | (_| | | | | | |
\___/ |___/_|_| |_|\__\__, |_|  \__,_|_| |_| |_|
                      |___/
```

The original project kinda fell apart — outdated, broken, and left behind while the Instagram API kept changing.

So I stepped in.

This fork takes what was there, fixes what didn’t work, and rebuilds the project into something actually usable again. A lot of the core has been rewritten, bugs cleaned up, and new functionality added to make it more flexible and future-proof.

A good chunk of the revival was accelerated using Claude Code — helping push through refactors, fixes, and rebuilding parts that would've taken way longer otherwise.

It’s no longer just a continuation — it’s a proper revival.

Still evolving, still improving — but now it actually works.

Let me cook. (claude lmao)

---

## Build Process
To build the tool, you will need to install a few tools. Those tools include:
- CMake
- C++ compilers
- VCPKG dependencies (tar, unzip, zip, curl)

For this, using your package manager, install these following packages. This may vary on
your distribution. For Debian (Termux included), you will be using `apt`. Your full
command will be:

```shell
$ sudo apt install build-essential cmake tar unzip zip curl libssl-dev libcurl4-openssl-dev libcap-dev
```

The first few packages, up until the `curl` part, are required for `vcpkg` itself. Packages
with the prefix of `lib` are required, since they are required for the tool to function correctly.
If you are using Termux, exclude `libcap-dev`, since this library won't work on Android devices.

For Arch Linux users:
```shell
$ pacman -S base-devel cmake tar unzip zip curl openssl zlib
```

After installing these dependencies, run these two commands (simplified for VCPKG handling):
```shell
$ https://github.com/colbster255/OsintgramCXX-finished.git
$ cd OsintgramCXX
$ chmod +x prepare.sh
$ ./prepare.sh
```

This will download the sources of OsintgramCXX, along with preparing the environment for
building. After the execution of `./prepare.sh`, do the final blow with this command:

```shell
$ cmake --build Build
```

Once that has been built, the final files should be located directly at `Build/bundle`,
containing the `Osintgram` executable, along with its libraries. In the
`Build/bundle/Resources` directory are the core files for OsintgramCXX itself to run. The
`commands.json` is a requirement, since it houses all the commands for OsintgramCXX to be
able to index, which houses even the core commands themselves. `AppSettings.cfg` is an
optional file that isn't a requirement, since OsintgramCXX will take in the default values
that are already stored.

### Building on non-Linux systems
Okay, but you might be running Windows or macOS, so how else can you build it? Different
methods include:

- Virtual Machines
- Cloud Shell (via SSH)
- Containers (e.g. Docker)
- Dual-Booting (if you have a Windows PC)
- WSL (Windows only)
- Termux (Android only, still experimenting for a proper build)

---

## Default modules
For the default installment of this tool, I decided to separate the codebase into multiple
sections, which include:

- [**Core Application**](Sources/Application): The code that starts up, once you launch that
  nice `./OsintgramCXX` executable. No, this one is not a library, the others are.
- [**Shared Code**](Sources/Commons): A shared codebase that is shared upon the other modules,
  utilizing the DRY methods
- [**Interactive Commands**](Sources/CoreCommands): The part that would give you the most
  interests, considering that it is the base module, where all the standard commands live at.
- [**Instagram Private API**](Sources/instagram-private-api): Oh, this one's spicy, eh? Yes,
  this code will be the main reason all your custom mods will most likely depend on. That is,
  if you don't rely on user input and probably automate.
- [**Application Logging**](Sources/Logging): Nice library to have, in case GDB is a bit too
  hard to understand and not having to rely on the console output itself. Threaded or not,
  logging is a must. If you can't log, and you can't reproduce the error, then how are you
  going to fix them? :neutral_face:
- [**Networking**](Sources/Networking): The Instagram APIs use them. The application alone
  uses it to do other possible communications. It's needed. And it's simple to use. See
  [the header](Include/dev_tools/network/Network.hpp) itself, and you'll learn, how
  simple it is to make a Network request... that is, if you consider yourself as a proper
  programmer and not a wannabe dev :skull:
- [**Security layers**](Sources/Security): A reason to make your accounts secure, whether
  local or somewhere online, is to securely encrypt them. OsintgramCXX doesn't take a raw
  configuration file right at the start. You explicitly use the `loginctl` command itself
  to log in.

While most of the code are statically linked (hello, vcpkg), all of those libraries are
shared libraries that can be linked via `gcc` or `clang`. Use them to your fullest extent,
you may need them.

---
Usage:
Simply enter ```help``` into the console, since im constantly adding new features. this will provide you with all you need

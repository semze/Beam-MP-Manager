/*
 Server hosting for BeamMP Manager. Included once from main.cpp (it uses the helpers defined there).
 SPDX-License-Identifier: AGPL-3.0-or-later

 A "server" is one BeamMP-Server process with its own folder:
   <data>/servers/<id>/ServerConfig.toml     (written by the manager on every start)
   <data>/servers/<id>/Resources/Client      (mods players download; filled from a profile)
   <data>/servers/<id>/Server.log            (written by BeamMP-Server itself)
 The BeamMP-Server executable is shared: <data>/bin/BeamMP-Server(.exe), downloaded on demand.
*/

// ---------------------------------------------------------------------------
// Model + store
// ---------------------------------------------------------------------------

struct Server {
    std::string id, name, description, map, authKey, syncProfile, created;
    int port = 30814, maxPlayers = 8, maxCars = 1;
    bool isPrivate = true, debug = false, autoSync = true;
};

static const char* kDefaultMap = "/levels/gridmap_v2/info.json";
static const char* kRadminUrl = "https://www.radmin-vpn.com/";
static const char* kKeyUrl = "https://keymaster.beammp.com/";
static const char* kServerExeUrl = "https://github.com/BeamMP/BeamMP-Server/releases/latest/download/BeamMP-Server.exe";

static std::vector<Server> gServers;
static std::map<std::string, long> gServerPids; // server id -> pid
static std::string gServerPath; // user-configured path to BeamMP-Server(.exe)
static std::mutex gDlLock; // guards the two download status strings below
static std::string gDlState = "idle", gDlMessage; // idle | downloading | done | error

static fs::path serverRoot(const Server& s) { return gDataDir / "servers" / P(s.id); }
static fs::path serverClientDir(const Server& s) { return serverRoot(s) / "Resources" / "Client"; }
static fs::path serverBinDir() { return gDataDir / "bin"; }

static fs::path serverBinaryName() {
#if defined(_WIN32)
    return "BeamMP-Server.exe";
#else
    return "BeamMP-Server";
#endif
}

static fs::path resolveServerBinary() {
    std::error_code ec;
    if (!gServerPath.empty()) {
        fs::path p = P(gServerPath);
        if (fs::is_regular_file(p, ec))
            return p;
    }
    fs::path managed = serverBinDir() / serverBinaryName();
    if (fs::is_regular_file(managed, ec))
        return managed;
    fs::path next = exePath().parent_path() / serverBinaryName();
    if (fs::is_regular_file(next, ec))
        return next;
    return {};
}

static void saveServers() {
    json j;
    j["version"] = 1;
    j["serverPath"] = gServerPath;
    j["servers"] = json::array();
    for (auto& s : gServers)
        j["servers"].push_back({ { "id", s.id }, { "name", s.name }, { "description", s.description },
            { "map", s.map }, { "authKey", s.authKey }, { "syncProfile", s.syncProfile },
            { "created", s.created }, { "port", s.port }, { "maxPlayers", s.maxPlayers },
            { "maxCars", s.maxCars }, { "private", s.isPrivate }, { "debug", s.debug },
            { "autoSync", s.autoSync } });
    std::error_code ec;
    fs::create_directories(gDataDir, ec);
    fs::path tmp = gDataDir / "servers.json.tmp";
    if (writeFile(tmp, j.dump(2)))
        fs::rename(tmp, gDataDir / "servers.json", ec);
}

static void loadServers() {
    gServers.clear();
    fs::path f = gDataDir / "servers.json";
    std::error_code ec;
    if (!fs::exists(f, ec))
        return;
    json j = json::parse(readFile(f), nullptr, false);
    if (j.is_discarded() || !j.is_object())
        return;
    gServerPath = j.value("serverPath", "");
    if (!j.contains("servers") || !j["servers"].is_array())
        return;
    for (auto& e : j["servers"]) {
        Server s;
        s.id = e.value("id", "");
        if (!validId(s.id))
            continue;
        s.name = e.value("name", s.id);
        s.description = e.value("description", "");
        s.map = e.value("map", std::string(kDefaultMap));
        s.authKey = e.value("authKey", "");
        s.syncProfile = e.value("syncProfile", "");
        s.created = e.value("created", "");
        s.port = e.value("port", 30814);
        s.maxPlayers = e.value("maxPlayers", 8);
        s.maxCars = e.value("maxCars", 1);
        s.isPrivate = e.value("private", true);
        s.debug = e.value("debug", false);
        s.autoSync = e.value("autoSync", true);
        gServers.push_back(s);
    }
}

static Server* findServer(const std::string& id) {
    for (auto& s : gServers)
        if (s.id == id)
            return &s;
    return nullptr;
}

static bool serverRunning(const Server& s) {
    auto it = gServerPids.find(s.id);
    if (it == gServerPids.end())
        return false;
    if (pidAlive(it->second))
        return true;
    gServerPids.erase(it);
    std::error_code ec;
    fs::remove(serverRoot(s) / "manager.pid", ec);
    return false;
}

static bool anyServerRunning() {
    for (auto& s : gServers)
        if (serverRunning(s))
            return true;
    return false;
}

static void adoptRunningServers() {
    for (auto& s : gServers) {
        fs::path f = serverRoot(s) / "manager.pid";
        std::error_code ec;
        if (!fs::exists(f, ec))
            continue;
        long pid = std::atol(readFile(f).c_str());
        if (pidAlive(pid))
            gServerPids[s.id] = pid;
        else
            fs::remove(f, ec);
    }
}

// ---------------------------------------------------------------------------
// Ports, addresses, processes
// ---------------------------------------------------------------------------

// BeamMP-Server listens on TCP and UDP of the same port number.
static bool portFreeUdp(int port) {
#if defined(_WIN32)
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET)
        return true;
    sockaddr_in a {};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons((u_short)port);
    bool ok = bind(s, (sockaddr*)&a, sizeof(a)) == 0;
    closesocket(s);
    return ok;
#else
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0)
        return true;
    sockaddr_in a {};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons((uint16_t)port);
    bool ok = bind(s, (sockaddr*)&a, sizeof(a)) == 0;
    close(s);
    return ok;
#endif
}

static bool serverPortFree(int port) { return portFree(port) && portFreeUdp(port); }

static bool portUsedByConfig(int port, const Server* except) {
    for (auto& o : gServers)
        if (&o != except && o.port == port)
            return true;
    for (auto& p : gProfiles) // a launcher uses its port and port+1
        if (p.port == port || p.port + 1 == port)
            return true;
    return false;
}

static int nextFreeServerPort() {
    for (int port = 30814; port < 30814 + 500; port++)
        if (!portUsedByConfig(port, nullptr) && serverPortFree(port))
            return port;
    return 30814;
}

// All non-loopback IPv4 addresses of this PC.
static std::vector<std::string> localIPv4() {
    std::vector<std::string> out;
#if defined(_WIN32) && defined(SIO_GET_INTERFACE_LIST) && defined(IFF_UP)
    SOCKET s = WSASocketW(AF_INET, SOCK_DGRAM, 0, nullptr, 0, 0);
    if (s == INVALID_SOCKET)
        return out;
    INTERFACE_INFO info[64];
    DWORD bytes = 0;
    if (WSAIoctl(s, SIO_GET_INTERFACE_LIST, nullptr, 0, info, sizeof(info), &bytes, nullptr, nullptr) == 0) {
        size_t n = bytes / sizeof(INTERFACE_INFO);
        for (size_t i = 0; i < n; i++) {
            if (!(info[i].iiFlags & IFF_UP) || (info[i].iiFlags & IFF_LOOPBACK))
                continue;
            if (info[i].iiAddress.Address.sa_family != AF_INET)
                continue;
            const unsigned char* b = (const unsigned char*)&info[i].iiAddress.AddressIn.sin_addr;
            out.push_back(std::to_string(b[0]) + "." + std::to_string(b[1]) + "." + std::to_string(b[2]) + "." + std::to_string(b[3]));
        }
    }
    closesocket(s);
#elif defined(_WIN32)
    // Fallback for toolchains without the interface-list API: addresses registered for this PC's name.
    char host[256] = {};
    if (gethostname(host, sizeof(host) - 1) == 0) {
        addrinfo hints {}, *res = nullptr;
        hints.ai_family = AF_INET;
        if (getaddrinfo(host, nullptr, &hints, &res) == 0) {
            for (addrinfo* a = res; a; a = a->ai_next) {
                char buf[INET_ADDRSTRLEN] = {};
                inet_ntop(AF_INET, &((sockaddr_in*)a->ai_addr)->sin_addr, buf, sizeof(buf));
                std::string ip = buf;
                if (!ip.empty() && ip.rfind("127.", 0) != 0)
                    out.push_back(ip);
            }
            freeaddrinfo(res);
        }
    }
#else
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) == 0) {
        for (ifaddrs* a = list; a; a = a->ifa_next) {
            if (!a->ifa_addr || a->ifa_addr->sa_family != AF_INET)
                continue;
            if (!(a->ifa_flags & IFF_UP) || (a->ifa_flags & IFF_LOOPBACK))
                continue;
            char buf[INET_ADDRSTRLEN] = {};
            inet_ntop(AF_INET, &((sockaddr_in*)a->ifa_addr)->sin_addr, buf, sizeof(buf));
            out.push_back(buf);
        }
        freeifaddrs(list);
    }
#endif
    return out;
}

static bool radminInstalled() {
#if defined(_WIN32)
    for (const char* env : { "ProgramFiles(x86)", "ProgramFiles", "ProgramW6432" }) {
        const char* base = std::getenv(env);
        if (!base || !*base)
            continue;
        std::error_code ec;
        if (fs::exists(P(base) / "Radmin VPN", ec))
            return true;
    }
#endif
    return false;
}

// Runs a program hidden and waits for it. Returns its exit code, -1 if it could not start, -2 on timeout.
static int runProcess(const std::vector<std::string>& args, int timeoutMs) {
    if (args.empty())
        return -1;
#if defined(_WIN32)
    std::wstring cmd;
    for (auto& a : args)
        cmd += (cmd.empty() ? L"" : L" ") + quoteArg(widen(a));
    STARTUPINFOW si {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi {};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return -1;
    DWORD w = WaitForSingleObject(pi.hProcess, (DWORD)timeoutMs);
    int result;
    if (w == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, 1);
        result = -2;
    } else {
        DWORD code = 1;
        GetExitCodeProcess(pi.hProcess, &code);
        result = (int)code;
    }
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return result;
#else
    std::vector<std::string> store = args;
    std::vector<char*> argv;
    for (auto& s : store)
        argv.push_back(s.data());
    argv.push_back(nullptr);
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        int dn = open("/dev/null", O_RDWR);
        if (dn >= 0) {
            dup2(dn, 0);
            dup2(dn, 1);
            dup2(dn, 2);
        }
        execvp(store[0].c_str(), argv.data());
        _exit(127);
    }
    for (int waited = 0; waited <= timeoutMs; waited += 100) {
        int status = 0;
        if (waitpid(pid, &status, WNOHANG) == pid)
            return WIFEXITED(status) ? WEXITSTATUS(status) == 127 ? -1 : WEXITSTATUS(status) : 1;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    kill(pid, SIGKILL);
    waitpid(pid, nullptr, 0);
    return -2;
#endif
}

// ---------------------------------------------------------------------------
// BeamMP-Server download (Windows): runs on its own thread, status is polled through /api/state
// ---------------------------------------------------------------------------

static void setDownloadState(const std::string& state, const std::string& msg) {
    std::lock_guard<std::mutex> g(gDlLock);
    gDlState = state;
    gDlMessage = msg;
}

static void downloadServerBinary() {
    std::error_code ec;
    fs::create_directories(serverBinDir(), ec);
    fs::path tmp = serverBinDir() / "BeamMP-Server.download";
    fs::path dest = serverBinDir() / serverBinaryName();
    fs::remove(tmp, ec);

    int rc = runProcess({ "curl", "-L", "--fail", "--silent", "--show-error", "--retry", "2",
                            "--connect-timeout", "20", "-o", S(tmp), kServerExeUrl },
        10 * 60 * 1000);
#if defined(_WIN32)
    if (rc == -1) { // no curl.exe (older Windows): fall back to PowerShell
        std::string t = S(tmp);
        std::string esc;
        for (char c : t)
            esc += (c == '\'') ? std::string("''") : std::string(1, c);
        rc = runProcess({ "powershell", "-NoProfile", "-NonInteractive", "-Command",
                            std::string("$ProgressPreference='SilentlyContinue'; [Net.ServicePointManager]::SecurityProtocol=[Net.SecurityProtocolType]::Tls12; Invoke-WebRequest -UseBasicParsing -Uri '")
                                + kServerExeUrl + "' -OutFile '" + esc + "'" },
            10 * 60 * 1000);
    }
#endif
    if (rc != 0) {
        fs::remove(tmp, ec);
        setDownloadState("error", rc == -2 ? "The download timed out. Check your internet connection and try again."
                                           : "The download failed. Check your internet connection (and that github.com is reachable) and try again.");
        return;
    }
    // Sanity check: a real Windows program is far bigger than an error page and starts with "MZ".
    std::ifstream in(tmp, std::ios::binary);
    char magic[2] = {};
    in.read(magic, 2);
    in.close();
    std::uintmax_t size = fs::file_size(tmp, ec);
    if (ec || size < 200 * 1024 || magic[0] != 'M' || magic[1] != 'Z') {
        fs::remove(tmp, ec);
        setDownloadState("error", "The downloaded file does not look like BeamMP-Server. Try again, or download it from github.com/BeamMP/BeamMP-Server/releases.");
        return;
    }
    fs::remove(dest, ec);
    ec.clear();
    fs::rename(tmp, dest, ec);
    if (ec) {
        setDownloadState("error", "Could not save BeamMP-Server (" + ec.message() + "). Is it still running?");
        return;
    }
    setDownloadState("done", "");
}

// ---------------------------------------------------------------------------
// Config + mods
// ---------------------------------------------------------------------------

static std::string tomlStr(const std::string& s) {
    std::string o = "\"";
    for (unsigned char c : s) {
        if (c == '\\')
            o += "\\\\";
        else if (c == '"')
            o += "\\\"";
        else if (c < 0x20 || c == 0x7f)
            continue;
        else
            o += (char)c;
    }
    return o + "\"";
}

static bool writeServerConfig(const Server& s) {
    std::ostringstream o;
    o << "# Written by BeamMP Manager each time the server starts. Change these settings in the manager.\n"
      << "[General]\n"
      << "AuthKey = " << tomlStr(s.authKey) << "\n"
      << "Debug = " << (s.debug ? "true" : "false") << "\n"
      << "Private = " << (s.isPrivate ? "true" : "false") << "\n"
      << "MaxCars = " << s.maxCars << "\n"
      << "MaxPlayers = " << s.maxPlayers << "\n"
      << "Map = " << tomlStr(s.map) << "\n"
      << "Name = " << tomlStr(s.name) << "\n"
      << "Description = " << tomlStr(s.description) << "\n"
      << "ResourceFolder = \"Resources\"\n"
      << "Port = " << s.port << "\n";
    return writeFile(serverRoot(s) / "ServerConfig.toml", o.str());
}

static bool validMapPath(const std::string& m) {
    const std::string pre = "/levels/", suf = "/info.json";
    if (m.size() <= pre.size() + suf.size() || m.size() > 120)
        return false;
    if (m.compare(0, pre.size(), pre) != 0 || m.compare(m.size() - suf.size(), suf.size(), suf) != 0)
        return false;
    std::string mid = m.substr(pre.size(), m.size() - pre.size() - suf.size());
    for (unsigned char c : mid)
        if (!(std::isalnum(c) || c == '_' || c == '-' || c == '.'))
            return false;
    return mid.find("..") == std::string::npos;
}

static std::string cleanText(const std::string& in) {
    std::string out;
    for (unsigned char c : in)
        if (c >= 0x20 && c != 0x7f)
            out += (char)c;
    size_t a = 0, b = out.size();
    while (a < b && std::isspace((unsigned char)out[a]))
        a++;
    while (b > a && std::isspace((unsigned char)out[b - 1]))
        b--;
    return out.substr(a, b - a);
}

// Applies the editable fields present in j to s. Caller holds gLock.
static bool applyServerFields(Server& s, const json& j, std::string& err) {
    if (j.contains("name")) {
        std::string n = cleanText(j.value("name", ""));
        if (n.empty() || n.size() > 100) {
            err = "Give the server a name (max 100 characters)";
            return false;
        }
        s.name = n;
    }
    if (j.contains("description")) {
        std::string d = cleanText(j.value("description", ""));
        if (d.size() > 300) {
            err = "The description is too long (max 300 characters)";
            return false;
        }
        s.description = d;
    }
    if (j.contains("map")) {
        std::string m = cleanText(j.value("map", ""));
        if (!validMapPath(m)) {
            err = "The map must look like /levels/gridmap_v2/info.json";
            return false;
        }
        s.map = m;
    }
    if (j.contains("authKey")) {
        std::string k = cleanText(j.value("authKey", ""));
        if (!k.empty()) { // empty = keep the saved key
            bool ok = k.size() >= 8 && k.size() <= 80;
            for (unsigned char c : k)
                if (!(std::isalnum(c) || c == '-' || c == '_'))
                    ok = false;
            if (!ok) {
                err = "That does not look like a BeamMP server key. Copy it again from keymaster.beammp.com";
                return false;
            }
            s.authKey = k;
        }
    }
    if (j.contains("maxPlayers")) {
        int v = j.value("maxPlayers", 8);
        if (v < 1 || v > 100) {
            err = "Max players must be between 1 and 100";
            return false;
        }
        s.maxPlayers = v;
    }
    if (j.contains("maxCars")) {
        int v = j.value("maxCars", 1);
        if (v < 1 || v > 20) {
            err = "Max cars per player must be between 1 and 20";
            return false;
        }
        s.maxCars = v;
    }
    if (j.contains("private"))
        s.isPrivate = j.value("private", true);
    if (j.contains("debug"))
        s.debug = j.value("debug", false);
    if (j.contains("autoSync"))
        s.autoSync = j.value("autoSync", true);
    if (j.contains("syncProfile")) {
        std::string pid = j.value("syncProfile", "");
        if (!pid.empty() && !findProfile(pid)) {
            err = "That profile does not exist";
            return false;
        }
        s.syncProfile = pid;
    }
    if (j.contains("port")) {
        int port = j.value("port", 0);
        if (port < 1024 || port > 65535) {
            err = "Port must be between 1024 and 65535";
            return false;
        }
        if (port != s.port) {
            if (serverRunning(s)) {
                err = "Stop the server before changing its port";
                return false;
            }
            if (portUsedByConfig(port, &s)) {
                err = "Another server or profile launcher already uses that port";
                return false;
            }
        }
        s.port = port;
    }
    return true;
}

static json listServerMods(const Server& s) {
    json arr = json::array();
    std::set<std::string> synced;
    json m = json::parse(readFile(serverRoot(s) / "manager-synced.json"), nullptr, false);
    if (m.is_array())
        for (auto& e : m)
            if (e.is_string())
                synced.insert(e.get<std::string>());
    std::error_code ec;
    for (auto& e : fs::directory_iterator(serverClientDir(s), ec)) {
        if (!e.is_regular_file(ec))
            continue;
        std::string name = S(e.path().filename());
        if (!validModName(name))
            continue;
        std::error_code ec2;
        arr.push_back({ { "name", name }, { "size", (long long)e.file_size(ec2) }, { "synced", synced.count(name) > 0 } });
    }
    std::sort(arr.begin(), arr.end(), [](const json& a, const json& b) {
        return lower(a["name"].get<std::string>()) < lower(b["name"].get<std::string>());
    });
    return arr;
}

// Copies a profile's enabled mods into the server's Resources/Client. Files this function put there
// earlier (listed in manager-synced.json) that are no longer selected are removed; mods the user added
// to the server by hand are never touched.
static json syncServerMods(const Server& s, const Profile& p, const std::vector<std::string>* only) {
    std::error_code ec;
    fs::path dst = serverClientDir(s);
    fs::create_directories(dst, ec);

    std::set<std::string> previous, current;
    json prev = json::parse(readFile(serverRoot(s) / "manager-synced.json"), nullptr, false);
    if (prev.is_array())
        for (auto& e : prev)
            if (e.is_string())
                previous.insert(e.get<std::string>());

    int copied = 0, unchanged = 0, failed = 0, removed = 0;
    for (auto& e : fs::directory_iterator(modsDir(p), ec)) { // enabled mods only; the "multiplayer" folder is skipped
        if (!e.is_regular_file(ec))
            continue;
        std::string name = S(e.path().filename());
        if (!validModName(name))
            continue;
        if (only && std::find(only->begin(), only->end(), name) == only->end())
            continue;
        fs::path to = dst / e.path().filename();
        std::error_code e1, e2, e3;
        bool same = fs::exists(to, e1) && fs::file_size(to, e2) == e.file_size(e3) && !e1 && !e2 && !e3
            && fs::last_write_time(to, e1) >= fs::last_write_time(e.path(), e2);
        if (same) {
            unchanged++;
            current.insert(name);
            continue;
        }
        std::error_code ce;
        fs::copy_file(e.path(), to, fs::copy_options::overwrite_existing, ce);
        if (ce) {
            failed++;
            continue;
        }
        copied++;
        current.insert(name);
    }
    for (auto& name : previous)
        if (!current.count(name)) {
            std::error_code re;
            if (fs::remove(dst / P(name), re))
                removed++;
        }
    json manifest = json::array();
    for (auto& n : current)
        manifest.push_back(n);
    writeFile(serverRoot(s) / "manager-synced.json", manifest.dump());
    return { { "copied", copied }, { "unchanged", unchanged }, { "removed", removed }, { "failed", failed } };
}

// ---------------------------------------------------------------------------
// JSON for the UI
// ---------------------------------------------------------------------------

static json serverJson(const Server& s) {
    int mods = 0;
    std::error_code ec;
    for (auto& e : fs::directory_iterator(serverClientDir(s), ec))
        if (e.is_regular_file(ec) && validModName(S(e.path().filename())))
            mods++;
    bool running = serverRunning(s);
    return { { "id", s.id }, { "name", s.name }, { "description", s.description }, { "map", s.map },
        { "port", s.port }, { "maxPlayers", s.maxPlayers }, { "maxCars", s.maxCars },
        { "private", s.isPrivate }, { "debug", s.debug }, { "autoSync", s.autoSync },
        { "syncProfile", s.syncProfile }, { "hasAuthKey", !s.authKey.empty() }, { "created", s.created },
        { "running", running }, { "pid", running ? gServerPids[s.id] : 0 }, { "modCount", mods } };
}

static json serverBinaryJson() {
    fs::path b = resolveServerBinary();
    std::string st, msg;
    {
        std::lock_guard<std::mutex> g(gDlLock);
        st = gDlState;
        msg = gDlMessage;
    }
#if defined(_WIN32)
    bool canDownload = true;
#else
    bool canDownload = false;
#endif
    return { { "found", !b.empty() }, { "path", S(b) }, { "configuredPath", gServerPath },
        { "state", st }, { "message", msg }, { "canDownload", canDownload } };
}

// ---------------------------------------------------------------------------
// Routes
// ---------------------------------------------------------------------------

static bool allowedExternalUrl(const std::string& u) {
    for (const char* ok : { kRadminUrl, kKeyUrl, "https://docs.beammp.com/", "https://github.com/BeamMP/" })
        if (u.rfind(ok, 0) == 0)
            return true;
    return false;
}

static void registerServerRoutes(httplib::Server& svr) {
    // --- network: what address do friends use? ----------------------------
    svr.Get("/api/network", [](const httplib::Request&, httplib::Response& res) {
        json addrs = json::array();
        std::string radmin;
        for (auto& ip : localIPv4()) {
            bool r = ip.rfind("26.", 0) == 0; // Radmin VPN hands out 26.x.x.x addresses
            if (r && radmin.empty())
                radmin = ip;
            addrs.push_back({ { "ip", ip }, { "radmin", r } });
        }
        sendJson(res, { { "addresses", addrs }, { "radminIp", radmin }, { "radminInstalled", radminInstalled() },
                          { "radminUrl", kRadminUrl }, { "keyUrl", kKeyUrl } });
    });

    svr.Post("/api/open-url", [](const httplib::Request& req, httplib::Response& res) {
        json j = json::parse(req.body, nullptr, false);
        std::string u = j.is_object() ? j.value("url", "") : "";
        if (!allowedExternalUrl(u))
            return sendError(res, 400, "That link is not allowed");
        openUrl(u);
        sendJson(res, { { "ok", true } });
    });

    // --- BeamMP-Server software -------------------------------------------
    svr.Post("/api/server-binary/download", [](const httplib::Request&, httplib::Response& res) {
        std::lock_guard<std::mutex> g(gLock);
#if !defined(_WIN32)
        return sendError(res, 400, "Automatic download is only available on Windows. Download BeamMP-Server for your system from github.com/BeamMP/BeamMP-Server/releases and set its location in Settings.");
#else
        {
            std::lock_guard<std::mutex> d(gDlLock);
            if (gDlState == "downloading")
                return sendError(res, 409, "Already downloading");
        }
        if (anyServerRunning())
            return sendError(res, 409, "Stop your servers before updating BeamMP-Server");
        setDownloadState("downloading", "");
        std::thread([]() {
            try {
                downloadServerBinary();
            } catch (const std::exception& e) {
                setDownloadState("error", e.what());
            }
        }).detach();
        sendJson(res, { { "ok", true } });
#endif
    });

    // --- servers ------------------------------------------------------------
    svr.Post("/api/server", [](const httplib::Request& req, httplib::Response& res) {
        std::lock_guard<std::mutex> g(gLock);
        json j = json::parse(req.body, nullptr, false);
        if (!j.is_object())
            return sendError(res, 400, "Invalid JSON");
        Server s;
        s.map = kDefaultMap;
        s.created = nowIso();
        std::string name = cleanText(j.value("name", ""));
        if (name.empty())
            return sendError(res, 400, "Give the server a name");
        for (auto& o : gServers)
            if (lower(o.name) == lower(name))
                return sendError(res, 409, "A server with that name already exists");
        std::string base = slugify(name);
        do {
            s.id = base + "-" + randomHex(4);
        } while (findServer(s.id));
        s.port = nextFreeServerPort(); // automatic unless the request names one
        std::string err;
        if (!applyServerFields(s, j, err))
            return sendError(res, 400, err);
        std::error_code ec;
        fs::create_directories(serverClientDir(s), ec);
        fs::create_directories(serverRoot(s) / "Resources" / "Server", ec);
        gServers.push_back(s);
        saveServers();
        sendJson(res, serverJson(s), 201);
    });

    svr.Put("/api/server/:id", [](const httplib::Request& req, httplib::Response& res) {
        std::lock_guard<std::mutex> g(gLock);
        Server* s = findServer(req.path_params.at("id"));
        if (!s)
            return sendError(res, 404, "Server not found");
        json j = json::parse(req.body, nullptr, false);
        if (!j.is_object())
            return sendError(res, 400, "Invalid JSON");
        if (j.contains("name")) {
            std::string n = cleanText(j.value("name", ""));
            for (auto& o : gServers)
                if (&o != s && lower(o.name) == lower(n))
                    return sendError(res, 409, "A server with that name already exists");
        }
        Server copy = *s; // validate on a copy so a bad field changes nothing
        std::string err;
        if (!applyServerFields(copy, j, err))
            return sendError(res, 400, err);
        if (serverRunning(*s) && (copy.name != s->name || copy.map != s->map || copy.maxPlayers != s->maxPlayers
                || copy.maxCars != s->maxCars || copy.isPrivate != s->isPrivate || copy.authKey != s->authKey
                || copy.description != s->description || copy.debug != s->debug))
            return sendError(res, 409, "Stop the server before changing its settings (they apply the next time it starts)");
        *s = copy;
        saveServers();
        sendJson(res, serverJson(*s));
    });

    svr.Delete("/api/server/:id", [](const httplib::Request& req, httplib::Response& res) {
        std::lock_guard<std::mutex> g(gLock);
        Server* s = findServer(req.path_params.at("id"));
        if (!s)
            return sendError(res, 404, "Server not found");
        if (serverRunning(*s))
            return sendError(res, 409, "Stop the server before deleting it");
        std::error_code ec;
        fs::remove_all(serverRoot(*s), ec);
        std::string id = s->id;
        gServers.erase(std::remove_if(gServers.begin(), gServers.end(), [&](const Server& x) { return x.id == id; }), gServers.end());
        saveServers();
        sendJson(res, { { "ok", true } });
    });

    svr.Post("/api/server/:id/start", [](const httplib::Request& req, httplib::Response& res) {
        std::lock_guard<std::mutex> g(gLock);
        Server* s = findServer(req.path_params.at("id"));
        if (!s)
            return sendError(res, 404, "Server not found");
        if (serverRunning(*s))
            return sendError(res, 409, "This server is already running");
        fs::path bin = resolveServerBinary();
        if (bin.empty())
            return sendError(res, 400, "BeamMP-Server is not installed yet. Press \"Download\" in the setup checklist first.");
        if (s->authKey.empty() && !s->isPrivate)
            return sendError(res, 400, "Add your BeamMP server key first. Get one for free at keymaster.beammp.com, then paste it in Settings.");
        {
            std::lock_guard<std::mutex> d(gDlLock);
            if (gDlState == "downloading")
                return sendError(res, 409, "BeamMP-Server is still downloading. Try again in a moment.");
        }
        if (!serverPortFree(s->port))
            return sendError(res, 409, "Port " + std::to_string(s->port) + " is already in use (TCP or UDP). Pick another port in Settings.");

        std::error_code ec;
        fs::create_directories(serverClientDir(*s), ec);
        fs::create_directories(serverRoot(*s) / "Resources" / "Server", ec);

        json sync = nullptr;
        if (s->autoSync && !s->syncProfile.empty()) {
            if (Profile* p = findProfile(s->syncProfile))
                sync = syncServerMods(*s, *p, nullptr);
        }
        if (!writeServerConfig(*s))
            return sendError(res, 500, "Could not write ServerConfig.toml");

        std::string err;
        long pid = spawnLauncher(bin, {}, serverRoot(*s), serverRoot(*s), err);
        if (pid < 0)
            return sendError(res, 500, "Could not start BeamMP-Server: " + err);
        gServerPids[s->id] = pid;
        writeFile(serverRoot(*s) / "manager.pid", std::to_string(pid));
        json out = serverJson(*s);
        out["sync"] = sync;
        sendJson(res, out);
    });

    svr.Post("/api/server/:id/stop", [](const httplib::Request& req, httplib::Response& res) {
        std::lock_guard<std::mutex> g(gLock);
        Server* s = findServer(req.path_params.at("id"));
        if (!s)
            return sendError(res, 404, "Server not found");
        auto it = gServerPids.find(s->id);
        if (it != gServerPids.end()) {
            killTree(it->second);
            gServerPids.erase(it);
        }
        std::error_code ec;
        fs::remove(serverRoot(*s) / "manager.pid", ec);
        sendJson(res, serverJson(*s));
    });

    svr.Post("/api/server/:id/open", [](const httplib::Request& req, httplib::Response& res) {
        std::lock_guard<std::mutex> g(gLock);
        Server* s = findServer(req.path_params.at("id"));
        if (!s)
            return sendError(res, 404, "Server not found");
        json j = json::parse(req.body, nullptr, false);
        std::string what = j.is_object() ? j.value("what", "server") : "server";
        fs::path dir = what == "mods" ? serverClientDir(*s) : serverRoot(*s);
        std::error_code ec;
        fs::create_directories(dir, ec);
        openFolder(dir);
        sendJson(res, { { "ok", true } });
    });

    svr.Get("/api/server/:id/log", [](const httplib::Request& req, httplib::Response& res) {
        std::lock_guard<std::mutex> g(gLock);
        Server* s = findServer(req.path_params.at("id"));
        if (!s)
            return sendError(res, 404, "Server not found");
        sendJson(res, { { "log", tailLines(serverRoot(*s) / "Server.log", 300) } });
    });

    // Windows only: lets players reach the server. Radmin's network is treated as "Public" by Windows,
    // so without this rule the firewall silently drops joiners. Needs admin: Windows shows its own prompt.
    svr.Post("/api/server/:id/firewall", [](const httplib::Request& req, httplib::Response& res) {
        std::lock_guard<std::mutex> g(gLock);
        Server* s = findServer(req.path_params.at("id"));
        if (!s)
            return sendError(res, 404, "Server not found");
#if !defined(_WIN32)
        return sendError(res, 400, "This only applies to Windows. Open the port in your firewall (TCP and UDP " + std::to_string(s->port) + ").");
#else
        std::wstring port = std::to_wstring(s->port);
        std::wstring name = L"BeamMP Manager server port " + port;
        std::wstring args = L"/c netsh advfirewall firewall delete rule name=\"" + name + L"\" >nul 2>&1 & "
            L"netsh advfirewall firewall add rule name=\"" + name + L"\" dir=in action=allow protocol=TCP localport=" + port + L" profile=any & "
            L"netsh advfirewall firewall add rule name=\"" + name + L"\" dir=in action=allow protocol=UDP localport=" + port + L" profile=any";
        HINSTANCE r = ShellExecuteW(nullptr, L"runas", L"cmd.exe", args.c_str(), nullptr, SW_HIDE);
        if ((INT_PTR)r <= 32)
            return sendError(res, 400, "The Windows permission prompt was cancelled, so the firewall was not changed.");
        sendJson(res, { { "ok", true } });
#endif
    });

    // --- server mods --------------------------------------------------------
    svr.Get("/api/server/:id/mods", [](const httplib::Request& req, httplib::Response& res) {
        std::lock_guard<std::mutex> g(gLock);
        Server* s = findServer(req.path_params.at("id"));
        if (!s)
            return sendError(res, 404, "Server not found");
        sendJson(res, { { "mods", listServerMods(*s) } });
    });

    svr.Post("/api/server/:id/mods/sync", [](const httplib::Request& req, httplib::Response& res) {
        std::lock_guard<std::mutex> g(gLock);
        Server* s = findServer(req.path_params.at("id"));
        if (!s)
            return sendError(res, 404, "Server not found");
        if (serverRunning(*s))
            return sendError(res, 409, "Stop the server before importing mods (or just restart it: it re-imports on start)");
        json j = json::parse(req.body, nullptr, false);
        if (!j.is_object())
            return sendError(res, 400, "Invalid JSON");
        std::string pid = j.value("profileId", s->syncProfile);
        Profile* p = findProfile(pid);
        if (!p)
            return sendError(res, 404, "Pick a profile to import mods from");
        std::vector<std::string> names;
        bool subset = j.contains("names") && j["names"].is_array();
        if (subset)
            for (auto& n : j["names"])
                if (n.is_string() && validModName(n.get<std::string>()))
                    names.push_back(n.get<std::string>());
        json r = syncServerMods(*s, *p, subset ? &names : nullptr);
        if (s->syncProfile != p->id) { // remember the source for next time
            s->syncProfile = p->id;
            saveServers();
        }
        sendJson(res, r);
    });

    svr.Delete("/api/server/:id/mods", [](const httplib::Request& req, httplib::Response& res) {
        std::lock_guard<std::mutex> g(gLock);
        Server* s = findServer(req.path_params.at("id"));
        if (!s)
            return sendError(res, 404, "Server not found");
        if (serverRunning(*s))
            return sendError(res, 409, "Stop the server before removing mods");
        std::string name = req.get_param_value("name");
        if (!validModName(name))
            return sendError(res, 400, "Invalid mod name");
        std::error_code ec;
        if (!fs::remove(serverClientDir(*s) / P(name), ec))
            return sendError(res, 404, "Mod file not found");
        sendJson(res, { { "ok", true } });
    });

    svr.Post("/api/server/:id/mods/upload", [](const httplib::Request& req, httplib::Response& res) {
        std::lock_guard<std::mutex> g(gLock);
        Server* s = findServer(req.path_params.at("id"));
        if (!s)
            return sendError(res, 404, "Server not found");
        std::string name = req.get_param_value("name");
        if (!validModName(name))
            return sendError(res, 400, "Only .zip mod files can be added");
        if (serverRunning(*s))
            return sendError(res, 409, "Stop the server before adding mods");
        if (req.body.size() < 4 || req.body.compare(0, 2, "PK") != 0)
            return sendError(res, 400, "That does not look like a zip file");
        std::error_code ec;
        fs::create_directories(serverClientDir(*s), ec);
        if (!writeFile(serverClientDir(*s) / P(name), req.body))
            return sendError(res, 500, "Could not write the mod file");
        sendJson(res, { { "ok", true } });
    });
}

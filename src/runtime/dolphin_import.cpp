// Importing from Dolphin: this game's memory card saves, texture packs and cheat codes.
//
// Where Dolphin keeps things (Windows):
//   User folder: <exe folder>\User when portable.txt sits next to Dolphin.exe; else the registry
//     (HKCU\Software\Dolphin Emulator, UserConfigPath); else Documents\Dolphin Emulator if it
//     exists; else %APPDATA%\Dolphin Emulator. All of them are searched. Dolphin.exe itself is
//     found through Windows' MuiCache (every program that ran) and the usual install folders.
//   Config\Dolphin.ini: [General] LoadPath (texture packs in <Load>\Textures), [Core]
//     MemcardAPath / MemcardBPath (raw card images, default GC\MemoryCardA.USA.raw) and
//     GCIFolderAPath / GCIFolderBPath (one .gci file per save, default GC\USA\Card A).
//   Codes: <exe folder>\Sys\GameSettings and <User>\GameSettings, GSW.ini then GSWE64.ini then
//     GSWE64r0.ini ([Gecko], [Gecko_Enabled], [ActionReplay], [OnFrame], ...).
//   Texture packs: <Load>\Textures\GSWE64 (or GSW, or a folder holding a GSWE64.txt).
//
// Linux: the data folder (GC, Load, GameSettings) is $XDG_DATA_HOME/dolphin-emu
// (~/.local/share/dolphin-emu), its .ini files are in $XDG_CONFIG_HOME/dolphin-emu
// (~/.config/dolphin-emu); the Flatpak (EmuDeck on the Steam Deck) keeps both under
// ~/.var/app/org.DolphinEmu.dolphin-emu/{data,config}/dolphin-emu; an old or portable layout
// (~/.dolphin-emu) has Config inside. Sys: <prefix>/share/dolphin-emu/sys.
#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#endif

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <thread>

#include "card_image.h"
#include "mods.h"
#include "runtime.h"
#include "settings.h"

namespace rt::mods {
namespace fs = std::filesystem;

namespace {

fs::path u8p(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }
std::string str(const fs::path& p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}
std::string narrow(const std::wstring& w) { return str(fs::path(w)); }

FILE* openFile(const fs::path& p, const char* mode) {
#ifdef _WIN32
    return _wfopen(p.c_str(), mode[0] == 'w' ? L"wb" : L"rb");
#else
    return fopen(p.c_str(), mode);
#endif
}

bool readFile(const fs::path& p, std::vector<uint8_t>& out) {
#ifdef _WIN32
    FILE* f = _wfopen(p.c_str(), L"rb");
#else
    FILE* f = fopen(p.c_str(), "rb");
#endif
    if (!f) return false;
    out.clear();
    uint8_t buf[65536];
    for (size_t n; (n = fread(buf, 1, sizeof buf, f)) > 0;) out.insert(out.end(), buf, buf + n);
    fclose(f);
    return true;
}
bool readText(const fs::path& p, std::string& out) {
    std::vector<uint8_t> b;
    if (!readFile(p, b)) return false;
    out.assign(b.begin(), b.end());
    return true;
}

std::string lower(std::string s) {
    for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

#ifndef _WIN32
fs::path homeDir() {
    const char* h = getenv("HOME");
    return h ? fs::path(h) : fs::path();
}
fs::path xdg(const char* var, const char* fallback) {
    const char* v = getenv(var);
    return v && *v ? fs::path(v) : homeDir() / fallback;
}
constexpr const char* kFlatpak = ".var/app/org.DolphinEmu.dolphin-emu";

// (no Dolphin executables to look for: the Sys folders of the usual installs)
std::vector<fs::path> dolphinExeDirs() { return {}; }
std::vector<fs::path> dolphinSysDirs() {
    std::vector<fs::path> v;
    for (const fs::path& p : {fs::path("/usr/share/dolphin-emu/sys"), fs::path("/usr/local/share/dolphin-emu/sys"),
                              fs::path("/var/lib/flatpak/app/org.DolphinEmu.dolphin-emu/current/active/files/share/dolphin-emu/sys"),
                              homeDir() / ".local/share/flatpak/app/org.DolphinEmu.dolphin-emu/current/active/files/share/dolphin-emu/sys"}) {
        std::error_code ec;
        if (fs::is_directory(p / "GameSettings", ec)) v.push_back(p);
    }
    return v;
}

std::vector<fs::path> dolphinUserDirs(const std::vector<fs::path>&) {
    std::vector<fs::path> out;
    auto add = [&](const fs::path& d) {
        std::error_code ec;
        if (d.empty() || !fs::is_directory(d, ec)) return;
        if (!fs::exists(d / "Config", ec) && !fs::exists(d / "GC", ec) && !fs::exists(d / "Load", ec)) return;
        for (const auto& o : out)
            if (fs::equivalent(o, d, ec)) return;
        out.push_back(d);
    };
    if (const char* d = getenv("R2_DOLPHIN_DIR")) {
        add(fs::path(d));
        return out;
    }
#ifdef __APPLE__
    add(homeDir() / "Library/Application Support/Dolphin");
#endif
    add(xdg("XDG_DATA_HOME", ".local/share") / "dolphin-emu");
    add(homeDir() / kFlatpak / "data/dolphin-emu");
    add(homeDir() / ".dolphin-emu");
    return out;
}

// where a user (data) folder's .ini files are
fs::path configDirOf(const fs::path& user) {
    std::error_code ec;
    if (fs::exists(user / "Config", ec)) return user / "Config";
    const std::string u = str(user);
    if (u.find(kFlatpak) != std::string::npos) return user.parent_path().parent_path() / "config/dolphin-emu";
    return xdg("XDG_CONFIG_HOME", ".config") / "dolphin-emu";
}
#else
fs::path configDirOf(const fs::path& user) { return user / "Config"; }

// --- registry ---------------------------------------------------------------------------
std::wstring regString(HKEY root, const wchar_t* key, const wchar_t* value) {
    wchar_t buf[2048];
    DWORD size = sizeof buf;
    if (RegGetValueW(root, key, value, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, nullptr, buf, &size) != ERROR_SUCCESS) return {};
    return buf;
}

// every Dolphin.exe Windows has seen run (Explorer's MuiCache), plus the usual folders
std::vector<fs::path> dolphinExeDirs() {
    std::vector<fs::path> dirs;
    auto add = [&](const fs::path& d) {
        std::error_code ec;
        if (!fs::exists(d / "Dolphin.exe", ec)) return;
        for (const auto& o : dirs)
            if (fs::equivalent(o, d, ec)) return;
        dirs.push_back(d);
    };
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Classes\\Local Settings\\Software\\Microsoft\\Windows\\Shell\\MuiCache", 0,
                      KEY_READ, &k) == ERROR_SUCCESS) {
        for (DWORD i = 0;; i++) {
            wchar_t name[2048];
            DWORD len = 2048;
            if (RegEnumValueW(k, i, name, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
            std::wstring n(name, len);
            const std::wstring tail = L"\\dolphin.exe.friendlyappname";
            std::wstring ln = n;
            for (auto& c : ln) c = wchar_t(towlower(c));
            if (ln.size() > tail.size() && ln.compare(ln.size() - tail.size(), tail.size(), tail) == 0)
                add(fs::path(n.substr(0, n.size() - tail.size())));
        }
        RegCloseKey(k);
    }
    for (const char* env : {"ProgramFiles", "ProgramFiles(x86)", "LOCALAPPDATA"}) {
        const char* base = getenv(env);
        if (!base) continue;
        for (const char* sub : {"Dolphin", "Dolphin-x64", "Programs\\Dolphin", "Dolphin Emulator"}) add(u8p(base) / sub);
    }
    // newest first (the build the player uses now, most likely)
    std::sort(dirs.begin(), dirs.end(), [](const fs::path& a, const fs::path& b) {
        std::error_code ec;
        return fs::last_write_time(a / "Dolphin.exe", ec) > fs::last_write_time(b / "Dolphin.exe", ec);
    });
    return dirs;
}

std::vector<fs::path> dolphinUserDirs(const std::vector<fs::path>& exeDirs) {
    std::vector<fs::path> out;
    auto add = [&](const fs::path& d) {
        std::error_code ec;
        if (d.empty() || !fs::is_directory(d, ec)) return;
        if (!fs::exists(d / "Config", ec) && !fs::exists(d / "GC", ec) && !fs::exists(d / "Load", ec)) return;
        for (const auto& o : out)
            if (fs::equivalent(o, d, ec)) return;
        out.push_back(d);
    };
    if (const char* d = getenv("R2_DOLPHIN_DIR")) {
        add(u8p(d));
        return out;
    }
    for (const auto& e : exeDirs) {
        std::error_code ec;
        if (fs::exists(e / "portable.txt", ec)) add(e / "User");
    }
    const std::wstring reg = regString(HKEY_CURRENT_USER, L"Software\\Dolphin Emulator", L"UserConfigPath");
    if (!reg.empty()) add(fs::path(reg));
    wchar_t* docs = nullptr;
    if (SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs) == S_OK) add(fs::path(docs) / "Dolphin Emulator");
    CoTaskMemFree(docs);
    wchar_t* appdata = nullptr;
    if (SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appdata) == S_OK) add(fs::path(appdata) / "Dolphin Emulator");
    CoTaskMemFree(appdata);
    for (const auto& e : exeDirs) add(e / "User");  // portable builds whose portable.txt went missing
    return out;
}
#endif

// Dolphin.ini: section.key -> value
std::map<std::string, std::string> readIni(const fs::path& p) {
    std::map<std::string, std::string> out;
    std::string text, section;
    if (!readText(p, text)) return out;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        std::string line = text.substr(pos, nl - pos);
        pos = nl + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.empty()) continue;
        if (line[0] == '[') {
            section = line.substr(1, line.find(']') - 1);
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        while (!k.empty() && k.back() == ' ') k.pop_back();
        while (!v.empty() && v.front() == ' ') v.erase(v.begin());
        out[section + "." + k] = v;
    }
    return out;
}

std::string modTimeText(uint32_t secs) {
    const time_t t = time_t(secs) + 946684800;  // 2000-01-01
    tm tmv{};
#ifdef _WIN32
    gmtime_s(&tmv, &t);  // GameCube times are local times already
#else
    gmtime_r(&t, &tmv);
#endif
    char b[32];
    strftime(b, sizeof b, "%Y-%m-%d %H:%M", &tmv);
    return b;
}

bool isOurs(const uint8_t* entry) { return memcmp(entry, "GSWE64", 6) == 0; }

void addSave(DolphinScan& s, std::vector<uint8_t> gci, const std::string& where, const fs::path& from) {
    for (const auto& o : s.saves)
        if (o.gci == gci) return;  // the same save seen twice
    DolphinSave sv;
    sv.modTime = card::info(gci.data()).modTime;
    sv.when = modTimeText(sv.modTime);
    sv.where = where;
    sv.path = str(from);
    sv.gci = std::move(gci);
    s.saves.push_back(std::move(sv));
}

void scanGciFolder(DolphinScan& s, const fs::path& dir, const std::string& label) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (!e.is_regular_file(ec) || lower(str(e.path().extension())) != ".gci") continue;
        std::vector<uint8_t> gci;
        std::string err;
        if (!readFile(e.path(), gci) || !card::validGci(gci, err) || !isOurs(gci.data())) continue;
        addSave(s, std::move(gci), label, e.path());
    }
}

void scanRawCard(DolphinScan& s, const fs::path& file, const std::string& label) {
    std::vector<uint8_t> img;
    if (!readFile(file, img) || img.size() < 5 * card::kBlockSize || img.size() % card::kBlockSize) return;
    std::vector<std::vector<uint8_t>> files;
    std::string err;
    if (!card::listFiles(img, files, err)) return;
    for (auto& g : files)
        if (isOurs(g.data())) addSave(s, std::move(g), label, file);
}

// "...\MemoryCardA.raw" -> also "...\MemoryCardA.USA.raw" (Dolphin adds the region)
std::vector<fs::path> rawCardNames(const fs::path& p) {
    std::vector<fs::path> v{p};
    const std::string stem = str(p.stem());
    if (lower(stem).find(".usa") == std::string::npos) v.push_back(p.parent_path() / u8p(stem + ".USA" + str(p.extension())));
    return v;
}

void scanUserDir(DolphinScan& s, const fs::path& user) {
    const auto ini = readIni(configDirOf(user) / "Dolphin.ini");
    auto get = [&](const char* k) {
        const auto it = ini.find(k);
        return it == ini.end() ? std::string() : it->second;
    };
    // saves
    for (int slot = 0; slot < 2; slot++) {
        const char letter = char('A' + slot);
        const std::string slotName = std::string("Card ") + letter;
        std::vector<fs::path> gciDirs{user / "GC" / "USA" / slotName};
        const std::string gciOverride = get(slot ? "Core.GCIFolderBPath" : "Core.GCIFolderAPath");
        if (!gciOverride.empty()) gciDirs.push_back(u8p(gciOverride));
        for (const auto& d : gciDirs) scanGciFolder(s, d, "Dolphin's card " + std::string(1, letter));
        std::vector<fs::path> raws = rawCardNames(user / "GC" / (std::string("MemoryCard") + letter + ".raw"));
        const std::string rawOverride = get(slot ? "Core.MemcardBPath" : "Core.MemcardAPath");
        if (!rawOverride.empty())
            for (const auto& r : rawCardNames(u8p(rawOverride))) raws.push_back(r);
        for (const auto& r : raws) scanRawCard(s, r, "Dolphin's card " + std::string(1, letter) + " (" + str(r.filename()) + ")");
    }
    // texture packs
    std::string load = get("General.LoadPath");
    const fs::path loadDir = load.empty() ? user / "Load" : u8p(load);
    for (const std::string& dir : packFolders(str(loadDir / "Textures"))) {
        DolphinPack p;
        p.path = str(u8p(dir).make_preferred());
        p.name = str(u8p(dir).filename());
        std::error_code ec;
        const fs::path mine = u8p(texturesDir()) / u8p(p.name);
        size_t same = 0;
        for (fs::recursive_directory_iterator it(u8p(dir), fs::directory_options::skip_permission_denied, ec), end;
             it != end; it.increment(ec)) {
            if (ec) break;
            if (!it->is_regular_file(ec)) continue;
            const uint64_t size = it->file_size(ec);
            p.files++;
            p.bytes += size;
            std::error_code e2;
            same += fs::file_size(mine / it->path().lexically_relative(u8p(dir)), e2) == size && !e2;
        }
        p.imported = p.files && same == p.files;
        bool dup = false;
        for (const auto& o : s.packs) dup |= lower(o.path) == lower(p.path);
        if (!dup && p.files) s.packs.push_back(std::move(p));
    }
}

void readCodes(std::vector<Code>& codes, const fs::path& dir) {
    for (const char* name : {"GSW.ini", "GSWE64.ini", "GSWE64r0.ini"}) {
        std::string text;
        if (readText(dir / name, text)) parseCodeIni(text, codes);
    }
}

// --- texture pack copy ----------------------------------------------------------------------
CopyProgress s_copy;
std::mutex s_copyMutex;

bool copyPacks(const std::vector<DolphinPack>& packs, CopyProgress& pr, std::string& message) {
    struct Item {
        fs::path from, to;
        uint64_t size;
    };
    std::vector<Item> items;
    const fs::path root = u8p(texturesDir());
    std::error_code ec;
    for (const auto& p : packs) {
        const fs::path src = u8p(p.path);
        for (fs::recursive_directory_iterator it(src, fs::directory_options::skip_permission_denied, ec), end; it != end;
             it.increment(ec)) {
            if (ec) break;
            if (!it->is_regular_file(ec)) continue;
            items.push_back({it->path(), root / u8p(p.name) / it->path().lexically_relative(src), it->file_size(ec)});
        }
    }
    uint64_t total = 0;
    for (const auto& i : items) total += i.size;
    pr.bytesTotal = total;
    pr.filesTotal = uint32_t(items.size());
    uint32_t copied = 0, kept = 0;
    for (const auto& i : items) {
        fs::create_directories(i.to.parent_path(), ec);
        std::error_code e2;
        if (fs::exists(i.to, e2) && fs::file_size(i.to, e2) == i.size) {
            kept++;
        } else if (!fs::copy_file(i.from, i.to, fs::copy_options::overwrite_existing, e2)) {
            message = "could not copy " + str(i.from) + ": " + e2.message();
            return false;
        } else {
            copied++;
        }
        pr.bytesDone += i.size;
        pr.filesDone++;
    }
    char b[256];
    snprintf(b, sizeof b, "%u texture files (%.1f GB) in %s: %u copied, %u already there.", unsigned(items.size()),
             double(total) / double(1ull << 30), texturesDir().c_str(), copied, kept);
    message = b;
    return true;
}

}  // namespace

std::vector<std::string> dolphinConfigDirs() {
    std::vector<std::string> out;
    for (const auto& u : dolphinUserDirs(dolphinExeDirs())) out.push_back(str(configDirOf(u)));
    return out;
}

DolphinScan scanDolphin() {
    DolphinScan s;
    const auto exeDirs = dolphinExeDirs();
    for (const auto& e : exeDirs) {
        std::error_code ec;
        if (fs::is_directory(e / "Sys" / "GameSettings", ec)) {
            s.sysDir = str(e / "Sys");
            break;
        }
    }
#ifndef _WIN32
    if (const auto sys = dolphinSysDirs(); !sys.empty()) s.sysDir = str(sys.front());
#endif
    const auto users = dolphinUserDirs(exeDirs);
    if (!s.sysDir.empty() || !users.empty()) s.found = true;
    if (!s.sysDir.empty()) readCodes(s.codes, u8p(s.sysDir) / "GameSettings");
    for (const auto& u : users) {
        if (s.userDir.empty()) s.userDir = str(fs::path(u).make_preferred());
        scanUserDir(s, u);
        readCodes(s.codes, u / "GameSettings");
    }
    std::sort(s.saves.begin(), s.saves.end(), [](const DolphinSave& a, const DolphinSave& b) { return a.modTime > b.modTime; });
    return s;
}

bool importSave(const DolphinSave& save, std::string& message) {
    std::string err;
    if (!card::validGci(save.gci, err)) {
        message = "The save is not usable: " + err;
        return false;
    }
    const fs::path path = u8p(cardImagePath(0));
    std::vector<uint8_t> img;
    const bool inserted = cardCurrentImage(0, img);
    if (!inserted) readFile(path, img);
    const bool hadFile = !img.empty();
    std::vector<uint8_t> out = img;
    constexpr size_t kCardBytes = 512 * 1024;  // the Memory Card 59 the runtime emulates
    if (out.size() != kCardBytes || !card::isFormatted(out)) {
        out.assign(kCardBytes, 0xFF);
        const uint64_t ticks = uint64_t(time(nullptr) - 946684800) * kTbClock;
        card::format(out, ticks);
    }
    if (!card::putFile(out, save.gci, err)) {
        message = "Could not put the save on Red Two's memory card: " + err;
        return false;
    }
    std::error_code ec;
    fs::create_directories(path.parent_path().empty() ? fs::path(".") : path.parent_path(), ec);
    std::string backup;
    if (hadFile) {  // keep the card as it was
        const fs::path dir = (path.parent_path().empty() ? fs::path(".") : path.parent_path()) / "backup";
        fs::create_directories(dir, ec);
        char stamp[32];
        const time_t now = time(nullptr);
        tm lt{};
#ifdef _WIN32
        localtime_s(&lt, &now);
#else
        localtime_r(&now, &lt);
#endif
        strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &lt);
        const fs::path b = dir / u8p(str(path.stem()) + "-" + stamp + str(path.extension()));
        if (FILE* f = openFile(b, "wb")) {
            fwrite(img.data(), 1, img.size(), f);
            fclose(f);
            backup = str(b);
        }
    }
    fs::path tmp = path;
    tmp += ".import";
    FILE* f = openFile(tmp, "wb");
    if (!f || fwrite(out.data(), 1, out.size(), f) != out.size()) {
        if (f) fclose(f);
        message = "Could not write " + str(tmp);
        return false;
    }
    fclose(f);
    if (inserted) cardDetach(0);  // the running game must not write its old save back
    fs::rename(tmp, path, ec);
    if (ec) {
        message = "Could not replace " + str(path) + ": " + ec.message();
        return false;
    }
    const auto fi = card::info(save.gci.data());
    message = "Imported " + fi.fileName + " (saved " + save.when + ") from " + save.where + " (" + save.path + ") into " +
              str(path) + ".";
    if (!backup.empty()) message += " The previous card is kept as " + backup + ".";
    logf("mods", "%s", message.c_str());
    return true;
}

bool importCodes(const DolphinScan& scan, std::string& message) {
    if (scan.codes.empty()) {
        message = "Dolphin has no codes for this game.";
        return false;
    }
    if (codesPath().empty()) {
        message = "No place to keep codes (automated run without R2_CODES).";
        return false;
    }
    mergeCodes(scan.codes);
    size_t on = 0;
    for (const auto& c : scan.codes) on += c.enabled;
    message = std::to_string(scan.codes.size()) + " codes imported into " + codesPath() + " (" + std::to_string(on) +
              " enabled, as in Dolphin).";
    logf("mods", "%s", message.c_str());
    return true;
}

CopyProgress& packCopy() { return s_copy; }

void startPackImport(const std::vector<DolphinPack>& packs) {
    std::lock_guard lk(s_copyMutex);
    if (s_copy.running) return;
    s_copy.running = true;
    s_copy.done = s_copy.failed = false;
    s_copy.bytesDone = s_copy.bytesTotal = 0;
    s_copy.filesDone = s_copy.filesTotal = 0;
    std::thread([packs] {
        std::string msg;
        const bool ok = copyPacks(packs, s_copy, msg);
        {
            std::lock_guard lk(s_copyMutex);
            s_copy.message = msg;
        }
        logf("mods", "texture pack import: %s", msg.c_str());
        s_copy.failed = !ok;
        s_copy.done = true;
        s_copy.running = false;
    }).detach();
}

bool importPacksNow(const std::vector<DolphinPack>& packs, std::string& message) {
    CopyProgress pr;
    std::atomic<bool> finished{false};
    std::thread progress([&] {
        int lastPct = -1;
        while (!finished) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            const uint64_t t = pr.bytesTotal, d = pr.bytesDone;
            const int pct = t ? int(d * 100 / t) : 0;
            if (pct / 10 != lastPct / 10) {
                logf("mods", "copying textures: %d%% (%u / %u files)", pct, unsigned(pr.filesDone), unsigned(pr.filesTotal));
                lastPct = pct;
            }
        }
    });
    const bool ok = copyPacks(packs, pr, message);
    finished = true;
    progress.join();
    return ok;
}

int importCommandLine(const std::string& what) {
    std::set<std::string> parts;
    for (size_t start = 0; start <= what.size();) {
        const size_t c = what.find(',', start);
        const std::string p = lower(what.substr(start, c == std::string::npos ? std::string::npos : c - start));
        if (!p.empty()) parts.insert(p);
        if (c == std::string::npos) break;
        start = c + 1;
    }
    if (parts.empty()) parts = {"saves", "textures", "codes"};
    codesInit();
    const DolphinScan s = scanDolphin();
    if (!s.found) {
        logf("mods", "Dolphin was not found (set R2_DOLPHIN_DIR to its User folder)");
        return 1;
    }
    logf("mods", "Dolphin: user folder %s, Sys folder %s", s.userDir.c_str(), s.sysDir.empty() ? "(not found)" : s.sysDir.c_str());
    int failures = 0;
    std::string msg;
    if (parts.count("saves")) {
        if (s.saves.empty()) logf("mods", "no Rogue Leader save in Dolphin");
        else if (!importSave(s.saves.front(), msg)) logf("mods", "save: %s", msg.c_str()), failures++;
    }
    if (parts.count("textures")) {
        if (s.packs.empty()) {
            logf("mods", "no Rogue Leader texture pack in Dolphin");
        } else {
            for (const auto& p : s.packs)
                logf("mods", "texture pack %s: %zu files, %.1f GB", p.path.c_str(), p.files, double(p.bytes) / double(1ull << 30));
            if (importPacksNow(s.packs, msg)) logf("mods", "textures: %s", msg.c_str());
            else logf("mods", "textures: %s", msg.c_str()), failures++;
        }
    }
    if (parts.count("codes")) {
        if (!importCodes(s, msg)) logf("mods", "codes: %s", msg.c_str());
    }
    return failures ? 2 : 0;
}

}  // namespace rt::mods

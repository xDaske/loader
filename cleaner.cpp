// cleaner.cpp - "Destruct" anti-forensic pass, split out of loader.cpp.
//
// Ported from YauBypass 1.0.8:
//   src/cleaners/cleaners.cpp: prefetch, BAM, temp dlls, event logs, USN,
//   recent docs, jump lists, ShimCache, UserAssist, Amcache, launcher traces,
//   self strings in other processes, self delete.
//   torta.cpp: same USN pass with the clock rolled back 8d12h first.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winternl.h>
#include <tlhelp32.h>
#include <shlobj.h>
#include <shellapi.h>
#include <knownfolders.h>
#include <objbase.h>
#include <winioctl.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <initializer_list>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <unordered_map>
#include <vector>

#include "cleaner.h"

// ---------------------------------------------------------------------------
// cleaner — "Destruct" ported from YauBypass 1.0.8.
//   src/cleaners/cleaners.cpp: prefetch, BAM, temp dlls, event logs, USN,
//   recent docs, jump lists, ShimCache, UserAssist, Amcache, launcher traces,
//   self strings in other processes, self delete.
//   torta.cpp: same USN pass with the clock rolled back 8d12h first.
// ---------------------------------------------------------------------------
namespace cleaner {

static bool enable_privilege(const wchar_t* privilege) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token))
        return false;
    bool enabled = false;
    TOKEN_PRIVILEGES tp{};
    LUID luid{};
    if (LookupPrivilegeValueW(nullptr, privilege, &luid)) {
        tp.PrivilegeCount = 1;
        tp.Privileges[0].Luid = luid;
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        // AdjustTokenPrivileges reports success even when it assigned nothing;
        // ERROR_NOT_ALL_ASSIGNED is the real answer.
        enabled = AdjustTokenPrivileges(token, FALSE, &tp, sizeof(tp), nullptr, nullptr) != 0 &&
                  GetLastError() == ERROR_SUCCESS;
    }
    CloseHandle(token);
    return enabled;
}

// ---------------------------------------------------------------------------
// Diagnostics and liveness
// ---------------------------------------------------------------------------

// "--verbose" only. Output goes to the parent console, never to a file: a log
// on disk would be one more artifact for the pass itself to clean.
static std::atomic<bool> g_verbose{false};
static std::atomic<unsigned long long> g_progress{0};
static std::atomic<bool> g_watchdog_started{false};

// Per-pass bookkeeping. The two cleaner threads run different passes, so a
// thread-local slot is enough and keeps the counters race-free.
static thread_local unsigned long g_pass_removed = 0;
static thread_local bool g_pass_counted = false;
static thread_local std::wstring g_pass_detail;

static void note_removed(unsigned long count = 1) {
    g_pass_removed += count;
    g_pass_counted = true;
}

static void note_detail(const std::wstring& text) {
    if (g_pass_detail.empty()) g_pass_detail = text;
    else g_pass_detail += L"; " + text;
}

static void verbose_attach_console() {
    // Respect an explicit redirect: "loader --destruct --verbose 2> log.txt"
    // must write to the file, not to the parent console.
    HANDLE current = GetStdHandle(STD_ERROR_HANDLE);
    if (current && current != INVALID_HANDLE_VALUE) {
        const DWORD type = GetFileType(current);
        if (type == FILE_TYPE_PIPE || type == FILE_TYPE_DISK) return;
    }
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) return;
    HANDLE out = CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_WRITE, nullptr,
                             OPEN_EXISTING, 0, nullptr);
    if (out == INVALID_HANDLE_VALUE) return;
    SetStdHandle(STD_ERROR_HANDLE, out);
    SetStdHandle(STD_OUTPUT_HANDLE, out);
}

static void log_line(const std::wstring& text) {
    if (!g_verbose.load(std::memory_order_relaxed)) return;

    HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
    if (err == nullptr || err == INVALID_HANDLE_VALUE) return;

    const std::wstring line = L"[cleaner] " + text + L"\r\n";
    DWORD written = 0;
    if (WriteConsoleW(err, line.c_str(), static_cast<DWORD>(line.size()), &written, nullptr))
        return;

    const int size = WideCharToMultiByte(CP_UTF8, 0, line.c_str(),
                                         static_cast<int>(line.size()),
                                         nullptr, 0, nullptr, nullptr);
    if (size <= 0) return;
    std::string narrow(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, line.c_str(), static_cast<int>(line.size()),
                        narrow.data(), size, nullptr, nullptr);
    WriteFile(err, narrow.data(), static_cast<DWORD>(narrow.size()), &written, nullptr);
}

static void log_line(const wchar_t* text) { log_line(std::wstring(text)); }

static void progress_tick() {
    g_progress.fetch_add(1, std::memory_order_relaxed);
}

// Idempotent, shared by the CLI and the async path. The old fixed 30s timer
// could fire in the middle of a slow pass (or pass entirely while the process
// was still shutting down); this one only fires after 60s without a single
// completed step, which is a genuine hang and not a slow disk.
static void ensure_watchdog() {
    if (g_watchdog_started.exchange(true)) return;

    std::thread([] {
        unsigned long long seen = g_progress.load(std::memory_order_relaxed);
        ULONGLONG last_change = GetTickCount64();
        for (;;) {
            Sleep(500);
            const unsigned long long now = g_progress.load(std::memory_order_relaxed);
            if (now != seen) {
                seen = now;
                last_change = GetTickCount64();
                continue;
            }
            if (GetTickCount64() - last_change < 60000) continue;
            log_line(L"watchdog: 60s sem progresso, encerrando");
            ExitProcess(0);
        }
    }).detach();
}

static bool is_elevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION elevation{};
    DWORD size = sizeof(elevation);
    const bool elevated =
        GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size) != 0 &&
        elevation.TokenIsElevated != 0;
    CloseHandle(token);
    return elevated;
}

static std::wstring self_path() {
    wchar_t buffer[MAX_PATH]{};
    GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    std::wstring path(buffer);
    for (auto& c : path) c = static_cast<wchar_t>(towlower(c));
    return path;
}

static std::wstring self_name() {
    const std::wstring path = self_path();
    const size_t slash = path.find_last_of(L"\\/");
    std::wstring name = slash == std::wstring::npos ? path : path.substr(slash + 1);
    const size_t dot = name.rfind(L'.');
    if (dot != std::wstring::npos) name = name.substr(0, dot);
    return name;
}

static bool contains_any(const std::wstring& text, std::initializer_list<const wchar_t*> needles) {
    for (const wchar_t* needle : needles)
        if (text.find(needle) != std::wstring::npos) return true;
    return false;
}

// Token-aware match: "loader.exe" must not match "downloader.exe". Registry and
// MRU sweeps would otherwise delete unrelated entries.
static bool contains_token(const std::wstring& haystack, const std::wstring& needle) {
    if (needle.empty() || haystack.size() < needle.size()) return false;
    size_t pos = haystack.find(needle);
    while (pos != std::wstring::npos) {
        const bool left_ok = pos == 0 || !iswalnum(haystack[pos - 1]);
        const size_t end = pos + needle.size();
        const bool right_ok = end >= haystack.size() || !iswalnum(haystack[end]);
        if (left_ok && right_ok) return true;
        pos = haystack.find(needle, pos + 1);
    }
    return false;
}

// File-name stems used by this tool and by the builds it replaces. The match
// is token aware: "loader_1721518693.exe" and "fivem_esp_x.exe" match, while
// "downloader.exe" does not.
static bool matches_tool_family(const std::wstring& lower) {
    for (const wchar_t* token : { L"loader", L"fivem_esp", L"phantom", L"cypher", L"openiv" })
        if (contains_token(lower, token)) return true;
    return lower.find(L"ph_") != std::wstring::npos;
}

// Forward declaration: clean_amcache runs before the definition below.
static void delete_if_exists(const std::wstring& path);

// ProcessPrefetchTraceFlags (0x3A): stops Windows from writing a .pf entry for
// this run in the first place.
void disable_prefetch_for_current_process() {
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) return;
    using nt_set_information_process = NTSTATUS(__stdcall*)(HANDLE, PROCESS_INFORMATION_CLASS, PVOID, ULONG);
    auto set_information = reinterpret_cast<nt_set_information_process>(
        GetProcAddress(ntdll, "NtSetInformationProcess"));
    if (!set_information) return;
    BOOL disable = TRUE;
    set_information(GetCurrentProcess(), static_cast<PROCESS_INFORMATION_CLASS>(0x3A),
                    &disable, sizeof(disable));
}

// Prefetch bodies store paths as UTF-16LE, and the trace file name may be
// random. Scan the raw buffer as wide chars so the own path still matches.
static bool contains_wide_ci(const std::string& raw, const std::wstring& needle) {
    if (needle.empty() || raw.size() < needle.size() * 2) return false;
    const size_t chars = raw.size() / 2;
    for (size_t start = 0; start + needle.size() <= chars; ++start) {
        size_t matched = 0;
        while (matched < needle.size()) {
            const size_t offset = (start + matched) * 2;
            const auto lo = static_cast<unsigned char>(raw[offset]);
            const auto hi = static_cast<unsigned char>(raw[offset + 1]);
            const auto wide = static_cast<wchar_t>(static_cast<unsigned>(lo) | (static_cast<unsigned>(hi) << 8));
            if (towlower(wide) != towlower(needle[matched])) break;
            ++matched;
        }
        if (matched == needle.size()) return true;
    }
    return false;
}

static void clean_prefetch_entries() {
    wchar_t windows_dir[MAX_PATH]{};
    GetWindowsDirectoryW(windows_dir, MAX_PATH);
    const std::wstring prefetch = std::wstring(windows_dir) + L"\\Prefetch";

    std::wstring upper = self_name();
    for (auto& c : upper) c = static_cast<wchar_t>(towupper(c));

    const std::wstring patterns[] = {
        upper + L"*.pf", L"PHANTOM*.pf", L"CYPHER*.pf", L"CYPHERBYPAS*.pf",
        L"PH_*.pf", L"CHEAT*.pf", L"OPENIV*.pf", L"RUNTIMEBROKER*.pf",
        L"LOADER*.pf", L"LOADER_*.pf"
    };
    for (const auto& pattern : patterns) {
        WIN32_FIND_DATAW entry{};
        HANDLE find = FindFirstFileW((prefetch + L"\\" + pattern).c_str(), &entry);
        if (find == INVALID_HANDLE_VALUE) continue;
        do {
            const std::wstring full = prefetch + L"\\" + entry.cFileName;
            SetFileAttributesW(full.c_str(), FILE_ATTRIBUTE_NORMAL);
            if (DeleteFileW(full.c_str())) note_removed();
        } while (FindNextFileW(find, &entry));
        FindClose(find);
    }

    // Second pass: the trace file name may be random, the body still names us.
    WIN32_FIND_DATAW entry{};
    HANDLE find = FindFirstFileW((prefetch + L"\\*.pf").c_str(), &entry);
    if (find == INVALID_HANDLE_VALUE) return;
    const std::wstring path = self_path();
    const std::wstring name = self_name();
    do {
        const std::wstring full = prefetch + L"\\" + entry.cFileName;
        HANDLE file = CreateFileW(full.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                  OPEN_EXISTING, 0, nullptr);
        if (file == INVALID_HANDLE_VALUE) continue;
        uint8_t buffer[8192]{};
        DWORD read = 0;
        SetFilePointer(file, 0x80, nullptr, FILE_BEGIN);
        ReadFile(file, buffer, sizeof(buffer) - 1, &read, nullptr);
        CloseHandle(file);
        std::string data(reinterpret_cast<char*>(buffer), read);
        for (auto& c : data) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        if (data.find("phantom") != std::string::npos ||
            data.find("cypher") != std::string::npos ||
            data.find("openiv") != std::string::npos ||
            data.find("loader") != std::string::npos ||
            contains_wide_ci(data, path) ||
            contains_wide_ci(data, name)) {
            SetFileAttributesW(full.c_str(), FILE_ATTRIBUTE_NORMAL);
            DeleteFileW(full.c_str());
        }
    } while (FindNextFileW(find, &entry));
    FindClose(find);
}

// Background Activity Moderator: per-user execution history under SYSTEM.
static void clean_bam_entries() {
    // BAM is ACL-protected: a plain administrator token is denied (error 5).
    // Opening it needs SeBackupPrivilege and SeRestorePrivilege enabled, so the
    // result of the adjustment decides which open path to try first.
    const bool backup = enable_privilege(L"SeBackupPrivilege");
    const bool restore = enable_privilege(L"SeRestorePrivilege");
    enable_privilege(L"SeTakeOwnershipPrivilege");

    const wchar_t* key_path = L"SYSTEM\\CurrentControlSet\\Services\\bam\\State\\UserSettings";
    HKEY root = nullptr;
    LSTATUS status = ERROR_ACCESS_DENIED;
    if (backup && restore) {
        status = RegOpenKeyExW(HKEY_LOCAL_MACHINE, key_path, REG_OPTION_BACKUP_RESTORE,
                               KEY_ALL_ACCESS | KEY_WOW64_64KEY, &root);
    }
    if (!root) {
        status = RegOpenKeyExW(HKEY_LOCAL_MACHINE, key_path, 0,
                               KEY_ALL_ACCESS | KEY_WOW64_64KEY, &root);
    }
    if (!root) {
        note_detail(std::wstring(L"chave inacessivel, erro ") +
                    std::to_wstring(static_cast<long>(status)) +
                    (backup && restore ? L"" : L", privilegios indisponiveis"));
        return;
    }

    const std::wstring name = self_name();
    unsigned long unreadable = 0;
    unsigned long failed = 0;
    for (DWORD index = 0;; ++index) {
        wchar_t sub_key[256];
        DWORD sub_length = 256;
        if (RegEnumKeyExW(root, index, sub_key, &sub_length, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
            break;

        HKEY user_key = nullptr;
        LSTATUS opened = RegOpenKeyExW(root, sub_key, REG_OPTION_BACKUP_RESTORE,
                                       KEY_ALL_ACCESS | KEY_WOW64_64KEY, &user_key);
        if (opened != ERROR_SUCCESS)
            opened = RegOpenKeyExW(root, sub_key, 0, KEY_ALL_ACCESS | KEY_WOW64_64KEY, &user_key);
        if (opened != ERROR_SUCCESS) {
            ++unreadable;
            continue;
        }

        std::vector<std::wstring> to_delete;
        for (DWORD value_index = 0;; ++value_index) {
            wchar_t value_name[512];
            DWORD value_length = 512;
            if (RegEnumValueW(user_key, value_index, value_name, &value_length,
                              nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
                break;
            std::wstring lower(value_name);
            for (auto& c : lower) c = static_cast<wchar_t>(towlower(c));
            if (matches_tool_family(lower) ||
                (name.size() >= 4 && contains_token(lower, name)))
                to_delete.push_back(value_name);
        }
        for (const auto& value : to_delete) {
            if (RegDeleteValueW(user_key, value.c_str()) == ERROR_SUCCESS) note_removed();
            else ++failed;
        }
        RegCloseKey(user_key);
    }
    RegCloseKey(root);

    if (unreadable) note_detail(std::to_wstring(unreadable) + L" subchaves ilegiveis");
    if (failed) note_detail(std::to_wstring(failed) + L" valores recusados");
}

static void clean_temp_dlls() {
    wchar_t temp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temp);
    const std::wstring dir(temp);
    for (const wchar_t* pattern : { L"ph_*.dll", L"ph_*.tmp", L"ph_*.tmp.dll",
                                    L"loader*.dll", L"loader*.tmp", L"loader*.tmp.dll" }) {
        WIN32_FIND_DATAW entry{};
        HANDLE find = FindFirstFileW((dir + pattern).c_str(), &entry);
        if (find == INVALID_HANDLE_VALUE) continue;
        do {
            DeleteFileW((dir + entry.cFileName).c_str());
        } while (FindNextFileW(find, &entry));
        FindClose(find);
    }

    wchar_t module[MAX_PATH]{};
    GetModuleFileNameW(nullptr, module, MAX_PATH);
    const std::wstring recipe = std::wstring(module) + L".recipe";
    SetFileAttributesW(recipe.c_str(), FILE_ATTRIBUTE_NORMAL);
    DeleteFileW(recipe.c_str());
}

static void clean_event_logs() {
    for (const wchar_t* name : { L"Application", L"System", L"Security" }) {
        HANDLE log = OpenEventLogW(nullptr, name);
        if (!log) continue;
        ClearEventLogW(log, nullptr);
        CloseEventLog(log);
    }
}

static void clean_usn_journal() {
    HANDLE volume = CreateFileW(L"\\\\.\\C:", GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                OPEN_EXISTING, 0, nullptr);
    if (volume == INVALID_HANDLE_VALUE) return;

    USN_JOURNAL_DATA journal{};
    DWORD bytes = 0;
    if (DeviceIoControl(volume, FSCTL_QUERY_USN_JOURNAL, nullptr, 0,
                        &journal, sizeof(journal), &bytes, nullptr)) {
        DELETE_USN_JOURNAL_DATA remove{};
        remove.UsnJournalID = journal.UsnJournalID;
        remove.DeleteFlags = USN_DELETE_FLAG_DELETE | USN_DELETE_FLAG_NOTIFY;
        DeviceIoControl(volume, FSCTL_DELETE_USN_JOURNAL, &remove, sizeof(remove),
                        nullptr, 0, &bytes, nullptr);

        CREATE_USN_JOURNAL_DATA create{};
        create.MaximumSize = 32 * 1024 * 1024;
        create.AllocationDelta = 4 * 1024 * 1024;
        DeviceIoControl(volume, FSCTL_CREATE_USN_JOURNAL, &create, sizeof(create),
                        nullptr, 0, &bytes, nullptr);
    }
    CloseHandle(volume);
}

static bool run_command_hidden_a(std::string command, DWORD* exit_code = nullptr) {
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessA(nullptr, command.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return false;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    if (exit_code) *exit_code = code;
    return true;
}

// torta.cpp: roll the clock back 8d12h so the recreated journal does not line
// up with the real timeline, delete the journal, optionally seed a dummy file,
// restore the clock, then clear the logs through wevtutil.
static bool clean_usn_journal_with_rollback(bool seed_dummy_file) {
    enable_privilege(L"SeSystemtimePrivilege");

    SYSTEMTIME original{};
    GetSystemTime(&original);

    SYSTEMTIME shifted{};
    GetSystemTime(&shifted);
    FILETIME ft{};
    SystemTimeToFileTime(&shifted, &ft);
    ULARGE_INTEGER ticks{};
    ticks.LowPart = ft.dwLowDateTime;
    ticks.HighPart = ft.dwHighDateTime;
    ticks.QuadPart -= (8ULL * 24 * 60 * 60 + 12ULL * 60 * 60) * 10000000ULL;
    ft.dwLowDateTime = ticks.LowPart;
    ft.dwHighDateTime = ticks.HighPart;
    FileTimeToSystemTime(&ft, &shifted);
    if (!SetSystemTime(&shifted)) return false;

    DWORD exit_code = 0;
    run_command_hidden_a("fsutil usn deletejournal /d /n c:", &exit_code);

    if (seed_dummy_file) {
        wchar_t temp[MAX_PATH]{};
        if (GetTempPathW(MAX_PATH, temp)) {
            const std::wstring dummy = std::wstring(temp) + L"yb_seed.tmp";
            HANDLE file = CreateFileW(dummy.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                      FILE_ATTRIBUTE_TEMPORARY, nullptr);
            if (file != INVALID_HANDLE_VALUE) {
                DWORD written = 0;
                WriteFile(file, "Dummy data", 10, &written, nullptr);
                CloseHandle(file);
                DeleteFileW(dummy.c_str());
            }
        }
        Sleep(1000);
    }

    SetSystemTime(&original);

    for (const char* channel : { "Application", "Security", "System", "Setup" }) {
        run_command_hidden_a(std::string("wevtutil cl ") + channel);
    }
    return exit_code == 0;
}

static void clean_recent_docs() {
    wchar_t recent[MAX_PATH]{};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_RECENT, nullptr, SHGFP_TYPE_CURRENT, recent)))
        return;
    for (const wchar_t* pattern : { L"phantom*", L"cheat*", L"ph_*", L"cypher*", L"loader*" }) {
        WIN32_FIND_DATAW entry{};
        HANDLE find = FindFirstFileW((std::wstring(recent) + L"\\" + pattern).c_str(), &entry);
        if (find == INVALID_HANDLE_VALUE) continue;
        do {
            DeleteFileW((std::wstring(recent) + L"\\" + entry.cFileName).c_str());
        } while (FindNextFileW(find, &entry));
        FindClose(find);
    }
}

static void delete_files_in_folder(const std::wstring& folder) {
    WIN32_FIND_DATAW entry{};
    HANDLE find = FindFirstFileW((folder + L"\\*").c_str(), &entry);
    if (find == INVALID_HANDLE_VALUE) return;
    do {
        if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        DeleteFileW((folder + L"\\" + entry.cFileName).c_str());
    } while (FindNextFileW(find, &entry));
    FindClose(find);
}

static void clean_jump_lists() {
    PWSTR app_data = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &app_data)))
        return;
    const std::wstring recent = std::wstring(app_data) + L"\\Microsoft\\Windows\\Recent";
    CoTaskMemFree(app_data);
    delete_files_in_folder(recent + L"\\CustomDestinations");
    delete_files_in_folder(recent + L"\\AutomaticDestinations");
}

static void clean_shim_cache() {
    enable_privilege(L"SeBackupPrivilege");
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
            L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\AppCompatCache",
            0, KEY_ALL_ACCESS | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS)
        return;
    RegDeleteValueW(key, L"AppCompatCache");
    RegCloseKey(key);
}

static void clean_user_assist() {
    HKEY users = nullptr;
    if (RegOpenKeyExW(HKEY_USERS, nullptr, 0, KEY_ENUMERATE_SUB_KEYS, &users) != ERROR_SUCCESS)
        return;

    // ROT13 of phantom, cypher, ph_, openiv, runtimebroker, loader.
    const wchar_t* rot13[] = { L"cuncgbz", L"plcure", L"cu_", L"bcravie", L"ehagvzroebyxre", L"ybnqre" };
    for (DWORD sid_index = 0;; ++sid_index) {
        wchar_t sid[256];
        DWORD sid_length = 256;
        if (RegEnumKeyExW(users, sid_index, sid, &sid_length, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
            break;

        const std::wstring base = std::wstring(sid) +
            L"\\Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\UserAssist";
        HKEY assist = nullptr;
        if (RegOpenKeyExW(users, base.c_str(), 0, KEY_ENUMERATE_SUB_KEYS, &assist) != ERROR_SUCCESS)
            continue;

        for (DWORD guid_index = 0;; ++guid_index) {
            wchar_t guid[256];
            DWORD guid_length = 256;
            if (RegEnumKeyExW(assist, guid_index, guid, &guid_length, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
                break;

            const std::wstring count_path = std::wstring(guid) + L"\\Count";
            HKEY count = nullptr;
            if (RegOpenKeyExW(assist, count_path.c_str(), 0, KEY_ALL_ACCESS, &count) != ERROR_SUCCESS)
                continue;

            std::vector<std::wstring> to_delete;
            for (DWORD value_index = 0;; ++value_index) {
                wchar_t value[1024];
                DWORD value_length = 1024;
                if (RegEnumValueW(count, value_index, value, &value_length,
                                  nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
                    break;
                std::wstring lower(value);
                for (auto& c : lower) c = static_cast<wchar_t>(towlower(c));
                for (const wchar_t* needle : rot13) {
                    if (lower.find(needle) != std::wstring::npos) {
                        to_delete.push_back(value);
                        break;
                    }
                }
            }
            for (const auto& name : to_delete) RegDeleteValueW(count, name.c_str());
            RegCloseKey(count);
        }
        RegCloseKey(assist);
    }
    RegCloseKey(users);
}

static void clean_amcache() {
    enable_privilege(L"SeBackupPrivilege");
    enable_privilege(L"SeRestorePrivilege");

    wchar_t windows_dir[MAX_PATH]{};
    GetWindowsDirectoryW(windows_dir, MAX_PATH);
    const std::wstring hive = std::wstring(windows_dir) + L"\\appcompat\\Programs\\Amcache.hve";
    const wchar_t* mount = L"AMCACHE_TMP";
    RegUnLoadKeyW(HKEY_LOCAL_MACHINE, mount);
    if (RegLoadKeyW(HKEY_LOCAL_MACHINE, mount, hive.c_str()) != ERROR_SUCCESS) return;

    const wchar_t* roots[] = {
        L"AMCACHE_TMP\\Root\\File",
        L"AMCACHE_TMP\\Root\\InventoryApplicationFile"
    };

    for (const wchar_t* root_path : roots) {
        HKEY root = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, root_path, 0,
                          KEY_ALL_ACCESS | KEY_WOW64_64KEY, &root) != ERROR_SUCCESS)
            continue;

        std::vector<std::wstring> containers;
        for (DWORD index = 0;; ++index) {
            wchar_t container[256];
            DWORD length = 256;
            if (RegEnumKeyExW(root, index, container, &length, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
                break;
            containers.push_back(container);
        }

        for (const auto& container : containers) {
            HKEY container_key = nullptr;
            if (RegOpenKeyExW(root, container.c_str(), 0, KEY_ALL_ACCESS, &container_key) != ERROR_SUCCESS)
                continue;

            std::vector<std::wstring> to_delete;
            for (DWORD index = 0;; ++index) {
                wchar_t entry[256];
                DWORD length = 256;
                if (RegEnumKeyExW(container_key, index, entry, &length, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
                    break;

                HKEY entry_key = nullptr;
                if (RegOpenKeyExW(container_key, entry, 0, KEY_READ, &entry_key) != ERROR_SUCCESS)
                    continue;

                wchar_t value[1024]{};
                DWORD value_size = sizeof(value);
                if (RegQueryValueExW(entry_key, L"FullPath", nullptr, nullptr,
                                     reinterpret_cast<LPBYTE>(value), &value_size) != ERROR_SUCCESS) {
                    value_size = sizeof(value);
                    RegQueryValueExW(entry_key, nullptr, nullptr, nullptr,
                                     reinterpret_cast<LPBYTE>(value), &value_size);
                }
                RegCloseKey(entry_key);

                std::wstring lower(value);
                for (auto& c : lower) c = static_cast<wchar_t>(towlower(c));
                if (contains_any(lower, { L"phantom", L"cypher", L"ph_", L"cheat", L"openiv", L"runtimebroker", L"loader" }))
                    to_delete.push_back(entry);
            }
            for (const auto& name : to_delete) RegDeleteKeyW(container_key, name.c_str());
            RegCloseKey(container_key);
        }
        RegCloseKey(root);
    }
    RegFlushKey(HKEY_LOCAL_MACHINE);
    RegUnLoadKeyW(HKEY_LOCAL_MACHINE, mount);

    // The transaction logs keep a replayable copy of the deleted keys.
    delete_if_exists(hive + L".LOG1");
    delete_if_exists(hive + L".LOG2");
    delete_if_exists(hive + L".regtrans-ms");
}

static DWORD service_process_id(const wchar_t* service_name) {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return 0;

    DWORD pid = 0;
    SC_HANDLE service = OpenServiceW(scm, service_name, SERVICE_QUERY_STATUS);
    if (service) {
        SERVICE_STATUS_PROCESS status{};
        DWORD needed = 0;
        if (QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO,
                                 reinterpret_cast<LPBYTE>(&status), sizeof(status), &needed))
            pid = status.dwProcessId;
        CloseServiceHandle(service);
    }
    CloseServiceHandle(scm);
    return pid;
}

static DWORD process_id_by_name(const wchar_t* process_name) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    DWORD pid = 0;
    if (Process32FirstW(snapshot, &entry)) do {
        if (_wcsicmp(entry.szExeFile, process_name) == 0) {
            pid = entry.th32ProcessID;
            break;
        }
    } while (Process32NextW(snapshot, &entry));
    CloseHandle(snapshot);
    return pid;
}

static int wipe_pattern_in_process(HANDLE process, const std::vector<uint8_t>& needle) {
    if (needle.empty()) return 0;

    int wiped = 0;
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    uintptr_t address = reinterpret_cast<uintptr_t>(info.lpMinimumApplicationAddress);
    const uintptr_t max_address = reinterpret_cast<uintptr_t>(info.lpMaximumApplicationAddress);
    MEMORY_BASIC_INFORMATION region{};

    while (address < max_address &&
           VirtualQueryEx(process, reinterpret_cast<LPCVOID>(address), &region, sizeof(region))) {
        const uintptr_t base = reinterpret_cast<uintptr_t>(region.BaseAddress);
        const size_t size = region.RegionSize;
        address = base + size;

        if (region.State != MEM_COMMIT || region.Protect == PAGE_NOACCESS || (region.Protect & PAGE_GUARD))
            continue;
        if (size < needle.size() || size > 256ull * 1024ull * 1024ull)
            continue;

        std::vector<uint8_t> buffer(size);
        SIZE_T read = 0;
        if (!ReadProcessMemory(process, reinterpret_cast<LPCVOID>(base), buffer.data(), size, &read))
            continue;
        if (read < needle.size())
            continue;

        const std::vector<uint8_t> zeros(needle.size(), 0);
        for (size_t offset = 0; offset + needle.size() <= read; ++offset) {
            if (std::memcmp(buffer.data() + offset, needle.data(), needle.size()) != 0)
                continue;
            SIZE_T written = 0;
            if (WriteProcessMemory(process, reinterpret_cast<LPVOID>(base + offset),
                                   zeros.data(), zeros.size(), &written))
                ++wiped;
            offset += needle.size() - 1;
        }
    }
    return wiped;
}

// Explorer keeps the path in its history, the indexing/cache services keep it
// in their heaps. Both are zeroed here.
static void wipe_self_strings() {
    wchar_t path[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, path, MAX_PATH) == 0) return;

    enable_privilege(L"SeDebugPrivilege");

    const std::wstring wide_path(path);
    const size_t slash = wide_path.find_last_of(L"\\/");
    const std::wstring wide_name = slash == std::wstring::npos ? wide_path : wide_path.substr(slash + 1);

    std::vector<std::vector<uint8_t>> needles;
    auto add_needle = [&needles](const std::wstring& text) {
        if (text.size() < 8) return;
        const uint8_t* bytes = reinterpret_cast<const uint8_t*>(text.c_str());
        needles.emplace_back(bytes, bytes + text.size() * sizeof(wchar_t));
    };
    add_needle(wide_path);
    add_needle(wide_name);

    struct target { const wchar_t* name; bool service; };
    const target targets[] = {
        { L"explorer.exe", false },
        { L"Dnscache", true },
        { L"Dps", true },
        { L"Pcasvc", true }
    };

    const DWORD self = GetCurrentProcessId();
    for (const auto& entry : targets) {
        const DWORD pid = entry.service ? service_process_id(entry.name) : process_id_by_name(entry.name);
        if (pid == 0 || pid == self) continue;

        HANDLE process = OpenProcess(PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_QUERY_INFORMATION,
                                     FALSE, pid);
        if (!process) continue;
        for (const auto& needle : needles) wipe_pattern_in_process(process, needle);
        CloseHandle(process);
    }
}

static void delete_if_exists(const std::wstring& path) {
    SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
    if (DeleteFileW(path.c_str())) note_removed();
}

static void delete_temp_pattern(const std::wstring& dir, const wchar_t* pattern) {
    WIN32_FIND_DATAW entry{};
    HANDLE find = FindFirstFileW((dir + pattern).c_str(), &entry);
    if (find == INVALID_HANDLE_VALUE) return;
    do {
        if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        delete_if_exists(dir + entry.cFileName);
    } while (FindNextFileW(find, &entry));
    FindClose(find);
}

static void delete_tree(const std::wstring& path) {
    WIN32_FIND_DATAW entry{};
    HANDLE find = FindFirstFileW((path + L"\\*").c_str(), &entry);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(entry.cFileName, L".") == 0 || wcscmp(entry.cFileName, L"..") == 0) continue;
            const std::wstring child = path + L"\\" + entry.cFileName;
            if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                delete_tree(child);
            } else {
                SetFileAttributesW(child.c_str(), FILE_ATTRIBUTE_NORMAL);
                DeleteFileW(child.c_str());
            }
        } while (FindNextFileW(find, &entry));
        FindClose(find);
    }
    RemoveDirectoryW(path.c_str());
}

// Report.wer is either plain text or UTF-16; both encodings are scanned.
static bool file_mentions(const std::wstring& path, const std::wstring& wide_needle,
                          const std::string& narrow_needle) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;

    std::vector<uint8_t> data;
    uint8_t chunk[8192];
    DWORD read = 0;
    while (data.size() < 512 * 1024 &&
           ReadFile(file, chunk, sizeof(chunk), &read, nullptr) && read > 0)
        data.insert(data.end(), chunk, chunk + read);
    CloseHandle(file);
    if (data.empty()) return false;

    if (!narrow_needle.empty()) {
        std::string lower(data.begin(), data.end());
        for (auto& c : lower) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        if (lower.find(narrow_needle) != std::string::npos) return true;
    }

    const size_t width = wide_needle.size() * sizeof(wchar_t);
    if (width == 0 || data.size() < width) return false;
    for (size_t i = 0; i + width <= data.size(); i += 2) {
        size_t j = 0;
        for (; j < wide_needle.size(); ++j) {
            const wchar_t value = static_cast<wchar_t>(data[i + j * 2] |
                                                       (data[i + j * 2 + 1] << 8));
            if (static_cast<wchar_t>(towlower(value)) != wide_needle[j]) break;
        }
        if (j == wide_needle.size()) return true;
    }
    return false;
}

// "Mark of the Web": the alternate stream that records the download origin.
static void strip_zone_identifier(const std::wstring& path) {
    const std::wstring stream = path + L":Zone.Identifier";
    SetFileAttributesW(stream.c_str(), FILE_ATTRIBUTE_NORMAL);
    DeleteFileW(stream.c_str());
}

static bool value_has_token(HKEY key, const wchar_t* name,
                            const std::wstring& path_needle,
                            const std::wstring& name_needle,
                            bool family = false) {
    DWORD type = 0, size = 0;
    if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &size) != ERROR_SUCCESS)
        return false;
    if (size == 0 || size > (1u << 20)) return false;

    std::vector<BYTE> data(size);
    if (RegQueryValueExW(key, name, nullptr, &type, data.data(), &size) != ERROR_SUCCESS)
        return false;

    if (type == REG_SZ || type == REG_EXPAND_SZ) {
        std::wstring text(reinterpret_cast<const wchar_t*>(data.data()), size / sizeof(wchar_t));
        for (auto& c : text) c = static_cast<wchar_t>(towlower(c));
        return contains_token(text, path_needle) || contains_token(text, name_needle) ||
               (family && matches_tool_family(text));
    }
    if (type == REG_BINARY) {
        const size_t path_width = path_needle.size() * sizeof(wchar_t);
        const size_t name_width = name_needle.size() * sizeof(wchar_t);
        if (path_width) {
            const auto* needle = reinterpret_cast<const uint8_t*>(path_needle.c_str());
            for (size_t i = 0; i + path_width <= data.size(); ++i)
                if (std::memcmp(data.data() + i, needle, path_width) == 0) return true;
        }
        if (name_width) {
            const auto* needle = reinterpret_cast<const uint8_t*>(name_needle.c_str());
            for (size_t i = 0; i + name_width <= data.size(); ++i)
                if (std::memcmp(data.data() + i, needle, name_width) == 0) return true;
        }
    }
    return false;
}

// Deletes values and, up to depth, subkeys whose name or payload mentions the
// binary. PIDL values (ComDlg32) are binary, hence the byte scan. With family
// set, old copies of the tool under another name (loader_<random>.exe) match too.
static void scrub_key_matches(HKEY root, const std::wstring& subkey,
                              const std::wstring& path_needle,
                              const std::wstring& name_needle,
                              int depth, bool family = false) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, subkey.c_str(), 0,
                      KEY_READ | KEY_WRITE | KEY_ENUMERATE_SUB_KEYS, &key) != ERROR_SUCCESS)
        return;

    std::vector<std::wstring> to_delete;
    for (DWORD index = 0;; ++index) {
        wchar_t name[1024];
        DWORD length = 1024;
        if (RegEnumValueW(key, index, name, &length, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
            break;

        std::wstring lower(name);
        for (auto& c : lower) c = static_cast<wchar_t>(towlower(c));

        if (contains_token(lower, path_needle) || contains_token(lower, name_needle) ||
            (family && matches_tool_family(lower)) ||
            value_has_token(key, name, path_needle, name_needle, family))
            to_delete.push_back(name);
    }
    for (const auto& name : to_delete)
        if (RegDeleteValueW(key, name.c_str()) == ERROR_SUCCESS) note_removed();

    if (depth > 0) {
        std::vector<std::wstring> children;
        for (DWORD index = 0;; ++index) {
            wchar_t child[512];
            DWORD length = 512;
            if (RegEnumKeyExW(key, index, child, &length, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
                break;
            children.push_back(child);
        }
        for (const auto& child : children) {
            std::wstring lower(child);
            for (auto& c : lower) c = static_cast<wchar_t>(towlower(c));
            if (contains_token(lower, path_needle) || contains_token(lower, name_needle) ||
                (family && matches_tool_family(lower))) {
                if (RegDeleteTreeW(key, child.c_str()) == ERROR_SUCCESS) note_removed();
                continue;
            }
            scrub_key_matches(key, child, path_needle, name_needle, depth - 1, family);
        }
    }
    RegCloseKey(key);
}

static std::wstring read_reg_string(HKEY root, const std::wstring& key, const wchar_t* value) {
    std::wstring result;
    HKEY handle = nullptr;
    if (RegOpenKeyExW(root, key.c_str(), 0, KEY_QUERY_VALUE, &handle) != ERROR_SUCCESS)
        return result;
    wchar_t buffer[128]{};
    DWORD size = sizeof(buffer);
    if (RegQueryValueExW(handle, value, nullptr, nullptr,
                         reinterpret_cast<BYTE*>(buffer), &size) == ERROR_SUCCESS)
        result = buffer;
    RegCloseKey(handle);
    return result;
}

static void remove_launcher_task(const wchar_t* task_name) {
    wchar_t system_dir[MAX_PATH]{};
    GetSystemDirectoryW(system_dir, MAX_PATH);

    std::wstring arguments = std::wstring(L"/delete /tn \"") + task_name + L"\" /f";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    if (CreateProcessW((std::wstring(system_dir) + L"\\schtasks.exe").c_str(), arguments.data(),
                       nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 5000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }

    // schtasks can be blocked; remove the orphaned GUID and XML by hand.
    const std::wstring task_key =
        std::wstring(L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Schedule\\TaskCache\\Tree\\") + task_name;
    const std::wstring task_id = read_reg_string(HKEY_LOCAL_MACHINE, task_key, L"Id");
    if (!task_id.empty())
        RegDeleteTreeW(HKEY_LOCAL_MACHINE,
            (std::wstring(L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Schedule\\TaskCache\\Tasks\\") + task_id).c_str());
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, task_key.c_str());

    wchar_t windows_dir[MAX_PATH]{};
    GetWindowsDirectoryW(windows_dir, MAX_PATH);
    delete_if_exists(std::wstring(windows_dir) + L"\\System32\\Tasks\\" + task_name);
}

static void clean_launcher_traces() {
    for (const wchar_t* task : { L"NVOverlayHost", L"NvOverlayHost2", L"NvOverlayTask" })
        remove_launcher_task(task);

    HKEY run = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                      0, KEY_SET_VALUE, &run) == ERROR_SUCCESS) {
        RegDeleteValueW(run, L"NVOverlayHost");
        RegCloseKey(run);
    }

    wchar_t local_app_data[MAX_PATH]{};
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", local_app_data, MAX_PATH) && local_app_data[0]) {
        const std::wstring install = std::wstring(local_app_data) + L"\\YauBypass";
        delete_if_exists(install + L"\\boot.cmd");
        delete_if_exists(install + L"\\yaubypass.dat");
        RemoveDirectoryW(install.c_str());
    }

    wchar_t temp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temp);
    const std::wstring dir(temp);
    delete_if_exists(dir + L"y.cmd");
    delete_if_exists(dir + L"y.b64");
    delete_if_exists(dir + L"yau.cmd");
    delete_if_exists(dir + L"c.b64");
    delete_if_exists(dir + L"c.cfg");
    delete_if_exists(dir + L"l.cmd");
    delete_if_exists(dir + L"l.b64");
    delete_if_exists(dir + L"l.cfg");
    delete_if_exists(dir + L"boot.cmd");
    delete_if_exists(dir + L"loader.dat");
    delete_if_exists(dir + L"cl.ps1");
    delete_temp_pattern(dir, L"cl_*.ps1");
    RemoveDirectoryW((dir + L"wz").c_str());
    delete_temp_pattern(dir, L"nv_*.exe");
    delete_temp_pattern(dir, L"nv_*.tmp");
    delete_temp_pattern(dir, L"nv_*.b64");
    delete_temp_pattern(dir, L"yb_*.ok");
    delete_temp_pattern(dir, L"yb_elev_*.vbs");
    delete_temp_pattern(dir, L"loader_*.tmp");
    delete_temp_pattern(dir, L"loader_*.dll");
    delete_temp_pattern(dir, L"loader*.tmp");
    // Portable mode drops loader_<rand>.exe in %TEMP%; a crashed run leaves one
    // behind. The copy that is running right now fails here and is removed by
    // schedule_self_delete instead.
    delete_temp_pattern(dir, L"loader_*.exe");

    // Reset only the streamproof value the cheat sets.
    const std::wstring fts = L"SOFTWARE\\NVIDIA Corporation\\Global\\NvApp\\ShadowPlay\\FTS";
    HKEY shadow = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, fts.c_str(), 0, KEY_SET_VALUE, &shadow) == ERROR_SUCCESS) {
        RegDeleteValueW(shadow, L"{497B8458-4244-4EE6-BFEA-F3D2BA294F21}");
        RegCloseKey(shadow);
        RegDeleteKeyW(HKEY_LOCAL_MACHINE, fts.c_str());
    }
}

// Rename the running file into an alternate data stream, then mark it for
// deletion on close. Falls back to delete-on-reboot.
static bool schedule_self_delete() {
    wchar_t path[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, path, MAX_PATH) == 0) return false;

    // Drop the download mark before renaming; the base file disappears anyway,
    // but a crash between the two calls would leave the origin on disk.
    strip_zone_identifier(std::wstring(path));

    HANDLE file = CreateFileW(path, DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return MoveFileExW(path, nullptr, MOVEFILE_DELAY_UNTIL_REBOOT) != 0;

    static const wchar_t stream[] = L":wizard";
    const DWORD stream_bytes = static_cast<DWORD>(wcslen(stream) * sizeof(wchar_t));
    std::vector<uint8_t> buffer(sizeof(FILE_RENAME_INFO) + stream_bytes + sizeof(wchar_t), 0);
    auto* info = reinterpret_cast<FILE_RENAME_INFO*>(buffer.data());
    info->ReplaceIfExists = FALSE;
    info->RootDirectory = nullptr;
    info->FileNameLength = stream_bytes;
    std::memcpy(info->FileName, stream, stream_bytes);
    const bool renamed = SetFileInformationByHandle(file, FileRenameInfo, info,
                                                    static_cast<DWORD>(buffer.size())) != 0;

    FILE_DISPOSITION_INFO disposition{};
    disposition.DeleteFile = TRUE;
    const bool marked = SetFileInformationByHandle(file, FileDispositionInfo, &disposition,
                                                   sizeof(disposition)) != 0;
    CloseHandle(file);

    // A failed rename still leaves the delete-on-close mark, which is enough;
    // only report failure when neither worked.
    return renamed || marked;
}

static std::wstring escape_powershell_path(const std::wstring& value) {
    std::wstring escaped;
    escaped.reserve(value.size() + 16);
    for (wchar_t c : value) {
        if (c == L'\'') escaped += L"''";
        else escaped += c;
    }
    return escaped;
}

// ---------------------------------------------------------------------------
// Extra passes: artifacts the original Destruct never touches.
// ---------------------------------------------------------------------------

// Deep pass (on by default, "--no-deep" disables). SRUM stores per-app run
// counts, focus time and bytes sent; the database is only deletable while the
// Diagnostic Policy Service is stopped.
static std::atomic<bool> g_deep_clean{true};

static void reset_srum() {
    wchar_t windows_dir[MAX_PATH]{};
    GetWindowsDirectoryW(windows_dir, MAX_PATH);
    const std::wstring sru = std::wstring(windows_dir) + L"\\System32\\sru";

    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) {
        delete_if_exists(sru + L"\\SRU.log");
        return;
    }

    SC_HANDLE service = OpenServiceW(scm, L"DPS",
                                     SERVICE_STOP | SERVICE_START | SERVICE_QUERY_STATUS);
    if (service) {
        SERVICE_STATUS status{};
        ControlService(service, SERVICE_CONTROL_STOP, &status);
        for (int i = 0; i < 40; ++i) {
            SERVICE_STATUS_PROCESS info{};
            DWORD needed = 0;
            if (QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO,
                                     reinterpret_cast<LPBYTE>(&info), sizeof(info), &needed) &&
                info.dwCurrentState == SERVICE_STOPPED)
                break;
            Sleep(100);
        }
        for (const wchar_t* name : { L"\\SRUDB.dat", L"\\SRUDB.jfm", L"\\SRU.log" })
            delete_if_exists(sru + name);
        StartServiceW(service, 0, nullptr);
        CloseServiceHandle(service);
    }
    CloseServiceHandle(scm);
}

// The destruct spawns powershell, fsutil, takeown, icacls, schtasks and
// wevtutil. Each leaves its own .pf behind, so they are wiped as well.
static void clean_helper_prefetch() {
    wchar_t windows_dir[MAX_PATH]{};
    GetWindowsDirectoryW(windows_dir, MAX_PATH);
    const std::wstring prefetch = std::wstring(windows_dir) + L"\\Prefetch";

    const wchar_t* helpers[] = {
        L"POWERSHELL", L"CONHOST", L"CMD", L"FSUTIL", L"TAKEOWN",
        L"ICACLS", L"SCHTASKS", L"WEVTUTIL", L"CURL", L"IPCONFIG", L"WSCRIPT"
    };
    for (const wchar_t* helper : helpers) {
        WIN32_FIND_DATAW entry{};
        HANDLE find = FindFirstFileW((prefetch + L"\\" + helper + L"*.pf").c_str(), &entry);
        if (find == INVALID_HANDLE_VALUE) continue;
        do {
            const std::wstring full = prefetch + L"\\" + entry.cFileName;
            SetFileAttributesW(full.c_str(), FILE_ATTRIBUTE_NORMAL);
            if (DeleteFileW(full.c_str())) note_removed();
        } while (FindNextFileW(find, &entry));
        FindClose(find);
    }

    // Layout.ini keeps the recently touched paths and survives prefetch deletion.
    delete_if_exists(prefetch + L"\\Layout.ini");
}

static void clean_etw_traces() {
    wchar_t windows_dir[MAX_PATH]{};
    GetWindowsDirectoryW(windows_dir, MAX_PATH);
    const std::wstring dir(windows_dir);
    // WMI activity traces (SYSTEM-owned, best effort).
    delete_files_in_folder(dir + L"\\System32\\LogFiles\\WMI\\RtBackup");
    // ReadyBoot write traces.
    delete_files_in_folder(dir + L"\\Prefetch\\ReadyBoot");
}

// Channels the original never clears. TaskScheduler records the task deletion,
// PowerShell records the helper script's own command lines.
static void clean_operational_logs() {
    const char* channels[] = {
        "Microsoft-Windows-TaskScheduler/Operational",
        "Microsoft-Windows-PowerShell/Operational",
        "Windows PowerShell",
        "Microsoft-Windows-Application-Experience/Program-Inventory",
        "Microsoft-Windows-Application-Experience/Program-Telemetry",
        "Microsoft-Windows-Shell-Core/Operational",
        "Microsoft-Windows-WMI-Activity/Operational",
        "Microsoft-Windows-AppModel-Runtime/Admin",
        "Microsoft-Windows-Diagnosis-Scripted/Operational",
        "Microsoft-Windows-CodeIntegrity/Operational"
    };
    for (const char* channel : channels)
        run_command_hidden_a(std::string("wevtutil cl \"") + channel + "\"");
}

// Windows Timeline: one database per device account.
static void clean_timeline() {
    wchar_t local_app_data[MAX_PATH]{};
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", local_app_data, MAX_PATH) || !local_app_data[0])
        return;

    const std::wstring base = std::wstring(local_app_data) + L"\\ConnectedDevicesPlatform";
    WIN32_FIND_DATAW entry{};
    HANDLE find = FindFirstFileW((base + L"\\*").c_str(), &entry);
    if (find == INVALID_HANDLE_VALUE) return;
    do {
        if (!(entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (wcscmp(entry.cFileName, L".") == 0 || wcscmp(entry.cFileName, L"..") == 0) continue;
        const std::wstring dir = base + L"\\" + entry.cFileName;
        for (const wchar_t* file : { L"\\ActivitiesCache.db", L"\\ActivitiesCache.db-wal",
                                     L"\\ActivitiesCache.db-shm", L"\\ActivitiesCache.db-journal" })
            delete_if_exists(dir + file);
    } while (FindNextFileW(find, &entry));
    FindClose(find);
}

// Windows Error Reporting archives the exe path in Report.wer and a zipped copy.
static void clean_wer() {
    wchar_t local_app_data[MAX_PATH]{};
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", local_app_data, MAX_PATH) || !local_app_data[0])
        return;

    const std::wstring wide_needle = self_name() + L".exe";
    std::string narrow_needle;
    for (wchar_t c : wide_needle) narrow_needle.push_back(static_cast<char>(towlower(c)));

    const std::wstring base = std::wstring(local_app_data) + L"\\Microsoft\\Windows\\WER";
    for (const wchar_t* sub : { L"ReportArchive", L"ReportQueue", L"Temp" }) {
        const std::wstring dir = base + L"\\" + sub;
        WIN32_FIND_DATAW entry{};
        HANDLE find = FindFirstFileW((dir + L"\\*").c_str(), &entry);
        if (find == INVALID_HANDLE_VALUE) continue;

        std::vector<std::wstring> targets;
        do {
            if (!(entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            if (wcscmp(entry.cFileName, L".") == 0 || wcscmp(entry.cFileName, L"..") == 0) continue;
            const std::wstring child = dir + L"\\" + entry.cFileName;
            if (file_mentions(child + L"\\Report.wer", wide_needle, narrow_needle))
                targets.push_back(child);
        } while (FindNextFileW(find, &entry));
        FindClose(find);

        for (const auto& target : targets) delete_tree(target);
    }
}

static void clean_crash_dumps() {
    wchar_t local_app_data[MAX_PATH]{};
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", local_app_data, MAX_PATH) || !local_app_data[0])
        return;

    const std::wstring dir = std::wstring(local_app_data) + L"\\CrashDumps";
    WIN32_FIND_DATAW entry{};
    HANDLE find = FindFirstFileW((dir + L"\\" + self_name() + L".exe.*.dmp").c_str(), &entry);
    if (find == INVALID_HANDLE_VALUE) return;
    do {
        delete_if_exists(dir + L"\\" + entry.cFileName);
    } while (FindNextFileW(find, &entry));
    FindClose(find);
}

static void flush_dns() {
    run_command_hidden_a("ipconfig /flushdns");
}

// RecentDocs, MuiCache, RunMRU, AppSwitched, ComDlg32 PIDLs, Search history and
// the compatibility store all name the binary. None of them are in the original.
// Every sweep passes family=true: portable mode gives the binary a random name
// (loader_<digits>.exe), so an exact self-name match cannot see older copies.
static void clean_registry_history() {
    const std::wstring path = self_path();
    const std::wstring name = self_name() + L".exe";
    const std::wstring explorer = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer";

    scrub_key_matches(HKEY_CURRENT_USER, explorer + L"\\RunMRU", path, name, 0, true);
    scrub_key_matches(HKEY_CURRENT_USER, explorer + L"\\TypedPaths", path, name, 0, true);
    scrub_key_matches(HKEY_CURRENT_USER, explorer + L"\\RecentDocs", path, name, 1, true);
    scrub_key_matches(HKEY_CURRENT_USER, explorer + L"\\ComDlg32\\OpenSavePidlMRU", path, name, 1, true);
    scrub_key_matches(HKEY_CURRENT_USER, explorer + L"\\ComDlg32\\LastVisitedPidlMRU", path, name, 1, true);
    scrub_key_matches(HKEY_CURRENT_USER, explorer + L"\\FeatureUsage\\AppSwitched", path, name, 0, true);
    scrub_key_matches(HKEY_CURRENT_USER, explorer + L"\\FeatureUsage\\AppLaunch", path, name, 0, true);
    scrub_key_matches(HKEY_CURRENT_USER, explorer + L"\\FileExts\\.exe\\OpenWithList", path, name, 0, true);
    scrub_key_matches(HKEY_CURRENT_USER,
        L"Software\\Classes\\Local Settings\\Software\\Microsoft\\Windows\\Shell\\MuiCache", path, name, 0, true);
    scrub_key_matches(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows NT\\CurrentVersion\\AppCompatFlags\\Compatibility Assistant\\Store",
        path, name, 0, true);
    scrub_key_matches(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Search\\RecentApps", path, name, 1, true);
    scrub_key_matches(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Search\\WordWheelQuery", path, name, 0, true);
}

// Every step is best-effort: one failing pass must not abort the rest, and an
// exception escaping the thread lambda would call std::terminate mid-cleanup.
static void run_pass(const wchar_t* name, void (*pass)()) {
    log_line(std::wstring(name) + L": iniciando");
    g_pass_removed = 0;
    g_pass_counted = false;
    g_pass_detail.clear();
    try {
        pass();
        std::wstring summary = g_pass_counted
            ? std::wstring(name) + L": " + std::to_wstring(g_pass_removed) + L" removidos"
            : std::wstring(name) + L": ok";
        if (!g_pass_detail.empty()) summary += L" (" + g_pass_detail + L")";
        log_line(summary);
    } catch (...) {
        log_line(std::wstring(L"falhou: ") + name);
    }
    progress_tick();
}

static void clean_all(bool clock_rollback) {
    std::thread files([] {
        run_pass(L"prefetch", clean_prefetch_entries);
        run_pass(L"temp dlls", clean_temp_dlls);
        run_pass(L"recent docs", clean_recent_docs);
        run_pass(L"jump lists", clean_jump_lists);
        run_pass(L"event logs", clean_event_logs);
        run_pass(L"operational logs", clean_operational_logs);
        run_pass(L"timeline", clean_timeline);
        run_pass(L"wer", clean_wer);
        run_pass(L"crash dumps", clean_crash_dumps);
        run_pass(L"helper prefetch", clean_helper_prefetch);
        run_pass(L"etw traces", clean_etw_traces);
        run_pass(L"dns", flush_dns);
    });
    std::thread registry([] {
        run_pass(L"bam", clean_bam_entries);
        run_pass(L"shimcache", clean_shim_cache);
        run_pass(L"userassist", clean_user_assist);
        run_pass(L"amcache", clean_amcache);
        run_pass(L"registry history", clean_registry_history);
    });

    log_line(L"usn journal");
    try {
        if (clock_rollback) clean_usn_journal_with_rollback(true);
        else clean_usn_journal();
    } catch (...) {
        log_line(L"falhou: usn journal");
    }
    progress_tick();

    run_pass(L"self strings", wipe_self_strings);
    // After the string wipe: stopping DPS would hide its process from it.
    if (g_deep_clean.load()) run_pass(L"srum", reset_srum);
    run_pass(L"launcher traces", clean_launcher_traces);

    files.join();
    registry.join();

    // Second pass: wevtutil and schtasks above created their own prefetch.
    run_pass(L"helper prefetch (final)", clean_helper_prefetch);
}

// Deferred pass: the injected hooks are already gone by the time this runs.
// A detached script keeps working after this process is deleted.
[[noreturn]] static void run_and_exit(bool self_delete, bool clock_rollback) {
    const DWORD self_pid = GetCurrentProcessId();
    ensure_watchdog();

    const bool elevated = is_elevated();
    log_line(std::wstring(L"iniciando destruct, ") +
             (elevated ? L"elevado" : L"SEM elevacao"));
    if (!elevated)
        log_line(L"sem elevacao: prefetch, BAM, Amcache, USN e logs vao falhar");

    wchar_t module[MAX_PATH]{};
    GetModuleFileNameW(nullptr, module, MAX_PATH);
    const std::wstring exe_path(module);
    std::wstring exe_name = exe_path.substr(exe_path.find_last_of(L"\\/") + 1);
    for (auto& c : exe_name) c = static_cast<wchar_t>(towupper(c));
    const size_t dot = exe_name.rfind(L'.');
    if (dot != std::wstring::npos) exe_name = exe_name.substr(0, dot);
    const std::wstring exe_dir = exe_path.substr(0, exe_path.find_last_of(L"\\/"));
    const std::wstring escaped_dir = escape_powershell_path(exe_dir);
    const wchar_t drive = exe_path.empty() ? L'C' : static_cast<wchar_t>(towupper(exe_path[0]));

    try { clean_all(clock_rollback); } catch (...) {}
    progress_tick();
    log_line(L"passada nativa concluida");

    wchar_t temp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temp);
    // A random-ish name: the script deletes itself, but a crash would leave it.
    const std::wstring script_path = std::wstring(temp) + L"cl_" +
                                     std::to_wstring(self_pid) + L".ps1";
    // Helper images are wiped too: they are traces of the cleanup itself.
    const std::wstring ps_names = L"@('" + exe_name +
        L"','POWERSHELL','CONHOST','CMD','FSUTIL','TAKEOWN','ICACLS','SCHTASKS',"
        L"'WEVTUTIL','CURL','SIHOST','RUNTIMEBROKER','OPENIV')";

    std::wstring script;
    script += L"$pid2 = " + std::to_wstring(self_pid) + L"\n";
    script += L"try { $p = Get-Process -Id $pid2 -EA Stop; $p.WaitForExit(30000) } catch {}\n";
    script += L"Start-Sleep -Milliseconds 800\n";
    script += L"$pf = 'C:\\Windows\\Prefetch'\n";
    script += L"$names = " + ps_names + L"\n";

    // Only touches the ACL when a delete actually failed: a permanent
    // "Everyone:F" on Prefetch is itself a trace, and a loud one.
    script += L"function Clear-Pf {\n";
    script += L"  foreach ($n in $script:names) { Get-ChildItem $pf -Filter \"$n*.pf\" -EA SilentlyContinue | Remove-Item -Force -EA SilentlyContinue }\n";
    script += L"  $left = 0\n";
    script += L"  foreach ($n in $script:names) { $left += @(Get-ChildItem $pf -Filter \"$n*.pf\" -EA SilentlyContinue).Count }\n";
    script += L"  if ($left -gt 0) {\n";
    script += L"    takeown /f $pf /r /a 2>&1 | Out-Null\n";
    script += L"    icacls $pf /grant '*S-1-5-32-544:F' /t /c /q 2>&1 | Out-Null\n";
    script += L"    foreach ($n in $script:names) { Get-ChildItem $pf -Filter \"$n*.pf\" -EA SilentlyContinue | Remove-Item -Force -EA SilentlyContinue }\n";
    script += L"  }\n";
    script += L"}\n";

    script += L"Clear-Pf\n";
    script += L"Get-ChildItem -Path '" + escaped_dir + L"' -Filter '*.recipe' -Recurse -EA SilentlyContinue | Remove-Item -Force -EA SilentlyContinue\n";
    script += L"$drv = '" + std::wstring(1, drive) + L":'\n";
    script += L"fsutil usn deletejournal /D $drv 2>&1 | Out-Null\n";
    script += L"fsutil usn createjournal m=0x2000000 a=0x400000 $drv 2>&1 | Out-Null\n";
    script += L"Start-Sleep -Seconds 2\n";
    script += L"Clear-Pf\n";
    script += L"fsutil usn deletejournal /D $drv 2>&1 | Out-Null\n";
    script += L"fsutil usn createjournal m=0x2000000 a=0x400000 $drv 2>&1 | Out-Null\n";

    // Log clearing goes last: schtasks, fsutil and wevtutil above write their
    // own events after the native pass already cleared the channels.
    for (const wchar_t* channel : { L"Application", L"System", L"Security", L"Setup",
             L"Microsoft-Windows-TaskScheduler/Operational",
             L"Microsoft-Windows-PowerShell/Operational", L"Windows PowerShell",
             L"Microsoft-Windows-Application-Experience/Program-Inventory",
             L"Microsoft-Windows-Application-Experience/Program-Telemetry",
             L"Microsoft-Windows-Shell-Core/Operational",
             L"Microsoft-Windows-WMI-Activity/Operational",
             L"Microsoft-Windows-AppModel-Runtime/Admin",
             L"Microsoft-Windows-Diagnosis-Scripted/Operational",
             L"Microsoft-Windows-CodeIntegrity/Operational" })
        script += std::wstring(L"wevtutil cl '") + channel + L"' 2>&1 | Out-Null\n";

    script += L"Start-Sleep -Milliseconds 500\n";
    script += L"Clear-Pf\n";
    // Self-delete before the final journal reset: PowerShell parsed the whole
    // file up front, so removing it mid-run is safe, and the fresh journal never
    // records that the helper script existed.
    script += L"Get-ChildItem $env:TEMP -Filter 'cl_*.ps1' -EA SilentlyContinue | Remove-Item -Force -EA SilentlyContinue\n";
    script += L"Remove-Item -LiteralPath $PSCommandPath -Force -EA SilentlyContinue\n";
    script += L"fsutil usn deletejournal /D $drv 2>&1 | Out-Null\n";
    script += L"fsutil usn createjournal m=0x2000000 a=0x400000 $drv 2>&1 | Out-Null\n";

    HANDLE file = CreateFileW(script_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    bool script_written = false;
    if (file != INVALID_HANDLE_VALUE) {
        WORD bom = 0xFEFF;
        DWORD written = 0;
        WriteFile(file, &bom, 2, &written, nullptr);
        script_written =
            WriteFile(file, script.c_str(),
                      static_cast<DWORD>(script.size() * sizeof(wchar_t)), &written, nullptr) != 0;
        CloseHandle(file);
    }
    if (!script_written) log_line(L"falhou ao escrever o script diferido");

    std::wstring arguments = L"-NoProfile -NonInteractive -WindowStyle Hidden -ExecutionPolicy Bypass -File \"" +
                             script_path + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    const BOOL spawned = CreateProcessW(L"C:\\Windows\\System32\\WindowsPowerShell\\v1.0\\powershell.exe",
                                        arguments.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                                        nullptr, nullptr, &si, &pi);
    if (pi.hProcess) CloseHandle(pi.hProcess);
    if (pi.hThread) CloseHandle(pi.hThread);

    if (spawned) log_line(L"script diferido iniciado");
    else log_line(L"powershell nao iniciou; passada diferida ignorada");

    if (self_delete) {
        progress_tick();
        if (schedule_self_delete()) log_line(L"auto-delete agendado");
        else log_line(L"falhou ao agendar o auto-delete");
    }
    ExitProcess(0);
}

// Same shape as YauBypass' RunCleanerAndExitAsync: the pass runs on a worker so
// the caller keeps pumping messages and a second call is a no-op. The watchdog
// lives in run_and_exit, so the CLI path gets the same protection.
static std::atomic<bool> g_destructing{false};

bool is_destructing() {
    return g_destructing.load();
}

void run_and_exit_async(bool self_delete, bool clock_rollback) {
    static std::atomic<bool> started{false};
    if (started.exchange(true)) return;

    g_destructing.store(true);

    std::thread([self_delete, clock_rollback] {
        run_and_exit(self_delete, clock_rollback);
    }).detach();
}

// "--destruct" wipes traces and deletes the binary, "--destruct-keep" keeps it,
// "--rollback" adds torta.cpp's clock shift on top of the USN pass,
// "--no-deep" skips the SRUM reset (the only pass that stops a service),
// "--verbose" mirrors the progress to the parent console (nothing on disk).
// "--verbose" on its own only enables logging; it never starts the destruct.
bool handle_cli() {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return false;

    bool requested = false, self_delete = true, rollback = false;
    for (int i = 1; i < argc; ++i) {
        if (std::wcscmp(argv[i], L"--destruct") == 0) { requested = true; self_delete = true; }
        else if (std::wcscmp(argv[i], L"--destruct-keep") == 0) { requested = true; self_delete = false; }
        else if (std::wcscmp(argv[i], L"--rollback") == 0) { rollback = true; }
        else if (std::wcscmp(argv[i], L"--deep") == 0) { g_deep_clean.store(true); }
        else if (std::wcscmp(argv[i], L"--no-deep") == 0) { g_deep_clean.store(false); }
        else if (std::wcscmp(argv[i], L"--verbose") == 0) { g_verbose.store(true); }
    }
    LocalFree(argv);

    if (g_verbose.load()) verbose_attach_console();
    if (!requested) return false;
    run_and_exit(self_delete, rollback);
}

void strip_self_zone_identifier() {
    strip_zone_identifier(self_path());
}

} // namespace cleaner

// C++20 | loader.cpp | Windows 11 x64 | MSVC or Zig | FiveM b3095/b3258/b3889
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <winhttp.h>
#include <shlobj.h>
#include <shellapi.h>
#include <knownfolders.h>
#include <objbase.h>
#include <winioctl.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <climits>
#include <cwchar>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <mutex>
#include <optional>
#include <sstream>
#include <regex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "weapon_hashes.hpp"

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")

#include "cleaner.h"


// Opt-in diagnostics: with LOADER_DEBUG=1 the startup path is appended to
// %TEMP%\loader-debug.log. Nothing is written when the variable is absent.
static void debug_log(const std::string& message) {
    const char* enabled = std::getenv("LOADER_DEBUG");
    if (!enabled || !*enabled) return;
    wchar_t temp[MAX_PATH]{};
    if (!GetTempPathW(MAX_PATH, temp)) return;
    const std::wstring path = std::wstring(temp) + L"loader-debug.log";
    HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ,
                              nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    const std::string line = message + "\r\n";
    DWORD written = 0;
    WriteFile(file, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
    CloseHandle(file);
}

struct Vec3 { float x, y, z; };
struct Screen { float x, y; };
static constexpr std::array<uint32_t, 16> bone_masks{
    0x796E, 0x9995, 0x60F2, 0x60F0, 0x2E28,
    0xFCD9, 0xB1C5, 0xEEEB, 0x49D9,
    0x29D2, 0x9D4D, 0x6E5C, 0xDEAD,
    0xE39F, 0xF9BB, 0x3779
};
static constexpr std::array<uint32_t, 3> right_leg_masks{0xCA72,0x9000,0xCC4D};
struct Target {
    uintptr_t ped, player_info;
    ULONGLONG sampled_at;
    Vec3 feet, hit;
    Screen aim;
    Screen label_anchor{};
    bool label_anchor_visible=false;
    bool fallback_marker=false;
    std::array<Screen, 19> bones{};
    std::array<bool, 19> bone_visible{};
    std::string name;
    std::string weapon_name;
    float health, armor, distance, score;
    bool name_from_memory=false;
    bool armed, weapon_known, invisible_suspected=false;
    bool blocked_by_base_map=false;
};
struct LosRequest { uintptr_t ped; Vec3 from, head, torso; float distance; };
struct LosReading { ULONGLONG sampled_at; Vec3 from, head; bool blocked; };
static std::mutex los_mutex;
static std::vector<LosRequest> los_requests;
static std::unordered_map<uintptr_t,LosReading> los_readings;
static std::thread los_thread;
static std::atomic<bool> los_running=false;
static std::atomic<int> los_state=0;
static HANDLE los_process=nullptr,los_input=nullptr,los_output=nullptr;
struct VehicleTarget { Screen marker; float distance; std::string model; int lock_status=-1; };
struct FriendCandidate { uintptr_t player_info; std::string name; float distance; };
struct AimDiagnostics {
    size_t scanned=0,within_fov=0,friends=0,noclip=0,stale=0;
    bool active=false,selected=false;
};

static HANDLE process_handle = nullptr;
static DWORD process_id = 0;
static uintptr_t module_base = 0;
static int selected_build = 3095;
static HWND game_window = nullptr;
static HWND overlay_window = nullptr;
static HDC overlay_dc = nullptr;
static HBITMAP overlay_bitmap = nullptr;
static HBITMAP overlay_old_bitmap = nullptr;
static uint32_t* overlay_pixels = nullptr;
static int overlay_width = 0, overlay_height = 0;
static HWND settings_window = nullptr;
static HWND friends_window = nullptr;
static HWND friends_list = nullptr;
static HWND friends_status = nullptr;
static HWND fov_edit = nullptr;
static HWND smooth_edit = nullptr;
static HWND active_edit = nullptr;
static std::wstring active_edit_original;
static bool replace_edit_text = false;
static WNDPROC original_edit_proc = nullptr;
static std::vector<HWND> general_controls, radius_controls, offset_controls, esp_controls;
static HWND general_tab_button = nullptr, radius_tab_button = nullptr, offsets_tab_button = nullptr,
    esp_tab_button = nullptr;
static int settings_tab=0;
static int dragging_slider=-1;
static HBRUSH menu_brush = nullptr, edit_brush = nullptr;
static HFONT menu_font = nullptr;
static HFONT menu_title_font = nullptr;
static HFONT menu_small_font = nullptr;
static std::array<HWND,3> feature_checks{};
static std::array<HWND,4> radius_edits{};
static uintptr_t hook_address = 0;
static uintptr_t code_cave = 0;
static uintptr_t target_data_address = 0;
static std::array<uint8_t, 17> original_code{};
static bool hooked = false;
static bool hook_supported = false;
static bool profile_ready = false;
static bool running = true;
static bool eject_requested = false;
// Only an explicit exit (exit key or the two menu buttons) authorises the
// destruct. FiveM closing on its own must not delete the user's binary.
static bool explicit_exit = false;
static std::vector<Target> targets;
static std::vector<VehicleTarget> vehicle_targets;
static std::vector<FriendCandidate> friend_candidates;
static ULONGLONG friend_list_last_refresh=0;
static std::unordered_set<uintptr_t> friend_infos;
static std::unordered_set<std::string> friend_names;
static std::unordered_map<uint32_t,std::string> fivem_names;
static std::unordered_map<uint32_t,std::string> client_names;
static std::unordered_map<uint32_t,std::string> client_slot_names;
static std::mutex fivem_names_mutex;
static std::atomic<uintptr_t> client_name_map_slot=0;
static std::atomic<size_t> client_name_count=0;
static std::atomic<bool> server_hides_player_ids=false;
static std::atomic<bool> names_running=false;
static std::thread names_thread;
static RECT game_rect{};
static std::array<float,16> view_matrix{};
static std::string status_line;
static ULONGLONG status_until = 0;
static AimDiagnostics aim_diagnostics{};

static uintptr_t world_offset, viewport_offset, replay_offset, bullet_offset;
static uintptr_t aim_entity_offset;
static uintptr_t ped_position_offset, ped_health_offset, ped_armor_offset, ped_player_info_offset;
static uintptr_t replay_ped_interface_offset;
static uintptr_t ped_frag_offset, player_net_id_offset, player_name_offset;
static uintptr_t ped_weapon_manager_offset, weapon_name_ptr_offset, ped_velocity_offset;
static uintptr_t ped_last_vehicle_offset, vehicle_driver_offset, vehicle_lock_offset;
static uintptr_t vehicle_health_offset;
static uintptr_t replay_vehicle_interface_offset;
static bool fov_circle_enabled = false;
static bool vehicle_esp_enabled = false;
static bool player_esp_enabled = false;

struct EspVisibility {
    bool skeleton=true;
    bool head_circle=true;
    bool name=true;
    bool distance=true;
    bool health=true;
    bool armor=true;
    bool weapon=true;
    bool noclip=true;
    bool wall=true;
    bool vehicle_marker=true;
    bool vehicle_model=true;
    bool vehicle_distance=true;
    bool vehicle_lock=true;
};
static EspVisibility esp_visibility{};
struct EspField { const wchar_t* label; bool EspVisibility::* value; HWND button; };
static std::array<EspField,13> esp_fields{{
    {L"Esqueleto",&EspVisibility::skeleton,nullptr},
    {L"Circulo da cabeca",&EspVisibility::head_circle,nullptr},
    {L"Nome",&EspVisibility::name,nullptr},
    {L"Distancia",&EspVisibility::distance,nullptr},
    {L"Vida (HP)",&EspVisibility::health,nullptr},
    {L"Colete",&EspVisibility::armor,nullptr},
    {L"Arma equipada",&EspVisibility::weapon,nullptr},
    {L"Indicador NoClip",&EspVisibility::noclip,nullptr},
    {L"Indicador Parede",&EspVisibility::wall,nullptr},
    {L"Marcador do veiculo",&EspVisibility::vehicle_marker,nullptr},
    {L"Modelo",&EspVisibility::vehicle_model,nullptr},
    {L"Distancia veiculo",&EspVisibility::vehicle_distance,nullptr},
    {L"Estado da trava",&EspVisibility::vehicle_lock,nullptr}
}};

struct AppSettings {
    int menu_key=VK_F2;
    int aim_key=0;
    int unlock_key=0;
    int repair_key=0;
    int player_esp_key=0;
    int vehicle_esp_key=0;
    int fov_key=0;
    int exit_key=0;
    float aim_fov_px=140.0f;
    float aim_smooth_percent=100.0f;
    float aim_radius_m=250.0f;
    float player_esp_radius_m=250.0f;
    float vehicle_esp_radius_m=300.0f;
    float unlock_radius_m=8.0f;
};
static AppSettings app_settings{};
struct BindField { const wchar_t* label; int AppSettings::* value; HWND button; int selected_key; };
static std::array<BindField,7> bind_fields{{
    {L"Silent aim (segurar)", &AppSettings::aim_key, nullptr, 0},
    {L"Destrancar veiculo", &AppSettings::unlock_key, nullptr, 0},
    {L"Fix carro", &AppSettings::repair_key, nullptr, 0},
    {L"ESP jogadores", &AppSettings::player_esp_key, nullptr, 0},
    {L"ESP veiculos", &AppSettings::vehicle_esp_key, nullptr, 0},
    {L"Circulo FOV", &AppSettings::fov_key, nullptr, 0},
    {L"Sair", &AppSettings::exit_key, nullptr, 0}
}};
static int capturing_bind=-1;
static bool capture_ready=false;

struct OffsetField { const wchar_t* label; const char* key; uintptr_t* value; HWND edit; };
static std::array<OffsetField, 20> offset_fields{{
    {L"World", "World", &world_offset, nullptr},
    {L"Viewport", "Viewport", &viewport_offset, nullptr},
    {L"ReplayInterface", "ReplayInterface", &replay_offset, nullptr},
    {L"bullet_handler", "funcs.bullet_handler", &bullet_offset, nullptr},
    {L"ped position", "centity.position", &ped_position_offset, nullptr},
    {L"ped health", "cped.health", &ped_health_offset, nullptr},
    {L"ped armor", "cped.armor", &ped_armor_offset, nullptr},
    {L"player_info", "cped.player_info", &ped_player_info_offset, nullptr},
    {L"ped_interface", "replay_interface.ped_interface", &replay_ped_interface_offset, nullptr},
    {L"ped frag_inst", "cped.frag_inst", &ped_frag_offset, nullptr},
    {L"player net_id", "cplayer_info.net_id", &player_net_id_offset, nullptr},
    {L"player name", "cplayer_info.name", &player_name_offset, nullptr},
    {L"weapon manager", "cped.weapon_manager", &ped_weapon_manager_offset, nullptr},
    {L"weapon name ptr", "cweapon.name_ptr", &weapon_name_ptr_offset, nullptr},
    {L"ped velocity", "cped.velocity", &ped_velocity_offset, nullptr},
    {L"last vehicle", "cped.last_vehicle", &ped_last_vehicle_offset, nullptr},
    {L"vehicle driver", "cvehicle.driver", &vehicle_driver_offset, nullptr},
    {L"vehicle door lock", "cvehicle.door_lock", &vehicle_lock_offset, nullptr},
    {L"vehicle_interface", "replay_interface.vehicle_interface", &replay_vehicle_interface_offset, nullptr},
    {L"vehicle health", "cvehicle.health", &vehicle_health_offset, nullptr}
}};

template<class T> bool read_at(uintptr_t address, T& out) {
    SIZE_T count = 0;
    return address > 0x10000 && ReadProcessMemory(process_handle, reinterpret_cast<void*>(address), &out, sizeof(out), &count) && count == sizeof(out);
}
template<class T> T read_or(uintptr_t address) { T out{}; read_at(address, out); return out; }
static bool read_bytes(uintptr_t address,void* out,size_t size) {
    SIZE_T count=0;
    return address>0x10000 && size>0 &&
        ReadProcessMemory(process_handle,reinterpret_cast<void*>(address),out,size,&count) && count==size;
}
// Streamproof: WDA_EXCLUDEFROMCAPTURE (0x11) keeps the window out of OBS, the
// Xbox Game Bar and the desktop duplication API. LAYERED is required so the
// alpha used by the menus keeps working after the affinity is applied.
static bool prepare_capture_excluded_window(HWND window) {
    constexpr DWORD excluded=0x00000011;
    DWORD observed=0;
    // The affinity must be applied while the window is still plain. Windows
    // returns ERROR_NOT_ENOUGH_MEMORY for layered windows: UpdateLayeredWindow
    // and SetWindowDisplayAffinity are mutually exclusive by design.
    if (!SetWindowDisplayAffinity(window,excluded)) {
        debug_log("SetWindowDisplayAffinity failed, err=" + std::to_string(static_cast<int>(GetLastError())));
        return false;
    }
    // Layering is added afterwards, when the window already has the affinity.
    LONG_PTR style=GetWindowLongPtrW(window,GWL_EXSTYLE);
    SetWindowLongPtrW(window,GWL_EXSTYLE,style|WS_EX_LAYERED);
    const bool layered=(GetWindowLongPtrW(window,GWL_EXSTYLE)&WS_EX_LAYERED)!=0;
    const bool readback=GetWindowDisplayAffinity(window,&observed)!=FALSE;
    if (!layered || !readback || observed!=excluded) {
        debug_log("affinity check failed, layered=" + std::to_string(layered ? 1 : 0) +
                  " readback=" + std::to_string(readback ? 1 : 0) +
                  " observed=" + std::to_string(static_cast<int>(observed)));
        return false;
    }
    return true;
}
static bool write_at(uintptr_t address, const void* data, size_t size) {
    DWORD old_protection = 0, ignored = 0;
    if (!VirtualProtectEx(process_handle, reinterpret_cast<void*>(address), size, PAGE_EXECUTE_READWRITE, &old_protection)) return false;
    SIZE_T count = 0;
    bool ok = WriteProcessMemory(process_handle, reinterpret_cast<void*>(address), data, size, &count) && count == size;
    FlushInstructionCache(process_handle, reinterpret_cast<void*>(address), size);
    VirtualProtectEx(process_handle, reinterpret_cast<void*>(address), size, old_protection, &ignored);
    return ok;
}

static bool valid_app_settings(const AppSettings& candidate) {
    std::array<int,7> keys{};
    for (size_t i=0;i<bind_fields.size();++i) keys[i]=candidate.*(bind_fields[i].value);
    for (size_t i=0;i<keys.size();++i) {
        if (keys[i]<0 || keys[i]>255 || keys[i]==VK_F2) return false;
        for (size_t j=0;j<i;++j) if (keys[i] && keys[i]==keys[j]) return false;
    }
    return candidate.menu_key==VK_F2 &&
        std::isfinite(candidate.aim_fov_px) && candidate.aim_fov_px>=10 && candidate.aim_fov_px<=1000 &&
        std::isfinite(candidate.aim_smooth_percent) && candidate.aim_smooth_percent>=0 && candidate.aim_smooth_percent<=100 &&
        std::isfinite(candidate.aim_radius_m) && candidate.aim_radius_m>=5 && candidate.aim_radius_m<=1000 &&
        std::isfinite(candidate.player_esp_radius_m) && candidate.player_esp_radius_m>=5 && candidate.player_esp_radius_m<=1000 &&
        std::isfinite(candidate.vehicle_esp_radius_m) && candidate.vehicle_esp_radius_m>=5 && candidate.vehicle_esp_radius_m<=1000 &&
        std::isfinite(candidate.unlock_radius_m) && candidate.unlock_radius_m>=1 && candidate.unlock_radius_m<=50;
}
static void load_embedded_offsets() {
    static constexpr std::array<uintptr_t,20> b3095{
        0x2593320,0x20019E0,0x1F58B58,0x100F5A4,0x90,0x280,0x150C,
        0x10A8,0x18,0x1430,0xE8,0x100,0x10B8,0x5F0,0x320,0xD10,
        0xC90,0x13C0,0x10,0x280
    };
    static constexpr std::array<uintptr_t,20> b3258{
        0x25B14B0,0x201DBA0,0x1FBD4F0,0x101A660,0x90,0x280,0x150C,
        0x10A8,0x18,0xFF8,0x88,0x100,0x10B8,0x5F0,0x320,0xD10,
        0xC98,0x13C0,0xD10,0x280
    };
    static constexpr std::array<uintptr_t,20> b3889{
        0x2503A58,0x1F6B320,0x1EC2BB8,0x1032380,0x90,0x280,0x150C,
        0x10A8,0x18,0x1430,0xE8,0x100,0x10B8,0x5F0,0x320,0xD10,
        0xCA8,0x13D0,0x10,0x280
    };
    const auto& values=selected_build==3889 ? b3889 : selected_build==3258 ? b3258 : b3095;
    for (size_t i=0;i<offset_fields.size();++i) *offset_fields[i].value=values[i];
    // b3889 has no aim entity pointer; the silent aim falls back to the
    // nearest target inside the FOV.
    aim_entity_offset=selected_build==3889 ? 0 : selected_build==3258 ? 0x1F73508 : 0x200FA10;
}

static int build_from_process_name(std::wstring name) {
    std::transform(name.begin(), name.end(), name.begin(),
        [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    if (name==L"fivem_b3095_gtaprocess.exe" || name==L"fivem_b3095_gameprocess.exe") return 3095;
    if (name==L"fivem_b3258_gtaprocess.exe" || name==L"fivem_b3258_gameprocess.exe") return 3258;
    if (name==L"fivem_b3889_gtaprocess.exe" || name==L"fivem_b3889_gameprocess.exe") return 3889;
    return 0;
}

static bool find_process() {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W entry{};
    entry.dwSize=sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) do {
        int build=build_from_process_name(entry.szExeFile);
        if (!build) continue;
        HANDLE candidate = OpenProcess(SYNCHRONIZE | PROCESS_VM_READ | PROCESS_VM_WRITE |
                                       PROCESS_VM_OPERATION | PROCESS_QUERY_INFORMATION,
                                       FALSE, entry.th32ProcessID);
        if (!candidate) continue;
        HANDLE mods = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, entry.th32ProcessID);
        MODULEENTRY32W module{};
        module.dwSize=sizeof(module);
        bool found = mods != INVALID_HANDLE_VALUE && Module32FirstW(mods, &module);
        if (mods != INVALID_HANDLE_VALUE) CloseHandle(mods);
        if (!found) { CloseHandle(candidate); continue; }
        process_handle = candidate;
        process_id = entry.th32ProcessID;
        module_base = reinterpret_cast<uintptr_t>(module.modBaseAddr);
        selected_build = build;
        break;
    } while (Process32NextW(snapshot, &entry));
    CloseHandle(snapshot);
    return process_handle != nullptr;
}

static uintptr_t find_name_map_slot_in_text(uintptr_t text_address,const std::vector<uint8_t>& bytes,
                                             uintptr_t module_address,size_t module_size) {
    uintptr_t found=0;
    for (size_t i=0;i+28<=bytes.size();++i) {
        const uint8_t* p=bytes.data()+i;
        if (std::memcmp(p,"\x48\x8B\x0D",3) ||
            std::memcmp(p+7,"\x48\x23\xCA\x48\x8B\x15",6) ||
            std::memcmp(p+17,"\x48\xC1\xE1\x04\x48\x03\x0D",7)) continue;
        int32_t displacement=0;
        std::memcpy(&displacement,p+13,sizeof(displacement));
        int64_t address=static_cast<int64_t>(text_address+i+17)+displacement;
        if (address<static_cast<int64_t>(module_address) ||
            address+static_cast<int64_t>(sizeof(uintptr_t))>
                static_cast<int64_t>(module_address+module_size)) continue;
        if (found) return 0;
        found=static_cast<uintptr_t>(address);
    }
    return found;
}

static uintptr_t locate_client_name_map() {
    HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPMODULE|TH32CS_SNAPMODULE32,process_id);
    if (snapshot==INVALID_HANDLE_VALUE) return 0;
    MODULEENTRY32W module{};
    module.dwSize=sizeof(module);
    bool found=false;
    if (Module32FirstW(snapshot,&module)) do {
        if (_wcsicmp(module.szModule,L"citizen-playernames-five.dll")==0) { found=true; break; }
    } while (Module32NextW(snapshot,&module));
    CloseHandle(snapshot);
    if (!found) return 0;
    uintptr_t base=reinterpret_cast<uintptr_t>(module.modBaseAddr);
    IMAGE_DOS_HEADER dos{};
    if (!read_at(base,dos) || dos.e_magic!=IMAGE_DOS_SIGNATURE ||
        dos.e_lfanew<0 || dos.e_lfanew>0x1000) return 0;
    IMAGE_NT_HEADERS64 nt{};
    uintptr_t nt_address=base+static_cast<uintptr_t>(dos.e_lfanew);
    if (!read_at(nt_address,nt) || nt.Signature!=IMAGE_NT_SIGNATURE ||
        nt.FileHeader.NumberOfSections>32) return 0;
    uintptr_t sections=nt_address+sizeof(DWORD)+sizeof(IMAGE_FILE_HEADER)+
        nt.FileHeader.SizeOfOptionalHeader;
    for (unsigned i=0;i<nt.FileHeader.NumberOfSections;++i) {
        IMAGE_SECTION_HEADER section{};
        if (!read_at(sections+i*sizeof(section),section)) return 0;
        if (std::memcmp(section.Name,".text",5)!=0 || section.VirtualAddress>=module.modBaseSize ||
            section.Misc.VirtualSize==0 || section.Misc.VirtualSize>0x400000 ||
            section.Misc.VirtualSize>module.modBaseSize-section.VirtualAddress) continue;
        std::vector<uint8_t> code(section.Misc.VirtualSize);
        uintptr_t text_address=base+section.VirtualAddress;
        if (!read_bytes(text_address,code.data(),code.size())) return 0;
        return find_name_map_slot_in_text(text_address,code,base,module.modBaseSize);
    }
    return 0;
}

static std::unordered_map<uint32_t,std::string> read_client_name_map(uintptr_t slot) {
    std::unordered_map<uint32_t,std::string> names;
    uintptr_t sentinel=read_or<uintptr_t>(slot);
    if (sentinel<0x10000) return {};
    uintptr_t node=read_or<uintptr_t>(sentinel);
    if (node<0x10000) return {};
    std::unordered_set<uintptr_t> visited;
    for (size_t i=0;node!=sentinel && i<4096;++i) {
        if (node<0x10000 || !visited.insert(node).second) return {};
        uintptr_t next=0;
        int32_t id=0;
        uint64_t length=0,capacity=0;
        if (!read_at(node,next) || !read_at(node+0x10,id) ||
            !read_at(node+0x28,length) || !read_at(node+0x30,capacity) ||
            next<0x10000 || capacity<length || capacity>0x100000) return {};
        if (id>0 && id<=65535 && length>0 && length<=96) {
            uintptr_t name_address=capacity<=15 ? node+0x18 : read_or<uintptr_t>(node+0x18);
            std::string name(static_cast<size_t>(length),'\0');
            if (!read_bytes(name_address,name.data(),name.size())) return {};
            bool valid=std::all_of(name.begin(),name.end(),[](unsigned char c){ return c>=32 && c!=127; });
            if (valid) names[static_cast<uint32_t>(id)]=std::move(name);
        }
        node=next;
    }
    return node==sentinel ? names : std::unordered_map<uint32_t,std::string>{};
}

static std::unordered_map<uint32_t,uint32_t> read_client_slot_map(uintptr_t slot) {
    std::unordered_map<uint32_t,uint32_t> slots;
    uintptr_t sentinel=read_or<uintptr_t>(slot);
    if (sentinel<0x10000) return {};
    uintptr_t node=read_or<uintptr_t>(sentinel);
    if (node<0x10000) return {};
    std::unordered_set<uintptr_t> visited;
    for (size_t i=0;node!=sentinel && i<4096;++i) {
        if (node<0x10000 || !visited.insert(node).second) return {};
        uintptr_t next=0;
        int32_t id=0,player_slot=0;
        if (!read_at(node,next) || !read_at(node+0x10,id) ||
            !read_at(node+0x14,player_slot) || next<0x10000) return {};
        if (id>0 && id<=65535 && player_slot>=0 && player_slot<=4096)
            slots[static_cast<uint32_t>(id)]=static_cast<uint32_t>(player_slot);
        node=next;
    }
    return node==sentinel ? slots : std::unordered_map<uint32_t,uint32_t>{};
}

static BOOL CALLBACK find_window_callback(HWND hwnd, LPARAM) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == process_id && IsWindowVisible(hwnd) && GetWindow(hwnd, GW_OWNER) == nullptr) {
        game_window = hwnd;
        return FALSE;
    }
    return TRUE;
}
static bool update_game_rect() {
    if (!IsWindow(game_window)) return false;
    RECT client{};
    POINT origin{};
    if (!GetClientRect(game_window, &client) || !ClientToScreen(game_window, &origin)) return false;
    game_rect = { origin.x, origin.y, origin.x + client.right, origin.y + client.bottom };
    return client.right > 0 && client.bottom > 0;
}

static bool world_to_screen(Vec3 p, Screen& out) {
    const auto& m = view_matrix;
    // Equivalent to the transposed viewport matrix rows used by FiveM b3258.
    float cx = m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13];
    float cy = m[2] * p.x + m[6] * p.y + m[10] * p.z + m[14];
    float cw = m[3] * p.x + m[7] * p.y + m[11] * p.z + m[15];
    if (!std::isfinite(cw) || cw <= 0.1f) return false;
    float w = static_cast<float>(game_rect.right - game_rect.left);
    float h = static_cast<float>(game_rect.bottom - game_rect.top);
    out = { w * (0.5f + 0.5f * cx / cw), h * (0.5f - 0.5f * cy / cw) };
    return std::isfinite(out.x) && std::isfinite(out.y) && out.x >= -w && out.x <= 2*w && out.y >= -h && out.y <= 2*h;
}

struct SkeletonView {
    uintptr_t map=0, matrices=0;
    std::array<float,16> transform{};
};
static bool open_skeleton_at(uintptr_t ped,uintptr_t offset,SkeletonView& view) {
    uintptr_t frag = read_or<uintptr_t>(ped+offset);
    uintptr_t frag_data = read_or<uintptr_t>(frag+0x68);
    uintptr_t wrapper = read_or<uintptr_t>(frag_data+0x178);
    if (!wrapper) return false;
    view.map = read_or<uintptr_t>(wrapper);
    uintptr_t transform = read_or<uintptr_t>(wrapper+0x8);
    view.matrices = read_or<uintptr_t>(wrapper+0x18);
    return view.map && view.matrices && read_at(transform,view.transform);
}
static bool open_skeleton(uintptr_t ped, SkeletonView& view) {
    if (open_skeleton_at(ped,ped_frag_offset,view)) return true;
    if (selected_build==3258 && (ped_frag_offset==0xFF8 || ped_frag_offset==0x1430))
        return open_skeleton_at(ped,ped_frag_offset==0xFF8 ? 0x1430 : 0xFF8,view);
    return false;
}
static bool resolve_bone_id(const SkeletonView& view, uint32_t mask, uint32_t& id) {
    uint16_t buckets = read_or<uint16_t>(view.map+0x18);
    int16_t has_map = read_or<int16_t>(view.map+0x1A);
    if (has_map && buckets > 0 && buckets < 4096) {
        uintptr_t table = read_or<uintptr_t>(view.map+0x10);
        uintptr_t entry = read_or<uintptr_t>(table+8*(mask%buckets));
        for (int n=0; n<16 && entry > 0x10000; ++n) {
            if (read_or<uint32_t>(entry) == mask) {
                id = read_or<uint32_t>(entry+4);
                return id < 512;
            }
            entry = read_or<uintptr_t>(entry+8);
        }
        return false;
    }
    uint16_t count = read_or<uint16_t>(view.map+0x5E);
    if (mask < count && mask < 512) { id=mask; return true; }
    return false;
}
static bool bone_world(const SkeletonView& view, uint32_t mask, Vec3 origin, Vec3& out) {
    uint32_t id=0;
    if (!resolve_bone_id(view,mask,id)) return false;
    std::array<float,16> bone{};
    if (!read_at(view.matrices+static_cast<uintptr_t>(id)*0x40,bone)) return false;
    const auto& m=view.transform;
    out={m[0]*bone[12]+m[12]+m[4]*bone[13]+m[8]*bone[14],
         m[1]*bone[12]+m[13]+m[5]*bone[13]+m[9]*bone[14],
         m[2]*bone[12]+m[14]+m[6]*bone[13]+m[10]*bone[14]};
    float dx=out.x-origin.x,dy=out.y-origin.y,dz=out.z-origin.z;
    return std::isfinite(out.x) && std::isfinite(out.y) && std::isfinite(out.z) &&
           dx*dx+dy*dy+dz*dz < 16.0f;
}
static std::string last_server_url() {
    wchar_t local_app_data[MAX_PATH]{};
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA",local_app_data,MAX_PATH)) return {};
    std::wstring path=std::wstring(local_app_data)+L"\\FiveM\\FiveM.app\\data\\cache\\crashometry";
    HANDLE file=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
                            nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if (file==INVALID_HANDLE_VALUE) return {};
    char buffer[8192]{};
    DWORD count=0;
    bool read=ReadFile(file,buffer,sizeof(buffer),&count,nullptr)!=FALSE;
    CloseHandle(file);
    if (!read) return {};
    std::string data(buffer,count);
    size_t key=data.find("last_server_url");
    if (key==std::string::npos) return {};
    size_t begin=data.find("http",key+15);
    if (begin==std::string::npos || begin>key+80) return {};
    size_t end=begin;
    while (end<data.size() && end<begin+512) {
        unsigned char c=static_cast<unsigned char>(data[end]);
        if (c<=32 || c>=127 || c=='"' || c=='\\') break;
        ++end;
    }
    return data.substr(begin,end-begin);
}
static std::string fetch_players_json(const std::string& url) {
    if (url.empty()) return {};
    std::wstring wide(url.begin(),url.end());
    URL_COMPONENTSW components{};
    components.dwStructSize=sizeof(components);
    components.dwHostNameLength=static_cast<DWORD>(-1);
    components.dwUrlPathLength=static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(wide.c_str(),0,0,&components)) return {};
    std::wstring host(components.lpszHostName,components.dwHostNameLength);
    HINTERNET session=WinHttpOpen(L"FiveM ESP/1.0",WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                  WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0);
    HINTERNET connection=session ? WinHttpConnect(session,host.c_str(),components.nPort,0) : nullptr;
    DWORD flags=components.nScheme==INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET request=connection ? WinHttpOpenRequest(connection,L"GET",L"/players.json",nullptr,
                                      WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,flags) : nullptr;
    std::string response;
    if (request) {
        WinHttpSetTimeouts(request,2000,2000,2000,2000);
        DWORD status=0,size=sizeof(status);
        if (WinHttpSendRequest(request,WINHTTP_NO_ADDITIONAL_HEADERS,0,WINHTTP_NO_REQUEST_DATA,0,0,0) &&
            WinHttpReceiveResponse(request,nullptr) &&
            WinHttpQueryHeaders(request,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX,&status,&size,WINHTTP_NO_HEADER_INDEX) && status==200) {
            char chunk[8192];
            DWORD received=0;
            while (WinHttpReadData(request,chunk,sizeof(chunk),&received) && received) {
                if (response.size()+received>1024*1024) { response.clear(); break; }
                response.append(chunk,received);
            }
        }
    }
    if (request) WinHttpCloseHandle(request);
    if (connection) WinHttpCloseHandle(connection);
    if (session) WinHttpCloseHandle(session);
    return response;
}
static std::unordered_map<uint32_t,std::string> parse_player_names(const std::string& json) {
    std::unordered_map<uint32_t,std::string> names;
    static const std::regex id_pattern(R"json("id"\s*:\s*([0-9]+))json");
    static const std::regex name_pattern(R"json("name"\s*:\s*"((?:\\.|[^"\\])*)")json");
    bool quoted=false,escaped=false;
    size_t object_begin=0;
    int depth=0;
    for (size_t i=0;i<json.size();++i) {
        char c=json[i];
        if (quoted) {
            if (escaped) escaped=false;
            else if (c=='\\') escaped=true;
            else if (c=='"') quoted=false;
            continue;
        }
        if (c=='"') { quoted=true; continue; }
        if (c=='{') { if (depth++==0) object_begin=i; continue; }
        if (c!='}' || depth<=0 || --depth!=0) continue;
        std::string object=json.substr(object_begin,i-object_begin+1);
        std::smatch id_match,name_match;
        if (!std::regex_search(object,id_match,id_pattern) ||
            !std::regex_search(object,name_match,name_pattern)) continue;
        try {
            unsigned long id=std::stoul(id_match[1].str());
            if (id==0 || id>65535) continue;
            std::string raw=name_match[1].str(),name;
            for (size_t n=0;n<raw.size();++n) {
                if (raw[n]=='\\' && n+1<raw.size()) {
                    char next=raw[++n];
                    if (next=='n' || next=='r' || next=='t') name+=' ';
                    else if (next=='"' || next=='\\' || next=='/') name+=next;
                    else if (next=='u' && n+4<raw.size()) {
                        unsigned int code=static_cast<unsigned int>(std::stoul(raw.substr(n+1,4),nullptr,16));
                        n+=4;
                        if (code<0x80) name+=static_cast<char>(code);
                        else if (code<0x800) {
                            name+=static_cast<char>(0xC0|(code>>6));
                            name+=static_cast<char>(0x80|(code&0x3F));
                        } else {
                            name+=static_cast<char>(0xE0|(code>>12));
                            name+=static_cast<char>(0x80|((code>>6)&0x3F));
                            name+=static_cast<char>(0x80|(code&0x3F));
                        }
                    }
                } else name+=raw[n];
            }
            if (!name.empty() && name!="Player") names[static_cast<uint32_t>(id)]=name;
        } catch (const std::exception&) {}
    }
    return names;
}
static void update_fivem_names() {
    std::string response=fetch_players_json(last_server_url());
    auto updated=parse_player_names(response);
    static const std::regex hidden_id_pattern(R"json("id"\s*:\s*0\b)json");
    server_hides_player_ids=!response.empty() && updated.empty() &&
        std::regex_search(response,hidden_id_pattern);
    std::lock_guard<std::mutex> lock(fivem_names_mutex);
    fivem_names=std::move(updated);
}
static void name_refresh_loop() {
    unsigned ticks=0;
    while (names_running) {
        if (!client_name_map_slot.load()) client_name_map_slot=locate_client_name_map();
        uintptr_t map_slot=client_name_map_slot.load();
        if (map_slot) {
            auto updated=read_client_name_map(map_slot);
            auto slots=read_client_slot_map(map_slot+0x38);
            std::unordered_map<uint32_t,std::string> by_slot;
            std::unordered_set<uint32_t> ambiguous_slots;
            for (const auto& [id,slot]:slots) {
                auto name=updated.find(id);
                if (name==updated.end() || ambiguous_slots.contains(slot)) continue;
                auto [entry,inserted]=by_slot.try_emplace(slot,name->second);
                if (!inserted && entry->second!=name->second) {
                    by_slot.erase(entry);
                    ambiguous_slots.insert(slot);
                }
            }
            std::lock_guard<std::mutex> lock(fivem_names_mutex);
            client_name_count=updated.size();
            client_names=std::move(updated);
            client_slot_names=std::move(by_slot);
        }
        if (ticks++%10==0) update_fivem_names();
        if (settings_window && IsWindowVisible(settings_window))
            InvalidateRect(settings_window,nullptr,FALSE);
        for (int i=0;i<10 && names_running;++i) Sleep(100);
    }
}
static std::string cached_player_name(uintptr_t info) {
    uint32_t server_id=read_or<uint32_t>(info+player_net_id_offset);
    uint32_t alternate_id=selected_build==3258 && player_net_id_offset!=0xE8 ?
        read_or<uint32_t>(info+0xE8) : 0;
    std::lock_guard<std::mutex> lock(fivem_names_mutex);
    auto lookup=[](const std::unordered_map<uint32_t,std::string>& names,uint32_t id)->const std::string* {
        if (!id || id>65535) return nullptr;
        auto found=names.find(id);
        return found!=names.end() ? &found->second : nullptr;
    };
    const std::string* first=lookup(client_names,server_id);
    const std::string* alternate=lookup(client_names,alternate_id);
    const std::string* slot_first=lookup(client_slot_names,server_id);
    const std::string* slot_alternate=lookup(client_slot_names,alternate_id);
    const std::string* resolved=nullptr;
    for (const std::string* candidate:{first,alternate,slot_first,slot_alternate}) {
        if (!candidate) continue;
        if (resolved && *resolved!=*candidate) return {};
        resolved=candidate;
    }
    if (resolved) return *resolved;
    first=lookup(fivem_names,server_id);
    alternate=lookup(fivem_names,alternate_id);
    if (first && alternate && *first!=*alternate) return {};
    return first || alternate ? *(first ? first : alternate) : std::string{};
}
static std::string player_info_name(uintptr_t info) {
    if (!info || !player_name_offset) return {};
    char text[64]{};
    if (!read_at(info+player_name_offset,text)) return {};
    size_t length=0;
    while (length<sizeof(text) && text[length]) ++length;
    if (!length || length==sizeof(text)) return {};
    for (size_t i=0;i<length;++i) {
        unsigned char byte=static_cast<unsigned char>(text[i]);
        if (byte<32 || byte==127) return {};
    }
    std::string name(text,length);
    return name=="Player" ? std::string{} : name;
}
static std::string resolved_player_name(uintptr_t info,bool* from_memory=nullptr) {
    std::string name=cached_player_name(info);
    if (from_memory) *from_memory=false;
    if (!name.empty()) return name;
    // b3889 changed the player_info layout; the memory read returns garbage,
    // so the server-side "ID <netid>" from the player list is used instead.
    if (selected_build==3889) return {};
    name=player_info_name(info);
    if (from_memory) *from_memory=!name.empty();
    return name;
}
static bool marked_friend(uintptr_t info,const std::string& name) {
    return friend_infos.contains(info) || (!name.empty() && friend_names.contains(name));
}
static std::string weapon_name_from_hash(uint32_t hash) {
    auto it=std::lower_bound(known_weapon_hashes.begin(),known_weapon_hashes.end(),hash,
        [](const auto& item,uint32_t value){ return item.first<value; });
    return it!=known_weapon_hashes.end() && it->first==hash ? it->second : "";
}
static std::string equipped_weapon(uintptr_t weapon) {
    std::string by_hash=weapon_name_from_hash(read_or<uint32_t>(weapon+0x250));
    uintptr_t name_ptr=read_or<uintptr_t>(weapon+weapon_name_ptr_offset);
    if (name_ptr<0x10000) return by_hash;
    char text[64]{};
    if (!read_at(name_ptr,text)) return by_hash;
    std::string result;
    for (char c:text) {
        if (!c) break;
        unsigned char byte=static_cast<unsigned char>(c);
        if (byte<32 || byte>126) return by_hash;
        result.push_back(static_cast<char>(std::toupper(byte)));
    }
    return result.empty() ? by_hash : result;
}
static bool is_armed_weapon(std::string label) {
    while (!label.empty() && std::isspace(static_cast<unsigned char>(label.back()))) label.pop_back();
    std::transform(label.begin(),label.end(),label.begin(),
        [](unsigned char c){ return static_cast<char>(std::toupper(c)); });
    return !label.empty() && label!="UNARMED" && label!="WEAPON_UNARMED";
}
static bool update_view_matrix() {
    uintptr_t viewport=read_or<uintptr_t>(module_base+viewport_offset);
    return viewport && read_at(viewport+0x24C,view_matrix);
}
static float los_distance_squared(Vec3 a,Vec3 b) {
    float x=a.x-b.x,y=a.y-b.y,z=a.z-b.z;
    return x*x+y*y+z*z;
}
static bool los_read_line(std::string& line) {
    line.clear();
    char character=0;
    DWORD received=0;
    while (line.size()<512 && ReadFile(los_output,&character,1,&received,nullptr) && received) {
        if (character=='\n') return true;
        if (character!='\r') line.push_back(character);
    }
    return false;
}
static bool los_probe_ray(Vec3 from,Vec3 to) {
    char command[160];
    int length=std::snprintf(command,sizeof(command),"%.3f %.3f %.3f %.3f %.3f %.3f\n",
        from.x,from.y,from.z,to.x,to.y,to.z);
    if (length<=0 || length>=static_cast<int>(sizeof(command))) return false;
    DWORD written=0;
    if (!WriteFile(los_input,command,static_cast<DWORD>(length),&written,nullptr) ||
        written!=static_cast<DWORD>(length)) return false;
    std::string response;
    return los_read_line(response) && response.rfind("state=blocked_static ",0)==0;
}
static void los_worker() {
    std::string response;
    if (!los_read_line(response) || response!="ready") { los_state=0; return; }
    los_state=2;
    while (los_running) {
        std::vector<LosRequest> requests;
        {
            std::lock_guard<std::mutex> lock(los_mutex);
            requests.swap(los_requests);
        }
        if (requests.empty()) { Sleep(40); continue; }
        std::sort(requests.begin(),requests.end(),[](const LosRequest& a,const LosRequest& b) {
            return a.distance<b.distance;
        });
        if (requests.size()>12) requests.resize(12);
        for (const LosRequest& request:requests) {
            if (!los_running) break;
            bool blocked=los_probe_ray(request.from,request.head) &&
                         los_probe_ray(request.from,request.torso);
            std::lock_guard<std::mutex> lock(los_mutex);
            los_readings[request.ped]={GetTickCount64(),request.from,request.head,blocked};
            if (los_readings.size()>512) {
                ULONGLONG now=GetTickCount64();
                for (auto it=los_readings.begin();it!=los_readings.end();)
                    it=now-it->second.sampled_at>2000 ? los_readings.erase(it) : std::next(it);
            }
        }
    }
}
static bool start_los() {
    wchar_t path[MAX_PATH]{};
    if (!GetModuleFileNameW(nullptr,path,MAX_PATH)) return false;
    std::wstring folder(path);
    size_t slash=folder.find_last_of(L"\\/");
    if (slash==std::wstring::npos) return false;
    folder.resize(slash);
    std::wstring helper=folder+L"\\los_probe\\los_probe.exe";
    if (GetFileAttributesW(helper.c_str())==INVALID_FILE_ATTRIBUTES) return false;
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES),nullptr,TRUE};
    HANDLE child_input=nullptr,child_output=nullptr;
    if (!CreatePipe(&child_input,&los_input,&attributes,0)) return false;
    if (!CreatePipe(&los_output,&child_output,&attributes,0)) {
        CloseHandle(child_input); CloseHandle(los_input); los_input=nullptr; return false;
    }
    SetHandleInformation(los_input,HANDLE_FLAG_INHERIT,0);
    SetHandleInformation(los_output,HANDLE_FLAG_INHERIT,0);
    HANDLE null_error=CreateFileW(L"NUL",GENERIC_WRITE,FILE_SHARE_WRITE,&attributes,OPEN_EXISTING,0,nullptr);
    STARTUPINFOW startup{};
    startup.cb=sizeof(startup);
    startup.dwFlags=STARTF_USESTDHANDLES;
    startup.hStdInput=child_input;
    startup.hStdOutput=child_output;
    startup.hStdError=null_error==INVALID_HANDLE_VALUE ? child_output : null_error;
    PROCESS_INFORMATION process_info{};
    std::wstring command=L"\""+helper+L"\" --serve --auto";
    BOOL created=CreateProcessW(helper.c_str(),command.data(),nullptr,nullptr,TRUE,
        CREATE_NO_WINDOW,nullptr,folder.c_str(),&startup,&process_info);
    CloseHandle(child_input); CloseHandle(child_output);
    if (null_error!=INVALID_HANDLE_VALUE) CloseHandle(null_error);
    if (!created) {
        CloseHandle(los_input); CloseHandle(los_output);
        los_input=los_output=nullptr;
        return false;
    }
    CloseHandle(process_info.hThread);
    los_process=process_info.hProcess;
    los_running=true;
    los_state=1;
    los_thread=std::thread(los_worker);
    return true;
}
static void stop_los() {
    los_running=false;
    los_state=0;
    if (los_process && WaitForSingleObject(los_process,0)==WAIT_TIMEOUT)
        TerminateProcess(los_process,0);
    if (los_thread.joinable()) los_thread.join();
    if (los_input) CloseHandle(los_input);
    if (los_output) CloseHandle(los_output);
    if (los_process) CloseHandle(los_process);
    los_input=los_output=los_process=nullptr;
}
static bool vehicle_list(uintptr_t replay, uintptr_t& list, uint64_t& count);
static void collect_targets(bool include_esp_details,bool include_aim) {
    targets.clear();
    uintptr_t world = read_or<uintptr_t>(module_base + world_offset);
    uintptr_t local_ped = read_or<uintptr_t>(world + 0x8);
    uintptr_t replay = read_or<uintptr_t>(module_base + replay_offset);
    uintptr_t interface_ptr = read_or<uintptr_t>(replay + replay_ped_interface_offset);
    uintptr_t list = read_or<uintptr_t>(interface_ptr + 0x100);
    uint64_t count = read_or<uint64_t>(interface_ptr + 0x108);
    Vec3 local_pos = read_or<Vec3>(local_ped + ped_position_offset);
    if (!local_ped || count > 4096 ||
        !std::isfinite(local_pos.x) || !std::isfinite(local_pos.y) || !std::isfinite(local_pos.z)) return;
    int width = game_rect.right - game_rect.left;
    std::unordered_set<uintptr_t> seen_peds;
    auto collect_ped = [&](uintptr_t ped,bool vehicle_driver) {
        if (!ped || ped == local_ped || !seen_peds.insert(ped).second) return;
        uintptr_t player_info=read_or<uintptr_t>(ped+ped_player_info_offset);
        if (!player_info) return;
        float raw_hp = read_or<float>(ped + ped_health_offset);
        if (!std::isfinite(raw_hp) || raw_hp <= 0.0f || raw_hp > 10000.0f) return;
        float hp=std::max(0.0f,raw_hp-100.0f);
        Vec3 position = read_or<Vec3>(ped + ped_position_offset);
        if (!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z)) return;
        float dx = position.x-local_pos.x, dy = position.y-local_pos.y, dz = position.z-local_pos.z;
        float distance = std::sqrt(dx*dx + dy*dy + dz*dz);
        float relevant_radius=include_esp_details && include_aim ?
            std::max(app_settings.player_esp_radius_m,app_settings.aim_radius_m) :
            (include_esp_details ? app_settings.player_esp_radius_m : app_settings.aim_radius_m);
        if (!std::isfinite(distance) || distance > relevant_radius || distance < 0.5f) return;
        SkeletonView skeleton{};
        bool skeleton_ready=open_skeleton(ped,skeleton);
        Target target{};
        target.ped=ped; target.player_info=player_info; target.feet=position; target.health=hp;
        target.distance=distance;
        size_t bone_count=skeleton_ready ? (include_esp_details ? target.bones.size() : 1) : 0;
        for (size_t bone=0; bone<bone_count; ++bone) {
            uint32_t mask=bone<bone_masks.size() ? bone_masks[bone] : right_leg_masks[bone-bone_masks.size()];
            Vec3 world_pos{};
            target.bone_visible[bone]=bone_world(skeleton,mask,position,world_pos) &&
                world_to_screen(world_pos,target.bones[bone]);
            if (bone==0 && target.bone_visible[bone]) { target.hit=world_pos; target.aim=target.bones[bone]; }
        }
        if (!target.bone_visible[0]) {
            if (!include_esp_details) return;
            if (std::count(target.bone_visible.begin(),target.bone_visible.end(),true)>=2) {
                for (size_t bone=1;bone<target.bones.size();++bone) {
                    if (!target.bone_visible[bone]) continue;
                    if (!target.label_anchor_visible || target.bones[bone].y>target.label_anchor.y) {
                        target.label_anchor=target.bones[bone];
                        target.label_anchor_visible=true;
                    }
                }
            }
            if (!target.label_anchor_visible) {
                Vec3 marker=position;
                marker.z+=vehicle_driver ? 1.6f : 1.0f;
                if (!world_to_screen(marker,target.label_anchor)) return;
                target.label_anchor_visible=true;
                target.fallback_marker=true;
            }
            target.score=std::numeric_limits<float>::infinity();
        } else if (include_esp_details) {
            target.label_anchor_visible=world_to_screen(position,target.label_anchor);
            float lowest=target.label_anchor_visible ? target.label_anchor.y : -std::numeric_limits<float>::infinity();
            for (size_t bone=0;bone<target.bones.size();++bone) {
                if (target.bone_visible[bone] && target.bones[bone].y>lowest) {
                    target.label_anchor=target.bones[bone];
                    target.label_anchor_visible=true;
                    lowest=target.bones[bone].y;
                }
            }
        }
        if (target.bone_visible[0]) {
            float sx=target.aim.x-width/2.0f;
            float sy=target.aim.y-(game_rect.bottom-game_rect.top)/2.0f;
            target.score=std::sqrt(sx*sx+sy*sy);
        }
        if ((include_esp_details || include_aim) && selected_build==3095) {
            uint8_t visibility=0;
            if (read_at(ped+0x145C,visibility))
                target.invisible_suspected=visibility==0 || visibility==4 || visibility==36;
        }
        if ((include_esp_details || include_aim) && selected_build==3258) {
            uint8_t entity_flags=0;
            if (read_at(ped+0x2C,entity_flags))
                target.invisible_suspected=(entity_flags&0x01)==0;
        }
        if (include_esp_details || !friend_names.empty())
            target.name=resolved_player_name(player_info,&target.name_from_memory);
        if (include_esp_details) {
            target.armor=read_or<float>(ped+ped_armor_offset);
            uintptr_t manager=read_or<uintptr_t>(ped+ped_weapon_manager_offset);
            uintptr_t weapon=read_or<uintptr_t>(manager+0x20);
            std::string weapon_label=equipped_weapon(weapon);
            if (!is_armed_weapon(weapon_label))
                weapon_label=weapon_name_from_hash(read_or<uint32_t>(manager+0x18));
            target.weapon_known=!weapon_label.empty();
            target.armed=is_armed_weapon(weapon_label);
            if (target.armed) target.weapon_name=weapon_label;
            std::transform(target.weapon_name.begin(),target.weapon_name.end(),target.weapon_name.begin(),
                [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
        }
        if (include_esp_details || include_aim) {
            Vec3 from{local_pos.x,local_pos.y,local_pos.z+1.5f};
            Vec3 head=target.bone_visible[0] ? target.hit :
                Vec3{position.x,position.y,position.z+1.6f};
            std::lock_guard<std::mutex> lock(los_mutex);
            auto reading=los_readings.find(ped);
            if (reading!=los_readings.end() &&
                GetTickCount64()-reading->second.sampled_at<600 &&
                los_distance_squared(from,reading->second.from)<0.56f &&
                los_distance_squared(head,reading->second.head)<0.56f)
                target.blocked_by_base_map=reading->second.blocked;
        }
        target.sampled_at=GetTickCount64();
        targets.push_back(std::move(target));
    };
    if (list) for (uint64_t i=0;i<count;++i)
        collect_ped(read_or<uintptr_t>(list+i*0x10),false);
    if (include_esp_details) {
        uintptr_t vehicles=0;
        uint64_t vehicle_count=0;
        if (vehicle_list(replay,vehicles,vehicle_count)) for (uint64_t i=0;i<vehicle_count;++i) {
            uintptr_t vehicle=read_or<uintptr_t>(vehicles+i*0x10);
            if (!vehicle) continue;
            Vec3 position{};
            if (!read_at(vehicle+ped_position_offset,position)) continue;
            float dx=position.x-local_pos.x,dy=position.y-local_pos.y,dz=position.z-local_pos.z;
            float distance=std::sqrt(dx*dx+dy*dy+dz*dz);
            if (!std::isfinite(distance) || distance>app_settings.player_esp_radius_m+10.0f) continue;
            collect_ped(read_or<uintptr_t>(vehicle+vehicle_driver_offset),true);
        }
    }
    if ((include_esp_details || include_aim) && los_running) {
        std::vector<LosRequest> next;
        next.reserve(targets.size());
        for (const Target& target:targets) {
            if ((!target.label_anchor_visible && !target.bone_visible[0]) ||
                target.distance>250.0f) continue;
            Vec3 from{local_pos.x,local_pos.y,local_pos.z+1.5f};
            Vec3 head=target.bone_visible[0] ? target.hit :
                Vec3{target.feet.x,target.feet.y,target.feet.z+1.6f};
            Vec3 torso{target.feet.x,target.feet.y,target.feet.z+1.0f};
            next.push_back({target.ped,from,head,torso,target.distance});
        }
        std::lock_guard<std::mutex> lock(los_mutex);
        los_requests.swap(next);
    }
}

static bool vehicle_list(uintptr_t replay, uintptr_t& list, uint64_t& count) {
    uintptr_t interface_ptr=read_or<uintptr_t>(replay+replay_vehicle_interface_offset);
    list=read_or<uintptr_t>(interface_ptr+0x180);
    count=read_or<uint64_t>(interface_ptr+0x188);
    if (selected_build==3258 && replay_vehicle_interface_offset==0xD10 &&
        (list<=0x10000 || count==0 || count>4096)) {
        interface_ptr=read_or<uintptr_t>(replay+0x10);
        list=read_or<uintptr_t>(interface_ptr+0x180);
        count=read_or<uint64_t>(interface_ptr+0x188);
    }
    return list>0x10000 && count>0 && count<=4096;
}
static std::string vehicle_model_name(uintptr_t vehicle) {
    uintptr_t model_info=read_or<uintptr_t>(vehicle+0x20);
    if (model_info<=0x10000) return {};
    char buffer[25]{};
    if (!read_bytes(model_info+0x298,buffer,24)) return {};
    size_t length=0;
    while (length<24 && buffer[length]) ++length;
    if (length==0 || length==24) return {};
    for (size_t i=0;i<length;++i) {
        unsigned char c=static_cast<unsigned char>(buffer[i]);
        if (c<32 || c>126) return {};
    }
    return std::string(buffer,length);
}
static int vehicle_lock_status(uintptr_t vehicle) {
    int status=-1;
    // b3889 only reports the two meaningful states.
    if (selected_build==3889)
        return read_at(vehicle+vehicle_lock_offset,status) && (status==1 || status==2) ? status : -1;
    return read_at(vehicle+vehicle_lock_offset,status) && status>=0 && status<=10 ? status : -1;
}
static void collect_vehicle_targets() {
    vehicle_targets.clear();
    if (!vehicle_esp_enabled) return;
    uintptr_t world=read_or<uintptr_t>(module_base+world_offset);
    uintptr_t ped=read_or<uintptr_t>(world+0x8);
    uintptr_t replay=read_or<uintptr_t>(module_base+replay_offset);
    Vec3 local{};
    uintptr_t list=0;
    uint64_t count=0;
    if (!read_at(ped+ped_position_offset,local) || !vehicle_list(replay,list,count)) return;
    for (uint64_t i=0;i<count;++i) {
        uintptr_t vehicle=read_or<uintptr_t>(list+i*0x10);
        Vec3 position{};
        if (!vehicle || !read_at(vehicle+ped_position_offset,position)) continue;
        float dx=position.x-local.x,dy=position.y-local.y,dz=position.z-local.z;
        float distance=std::sqrt(dx*dx+dy*dy+dz*dz);
        if (!std::isfinite(distance) || distance<1.0f || distance>app_settings.vehicle_esp_radius_m) continue;
        Screen marker{};
        Vec3 roof{position.x,position.y,position.z+1.2f};
        if (world_to_screen(roof,marker))
            vehicle_targets.push_back({marker,distance,vehicle_model_name(vehicle),vehicle_lock_status(vehicle)});
    }
}

static std::wstring ui_text(const std::string& value) {
    if (value.empty()) return {};
    int length=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.c_str(),-1,nullptr,0);
    UINT code_page=CP_UTF8;
    if (!length) { code_page=CP_ACP; length=MultiByteToWideChar(code_page,0,value.c_str(),-1,nullptr,0); }
    if (!length) return L"?";
    std::wstring result(static_cast<size_t>(length),L'\0');
    MultiByteToWideChar(code_page,code_page==CP_UTF8 ? MB_ERR_INVALID_CHARS : 0,
        value.c_str(),-1,result.data(),length);
    result.pop_back();
    return result;
}
static void refresh_friend_list(uintptr_t selected_info=0) {
    if (!friends_list) return;
    SendMessageW(friends_list,LB_RESETCONTENT,0,0);
    for (size_t i=0;i<friend_candidates.size();++i) {
        const auto& candidate=friend_candidates[i];
        wchar_t distance[32]{};
        std::swprintf(distance,32,L" (%.0f m)",candidate.distance);
        std::wstring label=(marked_friend(candidate.player_info,candidate.name) ? L"[AMIGO] " : L"")+
            (candidate.name.empty() ? L"Nome indisponivel" : ui_text(candidate.name))+distance;
        SendMessageW(friends_list,LB_ADDSTRING,0,reinterpret_cast<LPARAM>(label.c_str()));
        if (selected_info && candidate.player_info==selected_info)
            SendMessageW(friends_list,LB_SETCURSEL,static_cast<WPARAM>(i),0);
    }
    if (friends_status) SetWindowTextW(friends_status,
        friend_candidates.empty() ? L"Nenhum jogador proximo encontrado." :
        L"Selecione um jogador e clique em Marcar / Desmarcar.");
}
static void refresh_friend_candidates() {
    friend_list_last_refresh=GetTickCount64();
    uintptr_t selected_info=0;
    if (friends_list) {
        LRESULT selected=SendMessageW(friends_list,LB_GETCURSEL,0,0);
        if (selected!=LB_ERR && static_cast<size_t>(selected)<friend_candidates.size())
            selected_info=friend_candidates[static_cast<size_t>(selected)].player_info;
    }
    friend_candidates.clear();
    uintptr_t world=read_or<uintptr_t>(module_base+world_offset);
    uintptr_t local_ped=read_or<uintptr_t>(world+0x8);
    uintptr_t replay=read_or<uintptr_t>(module_base+replay_offset);
    uintptr_t interface_ptr=read_or<uintptr_t>(replay+replay_ped_interface_offset);
    uintptr_t list=read_or<uintptr_t>(interface_ptr+0x100);
    uint64_t count=read_or<uint64_t>(interface_ptr+0x108);
    Vec3 local{};
    if (!local_ped || count>4096 || !read_at(local_ped+ped_position_offset,local)) {
        refresh_friend_list(selected_info);
        return;
    }
    std::unordered_set<uintptr_t> seen;
    auto add_candidate=[&](uintptr_t ped) {
        if (!ped || ped==local_ped) return;
        uintptr_t info=read_or<uintptr_t>(ped+ped_player_info_offset);
        if (info<0x10000 || !seen.insert(info).second) return;
        Vec3 pos{};
        if (!read_at(ped+ped_position_offset,pos)) return;
        float dx=pos.x-local.x,dy=pos.y-local.y,dz=pos.z-local.z;
        float distance=std::sqrt(dx*dx+dy*dy+dz*dz);
        if (!std::isfinite(distance) || distance>500.0f) return;
        friend_candidates.push_back({info,resolved_player_name(info),distance});
    };
    if (list) for (uint64_t i=0;i<count;++i)
        add_candidate(read_or<uintptr_t>(list+i*0x10));
    uintptr_t vehicles=0;
    uint64_t vehicle_count=0;
    if (vehicle_list(replay,vehicles,vehicle_count)) for (uint64_t i=0;i<vehicle_count;++i) {
        uintptr_t vehicle=read_or<uintptr_t>(vehicles+i*0x10);
        if (vehicle) add_candidate(read_or<uintptr_t>(vehicle+vehicle_driver_offset));
    }
    std::sort(friend_candidates.begin(),friend_candidates.end(),
        [](const FriendCandidate& a,const FriendCandidate& b){ return a.distance<b.distance; });
    if (friend_candidates.size()>128) friend_candidates.resize(128);
    refresh_friend_list(selected_info);
}
static void toggle_selected_friend() {
    LRESULT selected=SendMessageW(friends_list,LB_GETCURSEL,0,0);
    if (selected==LB_ERR || static_cast<size_t>(selected)>=friend_candidates.size()) {
        SetWindowTextW(friends_status,L"Selecione um jogador da lista.");
        return;
    }
    const auto& candidate=friend_candidates[static_cast<size_t>(selected)];
    if (marked_friend(candidate.player_info,candidate.name)) {
        friend_infos.erase(candidate.player_info);
        friend_names.erase(candidate.name);
    } else {
        friend_infos.insert(candidate.player_info);
        if (!candidate.name.empty())
            friend_names.insert(candidate.name);
    }
    refresh_friend_list(candidate.player_info);
}

static uintptr_t nearest_vehicle(float radius) {
    uintptr_t world=read_or<uintptr_t>(module_base+world_offset);
    uintptr_t local_ped=read_or<uintptr_t>(world+0x8);
    Vec3 local_pos{};
    if (!local_ped || !read_at(local_ped+ped_position_offset,local_pos)) return 0;
    uintptr_t replay=read_or<uintptr_t>(module_base+replay_offset);
    uintptr_t list=0;
    uint64_t count=0;
    vehicle_list(replay,list,count);
    uintptr_t closest=0;
    float best=radius;
    auto consider=[&](uintptr_t vehicle) {
        if (!vehicle || vehicle==local_ped || read_or<uint8_t>(vehicle+0x28)!=3) return;
        Vec3 pos{};
        if (!read_at(vehicle+ped_position_offset,pos)) return;
        float dx=pos.x-local_pos.x,dy=pos.y-local_pos.y,dz=pos.z-local_pos.z;
        float distance=std::sqrt(dx*dx+dy*dy+dz*dz);
        if (std::isfinite(distance) && distance<best) { best=distance; closest=vehicle; }
    };
    for (uint64_t i=0; i<count; ++i) {
        uintptr_t vehicle=read_or<uintptr_t>(list+i*0x10);
        consider(vehicle);
    }
    consider(read_or<uintptr_t>(local_ped+ped_last_vehicle_offset));
    return closest;
}
static void unlock_nearest_vehicle() {
    uintptr_t closest=nearest_vehicle(app_settings.unlock_radius_m);
    if (!closest) {
        char message[80];
        std::snprintf(message,sizeof(message),"Nenhum veiculo a menos de %.0fm",app_settings.unlock_radius_m);
        status_line=message;
    }
    else {
        int unlocked=1;
        bool written=write_at(closest+vehicle_lock_offset,&unlocked,sizeof(unlocked));
        status_line=written && read_or<int>(closest+vehicle_lock_offset)==1 ?
            "Veiculo destrancado" : "Falha ao destrancar veiculo";
    }
    status_until=GetTickCount64()+3000;
}
static void repair_nearest_vehicle() {
    uintptr_t closest=nearest_vehicle(app_settings.unlock_radius_m);
    if (!closest) {
        char message[80];
        std::snprintf(message,sizeof(message),"Nenhum veiculo a menos de %.0fm",app_settings.unlock_radius_m);
        status_line=message;
    } else {
        constexpr float repaired=1000.0f;
        std::array<uintptr_t,3> fields{vehicle_health_offset,0x830,0x910};
        // b3095 only exposes the health field; 3258 and 3889 expose all three.
        size_t field_count=selected_build==3095 ? 1 : fields.size();
        std::array<uintptr_t,4> wheels{};
        std::array<float,4> tyre_health{};
        bool wheels_valid=selected_build==3258;
        if (wheels_valid) {
            for (size_t i=0;i<wheels.size();++i) {
                wheels[i]=read_or<uintptr_t>(closest+0x1990+i*sizeof(uintptr_t));
                float marker_a=0.0f,marker_b=0.0f,condition=0.0f;
                if (!wheels[i] || std::find(wheels.begin(),wheels.begin()+i,wheels[i])!=wheels.begin()+i ||
                    !read_at(wheels[i],marker_a) || !read_at(wheels[i]+0x18,marker_b) ||
                    !read_at(wheels[i]+0x1E8,condition) || !read_at(wheels[i]+0x1EC,tyre_health[i]) ||
                    !std::isfinite(condition) || condition<0.0f || condition>1000.0f ||
                    !std::isfinite(tyre_health[i]) || tyre_health[i]<0.0f || tyre_health[i]>1000.0f ||
                    std::abs(marker_a-1.0f)>0.01f || std::abs(marker_b-1.0f)>0.01f) {
                    wheels_valid=false;
                    break;
                }
            }
        }
        bool plausible=true;
        for (size_t i=0;i<field_count;++i) {
            float current=0.0f;
            plausible=read_at(closest+fields[i],current) && std::isfinite(current) &&
                current>=-4000.0f && current<=10000.0f;
            if (!plausible) break;
        }
        if (!plausible) status_line="Campo de reparo do veiculo invalido";
        else {
            size_t restored=0;
            for (size_t i=0;i<field_count;++i) {
                float observed=0.0f;
                bool written=write_at(closest+fields[i],&repaired,sizeof(repaired));
                bool verified=written && read_at(closest+fields[i],observed) &&
                    std::isfinite(observed) && std::abs(observed-repaired)<0.01f;
                if (verified) ++restored;
            }
            size_t tyres_needed=0,tyres_restored=0;
            if (wheels_valid) {
                for (size_t i=0;i<wheels.size();++i) {
                    if (tyre_health[i]>=999.99f) continue;
                    ++tyres_needed;
                    float observed=0.0f;
                    bool written=write_at(wheels[i]+0x1EC,&repaired,sizeof(repaired));
                    bool verified=written && read_at(wheels[i]+0x1EC,observed) &&
                        std::isfinite(observed) && std::abs(observed-repaired)<0.01f;
                    if (verified) ++tyres_restored;
                }
            }
            if (restored!=field_count || tyres_restored!=tyres_needed)
                status_line="Reparo parcial: confira o veiculo";
            else if (selected_build==3095)
                status_line="Vida do veiculo restaurada (reparo basico)";
            else if (!wheels_valid)
                status_line="Chassi, lataria e motor ok; pneus nao verificados";
            else if (tyres_needed)
                status_line="Chassi, lataria e motor ok; pneu restaurado na memoria";
            else status_line="Chassi, lataria, motor e pneus verificados";
        }
    }
    status_until=GetTickCount64()+3000;
}

static bool relative_jump(uintptr_t from, uintptr_t to, int32_t& out) {
    int64_t d = static_cast<int64_t>(to) - static_cast<int64_t>(from + 5);
    if (d < std::numeric_limits<int32_t>::min() || d > std::numeric_limits<int32_t>::max()) return false;
    out = static_cast<int32_t>(d);
    return true;
}
static uintptr_t allocate_near(uintptr_t site) {
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    uintptr_t gran = info.dwAllocationGranularity;
    uintptr_t start = site & ~(gran - 1);
    for (uintptr_t delta = gran; delta < 0x70000000; delta += gran) {
        for (int side : {1, -1}) {
            uintptr_t candidate = side == 1 ? start + delta : start - delta;
            if (side == -1 && delta > start) continue;
            void* p = VirtualAllocEx(process_handle, reinterpret_cast<void*>(candidate), 0x1000,
                                     MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
            if (p) return reinterpret_cast<uintptr_t>(p);
        }
    }
    return 0;
}

static bool prepare_hook() {
    hook_address = module_base + bullet_offset;
    static constexpr uint8_t expected[17] = {
        0xF3,0x41,0x0F,0x10,0x19, 0xF3,0x41,0x0F,0x10,0x41,0x04,
        0xF3,0x41,0x0F,0x10,0x51,0x08
    };
    SIZE_T n = 0;
    if (!ReadProcessMemory(process_handle, reinterpret_cast<void*>(hook_address), original_code.data(), 17, &n) || n != 17) return false;
    if (std::memcmp(original_code.data(), expected, 17) != 0) return false;
    code_cave = allocate_near(hook_address);
    if (!code_cave) return false;
    // Keep executable instructions fixed; only the 12-byte target data changes.
    std::vector<uint8_t> stub{0x50}; // push rax
    std::array<size_t, 3> load_offsets{};
    for (int i=0; i<3; ++i) {
        load_offsets[i] = stub.size();
        stub.insert(stub.end(), {0x8B,0x05,0,0,0,0}); // mov eax,[rip+disp32]
        if (i == 0) stub.insert(stub.end(), {0x41,0x89,0x01});
        else stub.insert(stub.end(), {0x41,0x89,0x41,static_cast<uint8_t>(i*4)});
    }
    stub.push_back(0x58); // pop rax
    stub.insert(stub.end(), original_code.begin(), original_code.end());
    size_t jump_offset = stub.size();
    stub.insert(stub.end(), {0xE9,0,0,0,0});
    while (stub.size() % 4) stub.push_back(0x90);
    target_data_address = code_cave + stub.size();
    stub.resize(stub.size()+12, 0);
    for (int i=0; i<3; ++i) {
        int64_t delta = static_cast<int64_t>(target_data_address + i*4) -
            static_cast<int64_t>(code_cave + load_offsets[i] + 6);
        if (delta < std::numeric_limits<int32_t>::min() || delta > std::numeric_limits<int32_t>::max()) return false;
        int32_t displacement = static_cast<int32_t>(delta);
        std::memcpy(stub.data()+load_offsets[i]+2,&displacement,4);
    }
    int32_t back = 0;
    if (!relative_jump(code_cave+jump_offset, hook_address+original_code.size(), back)) return false;
    std::memcpy(stub.data()+jump_offset+1,&back,4);
    hook_supported = write_at(code_cave,stub.data(),stub.size());
    return hook_supported;
}

static bool apply_hook(Vec3 p) {
    if (!hook_supported) return false;
    if (!write_at(target_data_address,&p,sizeof(p))) return false;
    if (!hooked) {
        int32_t forward = 0;
        if (!relative_jump(hook_address, code_cave, forward)) return false;
        uint8_t jump[5]{0xE9};
        std::memcpy(jump+1, &forward, 4);
        if (!write_at(hook_address, jump, sizeof(jump))) return false;
        hooked = true;
    }
    return true;
}
static bool restore_hook() {
    if (!hooked) return true;
    if (!process_handle || WaitForSingleObject(process_handle,0)!=WAIT_TIMEOUT) {
        hooked=false;
        return true;
    }
    if (!write_at(hook_address,original_code.data(),5)) return false;
    std::array<uint8_t,5> restored{};
    if (!read_at(hook_address,restored) ||
        !std::equal(restored.begin(),restored.end(),original_code.begin())) return false;
    hooked=false;
    return true;
}

static bool aim_target_allowed(const Target& target,uintptr_t aimed_ped,bool aimed_ped_valid) {
    return target.health>0.0f && (!aimed_ped_valid || target.ped==aimed_ped) &&
        !target.invisible_suspected && !target.blocked_by_base_map &&
        !marked_friend(target.player_info,target.name);
}

static float effective_aim_radius_px(const Target* target=nullptr) {
    float fov=app_settings.aim_fov_px;
    float strict_radius=std::min(fov,7.0f);
    if (target) {
        Screen feet_screen{};
        if (world_to_screen(target->feet,feet_screen)) {
            float dx=feet_screen.x-target->aim.x,dy=feet_screen.y-target->aim.y;
            float body_height=std::sqrt(dx*dx+dy*dy);
            strict_radius=std::min(strict_radius,std::clamp(body_height*0.25f,1.5f,7.0f));
        }
    }
    return fov+(strict_radius-fov)*(app_settings.aim_smooth_percent/100.0f);
}

static Vec3 smoothed_aim_point(const Target* target,ULONGLONG now) {
    static uintptr_t tracked_ped=0;
    static Vec3 filtered{};
    static ULONGLONG last_update=0;
    if (!target) {
        tracked_ped=0;
        last_update=0;
        return {};
    }
    float smooth=app_settings.aim_smooth_percent;
    if (tracked_ped!=target->ped || !last_update || smooth<=0) {
        tracked_ped=target->ped;
        filtered=target->hit;
        last_update=now;
        return filtered;
    }
    float elapsed=static_cast<float>(std::clamp<ULONGLONG>(now-last_update,1,50));
    float response_ms=10.0f+smooth*1.4f;
    float alpha=1.0f-std::exp(-elapsed/response_ms);
    filtered.x+=alpha*(target->hit.x-filtered.x);
    filtered.y+=alpha*(target->hit.y-filtered.y);
    filtered.z+=alpha*(target->hit.z-filtered.z);
    float dx=target->hit.x-filtered.x,dy=target->hit.y-filtered.y,dz=target->hit.z-filtered.z;
    float lag=std::sqrt(dx*dx+dy*dy+dz*dz);
    if (lag>0.25f) {
        float scale=0.25f/lag;
        filtered={target->hit.x-dx*scale,target->hit.y-dy*scale,target->hit.z-dz*scale};
    }
    last_update=now;
    return filtered;
}

static std::wstring key_label(int key) {
    if (key==0) return L"Nenhuma";
    if (key>=VK_F1 && key<=VK_F24) return L"F"+std::to_wstring(key-VK_F1+1);
    if (key>='A' && key<='Z') return std::wstring(1,static_cast<wchar_t>(key));
    if (key>='0' && key<='9') return std::wstring(1,static_cast<wchar_t>(key));
    switch(key) {
        case VK_LBUTTON: return L"Mouse esquerdo";
        case VK_RBUTTON: return L"Mouse direito";
        case VK_MBUTTON: return L"Mouse central";
        case VK_XBUTTON1: return L"Mouse X1";
        case VK_XBUTTON2: return L"Mouse X2";
        case VK_MENU: return L"Alt";
        case VK_CONTROL: return L"Ctrl";
        case VK_SHIFT: return L"Shift";
        case VK_INSERT: return L"Insert";
        case VK_DELETE: return L"Delete";
        case VK_HOME: return L"Home";
        case VK_END: return L"End";
        case VK_PRIOR: return L"Page Up";
        case VK_NEXT: return L"Page Down";
        case VK_SPACE: return L"Espaco";
        case VK_TAB: return L"Tab";
        case VK_UP: return L"Seta cima";
        case VK_DOWN: return L"Seta baixo";
        case VK_LEFT: return L"Seta esquerda";
        case VK_RIGHT: return L"Seta direita";
        default: {
            wchar_t text[16];
            std::swprintf(text,16,L"VK 0x%02X",key);
            return text;
        }
    }
}
static void refresh_bind_button(size_t index) {
    if (index<bind_fields.size() && bind_fields[index].button)
        SetWindowTextW(bind_fields[index].button,key_label(bind_fields[index].selected_key).c_str());
}
static void cancel_bind_capture() {
    if (capturing_bind>=0) refresh_bind_button(static_cast<size_t>(capturing_bind));
    capturing_bind=-1;
    capture_ready=false;
    if (settings_window && GetCapture()==settings_window) ReleaseCapture();
}
static LRESULT CALLBACK passive_edit_proc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp) {
    if (msg==WM_LBUTTONDOWN) {
        active_edit=hwnd;
        wchar_t buffer[64]{};
        GetWindowTextW(hwnd,buffer,64);
        active_edit_original=buffer;
        replace_edit_text=true;
        cancel_bind_capture();
    }
    return CallWindowProcW(original_edit_proc,hwnd,msg,wp,lp);
}
static void track_passive_edit(HWND hwnd) {
    WNDPROC previous=reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
        hwnd,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(passive_edit_proc)));
    if (!original_edit_proc) original_edit_proc=previous;
}
static void poll_passive_edit(const std::array<bool,256>& pressed) {
    if (!active_edit || !IsWindowVisible(settings_window)) return;
    if (pressed[VK_RETURN]) { active_edit=nullptr; return; }
    if (pressed[VK_ESCAPE]) {
        SetWindowTextW(active_edit,active_edit_original.c_str());
        active_edit=nullptr; return;
    }
    bool is_offset=std::any_of(offset_fields.begin(),offset_fields.end(),
        [](const OffsetField& field){ return field.edit==active_edit; });
    wchar_t buffer[64]{};
    GetWindowTextW(active_edit,buffer,64);
    std::wstring value=buffer;
    if (pressed[VK_BACK]) {
        if (replace_edit_text) value=is_offset ? L"0x" : L"";
        else if (value.size()>(is_offset ? 2U : 0U)) value.pop_back();
        replace_edit_text=false;
        SetWindowTextW(active_edit,value.c_str());
        return;
    }
    wchar_t character=0;
    for (int key='0';key<='9';++key) if (pressed[key]) { character=static_cast<wchar_t>(key); break; }
    if (!character) for (int key=VK_NUMPAD0;key<=VK_NUMPAD9;++key)
        if (pressed[key]) { character=static_cast<wchar_t>(L'0'+key-VK_NUMPAD0); break; }
    if (!character && is_offset) for (int key='A';key<='F';++key)
        if (pressed[key]) { character=static_cast<wchar_t>(key); break; }
    if (!character && !is_offset && pressed[VK_OEM_PERIOD]) character=L'.';
    if (!character && !is_offset && pressed[VK_OEM_MINUS]) character=L'-';
    if (!character) return;
    if (replace_edit_text) value=is_offset ? L"0x" : L"";
    replace_edit_text=false;
    if (value.size()<32 && (character!=L'.' || value.find(L'.')==std::wstring::npos) &&
        (character!=L'-' || value.empty())) {
        value.push_back(character);
        SetWindowTextW(active_edit,value.c_str());
    }
}
static HWND slider_edit(size_t index) {
    return index<radius_edits.size() ? radius_edits[index] : index==4 ? fov_edit : smooth_edit;
}
static float slider_min(size_t index) {
    static constexpr std::array<float,6> minimum{5,5,5,1,10,0};
    return minimum[index];
}
static float slider_max(size_t index) {
    static constexpr std::array<float,6> maximum{1000,1000,1000,50,1000,100};
    return maximum[index];
}
static float slider_value(size_t index) {
    HWND edit=slider_edit(index);
    wchar_t input[32]{};
    if (edit) GetWindowTextW(edit,input,32);
    wchar_t* end=nullptr;
    float value=std::wcstof(input,&end);
    if (end==input || !std::isfinite(value)) {
        const std::array<float,6> defaults{app_settings.aim_radius_m,app_settings.player_esp_radius_m,
            app_settings.vehicle_esp_radius_m,app_settings.unlock_radius_m,app_settings.aim_fov_px,
            app_settings.aim_smooth_percent};
        value=defaults[index];
    }
    return std::clamp(value,slider_min(index),slider_max(index));
}
static int slider_x(size_t index) { return index<3 ? 230 : 572; }
static int slider_row(size_t index) { return 120+static_cast<int>(index%3)*145; }
static void set_slider_from_x(size_t index,int x) {
    float progress=std::clamp((x-slider_x(index))/292.0f,0.0f,1.0f);
    float value=slider_min(index)+progress*(slider_max(index)-slider_min(index));
    wchar_t text[32];
    std::swprintf(text,32,L"%.0f",value);
    SetWindowTextW(slider_edit(index),text);
    active_edit=nullptr;
    InvalidateRect(settings_window,nullptr,FALSE);
}
static void select_settings_tab(int tab) {
    active_edit=nullptr;
    settings_tab=tab;
    for (HWND control:general_controls) ShowWindow(control,tab==0 ? SW_SHOWNA : SW_HIDE);
    for (HWND control:radius_controls) ShowWindow(control,tab==1 ? SW_SHOWNA : SW_HIDE);
    for (HWND control:offset_controls) ShowWindow(control,tab==2 ? SW_SHOWNA : SW_HIDE);
    for (HWND control:esp_controls) ShowWindow(control,tab==3 ? SW_SHOWNA : SW_HIDE);
    if (settings_window) InvalidateRect(settings_window,nullptr,TRUE);
    for (HWND button:{general_tab_button,esp_tab_button,radius_tab_button,offsets_tab_button})
        if (button) InvalidateRect(button,nullptr,TRUE);
}
static void show_settings_tab(bool offsets) { select_settings_tab(offsets ? 2 : 0); }
static void show_radius_tab() { select_settings_tab(1); }
static void refresh_feature_checks() {
    for (HWND control:feature_checks) if (control) InvalidateRect(control,nullptr,FALSE);
}
static float parse_float_edit(HWND edit,const char* label) {
    wchar_t input[32]{};
    GetWindowTextW(edit,input,32);
    std::wstring value(input);
    size_t consumed=0;
    float result=std::stof(value,&consumed);
    if (consumed!=value.size()) throw std::runtime_error(std::string("Valor invalido: ")+label);
    return result;
}

static void apply_settings() {
    std::array<uintptr_t, 20> values{};
    try {
        for (size_t i=0; i<offset_fields.size(); ++i) {
            wchar_t input[64]{};
            GetWindowTextW(offset_fields[i].edit,input,64);
            std::wstring value(input);
            size_t consumed = 0;
            values[i] = static_cast<uintptr_t>(std::stoull(value,&consumed,0));
            if (consumed != value.size() || values[i] == 0) throw std::runtime_error("Offset invalido");
        }
        AppSettings candidate=app_settings;
        for (const auto& field:bind_fields) {
            candidate.*(field.value)=field.selected_key;
        }
        candidate.aim_fov_px=parse_float_edit(fov_edit,"FOV");
        candidate.aim_smooth_percent=parse_float_edit(smooth_edit,"suavizacao");
        candidate.aim_radius_m=parse_float_edit(radius_edits[0],"raio do silent aim");
        candidate.player_esp_radius_m=parse_float_edit(radius_edits[1],"raio do ESP jogadores");
        candidate.vehicle_esp_radius_m=parse_float_edit(radius_edits[2],"raio do ESP veiculos");
        candidate.unlock_radius_m=parse_float_edit(radius_edits[3],"raio para destrancar");
        if (!valid_app_settings(candidate))
            throw std::runtime_error("Teclas repetidas ou raios fora do limite. F2 fica reservado para o menu.");
        bool offsets_changed=false;
        for (size_t i=0;i<offset_fields.size();++i)
            if (values[i]!=*offset_fields[i].value) offsets_changed=true;
        if (offsets_changed) {
            if (!restore_hook()) throw std::runtime_error("Falha ao restaurar hook anterior");
            if (code_cave) Sleep(100);
            if (code_cave && !VirtualFreeEx(process_handle,reinterpret_cast<void*>(code_cave),0,MEM_RELEASE))
                throw std::runtime_error("Falha ao liberar memoria do hook anterior");
            code_cave = 0;
            hook_supported = false;
            for (size_t i=0; i<offset_fields.size(); ++i) *offset_fields[i].value = values[i];
            if (profile_ready) prepare_hook();
        }
        app_settings=candidate;
        if (!app_settings.aim_key) restore_hook();
        cancel_bind_capture();
        active_edit=nullptr;
        ShowWindow(settings_window,SW_HIDE);
    } catch (const std::exception& e) {
        MessageBoxA(settings_window,e.what(),"Configuracao",MB_OK|MB_ICONERROR);
    }
}

static void show_friends_window();
static void paint_round_rect(HDC dc,int left,int top,int right,int bottom,
                             COLORREF fill,COLORREF border,int radius=12) {
    HBRUSH brush=CreateSolidBrush(fill);
    HPEN pen=CreatePen(PS_SOLID,1,border);
    HGDIOBJ old_brush=SelectObject(dc,brush),old_pen=SelectObject(dc,pen);
    RoundRect(dc,left,top,right,bottom,radius,radius);
    SelectObject(dc,old_brush); SelectObject(dc,old_pen);
    DeleteObject(brush); DeleteObject(pen);
}
static void paint_settings(HDC dc) {
    RECT client{};
    GetClientRect(settings_window,&client);
    FillRect(dc,&client,menu_brush);
    HGDIOBJ old_font=SelectObject(dc,menu_font);
    SetBkMode(dc,TRANSPARENT);
    auto draw=[&](int x,int y,const wchar_t* value,COLORREF color) {
        SetTextColor(dc,color);
        TextOutW(dc,x,y,value,static_cast<int>(std::wcslen(value)));
    };
    constexpr COLORREF panel=RGB(21,20,34);
    constexpr COLORREF border=RGB(36,37,55);
    constexpr COLORREF muted=RGB(139,147,171);
    constexpr COLORREF bright=RGB(235,239,248);
    constexpr COLORREF accent=RGB(34,172,237);
    paint_round_rect(dc,11,10,200,56,RGB(18,18,29),border,12);
    HPEN mark_pen=CreatePen(PS_SOLID,3,accent);
    HGDIOBJ old_pen=SelectObject(dc,mark_pen);
    MoveToEx(dc,27,39,nullptr); LineTo(dc,38,21);
    LineTo(dc,49,39);
    SelectObject(dc,old_pen); DeleteObject(mark_pen);
    SelectObject(dc,menu_title_font);
    draw(57,15,L"DASKE",bright);
    SelectObject(dc,menu_small_font);
    draw(221,15,L"PAINEL DE CONTROLE",muted);
    draw(221,36,L"FiveM  /  ajustes da sessao",RGB(90,101,126));
    std::wstring build=L"BUILD "+std::to_wstring(selected_build);
    draw(794,17,build.c_str(),RGB(135,194,227));
    paint_round_rect(dc,12,75,200,564,RGB(17,17,28),border,15);
    paint_round_rect(dc,214,75,546,564,panel,border,15);
    paint_round_rect(dc,556,75,888,564,panel,border,15);
    draw(28,87,L"NAVEGACAO",RGB(90,101,126));
    draw(28,512,L"F2  MENU",RGB(112,126,151));
    draw(28,533,L"Esc  fecha",RGB(84,95,117));
    SelectObject(dc,menu_font);
    if (settings_tab==0) {
        draw(231,95,L"Atalhos",bright);
        draw(572,95,L"Visuais",bright);
        for (int i=0;i<7;++i) {
            int y=116+i*55;
            if (i) {
                HBRUSH line=CreateSolidBrush(border);
                RECT separator{229,y-7,531,y-6}; FillRect(dc,&separator,line); DeleteObject(line);
            }
        }
        SelectObject(dc,menu_small_font);
        draw(572,275,L"Os visuais podem ser ligados aqui",muted);
        draw(572,296,L"ou pelas teclas configuradas.",muted);
        draw(572,340,L"Binds vazios nao acionam funcoes.",muted);
        draw(572,360,L"Clique em uma tecla para captura-la.",muted);
        draw(572,380,L"Backspace limpa o bind.",muted);
    } else if (settings_tab==1) {
        draw(231,95,L"Alcance",bright);
        draw(572,95,L"Mira e utilitarios",bright);
        static constexpr std::array<const wchar_t*,6> labels{
            L"Silent aim",L"ESP jogadores",L"ESP veiculos",
            L"Destrancar / fix",L"Circulo FOV",L"Suavizacao / precisao"};
        for (size_t i=0;i<labels.size();++i) {
            int x=slider_x(i),y=slider_row(i);
            draw(x,y,labels[i],bright);
            float progress=(slider_value(i)-slider_min(i))/(slider_max(i)-slider_min(i));
            int knob=x+static_cast<int>(progress*292.0f);
            HBRUSH track=CreateSolidBrush(RGB(50,50,69));
            RECT track_rect{x,y+44,x+292,y+49}; FillRect(dc,&track_rect,track); DeleteObject(track);
            HBRUSH fill=CreateSolidBrush(accent);
            RECT progress_rect{x,y+44,knob,y+49}; FillRect(dc,&progress_rect,fill);
            HGDIOBJ old_brush=SelectObject(dc,fill);
            HGDIOBJ knob_pen=SelectObject(dc,GetStockObject(NULL_PEN));
            Ellipse(dc,knob-6,y+40,knob+6,y+52);
            SelectObject(dc,knob_pen); SelectObject(dc,old_brush); DeleteObject(fill);
            wchar_t range[64]{};
            if (i==5) std::swprintf(range,64,L"0 livre  /  100 mira na cabeca");
            else std::swprintf(range,64,L"%.0f - %.0f %s",slider_min(i),slider_max(i),
                i==4 ? L"px" : L"m");
            SelectObject(dc,menu_small_font);
            draw(x,y+59,range,muted);
            SelectObject(dc,menu_font);
            if (i%3!=2) {
                HBRUSH line=CreateSolidBrush(border);
                RECT separator{x,y+111,x+292,y+112}; FillRect(dc,&separator,line); DeleteObject(line);
            }
        }
    } else if (settings_tab==2) {
        draw(231,95,L"Ponteiros principais",bright);
        draw(572,95,L"Entidades e veiculos",bright);
        SelectObject(dc,menu_small_font);
        draw(231,541,L"Valores ativos da build selecionada",muted);
    } else {
        draw(231,95,L"ESP de jogadores",bright);
        draw(572,95,L"ESP de veiculos",bright);
        SelectObject(dc,menu_small_font);
        draw(231,527,L"Selecione os detalhes exibidos na tela.",muted);
        paint_round_rect(dc,572,323,872,540,RGB(17,18,31),RGB(43,46,67),12);
        draw(588,338,L"PREVIA EM TEMPO REAL",RGB(105,148,179));
        HPEN preview_pen=CreatePen(PS_SOLID,2,RGB(200,224,238));
        HGDIOBJ preview_old_pen=SelectObject(dc,preview_pen);
        HGDIOBJ preview_old_brush=SelectObject(dc,GetStockObject(HOLLOW_BRUSH));
        if (esp_visibility.head_circle) Ellipse(dc,610,374,626,390);
        if (esp_visibility.skeleton) {
            MoveToEx(dc,618,391,nullptr); LineTo(dc,618,451);
            MoveToEx(dc,597,405,nullptr); LineTo(dc,639,405);
            MoveToEx(dc,597,405,nullptr); LineTo(dc,591,429);
            MoveToEx(dc,639,405,nullptr); LineTo(dc,645,429);
            MoveToEx(dc,618,451,nullptr); LineTo(dc,602,488);
            MoveToEx(dc,618,451,nullptr); LineTo(dc,634,488);
        }
        SelectObject(dc,preview_old_brush); SelectObject(dc,preview_old_pen); DeleteObject(preview_pen);
        int preview_y=385;
        if (esp_visibility.name || esp_visibility.distance) {
            std::wstring label=esp_visibility.name ? L"JOGADOR" : L"";
            if (esp_visibility.distance) label+=(label.empty() ? L"" : L"  ")+std::wstring(L"24m");
            draw(666,preview_y,label.c_str(),bright); preview_y+=22;
        }
        if (esp_visibility.health) { draw(666,preview_y,L"HP 100",RGB(119,225,145)); preview_y+=19; }
        if (esp_visibility.armor) { draw(666,preview_y,L"Colete 50",RGB(103,179,255)); preview_y+=19; }
        if (esp_visibility.weapon) { draw(666,preview_y,L"weapon_pistol",RGB(185,199,217)); preview_y+=19; }
        if (esp_visibility.noclip) draw(666,preview_y,L"NoClip?",RGB(255,113,126));
    }
    HBRUSH divider=CreateSolidBrush(border);
    RECT footer_line{214,580,888,581}; FillRect(dc,&footer_line,divider); DeleteObject(divider);
    SelectObject(dc,menu_small_font);
    draw(597,608,L"F2 FECHA",muted);
    SelectObject(dc,old_font);
}
static void draw_settings_button(const DRAWITEMSTRUCT& item) {
    HDC dc=item.hDC;
    RECT bounds=item.rcItem;
    int id=static_cast<int>(item.CtlID);
    bool nav=id>=110 && id<=113;
    bool selected=(id==110 && settings_tab==0) || (id==111 && settings_tab==1) ||
                  (id==112 && settings_tab==2) || (id==113 && settings_tab==3);
    bool toggle=(id>=400 && id<403) || (id>=500 && id<513);
    bool pressed=(item.itemState&ODS_SELECTED)!=0;
    bool enabled=false;
    if (id==400) enabled=player_esp_enabled;
    if (id==401) enabled=vehicle_esp_enabled;
    if (id==402) enabled=fov_circle_enabled;
    if (id>=500 && id<513) enabled=esp_visibility.*(esp_fields[id-500].value);
    wchar_t label[128]{};
    GetWindowTextW(item.hwndItem,label,128);
    HGDIOBJ old_font=SelectObject(dc,menu_font);
    SetBkMode(dc,TRANSPARENT);
    if (nav) {
        paint_round_rect(dc,bounds.left,bounds.top,bounds.right,bounds.bottom,
            selected ? RGB(28,42,62) : pressed ? RGB(33,33,51) : RGB(23,23,38),
            selected ? RGB(42,102,143) : RGB(33,34,51),12);
        if (selected) {
            HBRUSH stripe=CreateSolidBrush(RGB(35,178,241));
            RECT stripe_rect{bounds.left+1,bounds.top+8,bounds.left+4,bounds.bottom-8};
            FillRect(dc,&stripe_rect,stripe); DeleteObject(stripe);
        }
        SetTextColor(dc,selected ? RGB(238,246,252) : RGB(144,153,176));
        RECT text_rect{bounds.left+17,bounds.top,bounds.right-9,bounds.bottom};
        DrawTextW(dc,label,-1,&text_rect,DT_LEFT|DT_VCENTER|DT_SINGLELINE);
    } else if (toggle) {
        HBRUSH background=CreateSolidBrush(RGB(21,20,34));
        FillRect(dc,&bounds,background); DeleteObject(background);
        SetTextColor(dc,enabled ? RGB(233,237,248) : RGB(149,158,178));
        RECT text_rect{bounds.left+1,bounds.top,bounds.right-39,bounds.bottom};
        DrawTextW(dc,label,-1,&text_rect,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS);
        int x=bounds.right-26,y=(bounds.top+bounds.bottom)/2;
        paint_round_rect(dc,x-9,y-9,x+9,y+9,
            enabled ? RGB(31,167,235) : RGB(38,39,56),
            enabled ? RGB(54,194,252) : RGB(61,63,82),6);
        if (enabled) {
            HPEN check_pen=CreatePen(PS_SOLID,2,RGB(248,252,255));
            HGDIOBJ old_pen=SelectObject(dc,check_pen);
            MoveToEx(dc,x-5,y,nullptr); LineTo(dc,x-1,y+4); LineTo(dc,x+6,y-5);
            SelectObject(dc,old_pen); DeleteObject(check_pen);
        }
    } else {
        COLORREF background=id==100 ? RGB(34,171,237) :
            id==102 ? RGB(65,36,49) :
            pressed ? RGB(48,51,72) : RGB(31,32,49);
        COLORREF border=id==100 ? RGB(74,203,250) :
            id==102 ? RGB(107,62,75) : RGB(48,50,70);
        paint_round_rect(dc,bounds.left,bounds.top,bounds.right,bounds.bottom,background,border,10);
        SetTextColor(dc,id==100 ? RGB(8,21,33) :
            id==102 ? RGB(255,192,199) :
            (id>=200 && id<207 && *label && std::wcscmp(label,L"Nenhuma")!=0) ?
                RGB(129,212,248) : RGB(222,229,242));
        DrawTextW(dc,label,-1,&bounds,DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS);
    }
    SelectObject(dc,old_font);
}
static LRESULT CALLBACK settings_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg==WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if (msg==WM_NCHITTEST) {
        POINT point{static_cast<short>(LOWORD(lp)),static_cast<short>(HIWORD(lp))};
        ScreenToClient(hwnd,&point);
        if (point.y>=0 && point.y<65) return HTCAPTION;
    }
    if (msg==WM_ERASEBKGND) return 1;
    if (msg==WM_PAINT) {
        PAINTSTRUCT paint{};
        HDC dc=BeginPaint(hwnd,&paint);
        RECT client{};
        GetClientRect(hwnd,&client);
        HDC buffer=CreateCompatibleDC(dc);
        HBITMAP bitmap=CreateCompatibleBitmap(dc,client.right,client.bottom);
        if (buffer && bitmap) {
            HGDIOBJ old_bitmap=SelectObject(buffer,bitmap);
            paint_settings(buffer);
            BitBlt(dc,0,0,client.right,client.bottom,buffer,0,0,SRCCOPY);
            SelectObject(buffer,old_bitmap);
        } else paint_settings(dc);
        if (bitmap) DeleteObject(bitmap);
        if (buffer) DeleteDC(buffer);
        EndPaint(hwnd,&paint);
        return 0;
    }
    if (msg==WM_DRAWITEM) {
        auto* item=reinterpret_cast<DRAWITEMSTRUCT*>(lp);
        if (item && item->CtlType==ODT_BUTTON) { draw_settings_button(*item); return TRUE; }
    }
    if (msg==WM_CTLCOLORSTATIC) {
        HDC dc=reinterpret_cast<HDC>(wp);
        SetTextColor(dc,RGB(183,203,219)); SetBkMode(dc,TRANSPARENT);
        return reinterpret_cast<LRESULT>(GetStockObject(HOLLOW_BRUSH));
    }
    if (msg==WM_CTLCOLORBTN) {
        HDC dc=reinterpret_cast<HDC>(wp);
        SetTextColor(dc,RGB(225,234,243)); SetBkColor(dc,RGB(13,19,28));
        return reinterpret_cast<LRESULT>(menu_brush);
    }
    if (msg==WM_CTLCOLOREDIT || msg==WM_CTLCOLORLISTBOX) {
        HDC dc=reinterpret_cast<HDC>(wp);
        SetTextColor(dc,RGB(239,245,250)); SetBkColor(dc,RGB(31,43,58));
        return reinterpret_cast<LRESULT>(edit_brush);
    }
    if (capturing_bind>=0) {
        int mouse_key=0;
        if (msg==WM_LBUTTONDOWN) mouse_key=VK_LBUTTON;
        if (msg==WM_RBUTTONDOWN) mouse_key=VK_RBUTTON;
        if (msg==WM_MBUTTONDOWN) mouse_key=VK_MBUTTON;
        if (msg==WM_XBUTTONDOWN) {
            if (HIWORD(wp)==XBUTTON1) mouse_key=VK_XBUTTON1;
            if (HIWORD(wp)==XBUTTON2) mouse_key=VK_XBUTTON2;
        }
        if (mouse_key) {
            bind_fields[capturing_bind].selected_key=mouse_key;
            cancel_bind_capture();
            return msg==WM_XBUTTONDOWN ? TRUE : 0;
        }
        if (msg==WM_CAPTURECHANGED || msg==WM_CANCELMODE) {
            cancel_bind_capture();
            return 0;
        }
    }
    if (settings_tab==1 && msg==WM_LBUTTONDOWN) {
        int x=static_cast<short>(LOWORD(lp)),y=static_cast<short>(HIWORD(lp));
        for (int i=0;i<6;++i) {
            int track_x=slider_x(i),row=slider_row(i);
            if (x>=track_x-4 && x<=track_x+296 && y>=row+37 && y<=row+55) {
                dragging_slider=i;
                SetCapture(hwnd);
                set_slider_from_x(static_cast<size_t>(i),x);
                return 0;
            }
        }
    }
    if (msg==WM_MOUSEMOVE && dragging_slider>=0 && (wp&MK_LBUTTON)) {
        set_slider_from_x(static_cast<size_t>(dragging_slider),static_cast<short>(LOWORD(lp)));
        return 0;
    }
    if (msg==WM_LBUTTONUP && dragging_slider>=0) {
        dragging_slider=-1;
        if (GetCapture()==hwnd) ReleaseCapture();
        return 0;
    }
    if (msg==WM_CAPTURECHANGED && dragging_slider>=0) dragging_slider=-1;
    if (msg == WM_COMMAND) {
        int id=LOWORD(wp);
        if (id==110 && HIWORD(wp)==BN_CLICKED) { show_settings_tab(false); return 0; }
        if (id==111 && HIWORD(wp)==BN_CLICKED) { show_radius_tab(); return 0; }
        if (id==112 && HIWORD(wp)==BN_CLICKED) { show_settings_tab(true); return 0; }
        if (id==113 && HIWORD(wp)==BN_CLICKED) { select_settings_tab(3); return 0; }
        if (settings_tab==1 && HIWORD(wp)==EN_CHANGE) InvalidateRect(hwnd,nullptr,FALSE);
        if (HIWORD(wp)==BN_CLICKED) active_edit=nullptr;
        if (HIWORD(wp)==BN_CLICKED && id>=200 && id<207) {
            cancel_bind_capture();
            capturing_bind=id-200;
            capture_ready=false;
            SetWindowTextW(bind_fields[capturing_bind].button,L"Tecla ou mouse...");
            SetCapture(hwnd);
            return 0;
        }
        if (HIWORD(wp)==BN_CLICKED && id>=300 && id<307) {
            size_t index=static_cast<size_t>(id-300);
            if (capturing_bind==static_cast<int>(index)) cancel_bind_capture();
            bind_fields[index].selected_key=0;
            refresh_bind_button(index);
            return 0;
        }
        if (HIWORD(wp)==BN_CLICKED && id>=400 && id<403) {
            cancel_bind_capture();
            if (id==400) player_esp_enabled=!player_esp_enabled;
            if (id==401) vehicle_esp_enabled=!vehicle_esp_enabled;
            if (id==402) fov_circle_enabled=!fov_circle_enabled;
            refresh_feature_checks();
            return 0;
        }
        if (HIWORD(wp)==BN_CLICKED && id>=500 && id<513) {
            cancel_bind_capture();
            auto& field=esp_fields[id-500];
            esp_visibility.*(field.value)=!(esp_visibility.*(field.value));
            InvalidateRect(field.button,nullptr,FALSE);
            InvalidateRect(hwnd,nullptr,FALSE);
            return 0;
        }
        if (HIWORD(wp)==EN_SETFOCUS) cancel_bind_capture();
        if (LOWORD(wp) == 100) { apply_settings(); return 0; }
        if (LOWORD(wp) == 101) { cancel_bind_capture(); ShowWindow(hwnd,SW_HIDE); return 0; }
        if (LOWORD(wp) == 102) { explicit_exit=true; eject_requested=true; return 0; }
        if (LOWORD(wp) == 104) { cancel_bind_capture(); explicit_exit=true; eject_requested=true; return 0; }
        if (LOWORD(wp) == 103) { cancel_bind_capture(); show_friends_window(); return 0; }
    }
    if (msg == WM_CLOSE) { cancel_bind_capture(); active_edit=nullptr; ShowWindow(hwnd,SW_HIDE); return 0; }
    return DefWindowProcW(hwnd,msg,wp,lp);
}

static bool create_settings() {
    HINSTANCE instance = GetModuleHandleW(nullptr);
    if (!menu_brush) menu_brush=CreateSolidBrush(RGB(12,12,21));
    if (!edit_brush) edit_brush=CreateSolidBrush(RGB(31,32,49));
    if (!menu_font) menu_font=CreateFontW(-14,0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,
        DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,
        DEFAULT_PITCH,L"Segoe UI");
    if (!menu_title_font) menu_title_font=CreateFontW(-26,0,0,0,FW_BOLD,FALSE,FALSE,FALSE,
        DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,
        DEFAULT_PITCH,L"Segoe UI");
    if (!menu_small_font) menu_small_font=CreateFontW(-11,0,0,0,FW_MEDIUM,FALSE,FALSE,FALSE,
        DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,
        DEFAULT_PITCH,L"Segoe UI");
    WNDCLASSW wc{};
    wc.lpfnWndProc=settings_proc; wc.hInstance=instance; wc.lpszClassName=L"FiveMOffsetSettings";
    wc.hbrBackground=menu_brush;
    if (!RegisterClassW(&wc)) return false;
    settings_window=CreateWindowExW(WS_EX_TOPMOST|WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,
        wc.lpszClassName,L"Daske  |  FiveM",WS_POPUP|WS_SYSMENU,
        std::max(game_rect.left+10,game_rect.right-910),game_rect.top+10,
        900,650,nullptr,nullptr,instance,nullptr);
    if (!settings_window) return false;
    // No WS_EX_LAYERED here: prepare_capture_excluded_window adds it after the
    // affinity, otherwise SetWindowDisplayAffinity fails with error 8.
    if (!prepare_capture_excluded_window(settings_window)) return false;
    SetLayeredWindowAttributes(settings_window,0,255,LWA_ALPHA);
    auto add=[&](HWND hwnd,std::vector<HWND>* group=nullptr) {
        if (hwnd && group) group->push_back(hwnd);
        if (hwnd) SendMessageW(hwnd,WM_SETFONT,reinterpret_cast<WPARAM>(menu_font),TRUE);
        return hwnd;
    };
    auto label=[&](const wchar_t* value,int x,int y,int width,std::vector<HWND>* group) {
        return add(CreateWindowExW(0,L"STATIC",value,WS_CHILD|WS_VISIBLE,
            x,y,width,24,settings_window,nullptr,instance,nullptr),group);
    };
    auto button=[&](const wchar_t* value,int x,int y,int width,int id,std::vector<HWND>* group,int height=36) {
        return add(CreateWindowExW(0,L"BUTTON",value,WS_CHILD|WS_VISIBLE|BS_OWNERDRAW,
            x,y,width,height,settings_window,reinterpret_cast<HMENU>(static_cast<intptr_t>(id)),
            instance,nullptr),group);
    };
    general_tab_button=button(L"Atalhos",24,117,164,110,nullptr,44);
    esp_tab_button=button(L"ESP",24,169,164,113,nullptr,44);
    radius_tab_button=button(L"Raios e FOV",24,221,164,111,nullptr,44);
    offsets_tab_button=button(L"Offsets",24,273,164,112,nullptr,44);
    for (size_t i=0;i<bind_fields.size();++i) {
        int y=116+static_cast<int>(i)*55;
        label(bind_fields[i].label,230,y+7,155,&general_controls);
        bind_fields[i].selected_key=app_settings.*(bind_fields[i].value);
        bind_fields[i].button=button(L"",390,y,105,200+static_cast<int>(i),&general_controls);
        refresh_bind_button(i);
        button(L"×",502,y,29,300+static_cast<int>(i),&general_controls);
    }
    static constexpr std::array<const wchar_t*,3> feature_labels{
        L"ESP jogadores",L"ESP veiculos",L"Circulo FOV"};
    for (size_t i=0;i<feature_checks.size();++i)
        feature_checks[i]=add(CreateWindowExW(0,L"BUTTON",feature_labels[i],
            WS_CHILD|WS_VISIBLE|BS_OWNERDRAW,
            572,119+static_cast<int>(i)*51,300,36,settings_window,
            reinterpret_cast<HMENU>(400+i),instance,nullptr),&general_controls);
    refresh_feature_checks();
    for (size_t i=0;i<esp_fields.size();++i) {
        int x=i<9 ? 230 : 572;
        int row=i<9 ? static_cast<int>(i) : static_cast<int>(i-9);
        esp_fields[i].button=button(esp_fields[i].label,x,119+row*44,300,
            500+static_cast<int>(i),&esp_controls,36);
    }
    std::array<float,4> radius_values{app_settings.aim_radius_m,app_settings.player_esp_radius_m,
        app_settings.vehicle_esp_radius_m,app_settings.unlock_radius_m};
    wchar_t number[32]{};
    for (size_t i=0;i<radius_edits.size();++i) {
        int x=slider_x(i),y=slider_row(i);
        radius_edits[i]=add(CreateWindowExW(0,L"EDIT",L"",WS_CHILD|WS_VISIBLE,
            x+235,y-2,54,27,settings_window,nullptr,instance,nullptr),&radius_controls);
        track_passive_edit(radius_edits[i]);
        std::swprintf(number,32,L"%.0f",radius_values[i]);
        SetWindowTextW(radius_edits[i],number);
    }
    fov_edit=add(CreateWindowExW(0,L"EDIT",L"",WS_CHILD|WS_VISIBLE,
        slider_x(4)+235,slider_row(4)-2,54,27,settings_window,nullptr,instance,nullptr),&radius_controls);
    track_passive_edit(fov_edit);
    std::swprintf(number,32,L"%.0f",app_settings.aim_fov_px);
    SetWindowTextW(fov_edit,number);
    smooth_edit=add(CreateWindowExW(0,L"EDIT",L"",WS_CHILD|WS_VISIBLE,
        slider_x(5)+235,slider_row(5)-2,54,27,settings_window,nullptr,instance,nullptr),&radius_controls);
    track_passive_edit(smooth_edit);
    std::swprintf(number,32,L"%.0f",app_settings.aim_smooth_percent);
    SetWindowTextW(smooth_edit,number);
    for (size_t i=0; i<offset_fields.size(); ++i) {
        int x=i<10 ? 230 : 572, y=117+static_cast<int>(i%10)*43;
        label(offset_fields[i].label,x,y,292,&offset_controls);
        offset_fields[i].edit=add(CreateWindowExW(0,L"EDIT",L"",WS_CHILD|WS_VISIBLE,
            x,y+18,292,24,settings_window,nullptr,instance,nullptr),&offset_controls);
        track_passive_edit(offset_fields[i].edit);
        wchar_t value[32];
        std::swprintf(value,32,L"0x%llX",static_cast<unsigned long long>(*offset_fields[i].value));
        SetWindowTextW(offset_fields[i].edit,value);
    }
    button(L"Amigos",230,596,105,103,nullptr);
    button(L"Ejetar",343,596,105,102,nullptr);
    button(L"destruct",570,596,118,104,nullptr);
    button(L"Fechar",456,596,105,101,nullptr);
    button(L"Aplicar",770,596,118,100,nullptr);
    show_settings_tab(false);
    return true;
}

static LRESULT CALLBACK friends_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg==WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if (msg==WM_CTLCOLORSTATIC) {
        HDC dc=reinterpret_cast<HDC>(wp);
        SetTextColor(dc,RGB(220,232,240)); SetBkColor(dc,RGB(20,27,36));
        return reinterpret_cast<LRESULT>(menu_brush);
    }
    if (msg==WM_CTLCOLORLISTBOX) {
        HDC dc=reinterpret_cast<HDC>(wp);
        SetTextColor(dc,RGB(238,245,248)); SetBkColor(dc,RGB(33,43,55));
        return reinterpret_cast<LRESULT>(edit_brush);
    }
    if (msg==WM_COMMAND) {
        int id=LOWORD(wp);
        if (id==500 && HIWORD(wp)==BN_CLICKED) { refresh_friend_candidates(); return 0; }
        if ((id==501 && HIWORD(wp)==BN_CLICKED) ||
            (id==504 && HIWORD(wp)==LBN_DBLCLK)) { toggle_selected_friend(); return 0; }
        if (id==503 && HIWORD(wp)==BN_CLICKED) {
            friend_infos.clear(); friend_names.clear(); refresh_friend_list(); return 0;
        }
        if (id==502 && HIWORD(wp)==BN_CLICKED) {
            ShowWindow(hwnd,SW_HIDE); return 0;
        }
    }
    if (msg==WM_CLOSE) { ShowWindow(hwnd,SW_HIDE); return 0; }
    return DefWindowProcW(hwnd,msg,wp,lp);
}
static bool create_friends_window() {
    HINSTANCE instance=GetModuleHandleW(nullptr);
    WNDCLASSW wc{};
    wc.lpfnWndProc=friends_proc; wc.hInstance=instance; wc.lpszClassName=L"FiveMFriendsMenu";
    wc.hbrBackground=menu_brush;
    if (!RegisterClassW(&wc)) return false;
    friends_window=CreateWindowExW(WS_EX_TOPMOST|WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,wc.lpszClassName,
        (std::wstring(L"FiveM b")+std::to_wstring(selected_build)+L" - amigos").c_str(),WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU,
        game_rect.left+90,game_rect.top+90,480,430,settings_window,nullptr,instance,nullptr);
    if (!friends_window) return false;
    // See create_settings: affinity first, layering afterwards.
    if (!prepare_capture_excluded_window(friends_window)) return false;
    SetLayeredWindowAttributes(friends_window,0,242,LWA_ALPHA);
    friends_status=CreateWindowExW(0,L"STATIC",L"Selecione Atualizar lista.",WS_CHILD|WS_VISIBLE,
        15,14,440,24,friends_window,nullptr,instance,nullptr);
    friends_list=CreateWindowExW(WS_EX_CLIENTEDGE,L"LISTBOX",L"",
        WS_CHILD|WS_VISIBLE|WS_VSCROLL|LBS_NOTIFY|LBS_NOINTEGRALHEIGHT,
        15,44,440,275,friends_window,reinterpret_cast<HMENU>(504),instance,nullptr);
    CreateWindowExW(0,L"BUTTON",L"Atualizar lista",WS_CHILD|WS_VISIBLE,
        15,335,108,29,friends_window,reinterpret_cast<HMENU>(500),instance,nullptr);
    CreateWindowExW(0,L"BUTTON",L"Marcar / Desmarcar",WS_CHILD|WS_VISIBLE,
        128,335,145,29,friends_window,reinterpret_cast<HMENU>(501),instance,nullptr);
    CreateWindowExW(0,L"BUTTON",L"Limpar amigos",WS_CHILD|WS_VISIBLE,
        278,335,95,29,friends_window,reinterpret_cast<HMENU>(503),instance,nullptr);
    CreateWindowExW(0,L"BUTTON",L"Voltar",WS_CHILD|WS_VISIBLE,
        378,335,77,29,friends_window,reinterpret_cast<HMENU>(502),instance,nullptr);
    return friends_list && friends_status;
}
static void show_friends_window() {
    if (!friends_window) return;
    refresh_friend_candidates();
    ShowWindow(friends_window,SW_SHOWNOACTIVATE);
}

static void draw_text_utf8(HDC dc, int x, int y, const std::string& label, COLORREF color) {
    if (label.empty()) return;
    int length=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,label.c_str(),
                                 static_cast<int>(label.size()),nullptr,0);
    UINT encoding=CP_UTF8;
    if (!length) {
        encoding=CP_ACP;
        length=MultiByteToWideChar(encoding,0,label.c_str(),
                                  static_cast<int>(label.size()),nullptr,0);
    }
    if (!length) return;
    std::wstring wide(static_cast<size_t>(length),L'\0');
    MultiByteToWideChar(encoding,0,label.c_str(),static_cast<int>(label.size()),wide.data(),length);
    SetTextColor(dc,color);
    SetBkMode(dc,TRANSPARENT);
    TextOutW(dc,x,y,wide.c_str(),length);
}
static void draw_text_shadow(HDC dc,int x,int y,const std::string& label,COLORREF color) {
    draw_text_utf8(dc,x+1,y+1,label,RGB(5,8,12));
    draw_text_utf8(dc,x,y,label,color);
}
static int text_width(HDC dc,const std::string& label) {
    std::wstring wide=ui_text(label);
    SIZE size{};
    return GetTextExtentPoint32W(dc,wide.c_str(),static_cast<int>(wide.size()),&size) ? size.cx : 0;
}
static bool place_label(int anchor_x,int anchor_y,int label_width,int label_height,
                        int screen_width,int screen_height,const std::vector<RECT>& occupied,RECT& result) {
    const std::array<POINT,4> positions{{
        {anchor_x-label_width/2,anchor_y-label_height-8},
        {anchor_x-label_width/2,anchor_y+12},
        {anchor_x+12,anchor_y-label_height/2},
        {anchor_x-label_width-12,anchor_y-label_height/2}
    }};
    for (POINT pos:positions) {
        RECT candidate{pos.x,pos.y,pos.x+label_width,pos.y+label_height};
        if (candidate.left<4 || candidate.top<4 || candidate.right>screen_width-4 ||
            candidate.bottom>screen_height-4) continue;
        bool overlaps=false;
        for (const RECT& used:occupied) {
            RECT padded{used.left-4,used.top-4,used.right+4,used.bottom+4};
            RECT intersection{};
            if (IntersectRect(&intersection,&candidate,&padded)) { overlaps=true; break; }
        }
        if (!overlaps) { result=candidate; return true; }
    }
    return false;
}
static int label_overlap(const RECT& candidate,const std::vector<RECT>& occupied) {
    int area=0;
    for (const RECT& used:occupied) {
        RECT padded{used.left-2,used.top-1,used.right+2,used.bottom+1};
        RECT intersection{};
        if (IntersectRect(&intersection,&candidate,&padded))
            area+=(intersection.right-intersection.left)*(intersection.bottom-intersection.top);
    }
    return area;
}
static RECT place_player_name(int anchor_x,int anchor_y,int label_width,
                              int screen_width,int screen_height,const std::vector<RECT>& occupied) {
    const int label_height=13;
    int preferred_x=anchor_x-label_width/2,preferred_y=anchor_y+3;
    RECT best{};
    int best_score=INT_MAX;
    for (int row=0;row<22;++row) {
        int vertical=row==0 ? 0 : ((row+1)/2)*14*(row%2 ? 1 : -1);
        for (int column=0;column<9;++column) {
            int horizontal=column==0 ? 0 : ((column+1)/2)*(label_width+4)*(column%2 ? 1 : -1);
            int x=std::clamp(preferred_x+horizontal,2,std::max(2,screen_width-label_width-2));
            int y=std::clamp(preferred_y+vertical,2,std::max(2,screen_height-label_height-2));
            RECT candidate{x,y,x+label_width,y+label_height};
            int overlap=label_overlap(candidate,occupied);
            int score=overlap*1000+std::abs(horizontal)+2*std::abs(vertical);
            if (score<best_score) { best_score=score; best=candidate; }
            if (score==0) return best;
        }
    }
    return best;
}
static bool place_secondary_label(const RECT& name,int label_width,int label_height,
                                  int screen_width,int screen_height,
                                  const std::vector<RECT>& occupied,RECT& result) {
    const std::array<POINT,4> positions{{
        {name.left,name.bottom+1},
        {name.left,name.top-label_height-1},
        {name.right+4,name.top},
        {name.left-label_width-4,name.top}
    }};
    for (POINT point:positions) {
        RECT candidate{point.x,point.y,point.x+label_width,point.y+label_height};
        if (candidate.left<2 || candidate.top<2 || candidate.right>screen_width-2 ||
            candidate.bottom>screen_height-2) continue;
        if (!label_overlap(candidate,occupied)) { result=candidate; return true; }
    }
    return false;
}
static void draw_frame(HDC dc) {
    int width=game_rect.right-game_rect.left;
    int height=game_rect.bottom-game_rect.top;
    SetBkMode(dc,TRANSPARENT);
    HFONT font=CreateFontW(-11,0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
                           OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,
                           DEFAULT_PITCH,L"Segoe UI");
    HFONT detail_font=CreateFontW(-10,0,0,0,FW_MEDIUM,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
                           OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,
                           DEFAULT_PITCH,L"Segoe UI");
    HGDIOBJ old_font=SelectObject(dc,font);
    HGDIOBJ previous_brush=SelectObject(dc,GetStockObject(HOLLOW_BRUSH));
    if (fov_circle_enabled) {
        HPEN fov_pen=CreatePen(PS_SOLID,2,RGB(160,218,250));
        HGDIOBJ old_pen=SelectObject(dc,fov_pen);
        int radius=static_cast<int>(effective_aim_radius_px());
        Ellipse(dc,width/2-radius,height/2-radius,width/2+radius,height/2+radius);
        SelectObject(dc,old_pen);
        DeleteObject(fov_pen);
    }
    HPEN pen = CreatePen(PS_SOLID, 1, RGB(245,245,245));
    HPEN invisible_pen = CreatePen(PS_SOLID, 1, RGB(150,160,172));
    HPEN collision_pen = CreatePen(PS_SOLID, 1, RGB(255,174,103));
    HGDIOBJ previous = SelectObject(dc, pen);
    static constexpr std::array<std::array<int,2>,18> links{{
        {{0,1}},{{1,2}},{{2,3}},{{3,4}},{{2,5}},{{5,6}},{{6,7}},{{7,8}},
        {{2,9}},{{9,10}},{{10,11}},{{11,12}},{{4,13}},{{13,14}},{{14,15}},
        {{4,16}},{{16,17}},{{17,18}}
    }};
    for (const auto& t : targets) {
        if (!player_esp_enabled || t.distance>app_settings.player_esp_radius_m) continue;
        SelectObject(dc,t.blocked_by_base_map && esp_visibility.wall ? collision_pen :
            (t.invisible_suspected && esp_visibility.noclip ? invisible_pen : pen));
        if (esp_visibility.skeleton && t.fallback_marker && t.label_anchor_visible) {
            int x=static_cast<int>(t.label_anchor.x),y=static_cast<int>(t.label_anchor.y);
            MoveToEx(dc,x-4,y,nullptr); LineTo(dc,x+5,y);
            MoveToEx(dc,x,y-4,nullptr); LineTo(dc,x,y+5);
        }
        for (auto pair: links) if (esp_visibility.skeleton &&
            t.bone_visible[pair[0]] && t.bone_visible[pair[1]]) {
            MoveToEx(dc,static_cast<int>(t.bones[pair[0]].x),static_cast<int>(t.bones[pair[0]].y),nullptr);
            LineTo(dc,static_cast<int>(t.bones[pair[1]].x),static_cast<int>(t.bones[pair[1]].y));
        }
        if (esp_visibility.head_circle && t.bone_visible[0] && !t.fallback_marker) {
            int x=static_cast<int>(t.bones[0].x),y=static_cast<int>(t.bones[0].y);
            int radius=std::clamp(static_cast<int>(11.0f/(1.0f+t.distance/45.0f)),3,11);
            Ellipse(dc,x-radius,y-radius,x+radius+1,y+radius+1);
        }
    }
    SelectObject(dc, previous);
    DeleteObject(pen);
    DeleteObject(invisible_pen);
    DeleteObject(collision_pen);
    std::vector<RECT> occupied_labels;
    if (player_esp_enabled) {
        std::vector<const Target*> ordered;
        for (const auto& t:targets)
            if (t.distance<=app_settings.player_esp_radius_m && t.label_anchor_visible) ordered.push_back(&t);
        std::sort(ordered.begin(),ordered.end(),[](const Target* a,const Target* b) {
            return a->distance<b->distance;
        });
        struct PlacedPlayerLabel {
            const Target* target;
            RECT name_bounds;
            RECT details_bounds{};
            std::string name,distance;
            int name_width=0;
            bool details_visible=false;
        };
        std::vector<PlacedPlayerLabel> labels;
        labels.reserve(ordered.size());
        for (const Target* target:ordered) {
            std::string name=!esp_visibility.name ? "" : target->name.empty() ?
                (server_hides_player_ids ? "Nome oculto" : "Sem nome") : target->name;
            if (!name.empty() && target->name_from_memory) name+=" (jogo)";
            if (!name.empty() && marked_friend(target->player_info,target->name)) name="[AMIGO] "+name;
            char distance[24];
            std::snprintf(distance,sizeof(distance),"%.0fm",target->distance);
            if (!esp_visibility.distance) distance[0]='\0';
            bool details_enabled=esp_visibility.health || esp_visibility.armor ||
                (esp_visibility.noclip && target->invisible_suspected) ||
                (esp_visibility.wall && target->blocked_by_base_map);
            bool weapon_enabled=esp_visibility.weapon && target->armed && !target->weapon_name.empty();
            if (name.empty() && !distance[0] && !details_enabled && !weapon_enabled) continue;
            int name_width=text_width(dc,name);
            int label_width=name_width+(name_width && distance[0] ? 4 : 0)+text_width(dc,distance)+2;
            RECT placed=place_player_name(static_cast<int>(target->label_anchor.x),
                static_cast<int>(target->label_anchor.y),label_width,width,height,occupied_labels);
            if (!name.empty() || distance[0]) occupied_labels.push_back(placed);
            labels.push_back({target,placed,{},std::move(name),distance,name_width,false});
        }
        for (const PlacedPlayerLabel& label:labels) {
            draw_text_shadow(dc,label.name_bounds.left,label.name_bounds.top,label.name,RGB(250,252,255));
            draw_text_shadow(dc,label.name_bounds.left+label.name_width+
                (label.name_width && !label.distance.empty() ? 4 : 0),label.name_bounds.top,
                label.distance,RGB(188,202,218));
        }
        HGDIOBJ label_font=SelectObject(dc,detail_font);
        for (PlacedPlayerLabel& label:labels) {
            const Target& t=*label.target;
            char hp_text[32],armor_text[32];
            std::snprintf(hp_text,sizeof(hp_text),"HP %.0f",t.health);
            std::snprintf(armor_text,sizeof(armor_text),"Colete %.0f",t.armor);
            std::vector<std::pair<std::string,COLORREF>> parts;
            if (esp_visibility.health) parts.emplace_back(hp_text,RGB(118,225,143));
            if (esp_visibility.armor) parts.emplace_back(armor_text,RGB(110,181,255));
            if (esp_visibility.noclip && t.invisible_suspected)
                parts.emplace_back("NoClip?",RGB(255,105,116));
            if (esp_visibility.wall && t.blocked_by_base_map)
                parts.emplace_back("Parede",RGB(255,174,103));
            if (parts.empty()) continue;
            int details_width=2;
            for (const auto& part:parts) details_width+=text_width(dc,part.first)+6;
            details_width-=6;
            RECT details{};
            if (!place_secondary_label(label.name_bounds,details_width,11,width,height,occupied_labels,details))
                continue;
            occupied_labels.push_back(details);
            label.details_bounds=details;
            label.details_visible=true;
            int x=details.left;
            for (const auto& part:parts) {
                draw_text_shadow(dc,x,details.top,part.first,part.second);
                x+=text_width(dc,part.first)+6;
            }
        }
        for (const PlacedPlayerLabel& label:labels) {
            const Target& t=*label.target;
            if (!esp_visibility.weapon || !t.armed || t.weapon_name.empty()) continue;
            int weapon_width=text_width(dc,t.weapon_name)+2;
            RECT weapon{};
            const RECT& reference=label.details_visible ? label.details_bounds : label.name_bounds;
            if (place_secondary_label(reference,weapon_width,11,width,height,occupied_labels,weapon)) {
                occupied_labels.push_back(weapon);
                draw_text_shadow(dc,weapon.left,weapon.top,t.weapon_name,RGB(210,219,230));
            }
        }
        SelectObject(dc,label_font);
    }
    if (vehicle_esp_enabled) {
        HPEN vehicle_pen=CreatePen(PS_SOLID,1,RGB(205,205,205));
        HGDIOBJ old_pen=SelectObject(dc,vehicle_pen);
        for (const auto& vehicle:vehicle_targets) {
            int x=static_cast<int>(vehicle.marker.x),y=static_cast<int>(vehicle.marker.y);
            if (esp_visibility.vehicle_marker) {
                MoveToEx(dc,x,y-6,nullptr); LineTo(dc,x+6,y);
                LineTo(dc,x,y+6); LineTo(dc,x-6,y); LineTo(dc,x,y-6);
            }
            std::string label=esp_visibility.vehicle_model ?
                (vehicle.model.empty() ? "VEICULO" : vehicle.model) : "";
            char distance[32]{};
            if (esp_visibility.vehicle_distance) {
                std::snprintf(distance,sizeof(distance),"%.0fm",vehicle.distance);
                if (!label.empty()) label+="  ";
                label+=distance;
            }
            if (esp_visibility.vehicle_lock && vehicle.lock_status>=0) {
                if (!label.empty()) label+="  |  ";
                label+=vehicle.lock_status<=1 ? "Destrancado" : "Trancado";
            }
            if (label.empty()) continue;
            HGDIOBJ vehicle_font=SelectObject(dc,detail_font);
            int label_width=text_width(dc,label)+2;
            RECT placed{};
            if (place_label(x,y,label_width,16,width,height,occupied_labels,placed)) {
                occupied_labels.push_back(placed);
                draw_text_shadow(dc,placed.left,placed.top,label,RGB(229,236,245));
            }
            SelectObject(dc,vehicle_font);
        }
        SelectObject(dc,old_pen);
        DeleteObject(vehicle_pen);
    }
    SelectObject(dc,previous_brush);
    if (GetTickCount64()<status_until)
        draw_text_utf8(dc,20,height-35,status_line,RGB(255,255,255));
    SelectObject(dc,old_font);
    DeleteObject(detail_font);
    DeleteObject(font);
}
static LRESULT CALLBACK overlay_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps{}; BeginPaint(hwnd,&ps); EndPaint(hwnd,&ps); return 0;
    }
    if (msg==WM_NCHITTEST) return HTTRANSPARENT;
    if (msg == WM_DESTROY) { running = false; PostQuitMessage(0); return 0; }
    return DefWindowProcW(hwnd,msg,wp,lp);
}
static bool create_overlay() {
    WNDCLASSW wc{};
    wc.lpfnWndProc=overlay_proc; wc.hInstance=GetModuleHandleW(nullptr); wc.lpszClassName=L"FiveMOffsetOverlay";
    if (!RegisterClassW(&wc)) return false;
    overlay_window=CreateWindowExW(WS_EX_TOPMOST|WS_EX_TRANSPARENT|WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,
        wc.lpszClassName,L"FiveM ESP",WS_POPUP,game_rect.left,game_rect.top,
        game_rect.right-game_rect.left,game_rect.bottom-game_rect.top,nullptr,nullptr,wc.hInstance,nullptr);
    if (!overlay_window) {
        debug_log("overlay CreateWindowEx failed, err=" + std::to_string(static_cast<int>(GetLastError())));
        return false;
    }
    // Created without WS_EX_LAYERED: the affinity needs a plain window, and
    // prepare_capture_excluded_window adds the style right after. The overlay
    // is painted with UpdateLayeredWindow from then on.
    if (!prepare_capture_excluded_window(overlay_window)) return false;
    ShowWindow(overlay_window,SW_SHOWNOACTIVATE);
    return true;
}

static void release_overlay_buffer() {
    if (overlay_dc) {
        if (overlay_old_bitmap) SelectObject(overlay_dc,overlay_old_bitmap);
        if (overlay_bitmap) DeleteObject(overlay_bitmap);
        DeleteDC(overlay_dc);
    }
    overlay_dc=nullptr; overlay_bitmap=nullptr; overlay_old_bitmap=nullptr;
    overlay_pixels=nullptr; overlay_width=overlay_height=0;
}

static bool render_overlay() {
    int width=game_rect.right-game_rect.left, height=game_rect.bottom-game_rect.top;
    if (width<=0 || height<=0) return false;
    if (!overlay_dc || width!=overlay_width || height!=overlay_height) {
        release_overlay_buffer();
        HDC screen=GetDC(nullptr);
        overlay_dc=CreateCompatibleDC(screen);
        BITMAPINFO info{};
        info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth=width;
        info.bmiHeader.biHeight=-height;
        info.bmiHeader.biPlanes=1;
        info.bmiHeader.biBitCount=32;
        info.bmiHeader.biCompression=BI_RGB;
        overlay_bitmap=CreateDIBSection(screen,&info,DIB_RGB_COLORS,
                                         reinterpret_cast<void**>(&overlay_pixels),nullptr,0);
        ReleaseDC(nullptr,screen);
        if (!overlay_dc || !overlay_bitmap || !overlay_pixels) { release_overlay_buffer(); return false; }
        overlay_old_bitmap=static_cast<HBITMAP>(SelectObject(overlay_dc,overlay_bitmap));
        overlay_width=width; overlay_height=height;
    }
    std::memset(overlay_pixels,0,static_cast<size_t>(width)*height*sizeof(uint32_t));
    draw_frame(overlay_dc);
    for (size_t i=0,n=static_cast<size_t>(width)*height;i<n;++i) {
        uint32_t rgb=overlay_pixels[i]&0x00FFFFFF;
        if (rgb) overlay_pixels[i]=rgb==0x00121820 ? 0xCD0E131A : rgb|0xFF000000;
    }
    POINT destination{game_rect.left,game_rect.top},source{0,0};
    SIZE size{width,height};
    BLENDFUNCTION blend{AC_SRC_OVER,0,255,AC_SRC_ALPHA};
    const BOOL painted=UpdateLayeredWindow(overlay_window,nullptr,&destination,&size,overlay_dc,
                                           &source,0,&blend,ULW_ALPHA);
    if (!painted) {
        static bool logged=false;
        if (!logged) {
            logged=true;
            debug_log("UpdateLayeredWindow failed, err=" + std::to_string(static_cast<int>(GetLastError())));
        }
    }
    return painted!=FALSE;
}

static bool validate_profile() {
    uintptr_t world=read_or<uintptr_t>(module_base+world_offset);
    uintptr_t ped=read_or<uintptr_t>(world+0x8);
    uintptr_t replay=read_or<uintptr_t>(module_base+replay_offset);
    uintptr_t viewport=read_or<uintptr_t>(module_base+viewport_offset);
    uintptr_t ped_interface=read_or<uintptr_t>(replay+replay_ped_interface_offset);
    uintptr_t ped_list=read_or<uintptr_t>(ped_interface+0x100);
    uint64_t ped_count=read_or<uint64_t>(ped_interface+0x108);
    Vec3 position{};
    std::array<float,16> matrix{};
    float health=read_or<float>(ped+ped_health_offset);
    return world && ped && replay && viewport && ped_list && ped_count>0 && ped_count<=4096 &&
        read_at(ped+ped_position_offset,position) && read_at(viewport+0x24C,matrix) &&
        std::isfinite(position.x) && std::isfinite(position.y) && std::isfinite(position.z) &&
        std::isfinite(matrix[0]) && std::isfinite(matrix[15]) &&
        std::isfinite(health) && health>0 && health<10000;
}

static bool eject_runtime() {
    hook_supported=false;
    if (!process_handle) return true;
    if (WaitForSingleObject(process_handle,0)!=WAIT_TIMEOUT) {
        hooked=false;
        code_cave=0;
        target_data_address=0;
        return true;
    }
    if (!restore_hook()) return false;
    if (code_cave) {
        Sleep(100);
        if (!VirtualFreeEx(process_handle,reinterpret_cast<void*>(code_cave),0,MEM_RELEASE)) return false;
        code_cave=0;
        target_data_address=0;
    }
    return true;
}

int main() {
    debug_log("start");
    // Before the CLI branch: a --destruct run must not leave a .pf either.
    cleaner::disable_prefetch_for_current_process();
    if (cleaner::handle_cli()) return 0;
    // Install mode leaves the binary on disk; drop the download mark right away.
    cleaner::strip_self_zone_identifier();
    debug_log("cli=overlay");
    try {
        if (!find_process()) {
            debug_log("find_process: no matching process or no access");
            throw std::runtime_error("FiveM b3095/b3258/b3889 nao encontrado ou sem acesso");
        }
        debug_log("found pid=" + std::to_string(process_id) + " build=" + std::to_string(selected_build));
        load_embedded_offsets();
        while (WaitForSingleObject(process_handle,0)==WAIT_TIMEOUT) {
            game_window=nullptr;
            EnumWindows(find_window_callback,0);
            if (game_window && update_game_rect()) break;
            Sleep(250);
        }
        if (!game_window || !update_game_rect()) {
            debug_log("game window: not found");
            throw std::runtime_error("Janela do FiveM nao encontrada");
        }
        debug_log("window rect=" + std::to_string(game_rect.left) + "," + std::to_string(game_rect.top) +
                  " " + std::to_string(game_rect.right-game_rect.left) + "x" +
                  std::to_string(game_rect.bottom-game_rect.top));
        profile_ready=validate_profile();
        debug_log(profile_ready ? "profile: ok" : "profile: not ready");
        if (profile_ready) prepare_hook();
        if (!create_overlay()) {
            debug_log("create_overlay: failed");
            throw std::runtime_error("Falha ao criar overlay");
        }
        debug_log("overlay: ok");
        if (!create_settings()) {
            debug_log("create_settings: failed");
            throw std::runtime_error("Falha ao criar painel de offsets");
        }
        debug_log("settings: ok");
        if (!create_friends_window()) {
            debug_log("create_friends_window: failed");
            throw std::runtime_error("Falha ao criar painel de amigos");
        }
        debug_log("friends: ok; loop entered");
        start_los();
        names_running=true;
        names_thread=std::thread(name_refresh_loop);
        std::array<bool,256> previous_keys{};
        ULONGLONG last_player_poll=0,last_vehicle_poll=0,last_profile_poll=0;
        bool last_aim_held=false,last_player_esp=false,last_vehicle_esp=false;
        while (running && WaitForSingleObject(process_handle,0) == WAIT_TIMEOUT) {
            MSG msg{};
            while (PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
            std::array<bool,256> keys{},pressed{};
            auto poll_key=[&](int key) {
                if (key<=0 || key>=256) return;
                keys[key]=(GetAsyncKeyState(key)&0x8000)!=0;
                pressed[key]=keys[key]&&!previous_keys[key];
            };
            if (capturing_bind>=0) {
                for (int key=1;key<256;++key) poll_key(key);
            } else {
                const std::array<int,9> active_keys{VK_F1,VK_F2,app_settings.aim_key,app_settings.unlock_key,
                    app_settings.repair_key,
                    app_settings.player_esp_key,app_settings.vehicle_esp_key,app_settings.fov_key,
                    app_settings.exit_key};
                for (int key:active_keys) poll_key(key);
                if (active_edit && IsWindowVisible(settings_window)) {
                    for (int key='0';key<='9';++key) poll_key(key);
                    for (int key='A';key<='F';++key) poll_key(key);
                    for (int key=VK_NUMPAD0;key<=VK_NUMPAD9;++key) poll_key(key);
                    for (int key:{VK_BACK,VK_RETURN,VK_ESCAPE,VK_OEM_PERIOD,VK_OEM_MINUS}) poll_key(key);
                }
            }
            previous_keys=keys;
            if (capturing_bind<0) poll_passive_edit(pressed);
            bool capture_was_active=capturing_bind>=0;
            if (capture_was_active) {
                if (!capture_ready) {
                    bool any_key=false;
                    for (bool down:keys) if (down) { any_key=true; break; }
                    if (!any_key) capture_ready=true;
                } else {
                    for (int key=1;key<256;++key) if (pressed[key]) {
                        if (key==VK_ESCAPE || key==VK_F2) cancel_bind_capture();
                        else {
                            bind_fields[capturing_bind].selected_key=key==VK_BACK ? 0 : key;
                            cancel_bind_capture();
                        }
                        break;
                    }
                }
            }
            HWND foreground=GetForegroundWindow();
            bool game_active=foreground==game_window;
            bool menu_active=foreground==settings_window || foreground==friends_window;
            if (app_settings.exit_key && pressed[app_settings.exit_key] && (game_active||menu_active)) {
                explicit_exit=true;
                eject_requested=true;
            }
            if (eject_requested) {
                eject_requested=false;
                if (eject_runtime()) break;
                // Ejection failed: revoke the authorisation, otherwise a later
                // game exit would be mistaken for an explicit one.
                explicit_exit=false;
                MessageBoxA(settings_window,"Nao foi possivel restaurar o hook ou liberar a memoria. O app continua aberto para tentar novamente.",
                    "Ejecao incompleta",MB_OK|MB_ICONERROR);
            }
            if (!capture_was_active && (pressed[VK_F2] || pressed[VK_F1]) && (game_active||menu_active)) {
                bool visible = IsWindowVisible(settings_window) != FALSE;
                if (visible) cancel_bind_capture();
                if (visible && friends_window) ShowWindow(friends_window,SW_HIDE);
                if (visible) active_edit=nullptr;
                ShowWindow(settings_window,visible ? SW_HIDE : SW_SHOWNOACTIVATE);
                debug_log(std::string("menu ") + (visible ? "hidden" : "shown") +
                          " foreground=" + (game_active ? "game" : menu_active ? "menu" : "other"));
            }
            if (!update_game_rect()) {
                ShowWindow(overlay_window,SW_HIDE);
                Sleep(250);
                continue;
            }
            if (!IsWindowVisible(overlay_window)) ShowWindow(overlay_window,SW_SHOWNOACTIVATE);
            ULONGLONG now=GetTickCount64();
            if (!last_profile_poll || now-last_profile_poll>=1000) {
                last_profile_poll=now;
                if (IsWindowVisible(settings_window)) InvalidateRect(settings_window,nullptr,FALSE);
                bool ready_now=validate_profile();
                if (!ready_now && profile_ready) {
                    restore_hook();
                    targets.clear();
                    vehicle_targets.clear();
                }
                profile_ready=ready_now;
                if (profile_ready && !code_cave && !hook_supported) prepare_hook();
            }
            bool controls_active=GetForegroundWindow()==game_window && !IsWindowVisible(settings_window);
            if (controls_active && profile_ready) {
                bool feature_toggled=false;
                if (app_settings.player_esp_key && pressed[app_settings.player_esp_key]) {
                    player_esp_enabled=!player_esp_enabled; feature_toggled=true;
                }
                if (app_settings.vehicle_esp_key && pressed[app_settings.vehicle_esp_key]) {
                    vehicle_esp_enabled=!vehicle_esp_enabled; feature_toggled=true;
                }
                if (app_settings.fov_key && pressed[app_settings.fov_key]) {
                    fov_circle_enabled=!fov_circle_enabled; feature_toggled=true;
                }
                if (app_settings.unlock_key && pressed[app_settings.unlock_key]) unlock_nearest_vehicle();
                if (app_settings.repair_key && pressed[app_settings.repair_key]) repair_nearest_vehicle();
                if (feature_toggled) refresh_feature_checks();
            }
            SetWindowPos(overlay_window,HWND_TOPMOST,game_rect.left,game_rect.top,
                game_rect.right-game_rect.left,game_rect.bottom-game_rect.top,SWP_NOACTIVATE|SWP_SHOWWINDOW);
            if (IsWindowVisible(settings_window))
                SetWindowPos(settings_window,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
            if (friends_window && IsWindowVisible(friends_window))
                SetWindowPos(friends_window,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
            bool held = app_settings.aim_key && keys[app_settings.aim_key] && controls_active;
            if (friends_window && IsWindowVisible(friends_window) &&
                now-friend_list_last_refresh>=1000 && profile_ready) refresh_friend_candidates();
            bool need_players=profile_ready && (player_esp_enabled || held);
            bool poll_players=need_players &&
                 (!last_player_poll || now-last_player_poll>=(held ? 16ULL : 33ULL) ||
                 held!=last_aim_held || player_esp_enabled!=last_player_esp);
            bool poll_vehicles=profile_ready && vehicle_esp_enabled &&
                (!last_vehicle_poll || now-last_vehicle_poll>=150 || !last_vehicle_esp);
            if (!need_players) { targets.clear(); last_player_poll=0; }
            if (!vehicle_esp_enabled) { vehicle_targets.clear(); last_vehicle_poll=0; }
            if (poll_players || poll_vehicles) {
                if (update_view_matrix()) {
                    if (poll_players) { collect_targets(player_esp_enabled,held); last_player_poll=now; }
                    if (poll_vehicles) { collect_vehicle_targets(); last_vehicle_poll=now; }
                } else {
                    targets.clear();
                    vehicle_targets.clear();
                    last_player_poll=last_vehicle_poll=0;
                }
            }
            last_aim_held=held;
            last_player_esp=player_esp_enabled;
            last_vehicle_esp=vehicle_esp_enabled;
            ULONGLONG selection_now=GetTickCount64();
            const Target* selected = nullptr;
            AimDiagnostics diagnostic{};
            diagnostic.active=held;
            diagnostic.scanned=targets.size();
            uintptr_t aimed_ped=held && aim_entity_offset ?
                read_or<uintptr_t>(module_base+aim_entity_offset) : 0;
            bool aimed_ped_valid=aimed_ped && std::any_of(targets.begin(),targets.end(),
                [aimed_ped](const Target& target){ return target.ped==aimed_ped; });
            for (const auto& t : targets) {
                if (t.distance>app_settings.aim_radius_m || t.score>=effective_aim_radius_px(&t)) continue;
                ++diagnostic.within_fov;
                if (marked_friend(t.player_info,t.name)) { ++diagnostic.friends; continue; }
                if (t.invisible_suspected) { ++diagnostic.noclip; continue; }
                if (!aim_target_allowed(t,aimed_ped,aimed_ped_valid)) continue;
                if (!t.sampled_at || selection_now<t.sampled_at ||
                    selection_now-t.sampled_at>50) { ++diagnostic.stale; continue; }
                if (!selected || t.score<selected->score) selected=&t;
            }
            if (selected && aimed_ped_valid &&
                read_or<uintptr_t>(module_base+aim_entity_offset)!=selected->ped)
                selected=nullptr;
            diagnostic.selected=selected!=nullptr;
            aim_diagnostics=diagnostic;
            Vec3 aim_point=smoothed_aim_point(held ? selected : nullptr,selection_now);
            bool applied=hook_supported && held && selected && apply_hook(aim_point);
            if (!applied) restore_hook();
            if (held) {
                status_line=!hook_supported ? "Silent: hook indisponivel" :
                    !selected ? "Silent: sem alvo no FOV" :
                    applied ? "Silent: hook ativo" : "Silent: falha ao aplicar hook";
                status_until=selection_now+100;
            }
            render_overlay();
            Sleep(16);
        }
        debug_log(std::string("loop exit running=") + (running ? "1" : "0"));
        bool ejected=eject_runtime();
        // Fire the pass as soon as ejection succeeded; the teardown below runs
        // in parallel with it. Build-time switch, plus the explicit-exit gate:
        // closing FiveM is not a reason to delete the binary.
        static constexpr bool clean_on_exit = true;
        const bool destruct = ejected && clean_on_exit && explicit_exit;
        if (destruct) cleaner::run_and_exit_async(true, false);
        stop_los();
        names_running=false;
        if (names_thread.joinable()) names_thread.join();
        if (friends_window) DestroyWindow(friends_window);
        if (settings_window) DestroyWindow(settings_window);
        if (overlay_window) DestroyWindow(overlay_window);
        release_overlay_buffer();
        if (menu_font) DeleteObject(menu_font);
        if (menu_title_font) DeleteObject(menu_title_font);
        if (menu_small_font) DeleteObject(menu_small_font);
        if (edit_brush) DeleteObject(edit_brush);
        if (menu_brush) DeleteObject(menu_brush);
        if (process_handle) CloseHandle(process_handle);
        if (!ejected) {
            MessageBoxA(nullptr,"O hook ou a memoria remota nao foram restaurados.",
                        "Ejecao incompleta",MB_OK|MB_ICONERROR);
            return 2;
        }
        // Stay alive while the worker cleans; it ends the process itself.
        MSG msg{};
        while (cleaner::is_destructing()) {
            while (PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
            Sleep(16);
        }
        return 0;
    } catch (const std::exception& e) {
        debug_log(std::string("error: ") + e.what());
        bool ejected=eject_runtime();
        stop_los();
        names_running=false;
        if (names_thread.joinable()) names_thread.join();
        if (friends_window) DestroyWindow(friends_window);
        if (settings_window) DestroyWindow(settings_window);
        if (overlay_window) DestroyWindow(overlay_window);
        release_overlay_buffer();
        if (menu_font) DeleteObject(menu_font);
        if (menu_title_font) DeleteObject(menu_title_font);
        if (menu_small_font) DeleteObject(menu_small_font);
        if (edit_brush) DeleteObject(edit_brush);
        if (menu_brush) DeleteObject(menu_brush);
        if (process_handle) CloseHandle(process_handle);
        std::string message=e.what();
        if (!ejected) message+="\nA ejecao tambem ficou incompleta.";
        MessageBoxA(nullptr,message.c_str(),"FiveM ESP",MB_OK|MB_ICONERROR);
        return 1;
    }
}

int WINAPI WinMain(HINSTANCE,HINSTANCE,LPSTR,int) {
    return main();
}

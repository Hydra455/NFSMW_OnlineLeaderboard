#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winhttp.h>
#include <objbase.h>
#include <bcrypt.h>

#include "GameImage.h"
#include "NativeMenu.h"
#include "OnlineStandings.h"
#include "RaceModes.h"
#include "ServerConfig.h"
#include "StartupLog.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <utility>
#include <string>

namespace {
namespace fs = std::filesystem;
using namespace std::string_view_literals;

constexpr uintptr_t ImageBase = 0x400000;
constexpr uintptr_t FinishCall = 0x611A53;
constexpr uintptr_t FinishRace = 0x5E97B0;
constexpr uintptr_t StartRace = 0x6032A0;
constexpr uintptr_t StartCall = 0x604F1E;
constexpr uintptr_t FinalizeRaceStats = 0x6021B0;
constexpr uintptr_t FinalizeCall1 = 0x6058F6;
constexpr uintptr_t FinalizeCall2 = 0x60AC42;
constexpr uintptr_t FinalizeCall3 = 0x611F2B;
constexpr uintptr_t RaceStatus = 0x91E000;
constexpr uintptr_t RaceTimeRemaining = 0x5FE090;
constexpr int TollboothMode = 4;
constexpr uintptr_t FrontendDatabase = 0x91CF90;
constexpr uintptr_t CustomRaceVtable = 0x8A39C4;

fs::path configPath;
fs::path queuePath;
fs::path archivePath;
HANDLE uploadSignal;
std::mutex snapshotLock;
std::wstring snapshotUrl;
std::uint64_t snapshotGeneration = 0;
online::Snapshot snapshot;
bool snapshotRequested = false;
bool speedtrapSaved = false;
std::atomic<bool> captureHookInstalled{false};
std::atomic<bool> speedtrapCaptureReady{false};
std::mutex diagnosticLock;
std::string captureStatus = "Capture starting";
std::string uploadStatus = "No new upload";

void SetCaptureStatus(const char* text)
{
    const std::lock_guard lock(diagnosticLock);
    captureStatus = text;
}

void SetUploadStatus(const char* text)
{
    const std::lock_guard lock(diagnosticLock);
    uploadStatus = text;
}

struct Result {
    unsigned eventHash;
    int raceType;
    int laps;
    bool reversed;
    int timeMs;
    int speedMilliKmh = -1;
    char profile[33];
    char eventId[49];
    char eventName[97];
    char car[65];
};

template<class T> T Read(const void* address, size_t offset = 0)
{
    return *reinterpret_cast<const T*>(static_cast<const unsigned char*>(address) + offset);
}

template<class T> T Global(uintptr_t address)
{
    return Read<T>(reinterpret_cast<const void*>(address));
}

template<class T> T Method(uintptr_t address, void* self)
{
    return reinterpret_cast<T(__thiscall*)(void*)>(address)(self);
}

template<class T> T Virtual(void* self, size_t offset)
{
    return Method<T>(Read<uintptr_t>(Read<void*>(self), offset), self);
}

bool CopyText(char* dest, size_t capacity, const char* source, size_t limit)
{
    if (!source)
        return false;
    for (size_t i = 0; i < limit && i + 1 < capacity; ++i) {
        dest[i] = source[i];
        if (!dest[i])
            return i != 0;
    }
    return false;
}

void ReadCar(void* simable, char* name)
{
    __try {
        auto owner = Read<void*>(simable, 4);
        if (!owner)
            return;
        auto lookup = reinterpret_cast<void*(__thiscall*)(void*, uintptr_t)>(0x5D59F0);
        auto vehicle = lookup(owner, 0x4040E0);
        if (!vehicle)
            return;
        char model[49]{};
        if (!CopyText(model, sizeof(model), Virtual<const char*>(vehicle, 0x4C), 48))
            return;
        char label[64]{};
        std::snprintf(label, sizeof(label), "CAR_NAME_%s", model);
        const auto hash = reinterpret_cast<unsigned(__cdecl*)(const char*)>(0x5AF1C0)(label);
        if (reinterpret_cast<bool(__cdecl*)(unsigned)>(0x56BC00)(hash)) {
            const auto localized = reinterpret_cast<const char*(__cdecl*)(unsigned)>(0x56BC10)(hash);
            if (CopyText(name, 65, localized, 64))
                return;
        }
        CopyText(name, 65, model, sizeof(model));
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        name[0] = 0;
        OutputDebugStringA("MWLeaderboard: could not read vehicle name.\n");
    }
}

bool ReadResult(void* racer, void* simable, Result* result, const char** rejection = nullptr)
{
    if (rejection)
        *rejection = nullptr;
    __try {
        if (!simable || !Virtual<bool>(simable, 0x24))
            return false;

        void* player = Virtual<void*>(simable, 0x20);
        if (!player || Virtual<int>(player, 0x1C) != 0)
            return false;

        if (rejection)
            *rejection = "Skipped: failed or eliminated run";
        if (Read<unsigned char>(racer, 0x1C) || Read<unsigned char>(racer, 0x1D)
            || Read<unsigned char>(racer, 0x1E))
            return false;

        if (rejection)
            *rejection = "Capture: race state unavailable";
        void* status = Global<void*>(RaceStatus);
        if (!status || Read<int>(status, 0x1964) == 1)
            return false;
        void* event = Read<void*>(status, 0x1968);
        void* database = Global<void*>(FrontendDatabase);
        if (!event || !database)
            return false;

        if (rejection)
            *rejection = "Capture: profile unavailable";
        void* profile = Read<void*>(database, 0x10);
        if (!profile || !Read<unsigned char>(profile, 0x20)
            || !CopyText(result->profile, sizeof(result->profile),
                         static_cast<const char*>(profile), 32))
            return false;

        if (rejection)
            *rejection = "Capture: invalid finish time";
        const auto seconds = Read<float>(racer, 0x150);
        if (!(seconds >= 1.0f && seconds <= 21600.0f))
            return false;

        if (rejection)
            *rejection = "Capture: unsupported race type";
        result->raceType = Method<int>(0x5FAA20, event);
        if (result->raceType < 0 || result->raceType >= static_cast<int>(std::size(races::Modes)))
            return false;
        if (rejection)
            *rejection = "Skipped: Tollbooth timeout";
        if (result->raceType == TollboothMode && !(Method<float>(RaceTimeRemaining, status) > 0.0f))
            return false;

        result->eventHash = Method<unsigned>(0x5FAB40, event);
        result->laps = races::Modes[result->raceType].hasLaps ? Method<int>(0x5FBA00, event) : 1;
        result->reversed = Read<uintptr_t>(event) == CustomRaceVtable
                        && Read<unsigned char>(event, 0x1C) != 0;
        result->timeMs = static_cast<int>(static_cast<double>(seconds) * 1000.0 + 0.5);
        if (result->raceType == 5) {
            if (rejection)
                *rejection = "Capture: invalid Speedtrap score";
            const auto speed = Read<float>(racer, 0x120);
            if (!(speed >= 0.0f && speed <= 1000000.0f))
                return false;
            result->speedMilliKmh = static_cast<int>(static_cast<double>(speed) * 1000.0 + 0.5);
        }
        if (rejection)
            *rejection = "Capture: event data unavailable";
        if (!result->eventHash || result->laps < 1 || result->laps > 99
            || !CopyText(result->eventId, sizeof(result->eventId),
                         Method<const char*>(0x5FBA70, event), 48))
            return false;

        const auto hash = reinterpret_cast<unsigned(__cdecl*)(const char*, void*)>(0x56E560)(
            "TRACKNAME_", event);
        if (reinterpret_cast<bool(__cdecl*)(unsigned)>(0x56BC00)(hash)) {
            const auto name = reinterpret_cast<const char*(__cdecl*)(unsigned)>(0x56BC10)(hash);
            if (!CopyText(result->eventName, sizeof(result->eventName), name, 96))
                result->eventName[0] = 0;
        }
        ReadCar(simable, result->car);
        if (rejection)
            *rejection = nullptr;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        if (rejection && *rejection)
            *rejection = "Capture: could not read game memory";
        OutputDebugStringA("MWLeaderboard: could not read race result.\n");
        return false;
    }
}

std::string JsonString(const char* input)
{
    wchar_t wide[128]{};
    char utf8[512]{};
    if (!MultiByteToWideChar(CP_ACP, 0, input, -1, wide, 128)
        || !WideCharToMultiByte(CP_UTF8, 0, wide, -1, utf8, 512, nullptr, nullptr))
        return "\"\"";

    std::string encoded = "\"";
    for (const unsigned char ch : std::string(utf8)) {
        if (ch == '"' || ch == '\\')
            encoded += '\\';
        if (ch >= 32)
            encoded += static_cast<char>(ch);
    }
    return encoded + '"';
}

bool SaveResult(const Result& result)
{
    GUID guid{};
    if (FAILED(CoCreateGuid(&guid))) {
        SetCaptureStatus("Capture: could not create result ID");
        return false;
    }
    wchar_t id[39]{};
    if (!StringFromGUID2(guid, id, 39)) {
        SetCaptureStatus("Capture: could not format result ID");
        return false;
    }

    const std::wstring fileId(id + 1, 36);
    const std::string jsonId(fileId.begin(), fileId.end());
    char eventHash[9]{};
    std::snprintf(eventHash, sizeof(eventHash), "%08x", result.eventHash);
    auto pending = queuePath / (fileId + L".tmp");
    auto ready = queuePath / (fileId + L".json");
    std::ofstream file(pending, std::ios::binary | std::ios::trunc);
    file << "{\"id\":\"" << jsonId
         << "\",\"profile\":" << JsonString(result.profile)
         << ",\"event_hash\":\"" << eventHash
         << "\",\"event_id\":" << JsonString(result.eventId)
         << ",\"event_name\":" << JsonString(result.eventName)
         << ",\"mode\":\"" << races::Modes[result.raceType].name
         << "\",\"laps\":" << result.laps
         << ",\"reversed\":" << (result.reversed ? "true" : "false")
         << ",\"car\":" << JsonString(result.car)
         << ",\"time_ms\":" << result.timeMs;
    if (result.speedMilliKmh >= 0)
        file << ",\"speed_milli_kmh\":" << result.speedMilliKmh;
    file << "}\n";
    file.close();
    if (file && MoveFileExW(pending.c_str(), ready.c_str(), MOVEFILE_WRITE_THROUGH)) {
        SetCaptureStatus("Result saved locally");
        OutputDebugStringA("MWLeaderboard: race result saved.\n");
        if (uploadSignal)
            SetEvent(uploadSignal);
        return true;
    }
    DeleteFileW(pending.c_str());
    SetCaptureStatus("Capture: cannot write result file");
    OutputDebugStringA("MWLeaderboard: could not save race result.\n");
    return false;
}

void __fastcall OnFinish(void* racer, void* simable)
{
    const bool wasFinished = Read<unsigned char>(racer, 0x21) != 0;
    Method<void>(FinishRace, racer);
    if (wasFinished) {
        Result previous{};
        if (ReadResult(racer, simable, &previous))
            SetCaptureStatus("Finish skipped: racer already finalized");
        return;
    }

    Result result{};
    const char* rejection{};
    if (!ReadResult(racer, simable, &result, &rejection)) {
        if (rejection)
            SetCaptureStatus(rejection);
        return;
    }
    if (result.raceType == 5) {
        SetCaptureStatus(speedtrapCaptureReady ? "Speedtrap: waiting for final score"
                                              : "Speedtrap capture unavailable: hook conflict");
        startup::Log("Speedtrap finish detected. Waiting for finalized race statistics.");
        return;
    }
    try {
        SaveResult(result);
    } catch (...) {
        SetCaptureStatus("Capture: result queue error");
        OutputDebugStringA("MWLeaderboard: result queue error.\n");
    }
}

__declspec(naked) void FinishBridge()
{
    __asm {
        mov edx, edi
        jmp OnFinish
    }
}

void __fastcall OnRaceStart(void* racer, void*)
{
    speedtrapSaved = false;
    Method<void>(StartRace, racer);
}

bool ReadFinishedSpeedtrap(void* status, Result* result)
{
    __try {
        if (!status || status != Global<void*>(RaceStatus) || Read<int>(status, 0x1964) == 1)
            return false;
        const auto event = Read<void*>(status, 0x1968);
        if (!event || Method<int>(0x5FAA20, event) != 5)
            return false;
        const auto count = Read<int>(status, 0x1958);
        if (count < 1 || count > 16)
            return false;
        for (int index = 0; index < count; ++index) {
            auto racer = static_cast<unsigned char*>(status) + 0x18 + index * 0x194;
            if (!Read<unsigned char>(racer, 0x21) || !(Read<float>(racer, 0x18) >= 100.0f))
                continue;
            const auto handle = Read<unsigned>(racer);
            if (!handle)
                continue;
            const auto first = Global<uintptr_t>(0x92FEB4);
            const auto size = Global<unsigned>(0x92FEBC);
            if (!first || size > 1000000)
                continue;
            const auto lookup = reinterpret_cast<void*(__cdecl*)(uintptr_t, uintptr_t, unsigned)>(0x5D51F0);
            const auto entry = lookup(first, first + size * 8, handle);
            if (!entry)
                continue;
            const char* rejection{};
            if (ReadResult(racer, Read<void*>(entry, 4), result, &rejection))
                return true;
            if (rejection)
                SetCaptureStatus(rejection);
        }
        return false;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        startup::Log("Could not read finalized Speedtrap statistics.");
        return false;
    }
}

void __fastcall OnFinalizeRaceStats(void* status, void*)
{
    Method<void>(FinalizeRaceStats, status);
    if (speedtrapSaved)
        return;
    Result result{};
    if (!ReadFinishedSpeedtrap(status, &result))
        return;
    try {
        if (SaveResult(result)) {
            speedtrapSaved = true;
            char message[160]{};
            std::snprintf(message, sizeof(message), "Speedtrap result saved: time_ms=%d, speed_milli_kmh=%d.",
                          result.timeMs, result.speedMilliKmh);
            startup::Log(message);
        }
    } catch (...) {
        SetCaptureStatus("Capture: Speedtrap queue error");
        startup::Log("Could not queue the finalized Speedtrap result.");
    }
}

bool CheckExecutable()
{
    auto base = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
    if (reinterpret_cast<uintptr_t>(base) != ImageBase) {
        startup::Log("Unsupported speed.exe load address. Expected 0x00400000.");
        return false;
    }
    if (!game_image::Readable(ImageBase, sizeof(IMAGE_DOS_HEADER))) {
        startup::Log("Could not read the game executable header.");
        return false;
    }
    const auto dos = Read<IMAGE_DOS_HEADER>(base);
    if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0 || dos.e_lfanew >= 4096
        || !game_image::Readable(ImageBase + dos.e_lfanew, sizeof(IMAGE_NT_HEADERS32))) {
        startup::Log("Could not read the game executable header.");
        return false;
    }
    const auto pe = Read<IMAGE_NT_HEADERS32>(base, dos.e_lfanew);
    if (pe.Signature != IMAGE_NT_SIGNATURE || pe.FileHeader.Machine != IMAGE_FILE_MACHINE_I386
        || pe.FileHeader.SizeOfOptionalHeader < sizeof(IMAGE_OPTIONAL_HEADER32)
        || pe.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
        startup::Log("The game executable is not a valid 32-bit x86 PE image.");
        return false;
    }
    return true;
}

bool CaptureCodeMatches(uintptr_t address, std::string_view expected)
{
    if (game_image::MatchesCode(address, expected))
        return true;
    char message[96]{};
    std::snprintf(message, sizeof(message), "Capture blocked: code changed at %08X",
                  static_cast<unsigned>(address));
    SetCaptureStatus(message);
    startup::Log(message);
    return false;
}

bool InstallFinishHook()
{
    if (!CaptureCodeMatches(FinishCall - 6, "\x8B\xCB\x89\x5C\x24\x14\xE8\x58\x7D\xFD\xFF\xC7\x43\x18"sv)
        || !CaptureCodeMatches(FinishRace, "\x83\xEC\x10\x56\x8B\xF1\x8A\x86\x54\x01\x00\x00"sv)
        || !CaptureCodeMatches(RaceTimeRemaining, "\x51\x56\x8B\xF1\x8B\x8E\x68\x19\x00\x00"sv)
        || !CaptureCodeMatches(0x5FAA20, "\x56\x8B\xF1\x8B\x46\x04\x85\xC0\x74\x06\x0F\xBE"sv))
        return false;
    if (!game_image::Readable(RaceStatus, sizeof(void*))
        || !game_image::Readable(FrontendDatabase, sizeof(void*))) {
        SetCaptureStatus("Capture blocked: game data unavailable");
        return false;
    }

    struct Patch { uintptr_t site, original, replacement; };
    const auto apply = [](const Patch& patch, bool restore) {
        auto site = reinterpret_cast<unsigned char*>(patch.site);
        DWORD protection{};
        if (!VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &protection))
            return false;
        auto operand = reinterpret_cast<volatile LONG*>(site + 1);
        const auto original = static_cast<LONG>(patch.original - (patch.site + 5));
        const auto replacement = static_cast<LONG>(patch.replacement - (patch.site + 5));
        const auto expected = restore ? replacement : original;
        const auto desired = restore ? original : replacement;
        const bool changed = InterlockedCompareExchange(operand, desired, expected) == expected;
        FlushInstructionCache(GetCurrentProcess(), site, 5);
        DWORD ignored{};
        VirtualProtect(site, 5, protection, &ignored);
        return changed;
    };
    const Patch finish{FinishCall, FinishRace, reinterpret_cast<uintptr_t>(&FinishBridge)};
    if (!apply(finish, false)) {
        SetCaptureStatus("Capture blocked: cannot install finish hook");
        return false;
    }
    captureHookInstalled = true;

    if (!CaptureCodeMatches(StartCall, "\xE8\x7D\xE3\xFF\xFF"sv)
        || !CaptureCodeMatches(StartRace, "\x51\xA1\x00\xE0\x91\x00\x56\x8B\xF1"sv)
        || !CaptureCodeMatches(FinalizeCall1, "\xE8\xB5\xC8\xFF\xFF"sv)
        || !CaptureCodeMatches(FinalizeCall2, "\xE8\x69\x75\xFF\xFF"sv)
        || !CaptureCodeMatches(FinalizeCall3, "\xE8\x80\x02\xFF\xFF"sv)
        || !CaptureCodeMatches(FinalizeRaceStats, "\xA1\x00\xE0\x91\x00\x53\x8B\xD9"sv)
        || !CaptureCodeMatches(0x5D51F0, "\x83\xEC\x08\x8B\x4C\x24\x0C\x56\x8B\x74\x24\x14"sv)
        || !game_image::Readable(0x92FEB4, sizeof(void*))
        || !game_image::Readable(0x92FEBC, sizeof(unsigned))) {
        SetCaptureStatus("Timed capture ready; Speedtrap hook conflict");
        return true;
    }
    const Patch patches[] = {
        {StartCall, StartRace, reinterpret_cast<uintptr_t>(&OnRaceStart)},
        {FinalizeCall1, FinalizeRaceStats, reinterpret_cast<uintptr_t>(&OnFinalizeRaceStats)},
        {FinalizeCall2, FinalizeRaceStats, reinterpret_cast<uintptr_t>(&OnFinalizeRaceStats)},
        {FinalizeCall3, FinalizeRaceStats, reinterpret_cast<uintptr_t>(&OnFinalizeRaceStats)}
    };
    size_t installed = 0;
    for (; installed < std::size(patches); ++installed) {
        if (!apply(patches[installed], false)) {
            while (installed)
                apply(patches[--installed], true);
            SetCaptureStatus("Timed capture ready; Speedtrap hook failed");
            return true;
        }
    }
    speedtrapCaptureReady = true;
    SetCaptureStatus("Capture ready");
    startup::Log("Race capture installed, including finalized Speedtrap scores.");
    return true;
}

struct InternetHandle {
    HINTERNET value;
    ~InternetHandle() { if (value) WinHttpCloseHandle(value); }
};

DWORD RequestHttp(const std::wstring& url, const std::wstring& token,
                  std::string& body, std::string* response = nullptr, bool autoRegister = false)
{
    URL_COMPONENTS parts{sizeof(parts)};
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    parts.dwUserNameLength = static_cast<DWORD>(-1);
    parts.dwPasswordLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts) || parts.dwExtraInfoLength
        || parts.dwUserNameLength || parts.dwPasswordLength)
        return 0;

    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    const bool secure = parts.nScheme == INTERNET_SCHEME_HTTPS;
    if (!secure && (parts.nScheme != INTERNET_SCHEME_HTTP || host != L"127.0.0.1"))
        return 0;
    const std::wstring path = parts.dwUrlPathLength
        ? std::wstring(parts.lpszUrlPath, parts.dwUrlPathLength) : L"/";
    InternetHandle session{WinHttpOpen(L"MWLeaderboard/0.1", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                       WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
    if (!session.value)
        return 0;
    WinHttpSetTimeouts(session.value, 5000, 5000, 5000, 5000);
    InternetHandle connection{WinHttpConnect(session.value, host.c_str(), parts.nPort, 0)};
    if (!connection.value)
        return 0;
    InternetHandle request{WinHttpOpenRequest(connection.value, response ? L"GET" : L"POST", path.c_str(), nullptr,
                                               WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                               secure ? WINHTTP_FLAG_SECURE : 0)};
    DWORD disabled = WINHTTP_DISABLE_REDIRECTS;
    if (!request.value || !WinHttpSetOption(request.value, WINHTTP_OPTION_DISABLE_FEATURE,
                                           &disabled, sizeof(disabled)))
        return 0;

    std::wstring headers = response ? L"Accept: application/x-ndjson\r\nX-MWLeaderboard-All-Modes: 1\r\nX-MWLeaderboard-Version: 2\r\n"
        : L"Content-Type: application/json\r\nAuthorization: Bearer " + token + L"\r\n";
    if (response && !token.empty())
        headers += L"Authorization: Bearer " + token + L"\r\n";
    if (autoRegister)
        headers += L"X-MWLeaderboard-Register: 1\r\n";
    const auto size = static_cast<DWORD>(body.size());
    if (!WinHttpSendRequest(request.value, headers.c_str(), static_cast<DWORD>(-1),
                            size ? body.data() : nullptr, size, size, 0)
        || !WinHttpReceiveResponse(request.value, nullptr))
        return 0;
    DWORD status{};
    DWORD length = sizeof(status);
    if (!WinHttpQueryHeaders(request.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &length, WINHTTP_NO_HEADER_INDEX))
        return 0;
    if (response && status == 200) {
        constexpr size_t limit = 4 * 1024 * 1024;
        const auto started = GetTickCount64();
        char buffer[8192];
        for (;;) {
            DWORD received{};
            if (GetTickCount64() - started > 10000
                || !WinHttpReadData(request.value, buffer, sizeof(buffer), &received)
                || response->size() + received > limit)
                return 0;
            if (!received)
                break;
            response->append(buffer, received);
        }
    }
    return status;
}

std::wstring ReadSetting(const wchar_t* name)
{
    wchar_t value[2048]{};
    GetPrivateProfileStringW(L"Leaderboard", name, L"", value, 2048, configPath.c_str());
    return value;
}

std::wstring Endpoint(const wchar_t* setting, const wchar_t* path)
{
    if (ReadSetting(L"Online") == L"0")
        return {};
    if (server_config::BaseUrl[0])
        return std::wstring(server_config::BaseUrl) + path;
    return ReadSetting(setting);
}

std::wstring ReadPlayerToken(const fs::path& path);

void DownloadStandings()
{
    std::wstring url;
    std::uint64_t generation;
    {
        const std::lock_guard lock(snapshotLock);
        if (!snapshotRequested)
            return;
        url = snapshotUrl;
        generation = snapshotGeneration;
        snapshotRequested = false;
    }

    online::Pages pages;
    try {
        std::string body, response;
        auto credential = ReadSetting(L"PlayerToken");
        if (credential.empty())
            credential = ReadPlayerToken(configPath.parent_path() / L"MWLeaderboard.player");
        if (RequestHttp(url, credential, body, &response) == 200) {
            auto parsed = standings::ParseSnapshot(response);
            if (!parsed.empty())
                pages = std::make_shared<const std::vector<standings::Page>>(std::move(parsed));
        }
    } catch (...) {
        OutputDebugStringA("MWLeaderboard: standings download failed.\n");
    }
    const std::lock_guard lock(snapshotLock);
    if (generation == snapshotGeneration)
        snapshot = {std::move(pages), false};
}

bool ValidToken(std::wstring_view token)
{
    return token.size() == 64
        && token.find_first_not_of(L"0123456789abcdef") == std::wstring_view::npos;
}

std::wstring ReadPlayerToken(const fs::path& path)
{
    char bytes[65]{};
    std::ifstream file(path, std::ios::binary);
    file.read(bytes, sizeof(bytes));
    if (!file.eof() || file.gcount() != 64)
        return {};

    const std::wstring token(bytes, bytes + 64);
    return ValidToken(token) ? token : std::wstring{};
}

std::wstring CreatePlayerToken()
{
    const auto path = configPath.parent_path() / L"MWLeaderboard.player";
    if (fs::exists(path))
        return ReadPlayerToken(path);

    unsigned char random[32]{};
    if (BCryptGenRandom(nullptr, random, sizeof(random), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
        return {};

    std::string token;
    token.reserve(64);
    constexpr char hex[] = "0123456789abcdef";
    for (const auto byte : random) {
        token += hex[byte >> 4];
        token += hex[byte & 15];
    }
    GUID guid{};
    wchar_t id[39]{};
    if (FAILED(CoCreateGuid(&guid)) || !StringFromGUID2(guid, id, 39))
        return {};

    const auto pending = path.parent_path() / (std::wstring(id + 1, 36) + L".player.tmp");
    std::ofstream file(pending, std::ios::binary | std::ios::trunc);
    file.write(token.data(), static_cast<std::streamsize>(token.size()));
    file.close();
    if (file && MoveFileExW(pending.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH))
        return std::wstring(token.begin(), token.end());

    DeleteFileW(pending.c_str());
    return ReadPlayerToken(path);
}

void UploadPending()
{
    const auto url = Endpoint(L"SubmitUrl", L"/api/runs");
    if (url.empty()) {
        SetUploadStatus("Online disabled");
        return;
    }

    auto credential = ReadSetting(L"PlayerToken");
    const bool autoRegister = credential.empty()
        && (server_config::BaseUrl[0] || ReadSetting(L"AutoRegister") == L"1");
    if (!autoRegister && !ValidToken(credential)) {
        SetUploadStatus("Invalid INI PlayerToken");
        return;
    }

    bool uploaded = false;
    for (const auto& entry : fs::directory_iterator(queuePath)) {
        {
            const std::lock_guard lock(snapshotLock);
            if (snapshotRequested)
                break;
        }
        if (entry.path().extension() != L".json" || !entry.is_regular_file())
            continue;
        const auto size = entry.file_size();
        if (size == 0 || size > 4096)
            continue;
        std::string body(static_cast<size_t>(size), '\0');
        {
            std::ifstream file(entry.path(), std::ios::binary);
            if (!file.read(body.data(), static_cast<std::streamsize>(body.size())))
                continue;
        }
        if (credential.empty())
            credential = CreatePlayerToken();
        if (credential.empty()) {
            SetUploadStatus("Player identity file unreadable");
            OutputDebugStringA("MWLeaderboard: could not save the player token.\n");
            break;
        }
        const DWORD status = RequestHttp(url, credential, body, nullptr, autoRegister);
        if (status == 200 || status == 201) {
            SetUploadStatus("Upload accepted");
            uploaded = true;
            const auto destination = archivePath / entry.path().filename();
            if (!MoveFileExW(entry.path().c_str(), destination.c_str(),
                             MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                OutputDebugStringA("MWLeaderboard: uploaded result could not be moved to history.\n");
            continue;
        }
        char failure[80]{};
        if (status)
            std::snprintf(failure, sizeof(failure), "Upload HTTP %lu", status);
        else
            std::snprintf(failure, sizeof(failure), "Upload connection failed");
        SetUploadStatus(failure);
        if (status == 400 || status == 409 || status == 413 || status == 422) {
            auto rejected = entry.path();
            rejected.replace_extension(L".rejected");
            MoveFileExW(entry.path().c_str(), rejected.c_str(), MOVEFILE_REPLACE_EXISTING);
            continue;
        }
        OutputDebugStringA("MWLeaderboard: upload deferred. Check server and credentials.\n");
        break;
    }
    if (uploaded) {
        const std::lock_guard lock(snapshotLock);
        if (!snapshotUrl.empty() && !snapshotRequested) {
            ++snapshotGeneration;
            snapshot.pending = true;
            snapshotRequested = true;
        }
    }
}

DWORD WINAPI Run(void* module)
{
    try {
        wchar_t dllPath[32768]{};
        const DWORD length = GetModuleFileNameW(static_cast<HMODULE>(module), dllPath, 32768);
        if (!length || length >= 32768)
            return 0;
        const auto parent = fs::path(dllPath).parent_path();
        DeleteFileW((parent / L"MWLeaderboard-startup.log").c_str());
        wchar_t temp[MAX_PATH]{};
        const auto tempLength = GetTempPathW(MAX_PATH, temp);
        if (tempLength && tempLength < std::size(temp))
            DeleteFileW((fs::path(temp) / L"MWLeaderboard-startup.log").c_str());
        startup::Log("MWLeaderboard ASI loaded.");
        if (!CheckExecutable())
            return 0;

        HMODULE pinned{};
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                                reinterpret_cast<LPCWSTR>(module), &pinned)) {
            startup::Log("Could not keep the ASI loaded.");
            return 0;
        }

        configPath = parent / L"MWLeaderboard.ini";
        if (ReadSetting(L"Online") == L"0")
            startup::Log("Online standings disabled by Online=0.");
        else if (server_config::BaseUrl[0])
            startup::Log("Embedded leaderboard server address is configured.");
        else if (ReadSetting(L"StandingsUrl").empty())
            startup::Log("Online standings unavailable. Build the ASI with your Worker address.");
        else
            startup::Log("Using the leaderboard server address from the INI.");
        queuePath = parent / L"MWLeaderboardQueue";
        archivePath = parent / L"MWLeaderboardHistory";
        uploadSignal = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!uploadSignal) {
            SetUploadStatus("Upload worker unavailable");
            startup::Log("Could not start online standings or uploads.");
        }

        const bool menuInstalled = native_menu::Install(archivePath, queuePath);
        if (menuInstalled)
            startup::Log("Main-menu hooks installed.");

        std::error_code queueError;
        std::error_code historyError;
        fs::create_directories(queuePath, queueError);
        fs::create_directories(archivePath, historyError);
        if (queueError) {
            SetCaptureStatus("Capture blocked: scripts folder not writable");
            startup::Log("Could not create the results queue. Check write access for the scripts folder.");
        }
        if (historyError)
            startup::Log("Could not create the results history. Check write access for the scripts folder.");

        if (!queueError && !InstallFinishHook())
            startup::Log("Race capture hook could not be installed.");
        if (!uploadSignal)
            return 0;
        do {
            try {
                DownloadStandings();
                if (!queueError)
                    UploadPending();
                DownloadStandings();
            } catch (...) {
                SetUploadStatus("Upload queue unreadable");
                OutputDebugStringA("MWLeaderboard: could not read upload queue.\n");
            }
        } while (WaitForSingleObject(uploadSignal, 10000) != WAIT_FAILED);
    } catch (...) {
        startup::Log("Could not complete ASI initialization.");
    }
    return 0;
}
}

namespace game_image {
namespace {
bool CheckMemory(std::uintptr_t address, std::size_t size, bool code)
{
    while (size) {
        MEMORY_BASIC_INFORMATION region{};
        if (!VirtualQuery(reinterpret_cast<const void*>(address), &region, sizeof(region))
            || region.AllocationBase != reinterpret_cast<const void*>(ImageBase)
            || region.State != MEM_COMMIT || region.Type != MEM_IMAGE
            || (region.Protect & PAGE_GUARD))
            return false;

        const auto protection = region.Protect & 0xFF;
        const bool readCode = protection == PAGE_EXECUTE_READ
            || protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY;
        const bool readData = protection == PAGE_READONLY
            || protection == PAGE_READWRITE || protection == PAGE_WRITECOPY;
        if (!readCode && (code || !readData))
            return false;

        const auto offset = address - reinterpret_cast<std::uintptr_t>(region.BaseAddress);
        if (offset >= region.RegionSize)
            return false;
        const auto checked = std::min(size, static_cast<std::size_t>(region.RegionSize - offset));
        address += checked;
        size -= checked;
    }
    return true;
}
}

bool Readable(std::uintptr_t address, std::size_t size)
{
    return CheckMemory(address, size, false);
}

bool MatchesCode(std::uintptr_t address, std::string_view expected)
{
    return CheckMemory(address, expected.size(), true)
        && std::memcmp(reinterpret_cast<const void*>(address), expected.data(), expected.size()) == 0;
}
}

namespace startup {
void Log(const char* message) noexcept
{
    char line[256]{};
    const auto size = std::snprintf(line, sizeof(line), "MWLeaderboard [PID %lu] %s\r\n",
                                    GetCurrentProcessId(), message);
    if (size <= 0 || size >= static_cast<int>(sizeof(line)))
        return;
    OutputDebugStringA(line);

}
}

namespace diagnostics {
std::string Status()
{
    if (captureHookInstalled && (!game_image::Readable(FinishCall, 5)
        || Read<unsigned char>(reinterpret_cast<const void*>(FinishCall)) != 0xE8
        || Read<LONG>(reinterpret_cast<const void*>(FinishCall + 1))
            != static_cast<LONG>(reinterpret_cast<uintptr_t>(&FinishBridge) - (FinishCall + 5))))
        SetCaptureStatus("Capture hook replaced by another mod");
    const std::lock_guard lock(diagnosticLock);
    return captureStatus + " | " + uploadStatus;
}
}

namespace online {
namespace {
bool ReadProfile(char* name)
{
    __try {
        const auto database = Global<void*>(FrontendDatabase);
        const auto profile = database ? Read<void*>(database, 0x10) : nullptr;
        return profile && CopyText(name, 33, static_cast<const char*>(profile), 32);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
}

std::string Profile()
{
    char name[33]{};
    wchar_t wide[64]{};
    char utf8[256]{};
    if (!ReadProfile(name) || !MultiByteToWideChar(CP_ACP, 0, name, -1, wide, 64)
        || !WideCharToMultiByte(CP_UTF8, 0, wide, -1, utf8, 256, nullptr, nullptr))
        return {};
    return utf8;
}

bool Request()
{
    if (!uploadSignal)
        return false;
    auto url = Endpoint(L"StandingsUrl", L"/api/snapshot");
    if (url.empty())
        return false;
    {
        const std::lock_guard lock(snapshotLock);
        if (url != snapshotUrl)
            snapshot.pages.reset();
        snapshotUrl = std::move(url);
        ++snapshotGeneration;
        snapshot.pending = true;
        snapshotRequested = true;
    }
    SetEvent(uploadSignal);
    return true;
}

Snapshot Get()
{
    const std::lock_guard lock(snapshotLock);
    return snapshot;
}
}

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        if (const auto thread = CreateThread(nullptr, 0, Run, module, 0, nullptr))
            CloseHandle(thread);
    }
    return TRUE;
}

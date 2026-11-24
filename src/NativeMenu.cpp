#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "GameImage.h"
#include "LocalStandings.h"
#include "LeaderboardBrowser.h"
#include "NativeMenu.h"
#include "OnlineStandings.h"
#include "StartupLog.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace native_menu {
namespace {
using namespace std::string_view_literals;
constexpr uintptr_t MenuSetupCall = 0x5451F3;
constexpr uintptr_t LastButton = 0x571E80;
constexpr uintptr_t MainRefreshSlot = 0x89EAE0;
constexpr uintptr_t RefreshHeader = 0x573870;
constexpr uintptr_t RankingsFactorySlot = 0x8F444C;
constexpr uintptr_t RankingsFactory = 0x5712C0;
constexpr uintptr_t SwitchScreen = 0x525940;
constexpr uintptr_t ProcessResponse = 0x5BBC00;
constexpr uintptr_t ResponseCalls[] = {
    0x5BEB01, 0x5BEB3D, 0x5C1570, 0x5C1639, 0x5C172D, 0x5C1769,
};
constexpr char MainMenuPackage[] = "MainMenu.fng";
constexpr char RankingsPackage[] = "RapSheetRankingsDetail.fng";

constexpr unsigned Accept = 0x0C407210;
constexpr unsigned PadAccept = 0x406415E3;
constexpr unsigned SetInitialFocus = 0x35F8620B;
constexpr unsigned PreviousPage = 0x72619778;
constexpr unsigned NextPage = 0x911C0A4B;
constexpr unsigned Back = 0x911AB364;
constexpr unsigned ExitScreen = 0xE1FDE1D1;
constexpr unsigned ScrollUp = 0x9120409E;
constexpr unsigned ScrollDown = 0xB5971BF1;
constexpr unsigned Tick = 0xC98356BA;
constexpr unsigned SwitchView = 0xC519BFC4;
constexpr unsigned RankingsTitle = 0x8224E17C;
constexpr unsigned PersonalText = 0x5A1EAD01;
constexpr unsigned Gold = 0xFFFFCC66;
constexpr const char* ColumnNames[] = {"RANK_%d", "PLAYER_NAME_%d", "VEHICLE_%d", "TIME_%d"};

std::filesystem::path archivePath;
std::filesystem::path queuePath;
std::vector<standings::Page> localPages;
online::Snapshot shared;
std::string diagnosticText;
bool onlineConfigured = false;
bool preferOnline = false;
standings::Browser browser;
bool closingScreen = false;
bool enterHeld = false;
bool enterConsumed = false;
bool escapeConsumed = false;
unsigned lastBackMessage = 0;
ULONGLONG lastBackAt = 0;
uintptr_t optionMethods[2];
uintptr_t screenMethods[3];
char screenTag;
void* activeScreen = nullptr;
void* activePackage = nullptr;
struct ColumnPosition {
    float x{};
    float y{};
    bool captured = false;
};
std::array<ColumnPosition, standings::RowsPerPage> carPositions{};

template<class T>
T& Field(void* object, size_t offset)
{
    return *reinterpret_cast<T*>(static_cast<unsigned char*>(object) + offset);
}

template<class R, class... Args>
R Invoke(uintptr_t address, void* self, Args... args)
{
    return reinterpret_cast<R(__thiscall*)(void*, Args...)>(address)(self, args...);
}

template<class R, class... Args>
R Call(uintptr_t address, Args... args)
{
    return reinterpret_cast<R(__cdecl*)(Args...)>(address)(args...);
}

void SetText(const char* package, unsigned hash, const char* text, unsigned color = 0xFFFFFFFF)
{
    auto string = Call<void*>(0x5251E0, package, hash);
    if (!string)
        return;
    Invoke<void>(0x5BCC10, string, text);
    Field<unsigned>(string, 0x74) = 0;
    Call<void>(0x5157E0, string, color);
    Field<unsigned>(string, 0x1C) |= 0x00400000;
    Call<void>(0x514CC0, string);
}

void SetHeaderFont(void* string, const char* package)
{
    const auto body = Call<void*>(0x5251E0, package, Call<unsigned>(0x5AF1C0, "PLAYER_NAME_1"));
    if (string && body) {
        Field<unsigned>(string, 0x24) = Field<unsigned>(body, 0x24);
        Field<unsigned>(string, 0x70) = Field<unsigned>(body, 0x70);
        Field<unsigned>(string, 0x74) = 0;
        Field<unsigned>(string, 0x1C) |= 0x00400000;
    }
}

void ShowPersonal(const char* package, const std::string& text)
{
    auto label = Call<void*>(0x5251E0, package, PersonalText);
    if (!label && !text.empty() && activePackage) {
        const auto source = Call<void*>(0x5251E0, package, RankingsTitle);
        if (!source)
            return;
        const auto methods = Field<uintptr_t*>(source, 0);
        if (!methods || methods[1] != 0x50D8F0)
            return;
        label = Invoke<void*>(methods[1], source, false);
        if (!label)
            return;
        Field<unsigned>(label, 0x10) = PersonalText;
        Field<unsigned>(label, 0x58) = 0;
        float x{}, y{};
        Call<void>(0x514FD0, source, &x, &y);
        Call<void>(0x524CC0, label, x, y + 332.0f);
        Invoke<void>(0x50DB90, activePackage, label);
        SetHeaderFont(label, package);
    }
    if (label)
        SetText(package, PersonalText, text.c_str(), Gold);
}

std::string ReadableHeading(const standings::Page& page)
{
    if (page.mode.empty())
        return page.heading;
    const auto first = page.heading.find(" / ");
    if (first == std::string::npos)
        return page.heading;
    const auto next = page.heading.find(" / ", first + 3);
    return page.heading.substr(0, first)
        + (next == std::string::npos ? "" : page.heading.substr(next));
}

const std::vector<standings::Page>& VisiblePages()
{
    return preferOnline && shared.pages ? *shared.pages : localPages;
}

void PositionLapColumn(const char* package, size_t index, unsigned hash, bool selecting)
{
    const auto label = Call<void*>(0x5251E0, package, hash);
    if (!label)
        return;
    auto& original = carPositions[index];
    if (!original.captured) {
        Call<void>(0x514FD0, label, &original.x, &original.y);
        original.captured = true;
    }
    Call<void>(0x524CC0, label, original.x + (selecting ? 160.0f : 0.0f), original.y);
}

void RenderPage(void* screen)
{
    const auto package = Field<const char*>(screen, 0x10);
    if (screen == activeScreen && !activePackage) {
        const auto frontend = *reinterpret_cast<void**>(0x91CADC);
        if (frontend)
            activePackage = Invoke<void*>(0x516AF0, frontend, package);
    }
    const auto page = browser.View(VisiblePages());
    const char* view = preferOnline && shared.pages ? "Online"
        : preferOnline && shared.pending ? "Local (loading online)"
        : preferOnline && onlineConfigured ? "Local (offline)" : "Local";
    const std::string title = std::string(view) + " | " + ReadableHeading(page);
    SetHeaderFont(Call<void*>(0x5251E0, package, RankingsTitle), package);
    SetText(package, RankingsTitle, title.c_str());
    diagnosticText = diagnostics::Status();
    ShowPersonal(package, page.personal.empty() ? diagnosticText : page.personal);

    for (size_t index = 0; index < standings::RowsPerPage; ++index) {
        const auto row = index < page.rows.size() ? &page.rows[index] : nullptr;
        const std::string name = row ? (row->mine ? "YOU" : row->profile) : "";
        const char* values[] = {row ? row->rank.c_str() : "", name.c_str(),
                                row ? row->car.c_str() : "", row ? row->time.c_str() : ""};
        for (size_t col = 0; col < sizeof(ColumnNames) / sizeof(ColumnNames[0]); ++col) {
            const auto hash = Call<unsigned>(0x573140, ColumnNames[col], static_cast<int>(index + 1));
            const unsigned color = row && (row->mine || row->rank == ">") ? Gold : 0xFFFFFFFF;
            if (col == 2)
                PositionLapColumn(package, index, hash, page.mode.empty());
            SetText(package, hash, values[col], color);
        }
    }
}

bool ConfirmRequested(unsigned message)
{
    if (message != Tick && message != PadAccept)
        return false;

    const bool down = (GetAsyncKeyState(VK_RETURN) & 0x8000) != 0;
    DWORD foregroundProcess{};
    const auto foreground = GetForegroundWindow();
    if (foreground)
        GetWindowThreadProcessId(foreground, &foregroundProcess);
    if (foregroundProcess != GetCurrentProcessId()) {
        enterHeld = down;
        enterConsumed = down;
        return false;
    }
    if (!down) {
        enterHeld = false;
        enterConsumed = false;
        return message == PadAccept;
    }

    const bool requested = message == PadAccept || !enterHeld;
    enterHeld = true;
    if (!requested || enterConsumed)
        return false;
    enterConsumed = true;
    return true;
}

bool BackRequested(unsigned message)
{
    const bool down = (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
    if (!down)
        escapeConsumed = false;
    if (message != Back && message != ExitScreen)
        return false;

    const auto now = GetTickCount64();
    const bool pairedMessage = lastBackMessage && lastBackMessage != message && now - lastBackAt < 250;
    if ((down && escapeConsumed) || (!down && pairedMessage))
        return false;

    escapeConsumed = down;
    lastBackMessage = message;
    lastBackAt = now;
    return true;
}

void __fastcall OnRankings(void* screen, void*, unsigned message, void*, unsigned, unsigned)
{
    if (closingScreen || localPages.empty())
        return;
    try {
        const bool back = BackRequested(message);
        if ((message == Back || message == ExitScreen) && !back)
            return;
        if (back && !browser.Input(standings::BrowserInput::Back, VisiblePages())) {
            auto frontend = *reinterpret_cast<void**>(0x91CADC);
            if (!frontend)
                return;
            closingScreen = true;
            Invoke<void>(SwitchScreen, frontend, MainMenuPackage, static_cast<void*>(nullptr), 0, 0);
            return;
        }
        if (message != PadAccept && message != Back && message != ExitScreen
            && message != SwitchView && message != Tick
            && message != SetInitialFocus && message != PreviousPage && message != NextPage
            && message != ScrollUp && message != ScrollDown)
            return;

        const auto fresh = onlineConfigured ? online::Get() : online::Snapshot{};
        const bool changed = fresh.pages != shared.pages || fresh.pending != shared.pending
            || diagnostics::Status() != diagnosticText;
        shared = fresh;
        const bool confirm = ConfirmRequested(message);
        if (message == SwitchView && onlineConfigured) {
            preferOnline = !preferOnline;
            browser.Reset();
        } else if (message == PreviousPage || message == ScrollUp)
            browser.Input(standings::BrowserInput::Previous, VisiblePages());
        else if (message == NextPage || message == ScrollDown)
            browser.Input(standings::BrowserInput::Next, VisiblePages());
        else if (confirm) {
            browser.Input(standings::BrowserInput::Accept, VisiblePages());
            startup::Log("Leaderboard selection confirmed.");
        } else if (message == Tick && !changed)
            return;

        RenderPage(screen);
    } catch (...) {
        OutputDebugStringA("MWLeaderboard: could not render Rap Sheet standings.\n");
    }
}

void __fastcall ProcessLeaderboardResponse(void* engine, void*, void* response,
                                            void* object, void* package, unsigned parameter)
{
    if (activeScreen && package == activePackage && !closingScreen && response
        && Field<unsigned>(response, 0x0C) == Back) {
        OnRankings(activeScreen, nullptr, Back, object, parameter, 0);
        return;
    }
    Invoke<void>(ProcessResponse, engine, response, object, package, parameter);
}

void* __fastcall DestroyRankings(void* screen, void*, unsigned flags)
{
    if (screen == activeScreen) {
        activeScreen = nullptr;
        activePackage = nullptr;
        closingScreen = true;
    }
    return Invoke<void*>(0x586490, screen, flags);
}

void* __cdecl CreateRankings(void* constructorData)
{
    if (!constructorData || Field<void*>(constructorData, 8) != &screenTag)
        return Call<void*>(RankingsFactory, constructorData);

    auto screen = Call<void*>(0x652AD0, 0x2C);
    if (!screen)
        return nullptr;
    Invoke<void>(0x586290, screen, constructorData);
    Field<uintptr_t*>(screen, 0) = screenMethods;
    activeScreen = screen;
    carPositions = {};
    const auto frontend = *reinterpret_cast<void**>(0x91CADC);
    activePackage = frontend ? Invoke<void*>(0x516AF0, frontend, RankingsPackage) : nullptr;
    closingScreen = false;
    enterHeld = (GetAsyncKeyState(VK_RETURN) & 0x8000) != 0;
    enterConsumed = enterHeld;
    escapeConsumed = (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
    lastBackMessage = 0;
    lastBackAt = 0;
    startup::Log("Leaderboard Enter input initialized.");
    try {
        RenderPage(screen);
    } catch (...) {
        OutputDebugStringA("MWLeaderboard: could not render Rap Sheet standings.\n");
    }
    return screen;
}

void OpenLeaderboard() noexcept
{
    auto frontend = *reinterpret_cast<void**>(0x91CADC);
    auto manager = *reinterpret_cast<void**>(0x91D21C);
    if (!frontend || !manager || !Invoke<void*>(0x585820, manager, RankingsPackage)) {
        startup::Log("Rap Sheet rankings package is unavailable.");
        return;
    }

    try {
        localPages = standings::LoadPages(archivePath, queuePath, online::Profile());
        browser.Reset();
        onlineConfigured = online::Request();
        preferOnline = onlineConfigured;
        shared = onlineConfigured ? online::Get() : online::Snapshot{};
    } catch (...) {
        OutputDebugStringA("MWLeaderboard: could not read local standings.\n");
        return;
    }
    Invoke<void>(SwitchScreen, frontend, RankingsPackage, static_cast<void*>(&screenTag), 0, 0);
}

void __fastcall React(void*, void*, const char*, unsigned message, void*, unsigned, unsigned)
{
    if (message == Accept)
        OpenLeaderboard();
}

void __fastcall OnRefresh(void* menu, void*)
{
    Invoke<void>(RefreshHeader, menu);
    auto option = Field<void*>(menu, 0x38);
    if (option && Field<uintptr_t*>(option, 0) == optionMethods) {
        Call<void>(0x515CC0, Field<void*>(menu, 0x12C), L"Leaderboard");
        Call<void>(0x515CC0, Field<void*>(menu, 0x130), L"Leaderboard");
    }
}

unsigned SlotHash(const char* prefix, int index)
{
    char label[32]{};
    const auto length = std::snprintf(label, sizeof(label), "%s%d", prefix, index);
    if (length < 0 || length >= static_cast<int>(sizeof(label)))
        return 0;
    return Call<unsigned>(0x5AF1C0, static_cast<const char*>(label));
}

bool PrepareSlots(void* scroller, const char* package)
{
    const auto prefix = Field<const char*>(scroller, 0x1C);
    const auto count = Field<int>(scroller, 0x24);
    const auto bookends = Field<int>(scroller, 0xC0) / 2;
    if (!prefix || count < 2 || count > 32 || bookends < 0 || bookends > 4)
        return false;

    auto frontend = *reinterpret_cast<void**>(0x91CADC);
    const auto source = Call<void*>(0x5251A0, package, SlotHash(prefix, count - 1));
    auto fePackage = frontend ? Invoke<void*>(0x516AF0, frontend, package) : nullptr;
    if (!fePackage)
        return false;

    for (int index = count; index <= count + bookends; ++index) {
        const auto hash = SlotHash(prefix, index);
        if (!hash)
            return false;
        if (Call<void*>(0x5251A0, package, hash))
            continue;
        if (!source || Call<void*>(0x524850, package, hash))
            return false;
        const auto methods = Field<uintptr_t*>(source, 0);
        if (!methods || methods[1] != 0x5C44B0)
            return false;
        auto image = Invoke<void*>(methods[1], source, false);
        if (!image)
            return false;
        Field<unsigned>(image, 0x10) = hash;
        Field<unsigned>(image, 0x58) = 0;
        Call<void>(0x514C70, image);
        Invoke<void>(0x50DB90, fePackage, image);
        if (!Call<void*>(0x5251A0, package, hash))
            return false;
    }
    return true;
}

void __cdecl AddEntry(void* menu)
{
    if (!menu || Field<uintptr_t>(menu, 0) != 0x89EAD4)
        return;
    const auto package = Field<const char*>(menu, 0x10);
    if (!package || _stricmp(package, MainMenuPackage) != 0)
        return;
    auto scroller = static_cast<unsigned char*>(menu) + 0x2C;
    const auto sentinel = scroller + 4;
    auto link = Field<unsigned char*>(sentinel, 0);
    for (int i = 0; link && link != sentinel && i < 64; ++i) {
        if (Field<uintptr_t*>(link - 4, 0) == optionMethods)
            return;
        link = Field<unsigned char*>(link, 0);
    }
    if (!PrepareSlots(scroller, package)) {
        startup::Log("Main menu has no usable slot for the Leaderboard icon.");
        return;
    }

    auto option = Call<void*>(0x652AD0, 0x4C);
    if (!option) {
        startup::Log("Could not allocate the Leaderboard menu option.");
        return;
    }
    Invoke<void>(0x586FA0, option, 0xD13A2E66u, 0u, 0u);
    Field<uintptr_t*>(option, 0) = optionMethods;
    Field<unsigned char>(option, 0x45) = 1;
    Invoke<void>(0x573960, menu, option);
    startup::Log("Main-menu Leaderboard option added.");
}

__declspec(naked) void SetupBridge()
{
    __asm {
        push esi
        call AddEntry
        add esp, 4
        mov eax, 571E80h
        jmp eax
    }
}

bool PatchWord(uintptr_t address, uintptr_t expected, uintptr_t replacement)
{
    auto slot = reinterpret_cast<volatile LONG*>(address);
    DWORD protection{};
    auto memory = reinterpret_cast<void*>(address);
    if (!VirtualProtect(memory, sizeof(LONG), PAGE_EXECUTE_READWRITE, &protection))
        return false;
    const auto before = InterlockedCompareExchange(
        slot, static_cast<LONG>(replacement), static_cast<LONG>(expected));
    FlushInstructionCache(GetCurrentProcess(), memory, sizeof(LONG));
    DWORD ignored{};
    VirtualProtect(memory, sizeof(LONG), protection, &ignored);
    return before == static_cast<LONG>(expected);
}

void RestoreResponseHooks()
{
    for (const auto call : ResponseCalls) {
        const auto end = call + 5;
        PatchWord(call + 1, reinterpret_cast<uintptr_t>(&ProcessLeaderboardResponse) - end,
                  ProcessResponse - end);
    }
}

bool InstallResponseHooks()
{
    for (const auto call : ResponseCalls) {
        char expected[5]{static_cast<char>(0xE8)};
        const auto displacement = static_cast<LONG>(ProcessResponse - (call + 5));
        std::memcpy(expected + 1, &displacement, sizeof(displacement));
        if (!game_image::MatchesCode(call, std::string_view(expected, sizeof(expected))))
            return false;
    }
    size_t installed = 0;
    for (const auto call : ResponseCalls) {
        const auto end = call + 5;
        if (!PatchWord(call + 1, ProcessResponse - end,
                       reinterpret_cast<uintptr_t>(&ProcessLeaderboardResponse) - end)) {
            for (size_t i = 0; i < installed; ++i) {
                const auto previous = ResponseCalls[i];
                PatchWord(previous + 1,
                    reinterpret_cast<uintptr_t>(&ProcessLeaderboardResponse) - (previous + 5),
                    ProcessResponse - (previous + 5));
            }
            return false;
        }
        ++installed;
    }
    return true;
}
}

bool Install(const std::filesystem::path& archive, const std::filesystem::path& queue)
{
    archivePath = archive;
    queuePath = queue;
    using game_image::MatchesCode;
    if (!MatchesCode(MenuSetupCall - 4, "\x8B\x46\x10\x50\xE8\x88\xCC\x02\x00\x8A\x8E\x29\x01\x00\x00"sv)) {
        startup::Log("Main-menu setup differs from the supported layout.");
        return false;
    }
    if (!MatchesCode(0x5157E0, "\x83\xEC\x10\x56\x8B\x74\x24\x18\x85\xF6"sv)
        || !MatchesCode(SwitchScreen, "\x83\x3D\x90\x5E\x92\x00\x06\x56\x57"sv)
        || !MatchesCode(0x573960, "\x8B\x41\x2C\x56\x8B\x74\x24\x08\x83\xC1\x2C"sv)
        || !MatchesCode(0x586290, "\x8B\x44\x24\x04\x53\x56\x8B\xF1\x33\xDB"sv)
        || !game_image::Readable(MainRefreshSlot, sizeof(std::uintptr_t))
        || !game_image::Readable(RankingsFactorySlot, sizeof(std::uintptr_t))
        || !game_image::Readable(0x91CADC, sizeof(void*))
        || !game_image::Readable(0x91D21C, sizeof(void*))) {
        startup::Log("Leaderboard screen code or data layout differs from the supported layout.");
        return false;
    }
    optionMethods[0] = 0x51A500;
    optionMethods[1] = reinterpret_cast<uintptr_t>(&React);
    screenMethods[0] = reinterpret_cast<uintptr_t>(&DestroyRankings);
    screenMethods[1] = reinterpret_cast<uintptr_t>(&OnRankings);
    screenMethods[2] = 0x50DE70;
    const auto factory = reinterpret_cast<uintptr_t>(&CreateRankings);
    const auto refresh = reinterpret_cast<uintptr_t>(&OnRefresh);

    if (!InstallResponseHooks()) {
        startup::Log("Could not install the leaderboard Back response hooks.");
        return false;
    }
    if (!PatchWord(RankingsFactorySlot, RankingsFactory, factory)) {
        RestoreResponseHooks();
        startup::Log("Could not install the Rap Sheet rankings hook.");
        return false;
    }
    if (!PatchWord(MainRefreshSlot, RefreshHeader, refresh)) {
        PatchWord(RankingsFactorySlot, factory, RankingsFactory);
        RestoreResponseHooks();
        startup::Log("Could not install the main-menu title hook.");
        return false;
    }
    const auto callEnd = MenuSetupCall + 5;
    if (!PatchWord(MenuSetupCall + 1, LastButton - callEnd,
                   reinterpret_cast<uintptr_t>(&SetupBridge) - callEnd)) {
        PatchWord(MainRefreshSlot, refresh, RefreshHeader);
        PatchWord(RankingsFactorySlot, factory, RankingsFactory);
        RestoreResponseHooks();
        startup::Log("Could not install the main-menu option hook.");
        return false;
    }
    return true;
}
}

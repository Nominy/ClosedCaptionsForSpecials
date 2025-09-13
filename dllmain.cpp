/*****************************************************************
 *  Payday 3  –  "Sound‑Subtitles" Mod (PolyHook² + UE4SS)
 *****************************************************************/

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "MyAwesomeMod.h"
#include <polyhook2/Detour/x64Detour.hpp>
#include <polyhook2/Exceptions/AVehHook.hpp>
#include <polyhook2/Enums.hpp>
#include <LuaMadeSimple/LuaMadeSimple.hpp>
#include <DynamicOutput/DynamicOutput.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <LuaType/LuaUObject.hpp>
#include <lauxlib.h>
#include <atomic>

using namespace RC;
using namespace RC::Unreal;

// Global variable definitions
PostEventID_t_int g_PostEventTrampoline_intid = nullptr;
PostEventID_t_str g_PostEventTrampoline_strid = nullptr;
std::unordered_set<uint64_t> g_EventFilter;
std::shared_mutex g_FilterMx;
void* g_ModInstance = nullptr;
DWORD g_MainThreadId = 0;
std::queue<SoundEventData> g_PendingSoundEvents;
std::mutex g_EventQueueMutex;

// Helper function implementations
uint64_t hash_wstr(std::wstring_view s) noexcept {
    uint64_t h = 14695981039346656037ULL;
    for (wchar_t c : s) h = (h ^ static_cast<uint64_t>(c)) * 1099511628211ULL;
    return h;
}

bool WantedEvent(uint64_t idOrHash) noexcept {
    std::shared_lock lk(g_FilterMx);
    return g_EventFilter.empty() || g_EventFilter.contains(idOrHash);
}

bool IsInGameThread()
{
    return GetCurrentThreadId() == g_MainThreadId;
}

std::wstring mbs_to_ws(const char* s, size_t len) {
    if (!s || len == 0) return std::wstring();
    int size_needed = MultiByteToWideChar(CP_UTF8, 0, s, static_cast<int>(len), NULL, 0);
    std::wstring wstrTo(size_needed, 0);
    MultiByteToWideChar(CP_UTF8, 0, s, static_cast<int>(len), &wstrTo[0], size_needed);
    return wstrTo;
}

// Lua C Functions for sound event filter
int l_add_sound_event(lua_State* L) {
    std::unique_lock lk(g_FilterMx);
    if (lua_isinteger(L, 1)) {
        g_EventFilter.insert(static_cast<uint64_t>(lua_tointeger(L, 1)));
    }
    else if (lua_isstring(L, 1)) {
        size_t len;
        const char* s = lua_tolstring(L, 1, &len);
        g_EventFilter.insert(hash_wstr(mbs_to_ws(s, len)));
    }
    return 0;
}

int l_remove_sound_event(lua_State* L) {
    std::unique_lock lk(g_FilterMx);
    if (lua_isinteger(L, 1)) {
        g_EventFilter.erase(static_cast<uint64_t>(lua_tointeger(L, 1)));
    }
    else if (lua_isstring(L, 1)) {
        size_t len;
        const char* s = lua_tolstring(L, 1, &len);
        g_EventFilter.erase(hash_wstr(mbs_to_ws(s, len)));
    }
    return 0;
}

int l_clear_sound_events(lua_State* L) {
    std::unique_lock lk(g_FilterMx);
    g_EventFilter.clear();
    return 0;
}

// Safety Helpers
bool IsProbablyUObjectPtr(uint64_t addr) {
    if (addr < 0x10000) return false;
    if (addr & 0x7) return false;
    if (addr > 0x7FFFFFFF0000ULL) return false;
    return true;
}

bool TryValidateUObject(UObject* obj) {
    if (UObject::IsReal(obj)) {
        __try {
            return obj;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }
    return false;
}

void PushGameObjectOrNil(LuaMadeSimple::Lua& L, AkGameObjectID id)
{
    if (IsProbablyUObjectPtr(id) && IsInGameThread())
    {
        UObject* obj = reinterpret_cast<UObject*>(id);
        if (TryValidateUObject(obj)) {
            __try {
                if (obj->IsUnreachable()) {
                    Output::send(TEXT("[MyAwesomeMod] Invalid UObject pointer detected\n"));
                    L.set_nil();
                    return;
                }
                LuaType::auto_construct_object(L, obj);
                return;
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {
                Output::send(TEXT("[MyAwesomeMod] Exception while pushing UObject to Lua\n"));
                return;
            }
        }
    }
    L.set_nil();
}

// Per-event context stored by playingID
struct CallbackContext
{
    uint64_t magic;
    AkCallbackFunc originalCallback;
    void* originalCookie;
    AkUInt32 originalFlags;
    std::wstring eventName;
    uint64_t eventIdentifier;
};

static constexpr uint64_t kCookieMagic = 0xD15EA5EDCAFEBABEULL;

// Robust lifetime management: track active callbacks and store contexts by playingID.
static std::atomic<long> g_ActiveCallbackCount{0};
static std::atomic<bool> g_IsShuttingDown{false};
static std::mutex g_ContextMapMutex;
static std::unordered_map<AkPlayingID, std::unique_ptr<CallbackContext>> g_PlayingIdToContext;
static std::mutex g_PendingCtxFreesMutex;
static std::queue<std::unique_ptr<CallbackContext>> g_PendingCtxFrees;

// Helper function for safe original callback invocation
bool TryCallOriginalCallback(AkCallbackFunc callback, AkCallbackType in_eType, AkCallbackInfo* in_pCallbackInfo, void* originalCookie) {
    __try {
        void* savedCookie = in_pCallbackInfo->pCookie;
        in_pCallbackInfo->pCookie = originalCookie;
        callback(in_eType, in_pCallbackInfo);
        in_pCallbackInfo->pCookie = savedCookie;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Helper to compute flags and set our callback while preserving original cookie.
static AkUInt32 BuildFlagsAndCookie(
    AkUInt32 originalFlags,
    AkCallbackFunc /*originalCallback*/,
    void* originalCookie,
    const std::wstring& /*eventName*/,
    uint64_t /*eventIdentifier*/,
    AkCallbackFunc& outCallback,
    void*& outCookie)
{
    outCallback = CallbackWrapper;
    // Preserve engine's cookie semantics for the original callback
    outCookie = originalCookie;

    // Ensure we receive both Duration and EndOfEvent for our capture logic
    AkUInt32 newFlags = originalFlags | AK_Duration | AK_EndOfEvent;
    return newFlags;
}

// Register per-playingID context after PostEvent returns a valid ID
static void RegisterCallbackContext(
    AkPlayingID playingID,
    AkCallbackFunc originalCallback,
    void* originalCookie,
    AkUInt32 originalFlags,
    const std::wstring& eventName,
    uint64_t eventIdentifier)
{
    if (playingID == AK_INVALID_PLAYING_ID) {
        return;
    }
    auto ctx = std::make_unique<CallbackContext>(CallbackContext{
        kCookieMagic,
        originalCallback,
        originalCookie,
        originalFlags,
        eventName,
        eventIdentifier});

    {
        std::lock_guard<std::mutex> lk(g_ContextMapMutex);
        g_PlayingIdToContext[playingID] = std::move(ctx);
    }
}

// Schedule context deletion on the game thread
static void ScheduleContextDeletion(AkPlayingID playingID)
{
    std::unique_ptr<CallbackContext> toFree;
    {
        std::lock_guard<std::mutex> lk(g_ContextMapMutex);
        auto it = g_PlayingIdToContext.find(playingID);
        if (it != g_PlayingIdToContext.end()) {
            toFree = std::move(it->second);
            g_PlayingIdToContext.erase(it);
        }
    }
    if (toFree) {
        std::lock_guard<std::mutex> qlk(g_PendingCtxFreesMutex);
        g_PendingCtxFrees.push(std::move(toFree));
    }
}

AkPlayingID __cdecl Hook_PostEvent_intid(
    AkUniqueID eventID,
    AkGameObjectID gameObjID,
    AkUInt32 flags,
    AkCallbackFunc cb,
    void* cookie,
    AkUInt32 extCount,
    AkExternalSourceInfo* externals,
    AkPlayingID playingID
)
{
    if (!WantedEvent(eventID))
    {
        return g_PostEventTrampoline_intid(eventID, gameObjID, flags, cb, cookie, extCount, externals, playingID);
    }

    std::wstring eventNameW = L"EventID_" + std::to_wstring(eventID);
    AkCallbackFunc modifiedCb = cb;
    void* modifiedCookie = cookie;
    AkUInt32 modifiedFlags = BuildFlagsAndCookie(
        flags, cb, cookie, eventNameW, static_cast<uint64_t>(eventID), modifiedCb, modifiedCookie);

    AkPlayingID resultPlayingID = g_PostEventTrampoline_intid(
        eventID, gameObjID, modifiedFlags, modifiedCb,
        modifiedCookie, extCount, externals, playingID);

    // Register context mapping only if the event actually started
    if (resultPlayingID != AK_INVALID_PLAYING_ID) {
        RegisterCallbackContext(resultPlayingID, cb, cookie, flags, eventNameW, static_cast<uint64_t>(eventID));
    }

    return resultPlayingID;
}

AkPlayingID __cdecl Hook_PostEvent_stringid(
    const wchar_t* eventName,
    AkGameObjectID gameObjID,
    AkUInt32 flags,
    AkCallbackFunc cb,
    void* cookie,
    AkUInt32 extCount,
    AkExternalSourceInfo* externals,
    AkPlayingID playingID
)
{
    uint64_t hv = hash_wstr(eventName ? std::wstring_view(eventName) : L"");
    if (!WantedEvent(hv))
    {
        return g_PostEventTrampoline_strid(eventName, gameObjID, flags, cb, cookie, extCount, externals, playingID);
    }

    AkCallbackFunc modifiedCb = cb;
    void* modifiedCookie = cookie;
    AkUInt32 modifiedFlags = BuildFlagsAndCookie(
        flags, cb, cookie, eventName ? std::wstring(eventName) : std::wstring(L"Unknown"), hv,
        modifiedCb, modifiedCookie);

    AkPlayingID resultPlayingID = g_PostEventTrampoline_strid(
        eventName, gameObjID, modifiedFlags, modifiedCb,
        modifiedCookie, extCount, externals, playingID);

    // Register context mapping only if the event actually started
    if (resultPlayingID != AK_INVALID_PLAYING_ID) {
        RegisterCallbackContext(resultPlayingID, cb, cookie,
            flags,
            eventName ? std::wstring(eventName) : std::wstring(L"Unknown"),
            hv);
    }

    return resultPlayingID;
}

// Class implementation
PayDay3_SoundSubMod::PayDay3_SoundSubMod()
{
    ModName = TEXT("MyAwesomeMod");
    ModVersion = TEXT("1.0");
    ModDescription = TEXT("Shows subtitles for Wwise cues");
    ModAuthors = TEXT("NaftSan");

    g_ModInstance = this;
    g_MainThreadId = 0;  // Will be set in first on_update() call

    std::unique_lock lk(g_FilterMx);
    g_EventFilter.clear();
}

void PayDay3_SoundSubMod::ProcessQueuedSoundEvents()
{
    if (!IsInGameThread() || !m_main_lua) {
        return;
    }

    std::queue<SoundEventData> eventsToProcess;
    {
        std::lock_guard<std::mutex> lock(g_EventQueueMutex);
        eventsToProcess.swap(g_PendingSoundEvents);
    }

    while (!eventsToProcess.empty()) {
        const SoundEventData& eventData = eventsToProcess.front();
        ExecuteSoundHook(eventData.eventName, eventData.mediaID, eventData.playingID, eventData.gameObjectID);
        eventsToProcess.pop();
    }

    // Drain any pending per-event context frees on the game thread
    {
        std::queue<std::unique_ptr<CallbackContext>> toFree;
        {
            std::lock_guard<std::mutex> lk(g_PendingCtxFreesMutex);
            std::swap(toFree, g_PendingCtxFrees);
        }
        // unique_ptrs free on scope exit
    }
}

void PayDay3_SoundSubMod::ExecuteSoundHook(const std::wstring& eventName, AkUniqueID mediaID, AkPlayingID playingID, AkGameObjectID gameObjectID)
{
    if (!m_main_lua || !IsInGameThread()) {
        return;
    }

    try {
        if (!m_main_lua->is_global_function("OnSoundCaptured")) {
            return;
        }

        m_main_lua->prepare_function_call("OnSoundCaptured");

        std::string eventNameStr(eventName.begin(), eventName.end());

        m_main_lua->set_string(eventNameStr);
        m_main_lua->set_integer(static_cast<int64_t>(mediaID));
        m_main_lua->set_integer(static_cast<int64_t>(playingID));

        PushGameObjectOrNil(*m_main_lua, gameObjectID);

        m_main_lua->call_function(4, 0);
    }
    catch (std::runtime_error e) {
        Output::send(STR("[MyAwesomeMod] Lua hook execution failed with exception: {}\n"),
            std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>>().from_bytes(e.what()));
    }
}

void PayDay3_SoundSubMod::QueueSoundEvent(
    const std::wstring& eventName, 
    AkUniqueID mediaID, 
    AkPlayingID playingID, 
    AkGameObjectID gameObjectID
)
{
    std::lock_guard<std::mutex> lock(g_EventQueueMutex);
    g_PendingSoundEvents.push({ eventName, mediaID, playingID, gameObjectID });
}

void PayDay3_SoundSubMod::on_unreal_init()
{
    HMODULE hExe = GetModuleHandleW(nullptr);
    if (!hExe)
    {
        Output::send(TEXT("[MyAwesomeMod] ERROR: no module handle\n"));
        return;
    }

    FARPROC pAddr_int = GetProcAddress(hExe, MAKEINTRESOURCEA(171));
    if (!pAddr_int)
    {
        Output::send(TEXT("[MyAwesomeMod] ERROR: PostEvent_int not found\n"));
        return;
    }

    FARPROC pAddr_str = GetProcAddress(hExe, MAKEINTRESOURCEA(173));
    if (!pAddr_str)
    {
        Output::send(TEXT("[MyAwesomeMod] ERROR: PostEvent_str not found\n"));
        return;
    }

    Detour_int = std::make_unique<PLH::x64Detour>(
        reinterpret_cast<uint64_t>(pAddr_int),
        reinterpret_cast<uint64_t>(&Hook_PostEvent_intid),
        reinterpret_cast<uint64_t*>(&g_PostEventTrampoline_intid));

    Detour_str = std::make_unique<PLH::x64Detour>(
        reinterpret_cast<uint64_t>(pAddr_str),
        reinterpret_cast<uint64_t>(&Hook_PostEvent_stringid),
        reinterpret_cast<uint64_t*>(&g_PostEventTrampoline_strid));

    if (!Detour_int->hook() || !Detour_int->isHooked())
    {
        Output::send(TEXT("[MyAwesomeMod] ERROR: detour int failed\n"));
        return;
    }

    if (!Detour_str->hook() || !Detour_str->isHooked())
    {
        Output::send(TEXT("[MyAwesomeMod] ERROR: detour str failed\n"));
        return;
    }
    Output::send(TEXT("[MyAwesomeMod] PostEvent detoured successfully\n"));
}

void PayDay3_SoundSubMod::on_update()
{
    if (!g_MainThreadId)                // not set yet
        g_MainThreadId = GetCurrentThreadId();

    ProcessQueuedSoundEvents();
}

void PayDay3_SoundSubMod::on_lua_start(LuaMadeSimple::Lua& lua,
    LuaMadeSimple::Lua& main_lua,
    LuaMadeSimple::Lua& async_lua,
    std::vector<LuaMadeSimple::Lua*>& hook_luas)
{
    m_main_lua = &main_lua;

    lua_State* L = m_main_lua->get_lua_state();
    lua_register(L, "AddSoundEvent", l_add_sound_event);
    lua_register(L, "RemoveSoundEvent", l_remove_sound_event);
    lua_register(L, "ClearSoundEvents", l_clear_sound_events);
}

PayDay3_SoundSubMod::~PayDay3_SoundSubMod()
{
    g_ModInstance = nullptr;

    g_IsShuttingDown = true;

    if (Detour_int && Detour_int->isHooked())
        Detour_int->unHook();

    if (Detour_str && Detour_str->isHooked())
        Detour_str->unHook();

    // Wait for in-flight callbacks to quiesce (bounded wait)
    const DWORD kMaxWaitMs = 3000;
    DWORD waited = 0;
    while (g_ActiveCallbackCount.load() > 0 && waited < kMaxWaitMs) {
        Sleep(1);
        waited += 1;
    }

    // Clear any remaining contexts
    {
        std::lock_guard<std::mutex> lk(g_ContextMapMutex);
        if (!g_PlayingIdToContext.empty()) {
            std::lock_guard<std::mutex> qlk(g_PendingCtxFreesMutex);
            for (auto& [pid, ctx] : g_PlayingIdToContext) {
                g_PendingCtxFrees.push(std::move(ctx));
            }
            g_PlayingIdToContext.clear();
        }
    }
    // Drain frees synchronously
    {
        std::queue<std::unique_ptr<CallbackContext>> toFree;
        {
            std::lock_guard<std::mutex> lk(g_PendingCtxFreesMutex);
            std::swap(toFree, g_PendingCtxFrees);
        }
    }
}

void __cdecl CallbackWrapper(AkCallbackType in_eType, AkCallbackInfo* in_pCallbackInfo)
{
    if (!in_pCallbackInfo) {
        return;
    }

    g_ActiveCallbackCount.fetch_add(1);

    const AkUInt32 eType = static_cast<AkUInt32>(in_eType);
    AkPlayingID playingID = 0;
    // Most event callback types derive from AkEventCallbackInfo and carry playingID
    AkEventCallbackInfo* ev = static_cast<AkEventCallbackInfo*>(in_pCallbackInfo);
    playingID = ev ? ev->playingID : 0;

    // Snapshot needed context data
    AkCallbackFunc originalCallback = nullptr;
    void* originalCookie = nullptr;
    AkUInt32 originalFlags = 0;
    std::wstring eventName;
    uint64_t eventIdentifier = 0;
    bool haveContext = false;

    if (playingID != 0) {
        std::lock_guard<std::mutex> lk(g_ContextMapMutex);
        auto it = g_PlayingIdToContext.find(playingID);
        if (it != g_PlayingIdToContext.end() && it->second && it->second->magic == kCookieMagic) {
            originalCallback = it->second->originalCallback;
            originalCookie = it->second->originalCookie;
            originalFlags = it->second->originalFlags;
            eventName = it->second->eventName;
            eventIdentifier = it->second->eventIdentifier;
            haveContext = true;
        }
    }

    // Forward to original callback first (only if it asked for this type)
    if (haveContext && originalCallback && (eType & originalFlags)) {
        if (!TryCallOriginalCallback(originalCallback, in_eType, in_pCallbackInfo, originalCookie)) {
            Output::send(TEXT("[MyAwesomeMod] Original callback crashed, continuing...\n"));
        }
    }

    // Our own processing (skip during shutdown)
    if (!g_IsShuttingDown.load()) {
        if ((eType & AK_Duration) != 0 && haveContext) {
            AkDurationCallbackInfo* info = static_cast<AkDurationCallbackInfo*>(in_pCallbackInfo);
            if (info && info->mediaID > 0 && g_ModInstance && WantedEvent(eventIdentifier)) {
                static_cast<PayDay3_SoundSubMod*>(g_ModInstance)->QueueSoundEvent(
                    eventName, info->mediaID, info->playingID, in_pCallbackInfo->gameObjID);
            }
        }
    }

    // Cleanup at EndOfEvent: schedule deletion on game thread
    if ((eType & AK_EndOfEvent) != 0 && playingID != 0) {
        ScheduleContextDeletion(playingID);
    }

    g_ActiveCallbackCount.fetch_sub(1);
}

// DLL exports for UE4SS loader
#define MOD_API __declspec(dllexport)
extern "C" {
    MOD_API RC::CppUserModBase* start_mod() { return new PayDay3_SoundSubMod(); }
    MOD_API void uninstall_mod(RC::CppUserModBase* mod) { delete mod; }
}
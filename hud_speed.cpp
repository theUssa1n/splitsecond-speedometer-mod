#include "hud_speed.h"

#include <windows.h>

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

namespace {

// ---------------------------------------------------------
// PER-BUILD ADDRESSES
//
// The element is located by scanning writable memory for its class vtable (the
// object's first dword) and then validating the method name stored next to it,
// so these four addresses are the only build-specific pieces in the whole mod.
//
// The steam exe is the same game with a different layout. Its .rdata sits 0x58
// higher than retail's, but its code and globals moved independently, so nothing
// can be derived from the retail table - each number here was found separately.
// ---------------------------------------------------------
struct BuildAddresses {
    uintptr_t nameStringVa;     // the build's own copy of "SetTargetPlayerName"
    uint32_t  elementVtable;    // Hud::cTargetPlayerElement::vftable
    uintptr_t inGameUiPtr;      // global: pointer to the in-game UI object
    uintptr_t vehicleArrayPtr;  // global: pointer to the array of cars
};

constexpr BuildAddresses kRetail = { 0x00BD1520, 0x00BD1568, 0x00D66AD0, 0x00D6A04C };
constexpr BuildAddresses kSteam  = { 0x00BD1578, 0x00BD15C0, 0x00D5A170, 0x00D5D6EC };

const BuildAddresses* g_build = nullptr;

// Both builds load here; an exe that does not is not one of them.
constexpr uintptr_t kExpectedModuleBase = 0x00400000;

// The element's method descriptors. Every element keeps one inline descriptor
// per method, and the layout is the same for all of them:
//   element + 0x24 -> "TargetPlayerNameShow"  (bring it on screen)
//   element + 0x70 -> the ActionScript object the calls go to
//   element + 0x74 -> "SetTargetPlayerName"   (the text we feed)
constexpr uint32_t kShowNameOffset = 0x00000024;
constexpr uint32_t kContextOffset  = 0x00000070;
constexpr uint32_t kSetNameOffset  = 0x00000074;

const char* const kElementName = "SetTargetPlayerName";

// A splitscreen race runs one race HUD per viewport, so the same element can
// exist more than once at a time.
constexpr int kMaxInstances = 4;

// The car objects live in one array, one car every 0xFA8 bytes, with the speed
// at +0xF4C (mph). Reading each player's own car is the only way to be right in
// a splitscreen race - the published speed buffer only ever holds one of them.
constexpr uint32_t kVehicleStride = 0x00000FA8;
constexpr uint32_t kVehicleSpeed  = 0x00000F4C;

// ---------------------------------------------------------
// CALLING INTO ACTIONSCRIPT
//
// The engine's own entry point (retail 0x00536A70) is a tiny thunk:
//
//     cmp dword ptr [ecx+4], 0
//     jz  ret0
//     mov ecx, [ecx+4]
//     mov eax, [ecx]
//     mov eax, [eax+0x48]
//     jmp eax
//
// it takes the object out of the wrapper at +4 and tail-jumps into vtable entry
// 0x48/4. The steam build's thunk is byte-for-byte the same, so instead of
// hardcoding either address the mod does exactly the same thing itself.
// ---------------------------------------------------------
constexpr uint32_t kContextInnerOffset = 0x4;
constexpr uint32_t kInvokeVtableEntry  = 0x48 / 4;

typedef void* (__fastcall* tInvokeActionScript)(void* self, void* edx, const char* name,
                                                void* result, void* args, int argc);

// ---------------------------------------------------------
// GFxValue
//
// Exactly 16 bytes as the game builds it: {u32 type, u32 pad, double}.
// type 3 = number, type 4 = string (the char* sits in the low half of the same
// 8-byte slot). The size matters: the engine walks an argument array with a
// 16-byte stride, so a larger struct would make every argument after the first
// one read from the wrong offset.
// ---------------------------------------------------------
struct GfxValue {
    uint32_t type;
    uint32_t pad;
    union {
        double num;
        const char* str;
    } v;
};
static_assert(sizeof(GfxValue) == 16, "GFxValue must match the engine layout");

// ---------------------------------------------------------
// STATE
// ---------------------------------------------------------
// One element per viewport: a single-player race has one, a splitscreen race has
// one per player. The scanner fills the slots, the render thread reads them; a
// count of zero means "nothing found yet".
uintptr_t     g_elements[kMaxInstances] = {0};
volatile LONG g_elementCount = 0;

// false = miles, true = kilometres. Persisted in speedo.ini.
bool g_metric = false;

HANDLE g_scanThread = nullptr;

// ---------------------------------------------------------
// FILE HELPERS
// ---------------------------------------------------------
std::string ModuleDir() {
    char path[MAX_PATH];
    HMODULE self = nullptr;

    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(&ModuleDir), &self) &&
        GetModuleFileNameA(self, path, MAX_PATH)) {
        std::string p(path);
        return p.substr(0, p.find_last_of("\\/") + 1);
    }
    return std::string();
}

#ifdef SPEEDO_DEBUG
// Bring-up builds only: a plain text trail next to the module. The shipped
// build is compiled without SPEEDO_DEBUG and writes nothing at all.
void DebugLog(const char* fmt, ...) {
    char text[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);

    std::ofstream file(ModuleDir() + "speedo.log", std::ios::app);
    if (file.is_open()) file << text << "\n";
}
#else
#define DebugLog(...) ((void)0)
#endif

// ---------------------------------------------------------
// MEMORY HELPERS
// ---------------------------------------------------------
bool IsReadable(const void* p, size_t size) {
    if (!p) return false;
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(p, &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    if (!(mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ |
                         PAGE_EXECUTE_WRITECOPY | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE)))
        return false;
    return reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize >=
           reinterpret_cast<uintptr_t>(p) + size;
}

bool IsWritableRegion(const MEMORY_BASIC_INFORMATION& mbi) {
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    return (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY)) != 0;
}

// Bounded, zero-terminated compare. Comparing a fixed byte count would read past
// the terminator, and the game packs its method names back to back, so a
// fixed-length compare can never match a short name.
bool StringAt(uintptr_t address, const char* expected) {
    const std::size_t length = std::strlen(expected);
    if (!IsReadable(reinterpret_cast<const void*>(address), length + 1)) return false;

    const char* text = reinterpret_cast<const char*>(address);
    return std::memcmp(text, expected, length) == 0 && text[length] == 0;
}

// True while the cached element is still ours. The engine reuses heap blocks
// between modes, so an address that was right in one mode can point at a
// different element - or at freed memory - in the next one.
bool ElementMatches(uintptr_t element) {
    const std::size_t length = std::strlen(kElementName);

    __try {
        const uintptr_t namePtr = *reinterpret_cast<volatile uint32_t*>(element + kSetNameOffset);
        if (namePtr < 0x10000) return false;
        if (!IsReadable(reinterpret_cast<const void*>(namePtr), length + 1)) return false;
        if (std::memcmp(reinterpret_cast<const void*>(namePtr), kElementName, length) != 0) return false;
        return *reinterpret_cast<const volatile char*>(namePtr + length) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Current speed of one local player, read straight from that player's own car.
bool PlayerSpeed(int player, float* speed) {
    const uintptr_t arrayPtr = GameBuild::VehicleArrayPtr();
    if (!arrayPtr) return false;
    if (!IsReadable(reinterpret_cast<const void*>(arrayPtr), 4)) return false;

    const uintptr_t vehicles = *reinterpret_cast<volatile uint32_t*>(arrayPtr);
    if (vehicles < 0x10000) return false;

    const uintptr_t at = vehicles + static_cast<uintptr_t>(player) * kVehicleStride + kVehicleSpeed;
    if (!IsReadable(reinterpret_cast<const void*>(at), 4)) return false;

    *speed = *reinterpret_cast<volatile float*>(at);
    return true;
}

void* InvokeActionScript(void* context, const char* name, void* result, void* args, int argc) {
    const uintptr_t wrapper = reinterpret_cast<uintptr_t>(context);
    if (wrapper < 0x10000) return nullptr;
    if (!IsReadable(reinterpret_cast<const void*>(wrapper + kContextInnerOffset), 4)) return nullptr;

    const uintptr_t inner = *reinterpret_cast<volatile uint32_t*>(wrapper + kContextInnerOffset);
    if (inner < 0x10000) return nullptr;
    if (!IsReadable(reinterpret_cast<const void*>(inner), 4)) return nullptr;

    const uintptr_t vtable = *reinterpret_cast<volatile uint32_t*>(inner);
    if (vtable < 0x10000) return nullptr;

    const uintptr_t slot = vtable + kInvokeVtableEntry * 4;
    if (!IsReadable(reinterpret_cast<const void*>(slot), 4)) return nullptr;

    const uintptr_t fn = *reinterpret_cast<volatile uint32_t*>(slot);
    if (fn < 0x10000) return nullptr;

    return reinterpret_cast<tInvokeActionScript>(fn)(
        reinterpret_cast<void*>(inner), nullptr, name, result, args, argc);
}

// Walks [from, to). Returns true when the range was walked to the end; on a
// fault it returns false and sets *resumeAt past the page that faulted so the
// caller can carry on.
bool ScanRange(uintptr_t from, uintptr_t to, uintptr_t* found, int* count, int max,
               uintptr_t* resumeAt) {
    const uint32_t vtable = GameBuild::ElementVtable();
    uintptr_t a = from;

    __try {
        for (; a + 4 <= to && *count < max; a += 4) {
            if (*reinterpret_cast<volatile uint32_t*>(a) != vtable) continue;
            if (ElementMatches(a)) found[(*count)++] = a;
        }
        *resumeAt = to;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *resumeAt = (a & ~static_cast<uintptr_t>(0xFFF)) + 0x1000;
        if (*resumeAt <= from) *resumeAt = from + 0x1000;
        return false;
    }
}

// Full scan - only ever called from the scanner thread. Every instance is
// collected, because a splitscreen race builds one element per viewport.
int ScanAll(uintptr_t* found, int max) {
    int count = 0;
    MEMORY_BASIC_INFORMATION mbi;
    uintptr_t p = 0x10000;

    while (p < 0x7FFF0000 && count < max) {
        if (VirtualQuery(reinterpret_cast<LPCVOID>(p), &mbi, sizeof(mbi)) != sizeof(mbi)) break;

        const uintptr_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        const uintptr_t end = base + static_cast<uintptr_t>(mbi.RegionSize);

        if (IsWritableRegion(mbi) && end > base + 4) {
            uintptr_t from = (p > base) ? p : base;
            while (from + 4 <= end) {
                uintptr_t resume = from;
                if (ScanRange(from, end, found, &count, max, &resume)) break;  // region walked
                from = resume;
            }
        }
        p = end;
    }
    return count;
}

DWORD WINAPI ScanThreadProc(LPVOID) {
    for (;;) {
        if (g_elementCount == 0) {
            uintptr_t instances[kMaxInstances];
            const int count = ScanAll(instances, kMaxInstances);

            if (count > 0) {
                for (int i = 0; i < count; ++i) {
                    g_elements[i] = instances[i];
                    DebugLog("[speedo] element %d @ 0x%08X", i, static_cast<unsigned>(instances[i]));
                }
                InterlockedExchange(&g_elementCount, count);  // publishes them
            } else {
                DebugLog("[speedo] no element in memory yet");
                Sleep(2000);  // a full walk costs about a second
            }
        }
        Sleep(250);
    }
}

// ---------------------------------------------------------
// CONFIG (speedo.ini)
// ---------------------------------------------------------
std::string ConfigPath() {
    return ModuleDir() + "speedo.ini";
}

void SaveConfig() {
    std::ofstream file(ConfigPath(), std::ios::trunc);
    if (file.is_open())
        file << "; 0 = miles (MPH), 1 = kilometres (KM/H)\n"
             << "speedunit=" << (g_metric ? 1 : 0) << "\n";
}

void LoadConfig() {
    std::ifstream file(ConfigPath());
    if (!file.is_open()) {
        SaveConfig();  // first run: write the defaults so the file exists
        return;
    }

    std::string line;
    while (std::getline(file, line)) {
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;

        std::string key = line.substr(0, eq);
        while (!key.empty() && (key.back() == ' ' || key.back() == '\t' || key.back() == '\r'))
            key.pop_back();

        if (key == "speedunit")
            g_metric = (std::atoi(line.c_str() + eq + 1) != 0);
    }
}

}  // namespace

// ---------------------------------------------------------
// GAME BUILD
// ---------------------------------------------------------
GameBuild::Id GameBuild::Detect() {
    static Id cached = Id::Unknown;
    static bool resolved = false;

    if (!resolved) {
        resolved = true;

        if (reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr)) == kExpectedModuleBase) {
            if (StringAt(kRetail.nameStringVa, kElementName)) {
                g_build = &kRetail;
                cached = Id::Retail;
            } else if (StringAt(kSteam.nameStringVa, kElementName)) {
                g_build = &kSteam;
                cached = Id::Steam;
            }
        }

        DebugLog("[speedo] build: %s", cached == Id::Retail ? "retail" :
                                       (cached == Id::Steam ? "steam" : "unknown"));
    }

    return cached;
}

uintptr_t GameBuild::InGameUiPtr() {
    return g_build ? g_build->inGameUiPtr : 0;
}

uint32_t GameBuild::ElementVtable() {
    return g_build ? g_build->elementVtable : 0;
}

uintptr_t GameBuild::VehicleArrayPtr() {
    return g_build ? g_build->vehicleArrayPtr : 0;
}

// ---------------------------------------------------------
// PUBLIC API
// ---------------------------------------------------------
void HudSpeed::Init() {
    LoadConfig();
    InterlockedExchange(&g_elementCount, 0);

    // Start looking for the element right away, off the render thread: by the
    // time a race is running the address is usually already known.
    if (!g_scanThread)
        g_scanThread = CreateThread(nullptr, 0, ScanThreadProc, nullptr, 0, nullptr);
}

void HudSpeed::ToggleUnit() {
    g_metric = !g_metric;
    SaveConfig();
}

void HudSpeed::Update(bool inRace) {
    if (!inRace) return;

    const int count = g_elementCount;
    if (count == 0) return;

    // Every instance has to still be ours; otherwise the whole set is dropped
    // and the scanner looks for fresh ones.
    for (int i = 0; i < count; ++i) {
        if (ElementMatches(g_elements[i])) continue;

        DebugLog("[speedo] element went stale - rescanning");
        InterlockedExchange(&g_elementCount, 0);
        return;
    }

    GfxValue result;
    std::memset(&result, 0, sizeof(result));

    for (int i = 0; i < count; ++i) {
        const uintptr_t element = g_elements[i];
        void* context = reinterpret_cast<void*>(
            *reinterpret_cast<volatile uint32_t*>(element + kContextOffset));
        if (reinterpret_cast<uintptr_t>(context) < 0x10000) continue;

        float speedMph = 0.0f;
        if (!PlayerSpeed(i, &speedMph)) continue;

        float value = speedMph;
        if (g_metric) value *= 1.60934f;
        if (value < 0.0f) value = -value;

        // This element takes text rather than a number, so the value travels as
        // a string. The unit itself is not shown - press 'M' to switch the
        // number between miles and kilometres.
        char text[16];
        std::snprintf(text, sizeof(text), "%d", static_cast<int>(value));

        GfxValue args[1];
        std::memset(args, 0, sizeof(args));
        args[0].type = 4;  // string
        args[0].v.str = text;

        // Both calls go out every frame: the text has to keep up with the car,
        // and an element the game keeps hidden only stays on screen while
        // something keeps asking for it.
        const uintptr_t setName = *reinterpret_cast<volatile uint32_t*>(element + kSetNameOffset);
        if (setName >= 0x10000 && IsReadable(reinterpret_cast<const void*>(setName), 4))
            InvokeActionScript(context, reinterpret_cast<const char*>(setName), &result, args, 1);

        const uintptr_t showName = *reinterpret_cast<volatile uint32_t*>(element + kShowNameOffset);
        if (showName >= 0x10000 && IsReadable(reinterpret_cast<const void*>(showName), 4))
            InvokeActionScript(context, reinterpret_cast<const char*>(showName), &result, args, 1);
    }
}

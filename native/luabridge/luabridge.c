/*
 * luabridge – nativní doplněk klienta pro Mafia II (Steam, Classic), který spouští Lua kód
 * poslaný ze serveru M2O ve skriptovacím stroji hry.
 *
 * Nahrává se jako proxy `dinput8.dll` ve složce hry (`Mafia II/pc`). Skutečné DirectInput8Create
 * přepošle systémové dinput8.dll. Na vtable vstupních zařízení zachytí GetDeviceState /
 * GetDeviceData – hra je volá každý snímek na svém hlavním vlákně, a jen tam se smí sahat na Lua.
 *
 * Síť: TCP na server (luabridge.ini, výchozí 127.0.0.1:27020), řádkový protokol:
 *   klient → server  HELLO <steamid64> <verze>\n
 *                    RESULT <id> <kód> <délka>\n<text> (kód 0 = OK, jinak chyba Lua; text = první
 *                    návratová hodnota chunku nebo chybová hláška, pokud jde převést na řetězec)
 *   server → klient  LUA <id> <délka>\n<kód Lua>
 *
 * Adresy funkcí enginu pocházejí z m2sdk (Copyright MafiaHub / mafia2online, Apache License 2.0,
 * https://github.com/mafia2online/m2sdk) a platí jen pro Steam build mafia2.exe (PE razítko
 * 0x4D2745BA, obraz na 0x400000). M2O hru nespouští jako samostatný proces, ale mapuje mafia2.exe do
 * M2OLauncher.exe – build se proto ověřuje podle bajtů funkcí, ne podle hlavního modulu procesu.
 */
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BRIDGE_VERSION "0.2.0"

/* --- m2sdk: Steam mafia2.exe ------------------------------------------------------------- */
/* Začátky použitých funkcí Lua v podporovaném buildu */
static const unsigned char PROLOGUE_LOADBUFFER[] = {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x08, 0x8B, 0x45, 0x0C};
static const unsigned char PROLOGUE_PCALL[] = {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x14, 0x83, 0x7D, 0x14, 0x00};
static const unsigned char PROLOGUE_TOLSTRING[] = {0x55, 0x8B, 0xEC, 0x51, 0x8B, 0x45, 0x0C, 0x50, 0x8B, 0x4D};
static const unsigned char PROLOGUE_SETTOP[] = {0x55, 0x8B, 0xEC, 0x83, 0x7D, 0x0C, 0x00, 0x7C, 0x46, 0x8B};
static const unsigned char PROLOGUE_GETTOP[] = {0x55, 0x8B, 0xEC, 0x8B, 0x45, 0x08, 0x8B, 0x4D, 0x08, 0x8B};
#define ADDR_SCRIPT_MACHINE_MANAGER 0x01AC0C78u /* C_ScriptMachineManager* */
#define ADDR_LUAL_LOADBUFFER 0x005D5BD0u
#define ADDR_LUA_PCALL 0x005D3F80u
/* Další funkce Lua 5.1 API (pořadí lapi.c ověřené disassemblerem: tolstring volá index2adr,
   luaV_tostring a luaC_step; gettop/settop také v m2sdk/starém M2O) */
#define ADDR_LUA_GETTOP 0x005D2800u
#define ADDR_LUA_SETTOP 0x005D2820u
#define ADDR_LUA_TOLSTRING 0x005D3060u
#define RESULT_TEXT_MAX 60000

typedef int(__cdecl *luaL_loadbuffer_t)(void *L, const char *buf, size_t size, const char *name);
typedef int(__cdecl *lua_pcall_t)(void *L, int nargs, int nresults, int errfunc);
typedef int(__cdecl *lua_gettop_t)(void *L);
typedef void(__cdecl *lua_settop_t)(void *L, int idx);
typedef const char *(__cdecl *lua_tolstring_t)(void *L, int idx, size_t *len);

/* --- DirectInput (jen to, co potřebujeme) -------------------------------------------------- */
typedef HRESULT(WINAPI *DirectInput8Create_t)(HINSTANCE, DWORD, REFIID, LPVOID *, void *outer);
typedef HRESULT(__stdcall *CreateDevice_t)(void *self, REFGUID guid, void **device, void *outer);
typedef HRESULT(__stdcall *GetDeviceState_t)(void *self, DWORD size, LPVOID data);
typedef HRESULT(__stdcall *GetDeviceData_t)(void *self, DWORD size, void *data, DWORD *count, DWORD flags);

enum { VT_CREATE_DEVICE = 3, VT_GET_DEVICE_STATE = 9, VT_GET_DEVICE_DATA = 10 };

static DirectInput8Create_t real_DirectInput8Create;
static CreateDevice_t real_CreateDevice;
static GetDeviceState_t real_GetDeviceState;
static GetDeviceData_t real_GetDeviceData;

/* --- Stav ---------------------------------------------------------------------------------- */
static FILE *log_file;
static int game_supported;
static char server_host[128] = "127.0.0.1";
static int server_port = 27020;

static SOCKET sock = INVALID_SOCKET;
static int sock_connected;
static DWORD next_connect_tick;
static char rx[1 << 20];
static size_t rx_len;

static void logf(const char *fmt, ...)
{
    if (!log_file) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(log_file, "[%02d:%02d:%02d] ", t.wHour, t.wMinute, t.wSecond);
    va_list args;
    va_start(args, fmt);
    vfprintf(log_file, fmt, args);
    va_end(args);
    fputc('\n', log_file);
    fflush(log_file);
}

/* Jméno třídy (MSVC RTTI) objektu, jehož vtable leží v .rdata hry; jinak NULL */
static const char *rtti_name(DWORD object)
{
    if (object < 0x10000 || IsBadReadPtr((void *)object, 4)) return NULL;
    DWORD vtable = *(const DWORD *)object;
    if (vtable < 0x1800000 || vtable >= 0x1B00000 || IsBadReadPtr((void *)(vtable - 4), 4)) return NULL;
    const DWORD *col = *(const DWORD **)(vtable - 4);
    if ((DWORD)col < 0x1800000 || (DWORD)col >= 0x1B00000 || IsBadReadPtr(col, 16)) return NULL;
    const char *name = (const char *)(col[3] + 8);
    return !IsBadReadPtr(name, 4) && name[0] == '.' ? name : NULL;
}

/* Diagnostika pádů: výjimky zapíše do logu (kód, adresa, modul). U přístupu do paměti v kódu hry
   (0x400000–0x1B00000) navíc registry a adresy z vrcholu zásobníku, které vypadají jako návratové
   adresy kódu hry. Výjimku nezpracuje. */
static LONG CALLBACK crash_logger(EXCEPTION_POINTERS *info)
{
    static uintptr_t last_eip;
    static volatile LONG other_count;
    CONTEXT *c = info->ContextRecord;
    DWORD code = info->ExceptionRecord->ExceptionCode;
    /* Běžné neškodné výjimky: jméno vlákna, C++ throw, ladicí výpisy */
    if (code == 0x406D1388 || code == 0xE06D7363 || code == 0x40010006 || code == 0x4001000A) return EXCEPTION_CONTINUE_SEARCH;
    if (code != EXCEPTION_ACCESS_VIOLATION || c->Eip < 0x400000 || c->Eip >= 0x1B00000) {
        if (InterlockedIncrement(&other_count) > 30) return EXCEPTION_CONTINUE_SEARCH;
        char module[MAX_PATH] = "?";
        HMODULE owner = NULL;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)info->ExceptionRecord->ExceptionAddress, &owner))
            GetModuleFileNameA(owner, module, MAX_PATH);
        logf("VÝJIMKA %08lX na %p (%s + %lX), vlákno %lu, esp=%08lX", code, info->ExceptionRecord->ExceptionAddress,
             strrchr(module, '\\') ? strrchr(module, '\\') + 1 : module,
             (unsigned long)((uintptr_t)info->ExceptionRecord->ExceptionAddress - (uintptr_t)owner),
             GetCurrentThreadId(), c->Esp);
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (c->Eip == last_eip) return EXCEPTION_CONTINUE_SEARCH;
    last_eip = c->Eip;
    logf("PÁD eip=%08lX eax=%08lX ebx=%08lX ecx=%08lX edx=%08lX esi=%08lX edi=%08lX ebp=%08lX esp=%08lX vlákno %lu",
         c->Eip, c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi, c->Ebp, c->Esp, GetCurrentThreadId());
    /* Pád v 0x14CFC10: alokátor (2. argument, po dvou push na esp+0x10) vrátil NULL */
    if (c->Eip == 0x14CFC53 && !IsBadReadPtr((void *)(c->Esp + 0x10), 4)) {
        const DWORD *allocator = *(const DWORD **)(c->Esp + 0x10);
        if (allocator && !IsBadReadPtr(allocator, 32)) {
            logf("PÁD alokátor %p: %08lX %08lX %08lX %08lX %08lX %08lX %08lX %08lX", (void *)allocator, allocator[0],
                 allocator[1], allocator[2], allocator[3], allocator[4], allocator[5], allocator[6], allocator[7]);
            /* Zásoba si svůj buffer bere z nadřazeného alokátoru: allocator[5] → +0xC */
            const DWORD *holder = (const DWORD *)allocator[5];
            const DWORD *parent = holder && !IsBadReadPtr(holder, 16) ? (const DWORD *)holder[3] : NULL;
            for (int level = 0; parent && level < 3 && !IsBadReadPtr(parent, 64); level++) {
                logf("PÁD nadřazený[%d] %p: %08lX %08lX %08lX %08lX %08lX %08lX %08lX %08lX %08lX %08lX %08lX %08lX "
                     "%08lX %08lX %08lX %08lX", level, (void *)parent, parent[0], parent[1], parent[2], parent[3],
                     parent[4], parent[5], parent[6], parent[7], parent[8], parent[9], parent[10], parent[11],
                     parent[12], parent[13], parent[14], parent[15]);
                const DWORD *next = (const DWORD *)parent[1];
                parent = next && next != parent && !IsBadReadPtr(next, 4) && *next >= 0x1800000 && *next < 0x1B00000
                             ? next
                             : NULL;
            }
        }
    }
    char line[1024];
    size_t used = 0;
    const DWORD *stack = (const DWORD *)c->Esp;
    for (int i = 0; i < 512 && used < sizeof(line) - 16; i++) {
        if (IsBadReadPtr(stack + i, 4)) break;
        DWORD value = stack[i];
        if (value >= 0x401000 && value < 0x1900000)
            used += (size_t)snprintf(line + used, sizeof(line) - used, " %08lX", value);
    }
    line[used] = '\0';
    logf("PÁD zásobník:%s", line);

    /* Objekty na zásobníku a v registrech s RTTI – napoví, který subsystém alokoval */
    used = 0;
    line[0] = '\0';
    DWORD registers[] = {c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi, c->Ebp};
    for (int i = 0; i < 7 + 256 && used < sizeof(line) - 96; i++) {
        DWORD value = i < 7 ? registers[i] : (IsBadReadPtr(stack + i - 7, 4) ? 0 : stack[i - 7]);
        const char *name = rtti_name(value);
        if (name && !strstr(line, name)) used += (size_t)snprintf(line + used, sizeof(line) - used, " %s", name);
    }
    logf("PÁD objekty:%s", line);
    return EXCEPTION_CONTINUE_SEARCH;
}

/* --- Lineární zásoby paměti hry (načítání Sicílie v M2O) -----------------------------------------
   0x1567FB0 je konstruktor bump alokátoru (vtable 0x1920848): kapacitu dostane ukazatelem a buffer
   si vezme z nadřazeného alokátoru. Načítací vlákno má pevné zásoby pro jednu úlohu (346 112,
   780 000, 3 213 056 a 1 700 896 B), které se pro každý SDS vynulují; sicily01 se do nich nevejde.
   luabridge.ini, [debug]: arena_boost=780000:1200000,3213056:4900000 zvětší zásoby dané velikosti,
   arena_log_min=N zapíše do logu každou zásobu od N bajtů. */
#define ADDR_ARENA_CTOR 0x01567FB0u
#define ARENA_BOOST_MAX 8
static const unsigned char PROLOGUE_ARENA_CTOR[] = {0x8B, 0x44, 0x24, 0x04, 0x56, 0x8B, 0xF1};
static DWORD arena_log_min;
static DWORD arena_boost_from[ARENA_BOOST_MAX], arena_boost_to[ARENA_BOOST_MAX];
static int arena_boost_count;
static volatile LONG arena_log_count;

void on_arena_ctor(DWORD *stack) __asm__("luabridge_on_arena_ctor");
__attribute__((used)) void on_arena_ctor(DWORD *stack)
{
    DWORD *capacity = (DWORD *)stack[1];
    DWORD size = *capacity;
    int boosted = 0;
    for (int i = 0; i < arena_boost_count && !boosted; i++)
        if (size == arena_boost_from[i]) {
            *capacity = arena_boost_to[i];
            boosted = 1;
        }
    if (!arena_log_min || size < arena_log_min || InterlockedIncrement(&arena_log_count) > 600) return;
    /* Nadřazený alokátor: 2. argument (holder) → +0xC; u bump alokátoru [2] kapacita, [3] kurzor, [4] báze */
    const DWORD *holder = (const DWORD *)stack[2];
    const DWORD *parent = holder && !IsBadReadPtr(holder, 16) ? (const DWORD *)holder[3] : NULL;
    char parent_info[96] = "?";
    if (parent && !IsBadReadPtr(parent, 20)) {
        if (parent[0] == 0x01920848)
            snprintf(parent_info, sizeof(parent_info), "%p bump kapacita %lu volno %ld", (void *)parent, parent[2],
                     (long)(parent[4] + parent[2] - parent[3]));
        else
            snprintf(parent_info, sizeof(parent_info), "%p vtable %08lX [%08lX %08lX %08lX %08lX %08lX %08lX]",
                     (void *)parent, parent[0], parent[1], parent[2], parent[3], parent[4],
                     IsBadReadPtr(parent, 28) ? 0 : parent[5], IsBadReadPtr(parent, 28) ? 0 : parent[6]);
    }
    char callers[96];
    size_t used = 0;
    for (int i = 3; i < 64 && used < sizeof(callers) - 10; i++)
        if (stack[i] >= 0x401000 && stack[i] < 0x1900000)
            used += (size_t)snprintf(callers + used, sizeof(callers) - used, " %08lX", stack[i]);
    callers[used] = '\0';
    logf("zásoba %lu B%s, rodič %s, vlákno %lu, zásobník:%s", size, boosted ? " → zvětšena" : "", parent_info,
         GetCurrentThreadId(), callers);
}

/* Skok sem místo prvních 7 bajtů konstruktoru; zachová registry a pokračuje v originále */
__attribute__((naked)) static void hook_arena_ctor(void)
{
    __asm__ volatile(".intel_syntax noprefix\n"
                     "pushfd\n"
                     "pushad\n"
                     "lea eax, [esp + 36]\n"
                     "push eax\n"
                     "call luabridge_on_arena_ctor\n"
                     "add esp, 4\n"
                     "popad\n"
                     "popfd\n"
                     "mov eax, [esp + 4]\n"
                     "push esi\n"
                     "mov esi, ecx\n"
                     "push 0x01567FB7\n"
                     "ret\n"
                     ".att_syntax prefix\n");
}

/* Hlavní halda SDS (vtable 0x19209AC) má kapacitu přesně na všechny sloty + pracovní zásoby
   načítání, takže zvětšené zásoby se do ní musí vejít navíc: halda od 100 MB se zvětší o součet
   zvětšení. Halda vzniká buď konstruktorem 0x156A1D0 (velikost ukazatelem), nebo továrnou 0x156B680
   (argumenty: výstup, velikost, příznak). */
#define ADDR_HEAP_CTOR 0x0156A1D0u
#define ADDR_HEAP_FACTORY 0x0156B680u
static const unsigned char PROLOGUE_HEAP_FACTORY[] = {0x80, 0x7C, 0x24, 0x0C, 0x00};
static DWORD heap_extra;

static void grow_heap(DWORD *capacity, const char *source)
{
    if (*capacity < 100u * 1024 * 1024) return;
    logf("halda SDS %lu B (%s) → %lu B", *capacity, source, *capacity + heap_extra);
    *capacity += heap_extra;
}

void on_heap_ctor(DWORD *stack) __asm__("luabridge_on_heap_ctor");
__attribute__((used)) void on_heap_ctor(DWORD *stack) { grow_heap((DWORD *)stack[1], "konstruktor"); }
void on_heap_factory(DWORD *stack) __asm__("luabridge_on_heap_factory");
__attribute__((used)) void on_heap_factory(DWORD *stack) { grow_heap(&stack[2], "továrna"); }

__attribute__((naked)) static void hook_heap_ctor(void)
{
    __asm__ volatile(".intel_syntax noprefix\n"
                     "pushfd\n"
                     "pushad\n"
                     "lea eax, [esp + 36]\n"
                     "push eax\n"
                     "call luabridge_on_heap_ctor\n"
                     "add esp, 4\n"
                     "popad\n"
                     "popfd\n"
                     "mov eax, [esp + 4]\n"
                     "push esi\n"
                     "mov esi, ecx\n"
                     "push 0x0156A1D7\n"
                     "ret\n"
                     ".att_syntax prefix\n");
}

__attribute__((naked)) static void hook_heap_factory(void)
{
    __asm__ volatile(".intel_syntax noprefix\n"
                     "pushfd\n"
                     "pushad\n"
                     "lea eax, [esp + 36]\n"
                     "push eax\n"
                     "call luabridge_on_heap_factory\n"
                     "add esp, 4\n"
                     "popad\n"
                     "popfd\n"
                     "cmp byte ptr [esp + 0x0C], 0\n"
                     "push 0x0156B685\n"
                     "ret\n"
                     ".att_syntax prefix\n");
}

/* Přepíše začátek funkce skokem na hook, pokud bajty sedí na podporovaný build */
static int patch_jump(DWORD address, const unsigned char *expected, size_t length, void (*hook)(void))
{
    if (memcmp((void *)address, expected, length) != 0) {
        logf("hook %08lX: bajty nesedí, vynechán", address);
        return 0;
    }
    DWORD old;
    unsigned char *target = (unsigned char *)address;
    VirtualProtect(target, 5, PAGE_EXECUTE_READWRITE, &old);
    target[0] = 0xE9;
    *(DWORD *)(target + 1) = (DWORD)hook - (address + 5);
    VirtualProtect(target, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, 5);
    return 1;
}

static void install_arena_hook(void)
{
    if (!patch_jump(ADDR_ARENA_CTOR, PROLOGUE_ARENA_CTOR, sizeof(PROLOGUE_ARENA_CTOR), hook_arena_ctor)) return;
    for (int i = 0; i < arena_boost_count; i++)
        if (arena_boost_to[i] > arena_boost_from[i]) heap_extra += (arena_boost_to[i] - arena_boost_from[i] + 0xFFF) & ~0xFFFu;
    if (heap_extra) {
        patch_jump(ADDR_HEAP_CTOR, PROLOGUE_ARENA_CTOR, sizeof(PROLOGUE_ARENA_CTOR), hook_heap_ctor);
        patch_jump(ADDR_HEAP_FACTORY, PROLOGUE_HEAP_FACTORY, sizeof(PROLOGUE_HEAP_FACTORY), hook_heap_factory);
    }
    logf("zásoby: hook aktivní, log od %lu B, %d zvětšení, halda SDS +%lu B", arena_log_min, arena_boost_count,
         heap_extra);
}

/* --- Lua ve hře ---------------------------------------------------------------------------- */
static void *read_ptr(uintptr_t address)
{
    if (!address || IsBadReadPtr((void *)address, sizeof(void *))) return NULL;
    return *(void **)address;
}

/* C_ScriptMachineManager → stroj → pool → první skript → lua_State (offsety podle m2sdk) */
static void *game_lua_state(void)
{
    void *manager = read_ptr(ADDR_SCRIPT_MACHINE_MANAGER);
    void *machine = manager ? read_ptr((uintptr_t)manager + 4) : NULL;
    void *pool = machine ? read_ptr((uintptr_t)machine + 4) : NULL;
    void *script = pool ? read_ptr((uintptr_t)pool) : NULL;
    return script ? read_ptr((uintptr_t)script + 0x50) : NULL;
}

/* Spustí chunk. Vrací 0 = OK; jinak kód z luaL_loadbuffer (1xx) / lua_pcall (2xx), -1 bez Lua.
   Do text/text_len zkopíruje první návratovou hodnotu nebo chybovou hlášku (je-li to řetězec/číslo). */
static int run_lua(const char *code, size_t size, char *text, size_t *text_len)
{
    *text_len = 0;
    void *L = game_lua_state();
    if (!L) return -1;
    int top = ((lua_gettop_t)ADDR_LUA_GETTOP)(L);
    int result = ((luaL_loadbuffer_t)ADDR_LUAL_LOADBUFFER)(L, code, size, "=m2o_luabridge");
    if (result != 0) {
        result += 100;
    } else {
        int call = ((lua_pcall_t)ADDR_LUA_PCALL)(L, 0, 1, 0);
        result = call ? 200 + call : 0;
    }
    if (((lua_gettop_t)ADDR_LUA_GETTOP)(L) > top) {
        size_t len = 0;
        const char *value = ((lua_tolstring_t)ADDR_LUA_TOLSTRING)(L, -1, &len);
        if (value) {
            *text_len = len < RESULT_TEXT_MAX ? len : RESULT_TEXT_MAX;
            memcpy(text, value, *text_len);
        }
    }
    ((lua_settop_t)ADDR_LUA_SETTOP)(L, top);
    return result;
}

/* --- Steam ID (ISteamUser::GetSteamID, vtable index 2, návrat přes skrytý ukazatel) --------- */
typedef void *(__cdecl *SteamUser_t)(void);
typedef uint64_t *(__thiscall *GetSteamID_t)(void *self, uint64_t *out);

static uint64_t steam_id(void)
{
    HMODULE steam = GetModuleHandleA("steam_api.dll");
    SteamUser_t SteamUser = steam ? (SteamUser_t)GetProcAddress(steam, "SteamUser") : NULL;
    void *user = SteamUser ? SteamUser() : NULL;
    if (!user) return 0;
    uint64_t id = 0;
    ((GetSteamID_t)(*(void ***)user)[2])(user, &id);
    return id;
}

/* --- Síť (neblokující, volá se z hlavního vlákna) ----------------------------------------- */
static void net_close(const char *why)
{
    if (sock != INVALID_SOCKET) {
        closesocket(sock);
        logf("odpojeno: %s", why);
    }
    sock = INVALID_SOCKET;
    sock_connected = 0;
    rx_len = 0;
    next_connect_tick = GetTickCount() + 3000;
}

/* Neblokující socket: dopošle vše (krátce čeká, když je odchozí buffer plný) */
static void net_send_raw(const char *data, size_t len)
{
    if (sock == INVALID_SOCKET || !sock_connected) return;
    DWORD deadline = GetTickCount() + 2000;
    while (len > 0) {
        int sent = send(sock, data, (int)len, 0);
        if (sent > 0) {
            data += sent;
            len -= (size_t)sent;
        } else if (WSAGetLastError() == WSAEWOULDBLOCK && GetTickCount() < deadline) {
            Sleep(1);
        } else {
            net_close("odeslání selhalo");
            return;
        }
    }
}

static void net_send(const char *line)
{
    net_send_raw(line, strlen(line));
}

static void net_connect(void)
{
    if (GetTickCount() < next_connect_tick) return;
    next_connect_tick = GetTickCount() + 3000;

    struct hostent *host = gethostbyname(server_host);
    if (!host) return;
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)server_port);
    memcpy(&addr.sin_addr, host->h_addr_list[0], sizeof(addr.sin_addr));

    sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    u_long nonblocking = 1;
    ioctlsocket(sock, FIONBIO, &nonblocking);
    connect(sock, (struct sockaddr *)&addr, sizeof(addr));
}

static void handle_messages(void)
{
    for (;;) {
        char *newline = memchr(rx, '\n', rx_len);
        if (!newline) return;
        *newline = '\0';
        size_t header_len = (size_t)(newline - rx) + 1;

        unsigned id = 0;
        size_t size = 0;
        if (sscanf(rx, "LUA %u %zu", &id, &size) == 2) {
            if (size > sizeof(rx) - header_len) {
                net_close("příliš dlouhý kód");
                return;
            }
            if (rx_len < header_len + size) {
                *newline = '\n'; /* kód ještě nedorazil celý */
                return;
            }
            static char text[RESULT_TEXT_MAX];
            size_t text_len = 0;
            int result = run_lua(rx + header_len, size, text, &text_len);
            logf("LUA %u (%zu B) -> %d", id, size, result);
            char reply[64];
            snprintf(reply, sizeof(reply), "RESULT %u %d %zu\n", id, result, text_len);
            net_send(reply);
            net_send_raw(text, text_len);
            if (sock == INVALID_SOCKET) return; /* spojení spadlo, buffer je vyprázdněný */
            header_len += size;
        }
        memmove(rx, rx + header_len, rx_len - header_len);
        rx_len -= header_len;
    }
}

/* Jeden krok sítě a front – hlavní vlákno hry, každý snímek */
static void pump(void)
{
    if (!game_supported) return;
    if (sock == INVALID_SOCKET) {
        net_connect();
        return;
    }

    fd_set wr, ex;
    struct timeval zero = {0, 0};
    if (!sock_connected) {
        FD_ZERO(&wr);
        FD_ZERO(&ex);
        FD_SET(sock, &wr);
        FD_SET(sock, &ex);
        if (select(0, NULL, &wr, &ex, &zero) <= 0) return;
        if (FD_ISSET(sock, &ex)) {
            net_close("spojení se nepodařilo");
            return;
        }
        uint64_t id = steam_id();
        /* Steam ještě není připravený – zkusí se příští snímek. Po ~10 s se bridge přihlásí
           bez Steam ID (0) a server ho spáruje s jediným hráčem bez bridge. */
        if (!id && GetTickCount() < next_connect_tick + 7000) return;
        sock_connected = 1;
        char hello[96];
        snprintf(hello, sizeof(hello), "HELLO %llu %s\n", (unsigned long long)id, BRIDGE_VERSION);
        net_send(hello);
        logf("připojeno k %s:%d jako %llu", server_host, server_port, (unsigned long long)id);
    }

    for (;;) {
        int n = recv(sock, rx + rx_len, (int)(sizeof(rx) - rx_len), 0);
        if (n > 0) {
            rx_len += (size_t)n;
            handle_messages();
            continue;
        }
        if (n == 0 || WSAGetLastError() != WSAEWOULDBLOCK) net_close("spojení ukončeno");
        return;
    }
}

/* --- Háčky DirectInput --------------------------------------------------------------------- */
static void patch_vtable(void **vtable, int index, void *hook, void **original)
{
    if (vtable[index] == hook) return;
    DWORD old;
    VirtualProtect(&vtable[index], sizeof(void *), PAGE_EXECUTE_READWRITE, &old);
    *original = vtable[index];
    vtable[index] = hook;
    VirtualProtect(&vtable[index], sizeof(void *), old, &old);
}

static HRESULT __stdcall hook_GetDeviceState(void *self, DWORD size, LPVOID data)
{
    pump();
    return real_GetDeviceState(self, size, data);
}

static HRESULT __stdcall hook_GetDeviceData(void *self, DWORD size, void *data, DWORD *count, DWORD flags)
{
    pump();
    return real_GetDeviceData(self, size, data, count, flags);
}

static HRESULT __stdcall hook_CreateDevice(void *self, REFGUID guid, void **device, void *outer)
{
    HRESULT hr = real_CreateDevice(self, guid, device, outer);
    if (SUCCEEDED(hr) && device && *device) {
        void **vtable = *(void ***)*device;
        patch_vtable(vtable, VT_GET_DEVICE_STATE, (void *)hook_GetDeviceState, (void **)&real_GetDeviceState);
        patch_vtable(vtable, VT_GET_DEVICE_DATA, (void *)hook_GetDeviceData, (void **)&real_GetDeviceData);
    }
    return hr;
}

__declspec(dllexport) HRESULT WINAPI DirectInput8Create(HINSTANCE inst, DWORD version, REFIID iid, LPVOID *out,
                                                        void *outer)
{
    if (!real_DirectInput8Create) {
        char path[MAX_PATH];
        GetSystemDirectoryA(path, MAX_PATH);
        strcat(path, "\\dinput8.dll");
        HMODULE real = LoadLibraryA(path);
        real_DirectInput8Create = real ? (DirectInput8Create_t)GetProcAddress(real, "DirectInput8Create") : NULL;
        if (!real_DirectInput8Create) {
            logf("systémové dinput8.dll se nepodařilo načíst");
            return E_FAIL;
        }
    }
    HRESULT hr = real_DirectInput8Create(inst, version, iid, out, outer);
    if (SUCCEEDED(hr) && out && *out && game_supported) {
        void **vtable = *(void ***)*out;
        patch_vtable(vtable, VT_CREATE_DEVICE, (void *)hook_CreateDevice, (void **)&real_CreateDevice);
    }
    return hr;
}

/* --- Inicializace ------------------------------------------------------------------------- */
static void init(HMODULE self)
{
    char dir[MAX_PATH], path[MAX_PATH];
    GetModuleFileNameA(self, dir, MAX_PATH);
    char *slash = strrchr(dir, '\\');
    if (slash) slash[1] = '\0';

    snprintf(path, sizeof(path), "%sluabridge.log", dir);
    log_file = fopen(path, "w");

    snprintf(path, sizeof(path), "%sluabridge.ini", dir);
    GetPrivateProfileStringA("bridge", "host", server_host, server_host, sizeof(server_host), path);
    server_port = (int)GetPrivateProfileIntA("bridge", "port", server_port, path);

    /* Launcher M2O při startu vrací soubory ve vfs podle svého manifestu, proto upravenou
       StreamMapa.bin (řádky pro Sicílii, tools/streammap.py) nakopírujeme až tady – po kontrole
       launcheru a před otevřením světa. */
    char source[MAX_PATH], target[MAX_PATH];
    snprintf(source, sizeof(source), "%sluabridge_StreamMapa.bin", dir);
    GetModuleFileNameA(GetModuleHandleA(NULL), target, MAX_PATH);
    char *host_slash = strrchr(target, '\\');
    if (host_slash && GetFileAttributesA(source) != INVALID_FILE_ATTRIBUTES) {
        snprintf(host_slash + 1, sizeof(target) - (size_t)(host_slash + 1 - target), "vfs\\StreamMapa.bin");
        BOOL copied = CopyFileA(source, target, FALSE);
        logf("StreamMapa: %s → %s (%s)", source, target, copied ? "OK" : "chyba");
    }

    /* Adresy z m2sdk platí jen pro jeden build – jinak bridge zůstane jen průchozím dinput8.
       Hra je v paměti na 0x400000 (sama, nebo namapovaná launcherem M2O). */
    game_supported = !IsBadReadPtr((void *)ADDR_LUAL_LOADBUFFER, sizeof(PROLOGUE_LOADBUFFER)) &&
                     !memcmp((void *)ADDR_LUAL_LOADBUFFER, PROLOGUE_LOADBUFFER, sizeof(PROLOGUE_LOADBUFFER)) &&
                     !memcmp((void *)ADDR_LUA_PCALL, PROLOGUE_PCALL, sizeof(PROLOGUE_PCALL)) &&
                     !memcmp((void *)ADDR_LUA_TOLSTRING, PROLOGUE_TOLSTRING, sizeof(PROLOGUE_TOLSTRING)) &&
                     !memcmp((void *)ADDR_LUA_SETTOP, PROLOGUE_SETTOP, sizeof(PROLOGUE_SETTOP)) &&
                     !memcmp((void *)ADDR_LUA_GETTOP, PROLOGUE_GETTOP, sizeof(PROLOGUE_GETTOP));
    char host[MAX_PATH] = "?";
    GetModuleFileNameA(GetModuleHandleA(NULL), host, MAX_PATH);
    logf("luabridge %s v %s, mafia2.exe na 0x400000: %s, server %s:%d", BRIDGE_VERSION, host,
         game_supported ? "podporovaný build" : "NEPODPOROVANÝ build (nebo ještě nenačtený)", server_host,
         server_port);

    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    AddVectoredExceptionHandler(0, crash_logger);

    arena_log_min = (DWORD)GetPrivateProfileIntA("debug", "arena_log_min", 0, path);
    char boosts[256];
    GetPrivateProfileStringA("debug", "arena_boost", "", boosts, sizeof(boosts), path);
    for (char *item = strtok(boosts, ","); item && arena_boost_count < ARENA_BOOST_MAX; item = strtok(NULL, ",")) {
        unsigned long from = 0, to = 0;
        if (sscanf(item, " %lu:%lu", &from, &to) == 2 && from && to) {
            arena_boost_from[arena_boost_count] = from;
            arena_boost_to[arena_boost_count++] = to;
            logf("zásoby: %lu → %lu B", from, to);
        }
    }
    if (game_supported && (arena_log_min || arena_boost_count)) install_arena_hook();
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        init(instance);
    }
    return TRUE;
}

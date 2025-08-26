#include <windows.h>
#include <objbase.h>

#include <psapi.h>
#include <regex>
#include <TlHelp32.h>
#include <fstream>
#include <mutex>

#include <xxhash.h>
#include <zstd.h>

#include "worker.hpp"

#include "utils/resource.h"

#include "luacode.h"

extern "C" {
#include "blake3/blake3.h"
}

std::vector<std::uintptr_t> functions::GetChildrenAddresses(std::uintptr_t address, HANDLE handle) {
    std::vector<std::uintptr_t> children;
    {
        std::uintptr_t childrenPtr = read_memory<std::uintptr_t>(address + offsets::Children, handle);
        if (childrenPtr == 0)
            return children;

        std::uintptr_t childrenStart = read_memory<std::uintptr_t>(childrenPtr, handle);
        std::uintptr_t childrenEnd = read_memory<std::uintptr_t>(childrenPtr + 0x8, handle) + 1;

        for (std::uintptr_t childAddress = childrenStart; childAddress < childrenEnd; childAddress += 0x10) {
            std::uintptr_t childPtr = read_memory<std::uintptr_t>(childAddress, handle);
            if (childPtr != 0)
                children.push_back(childPtr);
        }
    }
    return children;
}

std::string functions::ReadRobloxString(std::uintptr_t address, HANDLE handle) {
    std::uint64_t stringCount = read_memory<std::uint64_t>(address + 0x10, handle);
    if (stringCount == 0 || stringCount > 15000) return "";

    if (stringCount > 15)
        address = read_memory<std::uintptr_t>(address, handle);

    std::string buffer(stringCount, '\0');
    SIZE_T bytesRead = 0;

    if (!ReadProcessMemory(handle, reinterpret_cast<LPCVOID>(address), buffer.data(), stringCount, &bytesRead) || bytesRead != stringCount)
        return "";

    return buffer;
}

std::string Instance::GetBytecode() const {
    if (ClassName() != "LocalScript" && ClassName() != "ModuleScript")
        return "";

    std::uintptr_t embeddedSourceOffset = (ClassName() == "LocalScript") ? offsets::LocalScriptEmbedded : offsets::ModuleScriptEmbedded;
    std::uintptr_t embeddedPtr = read_memory<std::uintptr_t>(_Self + embeddedSourceOffset, handle);

    std::uintptr_t bytecodePtr = read_memory<std::uintptr_t>(embeddedPtr + offsets::Bytecode, handle);
    std::uint64_t bytecodeSize = read_memory<std::uint64_t>(embeddedPtr + offsets::BytecodeSize, handle);

    if (bytecodeSize == 0) return "";

    std::string bytecodeBuffer(bytecodeSize, '\0');
    SIZE_T bytesRead = 0;

    if (!ReadProcessMemory(handle, reinterpret_cast<LPCVOID>(bytecodePtr), bytecodeBuffer.data(), bytecodeSize, &bytesRead) || bytesRead != bytecodeSize)
        return "";

    return decompress(bytecodeBuffer);
}

static HMODULE getModule() {
    HMODULE hModule;
    GetModuleHandleEx(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
        (LPCTSTR)getModule,
        &hModule);
    return hModule;
}

static std::filesystem::path GetHandlePath(HANDLE processHandle) {
    char buffer[MAX_PATH];
    DWORD bufferSize = sizeof(buffer);

    if (QueryFullProcessImageNameA(processHandle, 0, buffer, &bufferSize)) {
        return std::filesystem::path(buffer);
    }

    return {};
}

static std::string generateGUID() {
    GUID guid;
    HRESULT result = CoCreateGuid(&guid);

    if (result != S_OK) {
        throw std::runtime_error("Failed to generate GUID");
    }

    char guidStr[39];
    snprintf(guidStr, sizeof(guidStr), "%08lX-%04X-%04X-%04X-%012llX", guid.Data1,
        guid.Data2, guid.Data3, (guid.Data4[0] << 8) | guid.Data4[1],
        ((static_cast<unsigned long long>(guid.Data4[2]) << 40) |
            (static_cast<unsigned long long>(guid.Data4[3]) << 32) |
            (static_cast<unsigned long long>(guid.Data4[4]) << 24) |
            (static_cast<unsigned long long>(guid.Data4[5]) << 16) |
            (static_cast<unsigned long long>(guid.Data4[6]) << 8) |
            static_cast<unsigned long long>(guid.Data4[7])));

    return std::string(guidStr);
}

static void replaceString(std::string& source, const std::string_view toReplace, const std::string_view replacement) {
    size_t pos = source.find(toReplace);

    if (pos != std::string::npos) {
        source.replace(pos, toReplace.length(), replacement);
    }
}

uintptr_t GetBase(DWORD pid) {
    MODULEENTRY32W entry = { sizeof(entry) };
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;
    if (Module32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szModule, L"RobloxPlayerBeta.exe") == 0) {
                CloseHandle(snapshot);
                return reinterpret_cast<uintptr_t>(entry.modBaseAddr);
            }
        } while (Module32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return 0;
}

RBXClient::RBXClient(DWORD processID) :
    handle(OpenProcess(PROCESS_ALL_ACCESS, TRUE, processID)),
    PID(processID)
{
    if (handle == NULL) {
        std::cerr << "[!] Failed to open process " << processID << ": " << GetLastError() << "\n";
        return;
    }

    ClientDir = GetHandlePath(handle).parent_path();
    GUID = generateGUID();
    Version = ClientDir.filename().string();

    PROCESS_MEMORY_COUNTERS memory_counter;
    K32GetProcessMemoryInfo(handle, &memory_counter, sizeof(memory_counter));
    DWORD startTime = GetTickCount();
    while (memory_counter.WorkingSetSize < 150'000'000 && GetTickCount() - startTime < 5000) {
        K32GetProcessMemoryInfo(handle, &memory_counter, sizeof(memory_counter));
        Sleep(50);
    }

    HWND clientHWND = GetHWNDFromPID(GetProcessId(handle));
    startTime = GetTickCount();
    while (!clientHWND && GetTickCount() - startTime < 5000) {
        clientHWND = GetHWNDFromPID(GetProcessId(handle));
        std::cout << "Waiting for Client HWND\n";
        Sleep(50);
    }
    Sleep(500); 

    std::uintptr_t dataModelAddress = FetchDataModel();
    if (dataModelAddress == 0) {
        std::cerr << "[!] Failed to fetch datamodel\n";
        return;
    }
    Instance DataModel(dataModelAddress, handle);

    std::uintptr_t LocalPlayerAddr = 0;
    startTime = GetTickCount();
    while (LocalPlayerAddr == 0 && GetTickCount() - startTime < 5000) {
        LocalPlayerAddr = read_memory<std::uintptr_t>(
            DataModel.FindFirstChildOfClassAddress("Players") + offsets::LocalPlayer,
            handle
        );
        Sleep(50);
    }
    if (LocalPlayerAddr == 0) {
        std::cerr << "[!] Failed to fetch LocalPlayer\n";
        return;
    }

    Instance LocalPlayer(LocalPlayerAddr, handle);

    // Wait for username to load (max 5 seconds)
    Username = LocalPlayer.Name();
    startTime = GetTickCount();
    while ((Username == "Player" || Username.empty()) && GetTickCount() - startTime < 5000) {
        Username = LocalPlayer.Name();
        Sleep(50);
    }

    if (Username.empty())
        Username = "Unknown";

    // Need to add checks else the process will crash, yes it sucks.
    auto CoreGui = DataModel.FindFirstChild("CoreGui");
    if (!CoreGui) {
        std::cerr << "[!] Game->CoreGui not found\n";
        return;
    }

    auto RobloxGui = CoreGui->FindFirstChild("RobloxGui");
    if (!RobloxGui) {
        std::cerr << "[!] CoreGui->RobloxGui not found\n";
        return;
    }
    auto Modules = RobloxGui->FindFirstChild("Modules");
    if (!Modules) {
        std::cerr << "[!] RobloxGui->Modules not found\n";
        return;
    }

    std::unique_ptr<Instance> PatchScript = nullptr;
    {
        auto PlayerList = Modules->FindFirstChild("PlayerList");
        if (!PlayerList) {
            std::cerr << "[!] Modules->PlayerList not found\n";
            return;
        }

        PatchScript = PlayerList->FindFirstChild("PlayerListManager");
    }

    if (!PatchScript) {
        std::cerr << "[!] Patch Script was not found\n";
        return;
    }

    std::string clientScript;
    {
        HMODULE module = getModule();
        if (!module) {
            std::cerr << "[!] Could not get module handle: " << GetLastError() << "\n";
            return;
        }
        HRSRC resource = FindResource(module, MAKEINTRESOURCE(SCRIPT), MAKEINTRESOURCE(LUAFILE));
        if (!resource) {
            std::cerr << "[!] Could not get the client.lua resource\n";
            return;
        }

        HGLOBAL data = LoadResource(module, resource);
        if (!data) {
            std::cerr << "[!] Could not get the could load resource\n";
            return;
        }

        DWORD size = SizeofResource(module, resource);
        char* finalData = static_cast<char*>(LockResource(data));

        clientScript.assign(finalData, size);
    }

    replaceString(clientScript, "%XENO_UNIQUE_ID%", GUID);
    replaceString(clientScript, "%XENO_VERSION%", Xeno_Version);

    //const std::string PatchScriptSource = "--!native\n--!optimize 1\n--!nonstrict\nlocal a={}local b=game:GetService(\"ContentProvider\")local function c(d)local e,f=d:find(\"%.\")local g=d:sub(f+1)if g:sub(-1)~=\"/\"then g=g..\"/\"end;return g end;local d=b.BaseUrl;local g=c(d)local h=string.format(\"https://games.%s\",g)local i=string.format(\"https://apis.rcs.%s\",g)local j=string.format(\"https://apis.%s\",g)local k=string.format(\"https://accountsettings.%s\",g)local l=string.format(\"https://gameinternationalization.%s\",g)local m=string.format(\"https://locale.%s\",g)local n=string.format(\"https://users.%s\",g)local o={GAME_URL=h,RCS_URL=i,APIS_URL=j,ACCOUNT_SETTINGS_URL=k,GAME_INTERNATIONALIZATION_URL=l,LOCALE_URL=m,ROLES_URL=n}setmetatable(a,{__newindex=function(p,q,r)end,__index=function(p,r)return o[r]end})return a";

    if (DataModel.Name() == "LuaApp") { // In home page
        //PatchScript->SetBytecode(Compile("coroutine.wrap(function(...)" + clientScript + "\nend)();" + PatchScriptSource));
        return;
    }

    auto RobloxReplicatedStorage = DataModel.FindFirstChildOfClass("RobloxReplicatedStorage");
    if (RobloxReplicatedStorage->FindFirstChild("Xeno")) {
        std::cerr << "[!] Client '" << Username << "' is already attached\n";
        // When player serverhops the GUID is going to be replaced with the new one. This fixes the communication with the bridge
        //PatchScript->SetBytecode(Compile("coroutine.wrap(function(...)" + clientScript + "\nend)();" + PatchScriptSource));
        return;
    }

    std::lock_guard<std::mutex> lock(clientsMtx);

    if (!RobloxGui->FindFirstChild("DropDownFullscreenFrame")) { // If the player is joining the game
        std::cout << "Waiting for " + Username + " to join the game\n";
        RobloxGui->WaitForChildAddress("DropDownFullscreenFrame");
        if (DataModel.Name() == "App") { // In case the player leaves the join page
            //PatchScript->SetBytecode(Compile("coroutine.wrap(function(...)" + clientScript + "\nend)();" + PatchScriptSource));
            return;
        }
        std::cout << "Joined, Waiting 2.5 seconds for " + Username + " to load\n";
        Sleep(2500);
    }

    std::unique_ptr<Instance> Jest = nullptr;
    {
        auto CorePackages = DataModel.FindFirstChild("CorePackages");
        if (!CorePackages) {
            return;
        }

        auto Packages = CorePackages->FindFirstChild("Packages");
        if (!Packages) {
            return;
        }

        auto Index = Packages->FindFirstChild("_Index");
        if (!Index) {
            return;
        }

        auto CollisionMatchers2D_1 = Index->FindFirstChild("CollisionMatchers2D");
        if (!CollisionMatchers2D_1) {
            return;
        }

        auto CollisionMatchers2D_2 = CollisionMatchers2D_1->FindFirstChild("CollisionMatchers2D");
        if (!CollisionMatchers2D_2) {
            return;
        }

        Jest = CollisionMatchers2D_2->FindFirstChild("Jest");
    }

    uintptr_t base = GetBase(processID);
    write_memory<BYTE>(base + offsets::EnableLoadModule, 1, handle);

    //AvatarEditorPrompts->UnlockModule();
    write_memory<std::uintptr_t>(PatchScript->Self() + offsets::This, Jest->Self(), handle);

    Jest->SetBytecode(Compile(clientScript), true);

    HWND old = GetForegroundWindow();
    while (GetForegroundWindow() != clientHWND) {
        SetForegroundWindow(clientHWND);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    keybd_event(VK_ESCAPE, MapVirtualKey(VK_ESCAPE, 0), KEYEVENTF_SCANCODE, 0);
    keybd_event(VK_ESCAPE, MapVirtualKey(VK_ESCAPE, 0), KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP, 0);
    Sleep(100);
    if (old != nullptr)
        SetForegroundWindow(old);
    Sleep(800);

    write_memory<std::uintptr_t>(PatchScript->Self() + offsets::This, PatchScript->Self(), handle);

    if (Username == "Unknown") { // Atleast the username dosen't have to be "Unknown" and we can try to change that
        std::uintptr_t newLocalPlayerAddr = read_memory<std::uintptr_t>(DataModel.FindFirstChildOfClassAddress("Players") + offsets::LocalPlayer, handle);
        Instance newLocalPlayer = Instance(newLocalPlayerAddr, handle);
        Username = newLocalPlayer.Name();
        if (Username == "")
            Username = "Unknown";
    }
}

void RBXClient::execute(const std::string& source) const {
    std::uintptr_t dataModelAddr = FetchDataModel();
    if (!dataModelAddr) return;

    Instance DataModel(dataModelAddr, handle);

    auto ReplicatedStorage = DataModel.FindFirstChildOfClass("RobloxReplicatedStorage");
    auto xenoFolder = ReplicatedStorage->FindFirstChild("Xeno");
    auto xenoScripts = xenoFolder->FindFirstChild("Scripts");
    auto moduleScript = xenoScripts->FindFirstChildOfClass("ModuleScript");

    std::string compiledBytecode = Compile(
        "return {['x e n o']=function(...)"
        "do local function s(i,v)getfenv(debug.info(0,'f'))[i]=v;"
        "getfenv(debug.info(1,'f'))[i]=v;end;"
        "for i,v in pairs(getfenv(debug.info(1,'f')))do s(i,v)end;"
        "setmetatable(getgenv(),{__newindex=function(t,i,v)rawset(t,i,v)s(i,v)end})end;"
        + source + "\nend}"
    );

    moduleScript->SetBytecode(compiledBytecode, true);
    moduleScript->UnlockModule();
}


bool RBXClient::loadstring(const std::string& source, const std::string& script_name, const std::string& chunk_name) const {
    std::uintptr_t dataModel_Address = FetchDataModel();

    Instance DataModel(dataModel_Address, handle);
    auto RobloxReplicatedStorage = DataModel.FindFirstChildOfClass("RobloxReplicatedStorage");
    if (!RobloxReplicatedStorage)
        return false;

    auto xenoFolder = RobloxReplicatedStorage->FindFirstChild("Xeno");
    if (!xenoFolder)
        return false;

    auto cloned_module = xenoFolder->FindFirstChild(script_name);
    if (!cloned_module)
        return false;

    cloned_module->SetBytecode(Compile("return{[ [[" + chunk_name + "]] ]=function(...)do local function s(i, v)getfenv(debug.info(0, 'f'))[i] = v;getfenv(debug.info(1, 'f'))[i] = v;end;for i,v in pairs(getfenv(debug.info(1,'f')))do s(i, v)end;setmetatable(getgenv and getgenv()or{},{__newindex=function(t,i,v)rawset(t,i,v)s(i,v)end})end;" + source + "\nend}"), true);

    cloned_module->UnlockModule();

    return true;
}

std::uintptr_t RBXClient::GetObjectValuePtr(const std::string_view objectval_name) const
{
    std::uintptr_t dataModel_Address = FetchDataModel();

    Instance DataModel(dataModel_Address, handle);
    auto RobloxReplicatedStorage = DataModel.FindFirstChildOfClass("RobloxReplicatedStorage");

    auto xenoFolder = RobloxReplicatedStorage->FindFirstChild("Xeno");
    if (!xenoFolder)
        return 0;

    auto objectValContainer = xenoFolder->FindFirstChild("Instance Pointers");
    if (!objectValContainer)
        return 0;

    std::uintptr_t objectValue = objectValContainer->FindFirstChildAddress(objectval_name);
    if (objectValue == 0)
        return 0;

    return read_memory<std::uintptr_t>(objectValue + offsets::ObjectValue, handle);
}

std::vector<DWORD> GetProcessIDsByName(const std::wstring_view processName) {
    std::vector<DWORD> processIDs;

    HANDLE snapShot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, NULL);
    if (snapShot == INVALID_HANDLE_VALUE)
        return processIDs;

    PROCESSENTRY32W entry = { sizeof(PROCESSENTRY32W) };

    if (Process32FirstW(snapShot, &entry)) {
        if (_wcsicmp(processName.data(), entry.szExeFile) == 0) {
            processIDs.push_back(entry.th32ProcessID);
        }
        while (Process32NextW(snapShot, &entry)) {
            if (_wcsicmp(processName.data(), entry.szExeFile) == 0) {
                processIDs.push_back(entry.th32ProcessID);
            }
        }
    }

    CloseHandle(snapShot);
    return processIDs;
}



static std::string compress(const std::string_view bytecode)
{
    const auto data_size = bytecode.size();
    const auto max_size = ZSTD_compressBound(data_size);
    auto buffer = std::vector<char>(max_size + 8);

    strcpy_s(&buffer[0], buffer.capacity(), "RSB1");
    memcpy_s(&buffer[4], buffer.capacity(), &data_size, sizeof(data_size));

    const auto compressed_size = ZSTD_compress(&buffer[8], max_size, bytecode.data(), data_size, ZSTD_maxCLevel());
    if (ZSTD_isError(compressed_size))
        return "";

    const auto size = compressed_size + 8;
    const auto key = XXH32(buffer.data(), size, 42u);
    const auto bytes = reinterpret_cast<const uint8_t*>(&key);

    for (auto i = 0u; i < size; ++i)
        buffer[i] ^= bytes[i % 4] + i * 41u;

    return std::string(buffer.data(), size);
}

std::string decompress(const std::string_view compressed) {
    const uint8_t bytecodeSignature[4] = { 'R', 'S', 'B', '1' };
    const int bytecodeHashMultiplier = 41;
    const int bytecodeHashSeed = 42;

    if (compressed.size() < 8)
        return "Compressed data too short";

    std::vector<uint8_t> compressedData(compressed.begin(), compressed.end());
    std::vector<uint8_t> headerBuffer(4);

    for (size_t i = 0; i < 4; ++i) {
        headerBuffer[i] = compressedData[i] ^ bytecodeSignature[i];
        headerBuffer[i] = (headerBuffer[i] - i * bytecodeHashMultiplier) % 256;
    }

    for (size_t i = 0; i < compressedData.size(); ++i) {
        compressedData[i] ^= (headerBuffer[i % 4] + i * bytecodeHashMultiplier) % 256;
    }

    uint32_t hashValue = 0;
    for (size_t i = 0; i < 4; ++i) {
        hashValue |= headerBuffer[i] << (i * 8);
    }

    uint32_t rehash = XXH32(compressedData.data(), compressedData.size(), bytecodeHashSeed);
    if (rehash != hashValue)
        return "Hash mismatch during decompression";

    uint32_t decompressedSize = 0;
    for (size_t i = 4; i < 8; ++i) {
        decompressedSize |= compressedData[i] << ((i - 4) * 8);
    }

    compressedData = std::vector<uint8_t>(compressedData.begin() + 8, compressedData.end());
    std::vector<uint8_t> decompressed(decompressedSize);

    size_t const actualDecompressedSize = ZSTD_decompress(decompressed.data(), decompressedSize, compressedData.data(), compressedData.size());
    if (ZSTD_isError(actualDecompressedSize))
        return "ZSTD decompression error: " + std::string(ZSTD_getErrorName(actualDecompressedSize));

    decompressed.resize(actualDecompressedSize);
    return std::string(decompressed.begin(), decompressed.end());
}

static constexpr uint32_t MAGIC_A = 0x4C464F52;
static constexpr uint32_t MAGIC_B = 0x946AC432;
static constexpr uint8_t  KEY_BYTES[4] = { 0x52, 0x4F, 0x46, 0x4C };

static inline uint8_t rotl8(uint8_t value, int shift) {
    shift &= 7;
    return (value << shift) | (value >> (8 - shift));
}

static std::string sign_bytecode(const std::string& bytecode) {
    if (bytecode.empty()) {
        return "";
    }
    constexpr uint32_t FOOTER_SIZE = 40u;
    std::vector<uint8_t> blake3_hash(32);
    {
        blake3_hasher hasher;
        blake3_hasher_init(&hasher);
        blake3_hasher_update(&hasher, bytecode.data(), bytecode.size());
        blake3_hasher_finalize(&hasher, blake3_hash.data(), blake3_hash.size());
    }
    std::vector<uint8_t> transformed_hash(32);
    for (int i = 0; i < 32; ++i) {
        uint8_t byte = KEY_BYTES[i & 3];
        uint8_t hash_byte = blake3_hash[i];
        uint8_t combined = byte + i;
        uint8_t result;
        switch (i & 3) {
        case 0: {
            int shift = ((combined & 3) + 1);
            result = rotl8(hash_byte ^ ~byte, shift);
            break;
        }
        case 1: {
            int shift = ((combined & 3) + 2);
            result = rotl8(byte ^ ~hash_byte, shift);
            break;
        }
        case 2: {
            int shift = ((combined & 3) + 3);
            result = rotl8(hash_byte ^ ~byte, shift);
            break;
        }
        case 3: {
            int shift = ((combined & 3) + 4);
            result = rotl8(byte ^ ~hash_byte, shift);
            break;
        }
        }
        transformed_hash[i] = result;
    }
    std::vector<uint8_t> footer(FOOTER_SIZE, 0);
    uint32_t first_hash_dword = *reinterpret_cast<uint32_t*>(transformed_hash.data());
    uint32_t footer_prefix = first_hash_dword ^ MAGIC_B;
    memcpy(&footer[0], &footer_prefix, 4);
    uint32_t xor_ed = first_hash_dword ^ MAGIC_A;
    memcpy(&footer[4], &xor_ed, 4);
    memcpy(&footer[8], transformed_hash.data(), 32);
    std::string signed_bytecode = bytecode;
    signed_bytecode.append(reinterpret_cast<const char*>(footer.data()), footer.size());
    return compress(signed_bytecode);
}

std::string compilable(const std::string& source, bool returnBytecode) {
    static bytecode_encoder_t encoder = bytecode_encoder_t();
    std::string bytecode = Luau::compile(source, {}, {}, &encoder);
    if (bytecode[0] == '\0') {
        bytecode.erase(std::remove(bytecode.begin(), bytecode.end(), '\0'), bytecode.end());
        return bytecode;
    }
    if (returnBytecode) {
        return bytecode;
    }
    return "success";
}

std::string Compile(const std::string& source)
{
    static bytecode_encoder_t encoder = bytecode_encoder_t();
    const std::string bytecode = Luau::compile(source, {}, {}, &encoder);

    if (bytecode[0] == '\0') {
        std::string bytecodeP = bytecode;
        bytecodeP.erase(std::remove(bytecodeP.begin(), bytecodeP.end(), '\0'), bytecodeP.end());
        std::cerr << "Byecode compile failed: " << bytecodeP << std::endl;
    }

    return sign_bytecode(bytecode);
}

static BOOL CALLBACK EnumWindowsProcMy(HWND hwnd, LPARAM lParam)
{
    DWORD lpdwProcessId;
    GetWindowThreadProcessId(hwnd, &lpdwProcessId);

    std::pair<DWORD, HWND*>* params = reinterpret_cast<std::pair<DWORD, HWND*>*>(lParam);
    DWORD targetProcessId = params->first;
    HWND* resultHWND = params->second;

    if (lpdwProcessId == targetProcessId)
    {
        *resultHWND = hwnd;
        return FALSE;
    }

    return TRUE;
}

HWND GetHWNDFromPID(DWORD process_id) {
    HWND hwnd = nullptr;
    std::pair<DWORD, HWND*> params(process_id, &hwnd);

    EnumWindows(EnumWindowsProcMy, reinterpret_cast<LPARAM>(&params));

    return hwnd;
}

template<typename T>
T read_memory(std::uintptr_t address, HANDLE handle)
{
    T value = 0;
    MEMORY_BASIC_INFORMATION bi;

    VirtualQueryEx(handle, reinterpret_cast<LPCVOID>(address), &bi, sizeof(bi));

    NtReadVirtualMemory(handle, reinterpret_cast<LPCVOID>(address), &value, sizeof(value), nullptr);

    PVOID baddr = bi.AllocationBase;
    SIZE_T size = bi.RegionSize;
    NtUnlockVirtualMemory(handle, &baddr, &size, 1);

    return value;
}

template <typename T>
bool write_memory(std::uintptr_t address, const T& value, HANDLE handle)
{
    SIZE_T bytesWritten;
    DWORD oldProtection;

    if (!VirtualProtectEx(handle, reinterpret_cast<LPVOID>(address), sizeof(value), PAGE_READWRITE, &oldProtection)) {
        return false;
    }

    if (NtWriteVirtualMemory(handle, reinterpret_cast<PVOID>(address), (PVOID)&value, sizeof(value), &bytesWritten) || bytesWritten != sizeof(value)) {
        return false;
    }

    DWORD d;
    if (!VirtualProtectEx(handle, reinterpret_cast<LPVOID>(address), sizeof(value), oldProtection, &d)) {
        return false;
    }

    return true;
}
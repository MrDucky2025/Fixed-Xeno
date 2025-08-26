#include <future>
#include "worker.hpp"
#include "server/server.h"
#include "utils/ntdll.h"

extern std::vector<std::shared_ptr<RBXClient>> Clients;
std::mutex clientsMtx;

std::unordered_set<DWORD> closedClients;
std::unordered_set<DWORD> initializingClients;

static void newClient(DWORD pid) {
    if (closedClients.find(pid) != closedClients.end())
        return;

    auto client = std::make_shared<RBXClient>(pid);

    {
        std::lock_guard<std::mutex> lock(clientsMtx);
        Clients.push_back(client);
        initializingClients.erase(pid);
    }
}

static void init() {
    DWORD currentPID = GetCurrentProcessId();
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);

    std::vector<DWORD> xenoPIDs = GetProcessIDsByName(wcsrchr(path, L'\\') + 1);

    // Terminate existing Xeno processes
    for (DWORD pid : xenoPIDs) {
        if (pid == currentPID)
            continue;
        HANDLE hXeno = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
        if (hXeno)
            TerminateProcess(hXeno, 0);
    }

    std::unordered_set<DWORD> knownPIDs;

    while (true) {
        std::vector<DWORD> clientPIDs = GetProcessIDsByName(L"RobloxPlayerBeta.exe");
        std::vector<DWORD> shaderPIDs = GetProcessIDsByName(L"eurotrucks2.exe");

        clientPIDs.insert(clientPIDs.end(), shaderPIDs.begin(), shaderPIDs.end());

        std::unordered_set<DWORD> currentPIDs(clientPIDs.begin(), clientPIDs.end());

        {
            std::lock_guard<std::mutex> lock(clientsMtx);

            // Remove closed clients
            Clients.erase(std::remove_if(Clients.begin(), Clients.end(),
                [&](const std::shared_ptr<RBXClient>& client) {
                    if (currentPIDs.find(client->PID) == currentPIDs.end() || !client->isProcessAlive()) {
                        closedClients.insert(client->PID);
                        return true;
                    }
                    return false;
                }), Clients.end());
        }

        // Launch new clients outside lock
        for (DWORD pid : clientPIDs) {
            if (knownPIDs.find(pid) == knownPIDs.end() &&
                initializingClients.find(pid) == initializingClients.end() &&
                closedClients.find(pid) == closedClients.end())
            {
                initializingClients.insert(pid);
                std::thread(newClient, pid).detach();
                knownPIDs.insert(pid);
            }
        }

        Sleep(500); // Reduced polling frequency to save CPU
    }
}

extern "C" {
    __declspec(dllexport) ClientInfo* GetClients() {
        static std::vector<ClientInfo> simpleClients;
        {
            std::lock_guard<std::mutex> lock(clientsMtx);
            simpleClients.clear();

            for (const auto& client : Clients) {
                simpleClients.push_back({ client->Version.c_str(), client->Username.c_str(), static_cast<int>(client->PID) });
            }

            simpleClients.push_back({ nullptr, nullptr, 0 });
        }
        return simpleClients.data();
    }

    __declspec(dllexport) void Initialize() {
        HMODULE ntdll = LoadLibraryA("ntdll.dll");
        if (!ntdll)
            return;

        NTDLL_INIT_FCNS(ntdll);

        std::thread(init).detach();
        std::thread(setup_connection).detach();
    }

    __declspec(dllexport) void Execute(const char* script_source, const char** client_users, int num_users) {
        std::string source(script_source);

        std::unordered_map<std::string, std::shared_ptr<RBXClient>> usernameMap;
        {
            std::lock_guard<std::mutex> lock(clientsMtx);
            for (const auto& client : Clients) {
                usernameMap[client->Username] = client;
            }
        }

        for (int i = 0; i < num_users; ++i) {
            auto it = usernameMap.find(client_users[i]);
            if (it != usernameMap.end()) {
                it->second->execute(source);
            }
        }
    }


    __declspec(dllexport) const char* Compilable(const char* script_source) {
        std::string source(script_source);
        std::string result = compilable(source);
        char* result_cstr = new char[result.length() + 1];
        strcpy_s(result_cstr, result.length() + 1, result.c_str());
        return result_cstr;
    }
}

// Made by ente0216
// 
// Happy Skidding
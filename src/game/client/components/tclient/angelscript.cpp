#include "angelscript.h"

#include <base/hash.h>
#include <base/str.h>
#include <base/log.h>

#include <engine/console.h>
#include <engine/shared/config.h>
#include <engine/storage.h>

#include <game/client/component.h>
#include <game/client/gameclient.h>

#include <angelscript.h>

#include <algorithm>
#include <cstring>
#include <exception>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

class CAngelScriptRunner : CComponentInterfaces
{
private:
    const char *m_pFilename;
    std::string m_Args;

    static CAngelScriptRunner *s_pActiveRunner;

    struct CBytecodeStream : public asIBinaryStream
    {
        std::vector<unsigned char> *m_pOut{};
        const unsigned char *m_pIn{};
        size_t m_InSize{};
        size_t m_Offset{};

        // Write from engine into m_pOut
        int Write(const void *ptr, asUINT size) override
        {
            if(!m_pOut)
                return asERROR;
            const unsigned char *p = static_cast<const unsigned char *>(ptr);
            m_pOut->insert(m_pOut->end(), p, p + size);
            return asSUCCESS;
        }

        // Read from m_pIn into engine
        int Read(void *ptr, asUINT size) override
        {
            if(!m_pIn)
                return asERROR;
            if(m_Offset + size > m_InSize)
                return asERROR;
            std::memcpy(ptr, m_pIn + m_Offset, size);
            m_Offset += size;
            return asSUCCESS;
        }
    };

    struct CCacheEntry
    {
        SHA256_DIGEST m_Hash{};
        std::vector<unsigned char> m_Bytecode;
    };

    static std::unordered_map<std::string, CCacheEntry> s_Cache;

    const CServerInfo *GetServerInfo()
    {
        if(Client()->State() == IClient::STATE_ONLINE || Client()->State() == IClient::STATE_DEMOPLAYBACK)
        {
            static CServerInfo s_ServerInfo; // Prevent use after stack return
            Client()->GetServerInfo(&s_ServerInfo);
            return &s_ServerInfo;
        }
        else if(GameClient()->m_ConnectServerInfo)
        {
            return &*GameClient()->m_ConnectServerInfo;
        }
        return nullptr;
    }

    // Minimal std::string support for AngelScript
    class CStringFactory : public asIStringFactory
    {
    public:
        const void *GetStringConstant(const char *data, asUINT length) override
        {
            std::string *p = new std::string(data, data + length);
            m_RefCounts[p] = 1;
            return p;
        }
        int ReleaseStringConstant(const void *str) override
        {
            auto *p = const_cast<std::string *>(static_cast<const std::string *>(str));
            auto it = m_RefCounts.find(p);
            if(it == m_RefCounts.end())
                return asERROR;
            if(--it->second == 0)
            {
                m_RefCounts.erase(it);
                delete p;
            }
            return asSUCCESS;
        }
        int GetRawStringData(const void *str, char *data, asUINT *length) const override
        {
            auto *p = static_cast<const std::string *>(str);
            if(length)
                *length = (asUINT)p->size();
            if(data)
                std::memcpy(data, p->data(), p->size());
            return asSUCCESS;
        }

    private:
        std::unordered_map<std::string *, int> m_RefCounts;
    };

    static void DefaultConstructString(std::string *self)
    {
        new(self) std::string();
    }
    static void CopyConstructString(const std::string &other, std::string *self)
    {
        new(self) std::string(other);
    }
    static void DestructString(std::string *self)
    {
        self->~basic_string();
    }
    static std::string &AssignString(const std::string &other, std::string *self)
    {
        *self = other;
        return *self;
    }

    static void MessageCallback(const asSMessageInfo *msg, void *param)
    {
        const char *type = "INFO";
        if(msg->type == asMSGTYPE_WARNING)
            type = "WARN";
        else if(msg->type == asMSGTYPE_INFORMATION)
            type = "INFO";
        else if(msg->type == asMSGTYPE_ERROR)
            type = "ERROR";

        log_error("angelscript/%s", "%s (%d, %d): %s", type, msg->section ? msg->section : "<section>", msg->row, msg->col, msg->message ? msg->message : "");
    }

    // Global functions exposed to script (use s_pActiveRunner)
    static void ASPrint(const std::string &Str)
    {
        log_info("angelscript/print", "%s", Str.c_str());
    }
    static void ASPuts(const std::string &Str)
    {
        log_info("angelscript/puts", "%s", Str.c_str());
    }
    static void ASExec(const std::string &Str)
    {
        if(s_pActiveRunner)
            s_pActiveRunner->Console()->ExecuteLine(Str.c_str());
    }

    std::string StateStr(const std::string &Str)
    {
        if(Str == "game_mode")
        {
            return std::string(GameClient()->m_GameInfo.m_aGameType);
        }
        else if(Str == "game_mode_pvp")
        {
            return GameClient()->m_GameInfo.m_Pvp ? "true" : "false";
        }
        else if(Str == "game_mode_race")
        {
            return GameClient()->m_GameInfo.m_Race ? "true" : "false";
        }
        else if(Str == "eye_wheel_allowed")
        {
            return GameClient()->m_GameInfo.m_AllowEyeWheel ? "true" : "false";
        }
        else if(Str == "zoom_allowed")
        {
            return GameClient()->m_GameInfo.m_AllowZoom ? "true" : "false";
        }
        else if(Str == "dummy_allowed")
        {
            return Client()->DummyAllowed() ? "true" : "false";
        }
        else if(Str == "dummy_connected")
        {
            return Client()->DummyConnected() ? "true" : "false";
        }
        else if(Str == "rcon_authed")
        {
            return Client()->RconAuthed() ? "true" : "false";
        }
        else if(Str == "team")
        {
            return std::to_string(GameClient()->m_aClients[GameClient()->m_aLocalIds[g_Config.m_ClDummy]].m_Team);
        }
        if(Str == "ddnet_team")
        {
            return std::to_string(GameClient()->m_Teams.Team(GameClient()->m_aLocalIds[g_Config.m_ClDummy]));
        }
        if(Str == "map")
        {
            if(Client()->State() == IClient::STATE_ONLINE || Client()->State() == IClient::STATE_DEMOPLAYBACK)
                return std::string(Client()->GetCurrentMap());
            else if(GameClient()->m_ConnectServerInfo)
                return std::string(GameClient()->m_ConnectServerInfo->m_aMap);
            else
                return std::string();
        }
        else if(Str == "server_ip")
        {
            const NETADDR *pAddress = nullptr;
            if(Client()->State() == IClient::STATE_ONLINE)
                pAddress = &Client()->ServerAddress();
            else if(GameClient()->m_ConnectServerInfo)
                pAddress = &GameClient()->m_ConnectServerInfo->m_aAddresses[0];
            else
                return std::string();
            char Addr[128];
            net_addr_str(pAddress, Addr, sizeof(Addr), true);
            return std::string(Addr);
        }
        else if(Str == "players_connected")
        {
            return std::to_string(GameClient()->m_Snap.m_NumPlayers);
        }
        else if(Str == "players_cap")
        {
            const CServerInfo *pServerInfo = GetServerInfo();
            if(!pServerInfo)
                return std::string();
            return std::to_string(pServerInfo->m_MaxClients);
        }
        else if(Str == "server_name")
        {
            const CServerInfo *pServerInfo = GetServerInfo();
            if(!pServerInfo)
                return std::string();
            return std::string(pServerInfo->m_aName);
        }
        else if(Str == "community")
        {
            const CServerInfo *pServerInfo = GetServerInfo();
            if(!pServerInfo)
                return std::string();
            return std::string(pServerInfo->m_aCommunityId);
        }
        else if(Str == "location")
        {
            if(GameClient()->m_GameInfo.m_Race)
                return std::string();

            float x = 0.0f, y = 0.0f;
            float w = 0.0f, h = 0.0f;
            const CLayers* pLayers = GameClient()->m_MapLayersBackground.m_pLayers;
            const CMapItemLayerTilemap* pLayer = pLayers->GameLayer();
            if(pLayer)
            {
                w = (float)pLayer->m_Width * 30.0f;
                h = (float)pLayer->m_Height * 30.0f;
            }
            x = GameClient()->m_Camera.m_Center.x;
            y = GameClient()->m_Camera.m_Center.y;
            static const char *s_apLocations[] = {
                "NW", "N", "NE",
                "W", "C", "E",
                "SW", "S", "SE"};
            int i = std::clamp((int)(y / h * 3.0f), 0, 2) * 3 + std::clamp((int)(x / w * 3.0f), 0, 2);
            return std::string(s_apLocations[i]);
        }
        else if(Str == "state")
        {
            const char *pState = nullptr;
            switch(Client()->State())
            {
            case IClient::EClientState::STATE_CONNECTING:
                pState = "connecting";
                break;
            case IClient::STATE_OFFLINE:
                pState = "offline";
                break;
            case IClient::STATE_LOADING:
                pState = "loading";
                break;
            case IClient::STATE_ONLINE:
                pState = "online";
                break;
            case IClient::STATE_DEMOPLAYBACK:
                pState = "demo";
                break;
            case IClient::STATE_QUITTING:
                pState = "quitting";
                break;
            case IClient::STATE_RESTARTING:
                pState = "restarting";
                break;
            }
            return std::string(pState ? pState : "");
        }
        throw std::invalid_argument(std::string("No state with name ") + Str);
    }

    void AddGlobals(asIScriptEngine *pEngine)
    {
        // Register string type and factory
        pEngine->RegisterObjectType("string", sizeof(std::string), asOBJ_VALUE | asGetTypeTraits<std::string>());
        pEngine->RegisterObjectBehaviour("string", asBEHAVE_CONSTRUCT, "void f()", asFUNCTION(DefaultConstructString), asCALL_CDECL_OBJLAST);
        pEngine->RegisterObjectBehaviour("string", asBEHAVE_CONSTRUCT, "void f(const string &in)", asFUNCTION(CopyConstructString), asCALL_CDECL_OBJLAST);
        pEngine->RegisterObjectBehaviour("string", asBEHAVE_DESTRUCT, "void f()", asFUNCTION(DestructString), asCALL_CDECL_OBJLAST);
        pEngine->RegisterObjectMethod("string", "string &opAssign(const string &in)", asFUNCTION(AssignString), asCALL_CDECL_OBJLAST);

        static CStringFactory s_StringFactory;
        pEngine->RegisterStringFactory("string", &s_StringFactory);

        // Global functions
        pEngine->RegisterGlobalFunction("void print(const string &in)", asFUNCTION(ASPrint), asCALL_CDECL);
        pEngine->RegisterGlobalFunction("void puts(const string &in)", asFUNCTION(ASPuts), asCALL_CDECL);
        pEngine->RegisterGlobalFunction("void exec(const string &in)", asFUNCTION(ASExec), asCALL_CDECL);

        struct Local
        {
            static std::string Trampoline(const std::string &Str)
            {
                if(CAngelScriptRunner::s_pActiveRunner)
                    return CAngelScriptRunner::s_pActiveRunner->StateStr(Str);
                return std::string();
            }
        };
        pEngine->RegisterGlobalFunction("string state(const string &in)", asFUNCTION(Local::Trampoline), asCALL_CDECL);

        // args as global property
        pEngine->RegisterGlobalProperty("string args", &m_Args);
    }

public:
    CAngelScriptRunner(CGameClient *pClient, const char *pFilename, const char *pArgs) :
        m_pFilename(pFilename), m_Args(pArgs ? pArgs : "")
    {
        OnInterfacesInit(pClient);
    }

    bool Run()
    {
        if(!m_pFilename || !*m_pFilename)
            return false;

        char *pScript = Storage()->ReadFileStr(m_pFilename, IStorage::TYPE_ALL);
        if(!pScript)
        {
            log_error("angelscript", "Failed to open script '%s'", m_pFilename);
            return false;
        }
        const size_t ScriptLen = str_length(pScript);

        bool Success = true;

        asIScriptEngine *pEngine = asCreateScriptEngine();
        if(!pEngine)
        {
            log_error("angelscript", "Failed to create script engine");
            free(pScript);
            return false;
        }

        pEngine->SetMessageCallback(asFUNCTION(MessageCallback), 0, asCALL_CDECL);

        try
        {
            // Set active runner so our global functions can access interfaces
            s_pActiveRunner = this;

            AddGlobals(pEngine);

            // Module name derived from filename
            std::string ModName = std::string("mod:") + m_pFilename;
            for(char &c : ModName)
                if(c == '/' || c == '\\')
                    c = '_';

            asIScriptModule *pMod = pEngine->GetModule(ModName.c_str(), asGM_ALWAYS_CREATE);
            if(!pMod)
            {
                log_error("angelscript", "Failed to create module");
                Success = false;
            }
            else
            {
                // Cache key is filename
                std::string Key = m_pFilename;
                SHA256_DIGEST CurHash = sha256(pScript, ScriptLen);

                auto it = s_Cache.find(Key);
                bool LoadedFromCache = false;
                if(it != s_Cache.end() && it->second.m_Hash == CurHash && !it->second.m_Bytecode.empty())
                {
                    CBytecodeStream In{};
                    In.m_pIn = it->second.m_Bytecode.data();
                    In.m_InSize = it->second.m_Bytecode.size();
                    if(pMod->LoadByteCode(&In) >= 0)
                    {
                        LoadedFromCache = true;
                        log_info("angelscript", "Loaded bytecode for '%s' from cache", m_pFilename);
                    }
                    else
                    {
                        // Fall back to rebuild
                        pEngine->DiscardModule(ModName.c_str());
                        pMod = pEngine->GetModule(ModName.c_str(), asGM_ALWAYS_CREATE);
                    }
                }

                if(!LoadedFromCache)
                {
                    if(pMod->AddScriptSection(m_pFilename, pScript, ScriptLen) < 0)
                    {
                        log_error("angelscript", "AddScriptSection failed for '%s'", m_pFilename);
                        Success = false;
                    }
                    else if(pMod->Build() < 0)
                    {
                        log_error("angelscript", "Build failed for '%s'", m_pFilename);
                        Success = false;
                    }
                    else
                    {
                        // Save bytecode
                        CBytecodeStream Out{};
                        Out.m_pOut = &s_Cache[Key].m_Bytecode;
                        s_Cache[Key].m_Bytecode.clear();
                        if(pMod->SaveByteCode(&Out) >= 0)
                        {
                            s_Cache[Key].m_Hash = CurHash;
                        }
                    }
                }

                if(Success)
                {
                    // Find entry function
                    const char *Candidates[] = {"void main()", "void run()", "void tclient()"};
                    asIScriptFunction *pFunc = nullptr;
                    for(const char *Decl : Candidates)
                    {
                        pFunc = pMod->GetFunctionByDecl(Decl);
                        if(pFunc)
                            break;
                    }
                    if(!pFunc)
                    {
                        log_error("angelscript", "No entry function found in '%s' (expected one of: void main(), void run(), void tclient())", m_pFilename);
                        Success = false;
                    }
                    else
                    {
                        asIScriptContext *pCtx = pEngine->CreateContext();
                        if(!pCtx)
                        {
                            log_error("angelscript", "Failed to create context for '%s'", m_pFilename);
                            Success = false;
                        }
                        else
                        {
                            if(pCtx->Prepare(pFunc) < 0)
                            {
                                log_error("angelscript", "Prepare failed for '%s'", m_pFilename);
                                Success = false;
                            }
                            else
                            {
                                int r = pCtx->Execute();
                                if(r != asEXECUTION_FINISHED)
                                {
                                    if(r == asEXECUTION_EXCEPTION)
                                    {
                                        const char *Ex = pCtx->GetExceptionString();
                                        const char *Fn = pCtx->GetExceptionFunction() ? pCtx->GetExceptionFunction()->GetDeclaration() : "<fn>";
                                        log_error("angelscript", "Exception in '%s': %s at %s:%d", m_pFilename, Ex ? Ex : "<exception>", Fn, pCtx->GetExceptionLineNumber());
                                    }
                                    else
                                    {
                                        log_error("angelscript", "Execution failed in '%s' (code %d)", m_pFilename, r);
                                    }
                                    Success = false;
                                }
                            }
                            pCtx->Release();
                        }
                    }
                }
            }

            s_pActiveRunner = nullptr;
        }
        catch(const std::exception &e)
        {
            log_error("angelscript", "Exception in '%s': %s", m_pFilename, e.what());
            Success = false;
        }
        catch(...)
        {
            log_error("angelscript", "Unknown exception in '%s'", m_pFilename);
            Success = false;
        }

        pEngine->ShutDownAndRelease();
        free(pScript);
        return Success;
    }
};

CAngelScriptRunner *CAngelScriptRunner::s_pActiveRunner = nullptr;
std::unordered_map<std::string, CAngelScriptRunner::CCacheEntry> CAngelScriptRunner::s_Cache{};

void CAngelScript::ConExecScript(IConsole::IResult *pResult, void *pUserData)
{
    CAngelScript *pThis = static_cast<CAngelScript *>(pUserData);
    pThis->ExecScript(pResult->GetString(0), pResult->GetString(1));
}

bool CAngelScript::ExecScript(const char *pFilename, const char *pArgs)
{
    CAngelScriptRunner Runner(GameClient(), pFilename, pArgs);
    return Runner.Run();
}

void CAngelScript::OnConsoleInit()
{
    Console()->Register("angel", "s[file] ?r[args]", CFGFLAG_CLIENT, ConExecScript, this, "Execute an AngelScript module");
}

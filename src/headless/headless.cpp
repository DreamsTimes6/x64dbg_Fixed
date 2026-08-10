#include "../bridge/bridgemain.h"
#include "../bridge/startupargs.h"
#include "../dbg/_plugins.h"
#include "../dbg/concurrentqueue/blockingconcurrentqueue.h"

#include "tostring.h"

#include <atomic>
#include <cstdio>
#include <functional>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

#include "stringutils.h"

#define Cmd(x) DbgCmdExecDirect(x)
#define Eval(x) DbgValFromString(x)
#define dprintf(x, ...) _plugin_logprintf("[" PLUGIN_NAME "] " x, __VA_ARGS__)
#define dputs(x) _plugin_logprintf("[" PLUGIN_NAME "] %s\n", x)

static std::vector<SCRIPTTYPEINFO> scriptInfo;
static int curScriptId = 0;
static bool dbgStopped = false;
static DWORD dwGuiThreadId = 0;
static moodycamel::BlockingConcurrentQueue<std::function<bool()>> queue;
static constexpr DWORD NoShutdownCtrlType = MAXDWORD;
static std::atomic<bool> shutdownRequested{ false };
static std::atomic<bool> consoleCloseRequested{ false };
static std::atomic<DWORD> shutdownCtrlType{ NoShutdownCtrlType };
static std::atomic<HANDLE> commandThreadHandle{ nullptr };
static std::mutex redirectLogMutex;
static FILE* redirectLogFile = nullptr;
static std::atomic<int> gLastDbgState{ (int)DBGSTATE::initialized };
static bool gHasCommandFile = false; // -cf was passed: wait for the script on stdin EOF

// When enabled (-rpc), all stdout traffic is JSON Lines: one request line on
// stdin, one or more JSON response/event lines on stdout. This gives an AI or
// scripted client a stable, parseable protocol while keeping the full x64dbg
// command surface (and plugins loaded via -plugin) available.
static bool gRpcMode = false;

static std::string jsonEscape(const std::string & input)
{
    std::string out;
    out.reserve(input.size() + 16);
    for(char ch : input)
    {
        switch(ch)
        {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if((unsigned char)ch < 0x20)
            {
                char buf[8] = "";
                sprintf_s(buf, "\\u%04x", (unsigned char)ch);
                out += buf;
            }
            else
                out += ch;
            break;
        }
    }
    return out;
}

// Extract the value of a top-level JSON key. String values are returned
// unescaped; bare values (true/false/numbers) are returned as their raw text.
// Returns false if the key is missing.
static bool jsonGetString(const std::string & json, const char* key, std::string & out)
{
    std::string needle = "\"";
    needle += key;
    needle += "\"";
    auto pos = json.find(needle);
    if(pos == std::string::npos)
        return false;
    auto colon = json.find(':', pos + needle.size());
    if(colon == std::string::npos)
        return false;

    auto valueStart = colon + 1;
    while(valueStart < json.size() && (json[valueStart] == ' ' || json[valueStart] == '\t'))
        valueStart++;
    if(valueStart >= json.size())
        return false;

    if(json[valueStart] == '"')
    {
        std::string val;
        bool escape = false;
        for(size_t i = valueStart + 1; i < json.size(); i++)
        {
            char ch = json[i];
            if(escape)
            {
                switch(ch)
                {
                case 'n':
                    val += '\n';
                    break;
                case 'r':
                    val += '\r';
                    break;
                case 't':
                    val += '\t';
                    break;
                case '\\':
                    val += '\\';
                    break;
                case '"':
                    val += '"';
                    break;
                default:
                    val += ch;
                    break;
                }
                escape = false;
            }
            else if(ch == '\\')
                escape = true;
            else if(ch == '"')
            {
                out = val;
                return true;
            }
            else
                val += ch;
        }
        return false;
    }

    // Bare value: read until the next ',' or '}'.
    auto end = json.find_first_of(",}", valueStart);
    if(end == std::string::npos)
        end = json.size();
    std::string raw = json.substr(valueStart, end - valueStart);
    while(!raw.empty() && (raw.front() == ' ' || raw.front() == '\t'))
        raw.erase(raw.begin());
    while(!raw.empty() && (raw.back() == ' ' || raw.back() == '\t' || raw.back() == '\r'))
        raw.pop_back();
    out = raw;
    return !out.empty();
}

static std::mutex rpcOutMutex;

static void rpcPrintJson(const char* format, ...)
{
    // Serialize the whole JSON line (format + newline) so concurrent log
    // producers from different debugger threads cannot interleave output.
    std::lock_guard<std::mutex> lock(rpcOutMutex);
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    putchar('\n');
}

static void rpcLog(const char* text)
{
    if(!gRpcMode || text == nullptr)
        return;
    rpcPrintJson("{\"log\":\"%s\"}", jsonEscape(text).c_str());
}

static void stopRedirectLog()
{
    std::lock_guard<std::mutex> lock(redirectLogMutex);
    if(redirectLogFile)
    {
        fclose(redirectLogFile);
        redirectLogFile = nullptr;
    }
}

static void startRedirectLog(const char* filename)
{
    stopRedirectLog();
    if(filename == nullptr || *filename == '\0')
        return;

    FILE* file = nullptr;
    if(_wfopen_s(&file, Utf8ToUtf16(filename).c_str(), L"ab") != 0 || file == nullptr)
    {
        printf("[headless] failed to redirect log to %s\n", filename);
        return;
    }

    std::lock_guard<std::mutex> lock(redirectLogMutex);
    redirectLogFile = file;
}

static void appendRedirectedLog(const char* text)
{
    if(text == nullptr || *text == '\0')
        return;

    std::lock_guard<std::mutex> lock(redirectLogMutex);
    if(redirectLogFile == nullptr)
        return;

    fwrite(text, 1, strlen(text), redirectLogFile);
    fflush(redirectLogFile);
}

static void requestShutdown()
{
    if(shutdownRequested.exchange(true))
        return;
    queue.enqueue([]()
    {
        return false;
    });
}

static const char* shutdownCtrlTypeToString(DWORD ctrlType)
{
    switch(ctrlType)
    {
    case CTRL_C_EVENT:
        return "Ctrl+C";
    case CTRL_BREAK_EVENT:
        return "Ctrl+Break";
    case CTRL_CLOSE_EVENT:
        return "console close";
    case CTRL_LOGOFF_EVENT:
        return "logoff";
    case CTRL_SHUTDOWN_EVENT:
        return "system shutdown";
    default:
        return nullptr;
    }
}

static BOOL WINAPI consoleCtrlHandler(DWORD ctrlType)
{
    switch(ctrlType)
    {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        shutdownCtrlType = ctrlType;
        consoleCloseRequested = true;
        requestShutdown();
        if(auto handle = commandThreadHandle.load())
            CancelSynchronousIo(handle);
        return TRUE;
    default:
        return FALSE;
    }
}

struct GuiState
{
    duint disasm = 0;
    duint cip = 0;
    duint dump = 0;
    duint stack = 0;
    duint csp = 0;
    duint graph = 0;
    duint memmap = 0;
    duint symmod = 0;
    std::string globalNotes;
    std::string debuggeeNotes;
} guistate;

extern "C" __declspec(dllexport) int _gui_guiinit(int argc, char* argv[])
{
    // Disable buffering for stdout and stderr to ensure immediate output
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);

    shutdownRequested = false;
    consoleCloseRequested = false;
    shutdownCtrlType = NoShutdownCtrlType;
    commandThreadHandle = nullptr;

    // Init debugger
    const char* errormsg = DbgInitBlocking();
    if(errormsg)
    {
        puts(errormsg);
        return 1;
    }
    SetConsoleCtrlHandler(consoleCtrlHandler, TRUE);

    if(!gRpcMode)
        puts("[headless] entering command loop...");
    std::thread commandThread([]
    {
        if(gRpcMode)
        {
            // JSON Lines RPC mode: one request per line on stdin, JSON
            // response/event lines on stdout. Supported requests:
            //   {"ping":true}                        -> {"ok":true,"pong":true}
            //   {"get":"state"}                      -> {"ok":true,"state":"...","isDebugging":bool}
            //   {"eval":"<expr>"}                    -> {"ok":true,"value":N,"hex":"0x..."}
            //   {"cmd":"<x64dbg command>","sync":bool} -> {"ok":true} (sync uses DbgCmdExecDirect and blocks until the command finishes)
            //   {"wait":"<state>","timeout":ms}      -> block until debugger state matches (e.g. "paused" after run)
            //   {"exit":true}                        -> shutdown
            rpcPrintJson("{\"hello\":\"headless-rpc\"}");
            while(!shutdownRequested.load())
            {
                std::string line;
                if(!std::getline(std::cin, line))
                {
                    requestShutdown();
                    break;
                }
                while(!line.empty() && (line.front() == ' ' || line.front() == '\t' || line.front() == '\r'))
                    line.erase(line.begin());
                while(!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r'))
                    line.pop_back();
                if(line.empty())
                    continue;

                std::string value;                if(jsonGetString(line, "ping", value))
                {
                    rpcPrintJson("{\"ok\":true,\"pong\":true}");
                }
                else if(jsonGetString(line, "exit", value) && (value == "true" || value == "1"))
                {
                    requestShutdown();
                    break;
                }
                else if(jsonGetString(line, "get", value))
                {
                    if(value == "state")
                    {
                        rpcPrintJson("{\"ok\":true,\"state\":\"%s\",\"isDebugging\":%s}",
                                     dbgstate2str((DBGSTATE)gLastDbgState.load()),
                                     DbgIsDebugging() ? "true" : "false");
                    }
                    else
                        rpcPrintJson("{\"ok\":false,\"err\":\"unknown get key '%s'\"}", jsonEscape(value).c_str());
                }
                else if(jsonGetString(line, "wait", value))
                {
                    // Block until the debugger reaches the requested state
                    // (e.g. {"wait":"paused"} after {"cmd":"run"}). Optional
                    // "timeout" key in milliseconds (default 30000).
                    std::string timeoutStr;
                    auto timeout = 30000;
                    if(jsonGetString(line, "timeout", timeoutStr))
                        timeout = atoi(timeoutStr.c_str());
                    auto deadline = GetTickCount() + timeout;
                    bool matched = false;
                    while(GetTickCount() < deadline)
                    {
                        if(strcmp(dbgstate2str((DBGSTATE)gLastDbgState.load()), value.c_str()) == 0)
                        {
                            matched = true;
                            break;
                        }
                        Sleep(50);
                    }
                    if(matched)
                        rpcPrintJson("{\"ok\":true,\"state\":\"%s\"}", jsonEscape(value).c_str());
                    else
                        rpcPrintJson("{\"ok\":false,\"err\":\"timeout waiting for state '%s'\"}", jsonEscape(value).c_str());
                }
                else if(jsonGetString(line, "eval", value))
                {
                    queue.enqueue([value]
                    {
                        if(!DbgIsValidExpression(value.c_str()))
                        {
                            rpcPrintJson("{\"ok\":false,\"err\":\"invalid expression '%s'\"}", jsonEscape(value).c_str());
                            return true;
                        }
                        duint result = DbgValFromString(value.c_str());
                        rpcPrintJson("{\"ok\":true,\"value\":%llu,\"hex\":\"0x%llX\"}",
                                     (unsigned long long)result, (unsigned long long)result);
                        return true;
                    });
                }
                else if(jsonGetString(line, "cmd", value))
                {
                    std::string syncVal;
                    bool sync = jsonGetString(line, "sync", syncVal) && (syncVal == "true" || syncVal == "1");
                    queue.enqueue([value, sync]
                    {
                        bool ok = false;
                        if(sync)
                            ok = DbgCmdExecDirect(value.c_str());
                        else if(!scriptInfo.empty())
                            ok = scriptInfo[0].execute(value.c_str());
                        if(ok)
                            rpcPrintJson("{\"ok\":true}");
                        else
                            rpcPrintJson("{\"ok\":false,\"err\":\"command failed\"}");
                        return true;
                    });
                }
                else
                    rpcPrintJson("{\"ok\":false,\"err\":\"invalid request: %s\"}", jsonEscape(line).c_str());
            }
            return;
        }

        while(!shutdownRequested.load())
        {
            std::string command;
            if(!std::getline(std::cin, command))
            {
                if(!DbgIsTesting())
                {
                    if(gHasCommandFile)
                    {
                        // stdin closed. A -cf script is started asynchronously
                        // by the command loop; wait until it has started AND
                        // finished before tearing down the command queue, so
                        // DbgCmdExec commands issued by plugin worker threads
                        // while the script is active are not dropped.
                        bool wasRunning = false;
                        for(int i = 0; i < 600; i++) // up to 60s
                        {
                            bool running = DbgIsScriptRunning();
                            if(!running && wasRunning)
                                break; // script started and finished
                            wasRunning = wasRunning || running;
                            Sleep(100);
                        }
                    }
                    requestShutdown();
                }
                break;
            }
            if(command == "exit")
            {
                requestShutdown();
                break;
            }
            else if(command == "langs")
            {
                for(auto & info : scriptInfo)
                    printf("%d:%s\n", info.id, info.name);
            }
            else if(command == "state")
            {
                printf("disasm: 0x%p\n", (void*)guistate.disasm);
                printf("   cip: 0x%p\n", (void*)guistate.cip);
                printf("  dump: 0x%p\n", (void*)guistate.dump);
                printf(" stack: 0x%p\n", (void*)guistate.stack);
                printf("   csp: 0x%p\n", (void*)guistate.csp);
                if(guistate.graph)
                    printf("  graph: 0x%p\n", (void*)guistate.graph);
                if(guistate.memmap)
                    printf(" memmap: 0x%p\n", (void*)guistate.memmap);
                if(guistate.symmod)
                    printf("symmod: 0x%p\n", (void*)guistate.symmod);
            }
            else
            {
                int scriptId = 0;
                if(command.size() > 2 && isdigit(command[0]) && command[1] == '>')
                {
                    scriptId = command[0] - '0';
                    command = command.substr(2);
                }
                if(scriptId >= scriptInfo.size())
                {
                    printf("[FAIL] no script id registered %d\n", scriptId);
                    continue;
                }
                queue.enqueue([scriptId, command]()
                {
                    if(!scriptInfo[scriptId].execute(command.c_str()))
                    {
                        puts("[FAIL] command failed");
                    }
                    return true;
                });
            }
        }
    });
    commandThreadHandle = (HANDLE)commandThread.native_handle();
    while(true)
    {
        std::function<bool()> job;
        queue.wait_dequeue(job);
        if(!job())
        {
            // Graceful shutdown: a -cf script may still be running (the
            // command loop thread awaits it and pumps GUI_PROCESS_EVENTS,
            // which drains DbgCmdExec commands issued by plugin threads).
            // Tearing the command queue down now would drop those commands,
            // so wait for the script to finish first.
            for(int i = 0; i < 600 && DbgIsScriptRunning(); i++)
                Sleep(100);
            if(const auto reason = shutdownCtrlTypeToString(shutdownCtrlType.load()))
                printf("[headless] shutdown requested by %s\n", reason);
            DbgExit();
            dbgStopped = true;
            if(consoleCloseRequested.load())
            {
                if(auto handle = commandThreadHandle.load())
                    CancelSynchronousIo(handle);
            }
            break;
        }
    }
    if(commandThread.joinable())
        commandThread.join();
    commandThreadHandle = nullptr;
    stopRedirectLog();
    SetConsoleCtrlHandler(consoleCtrlHandler, FALSE);
    return 0;
}

extern "C" __declspec(dllexport) void* _gui_sendmessage(GUIMSG type, void* param1, void* param2){
    if(dbgStopped) //there can be no more messages if the debugger stopped = IGNORE
    {
        if(gRpcMode)
            rpcPrintJson("{\"log\":\"[WARN] Ignored %s (%d)\"}", guimsg2str(type), type);
        else
            printf("[WARN] Ignored %s (%d)\n", guimsg2str(type), type);
        return nullptr;
    }

    switch(type)
    {
    case GUI_AUTOCOMPLETE_ADDCMD:
    case GUI_UPDATE_TIME_WASTED_COUNTER:
    case GUI_FLUSH_LOG:
    case GUI_INVALIDATE_SYMBOL_SOURCE:
    case GUI_UPDATE_ARGUMENT_VIEW:
    case GUI_UPDATE_BREAKPOINTS_VIEW:
    case GUI_UPDATE_CALLSTACK:
    case GUI_UPDATE_DISASSEMBLY_VIEW:
    case GUI_UPDATE_DUMP_VIEW:
    case GUI_UPDATE_GRAPH_VIEW:
    case GUI_UPDATE_MEMORY_VIEW:
    case GUI_UPDATE_PATCHES:
    case GUI_UPDATE_REGISTER_VIEW:
    case GUI_UPDATE_SEHCHAIN:
    case GUI_UPDATE_SIDEBAR:
    case GUI_UPDATE_THREAD_VIEW:
    case GUI_UPDATE_TRACE_BROWSER:
    case GUI_UPDATE_TYPE_WIDGET:
    case GUI_UPDATE_WATCH_VIEW:
    case GUI_SYMBOL_UPDATE_MODULE_LIST:
    case GUI_FOCUS_VIEW:
    case GUI_REPAINT_TABLE_VIEW:
    case GUI_ADD_RECENT_FILE:
    case GUI_SCRIPT_SETIP:
    case GUI_SHOW_CPU:
        break;

    case GUI_UPDATE_WINDOW_TITLE:
        SetConsoleTitleW(Utf8ToUtf16((const char*)param1).c_str());
        break;

    case GUI_GET_WINDOW_HANDLE:
        return GetConsoleWindow();

    case GUI_CLOSE_APPLICATION:
        requestShutdown();
        if(auto handle = commandThreadHandle.load())
            CancelSynchronousIo(handle);
        break;

    case GUI_SYMBOL_LOG_ADD:
        if(gRpcMode)
        {
            rpcLog((const char*)param1);
            break;
        }
        printf("[SYMBOL] %s", (const char*)param1);
        if(param1)
        {
            std::string line = std::string("[SYMBOL] ") + (const char*)param1;
            appendRedirectedLog(line.c_str());
        }
        break;

    case GUI_ADD_MSG_TO_LOG_HTML:
    case GUI_ADD_MSG_TO_LOG:
        if(gRpcMode)
        {
            rpcLog((const char*)param1);
            break;
        }
        printf("%s", (const char*)param1);
        appendRedirectedLog((const char*)param1);
        break;

    case GUI_REDIRECT_LOG:
        startRedirectLog((const char*)param1);
        break;

    case GUI_STOP_REDIRECT_LOG:
        stopRedirectLog();
        break;

    case GUI_SET_DEBUG_STATE:
    {
        auto s = DBGSTATE(duint(param1));
        gLastDbgState = (int)s;
        if(gRpcMode)
        {
            rpcPrintJson("{\"event\":\"state\",\"state\":\"%s\"}", dbgstate2str(s));
            break;
        }
        printf("[STATE] %s\n", dbgstate2str(s));
    }
    break;

    case GUI_REGISTER_SCRIPT_LANG:
    {
        SCRIPTTYPEINFO* info = (SCRIPTTYPEINFO*)param1;
        info->id = (int)scriptInfo.size();
        scriptInfo.push_back(*info);
    }
    break;

    case GUI_UNREGISTER_SCRIPT_LANG:
    {
        int id = (int)(duint)param1;
        if(id != 0)
        {
            puts("[TODO] Not implemented GUI_UNREGISTER_SCRIPT_LANG");
        }
    }
    break;

    case GUI_DUMP_AT:
        guistate.dump = (duint)param1;
        break;

    case GUI_DISASSEMBLE_AT:
        guistate.disasm = (duint)param1;
        guistate.cip = (duint)param2;
        break;

    case GUI_STACK_DUMP_AT:
        guistate.stack = (duint)param1;
        guistate.csp = (duint)param2;
        break;

    case GUI_SET_GLOBAL_NOTES:
    {
        if(param1)
            guistate.globalNotes = (const char*)param1;
    }
    break;

    case GUI_SET_DEBUGGEE_NOTES:
    {
        if(param1)
            guistate.debuggeeNotes = (const char*)param1;
    }
    break;

    case GUI_GET_GLOBAL_NOTES:
    {
        char* result = nullptr;
        if(!guistate.globalNotes.empty())
        {
            result = (char*)BridgeAlloc(guistate.globalNotes.size() + 1);
            strcpy_s(result, guistate.globalNotes.size() + 1, guistate.globalNotes.c_str());
        }
        *(char**)param1 = result;
    }
    break;

    case GUI_GET_DEBUGGEE_NOTES:
    {
        char* result = nullptr;
        if(!guistate.debuggeeNotes.empty())
        {
            result = (char*)BridgeAlloc(guistate.debuggeeNotes.size() + 1);
            strcpy_s(result, guistate.debuggeeNotes.size() + 1, guistate.debuggeeNotes.c_str());
        }
        *(char**)param1 = result;
    }
    break;

    case GUI_SELECTION_GET:
    {
        int hWindow = (int)(duint)param1;
        SELECTIONDATA* selection = (SELECTIONDATA*)param2;
        if(!DbgIsDebugging())
            return (void*)false;
        duint p = 0;
        switch(hWindow)
        {
        case GUI_DISASSEMBLY:
            p = guistate.disasm;
            break;
        case GUI_DUMP:
            p = guistate.dump;
            break;
        case GUI_STACK:
            p = guistate.stack;
            break;
        case GUI_GRAPH:
            p = guistate.graph;
            break;
        case GUI_MEMMAP:
            p = guistate.memmap;
            break;
        case GUI_SYMMOD:
            p = guistate.symmod;
            break;
        default:
            return (void*)false;
        }
        selection->start = selection->end = p;
        return (void*)true;
    }
    break;

    case GUI_SELECTION_SET:
    {
        int hWindow = (int)(duint)param1;
        const SELECTIONDATA* selection = (const SELECTIONDATA*)param2;
        if(!DbgIsDebugging())
            return (void*)false;
        duint p = 0;
        switch(hWindow)
        {
        case GUI_DISASSEMBLY:
            guistate.disasm = selection->start;
            break;
        case GUI_DUMP:
            guistate.dump = selection->start;
            break;
        case GUI_STACK:
            guistate.stack = selection->start;
            break;
        case GUI_GRAPH:
            guistate.graph = selection->start;
            break;
        case GUI_MEMMAP:
            guistate.memmap = selection->start;
            break;
        case GUI_SYMMOD:
            guistate.symmod = selection->start;
            break;
        default:
            return (void*)false;
        }
        return (void*)true;
    }
    break;

    case GUI_EXECUTE_ON_GUI_THREAD:
    {
        if(GetCurrentThreadId() == dwGuiThreadId)
            ((GUICALLBACKEX)param1)(param2);
        else
        {
            queue.enqueue([param1, param2]()
            {
                ((GUICALLBACKEX)param1)(param2);
                return true;
            });
        }
    }
    break;

    case GUI_PROCESS_EVENTS:
        // GUI processes its Qt event loop here; headless instead drains the
        // debugger command queue so commands issued by plugin worker threads
        // (DbgCmdExec) keep executing while the command loop thread awaits a
        // running script (JobQueue::await -> GuiProcessEvents spin-wait).
        // In -testing mode the script commands rely on strict ordering
        // (e.g. testfinalize runs after the script), so don't drain there.
        if(!DbgIsTesting())
            DbgProcessPendingCommands();
        break;

    case GUI_MENU_ADD:
    {
        // No real GUI here: return a unique fake menu handle so plugin menu
        // setup succeeds and each menu entry can be addressed by handle.
        static int fakeMenuCounter = 0;
        return (void*)(duint)(++fakeMenuCounter);
    }

    case GUI_MENU_ADD_ENTRY:
    {
        // Return a unique entry handle so pluginmenucall() can locate the
        // entry. Combined with the generated "menu_<title>" commands in
        // plugin_loader.cpp this makes plugin menus invocable from CLI.
        static int fakeEntryCounter = 0;
        return (void*)(duint)(++fakeEntryCounter);
    }

    default:
    {
        if(gRpcMode)
        {
            char buf[256] = "";
            sprintf_s(buf, "[TODO] Not implemented %s (%d)\n", guimsg2str(type), type);
            rpcLog(buf);
            break;
        }
        printf("[TODO] Not implemented %s (%d)\n", guimsg2str(type), type);
        break;
    }
    }
    return nullptr;
}

extern "C" __declspec(dllexport) const char* _gui_translate_text(const char* source)
{
    return source;
}

int main(int argc, char* argv[])
{
    // GitHub Actions and other Node-based parents can set
    // SEM_NOGPFAULTERRORBOX. Clear it so WER LocalDumps can capture an
    // unhandled headless crash without showing legacy hard-error dialogs.
    SetErrorMode(SEM_FAILCRITICALERRORS);

    for(int i = 1; i < argc; i++)
    {
        if(_stricmp(argv[i], "-rpc") == 0)
            gRpcMode = true;
        else if(_stricmp(argv[i], "-cf") == 0)
            gHasCommandFile = true;
    }

    dwGuiThreadId = GetCurrentThreadId();

    // Construct user directory from executable name
    auto hMainModule = GetModuleHandleW(nullptr);
    const auto startupOptions = ParseHostStartupOptions();

    std::wstring userDirectory;
    if(!startupOptions.userDirectory.empty())
    {
        userDirectory = startupOptions.userDirectory;
    }
    else
    {
        wchar_t szUserDirectory[MAX_PATH] = L"";
        GetModuleFileNameW(hMainModule, szUserDirectory, _countof(szUserDirectory));
        auto period = wcsrchr(szUserDirectory, L'.');
        if(period == nullptr)
        {
            puts("Error getting module directory!");
            return EXIT_FAILURE;
        }
        *period = L'\0';
        CreateDirectoryW(szUserDirectory, nullptr);
        userDirectory = szUserDirectory;
    }

    // Initialize the bridge
    BRIDGE_CONFIG config = {};
    config.hGuiModule = hMainModule;
    config.szUserDirectory = userDirectory.c_str();
    const wchar_t* errormsg = BridgeInit(&config);
    if(errormsg != nullptr)
    {
        wprintf(L"BridgeInit failed: %s\n", errormsg);
        return EXIT_FAILURE;
    }

    // Start the debugger
    errormsg = BridgeStart();
    if(errormsg != nullptr)
    {
        wprintf(L"BridgeStart failed: %s\n", errormsg);
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

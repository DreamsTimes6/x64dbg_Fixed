// Data-pattern write watch: `bpmatch <hex>[, <addr>[, <size>]]` makes a
// region read-only; when the target writes into it, the debugger transparently
// unprotects, re-executes the write, single-steps, and compares the written
// bytes against the pattern. On match it pauses (so you can find the code that
// wrote the pattern); otherwise it re-protects and keeps running.
#include <Windows.h>
#include <vector>
#include <string>
#include "writewatch.h"
#include "debugger.h"
#include "console.h"
#include "memory.h"
#include "threading.h"
#include "plugin_loader.h"
#include "value.h"
#include "stringformat.h"

struct WriteWatchEntry
{
    duint addr = 0;
    duint size = 0;
    std::vector<unsigned char> pattern;
    DWORD originalProtect = 0;
    bool readOnly = false;
    bool pendingStep = false;
    duint pendingWriteAddr = 0;
};

static std::vector<WriteWatchEntry> g_writeWatches;

static void protectWatch(WriteWatchEntry& w, bool readOnly)
{
    if(readOnly == w.readOnly)
        return;
    DWORD old = 0;
    if(fdProcessInfo && fdProcessInfo->hProcess)
    {
        VirtualProtectEx(fdProcessInfo->hProcess, (void*)w.addr, w.size,
                         readOnly ? PAGE_READONLY : w.originalProtect, &old);
        if(readOnly)
            w.originalProtect = old;
        w.readOnly = readOnly;
    }
}

static void pauseOnMatch(const WriteWatchEntry& w)
{
    dprintf(QT_TRANSLATE_NOOP("DBG", "Data pattern (%zu bytes) matched at %p!\n"), w.pattern.size(), w.pendingWriteAddr);
    DebugUpdateGuiSetStateAsync(GetContextDataEx(hActiveThread, UE_CIP), paused);
    //lock
    lock(WAITID_RUN);
    // Plugin callback
    PLUG_CB_PAUSEDEBUG pauseInfo = { nullptr };
    plugincbcall(CB_PAUSEDEBUG, &pauseInfo);
    dbgsetforeground();
    dbgsetskipexceptions(false);
    wait(WAITID_RUN);
}

bool WriteWatchHandleException(EXCEPTION_DEBUG_INFO* ExceptionData)
{
    if(!ExceptionData || !ExceptionData->dwFirstChance)
        return false;
    auto& rec = ExceptionData->ExceptionRecord;

    for(auto& w : g_writeWatches)
    {
        if(rec.ExceptionCode == 0xC0000005 && rec.NumberParameters >= 2)
        {
            // Access violation; [1] is the address being written
            duint writeAddr = (duint)rec.ExceptionInformation[1];
            if(!w.readOnly || writeAddr < w.addr || writeAddr >= w.addr + w.size)
                continue;
            // Guard write: unprotect, re-execute the write instruction, single-step
            protectWatch(w, false);
            w.pendingStep = true;
            w.pendingWriteAddr = writeAddr;
            SetContextDataEx(hActiveThread, UE_CIP, (duint)rec.ExceptionAddress);
            duint eflags = GetContextDataEx(hActiveThread, UE_EFLAGS);
            SetContextDataEx(hActiveThread, UE_EFLAGS, eflags | 0x100); // TF
            dbgsetcontinuestatus(DBG_CONTINUE);
            dprintf(QT_TRANSLATE_NOOP("DBG", "bpmatch: write @%p, re-executing...\n"), writeAddr);
            return true;
        }
        if(rec.ExceptionCode == 0x80000004 && w.pendingStep)
        {
            // Single-step after the re-executed write: compare written bytes
            w.pendingStep = false;
            bool match = true;
            for(size_t i = 0; i < w.pattern.size(); i++)
            {
                unsigned char b = 0;
                if(!MemRead(w.pendingWriteAddr + i, &b, 1) || b != w.pattern[i])
                {
                    match = false;
                    break;
                }
            }
            if(match)
            {
                pauseOnMatch(w);
                return true;
            }
            // Not the target pattern: re-protect and keep going seamlessly
            protectWatch(w, true);
            dbgsetcontinuestatus(DBG_CONTINUE);
            return true;
        }
    }
    return false;
}

static bool parsePattern(const char* hex, std::vector<unsigned char>& pattern)
{
    std::string s = hex;
    for(size_t i = 0; i + 1 < s.size(); i += 2)
    {
        char tmp[3] = { s[i], s[i + 1], 0 };
        pattern.push_back((unsigned char)strtoul(tmp, nullptr, 16));
    }
    return !pattern.empty();
}

bool cbDebugBpMatch(int argc, char* argv[]) //bpmatch <hex>[, <addr>[, <size>]] | bpmatch clear
{
    if(argc < 2)
        return false;
    if(!_stricmp(argv[1], "clear"))
    {
        for(auto& w : g_writeWatches)
            protectWatch(w, false);
        g_writeWatches.clear();
        dputs(QT_TRANSLATE_NOOP("DBG", "bpmatch: all watches cleared"));
        return true;
    }

    std::vector<unsigned char> pattern;
    if(!parsePattern(argv[1], pattern))
        return false;

    duint addr = 0;
    duint size = pattern.size();
    if(argc > 2)
        addr = valfromstring(argv[2], nullptr, false);
    if(argc > 3)
    {
        duint s = valfromstring(argv[3], nullptr, false);
        if(s)
            size = s;
    }

    if(!addr)
    {
        // Enumerate writable committed regions and watch each one
        MEMORY_BASIC_INFORMATION mbi;
        duint count = 0;
        for(duint p = 0; VirtualQueryEx(fdProcessInfo->hProcess, (void*)p, &mbi, sizeof(mbi)); p = (duint)mbi.BaseAddress + mbi.RegionSize)
        {
            if(mbi.State != MEM_COMMIT || mbi.Protect == PAGE_NOACCESS)
                continue;
            DWORD prot = mbi.Protect & 0xFF;
            if(!(prot & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
                continue;
            WriteWatchEntry w;
            w.addr = (duint)mbi.BaseAddress;
            w.size = mbi.RegionSize;
            w.pattern = pattern;
            protectWatch(w, true);
            if(w.readOnly)
            {
                g_writeWatches.push_back(w);
                count++;
            }
        }
        dprintf(QT_TRANSLATE_NOOP("DBG", "bpmatch: watching %u writable regions for %zu-byte pattern\n"), count, pattern.size());
    }
    else
    {
        WriteWatchEntry w;
        w.addr = addr;
        w.size = size;
        w.pattern = pattern;
        protectWatch(w, true);
        if(!w.readOnly)
        {
            dputs(QT_TRANSLATE_NOOP("DBG", "bpmatch: failed to protect region"));
            return false;
        }
        g_writeWatches.push_back(w);
        dprintf(QT_TRANSLATE_NOOP("DBG", "bpmatch: watching %p size=%X for %zu-byte pattern\n"), addr, (unsigned int)size, pattern.size());
    }
    return true;
}

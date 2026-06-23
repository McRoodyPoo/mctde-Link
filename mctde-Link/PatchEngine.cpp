/*
    PatchEngine  -  mctde-Link (built into d3d9.dll)
    Implementation. See PatchEngine.h for the design rationale.
*/
#include "PatchEngine.h"

#include <tlhelp32.h>
#include <cstdio>

namespace mp {

// ------------------------------------------------------------------ logging

static void DefaultSink(const std::string& msg) {
    OutputDebugStringA(("[PhantomUnleashed] " + msg + "\n").c_str());
}
static void (*g_sink)(const std::string&) = DefaultSink;

void SetLogSink(void (*sink)(const std::string&)) { g_sink = sink ? sink : DefaultSink; }
void Log(const std::string& message) { if (g_sink) g_sink(message); }

static std::string Hex(const std::vector<uint8_t>& b, size_t n) {
    char buf[8];
    std::string s;
    for (size_t i = 0; i < n && i < b.size(); ++i) {
        sprintf_s(buf, sizeof(buf), "%02X ", b[i]);
        s += buf;
    }
    if (!s.empty()) s.pop_back();
    return s;
}

// -------------------------------------------------------------- ThreadFreezer

ThreadFreezer::ThreadFreezer() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        Log("ThreadFreezer: CreateToolhelp32Snapshot failed; patching without freeze.");
        return;
    }

    const DWORD myPid = GetCurrentProcessId();
    const DWORD myTid = GetCurrentThreadId();

    THREADENTRY32 te;
    te.dwSize = sizeof(te);
    if (Thread32First(snap, &te)) {
        do {
            if (te.dwSize >= FIELD_OFFSET(THREADENTRY32, th32OwnerProcessID) + sizeof(te.th32OwnerProcessID)
                && te.th32OwnerProcessID == myPid
                && te.th32ThreadID != myTid) {
                HANDLE th = OpenThread(THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
                if (th) {
                    if (SuspendThread(th) != (DWORD)-1)
                        suspended_.push_back(th);
                    else
                        CloseHandle(th);
                }
            }
            te.dwSize = sizeof(te);
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
}

ThreadFreezer::~ThreadFreezer() {
    for (HANDLE th : suspended_) {
        ResumeThread(th);
        CloseHandle(th);
    }
}

// ---------------------------------------------------------------- PatchEngine

void PatchEngine::Stage(uintptr_t address, std::vector<uint8_t> patched, size_t verifyLen, const char* note) {
    Patch p;
    p.address   = address;
    p.patched   = std::move(patched);
    p.verifyLen = (verifyLen <= p.patched.size()) ? verifyLen : p.patched.size();
    p.note      = note ? note : "";
    patches_.push_back(std::move(p));
}

int PatchEngine::Verify() {
    int mismatches = 0;
    for (const Patch& p : patches_) {
        if (p.verifyLen == 0) continue;

        const uint8_t* live = reinterpret_cast<const uint8_t*>(p.address);
        // Make sure the page is at least readable before we touch it.
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(live, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) {
            char buf[64]; sprintf_s(buf, sizeof(buf), "0x%p", (void*)p.address);
            Log("VERIFY  unreadable @ " + std::string(buf) + "  (" + p.note + ")");
            ++mismatches;
            continue;
        }

        bool ok = true;
        for (size_t i = 0; i < p.verifyLen; ++i) {
            if (live[i] != p.patched[i]) { ok = false; break; }
        }

        std::vector<uint8_t> found(live, live + p.verifyLen);
        char addr[32]; sprintf_s(addr, sizeof(addr), "0x%08X", (unsigned)p.address);
        if (ok) {
            Log(std::string("VERIFY  ok   ") + addr + "  [" + Hex(p.patched, p.verifyLen) + "]  " + p.note);
        } else {
            Log(std::string("VERIFY  MISS ") + addr + "  expected [" + Hex(p.patched, p.verifyLen)
                + "] found [" + Hex(found, p.verifyLen) + "]  " + p.note);
            ++mismatches;
        }
    }
    Log("VERIFY  complete: " + std::to_string(mismatches) + " mismatch(es) of "
        + std::to_string(patches_.size()) + " site(s).");
    return mismatches;
}

bool PatchEngine::Commit() {
    if (committed_) { Log("Commit: already committed."); return true; }

    ThreadFreezer freeze; // threads stay suspended until end of this function

    for (Patch& p : patches_) {
        const SIZE_T len = p.patched.size();
        void* addr = reinterpret_cast<void*>(p.address);

        DWORD oldProt = 0;
        if (!VirtualProtect(addr, len, PAGE_EXECUTE_READWRITE, &oldProt)) {
            char b[32]; sprintf_s(b, sizeof(b), "0x%08X", (unsigned)p.address);
            Log(std::string("Commit: VirtualProtect failed @ ") + b + " (" + p.note + "); rolling back.");
            RestoreAll();
            return false;
        }

        // Capture the original bytes before overwriting.
        p.original.assign(reinterpret_cast<uint8_t*>(addr), reinterpret_cast<uint8_t*>(addr) + len);
        memcpy(addr, p.patched.data(), len);
        p.applied = true;

        DWORD tmp = 0;
        VirtualProtect(addr, len, oldProt, &tmp);
        FlushInstructionCache(GetCurrentProcess(), addr, len);
    }

    committed_ = true;
    Log("Commit: applied " + std::to_string(patches_.size()) + " patch(es).");
    return true;
}

void PatchEngine::RestoreAll() {
    ThreadFreezer freeze;

    size_t restored = 0;
    // Reverse order so overlapping/adjacent patches unwind cleanly.
    for (auto it = patches_.rbegin(); it != patches_.rend(); ++it) {
        Patch& p = *it;
        if (!p.applied || p.original.empty()) continue;

        const SIZE_T len = p.original.size();
        void* addr = reinterpret_cast<void*>(p.address);

        DWORD oldProt = 0;
        if (VirtualProtect(addr, len, PAGE_EXECUTE_READWRITE, &oldProt)) {
            memcpy(addr, p.original.data(), len);
            DWORD tmp = 0;
            VirtualProtect(addr, len, oldProt, &tmp);
            FlushInstructionCache(GetCurrentProcess(), addr, len);
            p.applied = false;
            ++restored;
        }
    }

    committed_ = false;
    Log("RestoreAll: reverted " + std::to_string(restored) + " patch(es).");
}

} // namespace mp

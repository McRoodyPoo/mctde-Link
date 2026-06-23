/*
    PatchEngine  -  mctde-Link (built into d3d9.dll)

    A small, reversible in-memory patch framework used by the PhantomUnleashed module.

    This is a generic patch framework written for mctde-Link (not derived from any other
    project). Design goals:
      * Every applied patch saves the bytes it overwrote, so the whole set can be
        cleanly reverted on unload / failure.
      * Writes happen with all other threads suspended, so a game thread can never
        execute a half-written instruction.
      * Nothing ever spins forever; callers that need a scan use a bounded helper.
      * A dry-run Verify() reads each site and confirms the opcode bytes that should
        already be there, so a wrong offset is caught before any code runs.
*/
#pragma once

#include <windows.h>
#include <cstdint>
#include <vector>
#include <string>

namespace mp {

// Logging sink. Set once at startup (see PhantomUnleashed.cpp). Never null after Init().
void Log(const std::string& message);
void SetLogSink(void (*sink)(const std::string&));

// Suspends every other thread in this process for its lifetime, then resumes them.
// Used to make live code patching safe. The current thread is never suspended.
class ThreadFreezer {
public:
    ThreadFreezer();
    ~ThreadFreezer();
    ThreadFreezer(const ThreadFreezer&) = delete;
    ThreadFreezer& operator=(const ThreadFreezer&) = delete;
private:
    std::vector<HANDLE> suspended_;
};

// One reversible byte patch.
struct Patch {
    uintptr_t            address = 0;   // absolute VA to write
    std::vector<uint8_t> patched;       // bytes we write
    std::vector<uint8_t> original;      // bytes captured at Commit() (for restore)
    size_t               verifyLen = 0; // # of leading bytes that must already match `patched`
                                        // (opcode/modrm are identical pre- and post-patch;
                                        //  only the trailing immediate/count differs)
    std::string          note;          // human label for logs
    bool                 applied = false;
};

class PatchEngine {
public:
    // Stage a patch. `verifyLen` leading bytes of `patched` are the invariant opcode
    // bytes that should already exist at the site; pass 0 to skip verification for a site.
    void Stage(uintptr_t address, std::vector<uint8_t> patched, size_t verifyLen, const char* note);

    // Read each staged site and report whether its leading opcode bytes match.
    // Does NOT modify anything. Returns the number of mismatches (0 == all good).
    int Verify();

    // Apply every staged patch with threads frozen. Captures originals first.
    // On any failure, already-applied patches are rolled back and false is returned.
    bool Commit();

    // Revert every applied patch to its captured original bytes.
    void RestoreAll();

    size_t count() const { return patches_.size(); }

private:
    std::vector<Patch> patches_;
    bool committed_ = false;
};

} // namespace mp

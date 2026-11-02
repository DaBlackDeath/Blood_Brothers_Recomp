// The 68000 main CPU behind a small interface: the statically recompiled program (recomp_cpu.cpp).
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

class Machine;

struct CpuState {
    uint32_t d[8], a[8];
    uint32_t pc, sr;
    uint32_t usp, isp;
};

class CpuBackend {
public:
    virtual ~CpuBackend() = default;
    virtual void reset() = 0;
    // Runs at least `cycles` cycles (whole instructions); returns the cycles used.
    virtual int execute(int cycles) = 0;
    virtual void set_irq(int level) = 0;
    virtual void get_state(CpuState& s) const = 0;
    // The program ROM was modified (cheats). `patched` lists the byte ranges [first, last) that now differ
    // from the original program.
    virtual void code_changed(const std::vector<std::pair<uint32_t, uint32_t>>& patched) { (void)patched; }
    virtual const char* name() const = 0;
    virtual std::string report() const { return std::string(); }
};

// Returns nullptr (and sets `why`) if no recompiled code has been generated, or if the loaded program ROM
// is not the one the code was generated from.
std::unique_ptr<CpuBackend> make_recomp_backend(Machine* m, const std::vector<uint8_t>& prg, std::string& why);

// The game reached code that is not part of the recompiled program. Reports the problem and ends the
// program (defined by the front end, main.cpp).
[[noreturn]] void recomp_fatal(const std::string& message);

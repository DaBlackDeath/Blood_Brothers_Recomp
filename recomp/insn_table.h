// Table entry describing one generated instruction function (see tools/gen_recomp.py).
#pragma once
#include <cstdint>
#include "cpu68k.h"

namespace r68k {
struct InsnEntry {
    uint32_t addr;
    uint16_t opcode;
    uint8_t length;
    bool translated;
    uint8_t flags;   // bit 0: ends a basic block
    void (*fn)(Cpu&);
};

struct BlockEntry {
    uint32_t addr;
    uint16_t count;  // instructions in the block
    void (*fn)(Cpu&);
};
}  // namespace r68k

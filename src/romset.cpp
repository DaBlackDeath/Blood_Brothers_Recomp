#include "romset.h"

#include <cstdio>
#include <cstring>
#include <memory>

#include "miniz.h"

namespace {

struct RomEntry {
    const char* name;
    uint32_t crc;
    size_t size;
};

const char* const kSetName = "bloodbro";
const char* const kSetDescription = "Blood Bros. (World?)";

const RomEntry kMain0{"1.u022.8n", 0xac6719e7, 0x20000};   // even bytes, 0x00000
const RomEntry kMain1{"2.u021.7n", 0x204dca6e, 0x20000};   // odd bytes,  0x00000
const RomEntry kMain2{"3.u024.8l", 0x18d3c460, 0x20000};   // even bytes, 0x40000
const RomEntry kMain3{"4.u023.7l", 0xfd951c2c, 0x20000};   // odd bytes,  0x40000
const RomEntry kAudio{"bb_07.u1016.6a", 0x411b94e8, 0x10000};
const RomEntry kTx1{"bb_05.u061.6f", 0x04ba6d19, 0x10000};
const RomEntry kTx2{"bb_06.u063.6d", 0x7092e35b, 0x10000};
const RomEntry kBg{"blood_bros_bk__=c=1990_tad_corp.u064.4d", 0x1aa87ee6, 0x100000};
const RomEntry kObj{"blood_bros_obj__=c=1990_tad_corp.u078.2n", 0xd27c3952, 0x100000};
const RomEntry kOki{"bb_08.u095.5a", 0xdeb1b975, 0x20000};
const RomEntry kProm{"cb006.u083.6c", 0xb2b89a74, 0x100};

// One searchable location: a zip file or a directory.
struct Source {
    std::string path;
    bool is_zip = false;
    mz_zip_archive zip{};
    bool zip_open = false;

    ~Source() {
        if (zip_open) mz_zip_reader_end(&zip);
    }
};

bool read_file(const std::string& fn, std::vector<uint8_t>& data) {
    FILE* f = std::fopen(fn.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (n < 0) {
        std::fclose(f);
        return false;
    }
    data.resize((size_t)n);
    size_t got = n ? std::fread(data.data(), 1, (size_t)n, f) : 0;
    std::fclose(f);
    return got == (size_t)n;
}

bool find_in_source(Source& src, const std::string& set, const std::string& name,
                    std::vector<uint8_t>& data) {
    const std::string candidates[2] = {name, set + "/" + name};
    for (const std::string& c : candidates) {
        if (src.is_zip) {
            int idx = mz_zip_reader_locate_file(&src.zip, c.c_str(), nullptr, 0);
            if (idx < 0) continue;
            mz_zip_archive_file_stat st;
            if (!mz_zip_reader_file_stat(&src.zip, (mz_uint)idx, &st)) continue;
            data.resize((size_t)st.m_uncomp_size);
            if (!mz_zip_reader_extract_to_mem(&src.zip, (mz_uint)idx, data.data(), data.size(), 0))
                continue;
            return true;
        } else {
            if (read_file(src.path + "/" + c, data)) return true;
        }
    }
    return false;
}

bool fetch(std::vector<std::unique_ptr<Source>>& sources, const std::string& set,
           const RomEntry& e, std::vector<uint8_t>& data, std::string& error,
           std::string& warnings) {
    for (auto& s : sources) {
        if (find_in_source(*s, set, e.name, data)) {
            uint32_t crc = (uint32_t)mz_crc32(MZ_CRC32_INIT, data.data(), data.size());
            char buf[256];
            if (data.size() != e.size) {
                std::snprintf(buf, sizeof buf, "warning: %s has size 0x%zx, expected 0x%zx\n",
                              e.name, data.size(), e.size);
                warnings += buf;
            } else if (crc != e.crc) {
                std::snprintf(buf, sizeof buf, "warning: %s has CRC %08x, expected %08x\n", e.name,
                              crc, e.crc);
                warnings += buf;
            }
            if (data.size() < e.size) data.resize(e.size, 0);
            if (data.size() > e.size) data.resize(e.size);
            return true;
        }
    }
    error = std::string("ROM file not found: ") + e.name;
    return false;
}

void interleave(std::vector<uint8_t>& dst, size_t base, size_t lane, const std::vector<uint8_t>& src) {
    for (size_t i = 0; i < src.size(); i++) dst[base + 2 * i + lane] = src[i];
}

}  // namespace

bool load_romset(const std::vector<std::string>& paths, RomSet& out, std::string& error, std::string& warnings) {
    const std::string set = kSetName;
    std::vector<std::unique_ptr<Source>> sources;
    for (const std::string& p : paths) {
        auto s = std::make_unique<Source>();
        s->path = p;
        size_t n = p.size();
        s->is_zip = n > 4 && (p.compare(n - 4, 4, ".zip") == 0 || p.compare(n - 4, 4, ".ZIP") == 0);
        if (s->is_zip) {
            std::memset(&s->zip, 0, sizeof s->zip);
            if (!mz_zip_reader_init_file(&s->zip, p.c_str(), 0)) {
                error = "cannot open zip file: " + p;
                return false;
            }
            s->zip_open = true;
        }
        sources.push_back(std::move(s));
    }

    out = RomSet{};
    out.set_name = kSetName;
    out.description = kSetDescription;
    out.maincpu.assign(0x80000, 0);
    out.audiocpu.assign(0x20000, 0);
    out.txtiles.assign(0x20000, 0);
    out.oki.assign(0x40000, 0);

    std::vector<uint8_t> d;
    // 68000 program: ROM_LOAD16_BYTE, "even" file at offset 0 (high byte), "odd" file at offset 1
    if (!fetch(sources, set, kMain0, d, error, warnings)) return false;
    interleave(out.maincpu, 0x00000, 0, d);
    if (!fetch(sources, set, kMain1, d, error, warnings)) return false;
    interleave(out.maincpu, 0x00000, 1, d);
    if (!fetch(sources, set, kMain2, d, error, warnings)) return false;
    interleave(out.maincpu, 0x40000, 0, d);
    if (!fetch(sources, set, kMain3, d, error, warnings)) return false;
    interleave(out.maincpu, 0x40000, 1, d);

    // Z80: ROM_LOAD 0x8000 at 0, ROM_CONTINUE 0x8000 at 0x10000, ROM_COPY first half to 0x18000
    if (!fetch(sources, set, kAudio, d, error, warnings)) return false;
    std::memcpy(&out.audiocpu[0x00000], &d[0x0000], 0x8000);
    std::memcpy(&out.audiocpu[0x10000], &d[0x8000], 0x8000);
    std::memcpy(&out.audiocpu[0x18000], &d[0x0000], 0x8000);

    if (!fetch(sources, set, kTx1, d, error, warnings)) return false;
    std::memcpy(&out.txtiles[0x00000], d.data(), 0x10000);
    if (!fetch(sources, set, kTx2, d, error, warnings)) return false;
    std::memcpy(&out.txtiles[0x10000], d.data(), 0x10000);

    if (!fetch(sources, set, kBg, out.bgtiles, error, warnings)) return false;
    if (!fetch(sources, set, kObj, out.sprites, error, warnings)) return false;

    if (!fetch(sources, set, kOki, d, error, warnings)) return false;
    std::memcpy(out.oki.data(), d.data(), 0x20000);

    if (!fetch(sources, set, kProm, out.prom, error, warnings)) return false;
    return true;
}

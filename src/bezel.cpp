#include "bezel.h"

#include <algorithm>
#include <cstdio>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#include "stb_image.h"

bool Bezel::load(const std::string& file, std::string& error) {
    FILE* f = std::fopen(file.c_str(), "rb");
    if (!f) { error = "cannot open " + file; return false; }
    std::vector<unsigned char> data;
    unsigned char buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) data.insert(data.end(), buf, buf + n);
    std::fclose(f);

    int iw = 0, ih = 0, comp = 0;
    unsigned char* px = stbi_load_from_memory(data.data(), (int)data.size(), &iw, &ih, &comp, 4);
    if (!px) { error = file + ": " + stbi_failure_reason() + " (only PNG files are supported)"; return false; }
    if (iw < 64 || ih < 64 || iw > 8192 || ih > 8192) {
        stbi_image_free(px);
        error = file + ": unreasonable image size";
        return false;
    }
    w = iw;
    h = ih;
    path = file;
    argb.resize((size_t)w * h);
    for (size_t i = 0; i < argb.size(); i++) {
        const unsigned char* p = px + i * 4;
        argb[i] = ((uint32_t)p[3] << 24) | ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
    }
    stbi_image_free(px);

    // Screen window: the transparent region that contains (or is nearest to) the image centre.
    auto transparent = [&](int x, int y) { return (argb[(size_t)y * w + x] >> 24) < 40; };
    int sx = -1, sy = -1;
    for (int r = 0; r < std::min(w, h) / 2 && sx < 0; r += 4) {
        const int cx = w / 2, cy = h / 2;
        const int cand[4][2] = {{cx + r, cy}, {cx - r, cy}, {cx, cy + r}, {cx, cy - r}};
        for (auto& c : cand)
            if (c[0] >= 0 && c[0] < w && c[1] >= 0 && c[1] < h && transparent(c[0], c[1])) { sx = c[0]; sy = c[1]; break; }
    }
    has_window = false;
    if (sx >= 0) {
        std::vector<uint8_t> seen((size_t)w * h, 0);
        std::vector<int> stack;
        stack.push_back(sy * w + sx);
        seen[(size_t)sy * w + sx] = 1;
        int x0 = sx, x1 = sx, y0 = sy, y1 = sy;
        size_t area = 0;
        while (!stack.empty()) {
            const int cur = stack.back();
            stack.pop_back();
            const int x = cur % w, y = cur / w;
            area++;
            x0 = std::min(x0, x); x1 = std::max(x1, x); y0 = std::min(y0, y); y1 = std::max(y1, y);
            const int nb[4][2] = {{x + 1, y}, {x - 1, y}, {x, y + 1}, {x, y - 1}};
            for (auto& q : nb) {
                if (q[0] < 0 || q[0] >= w || q[1] < 0 || q[1] >= h) continue;
                const size_t k = (size_t)q[1] * w + q[0];
                if (seen[k] || !transparent(q[0], q[1])) continue;
                seen[k] = 1;
                stack.push_back((int)k);
            }
        }
        // a window must be a real part of the picture (not a speck), and not the whole image
        if (area > (size_t)w * h / 50 && !(x0 == 0 && y0 == 0 && x1 == w - 1 && y1 == h - 1)) {
            has_window = true;
            win_x = x0; win_y = y0; win_w = x1 - x0 + 1; win_h = y1 - y0 + 1;
        }
    }
    return true;
}

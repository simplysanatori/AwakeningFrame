// A short label on the HUD for something the player did that has no other sign (the rig's "View Reset", runtime/xr/xr_rig.c).
// view_toast_show may be called from any thread; menu.mm draws it with the pickup card's overlays (view_toast_wanted /
// view_toast_draw, render thread), in the same parchment style, centred low in the HUD's 16:9 box. No sound.
#include <chrono>
#include <float.h>
#include <math.h>
#include <mutex>
#include <stdio.h>
#include <string.h>

#include "imgui.h"
#include "i18n.h"

namespace {
const double kHold = 1.1, kOut = 0.4;  // seconds: fully shown, then fading
std::mutex g_mu;
char g_text[48];
double g_t0 = -100;

double now_s() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}
}  // namespace

extern "C" void view_toast_show(const char *text) {
    std::lock_guard<std::mutex> lk(g_mu);
    snprintf(g_text, sizeof g_text, "%s", text);
    g_t0 = now_s();
}

extern "C" bool view_toast_wanted(void) {
    std::lock_guard<std::mutex> lk(g_mu);
    return now_s() - g_t0 < kHold + kOut;
}

extern "C" void view_toast_draw(ImVec2 display, ImFont *font, float scale) {
    char text[48];
    double t;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        t = now_s() - g_t0;
        snprintf(text, sizeof text, "%s", g_text);
    }
    if (t < 0 || t >= kHold + kOut || !text[0]) return;
    float a = t < kHold ? 1.0f : 1.0f - (float)((t - kHold) / kOut);
    a = fmaxf(0, fminf(1, a));
    const char *label = L(text);
    float fs = 36 * scale, pad = 18 * scale;
    ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, 0, label);
    float h = fs * 0.62f + 2 * pad, w = ts.x + 2 * pad * 1.4f;
    float bw = fminf(display.x, display.y * 16 / 9);
    float x = (display.x - w) * 0.5f, y = display.y * 0.80f;
    (void)bw;
    ImDrawList *dl = ImGui::GetBackgroundDrawList();
    auto col = [a](int r, int g, int b, int al) { return IM_COL32(r, g, b, (int)(al * a)); };
    float r = 10 * scale, u = scale;
    ImVec2 p0(x, y), p1(x + w, y + h);
    dl->AddRectFilled(ImVec2(p0.x + 2 * u, p0.y + 3 * u), ImVec2(p1.x + 2 * u, p1.y + 3 * u), col(0, 0, 0, 45), r);
    dl->AddRectFilled(ImVec2(p0.x - 2 * u, p0.y - 2 * u), ImVec2(p1.x + 2 * u, p1.y + 2 * u), col(0x4A, 0x30, 0x20, 170), r + 2 * u);
    dl->AddRectFilled(p0, p1, col(0xF3, 0xE6, 0xC6, 200), r);
    dl->AddRect(p0, p1, col(0xC8, 0x9B, 0x3C, 220), r, 0, 2.0f * u);
    // (Baloo's line box is tall: the text is placed by its cap height, as the pickup card does)
    dl->AddText(font, fs, ImVec2(x + pad * 1.4f, y + pad - fs * 0.22f), col(0x4A, 0x30, 0x20, 255), label);
}

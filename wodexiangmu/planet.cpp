// planet.cpp — 星尘漩涡：粒子绕鼠标光标的引力漩涡（单文件 SDL2 炫酷可视化）
// 编译：g++ -std=c++17 planet.cpp -o planet.exe -lmingw32 -lSDL2
// 运行：./planet.exe
//
// 操作：
//   移动鼠标   —— 粒子被引力拖拽，跟随光标环绕
//   按住左键   —— 蓄力：粒子向光标处螺旋聚集，越聚越亮
//   松开左键   —— 释放：粒子爆炸喷发 + 冲击波环
//   鼠标滚轮   —— 放大 / 缩小
//   空格       —— 切换配色方案
//   R          —— 重置粒子
//   F          —— 切换全屏
//   ESC        —— 退出

#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <vector>

const float PI = 3.14159265358979323846f;

static float rand01() { return (float)rand() / (float)RAND_MAX; }

static float smoothstep(float e0, float e1, float x) {
    float t = (x - e0) / (e1 - e0);
    if (t < 0) t = 0; if (t > 1) t = 1;
    return t * t * (3 - 2 * t);
}

// ---------- HSV -> RGB ----------
static void hsv2rgb(float h, float s, float v, Uint8& r, Uint8& g, Uint8& b) {
    if (s <= 0.0f) { r = g = b = (Uint8)(v * 255); return; }
    h = fmodf(h, 1.0f); if (h < 0) h += 1.0f;
    h *= 6.0f;
    int i = (int)h; float f = h - i;
    float p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f));
    float rr, gg, bb;
    switch (i % 6) {
        case 0: rr = v; gg = t; bb = p; break;
        case 1: rr = q; gg = v; bb = p; break;
        case 2: rr = p; gg = v; bb = t; break;
        case 3: rr = p; gg = q; bb = v; break;
        case 4: rr = t; gg = p; bb = v; break;
        default: rr = v; gg = p; bb = q; break;
    }
    r = (Uint8)(rr * 255); g = (Uint8)(gg * 255); b = (Uint8)(bb * 255);
}

// ---------- 预生成贴图：软光晕 / 实心球 / 光环 ----------
// 全部是白色灰度 + alpha 通道，绘制时用 SDL_SetTextureColorMod 染色。
static SDL_Texture* makeGlowTex(SDL_Renderer* ren, int size) {
    SDL_Texture* tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, size, size);
    std::vector<Uint32> buf(size * size);
    float c = size * 0.5f;
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            float dx = x - c, dy = y - c;
            float d = sqrtf(dx * dx + dy * dy) / c;      // 0..~1.41
            int al = (int)(expf(-d * d * 5.0f) * 255.0f);
            buf[y * size + x] = (al << 24) | 0x00FFFFFF; // 白色，alpha 渐变
        }
    SDL_UpdateTexture(tex, NULL, buf.data(), size * 4);
    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_ADD);
    return tex;
}

static SDL_Texture* makeRingTex(SDL_Renderer* ren, int size) {
    SDL_Texture* tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, size, size);
    std::vector<Uint32> buf(size * size);
    float c = size * 0.5f;
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            float dx = x - c, dy = y - c;
            float d = sqrtf(dx * dx + dy * dy) / c;      // 0..~1.41
            float t = (d - 0.80f) / 0.09f;               // 环带(高斯)
            int al = (int)(expf(-t * t) * 255.0f);
            buf[y * size + x] = (al << 24) | 0x00FFFFFF;
        }
    SDL_UpdateTexture(tex, NULL, buf.data(), size * 4);
    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_ADD);
    return tex;
}

// ---------- 粒子 ----------
struct Particle { float x, y, vx, vy, size, hue; };

int main() {
    const int W = 1280, H = 720;      // 窗口
    const int CW = W / 2, CH = H / 2; // 画布(半分辨率)

    if (SDL_Init(SDL_INIT_VIDEO) != 0) { printf("SDL_Init: %s\n", SDL_GetError()); return 1; }
    SDL_Window* win = SDL_CreateWindow("星尘漩涡", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                       W, H, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    SDL_Renderer* ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!win || !ren) { printf("创建失败: %s\n", SDL_GetError()); SDL_Quit(); return 1; }

    // 离屏画布
    SDL_Texture* canvas = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, CW, CH);
    SDL_SetTextureScaleMode(canvas, SDL_ScaleModeLinear);
    SDL_SetRenderTarget(ren, canvas);
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_RenderClear(ren);
    SDL_SetRenderTarget(ren, NULL);

    // 贴图
    SDL_Texture* glowTex = makeGlowTex(ren, 64);
    SDL_Texture* ringTex = makeRingTex(ren, 128);

    // ---------- 参数 ----------
    const float CORE_R = 26.0f;   // 核心半径(引力中心吸收/重生半径)
    const float GM = 400000.0f;     // 引力强度
    const float SOFT = 400.0f;      // 软化，防止贴脸奇点
    const float DAMP = 0.15f;       // 阻尼(让轨道慢慢圆化)
    const float MAXV = 420.0f;      // 速度上限
    const float FOLLOW = 6.0f;      // 行星跟手速度
    const float CHARGE_RATE = 1.1f; // 蓄力增速(0→1 约 1 秒)
    const float CHARGE_PULL = 30000.0f; // 蓄力内吸
    const float CHARGE_SWIRL = 36000.0f;// 蓄力旋涡
    const float BURST_SPEED = 380.0f;   // 释放喷发速度
    const float FADE = 16.0f;       // 拖尾长度

    float px = CW * 0.5f, py = CH * 0.5f; // 行星位置(画布坐标)
    float zoom = 1.0f;
    float charge = 0.0f;
    bool holding = false, fullscreen = false;
    int palette = 0;

    // 冲击波
    float shockR = 0, shockA = 0; bool shockActive = false;

    // ---------- 粒子 ----------
    std::vector<Particle> ps(1600);
    auto respawn = [&](Particle& p) {
        float a = rand01() * 2 * PI;
        float R = CORE_R * 1.8f + rand01() * 140.0f;
        p.x = px + cosf(a) * R;
        p.y = py + sinf(a) * R;
        float orbit = sqrtf(GM / (R + 0.001f)) * (0.9f + rand01() * 0.25f);
        p.vx = -sinf(a) * orbit;      // 切向速度 → 绕行
        p.vy =  cosf(a) * orbit;
        p.size = 1.0f + rand01() * 2.2f;
        p.hue = rand01();
    };
    for (auto& p : ps) respawn(p);

    // ---------- 背景星野 ----------
    struct Star { float x, y; Uint8 a; };
    std::vector<Star> stars(240);
    for (auto& s : stars) { s.x = rand01() * CW; s.y = rand01() * CH; s.a = 40 + rand() % 90; }

    // ---------- 配色重映射 ----------
    auto paletteHue = [&](float u) -> float {
        switch (palette) {
            case 0: return u;                                     // 极光(全色相，低饱和更柔和)
            case 1: return 0.58f + 0.14f * sinf(2 * PI * u);      // 深空蓝紫
            case 2: return 0.08f + 0.05f * sinf(2 * PI * u);      // 琥珀金
            default: return 0.45f + 0.08f * sinf(2 * PI * u);     // 青碧
        }
    };

    SDL_Event ev;
    bool running = true;
    Uint32 last = SDL_GetTicks(), fpsTick = last;
    int frames = 0;

    while (running) {
        int ww, wh; SDL_GetWindowSize(win, &ww, &wh);
        float sx = (float)CW / ww, sy = (float)CH / wh;

        // ---------- 事件 ----------
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) running = false;
            else if (ev.type == SDL_KEYDOWN) {
                switch (ev.key.keysym.sym) {
                    case SDLK_ESCAPE: running = false; break;
                    case SDLK_SPACE:  palette = (palette + 1) % 4; break;
                    case SDLK_r:      for (auto& p : ps) respawn(p); break;
                    case SDLK_f:
                        fullscreen = !fullscreen;
                        SDL_SetWindowFullscreen(win, fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
                        break;
                }
            }
            else if (ev.type == SDL_MOUSEBUTTONDOWN && ev.button.button == SDL_BUTTON_LEFT)
                holding = true;
            else if (ev.type == SDL_MOUSEBUTTONUP && ev.button.button == SDL_BUTTON_LEFT) {
                // 释放：喷发 + 冲击波
                float b = charge * BURST_SPEED;
                for (auto& p : ps) {
                    float dx = p.x - px, dy = p.y - py;
                    float rr = sqrtf(dx * dx + dy * dy) + 1.0f;
                    p.vx += dx / rr * b;
                    p.vy += dy / rr * b;
                }
                shockActive = true; shockR = CORE_R * 0.5f; shockA = 1.0f;
                holding = false; charge = 0.0f;
            }
            else if (ev.type == SDL_MOUSEWHEEL) {
                zoom *= (ev.wheel.y > 0 ? 1.12f : 0.9f);
                if (zoom < 0.35f) zoom = 0.35f;
                if (zoom > 3.0f)  zoom = 3.0f;
            }
        }

        Uint32 now = SDL_GetTicks();
        float dt = (now - last) / 1000.0f; last = now;
        if (dt > 0.05f) dt = 0.05f;
        float t = now / 1000.0f;

        // 行星跟手（缓动，有"质量感"）
        int mx, my; SDL_GetMouseState(&mx, &my);
        float tx = mx * sx, ty = my * sy;
        float k = 1.0f - expf(-FOLLOW * dt);
        px += (tx - px) * k;
        py += (ty - py) * k;

        // 蓄力累积
        if (holding) charge += CHARGE_RATE * dt;
        if (charge > 1.0f) charge = 1.0f;

        // 冲击波推进
        if (shockActive) {
            shockR += 700.0f * dt;
            shockA -= 1.6f * dt;
            if (shockA <= 0) shockActive = false;
        }

        // ---------- 物理更新 ----------
        for (auto& p : ps) {
            float gx = px - p.x, gy = py - p.y;
            float r2 = gx * gx + gy * gy + SOFT;
            float r = sqrtf(gx * gx + gy * gy) + 0.001f;
            // 引力
            float g = GM / r2 * dt;
            p.vx += gx / r * g;
            p.vy += gy / r * g;
            // 蓄力：内吸 + 旋涡
            if (holding) {
                float pull = CHARGE_PULL * charge * dt / (r + 20.0f);
                float sw   = CHARGE_SWIRL * charge * dt / (r + 20.0f);
                p.vx += (gx / r) * pull - (gy / r) * sw;
                p.vy += (gy / r) * pull + (gx / r) * sw;
            }
            // 阻尼 + 限速
            p.vx *= (1.0f - DAMP * dt);
            p.vy *= (1.0f - DAMP * dt);
            float sp2 = p.vx * p.vx + p.vy * p.vy;
            if (sp2 > MAXV * MAXV) { float sp = sqrtf(sp2); p.vx *= MAXV / sp; p.vy *= MAXV / sp; }
            // 积分
            p.x += p.vx * dt;
            p.y += p.vy * dt;
            // 被吸入 / 飞出画面 → 重生
            float d2 = (p.x - px) * (p.x - px) + (p.y - py) * (p.y - py);
            if (d2 < (CORE_R * 0.5f) * (CORE_R * 0.5f)) respawn(p);
            else if (p.x < -60 || p.x > CW + 60 || p.y < -60 || p.y > CH + 60) respawn(p);
        }

        // ---------- 渲染 ----------
        SDL_SetRenderTarget(ren, canvas);

        // 1) 淡出上一帧
        SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(ren, 2, 3, 11, (Uint8)FADE);
        SDL_Rect full = {0, 0, CW, CH};
        SDL_RenderFillRect(ren, &full);

        // 2) 星野
        SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
        for (auto& s : stars) {
            SDL_SetRenderDrawColor(ren, 150, 170, 220, s.a);
            SDL_Rect sr = {(int)s.x, (int)s.y, 1, 1};
            SDL_RenderFillRect(ren, &sr);
        }

        // 3) 冲击波
        if (shockActive) {
            SDL_SetTextureAlphaMod(ringTex, (Uint8)(shockA * 255));
            SDL_SetTextureColorMod(ringTex, 200, 230, 255);
            float rr = shockR * zoom;
            SDL_Rect dr = {(int)(px - rr), (int)(py - rr), (int)(2 * rr), (int)(2 * rr)};
            SDL_RenderCopy(ren, ringTex, NULL, &dr);
        }

        // 4) 粒子
        float baseHue = fmodf(t * 0.022f, 1.0f);
        for (auto& p : ps) {
            float dx = (p.x - px) * zoom, dy = (p.y - py) * zoom;
            int dpx = (int)(px + dx), dpy = (int)(py + dy);
            float h = paletteHue(fmodf(p.hue + baseHue, 1.0f));
            Uint8 r, g, b; hsv2rgb(h, 0.62f, 0.78f, r, g, b);
            SDL_SetTextureColorMod(glowTex, r, g, b);
            int d = (int)(p.size * 3.5f * zoom) + 1;
            SDL_Rect dr = {dpx - d / 2, dpy - d / 2, d, d};
            SDL_RenderCopy(ren, glowTex, NULL, &dr);
        }

        // （行星已移除：粒子直接绕光标引力中心环绕，蓄力时自然聚成明亮核心）

        SDL_SetRenderTarget(ren, NULL);

        // ---------- 画布贴回窗口 ----------
        SDL_Rect winRect = {0, 0, ww, wh};
        SDL_RenderCopy(ren, canvas, NULL, &winRect);
        SDL_RenderPresent(ren);

        // FPS 标题
        if (++frames, now - fpsTick >= 1000) {
            char title[160];
            snprintf(title, sizeof(title), "星尘漩涡 - %d FPS - %d 粒子 - 缩放 %.1fx%s",
                     frames, (int)ps.size(), zoom, holding ? " - 蓄力中" : "");
            SDL_SetWindowTitle(win, title);
            fpsTick = now; frames = 0;
        }

        SDL_Delay(1);
    }

    SDL_DestroyTexture(ringTex);
    SDL_DestroyTexture(glowTex);
    SDL_DestroyTexture(canvas);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}

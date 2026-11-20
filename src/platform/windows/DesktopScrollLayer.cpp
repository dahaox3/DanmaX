#include "platform/windows/DesktopScrollLayer.h"
#include "renderer/TextTextureCache.h"
#include <d3d11.h>
#include <dcomp.h>
#include <dcompanimation.h>
#include <dwmapi.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <algorithm>
#include <unordered_map>
#include <cmath>

using Microsoft::WRL::ComPtr;
namespace {
struct Sprite {
    ComPtr<IDCompositionVisual> visual;
    QString surfaceKey;
    int width{};
    int keyframes{};
    float y{};
    bool live{};
    bool shared{};
};
struct Bitmap {
    ComPtr<IDCompositionSurface> surface;
    qint64 bytes{};
    int references{};
};
// All curves use the same display-divisor clock, including the short, renewable
// curve used when an individual flight would exceed the segment budget.
constexpr double sharedHorizonSeconds = 4;
struct MotionBatch {
    LARGE_INTEGER boundary;
    double velocity;
    double cadence;
    double distanceSinceBoundary;
    bool renewShared;
    bool sharedRenewed{};
    int remainingKeyframes{DesktopScrollLayer::maxNewKeyframes};
};

HRESULT setSteppedOffset(IDCompositionDevice* device, auto* target, double x, int steps, const MotionBatch& batch) {
    if (!steps) return target->SetOffsetX(float(x));
    ComPtr<IDCompositionAnimation> animation;
    HRESULT hr = device->CreateAnimation(&animation);
    const double stepWidth = batch.velocity / batch.cadence;
    for (int i = 0; SUCCEEDED(hr) && i < steps; ++i) {
        hr = animation->AddCubic(i / batch.cadence, float(x - i * stepWidth), 0, 0, 0);
    }
    if (SUCCEEDED(hr)) hr = animation->End(steps / batch.cadence, float(x - steps * stepWidth));
    if (SUCCEEDED(hr)) hr = animation->SetAbsoluteBeginTime(batch.boundary);
    if (SUCCEEDED(hr)) hr = target->SetOffsetX(animation.Get());
    return hr;
}
}

struct DesktopScrollLayer::State {
    HWND hwnd{};
    ComPtr<ID3D11Device> gpu;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDCompositionDevice> device;
    ComPtr<IDCompositionTarget> target;
    ComPtr<IDCompositionVisual> root;
    ComPtr<IDCompositionTranslateTransform> sharedMotion;
    std::unordered_map<quint64, Sprite> sprites;
    QHash<QString, Bitmap> surfaces;
    qint64 bytes{};
    quint64 generation{};
    qreal dpr{}, opacity{-1};
    double speed{-1};
    LARGE_INTEGER sharedOrigin{}, frequency{};
    double cadence{}, requestedCadence{};
    quintptr gridScreen{};
    bool refreshAligned{};
    LARGE_INTEGER grid{};
    int individualVisuals{}, sharedVisuals{}, individualSteps{};

    HRESULT initialize(HWND window) {
        hwnd = window;
        QueryPerformanceFrequency(&frequency);
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                       nullptr, 0, D3D11_SDK_VERSION, &gpu, nullptr, &context);
        ComPtr<IDXGIDevice> dxgi;
        if (SUCCEEDED(hr)) hr = gpu.As(&dxgi);
        if (SUCCEEDED(hr)) hr = DCompositionCreateDevice(dxgi.Get(), IID_PPV_ARGS(&device));
        if (SUCCEEDED(hr)) hr = device->CreateTargetForHwnd(window, FALSE, &target);
        if (SUCCEEDED(hr)) hr = device->CreateVisual(&root);
        if (SUCCEEDED(hr)) hr = target->SetRoot(root.Get());
        return hr;
    }

    // A stair edge very close to the compositor's sampling instant can fall on either
    // side from one refresh to the next. Center edges between vblanks on the primary
    // output. DWM timing is desktop-wide, so other outputs keep their QPC clock.
    void resetGrid(double requested, quintptr primaryScreen, LARGE_INTEGER now) {
        requestedCadence = requested;
        gridScreen = primaryScreen;
        grid = now;
        cadence = requested;
        refreshAligned = false;
        if (!primaryScreen || frequency.QuadPart <= 0) return;
        DWM_TIMING_INFO timing{};
        timing.cbSize = sizeof(timing);
        if (FAILED(DwmGetCompositionTimingInfo(nullptr, &timing)) || !timing.qpcVBlank ||
            !timing.qpcRefreshPeriod || !timing.rateRefresh.uiNumerator || !timing.rateRefresh.uiDenominator) return;
        const double period = double(timing.qpcRefreshPeriod);
        const double hz = double(timing.rateRefresh.uiNumerator) / timing.rateRefresh.uiDenominator;
        const double multiple = hz / requested;
        if (multiple < .999 || multiple > 16.001) return;
        const int divisor = int(std::round(multiple));
        if (std::abs(hz / divisor - requested) > .01 * requested ||
            std::abs(period * hz / frequency.QuadPart - 1) > .001 ||
            std::abs(double(now.QuadPart) - double(timing.qpcVBlank)) > 4 * period) return;
        // Use the precise measured vblank period (the advertised rate can be rounded).
        cadence = double(frequency.QuadPart) / (period * divisor);
        grid.QuadPart = static_cast<LONGLONG>(timing.qpcVBlank) - std::llround(period / 2);
        refreshAligned = true;
    }

    HRESULT upload(const QImage& pixels, IDCompositionSurface** result) {
        if (pixels.isNull()) return E_OUTOFMEMORY;
        ComPtr<IDCompositionSurface> surface;
        HRESULT hr = device->CreateSurface(pixels.width(), pixels.height(), DXGI_FORMAT_B8G8R8A8_UNORM,
                                           DXGI_ALPHA_MODE_PREMULTIPLIED, &surface);
        ComPtr<IDXGISurface> drawing;
        POINT offset{};
        if (SUCCEEDED(hr)) hr = surface->BeginDraw(nullptr, IID_PPV_ARGS(&drawing), &offset);
        if (FAILED(hr)) return hr;
        ComPtr<IDXGISurface2> view;
        ComPtr<ID3D11Texture2D> texture;
        UINT subresource{};
        hr = drawing.As(&view);
        if (SUCCEEDED(hr)) hr = view->GetResource(IID_PPV_ARGS(&texture), &subresource);
        if (SUCCEEDED(hr)) {
            const UINT x = static_cast<UINT>(offset.x), y = static_cast<UINT>(offset.y);
            D3D11_BOX box{x, y, 0, x + UINT(pixels.width()), y + UINT(pixels.height()), 1};
            context->UpdateSubresource(texture.Get(), subresource, &box, pixels.constBits(), UINT(pixels.bytesPerLine()), 0);
        }
        texture.Reset(); view.Reset(); drawing.Reset();
        const HRESULT end = surface->EndDraw();
        if (SUCCEEDED(hr)) hr = end;
        if (SUCCEEDED(hr)) *result = surface.Detach();
        return hr;
    }

    HRESULT positionSprite(Sprite& sprite, double x, bool retime, MotionBatch& batch, bool& changed) {
        if (!retime && !sprite.shared) return S_OK;
        // The engine gives us x at this tick. Start the curve at the preceding
        // display boundary so a new or promoted line stays in phase with its neighbours.
        const double start = x + batch.distanceSinceBoundary;
        const int available = std::min(batch.remainingKeyframes, maxIndividualKeyframes - individualSteps);
        const double stepsToExit = batch.velocity > 0
            ? std::ceil(std::max(0.0, start + sprite.width) / batch.velocity * batch.cadence) : 0;
        HRESULT hr = S_OK;
        if (stepsToExit <= available) {
            // Check before converting: a slow player or a very long line can
            // request far more steps than fit in an int.
            const int steps = int(stepsToExit);
            if (sprite.shared) hr = sprite.visual->SetTransform(nullptr);
            if (SUCCEEDED(hr)) hr = setSteppedOffset(device.Get(), sprite.visual.Get(), start, steps, batch);
            if (FAILED(hr)) return hr;
            sprite.shared = false;
            sprite.keyframes = steps;
            individualSteps += steps;
            batch.remainingKeyframes -= steps;
            changed = true;
            return S_OK;
        }

        // Budget overflow shares one stepped translation. Its visual can take
        // an individual curve on a later tick, without moving other lines.
        if (!sharedMotion) hr = device->CreateTranslateTransform(&sharedMotion);
        if (SUCCEEDED(hr) && batch.renewShared && !batch.sharedRenewed) {
            hr = setSteppedOffset(device.Get(), sharedMotion.Get(), 0,
                                  int(std::ceil(sharedHorizonSeconds * batch.cadence)), batch);
            sharedOrigin = batch.boundary;
            batch.sharedRenewed = true;
        }
        if (SUCCEEDED(hr) && !sprite.shared) hr = sprite.visual->SetTransform(sharedMotion.Get());
        if (SUCCEEDED(hr) && (retime || !sprite.shared || batch.renewShared)) {
            const double elapsed = double(batch.boundary.QuadPart - sharedOrigin.QuadPart) / frequency.QuadPart;
            hr = sprite.visual->SetOffsetX(float(start + elapsed * batch.velocity));
            changed = true;
        }
        sprite.shared = true;
        sprite.keyframes = 0;
        return hr;
    }

    HRESULT sync(std::span<const DanmakuVisual> lines, qreal scale, int stroke, qreal alpha, double velocity,
                 double nextCadence, quint64 style, qint64 limit, quintptr primaryScreen, quint64& commits, quint64& uploads) {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        bool changed = false;
        const bool rebuild = dpr != scale || generation != style || opacity != alpha;
        if (rebuild) {
            HRESULT hr = root->RemoveAllVisuals();
            if (FAILED(hr)) return hr;
            sprites.clear(); surfaces.clear(); bytes = 0; individualSteps = 0;
            dpr = scale; generation = style; opacity = alpha; changed = true;
        }
        for (auto& [id, s] : sprites) s.live = false;
        for (const auto& line : lines) {
            if (line.scrolling) {
                auto it = sprites.find(line.id);
                if (it != sprites.end()) it->second.live = true;
            }
        }
        for (auto it = sprites.begin(); it != sprites.end();) {
            if (it->second.live) { ++it; continue; }
            HRESULT hr = root->RemoveVisual(it->second.visual.Get());
            if (FAILED(hr)) return hr;
            auto bitmap = surfaces.find(it->second.surfaceKey);
            if (--bitmap->references == 0) { bytes -= bitmap->bytes; surfaces.erase(bitmap); }
            individualSteps -= it->second.keyframes;
            it = sprites.erase(it);
            changed = true;
        }
        if (bytes > limit) return E_OUTOFMEMORY;
        const double nextSpeed = velocity * scale;
        if (!std::isfinite(nextSpeed) || nextSpeed < 0 || !std::isfinite(nextCadence) || nextCadence <= 0)
            return E_INVALIDARG;
        const bool resetClock = requestedCadence != nextCadence || gridScreen != primaryScreen;
        const bool retime = rebuild || speed != nextSpeed || resetClock;
        if (resetClock) resetGrid(nextCadence, primaryScreen, now);
        if (retime) individualSteps = 0; // All surviving curves will be replaced in this commit.
        const double period = frequency.QuadPart / cadence;
        const double slot = std::floor(double(now.QuadPart - grid.QuadPart) / period);
        LARGE_INTEGER boundary{};
        boundary.QuadPart = grid.QuadPart + std::llround(slot * period);
        MotionBatch batch{boundary, nextSpeed, cadence,
                          nextSpeed * double(now.QuadPart - boundary.QuadPart) / frequency.QuadPart,
                          retime || !sharedMotion ||
                            double(now.QuadPart - sharedOrigin.QuadPart) / frequency.QuadPart >= sharedHorizonSeconds - 1};
        individualVisuals = sharedVisuals = 0;
        for (const auto& line : lines) {
            if (!line.scrolling) continue;
            auto [it, added] = sprites.try_emplace(line.id);
            auto& sprite = it->second;
            HRESULT hr = S_OK;
            if (added) {
                auto desc = TextTextureCache::describe(*line.layout, line.color, stroke, scale);
                if (desc.ineligible != TextTextureCache::Reason::None) return E_INVALIDARG;
                auto bitmap = surfaces.find(desc.key);
                if (bitmap == surfaces.end()) {
                    if (desc.bytes > limit - bytes) return E_OUTOFMEMORY;
                    Bitmap image;
                    QImage pixels = TextTextureCache::rasterize(*line.layout, desc, line.color, stroke, scale);
                    if (pixels.isNull()) return E_OUTOFMEMORY;
                    if (alpha < 1) {
                        // Bake opacity once. A group effect on the moving root
                        // can require a viewport-sized intermediate every refresh.
                        QPainter tint(&pixels);
                        tint.setCompositionMode(QPainter::CompositionMode_DestinationIn);
                        tint.fillRect(QRectF(QPointF(), pixels.deviceIndependentSize()), QColor::fromRgbF(0, 0, 0, float(alpha)));
                    }
                    hr = upload(pixels, &image.surface);
                    if (FAILED(hr)) return hr;
                    image.bytes = desc.bytes;
                    bitmap = surfaces.insert(desc.key, std::move(image));
                    bytes += desc.bytes; ++uploads;
                }
                sprite.surfaceKey = desc.key;
                sprite.width = desc.pixels.width();
                ++bitmap->references;
                hr = device->CreateVisual(&sprite.visual);
                if (SUCCEEDED(hr)) hr = sprite.visual->SetContent(bitmap->surface.Get());
                if (SUCCEEDED(hr)) hr = root->AddVisual(sprite.visual.Get(), TRUE, nullptr);
            }
            const float y = float(line.y * scale);
            if (SUCCEEDED(hr) && (added || sprite.y != y)) {
                hr = sprite.visual->SetOffsetY(y);
                sprite.y = y; changed = true;
            }
            if (SUCCEEDED(hr)) hr = positionSprite(sprite, line.x * scale, added || retime, batch, changed);
            if (FAILED(hr)) return hr;
            individualVisuals += sprite.keyframes > 0;
            sharedVisuals += sprite.shared;
        }
        if (!sharedVisuals) sharedMotion.Reset();
        speed = nextSpeed;
        if (!changed) return S_OK;
        HRESULT hr = gpu->GetDeviceRemovedReason();
        if (SUCCEEDED(hr)) hr = device->Commit();
        if (SUCCEEDED(hr)) ++commits;
        return hr;
    }
};

DesktopScrollLayer::DesktopScrollLayer() = default;
DesktopScrollLayer::~DesktopScrollLayer() { reset(); }
qint64 DesktopScrollLayer::bytes() const { return state_ ? state_->bytes : 0; }
int DesktopScrollLayer::count() const { return state_ ? int(state_->sprites.size()) : 0; }
int DesktopScrollLayer::individualCount() const { return state_ ? state_->individualVisuals : 0; }
int DesktopScrollLayer::sharedCount() const { return state_ ? state_->sharedVisuals : 0; }
int DesktopScrollLayer::individualKeyframes() const { return state_ ? state_->individualSteps : 0; }
bool DesktopScrollLayer::refreshAligned() const { return state_ && state_->refreshAligned; }
double DesktopScrollLayer::animationCadence() const { return state_ ? state_->cadence : 0; }
void DesktopScrollLayer::reset() {
    if (state_ && state_->target) {
        state_->target->SetRoot(nullptr);
        state_->device->Commit();
    }
    state_.reset();
}
bool DesktopScrollLayer::synchronize(quintptr window, std::span<const DanmakuVisual> lines, qreal dpr,
                                    int stroke, qreal opacity, double speed, double cadence, quint64 generation, qint64 limit, quintptr primaryScreen) {
    HWND hwnd = reinterpret_cast<HWND>(window);
    if (state_ && state_->hwnd != hwnd) reset();
    HRESULT hr = S_OK;
    if (!state_) {
        state_ = std::make_unique<State>();
        hr = state_->initialize(hwnd);
    }
    if (SUCCEEDED(hr)) hr = state_->sync(lines, dpr, stroke, opacity, speed, cadence, generation, limit, primaryScreen, commits_, uploads_);
    lastError_ = hr;
    if (FAILED(hr)) reset();
    return SUCCEEDED(hr);
}

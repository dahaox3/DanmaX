#include "renderer/DanmakuItem.h"
#include "renderer/TextTextureCache.h"
#include <QSGSimpleTextureNode>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QMatrix4x4>
#include <QQuickWindow>
#include <QScreen>
#include <QSGTextNode>
#include <QVariantMap>
#include <algorithm>
#include <unordered_map>
#include <QSet>
#include <chrono>
#include <cmath>

struct DanmakuSceneState {
    QSGNode* root{};
    QQuickWindow* window{};
};
namespace {
qint64 snapshotNow() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
struct Entry {
    QSGTransformNode* node{};
    std::shared_ptr<QTextLayout> layout;
    quint64 frame{};
    std::shared_ptr<TextTextureCache::Resource> texture;
    TextTextureCache::Description description;
    QRgb color{};
    TextTextureCache::Reason reason{TextTextureCache::Reason::None};
};
class Root final : public QSGNode {
  public:
    explicit Root(QQuickWindow* window)
        : state(std::make_shared<DanmakuSceneState>(DanmakuSceneState{this, window})) { budgetTime.start(); }
    std::shared_ptr<DanmakuSceneState> state;
    quint64 generation{};
    qreal dpr{};
    quint64 frame{};
    std::unordered_map<quint64, Entry> entries;
    TextTextureCache textures;
    TextureBudget budget;
    QElapsedTimer budgetTime;
    qint64 demand{};
    void clearContent() {
        while (firstChild()) {
            auto* child = firstChild();
            removeChildNode(child);
            delete child;
        }
        std::unordered_map<quint64, Entry>().swap(entries);
        textures = TextTextureCache{};
        budget = TextureBudget{};
        demand = 0;
        budgetTime.restart();
    }
    ~Root() override { clearContent(); }
};
void removeChildren(QSGNode* group) {
    while (auto* child = group->firstChild()) {
        group->removeChildNode(child);
        delete child;
    }
}
void addImage(Entry& entry, std::shared_ptr<TextTextureCache::Resource> texture) {
    removeChildren(entry.node);
    auto* image = new QSGSimpleTextureNode;
    image->setTexture(texture->texture.get());
    image->setRect(QRectF(QPointF(), texture->size));
    image->setFiltering(QSGTexture::Linear);
    entry.node->appendChildNode(image);
    entry.texture = std::move(texture);
    entry.reason = TextTextureCache::Reason::None;
}
void addText(Entry& entry, QQuickWindow* window, int stroke) {
    removeChildren(entry.node);
    auto add = [&](QPointF offset, QColor color) {
        auto* text = window->createTextNode();
        text->setColor(color);
        text->setRenderType(QSGTextNode::QtRendering);
        text->addTextLayout(offset, entry.layout.get());
        entry.node->appendChildNode(text);
    };
    if (stroke > 1) {
        for (const auto& p : {QPointF(-stroke, 0), QPointF(stroke, 0), QPointF(0, -stroke), QPointF(0, stroke),
                             QPointF(-stroke * .7, -stroke * .7), QPointF(stroke * .7, -stroke * .7),
                             QPointF(-stroke * .7, stroke * .7), QPointF(stroke * .7, stroke * .7)})
            add(p + QPointF(stroke, stroke), Qt::black);
    }
    if (stroke == 1) {
        auto* text = window->createTextNode();
        text->setColor(QColor::fromRgba(entry.color));
        text->setTextStyle(QSGTextNode::Outline);
        text->setStyleColor(Qt::black);
        text->addTextLayout(QPointF(1, 1), entry.layout.get());
        entry.node->appendChildNode(text);
    } else add(QPointF(stroke, stroke), QColor::fromRgba(entry.color));
}
} // namespace
DanmakuItem::DanmakuItem(QQuickItem* parent) : QQuickItem(parent) {
    setFlag(ItemHasContents, true);
    setClip(true);
    connect(this, &QQuickItem::windowChanged, this, [this](QQuickWindow* window) {
        enableDesktopScroll(false);
        disconnect(invalidationConnection_);
        disconnect(stoppingConnection_);
        if (window) {
            stoppingConnection_ = connect(window, &QQuickWindow::sceneGraphAboutToStop, this, [this, window] {
                tracePoint_ = {};
                // All render loops emit this before hiding. Drop drawing resources here,
                // including temporary hides; GUI snapshots preserve IDs and paused positions.
                // A weak lifetime handle prevents access to a root already deleted by Qt.
                if (const auto scene = renderScene_.lock(); scene && scene->window == window)
                    static_cast<Root*>(scene->root)->clearContent();
                sceneEntries_.store(0);
                imageNodes_.store(0);
                textureBytes_.store(0);
                textureBudgetBytes_.store(0);
                textureDemandBytes_.store(0);
                capacityFallbacks_ = preparationFallbacks_ = sizeFallbacks_ = allocationFallbacks_ = 0;
            }, Qt::DirectConnection);
            invalidationConnection_ = connect(window, &QQuickWindow::sceneGraphInvalidated, this, [this] {
                tracePoint_ = {};
                // This signal runs on the render thread; do not touch GUI snapshots or caches.
                sceneEntries_.store(0);
                imageNodes_.store(0);
                textureBytes_.store(0);
                textureBudgetBytes_.store(0);
                textureDemandBytes_.store(0);
                capacityFallbacks_ = preparationFallbacks_ = sizeFallbacks_ = allocationFallbacks_ = 0;
                renderScene_.reset();
            }, Qt::DirectConnection);
        }
    });
}
void DanmakuItem::itemChange(ItemChange change, const ItemChangeData& data) {
    QQuickItem::itemChange(change, data);
    if (change == ItemDevicePixelRatioHasChanged && desktopActive_) emit desktopScaleChanged();
}
void DanmakuItem::enableDesktopScroll(bool enable) {
    if (desktopRequested_ == enable) return;
    desktopRequested_ = enable;
    desktopFailed_ = false;
    if (!enable) {
        desktop_.reset();
        desktopActive_ = false;
        desktopBytesForScene_ = 0;
    }
}
void DanmakuItem::clearContent() {
    desktop_.reset();
    desktopActive_ = false;
    desktopFailed_ = false;
    desktopBytesForScene_ = 0;
    std::vector<Visual>().swap(nextVisuals_);
    if (traceEnabled_) { ++snapshotSerial_; snapshotTimestamp_ = snapshotNow(); }
    std::vector<Visual>().swap(visuals_);
    std::vector<Visual>().swap(slotVisuals_);
    std::vector<std::size_t>().swap(previousSlots_);
    layouts_.clear();
    hits_ = misses_ = 0;
    ++generation_;
    update();
}
void DanmakuItem::invalidateLayoutCache() {
    layouts_.clear();
    slotVisuals_.clear();
    previousSlots_.clear();
    ++generation_;
    update();
}
bool DanmakuItem::configure(const QVariantMap& s) {
    QFont font(s["fontFamily"].toString());
    font.setPixelSize(s["fontSize"].toInt());
    font.setWeight(static_cast<QFont::Weight>(s.value("fontWeight", 600).toInt()));
    const bool uniform = s.value("uniformFontSize", false).toBool();
    const QColor color(s.value("textColor").toString());
    const int stroke = s["strokeWidth"].toInt();
    const bool dimensionsChanged = font_ != font || uniformFontSize_ != uniform || stroke_ != stroke || spacing_ != s["lineSpacing"].toDouble();
    if (font_ != font || uniformFontSize_ != uniform) layouts_.clear();
    if (font_ != font || uniformFontSize_ != uniform || textColor_ != color || stroke_ != stroke) {
        uniformFontSize_ = uniform; textColor_ = color;
        font_ = font;
        stroke_ = stroke;
        // Keep the current snapshot alive until present() atomically replaces it.
        slotVisuals_.clear();
        previousSlots_.clear();
        ++generation_;
    }
    spacing_ = s["lineSpacing"].toDouble();
    textureBudgetAuto_ = s.value("textureBudgetAuto", true).toBool();
    textureBudgetLimitMiB_ = std::clamp(s.value("textureBudgetMiB", 512).toInt(), 32, 1024);
    setOpacity(s["opacity"].toDouble());
    update();
    return dimensionsChanged;
}
QVariantMap DanmakuItem::textureFallbackReasons() const {
    return {{"capacity", capacityFallbacks_.load()}, {"preparation", preparationFallbacks_.load()},
            {"size", sizeFallbacks_.load()}, {"allocation", allocationFallbacks_.load()}};
}
DanmakuItem::Layout* DanmakuItem::layout(const danmaku::Item& item) {
    QFont font = font_;
    font.setPixelSize(uniformFontSize_ ? font_.pixelSize() : std::max(1, qRound(font_.pixelSize() * std::clamp(item.fontSize, 1, 200) / 25.0)));
    const auto content = QString::fromUtf8(item.text.data(), static_cast<qsizetype>(item.text.size()));
    const auto key = QString::number(font.pixelSize()) + QChar(0) + content;
    if (auto* found = layouts_.object(key)) {
        ++hits_;
        return found;
    }
    ++misses_;
    auto text = std::make_shared<QTextLayout>(content, font);
    text->setCacheEnabled(true);
    text->beginLayout();
    auto line = text->createLine();
    line.setLineWidth(100000);
    text->endLayout();
    auto* entry = new Layout{text};
    layouts_.insert(key, entry, static_cast<int>(1 + (key.size() * 64 + 1023) / 1024));
    return entry;
}
danmaku::Engine::Extent DanmakuItem::measure(const danmaku::Item& item) {
    const auto* entry = layout(item);
    return {entry->text->lineAt(0).naturalTextWidth() + stroke_ * 2,
            QFontMetricsF(entry->text->font()).height() * (1 + spacing_) + stroke_ * 2};
}
double DanmakuItem::trackHeight() const {
    return QFontMetricsF(font_).height() * (1 + spacing_) + stroke_ * 2;
}
void DanmakuItem::present(const danmaku::Engine& engine, double scrollingPixelsPerSecond) {
    snapshotTimestamp_ = snapshotNow();
    if (traceEnabled_) ++snapshotSerial_;
    scrollingPixelsPerSecond_ = std::isfinite(scrollingPixelsPerSecond) ? scrollingPixelsPerSecond : 0;
    const auto& active = engine.activeSlots();
    // Retire references before reusing slots; only live slots enter the scene snapshot.
    for (auto slot : previousSlots_)
        if (slot < slotVisuals_.size() && (slot >= active.size() || !active[slot].alive))
            slotVisuals_[slot] = {};
    slotVisuals_.resize(active.size());
    previousSlots_ = engine.activeIndices();
    nextVisuals_.clear();
    nextVisuals_.reserve(engine.activeCount());
    for (const auto i : engine.activeIndices()) {
        const auto& a = active[i];
        auto& visual = slotVisuals_[i];
        if (!a.alive) {
            visual = {};
            continue;
        }
        if (visual.id != a.id) {
            const auto& data = engine.items()[a.item];
            visual = {a.id, a.x, a.y, textColor_.isValid() ? textColor_.rgba() : 0xff000000 | data.color,
                      layout(data)->text, data.mode == danmaku::Mode::Scroll};
        } else {
            visual.x = a.x;
            visual.y = a.y;
        }
        nextVisuals_.push_back(visual);
    }
    if (desktopRequested_ && !desktopFailed_ && window() && window()->handle() && window()->isVisible()) {
        const auto screen = window()->screen();
        const double hz = screen && screen->refreshRate() >= 1 ? screen->refreshRate() : 60.0;
        desktopCadence_ = hz / std::max(1, (qRound(hz) + 79) / 80);
        const auto remaining = std::max<qint64>(0, textureBudgetLimitMiB_ * TextureBudget::MiB - textureBytes_.load());
        const quintptr primaryScreen = screen && screen == QGuiApplication::primaryScreen() ? reinterpret_cast<quintptr>(screen) : 0;
        desktopActive_ = desktop_.synchronize(window()->winId(), nextVisuals_, window()->effectiveDevicePixelRatio(),
            stroke_, opacity(), scrollingPixelsPerSecond_, desktopCadence_, generation_, remaining, primaryScreen);
        desktopBytesForScene_ = desktop_.bytes();
        if (!desktopActive_) {
            desktopFailed_ = true;
            qWarning("Windows composition scroll fallback (HRESULT 0x%08lx)", desktop_.lastError());
        }
    }
    if (desktopActive_) {
        std::erase_if(nextVisuals_, [](const Visual& v) { return v.scrolling; });
        if (nextVisuals_.size() == visuals_.size() &&
            std::equal(nextVisuals_.begin(), nextVisuals_.end(), visuals_.begin(), [](const Visual& a, const Visual& b) {
                return a.id == b.id && a.x == b.x && a.y == b.y && a.color == b.color && a.layout == b.layout;
            })) return; // DWM samples the motion even if the Qt scene is completely idle.
    }
    visuals_.swap(nextVisuals_);
    update();
}
QSGNode* DanmakuItem::updatePaintNode(QSGNode* old, UpdatePaintNodeData*) {
    // beginFrame can block on the compositor/GPU before scene synchronization.
    // Reproject only the drawn scrolling position to this synchronization time;
    // admission, collisions, lifetimes and seeks remain owned by the engine.
    const auto sceneNow = snapshotNow();
    const double age = snapshotTimestamp_ ? std::max(0.0, (sceneNow - snapshotTimestamp_) * 1e-9) : 0;
    if (traceEnabled_) {
        tracePoint_ = {};
        tracePoint_.snapshot = snapshotSerial_;
        tracePoint_.snapshotNs = snapshotTimestamp_;
        tracePoint_.sceneNs = sceneNow;
        tracePoint_.active = static_cast<int>(visuals_.size());
    }
    snapshotAgeMs_.store(age * 1000);
    const double correction = scrollingPixelsPerSecond_ * std::min(age, .1);
    auto* root = static_cast<Root*>(old);
    if (visuals_.empty()) {
        delete root;
        sceneEntries_.store(0);
        imageNodes_.store(0);
        textureBytes_.store(0);
        textureBudgetBytes_.store(0);
        textureDemandBytes_.store(0);
        capacityFallbacks_ = preparationFallbacks_ = sizeFallbacks_ = allocationFallbacks_ = 0;
        return nullptr;
    }
    const auto dpr = window()->effectiveDevicePixelRatio();
    if (root && (root->generation != generation_ || root->dpr != dpr)) {
        delete root;
        root = nullptr;
    }
    if (!root) {
        root = new Root(window());
        root->generation = generation_;
        root->dpr = dpr;
    }
    renderScene_ = root->state;
    ++root->frame;
    // Update membership without rebuilding keys or estimating text on steady frames.
    bool membershipChanged = false;
    for (const auto& visual : visuals_)
        if (auto it = root->entries.find(visual.id); it != root->entries.end())
            it->second.frame = root->frame;
    for (auto it = root->entries.begin(); it != root->entries.end();) {
        if (it->second.frame != root->frame) {
            root->removeChildNode(it->second.node);
            delete it->second.node;
            it = root->entries.erase(it);
            membershipChanged = true;
        } else ++it;
    }
    root->textures.retireUnused();
    for (const auto& visual : visuals_) {
        if (!visual.id || root->entries.contains(visual.id)) continue;
        auto* group = new QSGTransformNode;
        root->appendChildNode(group);
        Entry entry;
        entry.node = group;
        entry.layout = visual.layout;
        entry.color = visual.color;
        if (imageBackend_)
            entry.description = TextTextureCache::describe(*visual.layout, visual.color, stroke_, dpr);
        root->entries.emplace(visual.id, std::move(entry));
        membershipChanged = true;
    }
    if (membershipChanged && imageBackend_) {
        root->demand = 0;
        QSet<QString> unique;
        for (const auto& [id, entry] : root->entries) {
            Q_UNUSED(id);
            if (entry.description.ineligible != TextTextureCache::Reason::None ||
                unique.contains(entry.description.key)) continue;
            unique.insert(entry.description.key);
            root->demand += entry.description.bytes;
        }
    }
    const qint64 remaining = std::max<qint64>(0, textureBudgetLimitMiB_ * TextureBudget::MiB - desktopBytesForScene_);
    const qint64 budget = imageBackend_ ? std::min<qint64>(remaining, root->budget.update(root->demand, textureBudgetAuto_,
        textureBudgetLimitMiB_ * TextureBudget::MiB, root->budgetTime.elapsed())) : 0;
    root->textures.setBudget(budget);
    if (root->textures.bytes() > budget) {
        // Preserve the earliest live textures. Destroy borrowing image nodes before
        // dropping their shared resources; all changes stay on the render thread.
        QSet<const TextTextureCache::Resource*> kept;
        qint64 retained = 0;
        for (const auto& visual : visuals_) {
            auto& entry = root->entries.at(visual.id);
            if (!entry.texture) continue;
            if (kept.contains(entry.texture.get())) continue;
            if (entry.texture->bytes <= budget - retained) {
                retained += entry.texture->bytes;
                kept.insert(entry.texture.get());
            } else {
                addText(entry, window(), stroke_);
                entry.texture.reset();
                entry.reason = TextTextureCache::Reason::Capacity;
            }
        }
        root->textures.retireUnused();
    }
    root->textures.beginFrame();
    int images = 0, capacity = 0, preparation = 0, size = 0, allocation = 0;
    for (const auto& visual : visuals_) {
        if (!visual.id) continue;
        auto& entry = root->entries.at(visual.id);
        if (imageBackend_ && !entry.texture) {
            const auto result = root->textures.acquire(window(), *entry.layout, entry.description,
                                                       entry.color, stroke_, dpr);
            if (result.texture) addImage(entry, result.texture);
            else entry.reason = result.reason;
        }
        if (!entry.node->firstChild()) addText(entry, window(), stroke_);
        entry.frame = root->frame;
        const double x = visual.x - (visual.scrolling ? correction : 0);
        if (&visual == &visuals_.front()) firstRenderedX_.store(x);
        // setMatrix marks the subtree dirty even for an identical transform.
        // In particular a stationary bottom caption must not expand each
        // scrolling frame's damaged region to the full height of the window.
        const float tx = static_cast<float>(x), ty = static_cast<float>(visual.y);
        const auto& current = entry.node->matrix();
        if (current(0, 3) != tx || current(1, 3) != ty) {
            QMatrix4x4 matrix;
            matrix.translate(tx, ty);
            entry.node->setMatrix(matrix);
        }
        if (entry.texture) ++images;
        else if (imageBackend_) {
            switch (entry.reason) {
            case TextTextureCache::Reason::Capacity: ++capacity; break;
            case TextTextureCache::Reason::Preparation: ++preparation; break;
            case TextTextureCache::Reason::Dimensions:
            case TextTextureCache::Reason::SingleUpload: ++size; break;
            case TextTextureCache::Reason::Allocation: ++allocation; break;
            default: break;
            }
        }
    }
    if (traceEnabled_) {
        // Arrival order is stable until retirement. A fixed caption can be first.
        const auto it = std::find_if(visuals_.begin(), visuals_.end(), [](const Visual& v) { return v.scrolling; });
        if (it != visuals_.end()) {
            tracePoint_.id = it->id;
            tracePoint_.engineX = it->x;
            tracePoint_.drawnX = root->entries.at(it->id).node->matrix()(0, 3);
        }
    }
    imageNodes_.store(images);
    sceneEntries_.store(static_cast<int>(root->entries.size()));
    textureBytes_.store(root->textures.bytes());
    textureBudgetBytes_.store(budget);
    textureDemandBytes_.store(root->demand);
    capacityFallbacks_.store(capacity);
    preparationFallbacks_.store(preparation);
    sizeFallbacks_.store(size);
    allocationFallbacks_.store(allocation);
    return root;
}

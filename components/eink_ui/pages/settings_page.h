#pragma once
// ============================================================================
//  einkui/pages/settings_page.h
//
//  SettingsPage — hierarchical settings menu in a modern list style:
//
//    ‹  Audio                      ← back icon (sub-pages) + large title
//    Bass                +3.0 dB
//    ━━━━━━━━━━○┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄    ← rows separated by dotted dividers
//    ┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄
//    Dark Mode              (●  )
//    ┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄
//    EQ                         ›  ← addSubPage() rows get a chevron
// ============================================================================
#include "../include/element.h"
#include "../elements/button.h"
#include "../elements/toggle.h"
#include "../elements/slider.h"
#include "../elements/text_display.h"
#include "../elements/widgets.h"
#include <vector>
#include <functional>

namespace einkui {

class SettingsPage {
public:
    std::function<void(Parent*)> onNavigate;
    // Top-level page only: show a "‹" back button in the header that calls fn
    // (sub-pages always get a back button to their parent page).
    void setOnBack(std::function<void()> fn) {
        onBack = fn;
        if (!parent_ && backBtn_) {
            if (fn) backBtn_->clearStyle(STYLE_DISABLED).setSize(34, 30);
            else    backBtn_->addStyle(STYLE_DISABLED).setSize(1, 1);
        }
    }
    std::function<void()> onBack;

    explicit SettingsPage(const char* title, SettingsPage* parent=nullptr,
                          uint8_t hdrH=30)
        : title_(title), parent_(parent), hdrH_(hdrH)
    {
        root_ = std::make_shared<Parent>(title,
                    STYLE_DISPLAY_FLEX | STYLE_VERTICAL | STYLE_HIDE_LABEL);
        root_->setPadding(6).setSpacing(0);

        // ---- Header: [‹]  Title -------------------------------------------------
        auto* hdr = root_->add(
            new Parent("hdr", STYLE_DISPLAY_FLEX)).get();
        hdr->setHeight(hdrH_).setSpacing(2).setPadding(0).setMargin(0);

        // Back button: always created; on the top-level page it stays hidden
        // until setOnBack() gives it somewhere to go (e.g. the home screen).
        backBtn_ = hdr->add(new Button("&ui_back", 34, 30)).get();
        backBtn_->ghost().setIconSize(16).alignCenter();
        backBtn_->setMargin(0);
        backBtn_->cb().ctx       = this;
        backBtn_->cb().onTouchUp = [](void* ctx, Element*){
            auto* sp = static_cast<SettingsPage*>(ctx);
            if (sp->parent_) { if (sp->onNavigate) sp->onNavigate(sp->parent_->root_.get()); }
            else if (sp->onBack) sp->onBack();
        };
        if (!parent_) backBtn_->addStyle(STYLE_DISABLED).setSize(1, 1);
        titleElem_ = hdr->add(new TextDisplay(title)).get();
        titleElem_->setTextSize(TEXT_TITLE).textLeft().flexGrow().setHeight(hdrH_);
        titleElem_->setPadding(2).setMargin(0);

        root_->add(new Element("", STYLE_NO_FILL | STYLE_HIDE_LABEL, 0, 4))->setMargin(0);
    }

    // ---- add methods --------------------------------------------------------
    // Section header ("AUDIO ———") between groups of rows.
    void addSection(const char* text) {
        auto* d = new Divider(text);
        d->setMargin(0);
        d->setHeight(22);
        root_->add(d);
    }

    void addText(const char* text) {
        auto* t = root_->add(new TextDisplay(text)).get();
        t->setTextSize(TEXT_CAPTION).setHeight(18).setMargin(0);
        t->setPadding(4);
    }

    // Action row (no chevron).  Label may contain an icon: "&ui_home Home".
    Button* addButton(const char* label, TouchFn fn, void* ctx=nullptr) {
        auto* b = root_->add(new Button(label, 0, 0)).get();
        b->asListRow().showChevron(false).setMargin(0);
        b->setIconSize(16);
        b->cb().onTouchUp=fn; b->cb().ctx=ctx;
        return b;
    }

    Toggle* addToggle(const char* label, bool initial,
                      StringFn fn=nullptr, void* ctx=nullptr)
    {
        auto* t = root_->add(new Toggle(label, initial, 0, 0)).get();
        t->listRow().setMargin(0);
        if (fn) { t->cb().onChange=fn; t->cb().ctx=ctx; }
        return t;
    }

    Slider* addSlider(const char* label,
                      float minV, float maxV, float initial,
                      StringFn fn=nullptr, void* ctx=nullptr,
                      float step=0.f, const char* unit=nullptr,
                      uint8_t decimals=0)
    {
        auto* s = root_->add(new Slider(label, minV, maxV, 0, 40)).get();
        s->setValue(initial).setStep(step);
        if (unit)    s->setUnit(unit);
        if (decimals)s->setDecimals(decimals);
        if (minV < 0.f) s->showSign();
        s->listRow().setMargin(0);
        if (fn)      { s->cb().onChange=fn; s->cb().ctx=ctx; }
        return s;
    }

    // Sub-page: child SettingsPage navigated into via a button row
    SettingsPage* addSubPage(const char* label) {
        auto* child = new SettingsPage(label, this, hdrH_);
        child->onNavigate = onNavigate;
        children_.push_back(std::unique_ptr<SettingsPage>(child));

        auto* b = root_->add(new Button(label, 0, 0)).get();
        b->asListRow().setMargin(0);
        child->entryRow_ = b;
        b->cb().ctx = child;
        b->cb().onTouchUp = [](void* ctx, Element*){
            auto* sp = static_cast<SettingsPage*>(ctx);
            // Inherit parent's onNavigate before navigating
            if (sp->parent_ && sp->parent_->onNavigate)
                sp->onNavigate = sp->parent_->onNavigate;
            if (sp->onNavigate)
                sp->onNavigate(sp->root_.get());
        };
        return child;
    }

    std::shared_ptr<Parent> root() { return root_; }
    // The list row in the parent page that opens this sub-page (nullptr for
    // the top page).  Use it to show the current value: entryRow()->setTrailing("On")
    Button* entryRow() const { return entryRow_; }

    // Register all sub-page roots with the UI (call once after building)
    void registerPages(UI& ui) {
        ui.addPage(root_);
        for (auto& c : children_) c->registerPages(ui);
    }

private:
    const char*   title_;
    SettingsPage* parent_;
    uint8_t       hdrH_;
    TextDisplay*  titleElem_ = nullptr;
    Button*       backBtn_   = nullptr;
    Button*       entryRow_  = nullptr;
    std::shared_ptr<Parent> root_;
    std::vector<std::unique_ptr<SettingsPage>> children_;
};

} // namespace einkui
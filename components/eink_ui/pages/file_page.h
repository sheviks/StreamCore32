#pragma once
// ============================================================================
//  einkui/pages/file_page.h
//
//  FilePage  — FatFS/SDMMC file browser for EPD displays.
//
//  Features
//  --------
//  • Abstract IFileSystem interface — works with FatFS, SPIFFS, or any FS
//  • Folders listed first, then files, both sorted alphabetically
//  • File sizes shown right-aligned (B / KB / MB)
//  • Extension filter: only show files matching a set of extensions
//  • Paged list: ⌃/⌄ buttons + "1–8 / 23" indicator; rows per page follow
//    the real list height (works in portrait and landscape)
//  • Breadcrumb path shown in header; back button navigates up
//  • onFileSelected  callback fires with the full absolute path
//  • onDirChanged    callback fires when directory changes (optional)
//
//  IFileSystem
//  -----------
//  Implement once per filesystem type:
//
//    struct FatFSAdapter : einkui::IFileSystem {
//        bool list(const char* path,
//                  IFileSystem::Entry* out, size_t& count, size_t max) override
//        {
//            DIR dir; FILINFO fi;
//            if (f_opendir(&dir, path) != FR_OK) return false;
//            count = 0;
//            while (count < max) {
//                if (f_readdir(&dir, &fi) != FR_OK || fi.fname[0]==0) break;
//                strncpy(out[count].name, fi.fname, sizeof(out[count].name)-1);
//                out[count].name[sizeof(out[count].name)-1] = '\0';
//                out[count].isDir = (fi.fattrib & AM_DIR) != 0;
//                out[count].size  = fi.fsize;
//                ++count;
//            }
//            f_closedir(&dir);
//            return true;
//        }
//    };
//
//  Usage
//  -----
//    FatFSAdapter fs;
//    auto* browser = new einkui::FilePage(&fs, "/sdcard");
//    browser->setFilter({".mp3",".flac",".wav"});   // optional
//    browser->onFileSelected = [](void*, Element*, const char* path){ play(path); };
//    browser->onRefresh = []{ ui.markDirty(); };     // redraw after scroll / cd
//    ui.addPage(browser->root());
//
//  SDMMC / ESP-IDF note
//  ---------------------
//    Mount the SD card with esp_vfs_fat_sdmmc_mount() first.
//    Use rootPath = "/sdcard" (or wherever you mounted it).
//    FatFS via POSIX (opendir/readdir) works too — see PosixFSAdapter below.
//
//    struct PosixFSAdapter : einkui::IFileSystem {
//        bool list(const char* path,
//                  IFileSystem::Entry* out, size_t& count, size_t max) override
//        {
//            DIR* dir = opendir(path);
//            if (!dir) return false;
//            count = 0;
//            struct dirent* ent;
//            while (count < max && (ent = readdir(dir)) != nullptr) {
//                if (ent->d_name[0] == '.') continue;   // skip . and ..
//                strncpy(out[count].name, ent->d_name, sizeof(out[count].name)-1);
//                out[count].name[sizeof(out[count].name)-1] = '\0';
//                out[count].isDir = (ent->d_type == DT_DIR);
//                // Get file size via stat if needed
//                out[count].size = 0;
//                if (!out[count].isDir) {
//                    char full[256];
//                    snprintf(full, sizeof(full), "%s/%s", path, ent->d_name);
//                    struct stat st;
//                    if (stat(full, &st) == 0) out[count].size = (uint32_t)st.st_size;
//                }
//                ++count;
//            }
//            closedir(dir);
//            return true;
//        }
//    };
// ============================================================================
#include "../include/element.h"
#include "../elements/button.h"
#include "../elements/text_display.h"
#include <functional>
#include <strings.h>   // strcasecmp
#include <cstring>
#include <cstdio>
#include <algorithm>
#include <vector>
#include <string>
#include <initializer_list>

namespace einkui {

// ============================================================================
//  IFileSystem — implement this once per filesystem type
// ============================================================================
struct IFileSystem {
    struct Entry {
        char     name[128];   // filename (not full path)
        bool     isDir;
        uint32_t size;        // bytes; 0 for directories
    };
    /// Fill out[0..max-1], set count = number written. Return false on error.
    virtual bool list(const char* path,
                      Entry* out, size_t& count, size_t max) = 0;
    virtual ~IFileSystem() = default;
};

// ============================================================================
//  FileRow — one row in the list (internal element)
//    [icon]  name.ext ..........  12.3 MB      (files)
//    [icon]  Folder ............        ›      (folders)
// ============================================================================
class FileRow : public Element {
public:
    static constexpr uint16_t kRowH = 28;

    FileRow(const char* name, bool isDir, uint32_t size)
        : Element(name, STYLE_LIST_ROW, 0, kRowH),
          isDir_(isDir), size_(size)
    { margin_ = 0; }

    bool     isDir() const { return isDir_; }
    void     cancelTouch() override { pressed_ = false; }
    uint32_t size()  const { return size_;  }

    Rect measure(DrawCtx& ctx, Rect cursor, Rect area, uint16_t pStyle) override {
        (void)ctx;
        if (style_ & STYLE_DISPLAY_FIXED) return Rect(x,y,w,h);
        placeAt(cursor, pStyle);
        w = resolveW(area);
        h = int16_t(height_);
        return Rect(x,y,w,h);
    }

    void draw(DrawCtx& ctx, Colors colors) override {
        if (style_ & STYLE_DISABLED) return;
        Colors c = resolveColors(colors);
        ctx.fillRect(ctx.d, x, y, w, h, c.bg);
        uint16_t fg = c.fg, bg = c.bg;
        if (pressed_) {
            ctx.fillRoundRect(ctx.d, x, y, w, int16_t(h - 1), ctx.th().radiusSm, c.fg);
            std::swap(fg, bg);
        }
        const int16_t isz = 14, cy = int16_t(y + (h - 1) / 2);
        int16_t tx = int16_t(x + 4);
        ctx.icon(iconName(), int16_t(tx + isz / 2), cy, isz, fg, bg);
        tx = int16_t(tx + isz + 8);

        int16_t right = int16_t(x + w - 4);
        if (isDir_) {
            ctx.icon("ui_chevron_right", int16_t(right - 5), cy, 10, fg, bg);
            right = int16_t(right - 16);
        } else if (size_ > 0) {
            char szBuf[16]; fmtSize(szBuf, sizeof(szBuf));
            int16_t sw = ctx.textWidth(szBuf, TEXT_CAPTION);
            ctx.textBox(int16_t(right - sw), y, sw, h, szBuf, TEXT_CAPTION, fg, TEXT_HALIGN_RIGHT);
            right = int16_t(right - sw - 8);
        }
        ctx.textBox(tx, y, int16_t(right - tx), h, label_.c_str(),
                    isDir_ ? TEXT_BOLD : TEXT_BODY, fg);
        if (!pressed_) drawRowDivider(ctx, c);
    }

    bool onTouch(DrawCtx& /*ctx*/, const TTouchFrame& tf,
                 std::shared_ptr<Element>& focused) override
    {
        if (!hasCb()) return false;
        uint8_t ev = (uint8_t)tf.p[0].event;
        // rows are stacked edge to edge: never use the enlarged 2nd-pass target
        if (ev == 0 && touchExpand()) return false;
        if (ev == 0 && hitTest(tf.p[0].x, tf.p[0].y)) {
            focused = std::shared_ptr<Element>(this,[](Element*){});
            pressed_ = true;
            return true;
        }
        if (ev == 1 && focused.get() == this) {
            pressed_ = false;
            focused = nullptr;
            if (cb_->onTouchUp && releaseHit(tf.p[0].x, tf.p[0].y))
                cb_->onTouchUp(cb_->ctx, this);
            return true;
        }
        return false;
    }

private:
    bool     isDir_;
    uint32_t size_;
    bool     pressed_ = false;

    const char* iconName() const {
        if (isDir_) return "ui_folder";
        const char* dot = strrchr(label_.c_str(), '.');
        if (dot) {
            static const char* audio[] = { ".mp3", ".flac", ".wav", ".ogg", ".m4a", ".aac", ".opus" };
            for (auto* e : audio) if (strcasecmp(dot, e) == 0) return "ui_note";
        }
        return "ui_file";
    }

    void fmtSize(char* buf, size_t len) const {
        if      (size_ >= 1024*1024) snprintf(buf,len,"%.1f MB",(double)size_/(1024*1024));
        else if (size_ >= 1024)      snprintf(buf,len,"%.0f KB",(double)size_/1024);
        else                         snprintf(buf,len,"%u B",  (unsigned)size_);
    }
};

// ----------------------------------------------------------------------------
//  HookParent — Parent that tells its owner how tall it will be right before
//  laying out its children (used to fit the number of list rows).
// ----------------------------------------------------------------------------
class HookParent : public Parent {
public:
    using Hook = std::function<void(int16_t height)>;
    HookParent(const char* label, uint16_t style, Hook h) : Parent(label, style), hook_(h) {}
    Rect measure(DrawCtx& ctx, Rect cursor, Rect area, uint16_t pStyle) override {
        int16_t hh = height_ > 0 ? int16_t(height_) : int16_t(layoutH_);
        if (hook_ && hh > 0) hook_(int16_t(hh - padding_ * 2));
        return Parent::measure(ctx, cursor, area, pStyle);
    }
private:
    Hook hook_;
};

// ============================================================================
//  FilePage
// ============================================================================
static constexpr size_t FILE_PAGE_MAX_ENTRIES = 128;
static constexpr size_t FILE_PAGE_MAX_PATH    = 256;

class FilePage {
public:
    StringFn onFileSelected = nullptr;  // (ctx, element, fullPath)
    TouchFn  onDirChanged   = nullptr;  // (ctx, element) — element label = new path
    void*    cbCtx          = nullptr;
    // Called when "back" is pressed in the root folder (e.g. go home).
    TouchFn  onExit         = nullptr;
    // Called whenever the list content changed (scroll, folder change) and the
    // page needs a redraw — typically:  fp->onRefresh = []{ ui.markDirty(); };
    // (UI::onTouchFrame then redraws the whole page after the touch handler.)
    std::function<void()> onRefresh;

    explicit FilePage(IFileSystem* fs, const char* rootPath = "/")
        : fs_(fs)
    {
        strncpy(rootPath_, rootPath, sizeof(rootPath_)-1);
        strncpy(path_,     rootPath, sizeof(path_)-1);
        buildLayout();
        refresh();
    }

    std::shared_ptr<Parent> root() { return root_; }

    // ---- configuration -------------------------------------------------------
    /// Only show files whose extension matches one of the given strings.
    void setFilter(std::initializer_list<const char*> exts) {
        filter_.clear();
        for (auto* e : exts) filter_.push_back(e);
    }

    /// Row height in pixels (default 28).
    void setRowHeight(uint16_t h) { rowH_ = h; }

    // ---- navigation (callable externally too) --------------------------------
    void navigate(const char* subdir) {
        size_t len = strlen(path_);
        if (len > 0 && path_[len-1] != '/')
            strncat(path_, "/", sizeof(path_)-len-1);
        strncat(path_, subdir, sizeof(path_)-strlen(path_)-1);
        scrollOffset_ = 0;
        refresh();
        notifyDir();
        if (onRefresh) onRefresh();
    }

    void navigateUp() {
        char* slash = strrchr(path_, '/');
        if (!slash) return;
        if (strlen(path_) <= strlen(rootPath_)) {
            if (onExit) onExit(cbCtx, backBtn_);
            return;
        }
        *slash = '\0';
        if (strlen(path_) < strlen(rootPath_)) strncpy(path_, rootPath_, sizeof(path_)-1);
        scrollOffset_ = 0;
        refresh();
        notifyDir();
        if (onRefresh) onRefresh();
    }

    void navigateTo(const char* absPath) {
        strncpy(path_, absPath, sizeof(path_)-1);
        scrollOffset_ = 0;
        refresh();
    }

    const char* currentPath() const { return path_; }

private:
    IFileSystem* fs_;
    char  rootPath_[FILE_PAGE_MAX_PATH] = {};
    char  path_    [FILE_PAGE_MAX_PATH] = {};
    uint16_t rowH_        = FileRow::kRowH;
    int16_t  scrollOffset_= 0;   // index of first visible row
    int16_t  visibleRows_ = 6;   // updated from the real list height

    std::vector<std::string> filter_;

    std::shared_ptr<Parent> root_;
    Parent*      listArea_    = nullptr;
    Button*      backBtn_     = nullptr;
    Button*      scrollUp_    = nullptr;
    Button*      scrollDn_    = nullptr;
    TextDisplay* pathLabel_   = nullptr;
    TextDisplay* countLabel_  = nullptr;
    TextDisplay* pageLabel_   = nullptr;

    struct CachedEntry {
        std::string name;
        bool        isDir;
        uint32_t    size;
    };
    std::vector<CachedEntry> entries_;

    // ---- layout build (called once in constructor) ---------------------------
    void buildLayout() {
        root_ = std::make_shared<Parent>("files",
                    STYLE_DISPLAY_FLEX | STYLE_VERTICAL | STYLE_HIDE_LABEL);
        root_->setPadding(6).setSpacing(2);

        // ---- Header: [‹]  folder name            12 items ----------------------
        auto* hdr = root_->add(new Parent("hdr", STYLE_DISPLAY_FLEX)).get();
        hdr->setHeight(30).setSpacing(2).setPadding(0).setMargin(0);

        backBtn_ = hdr->add(new Button("&ui_back", 34, 30)).get();
        backBtn_->ghost().setIconSize(16).alignCenter();
        backBtn_->setMargin(0);
        backBtn_->cb().ctx       = this;
        backBtn_->cb().onTouchUp = [](void* ctx, Element*){
            static_cast<FilePage*>(ctx)->navigateUp();
        };

        pathLabel_ = hdr->add(new TextDisplay("")).get();
        pathLabel_->setTextSize(TEXT_TITLE).flexGrow().setHeight(30);
        pathLabel_->setPadding(2).setMargin(0);

        countLabel_ = hdr->add(new TextDisplay("")).get();
        countLabel_->setTextSize(TEXT_CAPTION).textRight().setWidth(48).setHeight(30);
        countLabel_->setPadding(2).setMargin(0);

        // ---- List (fills the page) -------------------------------------------
        auto* la = new HookParent("list", STYLE_DISPLAY_FLEX | STYLE_VERTICAL,
            [this](int16_t hgt){
                int16_t rows = std::max<int16_t>(1, int16_t(hgt / int16_t(rowH_)));
                if (rows != visibleRows_) {
                    visibleRows_ = rows;
                    int16_t maxScroll = std::max<int16_t>(0, int16_t(entries_.size()) - visibleRows_);
                    if (scrollOffset_ > maxScroll) scrollOffset_ = maxScroll;
                    rebuildList();
                }
            });
        listArea_ = la;
        root_->add(std::shared_ptr<Parent>(la));
        listArea_->flexGrow().setPadding(0).setSpacing(0);
        listArea_->setMargin(0);

        // ---- Pager footer: [⌃]   1–6 / 10   [⌄] -------------------------------
        auto* foot = root_->add(new Parent("pager", STYLE_DISPLAY_FLEX)).get();
        foot->setHeight(30).setSpacing(4).setPadding(0).setMargin(0);

        scrollUp_ = foot->add(new Button("&ui_chevron_up", 44, 28)).get();
        scrollUp_->setIconSize(14);
        scrollUp_->setMargin(0);
        scrollUp_->cb().ctx       = this;
        scrollUp_->cb().onTouchUp = [](void* ctx, Element*){
            auto* fp = static_cast<FilePage*>(ctx);
            if (fp->scrollOffset_ > 0) {
                fp->scrollOffset_ = std::max<int16_t>(0, int16_t(fp->scrollOffset_ - fp->visibleRows_));
                fp->rebuildList();
                if (fp->onRefresh) fp->onRefresh();
            }
        };

        pageLabel_ = foot->add(new TextDisplay("")).get();
        pageLabel_->setTextSize(TEXT_CAPTION).textCenter().flexGrow().setHeight(28);
        pageLabel_->setMargin(0);

        scrollDn_ = foot->add(new Button("&ui_chevron_down", 44, 28)).get();
        scrollDn_->setIconSize(14);
        scrollDn_->setMargin(0);
        scrollDn_->cb().ctx       = this;
        scrollDn_->cb().onTouchUp = [](void* ctx, Element*){
            auto* fp = static_cast<FilePage*>(ctx);
            int16_t maxScroll = std::max<int16_t>(0,
                int16_t(fp->entries_.size()) - fp->visibleRows_);
            if (fp->scrollOffset_ < maxScroll) {
                fp->scrollOffset_ = std::min<int16_t>(maxScroll, int16_t(fp->scrollOffset_ + fp->visibleRows_));
                fp->rebuildList();
                if (fp->onRefresh) fp->onRefresh();
            }
        };
    }

    void notifyDir() { if (onDirChanged) onDirChanged(cbCtx, pathLabel_); }

    // ---- refresh: re-read directory and rebuild list -------------------------
    void refresh() {
        // Header shows the current folder name (root → "Files")
        const char* display = path_;
        size_t rl = strlen(rootPath_);
        if (strncmp(path_, rootPath_, rl) == 0 && path_[rl] != '\0') display = path_ + rl;
        else display = "";
        const char* slash = strrchr(display, '/');
        if (slash && slash[1]) display = slash + 1;
        pathLabel_->setDynLabel(*display ? display : "Files");

        entries_.clear();
        if (fs_) {
            static IFileSystem::Entry raw[FILE_PAGE_MAX_ENTRIES];   // static: ~17 KB, keep it off the stack
            size_t count = 0;
            if (fs_->list(path_, raw, count, FILE_PAGE_MAX_ENTRIES)) {
                for (size_t i = 0; i < count; ++i) {
                    if (!raw[i].isDir && !passesFilter(raw[i].name)) continue;
                    entries_.push_back({raw[i].name, raw[i].isDir, raw[i].size});
                }
            }
        }

        std::sort(entries_.begin(), entries_.end(),
            [](const CachedEntry& a, const CachedEntry& b){
                if (a.isDir != b.isDir) return a.isDir > b.isDir;
                const char* an = a.name.c_str();
                const char* bn = b.name.c_str();
                while (*an && *bn) {
                    char ac = *an | 0x20, bc = *bn | 0x20;
                    if (ac != bc) return ac < bc;
                    ++an; ++bn;
                }
                return *an < *bn;
            });

        char cBuf[16];
        snprintf(cBuf, sizeof(cBuf), "%u item%s", (unsigned)entries_.size(), entries_.size() == 1 ? "" : "s");
        countLabel_->setDynLabel(cBuf);

        scrollOffset_ = 0;
        rebuildList();
    }

    // ---- rebuildList: populate listArea_ with the visible window of rows -----
    void rebuildList() {
        listArea_->clearChildren();

        if (entries_.empty()) {
            auto* empty = listArea_->add(new TextDisplay("This folder is empty")).get();
            empty->setHeight(40).textCenter().setTextSize(TEXT_CAPTION);
            updatePager();
            return;
        }

        visibleRows_ = std::max<int16_t>(1, visibleRows_);
        int16_t start = scrollOffset_;
        int16_t end   = std::min<int16_t>(int16_t(entries_.size()), int16_t(start + visibleRows_));

        for (int16_t i = start; i < end; ++i) {
            auto& e   = entries_[i];
            auto* row = listArea_->add(new FileRow(e.name.c_str(), e.isDir, e.size)).get();
            row->setHeight(rowH_);
            row->cb().ctx = this;
            if (e.isDir) {
                row->cb().onTouchUp = [](void* ctx, Element* el){
                    static_cast<FilePage*>(ctx)->navigate(el->label().c_str());
                };
            } else {
                row->cb().onTouchUp = [](void* ctx, Element* el){
                    auto* fp = static_cast<FilePage*>(ctx);
                    std::string full = std::string(fp->path_) + "/" + el->label();
                    if (fp->onFileSelected) fp->onFileSelected(fp->cbCtx, el, full.c_str());
                };
            }
        }
        updatePager();
    }

    void updatePager() {
        int16_t n = int16_t(entries_.size());
        int16_t maxScroll = std::max<int16_t>(0, int16_t(n - visibleRows_));
        char buf[40];
        if (n == 0) buf[0] = 0;
        else snprintf(buf, sizeof(buf), "%d-%d of %d", scrollOffset_ + 1,
                      std::min<int16_t>(n, int16_t(scrollOffset_ + visibleRows_)), n);
        pageLabel_->setDynLabel(buf);
        // Pager buttons are hidden when there is nothing to scroll
        if (scrollUp_) { if (scrollOffset_ <= 0) scrollUp_->addStyle(STYLE_DISABLED); else scrollUp_->clearStyle(STYLE_DISABLED); }
        if (scrollDn_) { if (scrollOffset_ >= maxScroll) scrollDn_->addStyle(STYLE_DISABLED); else scrollDn_->clearStyle(STYLE_DISABLED); }
    }

    bool passesFilter(const char* name) const {
        if (filter_.empty()) return true;
        const char* dot = strrchr(name, '.');
        if (!dot) return false;
        char ext[16]; size_t ei=0;
        for (const char* p=dot; *p && ei<sizeof(ext)-1; ++p,++ei)
            ext[ei] = (*p>='A'&&*p<='Z') ? char(*p|0x20) : *p;
        ext[ei] = '\0';
        for (auto& f : filter_)
            if (f == ext) return true;
        return false;
    }
};

} // namespace einkui

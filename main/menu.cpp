#include "menu.hpp"

#include <cstdio>
#include <cstring>

#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "M5Unified.h"

#include "profiles.hpp"
#include "soft_keyboard.hpp"
#include "wifi_config.hpp"

namespace tab5 {

namespace {

constexpr const char* kTag = "menu";

// Menu occupies the left + centre columns of the screen; the right
// 160 px belongs to the status panel and is never covered.
constexpr int kScreenW = 1120;
constexpr int kScreenH = 720;

constexpr uint16_t kBg            = 0x10A2;
constexpr uint16_t kHeaderBg      = 0x2965;
constexpr uint16_t kHeaderFg      = 0xFFFF;
constexpr uint16_t kRowBg         = 0x18C3;
constexpr uint16_t kRowBgSelected = 0x2A8E;
constexpr uint16_t kRowFg         = 0xFFFF;
constexpr uint16_t kRowEdge       = 0x4208;
constexpr uint16_t kAccent        = 0x07E0;
constexpr uint16_t kDangerBg      = 0x7800;
constexpr uint16_t kDangerFg      = 0xFFFF;
constexpr uint16_t kAddBg         = 0x0410;
constexpr uint16_t kAddFg         = 0xFFFF;
constexpr uint16_t kCloseBg       = 0x4208;
constexpr uint16_t kEditBg        = 0x4A89;
constexpr uint16_t kEditFg        = 0xFFFF;
constexpr uint16_t kFieldBg       = 0x18C3;
constexpr uint16_t kFieldBgFocus  = 0x3186;
constexpr uint16_t kSaveBg        = 0x0410;
constexpr uint16_t kSaveFg        = 0xFFFF;

// Header layout
constexpr int kHeaderH = 60;
constexpr int kCloseX  = kScreenW - 60;
constexpr int kCloseW  = 50;
constexpr int kCloseY  = 5;
constexpr int kCloseH  = 50;

// Row layout
constexpr int kRowY0   = kHeaderH + 10;
constexpr int kRowH    = 80;
constexpr int kRowGap  = 6;
constexpr int kRowX    = 20;
constexpr int kRowW    = kScreenW - 40;          // 1080
constexpr int kDelW    = 130;
constexpr int kDelX    = kRowX + kRowW - kDelW;  // right-aligned, 970
constexpr int kEditW   = 130;
constexpr int kEditX   = kDelX - 10 - kEditW;    // 830

// Footer buttons on ProfileList: [Wi-Fi] | [Manage TOFU] | [+ Add new]
// Three buttons, 200 px wide each, 30 px gaps, centred.
constexpr int kAddW     = 200;
constexpr int kAddH     = 60;
constexpr int kFooterY  = kScreenH - kAddH - 20;
constexpr int kAddY     = kFooterY;
constexpr int kWifiBtnW = 200;
constexpr int kTofuMgrW = 200;
constexpr int kFooterTotal = kWifiBtnW + 30 + kTofuMgrW + 30 + kAddW;
constexpr int kWifiBtnX = (kScreenW - kFooterTotal) / 2;
constexpr int kTofuMgrX = kWifiBtnX + kWifiBtnW + 30;
constexpr int kAddX     = kTofuMgrX + kTofuMgrW + 30;

// TofuList layout (re-uses some ProfileList constants for visual
// consistency).
constexpr int kTofuRowH   = 70;
constexpr int kTofuRowY0  = kHeaderH + 10;
constexpr int kTofuBackX  = 20;
constexpr int kTofuBackW  = 200;

// Profile editor layout — everything must fit above the soft keyboard
// panel at y=336.
constexpr int kEdHeaderH = 50;
constexpr int kEdFormY0  = kEdHeaderH + 6;
constexpr int kEdFieldH  = 40;
constexpr int kEdLabelX  = 20;
constexpr int kEdLabelW  = 200;
constexpr int kEdValueX  = kEdLabelX + kEdLabelW + 10;
constexpr int kEdValueW  = kScreenW - kEdValueX - 20;
// Editor lives above the soft keyboard panel (y >= 336) — that's
// where the kbd takes over the LCD.
constexpr int kEditorBottom = 336;

constexpr int kEdBtnY    = 290;
constexpr int kEdBtnH    = 40;
constexpr int kEdBtnW    = 160;
constexpr int kEdSaveX   = kScreenW - kEdBtnW - 20;
constexpr int kEdCancelX = kEdSaveX - kEdBtnW - 10;
constexpr int kEdShowPwW = 180;
constexpr int kEdShowPwX = 20;

// Field definitions
enum class FieldKind : uint8_t { Text, Number, Toggle };
struct FieldDef {
    const char* label;
    FieldKind   kind;
    bool        mask;        // password rendering
};
constexpr FieldDef kFields[] = {
    {"Name",     FieldKind::Text,   false},
    {"Protocol", FieldKind::Toggle, false},
    {"Host",     FieldKind::Text,   false},
    {"Port",     FieldKind::Number, false},
    {"User",     FieldKind::Text,   false},
    {"Password", FieldKind::Text,   true},
};
constexpr int kFieldCount = static_cast<int>(sizeof(kFields) / sizeof(kFields[0]));

}  // namespace

Menu menu;

Menu::Menu() = default;

void Menu::open() {
    if (state_ != State::Hidden) return;
    state_ = State::ProfileList;
    // The ProfileList view paints full screen (above the soft kbd);
    // hide the kbd so its panel doesn't show through.
    if (kbd_ && kbd_->visible()) kbd_->toggle();
    if (repaint_) repaint_();
}

void Menu::close() {
    if (state_ == State::Hidden) return;
    state_ = State::Hidden;
    if (repaint_) repaint_();
}

// ---------- hit-tests ----------

int Menu::hit_row(int y) const {
    if (y < kRowY0) return -1;
    int rel = y - kRowY0;
    int row = rel / (kRowH + kRowGap);
    int row_top = kRowY0 + row * (kRowH + kRowGap);
    if (y >= row_top + kRowH) return -1;          // in the gap
    if (row < 0 || row >= profiles.count()) return -1;
    return row;
}

bool Menu::hit_close(int x, int y) const {
    return x >= kCloseX && x < kCloseX + kCloseW &&
           y >= kCloseY && y < kCloseY + kCloseH;
}

bool Menu::hit_delete(int x) const {
    return x >= kDelX && x < kDelX + kDelW;
}

bool Menu::hit_edit(int x) const {
    return x >= kEditX && x < kEditX + kEditW;
}

bool Menu::hit_add(int x, int y) const {
    return x >= kAddX && x < kAddX + kAddW &&
           y >= kAddY && y < kAddY + kAddH;
}

bool Menu::hit_manage_tofu(int x, int y) const {
    return x >= kTofuMgrX && x < kTofuMgrX + kTofuMgrW &&
           y >= kFooterY  && y < kFooterY  + kAddH;
}

bool Menu::hit_manage_wifi(int x, int y) const {
    return x >= kWifiBtnX && x < kWifiBtnX + kWifiBtnW &&
           y >= kFooterY  && y < kFooterY  + kAddH;
}

// Wi-Fi edit form layout. Three fields stacked above the kbd panel.
namespace {
constexpr int kWifiHeaderH = 50;
constexpr int kWifiFormY0  = kWifiHeaderH + 10;
constexpr int kWifiFieldH  = 50;
constexpr int kWifiLabelX  = 20;
constexpr int kWifiLabelW  = 160;
constexpr int kWifiValueX  = kWifiLabelX + kWifiLabelW + 10;
constexpr int kWifiValueW  = kScreenW - kWifiValueX - 20;
constexpr int kWifiBtnY    = kWifiFormY0 + 3 * kWifiFieldH + 10;  // ~230
constexpr int kWifiBtnH    = 40;
constexpr int kWifiBtnGapW = 160;
constexpr int kWifiSaveX   = kScreenW - kWifiBtnGapW - 20;
constexpr int kWifiCancelX = kWifiSaveX - kWifiBtnGapW - 10;
constexpr int kWifiShowX   = 20;
constexpr int kWifiShowW   = 180;
}  // namespace

int Menu::hit_wifi_field(int x, int y) const {
    if (x < kWifiValueX || x >= kWifiValueX + kWifiValueW) return -1;
    int rel = y - kWifiFormY0;
    if (rel < 0) return -1;
    int idx = rel / kWifiFieldH;
    if (idx < 0 || idx > 2) return -1;
    int row_top = kWifiFormY0 + idx * kWifiFieldH;
    if (y >= row_top + kWifiFieldH - 4) return -1;
    return idx;
}

bool Menu::hit_wifi_save(int x, int y) const {
    return x >= kWifiSaveX && x < kWifiSaveX + kWifiBtnGapW &&
           y >= kWifiBtnY  && y < kWifiBtnY  + kWifiBtnH;
}

bool Menu::hit_wifi_cancel(int x, int y) const {
    return x >= kWifiCancelX && x < kWifiCancelX + kWifiBtnGapW &&
           y >= kWifiBtnY    && y < kWifiBtnY    + kWifiBtnH;
}

bool Menu::hit_wifi_show_pw(int x, int y) const {
    return x >= kWifiShowX && x < kWifiShowX + kWifiShowW &&
           y >= kWifiBtnY  && y < kWifiBtnY  + kWifiBtnH;
}

int Menu::hit_tofu_row(int y) const {
    if (y < kTofuRowY0) return -1;
    int rel = y - kTofuRowY0;
    int row = rel / (kTofuRowH + kRowGap);
    int row_top = kTofuRowY0 + row * (kTofuRowH + kRowGap);
    if (y >= row_top + kTofuRowH) return -1;
    if (row < 0 || row >= static_cast<int>(tofu_.entries.size())) return -1;
    return row;
}

bool Menu::hit_tofu_delete(int x) const {
    return x >= kDelX && x < kDelX + kDelW;
}

bool Menu::hit_tofu_back(int x, int y) const {
    return x >= kTofuBackX && x < kTofuBackX + kTofuBackW &&
           y >= kFooterY   && y < kFooterY   + kAddH;
}

int Menu::hit_editor_field(int x, int y) const {
    if (x < kEdValueX || x >= kEdValueX + kEdValueW) return -1;
    int rel = y - kEdFormY0;
    if (rel < 0) return -1;
    int idx = rel / kEdFieldH;
    if (idx < 0 || idx >= kFieldCount) return -1;
    int row_top = kEdFormY0 + idx * kEdFieldH;
    if (y >= row_top + kEdFieldH - 4) return -1;  // gap
    return idx;
}

bool Menu::hit_editor_save(int x, int y) const {
    return x >= kEdSaveX && x < kEdSaveX + kEdBtnW &&
           y >= kEdBtnY  && y < kEdBtnY  + kEdBtnH;
}

bool Menu::hit_editor_cancel(int x, int y) const {
    return x >= kEdCancelX && x < kEdCancelX + kEdBtnW &&
           y >= kEdBtnY    && y < kEdBtnY    + kEdBtnH;
}

bool Menu::hit_editor_show_pw(int x, int y) const {
    return x >= kEdShowPwX && x < kEdShowPwX + kEdShowPwW &&
           y >= kEdBtnY    && y < kEdBtnY    + kEdBtnH;
}

// ---------- drawing ----------

namespace {

void draw_button(int x, int y, int w, int h, const char* label,
                 uint16_t bg, uint16_t fg, bool pressed) {
    auto& d = M5.Display;
    if (pressed) bg = static_cast<uint16_t>((bg >> 1) & 0x7BEF);  // dim
    d.fillRoundRect(x, y, w, h, 8, bg);
    d.drawRoundRect(x, y, w, h, 8, kRowEdge);
    d.setTextColor(fg, bg);
    d.setTextDatum(middle_center);
    d.setFont(&fonts::lgfxJapanGothic_24);
    d.drawString(label, x + w / 2, y + h / 2);
}

const char* proto_label(ConnProto p) {
    return p == ConnProto::SSH ? "ssh" : "telnet";
}

}  // namespace

void Menu::render_profile_list() {
    auto& d = M5.Display;
    d.fillRect(0, 0, kScreenW, kScreenH, kBg);

    // Header
    d.fillRect(0, 0, kScreenW, kHeaderH, kHeaderBg);
    d.setTextColor(kHeaderFg, kHeaderBg);
    d.setTextDatum(middle_left);
    d.setFont(&fonts::lgfxJapanGothic_28);
    char title[64];
    snprintf(title, sizeof(title), "Profiles  (%d / %d)",
             profiles.count(), profiles.max());
    d.drawString(title, 24, kHeaderH / 2);
    draw_button(kCloseX, kCloseY, kCloseW, kCloseH, "X",
                kCloseBg, kHeaderFg, pressed_idx_ == -2);

    // Rows
    for (int i = 0; i < profiles.count(); ++i) {
        int y = kRowY0 + i * (kRowH + kRowGap);
        bool sel = (i == profiles.selected());
        bool press_row = (pressed_idx_ == i && pressed_btn_ == 0);
        bool press_del = (pressed_idx_ == i && pressed_btn_ == 1);
        bool press_ed  = (pressed_idx_ == i && pressed_btn_ == 2);

        uint16_t bg = sel ? kRowBgSelected : kRowBg;
        if (press_row) bg = static_cast<uint16_t>((bg >> 1) & 0x7BEF);
        d.fillRoundRect(kRowX, y, kRowW, kRowH, 10, bg);
        d.drawRoundRect(kRowX, y, kRowW, kRowH, 10, kRowEdge);

        if (sel) {
            d.fillRect(kRowX + 6, y + 8, 6, kRowH - 16, kAccent);
        }

        auto p = profiles.get(i);
        if (p) {
            d.setTextColor(kRowFg, bg);
            d.setFont(&fonts::lgfxJapanGothic_24);
            d.setTextDatum(top_left);
            char buf[96];
            snprintf(buf, sizeof(buf), "%s", p->name);
            d.drawString(buf, kRowX + 30, y + 10);

            d.setFont(&fonts::lgfxJapanGothic_20);
            char line2[160];
            snprintf(line2, sizeof(line2),
                     "%s  %s%s%s:%u",
                     proto_label(p->proto),
                     p->user[0] ? p->user : "",
                     p->user[0] ? "@" : "",
                     p->host, p->port);
            d.drawString(line2, kRowX + 30, y + 44);
        }

        draw_button(kEditX, y + 14, kEditW, kRowH - 28,
                    "Edit", kEditBg, kEditFg, press_ed);
        draw_button(kDelX + 10, y + 14, kDelW - 20, kRowH - 28,
                    "Delete", kDangerBg, kDangerFg, press_del);
    }

    // Empty-state hint
    if (profiles.count() == 0) {
        d.setTextColor(kRowFg, kBg);
        d.setFont(&fonts::lgfxJapanGothic_24);
        d.setTextDatum(middle_center);
        d.drawString("No profiles yet — tap \"+ Add\" below",
                     kScreenW / 2, kScreenH / 2);
    }

    // Footer buttons: [Wi-Fi] | [Manage TOFU] | [+ Add new]
    bool press_add  = (pressed_idx_ == -3);
    bool press_tofu = (pressed_idx_ == -4);
    bool press_wifi = (pressed_idx_ == -5);
    draw_button(kWifiBtnX, kFooterY, kWifiBtnW, kAddH, "Wi-Fi",
                kEditBg, kEditFg, press_wifi);
    draw_button(kTofuMgrX, kFooterY, kTofuMgrW, kAddH, "Manage TOFU",
                kEditBg, kEditFg, press_tofu);
    draw_button(kAddX, kAddY, kAddW, kAddH, "+ Add new",
                kAddBg, kAddFg, press_add);
}

void Menu::render_tofu_list() {
    auto& d = M5.Display;
    d.fillRect(0, 0, kScreenW, kScreenH, kBg);

    // Header
    d.fillRect(0, 0, kScreenW, kHeaderH, kHeaderBg);
    d.setTextColor(kHeaderFg, kHeaderBg);
    d.setTextDatum(middle_left);
    d.setFont(&fonts::lgfxJapanGothic_28);
    char title[64];
    snprintf(title, sizeof(title), "TOFU entries  (%u)",
             static_cast<unsigned>(tofu_.entries.size()));
    d.drawString(title, 24, kHeaderH / 2);
    draw_button(kCloseX, kCloseY, kCloseW, kCloseH, "X",
                kCloseBg, kHeaderFg, pressed_idx_ == -2);

    // Rows
    if (tofu_.entries.empty()) {
        d.setTextColor(kRowFg, kBg);
        d.setFont(&fonts::lgfxJapanGothic_24);
        d.setTextDatum(middle_center);
        d.drawString("No saved fingerprints yet",
                     kScreenW / 2, kScreenH / 2);
    }
    for (size_t i = 0; i < tofu_.entries.size(); ++i) {
        int y = kTofuRowY0 + static_cast<int>(i) * (kTofuRowH + kRowGap);
        bool press_del = (tofu_.pressed_idx == static_cast<int>(i) &&
                          tofu_.pressed_btn == 1);

        uint16_t bg = kRowBg;
        d.fillRoundRect(kRowX, y, kRowW, kTofuRowH, 10, bg);
        d.drawRoundRect(kRowX, y, kRowW, kTofuRowH, 10, kRowEdge);

        const auto& e = tofu_.entries[i];
        char line1[96];
        if (e.host[0]) {
            snprintf(line1, sizeof(line1), "%s:%u", e.host, e.port);
        } else {
            snprintf(line1, sizeof(line1), "(legacy entry)  key=%s", e.key);
        }
        d.setTextColor(kRowFg, bg);
        d.setFont(&fonts::lgfxJapanGothic_24);
        d.setTextDatum(top_left);
        d.drawString(line1, kRowX + 20, y + 8);

        // Fingerprint preview: SHA256:hh:hh:... first 8 hex bytes
        char fp_line[80];
        int n = snprintf(fp_line, sizeof(fp_line), "SHA256:");
        for (int b = 0; b < 16 && n + 3 < (int)sizeof(fp_line); ++b) {
            n += snprintf(fp_line + n, sizeof(fp_line) - n,
                          "%02x", e.fp[b]);
        }
        if (n + 3 < (int)sizeof(fp_line)) {
            snprintf(fp_line + n, sizeof(fp_line) - n, "...");
        }
        d.setFont(&fonts::lgfxJapanGothic_20);
        d.drawString(fp_line, kRowX + 20, y + 40);

        draw_button(kDelX + 10, y + 14, kDelW - 20, kTofuRowH - 28,
                    "Delete", kDangerBg, kDangerFg, press_del);
    }

    // Back to profiles
    draw_button(kTofuBackX, kFooterY, kTofuBackW, kAddH, "< Back",
                kEditBg, kEditFg, tofu_.pressed_back);
}

void Menu::render_wifi_edit() {
    auto& d = M5.Display;
    d.fillRect(0, 0, kScreenW, kEditorBottom, kBg);

    // Header
    d.fillRect(0, 0, kScreenW, kWifiHeaderH, kHeaderBg);
    d.setTextColor(kHeaderFg, kHeaderBg);
    d.setTextDatum(middle_left);
    d.setFont(&fonts::lgfxJapanGothic_24);
    d.drawString("Wi-Fi", 24, kWifiHeaderH / 2);

    static const char* kLabels[3] = {"SSID", "Password", "Timeout (s)"};
    for (int i = 0; i < 3; ++i) {
        int y = kWifiFormY0 + i * kWifiFieldH;
        bool focused = (i == wifi_.focused_field);
        bool pressed = (i == wifi_.pressed_field);

        d.setFont(&fonts::lgfxJapanGothic_20);
        d.setTextColor(kRowFg, kBg);
        d.setTextDatum(middle_left);
        d.drawString(kLabels[i], kWifiLabelX, y + kWifiFieldH / 2);

        uint16_t bg = focused ? kFieldBgFocus : kFieldBg;
        if (pressed) bg = static_cast<uint16_t>((bg >> 1) & 0x7BEF);
        int box_h = kWifiFieldH - 6;
        d.fillRoundRect(kWifiValueX, y + 2, kWifiValueW, box_h, 6, bg);
        d.drawRoundRect(kWifiValueX, y + 2, kWifiValueW, box_h, 6, kRowEdge);
        d.setTextColor(kRowFg, bg);
        d.setFont(&fonts::lgfxJapanGothic_24);

        char buf[80];
        if (i == 0) {
            snprintf(buf, sizeof(buf), "%s", wifi_.ssid);
        } else if (i == 1) {
            if (wifi_.show_psk) {
                snprintf(buf, sizeof(buf), "%s", wifi_.psk);
            } else {
                size_t n = strnlen(wifi_.psk, sizeof(wifi_.psk));
                if (n > sizeof(buf) - 1) n = sizeof(buf) - 1;
                memset(buf, '*', n);
                buf[n] = '\0';
            }
        } else {
            snprintf(buf, sizeof(buf), "%d", wifi_.timeout_s);
        }
        if (focused) {
            size_t l = strnlen(buf, sizeof(buf) - 2);
            if (l < sizeof(buf) - 2) { buf[l] = '_'; buf[l + 1] = '\0'; }
        }
        d.drawString(buf, kWifiValueX + 10, y + kWifiFieldH / 2);
    }

    draw_button(kWifiShowX, kWifiBtnY, kWifiShowW, kWifiBtnH,
                wifi_.show_psk ? "Hide pw" : "Show pw",
                kEditBg, kEditFg, wifi_.pressed_show_pw);
    draw_button(kWifiCancelX, kWifiBtnY, kWifiBtnGapW, kWifiBtnH, "Cancel",
                kDangerBg, kDangerFg, wifi_.pressed_cancel);
    draw_button(kWifiSaveX, kWifiBtnY, kWifiBtnGapW, kWifiBtnH, "Save",
                kSaveBg, kSaveFg, wifi_.pressed_save);
}

namespace {

// Return a pointer to the editable text buffer for field idx within p,
// plus its capacity. Returns nullptr for non-text fields.
char* field_text_buf(Profile& p, int idx, size_t* cap) {
    switch (idx) {
        case 0: *cap = sizeof(p.name);     return p.name;
        case 2: *cap = sizeof(p.host);     return p.host;
        case 4: *cap = sizeof(p.user);     return p.user;
        case 5: *cap = sizeof(p.password); return p.password;
        default: *cap = 0;                 return nullptr;
    }
}

}  // namespace

void Menu::render_profile_editor() {
    auto& d = M5.Display;
    // Don't draw into the soft keyboard's region (y >= 336) — it lives
    // underneath and would be wiped otherwise. The compositor already
    // paints kbd panel at z=40 before menu at z=100.
    d.fillRect(0, 0, kScreenW, kEditorBottom, kBg);

    // Header
    d.fillRect(0, 0, kScreenW, kEdHeaderH, kHeaderBg);
    d.setTextColor(kHeaderFg, kHeaderBg);
    d.setTextDatum(middle_left);
    d.setFont(&fonts::lgfxJapanGothic_24);
    const char* title = (editor_.editing_idx < 0) ? "New profile"
                                                  : "Edit profile";
    d.drawString(title, 24, kEdHeaderH / 2);

    // Fields
    for (int i = 0; i < kFieldCount; ++i) {
        int y = kEdFormY0 + i * kEdFieldH;
        bool focused = (i == editor_.focused_field);
        bool pressed = (i == editor_.pressed_field);

        // Label
        d.setFont(&fonts::lgfxJapanGothic_20);
        d.setTextColor(kRowFg, kBg);
        d.setTextDatum(middle_left);
        d.drawString(kFields[i].label, kEdLabelX, y + kEdFieldH / 2);

        // Value box
        uint16_t bg = focused ? kFieldBgFocus : kFieldBg;
        if (pressed) bg = static_cast<uint16_t>((bg >> 1) & 0x7BEF);
        int box_h = kEdFieldH - 6;
        d.fillRoundRect(kEdValueX, y + 2, kEdValueW, box_h, 6, bg);
        d.drawRoundRect(kEdValueX, y + 2, kEdValueW, box_h, 6, kRowEdge);

        d.setTextColor(kRowFg, bg);
        d.setFont(&fonts::lgfxJapanGothic_24);
        d.setTextDatum(middle_left);

        char buf[160];
        if (kFields[i].kind == FieldKind::Toggle) {
            // Currently only field 1 (Protocol) is a toggle.
            if (i == 1) {
                snprintf(buf, sizeof(buf), "%s   [tap to switch]",
                         proto_label(editor_.working.proto));
            } else {
                buf[0] = '\0';
            }
        } else if (kFields[i].kind == FieldKind::Number) {
            snprintf(buf, sizeof(buf), "%u",
                     static_cast<unsigned>(editor_.working.port));
        } else {
            size_t cap = 0;
            const char* src = field_text_buf(editor_.working, i, &cap);
            if (src) {
                bool mask = kFields[i].mask && !editor_.show_password;
                if (mask) {
                    size_t n = strnlen(src, cap);
                    if (n > sizeof(buf) - 1) n = sizeof(buf) - 1;
                    memset(buf, '*', n);
                    buf[n] = '\0';
                } else {
                    snprintf(buf, sizeof(buf), "%.*s",
                             static_cast<int>(cap - 1), src);
                }
            } else {
                buf[0] = '\0';
            }
        }
        // Add a thin cursor marker when this field is focused.
        if (focused) {
            size_t l = strnlen(buf, sizeof(buf) - 2);
            if (l < sizeof(buf) - 2) {
                buf[l]     = '_';
                buf[l + 1] = '\0';
            }
        }
        d.drawString(buf, kEdValueX + 10, y + kEdFieldH / 2);
    }

    // Show/Hide password toggle (left), Cancel, Save (right)
    draw_button(kEdShowPwX, kEdBtnY, kEdShowPwW, kEdBtnH,
                editor_.show_password ? "Hide pw" : "Show pw",
                kEditBg, kEditFg, editor_.pressed_show_pw);
    draw_button(kEdCancelX, kEdBtnY, kEdBtnW, kEdBtnH, "Cancel",
                kDangerBg, kDangerFg, editor_.pressed_cancel);
    draw_button(kEdSaveX, kEdBtnY, kEdBtnW, kEdBtnH, "Save",
                kSaveBg, kSaveFg, editor_.pressed_save);
}

void Menu::render() {
    if (state_ == State::ProfileList) {
        render_profile_list();
    } else if (state_ == State::ProfileEditor) {
        render_profile_editor();
    } else if (state_ == State::TofuList) {
        render_tofu_list();
    } else if (state_ == State::WifiEdit) {
        render_wifi_edit();
    }
}

// ---------- touch dispatch ----------

bool Menu::handle_touch(const TouchPoint& p) {
    if (state_ == State::Hidden) return false;

    // The right-margin status panel is *not* covered by the menu, so
    // taps in that strip don't belong to us. Consume them anyway (the
    // status panel has no interactive elements yet) but don't update
    // any press state.
    if (p.x >= kScreenW) return true;

    if (state_ == State::ProfileEditor) {
        return handle_touch_profile_editor(p);
    }
    if (state_ == State::TofuList) {
        return handle_touch_tofu_list(p);
    }
    if (state_ == State::WifiEdit) {
        return handle_touch_wifi_edit(p);
    }
    return handle_touch_profile_list(p);
}

bool Menu::handle_touch_profile_list(const TouchPoint& p) {
    if (p.event == TouchEvent::Down) {
        pressed_btn_ = 0;
        if (hit_close(p.x, p.y)) {
            pressed_idx_ = -2;
        } else if (hit_add(p.x, p.y)) {
            pressed_idx_ = -3;
        } else if (hit_manage_tofu(p.x, p.y)) {
            pressed_idx_ = -4;
        } else if (hit_manage_wifi(p.x, p.y)) {
            pressed_idx_ = -5;
        } else {
            int row = hit_row(p.y);
            if (row >= 0) {
                pressed_idx_ = row;
                pressed_btn_ = hit_delete(p.x) ? 1
                              : hit_edit(p.x) ? 2
                              : 0;
            } else {
                pressed_idx_ = -1;
            }
        }
        render();
        return true;
    }
    if (p.event == TouchEvent::Move) {
        if (pressed_idx_ == -2 && !hit_close(p.x, p.y)) {
            pressed_idx_ = -1;
            render();
        } else if (pressed_idx_ == -3 && !hit_add(p.x, p.y)) {
            pressed_idx_ = -1;
            render();
        } else if (pressed_idx_ == -4 && !hit_manage_tofu(p.x, p.y)) {
            pressed_idx_ = -1;
            render();
        } else if (pressed_idx_ == -5 && !hit_manage_wifi(p.x, p.y)) {
            pressed_idx_ = -1;
            render();
        } else if (pressed_idx_ >= 0) {
            int row = hit_row(p.y);
            int btn = hit_delete(p.x) ? 1
                     : hit_edit(p.x) ? 2
                     : 0;
            if (row != pressed_idx_ || btn != pressed_btn_) {
                pressed_idx_ = -1;
                render();
            }
        }
        return true;
    }
    if (p.event == TouchEvent::Up) {
        int armed_idx = pressed_idx_;
        int armed_btn = pressed_btn_;
        pressed_idx_ = -1;
        pressed_btn_ = -1;

        if (armed_idx == -2 && hit_close(p.x, p.y)) {
            close();
            return true;
        }
        if (armed_idx == -3 && hit_add(p.x, p.y)) {
            open_editor(-1);
            return true;
        }
        if (armed_idx == -4 && hit_manage_tofu(p.x, p.y)) {
            open_tofu();
            return true;
        }
        if (armed_idx == -5 && hit_manage_wifi(p.x, p.y)) {
            open_wifi();
            return true;
        }
        if (armed_idx >= 0 && hit_row(p.y) == armed_idx) {
            if (armed_btn == 1 && hit_delete(p.x)) {
                ESP_LOGI(kTag, "delete profile %d", armed_idx);
                profiles.remove(armed_idx);
                render();
                return true;
            }
            if (armed_btn == 2 && hit_edit(p.x)) {
                open_editor(armed_idx);
                return true;
            }
            if (armed_btn == 0) {
                ESP_LOGI(kTag, "selecting profile %d — restart pending",
                         armed_idx);
                profiles.select(armed_idx);
                render();
                vTaskDelay(pdMS_TO_TICKS(120));
                esp_restart();
            }
        }
        render();
        return true;
    }
    return true;
}

// ---------- editor ----------

bool Menu::handle_touch_profile_editor(const TouchPoint& p) {
    // The kbd panel lives at y >= 336; let those taps fall through to
    // the soft keyboard so the user can type.
    if (p.y >= 336) return false;

    if (p.event == TouchEvent::Down) {
        editor_.pressed_field   = hit_editor_field(p.x, p.y);
        editor_.pressed_save    = hit_editor_save(p.x, p.y);
        editor_.pressed_cancel  = hit_editor_cancel(p.x, p.y);
        editor_.pressed_show_pw = hit_editor_show_pw(p.x, p.y);
        render();
        return true;
    }
    if (p.event == TouchEvent::Move) {
        int f = hit_editor_field(p.x, p.y);
        if (f != editor_.pressed_field) {
            editor_.pressed_field = -1;
            render();
        }
        if (editor_.pressed_save && !hit_editor_save(p.x, p.y)) {
            editor_.pressed_save = false; render();
        }
        if (editor_.pressed_cancel && !hit_editor_cancel(p.x, p.y)) {
            editor_.pressed_cancel = false; render();
        }
        if (editor_.pressed_show_pw && !hit_editor_show_pw(p.x, p.y)) {
            editor_.pressed_show_pw = false; render();
        }
        return true;
    }
    if (p.event == TouchEvent::Up) {
        int armed_field    = editor_.pressed_field;
        bool armed_save    = editor_.pressed_save;
        bool armed_cancel  = editor_.pressed_cancel;
        bool armed_show_pw = editor_.pressed_show_pw;
        editor_.pressed_field   = -1;
        editor_.pressed_save    = false;
        editor_.pressed_cancel  = false;
        editor_.pressed_show_pw = false;

        if (armed_cancel && hit_editor_cancel(p.x, p.y)) {
            close_editor(/*save=*/false);
            return true;
        }
        if (armed_save && hit_editor_save(p.x, p.y)) {
            close_editor(/*save=*/true);
            return true;
        }
        if (armed_show_pw && hit_editor_show_pw(p.x, p.y)) {
            editor_.show_password = !editor_.show_password;
            render();
            return true;
        }
        if (armed_field >= 0 && hit_editor_field(p.x, p.y) == armed_field) {
            // Toggle fields don't take focus — they flip on tap.
            if (kFields[armed_field].kind == FieldKind::Toggle) {
                if (armed_field == 1) {
                    editor_.working.proto =
                        (editor_.working.proto == ConnProto::SSH)
                            ? ConnProto::Telnet
                            : ConnProto::SSH;
                }
                editor_.focused_field = -1;
            } else {
                editor_.focused_field = armed_field;
            }
        }
        render();
        return true;
    }
    return true;
}

void Menu::open_editor(int idx) {
    if (state_ == State::ProfileEditor) return;  // already editing

    editor_ = Editor{};
    editor_.editing_idx   = idx;
    editor_.focused_field = -1;
    editor_.pressed_field = -1;

    if (idx >= 0) {
        auto p = profiles.get(idx);
        if (p) editor_.working = *p;
    } else {
        // New profile: sensible defaults.
        snprintf(editor_.working.name, sizeof(editor_.working.name),
                 "new-profile");
        editor_.working.proto = ConnProto::SSH;
        editor_.working.port  = 22;
        editor_.working.auth  = SshAuth::Password;
    }

    state_ = State::ProfileEditor;

    // Capture the keyboard's current sink and redirect into the editor
    // until close_editor() restores it. Auto-show the keyboard so the
    // user can start typing without an extra tap.
    if (kbd_) {
        editor_.saved_sink = kbd_->swap_sink(
            [](std::span<const uint8_t> bytes) { tab5::menu.editor_feed(bytes); });
        if (!kbd_->visible()) {
            kbd_->toggle();   // repaints via the layer compositor
        } else {
            render();
        }
    } else {
        render();
    }
}

void Menu::close_editor(bool save) {
    if (save) {
        if (editor_.editing_idx < 0) {
            int nidx = profiles.add(editor_.working);
            if (nidx >= 0) {
                ESP_LOGI(kTag, "added profile at slot %d", nidx);
                profiles.select(nidx);
            } else {
                ESP_LOGW(kTag, "profile store full");
            }
        } else {
            profiles.update(editor_.editing_idx, editor_.working);
            ESP_LOGI(kTag, "updated profile %d", editor_.editing_idx);
        }
    }
    // Restore the keyboard sink + hide the kbd so the ProfileList view
    // (which paints full screen) doesn't sit over the kbd panel.
    if (kbd_) {
        (void)kbd_->swap_sink(std::move(editor_.saved_sink));
        state_ = State::ProfileList;
        if (kbd_->visible()) {
            kbd_->toggle();   // triggers compositor repaint via repaint_kbd
            return;
        }
    } else {
        state_ = State::ProfileList;
    }
    render();
}

// ---------- TOFU list ----------

void Menu::refresh_tofu() {
    tofu::list_entries(tofu_.entries);
}

void Menu::open_tofu() {
    refresh_tofu();
    tofu_.pressed_idx  = -1;
    tofu_.pressed_btn  = -1;
    tofu_.pressed_back = false;
    state_ = State::TofuList;
    render();
}

bool Menu::handle_touch_tofu_list(const TouchPoint& p) {
    if (p.event == TouchEvent::Down) {
        tofu_.pressed_idx  = -1;
        tofu_.pressed_btn  = -1;
        tofu_.pressed_back = false;
        if (hit_close(p.x, p.y)) {
            pressed_idx_ = -2;  // share the header X press state
        } else if (hit_tofu_back(p.x, p.y)) {
            tofu_.pressed_back = true;
        } else {
            int row = hit_tofu_row(p.y);
            if (row >= 0) {
                tofu_.pressed_idx = row;
                tofu_.pressed_btn = hit_tofu_delete(p.x) ? 1 : 0;
            }
        }
        render();
        return true;
    }
    if (p.event == TouchEvent::Move) {
        if (pressed_idx_ == -2 && !hit_close(p.x, p.y)) {
            pressed_idx_ = -1; render();
        }
        if (tofu_.pressed_back && !hit_tofu_back(p.x, p.y)) {
            tofu_.pressed_back = false; render();
        }
        if (tofu_.pressed_idx >= 0) {
            int row = hit_tofu_row(p.y);
            int btn = hit_tofu_delete(p.x) ? 1 : 0;
            if (row != tofu_.pressed_idx || btn != tofu_.pressed_btn) {
                tofu_.pressed_idx = -1;
                render();
            }
        }
        return true;
    }
    if (p.event == TouchEvent::Up) {
        bool armed_back = tofu_.pressed_back;
        int  armed_idx  = tofu_.pressed_idx;
        int  armed_btn  = tofu_.pressed_btn;
        int  armed_x    = pressed_idx_;
        tofu_.pressed_back = false;
        tofu_.pressed_idx  = -1;
        tofu_.pressed_btn  = -1;
        pressed_idx_ = -1;

        if (armed_x == -2 && hit_close(p.x, p.y)) {
            close();
            return true;
        }
        if (armed_back && hit_tofu_back(p.x, p.y)) {
            state_ = State::ProfileList;
            render();
            return true;
        }
        if (armed_idx >= 0 && hit_tofu_row(p.y) == armed_idx &&
            armed_btn == 1 && hit_tofu_delete(p.x)) {
            if (armed_idx < static_cast<int>(tofu_.entries.size())) {
                tofu::remove_by_key(tofu_.entries[armed_idx].key);
                refresh_tofu();
            }
        }
        render();
        return true;
    }
    return true;
}

// ---------- Wi-Fi edit ----------

void Menu::open_wifi() {
    if (state_ == State::WifiEdit) return;
    auto c = wifi_config::get();
    wifi_ = WifiView{};
    strncpy(wifi_.ssid, c.ssid, sizeof(wifi_.ssid) - 1);
    strncpy(wifi_.psk,  c.psk,  sizeof(wifi_.psk)  - 1);
    wifi_.timeout_s     = c.timeout_s > 0 ? c.timeout_s : 20;
    wifi_.focused_field = -1;
    wifi_.pressed_field = -1;

    state_ = State::WifiEdit;

    if (kbd_) {
        wifi_.saved_sink = kbd_->swap_sink(
            [](std::span<const uint8_t> b) { tab5::menu.wifi_feed(b); });
        if (!kbd_->visible()) {
            kbd_->toggle();
        } else {
            render();
        }
    } else {
        render();
    }
}

void Menu::close_wifi(bool save) {
    if (save) {
        wifi_config::Config c{};
        strncpy(c.ssid, wifi_.ssid, sizeof(c.ssid) - 1);
        strncpy(c.psk,  wifi_.psk,  sizeof(c.psk)  - 1);
        c.timeout_s = wifi_.timeout_s;
        wifi_config::set(c);
        ESP_LOGI(kTag, "Wi-Fi saved: ssid='%s' — restart pending", c.ssid);
    }
    if (kbd_) {
        (void)kbd_->swap_sink(std::move(wifi_.saved_sink));
        state_ = State::ProfileList;
        if (kbd_->visible()) {
            kbd_->toggle();
            if (save) { vTaskDelay(pdMS_TO_TICKS(120)); esp_restart(); }
            return;
        }
    } else {
        state_ = State::ProfileList;
    }
    if (save) { vTaskDelay(pdMS_TO_TICKS(120)); esp_restart(); }
    render();
}

bool Menu::handle_touch_wifi_edit(const TouchPoint& p) {
    if (p.y >= 336) return false;  // kbd panel area passes through

    if (p.event == TouchEvent::Down) {
        wifi_.pressed_field   = hit_wifi_field(p.x, p.y);
        wifi_.pressed_save    = hit_wifi_save(p.x, p.y);
        wifi_.pressed_cancel  = hit_wifi_cancel(p.x, p.y);
        wifi_.pressed_show_pw = hit_wifi_show_pw(p.x, p.y);
        render();
        return true;
    }
    if (p.event == TouchEvent::Move) {
        if (wifi_.pressed_field >= 0 &&
            hit_wifi_field(p.x, p.y) != wifi_.pressed_field) {
            wifi_.pressed_field = -1; render();
        }
        if (wifi_.pressed_save && !hit_wifi_save(p.x, p.y)) {
            wifi_.pressed_save = false; render();
        }
        if (wifi_.pressed_cancel && !hit_wifi_cancel(p.x, p.y)) {
            wifi_.pressed_cancel = false; render();
        }
        if (wifi_.pressed_show_pw && !hit_wifi_show_pw(p.x, p.y)) {
            wifi_.pressed_show_pw = false; render();
        }
        return true;
    }
    if (p.event == TouchEvent::Up) {
        int armed_field    = wifi_.pressed_field;
        bool armed_save    = wifi_.pressed_save;
        bool armed_cancel  = wifi_.pressed_cancel;
        bool armed_show_pw = wifi_.pressed_show_pw;
        wifi_.pressed_field   = -1;
        wifi_.pressed_save    = false;
        wifi_.pressed_cancel  = false;
        wifi_.pressed_show_pw = false;

        if (armed_cancel && hit_wifi_cancel(p.x, p.y)) {
            close_wifi(false); return true;
        }
        if (armed_save && hit_wifi_save(p.x, p.y)) {
            close_wifi(true); return true;
        }
        if (armed_show_pw && hit_wifi_show_pw(p.x, p.y)) {
            wifi_.show_psk = !wifi_.show_psk;
            render();
            return true;
        }
        if (armed_field >= 0 && hit_wifi_field(p.x, p.y) == armed_field) {
            wifi_.focused_field = armed_field;
        }
        render();
        return true;
    }
    return true;
}

void Menu::wifi_feed(std::span<const uint8_t> bytes) {
    if (state_ != State::WifiEdit || wifi_.focused_field < 0) return;
    int f = wifi_.focused_field;
    bool changed = false;
    for (uint8_t b : bytes) {
        if (b == 0x7F || b == 0x08) {
            if (f == 2) {
                if (wifi_.timeout_s > 0) {
                    wifi_.timeout_s /= 10;
                    changed = true;
                }
            } else {
                char* buf  = (f == 0) ? wifi_.ssid : wifi_.psk;
                size_t cap = (f == 0) ? sizeof(wifi_.ssid) : sizeof(wifi_.psk);
                size_t n   = strnlen(buf, cap);
                if (n > 0) { buf[n - 1] = '\0'; changed = true; }
            }
        } else if (b >= 0x20 && b < 0x7F) {
            if (f == 2) {
                if (b < '0' || b > '9') continue;
                int n = wifi_.timeout_s * 10 + (b - '0');
                if (n <= 3600) { wifi_.timeout_s = n; changed = true; }
            } else {
                char* buf  = (f == 0) ? wifi_.ssid : wifi_.psk;
                size_t cap = (f == 0) ? sizeof(wifi_.ssid) : sizeof(wifi_.psk);
                size_t n   = strnlen(buf, cap);
                if (n + 1 < cap) {
                    buf[n] = static_cast<char>(b);
                    buf[n + 1] = '\0';
                    changed = true;
                }
            }
        } else if (b == 0x1B) {
            close_wifi(false);
            return;
        }
    }
    if (changed) render();
}

void Menu::editor_feed(std::span<const uint8_t> bytes) {
    if (state_ != State::ProfileEditor || editor_.focused_field < 0) return;
    int f = editor_.focused_field;
    if (kFields[f].kind == FieldKind::Toggle) return;

    bool changed = false;
    for (uint8_t b : bytes) {
        if (b == 0x7F || b == 0x08) {  // BS / DEL — drop the last char
            if (kFields[f].kind == FieldKind::Number) {
                if (editor_.working.port > 0) {
                    editor_.working.port /= 10;
                    changed = true;
                }
            } else {
                size_t cap = 0;
                char* buf = field_text_buf(editor_.working, f, &cap);
                if (!buf) continue;
                size_t n = strnlen(buf, cap);
                if (n > 0) { buf[n - 1] = '\0'; changed = true; }
            }
        } else if (b >= 0x20 && b < 0x7F) {  // printable ASCII
            if (kFields[f].kind == FieldKind::Number) {
                if (b < '0' || b > '9') continue;
                uint32_t n = static_cast<uint32_t>(editor_.working.port) * 10
                             + (b - '0');
                if (n <= 65535) {
                    editor_.working.port = static_cast<uint16_t>(n);
                    changed = true;
                }
            } else {
                size_t cap = 0;
                char* buf = field_text_buf(editor_.working, f, &cap);
                if (!buf) continue;
                size_t n = strnlen(buf, cap);
                if (n + 1 < cap) {
                    buf[n] = static_cast<char>(b);
                    buf[n + 1] = '\0';
                    changed = true;
                }
            }
        } else if (b == 0x1B) {  // ESC — cancel editor
            close_editor(/*save=*/false);
            return;
        }
        // Ignore everything else (Tab, arrows, F-keys, etc.) for MVP.
    }
    if (changed) render();
}

}  // namespace tab5

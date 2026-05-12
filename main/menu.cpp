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

// "+ Add new" footer button
constexpr int kAddW    = 240;
constexpr int kAddH    = 60;
constexpr int kAddX    = (kScreenW - kAddW) / 2;
constexpr int kAddY    = kScreenH - kAddH - 20;

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

    // Add button
    bool press_add = (pressed_idx_ == -3);
    draw_button(kAddX, kAddY, kAddW, kAddH, "+ Add new",
                kAddBg, kAddFg, press_add);
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
    return handle_touch_profile_list(p);
}

bool Menu::handle_touch_profile_list(const TouchPoint& p) {
    if (p.event == TouchEvent::Down) {
        pressed_btn_ = 0;
        if (hit_close(p.x, p.y)) {
            pressed_idx_ = -2;
        } else if (hit_add(p.x, p.y)) {
            pressed_idx_ = -3;
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

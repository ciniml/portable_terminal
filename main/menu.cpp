#include "menu.hpp"

#include <cstdio>
#include <cstring>

#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "M5Unified.h"

#include "profiles.hpp"

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
constexpr int kRowW    = kScreenW - 40;          // 1240
constexpr int kDelW    = 140;
constexpr int kDelX    = kRowX + kRowW - kDelW;  // right-aligned

// "+ Add new" footer button
constexpr int kAddW    = 240;
constexpr int kAddH    = 60;
constexpr int kAddX    = (kScreenW - kAddW) / 2;
constexpr int kAddY    = kScreenH - kAddH - 20;

}  // namespace

Menu menu;

Menu::Menu() = default;

void Menu::open() {
    if (state_ != State::Hidden) return;
    state_ = State::ProfileList;
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

bool Menu::hit_add(int x, int y) const {
    return x >= kAddX && x < kAddX + kAddW &&
           y >= kAddY && y < kAddY + kAddH;
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

void Menu::render() {
    if (state_ == State::ProfileList) {
        render_profile_list();
    }
    // Future: ProfileEditor / TofuList
}

// ---------- touch dispatch ----------

bool Menu::handle_touch(const TouchPoint& p) {
    if (state_ == State::Hidden) return false;

    // The right-margin status panel is *not* covered by the menu, so
    // taps in that strip don't belong to us. Consume them anyway (the
    // status panel has no interactive elements yet) but don't update
    // any press state.
    if (p.x >= kScreenW) return true;

    // Treat any touch event while the menu is up as consumed, so the
    // soft keyboard never sees stray taps through the overlay.
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
                pressed_btn_ = hit_delete(p.x) ? 1 : 0;
            } else {
                pressed_idx_ = -1;
            }
        }
        render();
        return true;
    }
    if (p.event == TouchEvent::Move) {
        // Cancel highlight if finger leaves the original target.
        if (pressed_idx_ == -2 && !hit_close(p.x, p.y)) {
            pressed_idx_ = -1;
            render();
        } else if (pressed_idx_ == -3 && !hit_add(p.x, p.y)) {
            pressed_idx_ = -1;
            render();
        } else if (pressed_idx_ >= 0) {
            int row = hit_row(p.y);
            int btn = hit_delete(p.x) ? 1 : 0;
            if (row != pressed_idx_ || btn != pressed_btn_) {
                pressed_idx_ = -1;
                if (repaint_) repaint_();
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
            ESP_LOGI(kTag, "Add new profile — editor lands in step 3");
            // TODO: open ProfileEditor in Add mode.
            render();
            return true;
        }
        if (armed_idx >= 0 && hit_row(p.y) == armed_idx) {
            if (armed_btn == 1 && hit_delete(p.x)) {
                ESP_LOGI(kTag, "delete profile %d", armed_idx);
                profiles.remove(armed_idx);
                render();
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

}  // namespace tab5

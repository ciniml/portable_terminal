#include "term_core/parser.hpp"

namespace term {

namespace {

constexpr uint8_t kEsc = 0x1B;
constexpr uint8_t kBel = 0x07;
constexpr uint8_t kCan = 0x18;
constexpr uint8_t kSub = 0x1A;
constexpr uint8_t kSt = 0x9C;  // unused under 7-bit assumption; ESC \ used instead

bool is_csi_intermediate(uint8_t b) { return b >= 0x20 && b <= 0x2F; }
bool is_csi_final(uint8_t b) { return b >= 0x40 && b <= 0x7E; }
bool is_csi_private_marker(uint8_t b) { return b >= 0x3C && b <= 0x3F; }   // < = > ?
bool is_esc_intermediate(uint8_t b) { return b >= 0x20 && b <= 0x2F; }
bool is_esc_final(uint8_t b) { return b >= 0x30 && b <= 0x7E; }
bool is_c0(uint8_t b) { return b < 0x20; }

}  // namespace

void Parser::reset() {
    state_ = State::Ground;
    n_params_ = 0;
    param_started_ = false;
    n_intermediates_ = 0;
    private_marker_ = false;
    csi_overflow_ = false;
    n_esc_intermediates_ = 0;
    cp_ = 0;
    cp_remaining_ = 0;
    osc_len_ = 0;
    osc_overflow_ = false;
}

void Parser::enter_ground() { state_ = State::Ground; }

void Parser::enter_esc() {
    state_ = State::Esc;
    n_esc_intermediates_ = 0;
}

void Parser::enter_csi() {
    state_ = State::CsiEntry;
    n_params_ = 0;
    param_started_ = false;
    n_intermediates_ = 0;
    private_marker_ = false;
    csi_overflow_ = false;
}

void Parser::enter_osc() {
    state_ = State::OscString;
    osc_len_ = 0;
    osc_overflow_ = false;
}

void Parser::push_param_digit(uint8_t d) {
    if (n_params_ == 0) {
        n_params_ = 1;
        params_[0] = 0;
    }
    if (n_params_ - 1 >= kMaxParams) { csi_overflow_ = true; return; }
    int& cur = params_[n_params_ - 1];
    if (cur > 99999) { csi_overflow_ = true; return; }
    cur = cur * 10 + (d - '0');
    param_started_ = true;
}

void Parser::next_param() {
    if (n_params_ == 0) {
        // first param is implicit empty
        n_params_ = 1;
        params_[0] = 0;
    }
    if (n_params_ < kMaxParams) {
        params_[n_params_] = 0;
        ++n_params_;
    } else {
        csi_overflow_ = true;
    }
    param_started_ = false;
}

void Parser::dispatch_csi(uint8_t final_byte, IParserSink& sink) {
    if (csi_overflow_) {
        enter_ground();
        return;
    }
    // If no params at all but final reached, pass empty params.
    sink.csi(static_cast<char>(final_byte),
             std::span<const int>(params_.data(), n_params_),
             std::span<const uint8_t>(intermediates_.data(), n_intermediates_),
             private_marker_);
    enter_ground();
}

void Parser::dispatch_esc(uint8_t final_byte, IParserSink& sink) {
    sink.esc(static_cast<char>(final_byte),
             std::span<const uint8_t>(esc_intermediates_.data(), n_esc_intermediates_));
    enter_ground();
}

void Parser::dispatch_osc(IParserSink& sink) {
    if (!osc_overflow_) {
        sink.osc(std::span<const uint8_t>(osc_buf_.data(), osc_len_));
    }
    enter_ground();
}

void Parser::on_ground_byte(uint8_t b, IParserSink& sink) {
    if (b == kEsc) { enter_esc(); return; }
    if (b < 0x20 || b == 0x7F) {
        // C0 control or DEL.
        if (b == 0x7F) return;  // ignore DEL
        sink.execute(b);
        return;
    }
    // Printable / UTF-8.
    if (cp_remaining_ > 0) {
        if ((b & 0xC0) != 0x80) {
            // invalid continuation; reset and treat as new ground byte.
            cp_remaining_ = 0;
            cp_ = 0;
        } else {
            cp_ = (cp_ << 6) | (b & 0x3F);
            if (--cp_remaining_ == 0) {
                sink.put_char(cp_);
                cp_ = 0;
            }
            return;
        }
    }
    if (b < 0x80) {
        sink.put_char(static_cast<char32_t>(b));
        return;
    }
    if ((b & 0xE0) == 0xC0) {
        cp_ = b & 0x1F;
        cp_remaining_ = 1;
        return;
    }
    if ((b & 0xF0) == 0xE0) {
        cp_ = b & 0x0F;
        cp_remaining_ = 2;
        return;
    }
    if ((b & 0xF8) == 0xF0) {
        cp_ = b & 0x07;
        cp_remaining_ = 3;
        return;
    }
    // Invalid lead; ignore.
    cp_ = 0;
    cp_remaining_ = 0;
}

void Parser::feed(uint8_t b, IParserSink& sink) {
    // Universal aborts.
    if (b == kCan || b == kSub) { enter_ground(); return; }

    switch (state_) {
        case State::Ground:
            on_ground_byte(b, sink);
            return;

        case State::Esc:
            if (b == '[') { enter_csi(); return; }
            if (b == ']') { enter_osc(); return; }
            if (is_esc_intermediate(b)) {
                if (n_esc_intermediates_ < kMaxIntermediates)
                    esc_intermediates_[n_esc_intermediates_++] = b;
                state_ = State::EscIntermediate;
                return;
            }
            if (is_esc_final(b)) {
                dispatch_esc(b, sink);
                return;
            }
            if (b < 0x20) {
                // Execute then stay in Esc — Williams says execute and remain.
                sink.execute(b);
                return;
            }
            enter_ground();
            return;

        case State::EscIntermediate:
            if (is_esc_intermediate(b)) {
                if (n_esc_intermediates_ < kMaxIntermediates)
                    esc_intermediates_[n_esc_intermediates_++] = b;
                return;
            }
            if (is_esc_final(b)) {
                dispatch_esc(b, sink);
                return;
            }
            if (b < 0x20) { sink.execute(b); return; }
            enter_ground();
            return;

        case State::CsiEntry:
            if (is_c0(b)) { sink.execute(b); return; }
            if (b >= '0' && b <= '9') {
                push_param_digit(b);
                state_ = State::CsiParam;
                return;
            }
            if (b == ';' || b == ':') {
                next_param();
                state_ = State::CsiParam;
                return;
            }
            if (is_csi_private_marker(b)) {
                private_marker_ = true;
                state_ = State::CsiParam;
                return;
            }
            if (is_csi_intermediate(b)) {
                if (n_intermediates_ < kMaxIntermediates)
                    intermediates_[n_intermediates_++] = b;
                state_ = State::CsiIntermediate;
                return;
            }
            if (is_csi_final(b)) { dispatch_csi(b, sink); return; }
            state_ = State::CsiIgnore;
            return;

        case State::CsiParam:
            if (is_c0(b)) { sink.execute(b); return; }
            if (b >= '0' && b <= '9') { push_param_digit(b); return; }
            if (b == ';' || b == ':') { next_param(); return; }
            if (is_csi_intermediate(b)) {
                if (n_intermediates_ < kMaxIntermediates)
                    intermediates_[n_intermediates_++] = b;
                state_ = State::CsiIntermediate;
                return;
            }
            if (is_csi_final(b)) {
                // ensure n_params_ reflects an empty trailing param if needed.
                if (n_params_ == 0 && param_started_) n_params_ = 1;
                dispatch_csi(b, sink);
                return;
            }
            state_ = State::CsiIgnore;
            return;

        case State::CsiIntermediate:
            if (is_c0(b)) { sink.execute(b); return; }
            if (is_csi_intermediate(b)) {
                if (n_intermediates_ < kMaxIntermediates)
                    intermediates_[n_intermediates_++] = b;
                return;
            }
            if (is_csi_final(b)) { dispatch_csi(b, sink); return; }
            state_ = State::CsiIgnore;
            return;

        case State::CsiIgnore:
            if (is_c0(b)) { sink.execute(b); return; }
            if (is_csi_final(b)) { enter_ground(); return; }
            return;

        case State::OscString:
            if (b == kBel) { dispatch_osc(sink); return; }
            if (b == kEsc) {
                // ESC \ string terminator. The next byte should be '\'.
                // We consume the ESC and wait for the next byte; simulate by
                // moving to Esc state and trusting the dispatcher there.
                // Simpler: snapshot OSC, then re-enter Esc.
                dispatch_osc(sink);
                enter_esc();
                return;
            }
            if (osc_len_ < kMaxOscLen) {
                osc_buf_[osc_len_++] = b;
            } else {
                osc_overflow_ = true;
            }
            return;
    }
}

void Parser::feed(std::span<const uint8_t> bytes, IParserSink& sink) {
    for (uint8_t b : bytes) feed(b, sink);
}

}  // namespace term

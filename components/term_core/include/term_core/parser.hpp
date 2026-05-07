#pragma once

#include <array>
#include <cstdint>
#include <span>

namespace term {

// Sink notified by the parser as it walks input bytes.
class IParserSink {
public:
    virtual ~IParserSink() = default;

    // A printable codepoint in the GROUND state (after UTF-8 decoding).
    virtual void put_char(char32_t ch) = 0;

    // A C0 control byte (BEL/BS/HT/LF/VT/FF/CR) reached in GROUND state.
    virtual void execute(uint8_t c0) = 0;

    // CSI dispatch: final byte, parsed parameters, intermediate bytes,
    // and a flag for the leading private marker (?, >, =, <).
    virtual void csi(char final_byte,
                     std::span<const int> params,
                     std::span<const uint8_t> intermediates,
                     bool private_marker) = 0;

    // ESC dispatch (no CSI), e.g. ESC c (RIS), ESC D, ESC M, ESC 7/8.
    virtual void esc(char final_byte,
                     std::span<const uint8_t> intermediates) = 0;

    // OSC dispatch (terminator stripped). Phase 1 may ignore.
    virtual void osc(std::span<const uint8_t> data) = 0;
};

// Williams-style VT/ANSI parser. Stateless across feed() calls (state is
// kept in the Parser instance) — call reset() to drop a partially-parsed
// sequence.
class Parser {
public:
    Parser() = default;

    void feed(std::span<const uint8_t> bytes, IParserSink& sink);
    void feed(uint8_t byte, IParserSink& sink);
    void reset();

    static constexpr int kMaxParams = 16;
    static constexpr int kMaxIntermediates = 2;
    static constexpr int kMaxOscLen = 256;

private:
    enum class State : uint8_t {
        Ground,
        Esc,
        EscIntermediate,
        CsiEntry,
        CsiParam,
        CsiIntermediate,
        CsiIgnore,
        OscString,
    };

    void enter_ground();
    void enter_esc();
    void enter_csi();
    void enter_osc();
    void push_param_digit(uint8_t d);
    void next_param();
    void dispatch_csi(uint8_t final_byte, IParserSink& sink);
    void dispatch_esc(uint8_t final_byte, IParserSink& sink);
    void dispatch_osc(IParserSink& sink);
    void on_ground_byte(uint8_t b, IParserSink& sink);

    State state_{State::Ground};

    // CSI parameter accumulator.
    std::array<int, kMaxParams> params_{};
    int n_params_{0};
    bool param_started_{false};

    // CSI intermediates and private marker.
    std::array<uint8_t, kMaxIntermediates> intermediates_{};
    int n_intermediates_{0};
    bool private_marker_{false};
    bool csi_overflow_{false};

    // ESC intermediates.
    std::array<uint8_t, kMaxIntermediates> esc_intermediates_{};
    int n_esc_intermediates_{0};

    // UTF-8 decoder.
    char32_t cp_{0};
    int cp_remaining_{0};

    // OSC string buffer.
    std::array<uint8_t, kMaxOscLen> osc_buf_{};
    int osc_len_{0};
    bool osc_overflow_{false};
};

}  // namespace term

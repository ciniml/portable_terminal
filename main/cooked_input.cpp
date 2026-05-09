#include "cooked_input.hpp"

namespace tab5 {

namespace {

constexpr uint8_t kCr = 0x0D;
constexpr uint8_t kLf = 0x0A;
constexpr uint8_t kBs = 0x08;
constexpr uint8_t kDel = 0x7F;
constexpr uint8_t kSp = 0x20;

}  // namespace

void CookedInputFilter::process(std::span<const uint8_t> in,
                                std::vector<uint8_t>& out) {
    out.reserve(out.size() + in.size() * 3);
    for (uint8_t b : in) {
        switch (b) {
            case kCr:
                out.push_back(kCr);
                out.push_back(kLf);
                prev_was_cr_ = true;
                break;
            case kLf:
                if (!prev_was_cr_) {
                    out.push_back(kCr);
                    out.push_back(kLf);
                }
                prev_was_cr_ = false;
                break;
            case kBs:
            case kDel:
                out.push_back(kBs);
                out.push_back(kSp);
                out.push_back(kBs);
                prev_was_cr_ = false;
                break;
            default:
                out.push_back(b);
                prev_was_cr_ = false;
                break;
        }
    }
}

}  // namespace tab5

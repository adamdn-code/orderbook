// Nasdaq TotalView-ITCH 5.0 reader and decoder.
//
// File format (Nasdaq's "BinaryFILE"): each message is preceded by a 2-byte
// big-endian length. Every message starts with the same 11-byte header:
//   type (1) | stock locate (2) | tracking number (2) | timestamp (6, ns since midnight)
// All integers are big-endian. Prices are 4-decimal fixed point (price * 10,000).
//
// Reader streams a file, gzipped or not (zlib reads plain files transparently),
// and hands each message to a callback without copying it. dispatch() decodes
// the message types that affect an order book and calls the matching method
// on a visitor.
#pragma once

#include <zlib.h>

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace itch {

inline std::uint16_t be16(const std::uint8_t* p) { return std::uint16_t(p[0] << 8 | p[1]); }

inline std::uint32_t be32(const std::uint8_t* p) {
    return std::uint32_t(p[0]) << 24 | std::uint32_t(p[1]) << 16 | std::uint32_t(p[2]) << 8 | p[3];
}

inline std::uint64_t be_n(const std::uint8_t* p, int n) {
    std::uint64_t v = 0;
    for (int i = 0; i < n; ++i) v = v << 8 | p[i];
    return v;
}

inline std::uint64_t be48(const std::uint8_t* p) { return be_n(p, 6); }
inline std::uint64_t be64(const std::uint8_t* p) { return be_n(p, 8); }

// Symbol fields are 8 ASCII characters, right-padded with spaces.
inline std::string_view symbol(const std::uint8_t* p) {
    std::size_t n = 8;
    while (n && p[n - 1] == ' ') --n;
    return {reinterpret_cast<const char*>(p), n};
}

class Reader {
public:
    explicit Reader(const std::string& path, std::size_t buffer_bytes = std::size_t(1) << 22) : buf_(buffer_bytes) {
        file_ = gzopen(path.c_str(), "rb");
        if (!file_) throw std::runtime_error("can't open " + path);
        gzbuffer(file_, 1 << 20);
    }
    ~Reader() {
        if (file_) gzclose(file_);
    }
    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;

    // Calls fn(message, length) for each message in order. Returns false if
    // the file ends partway through a message.
    template <class Fn>
    bool for_each(Fn&& fn) {
        for (;;) {
            if (!fill(2)) return pos_ == end_;  // clean end of file
            const std::size_t len = be16(&buf_[pos_]);
            if (!fill(2 + len)) return false;
            fn(&buf_[pos_ + 2], len);
            pos_ += 2 + len;
        }
    }

    // Uncompressed bytes consumed so far.
    std::uint64_t bytes() const { return bytes_; }

private:
    // Make sure at least `need` unread bytes are buffered. False at end of file.
    bool fill(std::size_t need) {
        if (end_ - pos_ >= need) return true;
        std::memmove(buf_.data(), buf_.data() + pos_, end_ - pos_);
        end_ -= pos_;
        pos_ = 0;
        while (end_ < need) {
            const int got = gzread(file_, buf_.data() + end_, unsigned(buf_.size() - end_));
            if (got < 0) {
                int err;
                throw std::runtime_error(std::string("read error: ") + gzerror(file_, &err));
            }
            if (got == 0) return false;
            end_ += std::size_t(got);
            bytes_ += std::uint64_t(got);
        }
        return true;
    }

    gzFile file_ = nullptr;
    std::vector<std::uint8_t> buf_;
    std::size_t pos_ = 0;
    std::size_t end_ = 0;
    std::uint64_t bytes_ = 0;
};

// Decodes one message and calls the visitor. Returns false if the message is
// shorter or longer than ITCH 5.0 says it should be.
//
// Visitor methods (all take locate and timestamp first where the message has them):
//   on_system_event(ts, code)
//   on_stock_directory(locate, ts, symbol)
//   on_add(locate, ts, ref, is_buy, shares, price)            // 'A' and 'F'
//   on_executed(locate, ts, ref, shares)                       // 'E': at the order's own price
//   on_executed_with_price(locate, ts, ref, shares, printable, price)  // 'C'
//   on_cancel(locate, ts, ref, shares)                         // 'X': partial cancel
//   on_delete(locate, ts, ref)                                 // 'D'
//   on_replace(locate, ts, old_ref, new_ref, shares, price)    // 'U'
//   on_trade(locate, ts, is_buy, shares, price)                // 'P': hidden-order execution
//   on_cross(locate, ts, shares, price)                        // 'Q': opening/closing cross
//   on_other(type)
template <class Visitor>
bool dispatch(const std::uint8_t* m, std::size_t len, Visitor& v) {
    if (len < 11) return false;
    const char type = char(m[0]);
    const std::uint16_t locate = be16(m + 1);
    const std::uint64_t ts = be48(m + 5);
    switch (type) {
        case 'S':
            if (len != 12) return false;
            v.on_system_event(ts, char(m[11]));
            return true;
        case 'R':
            if (len != 39) return false;
            v.on_stock_directory(locate, ts, symbol(m + 11));
            return true;
        case 'A':
        case 'F':
            if (len != (type == 'A' ? 36u : 40u)) return false;
            v.on_add(locate, ts, be64(m + 11), m[19] == 'B', be32(m + 20), be32(m + 32));
            return true;
        case 'E':
            if (len != 31) return false;
            v.on_executed(locate, ts, be64(m + 11), be32(m + 19));
            return true;
        case 'C':
            if (len != 36) return false;
            v.on_executed_with_price(locate, ts, be64(m + 11), be32(m + 19), m[31] == 'Y', be32(m + 32));
            return true;
        case 'X':
            if (len != 23) return false;
            v.on_cancel(locate, ts, be64(m + 11), be32(m + 19));
            return true;
        case 'D':
            if (len != 19) return false;
            v.on_delete(locate, ts, be64(m + 11));
            return true;
        case 'U':
            if (len != 35) return false;
            v.on_replace(locate, ts, be64(m + 11), be64(m + 19), be32(m + 27), be32(m + 31));
            return true;
        case 'P':
            if (len != 44) return false;
            v.on_trade(locate, ts, m[19] == 'B', be32(m + 20), be32(m + 32));
            return true;
        case 'Q':
            if (len != 40) return false;
            v.on_cross(locate, ts, be64(m + 11), be32(m + 27));
            return true;
        default:
            v.on_other(type);
            return true;
    }
}

}  // namespace itch

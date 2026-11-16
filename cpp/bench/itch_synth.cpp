// Writes a synthetic multi-stock ITCH 5.0 file for benchmarking the replay: STOCKS stocks with
// Zipf-skewed activity (a few very busy names, a long quiet tail, as on a real day), adds near
// each stock's price, executions, cancels, deletes and replaces of live orders.
//   itch_synth OUT MESSAGES [STOCKS]
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

namespace {
std::vector<std::uint8_t> buf;
void be(std::uint64_t v, int n) { for (int i = n - 1; i >= 0; --i) buf.push_back(std::uint8_t(v >> (8 * i))); }
void ch(char c) { buf.push_back(std::uint8_t(c)); }
void header(char type, std::uint16_t locate, std::uint64_t ts, std::size_t len) { be(len, 2); ch(type); be(locate, 2); be(0, 2); be(ts, 6); }
void sym(const std::string& s) { for (std::size_t i = 0; i < 8; ++i) ch(i < s.size() ? s[i] : ' '); }
}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: itch_synth OUT MESSAGES [STOCKS]\n"); return 2; }
    const std::uint64_t total = std::stoull(argv[2]);
    const int stocks = argc > 3 ? std::stoi(argv[3]) : 4000;
    std::FILE* f = std::fopen(argv[1], "wb");
    if (!f) return 1;
    std::mt19937_64 rng(7);
    std::vector<double> cdf(stocks);
    double acc = 0;
    for (int k = 0; k < stocks; ++k) cdf[k] = acc += 1.0 / std::pow(k + 1, 1.1);
    for (auto& c : cdf) c /= acc;
    struct Live { std::uint64_t ref; std::uint32_t shares; bool buy; };
    std::vector<std::vector<Live>> live(stocks + 1);
    std::vector<std::uint32_t> mid(stocks + 1);
    for (int k = 1; k <= stocks; ++k) {
        header('R', std::uint16_t(k), 0, 39);
        sym("S" + std::to_string(k));
        for (int i = 0; i < 20; ++i) ch(' ');
        mid[k] = std::uint32_t(10'000 * (5 + rng() % 400));
    }
    std::uniform_real_distribution<double> u(0, 1);
    std::uint64_t ref = 1, ts = 34'200'000'000'000ULL;
    for (std::uint64_t n = 0; n < total; ++n) {
        ts += 600;
        const int k = 1 + int(std::lower_bound(cdf.begin(), cdf.end(), u(rng)) - cdf.begin());
        auto& refs = live[k];
        const int op = int(rng() % 100);
        if (op < 46 || refs.size() < 20) {
            const bool buy = rng() & 1;
            const std::uint32_t off = 100 * std::uint32_t(1 + std::min<std::uint64_t>(rng() % 40, rng() % 40));
            header('A', std::uint16_t(k), ts, 36);
            const std::uint32_t shares = 100 * std::uint32_t(1 + rng() % 5);
            be(ref, 8); ch(buy ? 'B' : 'S'); be(shares, 4); sym("S" + std::to_string(k));
            be(buy ? mid[k] - off : mid[k] + off, 4);
            refs.push_back({ref++, shares, buy});
        } else if (op < 60) {  // execution or partial cancel of 100 shares
            const std::size_t j = rng() % refs.size();
            if (op < 52) { header('E', std::uint16_t(k), ts, 31); be(refs[j].ref, 8); be(100, 4); be(n, 8); }
            else { header('X', std::uint16_t(k), ts, 23); be(refs[j].ref, 8); be(100, 4); }
            if ((refs[j].shares -= 100) == 0) { refs[j] = refs.back(); refs.pop_back(); }
        } else if (op < 92) {
            const std::size_t j = rng() % refs.size();
            header('D', std::uint16_t(k), ts, 19);
            be(refs[j].ref, 8);
            refs[j] = refs.back(); refs.pop_back();
        } else {
            const std::size_t j = rng() % refs.size();
            header('U', std::uint16_t(k), ts, 35);
            const std::uint32_t off = 100 * std::uint32_t(1 + rng() % 20);
            be(refs[j].ref, 8); be(ref, 8); be(100, 4); be(refs[j].buy ? mid[k] - off : mid[k] + off, 4);
            refs[j] = {ref++, 100, refs[j].buy};
        }
        if (buf.size() > (1 << 22)) { std::fwrite(buf.data(), 1, buf.size(), f); buf.clear(); }
    }
    std::fwrite(buf.data(), 1, buf.size(), f);
    std::fclose(f);
    return 0;
}

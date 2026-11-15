// The 19 order-flow features of backtest/ml_signal.py, computed incrementally.
//
// Feed it every execution as it happens (on_execution) and the top five levels of the book after
// the last event of each timestamp (on_snapshot). on_snapshot returns true when the snapshot is
// inside the recording window and has written the feature vector, in ml_signal.FEATURES order.
//
// Windowed features (100 ms / 1 s / 10 s) need the value of a running total as it was one window
// ago. Snapshot times only go forward, so each window keeps a pointer into the snapshot history
// that only ever moves forward: O(1) amortised per snapshot, no searching. The history is a ring
// buffer trimmed to the longest window, so memory stays bounded over a whole day.
#pragma once

#include <cmath>
#include <cstdint>
#include <deque>
#include <utility>

namespace ml {

constexpr int kFeatures = 19;
constexpr std::int64_t kNs = 1'000'000'000;

class OrderFlowFeatures {
public:
    // Snapshots are recorded (and features computed) for ts in [record_from, record_to).
    OrderFlowFeatures(std::int64_t record_from, std::int64_t record_to) : from_(record_from), to_(record_to) {}

    // `resting_is_sell`: the executed resting order was an ask, so the trade was buyer-initiated.
    void on_execution(bool resting_is_sell, std::int64_t qty) {
        cum_tv_ += resting_is_sell ? double(qty) : -double(qty);
        cum_tn_ += 1;
    }

    // Levels best first: (price, qty). Needs at least one level per side.
    bool on_snapshot(std::int64_t ts, const std::pair<std::int64_t, std::int64_t>* bids, int n_bids,
                     const std::pair<std::int64_t, std::int64_t>* asks, int n_asks, double* out) {
        const double bb = double(bids[0].first), ba = double(asks[0].first);
        const double bq1 = double(bids[0].second), aq1 = double(asks[0].second);
        if (has_prev_) {  // order-flow imbalance increment, Cont-Kukanov-Stoikov
            double e = 0;
            if (bb >= pb_) e += bq1;
            if (bb <= pb_) e -= pbq_;
            if (ba <= pa_) e -= aq1;
            if (ba >= pa_) e += paq_;
            cum_ofi_ += e;
        }
        has_prev_ = true;
        pb_ = bb, pbq_ = bq1, pa_ = ba, paq_ = aq1;
        const std::int64_t mid2 = bids[0].first + asks[0].first;  // twice the mid, exact
        if (has_mid_ && mid2 != last_mid2_) {
            cum_moves_ += 1;
            last_move_ = mid2 > last_mid2_ ? 1 : -1;
            last_move_ts_ = ts;
        }
        has_mid_ = true;
        last_mid2_ = mid2;
        if (ts < from_ || ts >= to_) return false;

        const double mid = double(mid2) / 2;
        hist_.push_back({ts, cum_ofi_, cum_tv_, cum_tn_, cum_moves_, mid});
        ++pushed_;
        if (pushed_ == 1) first_ = hist_.front();
        const Row& now = hist_.back();
        const Row& l100 = lag(0, ts - kNs / 10);
        const Row& l1 = lag(1, ts - kNs);
        const Row& l10 = lag(2, ts - 10 * kNs);
        trim();

        double bq2 = bq1 + (n_bids > 1 ? double(bids[1].second) : 0), aq2 = aq1 + (n_asks > 1 ? double(asks[1].second) : 0);
        double bq5 = 0, aq5 = 0;
        for (int i = 0; i < n_bids && i < 5; ++i) bq5 += double(bids[i].second);
        for (int i = 0; i < n_asks && i < 5; ++i) aq5 += double(asks[i].second);
        const double spread = ba - bb, touch = (bq1 + aq1) / 2;
        const double micro = (bb * aq1 + ba * bq1) / (bq1 + aq1);
        out[0] = spread;
        out[1] = (bq1 - aq1) / (bq1 + aq1);
        out[2] = (bq2 - aq2) / (bq2 + aq2);
        out[3] = (bq5 - aq5) / (bq5 + aq5);
        out[4] = std::log1p(bq1);
        out[5] = std::log1p(aq1);
        out[6] = (micro - (bb + ba) / 2) / spread;
        out[7] = (now.ofi - l100.ofi) / touch;
        out[8] = (now.ofi - l1.ofi) / touch;
        out[9] = (now.ofi - l10.ofi) / touch;
        out[10] = (now.tv - l1.tv) / touch;
        out[11] = (now.tv - l10.tv) / touch;
        out[12] = now.tn - l1.tn;
        out[13] = now.tn - l10.tn;
        out[14] = now.mid - l1.mid;
        out[15] = now.mid - l10.mid;
        out[16] = now.moves - l10.moves;
        out[17] = std::log10(1 + double(ts - last_move_ts_) / 1e3);
        out[18] = last_move_;
        return true;
    }

private:
    struct Row {
        std::int64_t ts;
        double ofi, tv, tn, moves, mid;
    };

    // Last recorded snapshot at or before `t`; the very first one if none is (as ml_signal._lag).
    const Row& lag(int w, std::int64_t t) {
        std::size_t& p = ptr_[w];  // absolute index of the candidate
        while (p + 1 < pushed_ && at(p + 1).ts <= t) ++p;
        if (at(p).ts > t) return first_;
        return at(p);
    }
    const Row& at(std::size_t abs) const { return hist_[abs - dropped_]; }
    void trim() {  // drop rows no window pointer can return again
        std::size_t keep = ptr_[0];
        for (auto p : ptr_) keep = p < keep ? p : keep;
        while (dropped_ < keep) {
            hist_.pop_front();
            ++dropped_;
        }
    }

    std::int64_t from_, to_;
    double cum_ofi_ = 0, cum_tv_ = 0, cum_tn_ = 0, cum_moves_ = 0;
    bool has_prev_ = false, has_mid_ = false;
    double pb_ = 0, pbq_ = 0, pa_ = 0, paq_ = 0;
    std::int64_t last_mid2_ = 0, last_move_ts_ = 0;
    double last_move_ = 0;
    std::deque<Row> hist_;
    Row first_{};
    std::size_t pushed_ = 0, dropped_ = 0;
    std::size_t ptr_[3] = {0, 0, 0};
};

}  // namespace ml

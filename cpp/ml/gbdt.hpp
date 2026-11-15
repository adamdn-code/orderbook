// Gradient-boosted tree ensemble, evaluated in C++.
//
// Loads the text format written by backtest/export_model.py from a scikit-learn
// HistGradientBoostingClassifier and returns the same probability scikit-learn does:
// sigmoid(baseline + sum of one leaf value per tree). A node sends a sample left when
// x[feature] <= threshold, or when x[feature] is NaN and the node says missing values go left,
// which are scikit-learn's rules.
//
// All trees live in one flat node array (24 bytes per node).
#pragma once

#include <algorithm>
#include <cmath>
#include <limits>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace ml {

struct Node {
    double thr;          // split threshold; +inf on leaves
    std::uint32_t left;  // absolute indices into the node array; a leaf points at itself
    std::uint32_t right;
    std::int16_t feature;
    std::uint8_t missing_left;
    std::uint8_t pad[5];
};
static_assert(sizeof(Node) == 24, "keep nodes compact");

// Evaluation walks kLanes trees side by side, one level per step, with no branches: a leaf
// points at itself (threshold +inf), so every tree in a group simply takes as many steps as the
// deepest one. The kLanes loads per step don't depend on each other, so the CPU overlaps their
// cache misses instead of waiting for each in turn. Leaf values are added in tree order, so the
// sum (and the probability) is bit-identical to scikit-learn's.
class Gbdt {
public:
    static constexpr int kLanes = 8;

    static Gbdt load(const std::string& path) {
        std::ifstream in(path);
        if (!in) throw std::runtime_error("can't open model file " + path);
        std::string magic;
        Gbdt g;
        std::size_t n_trees = 0;
        in >> magic >> g.n_features_ >> n_trees >> g.baseline_;
        if (magic != "gbdt1") throw std::runtime_error("not a gbdt1 model file: " + path);
        for (std::size_t t = 0; t < n_trees; ++t) {
            std::string tag;
            std::size_t n_nodes = 0;
            in >> tag >> n_nodes;
            if (tag != "tree") throw std::runtime_error("bad tree header in " + path);
            const auto base = std::uint32_t(g.nodes_.size());
            g.roots_.push_back(base);
            for (std::size_t i = 0; i < n_nodes; ++i) {
                long feature, left, right, leaf, miss;
                std::string thr;
                in >> feature >> thr >> left >> right >> leaf >> miss;
                const double v = std::strtod(thr.c_str(), nullptr);
                Node n{};
                const auto self = base + std::uint32_t(i);
                if (leaf) {
                    n.thr = std::numeric_limits<double>::infinity();
                    n.left = n.right = self;
                    n.feature = 0;
                    n.missing_left = 1;
                } else {
                    n.thr = v;
                    n.left = base + std::uint32_t(left);
                    n.right = base + std::uint32_t(right);
                    n.feature = std::int16_t(feature);
                    n.missing_left = std::uint8_t(miss);
                }
                g.nodes_.push_back(n);
                g.values_.push_back(leaf ? v : 0.0);
            }
            g.depth_.push_back(depth_of(g.nodes_, base, base));
        }
        if (!in) throw std::runtime_error("truncated model file " + path);
        for (std::size_t t = 0; t < g.roots_.size(); t += kLanes) {  // steps needed per group of trees
            int d = 0;
            for (std::size_t k = t; k < std::min(t + kLanes, g.roots_.size()); ++k) d = std::max(d, g.depth_[k]);
            g.group_depth_.push_back(d);
        }
        return g;
    }

    // Raw score (log-odds).
    double raw(const double* x) const {
        double s = baseline_;
        const Node* nodes = nodes_.data();
        const std::size_t n = roots_.size();
        std::size_t t = 0;
        for (std::size_t grp = 0; t + kLanes <= n; t += kLanes, ++grp) {
            std::uint32_t idx[kLanes];
            for (int k = 0; k < kLanes; ++k) idx[k] = roots_[t + std::size_t(k)];
            for (int d = 0; d < group_depth_[grp]; ++d) {
                for (int k = 0; k < kLanes; ++k) {
                    const Node& nd = nodes[idx[k]];
                    const double v = x[nd.feature];
                    idx[k] = step(nd, v);
                }
            }
            for (int k = 0; k < kLanes; ++k) s += values_[idx[k]];
        }
        for (; t < n; ++t) {  // leftover trees, one at a time
            std::uint32_t i = roots_[t];
            for (int d = 0; d < depth_[t]; ++d) {
                const Node& nd = nodes[i];
                const double v = x[nd.feature];
                i = step(nd, v);
            }
            s += values_[i];
        }
        return s;
    }

    double proba(const double* x) const { return 1.0 / (1.0 + std::exp(-raw(x))); }

    std::size_t n_features() const { return n_features_; }
    std::size_t n_trees() const { return roots_.size(); }
    std::size_t n_nodes() const { return nodes_.size(); }
    int max_depth() const { return depth_.empty() ? 0 : *std::max_element(depth_.begin(), depth_.end()); }

private:
    // Next node, chosen with a mask rather than a branch: split outcomes are close to coin flips,
    // and a mispredicted branch per level costs more than the whole comparison.
    static std::uint32_t step(const Node& nd, double v) {
        const std::uint32_t go_left = std::uint32_t(v <= nd.thr) | (std::uint32_t(v != v) & nd.missing_left);
        const std::uint32_t m = 0u - go_left;  // all ones if left
        return (nd.left & m) | (nd.right & ~m);
    }

    static int depth_of(const std::vector<Node>& nodes, std::uint32_t i, std::uint32_t) {
        const Node& n = nodes[i];
        if (n.left == i) return 0;
        return 1 + std::max(depth_of(nodes, n.left, 0), depth_of(nodes, n.right, 0));
    }

    std::vector<Node> nodes_;
    std::vector<double> values_;
    std::vector<std::uint32_t> roots_;
    std::vector<int> depth_, group_depth_;
    std::size_t n_features_ = 0;
    double baseline_ = 0;
};

}  // namespace ml

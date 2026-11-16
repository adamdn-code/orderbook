"""Figures in the site's working-paper style, drawn from docs/playground_data.json.

    python backtest/make_figures.py

writes
    backtest/results/ml/headline.png   Figure 1 of the playground (README, paper)
    backtest/results/ml/frontier.png   loss per share against volume, one line per filter (paper)
    backtest/results/ml/roc.png        ROC of the next-move model and imbalance (paper)
    docs/og.png                        1200x630 link-preview card (LinkedIn, Slack, iMessage)
"""

from __future__ import annotations

import json
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
from matplotlib import font_manager as fm  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
DATA = ROOT / "docs" / "playground_data.json"
INK, MUTED, RULE, PENCIL, NONE = "#111111", "#5c5c5c", "#e1e1dc", "#a8231a", "#7a7a7a"


def font(*names: str) -> str:
    have = {f.name for f in fm.fontManager.ttflist}
    return next((n for n in names if n in have), "serif")


SERIF = font("Source Serif 4", "Latin Modern Roman", "LM Roman 10", "DejaVu Serif")
SERIF_BOLD = font("Source Serif 4", "Latin Modern Roman Demi", "LM Roman Demi 10", "DejaVu Serif")
SANS = font("Archivo", "Inter", "DejaVu Sans")


def runs():
    d = json.loads(DATA.read_text())

    def get(f, rate):
        return next(r for r in d["runs"] if r["filter"] == f and abs(r["pull_rate"] - rate) < 1e-9
                    and r["latency_us"] == 0 and r["placement"] == "join" and r["action"] == "pull")

    pick = next(s["pull_rate"] for s in d["meta"]["selected"]
                if s["filter"] == "model" and s["latency_us"] == 0 and s["action"] == "pull")
    return d, get("model", pick), get("none", 0.0), pick


def money(v: float) -> str:
    return ("−$" if v < 0 else "$") + f"{abs(round(v)):,}"


def pnl_axes(ax, d, sel, base, label_size=10.5, tick_size=9.5, lw=2.0):
    t = d["mid"]["t"]
    n = min(len(t), len(sel["series"]["pnl"]), len(base["series"]["pnl"]))
    h = [x / 3600 for x in t[:n]]
    ax.axhline(0, color=INK, lw=0.8, zorder=1)
    ax.axvline(14, color=PENCIL, lw=0.9, ls=(0, (2, 3)), zorder=1)
    ax.text(14.04, 1.0, "Fed statement, 2:00 pm", transform=ax.get_xaxis_transform(), color=PENCIL,
            fontfamily=SERIF, fontstyle="italic", fontsize=label_size, va="top")
    ax.plot(h, base["series"]["pnl"][:n], color=NONE, lw=lw * 0.7, ls=(0, (5, 4)), zorder=2)
    ax.plot(h, sel["series"]["pnl"][:n], color=INK, lw=lw, zorder=3)
    for run, name, color in [(sel, "Next-move filter", INK), (base, "No filter", NONE)]:
        ax.annotate(f"{name}, {money(run['pnl_with_rebate'])}", (h[-1], run["series"]["pnl"][n - 1]),
                    xytext=(6, 0), textcoords="offset points", va="center", color=color,
                    fontfamily=SANS, fontsize=label_size, fontweight="semibold", annotation_clip=False)
    ax.set_xlim(h[0], h[-1])
    ax.set_xticks([13.5, 14, 14.5, 15, 15.5])
    ax.set_xticklabels(["1:30", "2:00", "2:30", "3:00", "3:30 pm"])
    ax.yaxis.set_major_formatter(matplotlib.ticker.FuncFormatter(lambda v, _: money(v)))
    ax.grid(axis="y", color=RULE, lw=0.8)
    ax.set_axisbelow(True)
    for s in ("top", "right", "left"):
        ax.spines[s].set_visible(False)
    ax.spines["bottom"].set_color(INK)
    ax.tick_params(colors=MUTED, labelsize=tick_size, length=0, pad=6)
    for lab in ax.get_xticklabels() + ax.get_yticklabels():
        lab.set_fontfamily(SANS)


def headline(d, sel, base, pick) -> Path:
    fig, ax = plt.subplots(figsize=(9, 4.2), dpi=200)
    fig.subplots_adjust(left=0.1, right=0.78, top=0.9, bottom=0.12)
    pnl_axes(ax, d, sel, base)
    out = ROOT / "backtest" / "results" / "ml" / "headline.png"
    fig.savefig(out, facecolor="white")
    plt.close(fig)
    return out


def og_card(d, sel, base, pick) -> Path:
    auc = d["model"]["labels"]["next_move"]["auc"]
    fig = plt.figure(figsize=(12, 6.3), dpi=100)
    fig.patch.set_facecolor("white")
    fig.text(0.05, 0.89, "Market making on Apple's order book,", fontfamily=SERIF_BOLD, fontsize=38, color=INK)
    fig.text(0.05, 0.805, "with and without an order-flow filter", fontfamily=SERIF_BOLD, fontsize=38, color=INK)
    fig.text(0.05, 0.735, f"Adam Nguyen  ·  Nasdaq TotalView-ITCH  ·  next-move AUC {auc['gbm']:.3f} vs {auc['imbalance']:.3f}  ·  "
             f"out-of-sample loss {money(base['pnl_with_rebate'])} → {money(sel['pnl_with_rebate'])}".replace("$", r"\$"),
             fontfamily=SANS, fontsize=14, color=MUTED)
    ax = fig.add_axes([0.08, 0.1, 0.68, 0.55])
    pnl_axes(ax, d, sel, base, label_size=13, tick_size=12, lw=2.6)
    out = ROOT / "docs" / "og.png"
    fig.savefig(out, facecolor="white")
    plt.close(fig)
    return out


SERIES = {"imbalance": ("Imbalance", "#8f6414"), "model": ("Next move", INK), "toxicity": ("Toxicity", "#6e4f8f")}


def style(ax, tick_size=9):
    ax.grid(axis="y", color=RULE, lw=0.8)
    ax.set_axisbelow(True)
    for s in ("top", "right", "left"):
        ax.spines[s].set_visible(False)
    ax.spines["bottom"].set_color(INK)
    ax.tick_params(colors=MUTED, labelsize=tick_size, length=0, pad=5)
    for lab in ax.get_xticklabels() + ax.get_yticklabels():
        lab.set_fontfamily(SANS)


def frontier(d, sel, base, pick) -> Path:
    fig, ax = plt.subplots(figsize=(6, 3.6), dpi=200)
    fig.subplots_adjust(left=0.13, right=0.8, top=0.95, bottom=0.17)
    sel_rates = {s["filter"]: s["pull_rate"] for s in d["meta"]["selected"] if s["latency_us"] == 0 and s["action"] == "pull"}
    ax.axhline(0, color=INK, lw=0.8)
    for f, (name, color) in SERIES.items():
        rs = sorted([r for r in d["runs"] if r["filter"] == f and r["latency_us"] == 0 and r["placement"] == "join"
                     and r["action"] == "pull"], key=lambda r: r["pull_rate"])
        x = [r["shares"] / 1000 for r in rs]
        y = [r["pnl_per_100_shares"] for r in rs]
        ax.plot(x, y, color=color, lw=2 if f == "model" else 1.2, marker="o", ms=3)
        for r in rs:
            if abs(r["pull_rate"] - sel_rates.get(f, -1)) < 1e-9:
                ax.plot(r["shares"] / 1000, r["pnl_per_100_shares"], "o", ms=9, mfc="none", mec=color, mew=1.2)
        end = max(rs, key=lambda r: r["shares"])
        ax.annotate(name, (end["shares"] / 1000, end["pnl_per_100_shares"]), xytext=(7, 0), textcoords="offset points",
                    va="center", color=color, fontfamily=SANS, fontsize=9, fontweight="semibold", annotation_clip=False)
    ax.plot(base["shares"] / 1000, base["pnl_per_100_shares"], "o", ms=6, mfc="white", mec=NONE, mew=1.4)
    ax.annotate("No filter", (base["shares"] / 1000, base["pnl_per_100_shares"]), xytext=(7, -2), textcoords="offset points",
                va="center", color=NONE, fontfamily=SANS, fontsize=9, fontweight="semibold", annotation_clip=False)
    ax.set_xlabel("shares traded on the test afternoon (thousands)", fontfamily=SANS, fontsize=9, color=MUTED)
    ax.yaxis.set_major_formatter(matplotlib.ticker.FuncFormatter(lambda v, _: ("−$" if v < 0 else "$") + f"{abs(v):.2f}"))
    style(ax)
    out = ROOT / "backtest" / "results" / "ml" / "frontier.png"
    fig.savefig(out, facecolor="white")
    plt.close(fig)
    return out


def roc(d, sel, base, pick) -> Path:
    m = d["model"]
    fig, ax = plt.subplots(figsize=(3.6, 3.4), dpi=200)
    fig.subplots_adjust(left=0.16, right=0.96, top=0.95, bottom=0.17)
    ax.plot([0, 1], [0, 1], color=RULE, lw=1, ls=(0, (4, 4)))
    ax.plot(m["roc"]["imbalance"]["fpr"], m["roc"]["imbalance"]["tpr"], color="#8f6414", lw=1.4,
            label=f"Imbalance, AUC {m['labels']['next_move']['auc']['imbalance']:.3f}")
    ax.plot(m["roc"]["gbm"]["fpr"], m["roc"]["gbm"]["tpr"], color=INK, lw=2,
            label=f"Next move, AUC {m['labels']['next_move']['auc']['gbm']:.3f}")
    ax.set_xlim(0, 1); ax.set_ylim(0, 1)
    ax.set_xlabel("false positive rate", fontfamily=SANS, fontsize=9, color=MUTED)
    ax.set_ylabel("true positive rate", fontfamily=SANS, fontsize=9, color=MUTED)
    leg = ax.legend(frameon=False, loc="lower right", prop={"family": SANS, "size": 8.5})
    style(ax)
    out = ROOT / "backtest" / "results" / "ml" / "roc.png"
    fig.savefig(out, facecolor="white")
    plt.close(fig)
    return out


if __name__ == "__main__":
    args = runs()
    for p in (headline(*args), og_card(*args), frontier(*args), roc(*args)):
        print("wrote", p.relative_to(ROOT))

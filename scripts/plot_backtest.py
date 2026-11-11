#!/usr/bin/env python3
"""Charts for the research note. Usage: plot_backtest.py <results_dir> <img_dir>"""
import csv, sys
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

res, img = sys.argv[1], sys.argv[2]
BLUE, ORANGE, INK, MUTED, SURFACE = "#2a78d6", "#eb6834", "#0b0b0b", "#52514e", "#fcfcfb"
plt.rcParams.update({
    "figure.facecolor": SURFACE, "axes.facecolor": SURFACE, "savefig.facecolor": SURFACE,
    "axes.edgecolor": "#c9c8c2", "axes.labelcolor": MUTED, "xtick.color": MUTED, "ytick.color": MUTED,
    "text.color": INK, "axes.spines.top": False, "axes.spines.right": False,
    "axes.grid": True, "grid.color": "#e8e7e2", "grid.linewidth": 0.8, "font.size": 10,
    "axes.titlesize": 11, "axes.titleweight": "bold", "axes.titlelocation": "left",
})

def load(path):
    with open(path) as f:
        rows = list(csv.DictReader(f))
    return {k: [float(r[k]) for r in rows] for k in rows[0]}

# --- Figure 1: one simulated hour, same market, two strategies -------------
a = load(f"{res}/path_gamma_0.csv")
b = load(f"{res}/path_gamma_0.05.csv")
mins = [t / 60 for t in a["t"]]
fig, ax = plt.subplots(3, 1, figsize=(9, 7.5), sharex=True)
ax[0].plot(mins, a["value"], color=MUTED, lw=1.2)
ax[0].set_title("Fundamental value (ticks)")
for d, col, name in ((a, ORANGE, "No skew (gamma = 0)"), (b, BLUE, "Avellaneda-Stoikov (gamma = 0.05)")):
    ax[1].plot(mins, d["inventory"], color=col, lw=1.4, label=name)
    ax[2].plot(mins, d["pnl"], color=col, lw=1.8, label=name)
ax[1].axhline(0, color="#c9c8c2", lw=1)
ax[1].set_title("Inventory (units)")
ax[1].legend(frameon=False, loc="upper left", ncol=2)
ax[2].set_title("PnL marked to true value (ticks x units)")
ax[2].set_xlabel("minutes")
ax[2].annotate(f"{a['pnl'][-1]:,.0f}", (mins[-1], a["pnl"][-1]), xytext=(4, 0), textcoords="offset points", va="center")
ax[2].annotate(f"{b['pnl'][-1]:,.0f}", (mins[-1], b["pnl"][-1]), xytext=(4, 0), textcoords="offset points", va="center")
fig.suptitle("One simulated hour, identical order flow (seed 1)", x=0.02, ha="left", fontsize=13, fontweight="bold")
fig.tight_layout(rect=(0, 0, 0.97, 0.97))
fig.savefig(f"{img}/backtest_paths.png", dpi=160)

# --- Figure 2: the risk / return trade-off across gamma --------------------
g = load(f"{res}/gamma_sweep.csv")
labels = [f"{x:g}" for x in g["gamma"]]
fig, ax = plt.subplots(1, 3, figsize=(13.5, 3.9))
panels = (("mean_pnl", "Mean PnL per hour"), ("sd_pnl", "Std dev of PnL across runs"), ("mean_abs_inv", "Average |inventory|"))
for axi, (key, title) in zip(ax, panels):
    bars = axi.bar(labels, g[key], color=BLUE, width=0.62)
    axi.axhline(0, color="#c9c8c2", lw=1)
    axi.set_title(title)
    axi.set_xlabel("risk aversion gamma")
    axi.grid(axis="x", visible=False)
    axi.set_axisbelow(True)
    axi.margins(y=0.12)
    for r, v in zip(bars, g[key]):
        txt = f"{v / 1000:.1f}k" if abs(v) >= 1000 else f"{v:.0f}"
        axi.annotate(txt, (r.get_x() + r.get_width() / 2, v), xytext=(0, 3 if v >= 0 else -11),  # label above positive bars, below negative ones
                     textcoords="offset points", ha="center", fontsize=8, color=MUTED)
fig.suptitle("Inventory skew buys a large drop in risk for a small drop in return, until it is overdone",
             x=0.01, ha="left", fontsize=12, fontweight="bold")
fig.tight_layout(rect=(0, 0, 1, 0.93))
fig.savefig(f"{img}/gamma_sweep.png", dpi=160)

# --- Figure 3: edge per unit by counterparty as toxicity rises -------------
t = load(f"{res}/toxicity_sweep.csv")
fig, axi = plt.subplots(figsize=(7.5, 4))
x = [f"{v:g}" for v in t["inf_rate"]]
axi.plot(x, t["quoted_edge"], color=MUTED, lw=2, ls="--", marker="o", ms=5, label="Quoted edge (vs our reference mid)")
axi.plot(x, t["realized_edge"], color=BLUE, lw=2, marker="o", ms=6, label="Realized edge (vs true value)")
axi.set_ylim(0, 2.2)
axi.set_xlabel("informed-trader arrivals per second")
axi.set_ylabel("ticks per unit traded")
axi.set_title("Adverse selection: the edge we quote vs the edge we keep (gamma = 0.05)")
axi.legend(frameon=False, loc="lower left")
fig.tight_layout()
fig.savefig(f"{img}/adverse_selection.png", dpi=160)
print("wrote 3 charts to", img)

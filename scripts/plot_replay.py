#!/usr/bin/env python3
"""Charts for the real-data replay. Usage: plot_replay.py <replay_results_dir> <img_dir>"""
import csv, sys
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

res, img = sys.argv[1], sys.argv[2]
C = ["#2a78d6", "#eb6834", "#1baf7a", "#eda100"]
INK, MUTED, SURFACE = "#0b0b0b", "#52514e", "#fcfcfb"
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

# --- the day: price, inventory, PnL for two settings -----------------------
a = load(f"{res}/replay_path_s14_g0.csv")
b = load(f"{res}/replay_path_s14_g0.1.csv")
hours = [9.5 + t / 3600 for t in a["t"]]
fig, ax = plt.subplots(3, 1, figsize=(9, 7.5), sharex=True)
ax[0].plot(hours, [m / 100 for m in a["mid"]], color=MUTED, lw=1.2)
ax[0].set_title("AMZN mid price, 21 June 2012 (USD)")
for d, col, name in ((a, C[1], "No skew (gamma = 0)"), (b, C[0], "Avellaneda-Stoikov (gamma = 0.1)")):
    ax[1].plot(hours, d["inventory"], color=col, lw=1.3, label=name)
    ax[2].plot(hours, d["pnl_usd"], color=col, lw=1.8, label=name)
for i in (1, 2):
    ax[i].axhline(0, color="#c9c8c2", lw=1)
ax[1].set_title("Market maker inventory (shares)")
ax[2].set_title("Market maker PnL, marked to mid, no fees (USD)")
ax[2].set_xlabel("time of day (hours)")
ax[2].legend(frameon=False, loc="upper right")
fig.suptitle("Replay of a real trading day: both versions lose money (baseline spread 14 ticks)",
             x=0.02, ha="left", fontsize=13, fontweight="bold")
fig.tight_layout(rect=(0, 0, 0.96, 0.97))
fig.savefig(f"{img}/replay_day.png", dpi=160)

# --- markouts: what a fill is worth as time passes --------------------------
s = load(f"{res}/replay_sweep.csv")
fig, axi = plt.subplots(figsize=(8, 4.4))
xs = ["at the fill", "1 s later", "10 s later", "60 s later"]
k = 0
for i in range(len(s["gamma"])):
    if abs(s["gamma"][i] - 0.05) > 1e-9:
        continue
    ys = [s["edge_cents"][i], s["markout_1s"][i], s["markout_10s"][i], s["markout_60s"][i]]
    axi.plot(xs, ys, color=C[k], lw=2, marker="o", ms=6, label=f"baseline spread {s['spread_ticks'][i]:g} ticks")
    k += 1
axi.axhline(0, color=MUTED, lw=1)
axi.set_ylabel("cents per share, from the maker's side")
axi.set_title("Value of the maker's fills over time (gamma = 0.05)")
axi.legend(frameon=False, loc="upper right")
fig.tight_layout()
fig.savefig(f"{img}/replay_markouts.png", dpi=160)
print("wrote replay_day.png and replay_markouts.png to", img)

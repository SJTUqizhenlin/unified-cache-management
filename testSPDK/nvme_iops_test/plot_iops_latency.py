#!/usr/bin/env python3
"""Plot IOPS-vs-latency curves from the QD sweep CSVs.

Reads iops_sweep_kernel.csv and iops_sweep_spdk.csv (same directory),
draws both into one figure: x = IOPS, y = avg latency (us).
Annotated with queue depths, saved as iops_vs_latency.png.
"""

import csv
import os
import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt

HERE = os.path.dirname(os.path.abspath(__file__))


def load(path):
    rows = []
    with open(path) as f:
        for line in f:
            if line.startswith("#") or line.strip().startswith("qd,"):
                continue
            qd, iops, tp, lat, comp = line.strip().split(",")
            rows.append((int(qd), float(iops), float(lat)))
    return rows


def main():
    kernel = load(os.path.join(HERE, "iops_sweep_kernel.csv"))
    spdk = load(os.path.join(HERE, "iops_sweep_spdk.csv"))

    fig, ax = plt.subplots(figsize=(9, 6.5))

    for rows, label, marker, color in (
        (kernel, "kernel (libaio, ext4 on 83:00.0)", "o", "tab:blue"),
        (spdk, "SPDK (raw NVMe on 84:00.0)", "s", "tab:red"),
    ):
        xs = [r[1] / 1000.0 for r in rows]  # KIOPS
        ys = [r[2] for r in rows]
        ax.plot(xs, ys, marker=marker, color=color, linewidth=1.8,
                markersize=6, label=label)
        for qd, x, y in zip(xs, ys, ys):
            ax.annotate(f"qd={qd}", (x, y), textcoords="offset points",
                        xytext=(6, 5), fontsize=8, color=color)

    ax.set_xlabel("IOPS (thousand), 4KB random read, 80 GiB span")
    ax.set_ylabel("Average latency (us)")
    ax.set_title("IOPS vs Latency: kernel/libaio vs SPDK (same SSD model, "
                 "GLM-5.3 era tooling)")
    ax.grid(True, which="both", linestyle=":", alpha=0.6)
    ax.legend()
    fig.tight_layout()

    out = os.path.join(HERE, "iops_vs_latency.png")
    fig.savefig(out, dpi=150)
    print(f"saved: {out}")


if __name__ == "__main__":
    main()

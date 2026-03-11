#!/usr/bin/env python3
"""
Plot Throughput vs Block Size for Linux I/O benchmark (syscall, mmap, io_uring).
Generates 3 figures in this folder (Graphs).
"""

import os
import pandas as pd
import matplotlib.pyplot as plt
import numpy as np

# Paths: script lives in Graphs/, results are in ../results
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
RESULTS_DIR = os.path.normpath(os.path.join(SCRIPT_DIR, "..", "results"))
GRAPHS_DIR = SCRIPT_DIR

BLOCK_SIZES_BYTES = [4096, 65536, 1048576]  # 4KB, 64KB, 1MB
BLOCK_SIZE_LABELS = ["4 KB", "64 KB", "1 MB"]
INTERFACE_ORDER = ["syscall", "mmap", "io_uring"]
INTERFACE_COLORS = {"syscall": "#2ecc71", "mmap": "#3498db", "io_uring": "#e74c3c"}
INTERFACE_MARKERS = {"syscall": "o", "mmap": "s", "io_uring": "^"}


def load_and_merge_csvs():
    """Load syscall, mmap, io_uring CSVs and merge into one DataFrame."""
    paths = {
        "syscall": os.path.join(RESULTS_DIR, "syscall_results.csv"),
        "mmap": os.path.join(RESULTS_DIR, "mmap_results.csv"),
        "io_uring": os.path.join(RESULTS_DIR, "results_iouring.csv"),
    }
    dfs = []
    for interface, path in paths.items():
        if not os.path.isfile(path):
            print(f"Warning: {path} not found, skipping.")
            continue
        df = pd.read_csv(path)
        # Drop completely empty rows (io_uring has blank lines)
        df = df.dropna(how="all")
        # Normalize column names (Minor_faults vs minor_faults)
        col_map = {c: "minor_faults" for c in df.columns if str(c).strip().lower() == "minor_faults"}
        if col_map:
            df = df.rename(columns=col_map)
        if "interface" not in df.columns or df["interface"].isna().all():
            df["interface"] = interface
        else:
            # Normalize interface name for io_uring
            df["interface"] = df["interface"].str.strip().str.lower().replace("iouring", "io_uring")
        # Filter invalid workload (typo in some io_uring rows)
        valid_workloads = {"seq_read", "seq_write", "rand_read", "rand_write", "mixed_rand"}
        df = df[df["workload"].astype(str).str.strip().isin(valid_workloads)]
        df = df[df["block_size_bytes"].isin(BLOCK_SIZES_BYTES)]
        dfs.append(df)
    if not dfs:
        raise FileNotFoundError("No CSV files found in results/")
    return pd.concat(dfs, ignore_index=True)


def aggregate_throughput(df, workload, cache_state, workload_label=None):
    """
    Filter by workload and cache_state, then average throughput per (interface, block_size).
    workload can be a string or list (e.g. ['seq_read','seq_write']).
    """
    if isinstance(workload, str):
        workload = [workload]
    subset = df[
        (df["workload"].astype(str).str.strip().isin(workload))
        & (df["cache_state"].astype(str).str.strip().str.lower() == cache_state)
    ]
    if subset.empty:
        return None
    agg = (
        subset.groupby(["interface", "block_size_bytes"])["throughput_MBps"]
        .agg(["mean", "std"])
        .reset_index()
    )
    return agg


def plot_throughput_subplot(ax, agg, title):
    """Draw throughput vs block size lines for syscall, mmap, io_uring on given axes."""
    if agg is None or agg.empty:
        ax.set_title(title + " (no data)")
        return
    x_pos = np.arange(len(BLOCK_SIZES_BYTES))
    for iface in INTERFACE_ORDER:
        iface_data = agg[agg["interface"].str.strip().str.lower() == iface.lower()]
        if iface_data.empty:
            continue
        # Ensure order 4KB, 64KB, 1MB
        sorted_ = iface_data.set_index("block_size_bytes").reindex(BLOCK_SIZES_BYTES).reset_index()
        means = sorted_["mean"].values
        stds = sorted_["std"].values
        # Drop NaN from missing block sizes
        valid = ~np.isnan(means)
        if not np.any(valid):
            continue
        x = x_pos[valid]
        y = np.where(np.isnan(means), 0, means)[valid]
        err = np.where(np.isnan(stds), 0, stds)[valid]
        color = INTERFACE_COLORS.get(iface, None)
        marker = INTERFACE_MARKERS.get(iface, "o")
        ax.plot(x, y, "-", color=color, marker=marker, label=iface, linewidth=2, markersize=8)
        if np.any(err > 0):
            ax.fill_between(x, y - err, y + err, color=color, alpha=0.2)
    ax.set_xticks(x_pos)
    ax.set_xticklabels(BLOCK_SIZE_LABELS)
    ax.set_ylabel("Throughput (MB/s)")
    ax.set_xlabel("Block size")
    ax.set_title(title)
    ax.legend()
    ax.grid(True, alpha=0.3)


def main():
    os.makedirs(GRAPHS_DIR, exist_ok=True)
    df = load_and_merge_csvs()

    # --- Figure 1: Sequential Read – Warm, Sequential Write – Warm ---
    fig1, (ax1a, ax1b) = plt.subplots(1, 2, figsize=(12, 5))
    for ax, workload, title in [
        (ax1a, "seq_read", "Sequential Read – Warm"),
        (ax1b, "seq_write", "Sequential Write – Warm"),
    ]:
        agg = aggregate_throughput(df, workload, "warm")
        plot_throughput_subplot(ax, agg, title)
    fig1.suptitle("Throughput vs Block Size (Sequential, Warm)", fontsize=12)
    fig1.tight_layout()
    fig1.savefig(os.path.join(GRAPHS_DIR, "throughput_vs_blocksize_sequential_warm.png"), dpi=150)
    plt.close(fig1)
    print("Saved: throughput_vs_blocksize_sequential_warm.png")

    # --- Figure 2: Random Read – Cold, Random Write – Cold ---
    fig2, (ax2a, ax2b) = plt.subplots(1, 2, figsize=(12, 5))
    for ax, workload, title in [
        (ax2a, "rand_read", "Random Read – Cold"),
        (ax2b, "rand_write", "Random Write – Cold"),
    ]:
        agg = aggregate_throughput(df, workload, "cold")
        plot_throughput_subplot(ax, agg, title)
    fig2.suptitle("Throughput vs Block Size (Random, Cold)", fontsize=12)
    fig2.tight_layout()
    fig2.savefig(os.path.join(GRAPHS_DIR, "throughput_vs_blocksize_random_cold.png"), dpi=150)
    plt.close(fig2)
    print("Saved: throughput_vs_blocksize_random_cold.png")

    # --- Figure 3: Mixed random – Warm and Cold ---
    fig3, (ax3a, ax3b) = plt.subplots(1, 2, figsize=(12, 5))
    for ax, cache, title in [
        (ax3a, "warm", "Mixed Random (70R/30W) – Warm"),
        (ax3b, "cold", "Mixed Random (70R/30W) – Cold"),
    ]:
        agg = aggregate_throughput(df, "mixed_rand", cache)
        plot_throughput_subplot(ax, agg, title)
    fig3.suptitle("Throughput vs Block Size (Mixed Random)", fontsize=12)
    fig3.tight_layout()
    fig3.savefig(os.path.join(GRAPHS_DIR, "throughput_vs_blocksize_mixed_warm_cold.png"), dpi=150)
    plt.close(fig3)
    print("Saved: throughput_vs_blocksize_mixed_warm_cold.png")

    print("Done. All plots are in", GRAPHS_DIR)


if __name__ == "__main__":
    main()

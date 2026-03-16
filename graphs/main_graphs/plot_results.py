import pandas as pd
import matplotlib.pyplot as plt
from pathlib import Path
import pandas as pd
import matplotlib.pyplot as plt
from pathlib import Path

sys_df = pd.read_csv("syscall_results.csv")
mmap_df = pd.read_csv("mmap_results.csv")
io_df = pd.read_csv("iouring_results.csv")

df = pd.concat([sys_df, mmap_df, io_df], ignore_index=True)

group_cols = [
    "interface",
    "workload",
    "cache_state",
    "block_size_bytes",
    "threads"
]

df = df.groupby(group_cols, as_index=False).mean()

block_map = {
    4096: "4KB",
    65536: "64KB",
    1048576: "1MB"
}

df["block_label"] = df["block_size_bytes"].map(block_map)

# CPU total
df["cpu_total"] = df["cpu_user_seconds"] + df["cpu_sys_seconds"]

interfaces = ["syscall", "mmap", "io_uring"]

out = Path("report_plots")
out.mkdir(exist_ok=True)

plt.rcParams["figure.figsize"] = (12,5)
plt.rcParams["axes.grid"] = True

fig, ax = plt.subplots(1,2, figsize=(12,5))

# Sequential Warm
sub = df[
    (df["workload"]=="seq_read") &
    (df["cache_state"]=="warm") &
    (df["threads"]==1)
]

for i in interfaces:
    s = sub[sub["interface"]==i].sort_values("block_size_bytes")
    if not s.empty:
        ax[0].plot(s["block_label"], s["throughput_MBps"], marker="o", label=i)

ax[0].set_title("Sequential Read (Warm Cache)")
ax[0].set_xlabel("Block Size")
ax[0].set_ylabel("Throughput (MB/s)")
ax[0].legend()


# Random Cold
sub = df[
    (df["workload"]=="rand_read") &
    (df["cache_state"]=="cold") &
    (df["threads"]==1)
]

for i in interfaces:
    s = sub[sub["interface"]==i].sort_values("block_size_bytes")
    if not s.empty:
        ax[1].plot(s["block_label"], s["throughput_MBps"], marker="o", label=i)

ax[1].set_title("Random Read (Cold Cache)")
ax[1].set_xlabel("Block Size")

plt.suptitle("Throughput vs Block Size")

plt.tight_layout()
plt.savefig("throughput_blocksize_sidebyside.png", dpi=300)
plt.show()

fig, ax = plt.subplots(1,2, figsize=(12,5))

# seq warm 1MB
sub = df[
    (df["workload"]=="seq_read") &
    (df["cache_state"]=="warm") &
    (df["block_size_bytes"]==1048576)
]

for i in interfaces:
    s = sub[sub["interface"]==i].sort_values("threads")
    if not s.empty:
        ax[0].plot(s["threads"], s["throughput_MBps"], marker="o", label=i)

ax[0].set_title("Sequential Read (Warm Cache, 1MB)")
ax[0].set_xlabel("Threads")
ax[0].set_ylabel("Throughput (MB/s)")
ax[0].legend()


# rand cold 4KB
sub = df[
    (df["workload"]=="rand_read") &
    (df["cache_state"]=="cold") &
    (df["block_size_bytes"]==4096)
]

for i in interfaces:
    s = sub[sub["interface"]==i].sort_values("threads")
    if not s.empty:
        ax[1].plot(s["threads"], s["throughput_MBps"], marker="o", label=i)

ax[1].set_title("Random Read (Cold Cache, 4KB)")
ax[1].set_xlabel("Threads")

plt.suptitle("Concurrency Scaling")

plt.tight_layout()
plt.savefig("concurrency_scaling.png", dpi=300)
plt.show()

fig, ax = plt.subplots(1,2, figsize=(12,5))

# seq warm 1MB
sub = df[
    (df["workload"]=="seq_read") &
    (df["cache_state"]=="warm") &
    (df["block_size_bytes"]==1048576) &
    (df["threads"]==1)
]

cpu = sub.groupby("interface")["cpu_total"].mean()

ax[0].bar(cpu.index, cpu.values)
ax[0].set_title("CPU Time (Sequential Warm 1MB)")
ax[0].set_ylabel("CPU Seconds")


# rand cold 4KB
sub = df[
    (df["workload"]=="rand_read") &
    (df["cache_state"]=="cold") &
    (df["block_size_bytes"]==4096) &
    (df["threads"]==1)
]

cpu = sub.groupby("interface")["cpu_total"].mean()

ax[1].bar(cpu.index, cpu.values)
ax[1].set_title("CPU Time (Random Cold 4KB)")

plt.suptitle("CPU Overhead Comparison")

plt.tight_layout()
plt.savefig("cpu_overhead.png", dpi=300)
plt.show()

print("All graphs saved in:", out)
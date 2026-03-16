# Linux File I/O Interface Benchmarking

## Overview

This project benchmarks and compares three Linux file I/O interfaces:

-   **System Call I/O** (`read()` / `write()`)
-   **Memory-Mapped I/O** (`mmap`)
-   **Asynchronous I/O** (`io_uring`)

Each interface has different trade-offs in terms of performance, CPU
overhead, and complexity. The goal of this project is to systematically
evaluate these interfaces under controlled workloads and analyze their
behavior using performance metrics and visualizations.

The benchmarks evaluate how these interfaces perform under different
conditions such as:

-   Sequential vs Random access
-   Different block sizes
-   Thread scaling
-   Cache states (warm vs cold)

The collected results are used to generate graphs comparing throughput
and latency across interfaces.

------------------------------------------------------------------------

# Folder Descriptions

## `syscall/`

Contains the benchmark implementation using **traditional system call
I/O**

------------------------------------------------------------------------

## `mmap/`

Contains the benchmark implementation using **memory-mapped file I/O**

------------------------------------------------------------------------

## `io_uring/`

Contains the benchmark implementation using **asynchronous I/O with
io_uring**

------------------------------------------------------------------------

## `results/`

Stores the **CSV output files produced by the benchmarks**.

Files:

-   `syscall_results.csv`
-   `mmap_results.csv`
-   `io_uring_results.csv`

These files contain metrics such as throughput, latency, CPU usage, page
faults, and context switches.

------------------------------------------------------------------------

## `graphs/`

Contains Python scripts used to generate graphs and visualizations from
benchmark results.

### `main_graphs/`

Contains plots and script used to generate the **primary graphs included in the
project report**

### `extra_graphs/`

Contains **additional exploratory graphs** that
provide deeper insight into system behavior but are not required for the
main report

------------------------------------------------------------------------

# Metrics Collected

Each benchmark run records the following metrics:

  Metric              Description
  ------------------- -----------------------------------
  throughput_MBps     Data processed per second
  latency_us_per_op   Average latency per I/O operation
  cpu_user_seconds    CPU time spent in user mode
  cpu_sys_seconds     CPU time spent in kernel mode
  minor_faults        Minor page faults
  major_faults        Major page faults
  voluntary_ctx       Voluntary context switches
  involuntary_ctx     Involuntary context switches

------------------------------------------------------------------------

# Workloads Tested

The benchmarks evaluate several I/O workloads:

-   Sequential Read
-   Sequential Write
-   Random Read
-   Random Write
-   Mixed Workload (70% read, 30% write)

Each workload is tested with different parameters:

**Block Sizes**

-   4 KB
-   64 KB
-   1 MB

**Thread Counts**

-   1
-   4
-   8

**Cache States**

-   Warm cache
-   Cold cache

------------------------------------------------------------------------

# Requirements

## C++ Compiler

g++

## Python Libraries

-   pandas
-   matplotlib
-   seaborn

Install using:

pip install pandas matplotlib seaborn

------------------------------------------------------------------------

# Running the Benchmarks

Compile a benchmark implementation:

For example, g++ bench_syscall.cpp -o bench_syscall

Run the executable to generate results stored in the `results/`
directory.

------------------------------------------------------------------------

# Generating Graphs

Generate graphs using:

python graphs/main_graphs/plot_results.py

This script reads the CSV files in the `results/` folder and produces
the visualizations.

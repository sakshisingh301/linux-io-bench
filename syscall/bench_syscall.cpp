#include <atomic>
#include <cerrno>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <random>
#include <string>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <thread>
#include <unistd.h>
#include <vector>

static inline uint64_t now_ns() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static inline double tv_to_seconds(const timeval &tv) {
  return (double)tv.tv_sec + (double)tv.tv_usec / 1e6;
}

struct RUsageDelta {
  double user_s = 0.0;
  double sys_s = 0.0;
  long minflt = 0;
  long majflt = 0;
  long nvcsw = 0;
  long nivcsw = 0;
};

static RUsageDelta diff_rusage(const rusage &a, const rusage &b) {
  RUsageDelta d;
  d.user_s = tv_to_seconds(b.ru_utime) - tv_to_seconds(a.ru_utime);
  d.sys_s  = tv_to_seconds(b.ru_stime) - tv_to_seconds(a.ru_stime);
  d.minflt = b.ru_minflt - a.ru_minflt;
  d.majflt = b.ru_majflt - a.ru_majflt;
  d.nvcsw  = b.ru_nvcsw  - a.ru_nvcsw;
  d.nivcsw = b.ru_nivcsw - a.ru_nivcsw;
  return d;
}

static bool file_exists(const std::string &p) {
  struct stat st{};
  return stat(p.c_str(), &st) == 0;
}

static uint64_t get_file_size(const std::string &p) {
  struct stat st{};
  if (stat(p.c_str(), &st) != 0) {
    perror("stat");
    exit(1);
  }
  return (uint64_t)st.st_size;
}

// Create a file of size bytes
static void ensure_file(const std::string &path, uint64_t bytes, uint32_t block_size) {
  if (file_exists(path) && get_file_size(path) == bytes) return;

  std::cerr << "Creating file: " << path << " size=" << bytes << " bytes\n";
  int fd = ::open(path.c_str(), O_CREAT | O_TRUNC | O_RDWR, 0644);
  if (fd < 0) { perror("open(create)"); exit(1); }

  if (ftruncate(fd, (off_t)bytes) != 0) { perror("ftruncate"); exit(1); }

  // Writes 1 byte at the start of each block
  std::vector<char> one(1, 0);
  uint64_t num_blocks = bytes / block_size;
  for (uint64_t i = 0; i < num_blocks; i++) {
    off_t off = (off_t)(i * (uint64_t)block_size);
    if (pwrite(fd, one.data(), 1, off) != 1) { perror("pwrite(touch)"); exit(1); }
  }

  if (fsync(fd) != 0) { perror("fsync(create)"); exit(1); }
  close(fd);
}

enum class Workload { SEQ_READ, RAND_READ, MIXED_70R30W };

static std::string wl_name(Workload w) {
  switch (w) {
    case Workload::SEQ_READ: return "seq_read";
    case Workload::RAND_READ: return "rand_read";
    case Workload::MIXED_70R30W: return "mixed_70r30w";
  }
  return "unknown";
}

struct Args {
  std::string file_path;
  std::string out_csv;

  Workload workload = Workload::SEQ_READ;
  std::string cache_state = "warm"; 

  uint64_t file_size = 2ull * 1024 * 1024 * 1024; // default 2GB
  uint32_t block_size = 4096;

  int threads = 1;
  uint32_t qd = 1; // unused for syscall; still recorded
  uint32_t seed = 42;
  int repetition = 0;

  bool do_create = false;
};

static void usage(const char *prog) {
  std::cerr <<
    "Usage:\n"
    "  " << prog << " --file PATH --out OUT.csv --workload seq|rand|mixed \n"
    "       --file_size BYTES --block_size BYTES --threads N --seed 42 \n"
    "       --cache warm|cold --rep k [--create]\n\n"
    "Notes:\n"
    "  - syscall impl uses pread/pwrite for correctness with threads.\n"
    "  - --cache cold: best-effort (will call posix_fadvise DONTNEED; drop_caches requires sudo).\n";
}

static Args parse(int argc, char **argv) {
  Args a;
  for (int i = 1; i < argc; i++) {
    std::string k = argv[i];
    auto need = [&](const char *name) -> std::string {
      if (i + 1 >= argc) { std::cerr << "Missing value for " << name << "\n"; exit(1); }
      return argv[++i];
    };

    if (k == "--file") a.file_path = need("--file");
    else if (k == "--out") a.out_csv = need("--out");
    else if (k == "--workload") {
      auto v = need("--workload");
      if (v == "seq") a.workload = Workload::SEQ_READ;
      else if (v == "rand") a.workload = Workload::RAND_READ;
      else if (v == "mixed") a.workload = Workload::MIXED_70R30W;
      else { std::cerr << "Unknown workload: " << v << "\n"; exit(1); }
    } else if (k == "--cache") {
      a.cache_state = need("--cache");
      if (a.cache_state != "warm" && a.cache_state != "cold") {
        std::cerr << "cache must be warm|cold\n"; exit(1);
      }
    } else if (k == "--file_size") {
      a.file_size = std::stoull(need("--file_size"));
    } else if (k == "--block_size") {
      a.block_size = (uint32_t)std::stoul(need("--block_size"));
    } else if (k == "--threads") {
      a.threads = std::stoi(need("--threads"));
    } else if (k == "--qd") {
      a.qd = (uint32_t)std::stoul(need("--qd"));
    } else if (k == "--seed") {
      a.seed = (uint32_t)std::stoul(need("--seed"));
    } else if (k == "--rep") {
      a.repetition = std::stoi(need("--rep"));
    } else if (k == "--create") {
      a.do_create = true;
    } else if (k == "--help") {
      usage(argv[0]); exit(0);
    } else {
      std::cerr << "Unknown arg: " << k << "\n";
      usage(argv[0]); exit(1);
    }
  }

  if (a.file_path.empty() || a.out_csv.empty()) {
    usage(argv[0]); exit(1);
  }
  return a;
}

static std::vector<uint64_t> make_offsets(uint64_t file_size, uint32_t block_size, Workload w, uint32_t seed) {
  uint64_t num_blocks = file_size / block_size;
  std::vector<uint64_t> offs;
  offs.reserve((size_t)num_blocks);

  if (w == Workload::SEQ_READ) {
    for (uint64_t i = 0; i < num_blocks; i++) offs.push_back(i * (uint64_t)block_size);
  } else {
    std::mt19937 rng(seed);
    std::uniform_int_distribution<uint64_t> dist(0, num_blocks - 1);
    for (uint64_t i = 0; i < num_blocks; i++) {
      uint64_t bi = dist(rng);
      offs.push_back(bi * (uint64_t)block_size);
    }
  }
  return offs;
}

static void best_effort_cold_cache(int fd, uint64_t file_size) {
  (void)file_size;
#ifdef POSIX_FADV_DONTNEED
  if (posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED) != 0) {
  }
#endif
}

struct RunResult {
  double wall_s = 0.0;
  uint64_t total_bytes = 0;
  uint64_t num_ops = 0;
  RUsageDelta ru{};
};

static RunResult run_syscall(const Args &a) {
  int fd = ::open(a.file_path.c_str(), O_RDWR);
  if (fd < 0) { perror("open"); exit(1); }

  if (a.cache_state == "cold") best_effort_cold_cache(fd, a.file_size);

  auto offsets = make_offsets(a.file_size, a.block_size, a.workload, a.seed);
  uint64_t num_ops = offsets.size();
  uint64_t total_bytes = num_ops * (uint64_t)a.block_size;

  std::vector<char> buf(a.block_size);
  for (size_t i = 0; i < buf.size(); i++) buf[i] = (char)(i * 131u + 7u);

  std::atomic<uint64_t> idx{0};

  rusage ru0{}, ru1{};
  getrusage(RUSAGE_SELF, &ru0);
  uint64_t t0 = now_ns();

  auto worker = [&](int /*tid*/) {
    // Each thread uses its own buffer to avoid false sharing
    std::vector<char> local(buf.begin(), buf.end());
    std::mt19937 rng(a.seed + 12345);
    std::uniform_real_distribution<double> prob(0.0, 1.0);

    while (true) {
      uint64_t i = idx.fetch_add(1, std::memory_order_relaxed);
      if (i >= num_ops) break;

      uint64_t off = offsets[(size_t)i];

      if (a.workload == Workload::MIXED_70R30W) {
        double p = prob(rng);
        if (p < 0.7) {
          ssize_t r = pread(fd, local.data(), a.block_size, (off_t)off);
          if (r < 0) { perror("pread"); exit(1); }
        } else {
          ssize_t w = pwrite(fd, local.data(), a.block_size, (off_t)off);
          if (w < 0) { perror("pwrite"); exit(1); }
        }
      } else {
        // seq_read or rand_read
        ssize_t r = pread(fd, local.data(), a.block_size, (off_t)off);
        if (r < 0) { perror("pread"); exit(1); }
      }
    }
  };

  std::vector<std::thread> th;
  th.reserve((size_t)a.threads);
  for (int t = 0; t < a.threads; t++) th.emplace_back(worker, t);
  for (auto &x : th) x.join();

  uint64_t t1 = now_ns();
  getrusage(RUSAGE_SELF, &ru1);

  close(fd);

  RunResult rr;
  rr.wall_s = (double)(t1 - t0) / 1e9;
  rr.total_bytes = total_bytes;
  rr.num_ops = num_ops;
  rr.ru = diff_rusage(ru0, ru1);
  return rr;
}

static void append_csv_row(const Args &a, const RunResult &r) {
  // CSV header (only if file doesn't exist)
  bool new_file = !file_exists(a.out_csv);
  FILE *f = fopen(a.out_csv.c_str(), "a");
  if (!f) { perror("fopen(out)"); exit(1); }

  if (new_file) {
    fprintf(f,
      "interface,workload,cache_state,file_size_bytes,block_size_bytes,threads,qd,seed,repetition,"
      "wall_seconds,total_bytes,throughput_MBps,latency_us_per_op,"
      "cpu_user_seconds,cpu_sys_seconds,"
      "Minor_faults,major_faults,voluntary_ctx,involuntary_ctx\n"
    );
  }

  double throughput = ((double)r.total_bytes / r.wall_s) / (1024.0 * 1024.0);
  double latency_us = (r.wall_s * 1e6) / (double)r.num_ops;

  fprintf(f,
    "syscall,%s,%s,%" PRIu64 ",%u,%d,%u,%u,%d,"
    "%.9f,%" PRIu64 ",%.6f,%.3f,"
    "%.6f,%.6f,"
    "%ld,%ld,%ld,%ld\n",
    wl_name(a.workload).c_str(),
    a.cache_state.c_str(),
    a.file_size,
    a.block_size,
    a.threads,
    a.qd,
    a.seed,
    a.repetition,
    r.wall_s,
    r.total_bytes,
    throughput,
    latency_us,
    a.cache_state == "cold" ? r.ru.user_s : r.ru.user_s,
    a.cache_state == "cold" ? r.ru.sys_s : r.ru.sys_s,
    r.ru.minflt,
    r.ru.majflt,
    r.ru.nvcsw,
    r.ru.nivcsw
  );

  fclose(f);
}

int main(int argc, char **argv) {
  Args a = parse(argc, argv);

  if (a.do_create) {
    ensure_file(a.file_path, a.file_size, a.block_size);
    std::cerr << "Created file: " << a.file_path
              << " size=" << a.file_size
              << " block=" << a.block_size << "\n";
    return 0;
  } else {
    if (!file_exists(a.file_path)) {
      std::cerr << "File not found. Use --create first.\n";
      return 1;
    }
    uint64_t sz = get_file_size(a.file_path);
    if (sz != a.file_size) {
      std::cerr << "File size mismatch. actual=" << sz << " expected=" << a.file_size << "\n";
      return 1;
    }
  }

  RunResult r = run_syscall(a);
  append_csv_row(a, r);

  std::cerr << "Done: workload=" << wl_name(a.workload)
            << " cache=" << a.cache_state
            << " file=" << a.file_size
            << " block=" << a.block_size
            << " threads=" << a.threads
            << " rep=" << a.repetition
            << " wall_s=" << r.wall_s << "\n";
  return 0;
}

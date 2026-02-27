#include <fcntl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <random>
#include <string>
#include <thread>
#include <vector>

enum class Workload { SeqRead, RandRead, MixedRand };

static inline uint64_t to_u64(const char* s) {
    return std::stoull(std::string(s));
}

static Workload parse_workload(const std::string& w) {
    if (w == "seq_read") return Workload::SeqRead;
    if (w == "rand_read") return Workload::RandRead;
    if (w == "mixed_rand") return Workload::MixedRand;
    std::cerr << "Unknown workload: " << w << "\n";
    std::exit(1);
}

struct RUsageDelta {
    double user_s{};
    double sys_s{};
    long minflt{};
    long majflt{};
    long nvcsw{};
    long nivcsw{};
};

static RUsageDelta delta_rusage(const rusage& a, const rusage& b) {
    auto tv_to_s = [](const timeval& tv) {
        return tv.tv_sec + tv.tv_usec / 1e6;
    };

    RUsageDelta d;
    d.user_s = tv_to_s(b.ru_utime) - tv_to_s(a.ru_utime);
    d.sys_s  = tv_to_s(b.ru_stime) - tv_to_s(a.ru_stime);
    d.minflt = b.ru_minflt - a.ru_minflt;
    d.majflt = b.ru_majflt - a.ru_majflt;
    d.nvcsw  = b.ru_nvcsw  - a.ru_nvcsw;
    d.nivcsw = b.ru_nivcsw - a.ru_nivcsw;
    return d;
}

static inline void consume_bytes(const uint8_t* p, size_t n, uint64_t& acc) {
    const size_t step = 64;
    for (size_t i = 0; i < n; i += step) acc += p[i];
    acc += p[n - 1];
}

struct RunConfig {
    std::string file_path;
    Workload workload;
    std::string cache_state;
    uint64_t file_size;
    uint64_t block_size;
    int threads;
    uint64_t seed;
    int repetition;
};

struct RunResult {
    double wall_s{};
    uint64_t total_bytes{};
    uint64_t num_ops{};
    double throughput_mb_s{};
    double latency_us_op{};
    RUsageDelta ru{};
};

static RunResult run_mmap(const RunConfig& cfg) {
    int fd = ::open(cfg.file_path.c_str(), O_RDWR);
    if (fd < 0) { perror("open"); std::exit(1); }

    struct stat st{};
    if (fstat(fd, &st) != 0) { perror("fstat"); std::exit(1); }

    uint64_t fsz = static_cast<uint64_t>(st.st_size);
    if (fsz < cfg.block_size) {
        std::cerr << "File too small\n";
        std::exit(1);
    }

    void* map = mmap(nullptr, fsz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) { perror("mmap"); std::exit(1); }

    const uint64_t num_blocks = fsz / cfg.block_size;
    const uint64_t num_ops = num_blocks;
    const uint64_t total_bytes = num_ops * cfg.block_size;

    std::atomic<uint64_t> sink{0};

    auto worker = [&](int tid) {
        uint64_t local_acc = 0;

        uint64_t start = (num_ops * tid) / cfg.threads;
        uint64_t end   = (num_ops * (tid + 1)) / cfg.threads;

        std::mt19937_64 rng(cfg.seed + static_cast<uint64_t>(tid));
        std::uniform_int_distribution<uint64_t> dist(0, num_blocks - 1);
        std::bernoulli_distribution is_read(0.7);

        auto* base = static_cast<uint8_t*>(map);

        for (uint64_t op = start; op < end; ++op) {
            uint64_t block_index;

            if (cfg.workload == Workload::SeqRead) {
                block_index = op;
            } else {
                block_index = dist(rng);
            }

            uint64_t off = block_index * cfg.block_size;
            uint8_t* ptr = base + off;

            if (cfg.workload == Workload::MixedRand) {
                if (is_read(rng)) {
                    consume_bytes(ptr, cfg.block_size, local_acc);
                } else {
                    std::memset(ptr, 0xAB, 64);
                }
            } else {
                consume_bytes(ptr, cfg.block_size, local_acc);
            }
        }

        sink.fetch_add(local_acc, std::memory_order_relaxed);
    };

    rusage ru_before{}, ru_after{};
    getrusage(RUSAGE_SELF, &ru_before);
    auto t0 = std::chrono::steady_clock::now();

    std::vector<std::thread> threads;
    for (int i = 0; i < cfg.threads; i++) {
        threads.emplace_back(worker, i);
    }
    for (auto& th : threads) th.join();

    if (cfg.workload == Workload::MixedRand) {
        if (msync(map, fsz, MS_SYNC) != 0) perror("msync");
    }

    auto t1 = std::chrono::steady_clock::now();
    getrusage(RUSAGE_SELF, &ru_after);

    munmap(map, fsz);
    close(fd);

    std::chrono::duration<double> dt = t1 - t0;

    RunResult r;
    r.wall_s = dt.count();
    r.total_bytes = total_bytes;
    r.num_ops = num_ops;
    r.throughput_mb_s = (static_cast<double>(total_bytes) / r.wall_s) / (1024.0 * 1024.0);
    r.latency_us_op = (r.wall_s * 1e6) / static_cast<double>(num_ops);
    r.ru = delta_rusage(ru_before, ru_after);

    if (sink.load(std::memory_order_relaxed) == 0xdeadbeef)
        std::cerr << "sink\n";

    return r;
}

static void write_csv_header(std::ostream& out) {
    out << "interface,workload,cache_state,file_size_bytes,block_size_bytes,threads,qd,seed,repetition,"
        << "wall_seconds,total_bytes,throughput_MBps,latency_us_per_op,"
        << "cpu_user_seconds,cpu_sys_seconds,Minor_faults,major_faults,voluntary_ctx,involuntary_ctx\n";
}

int main(int argc, char** argv) {
    if (argc < 9) {
        std::cerr << "Usage:\n"
                  << "mmap_bench <file_path> <workload> <cache_state>"
                  << " <block_size> <threads> <seed> <repetition> <csv_out>\n";
        return 1;
    }

    RunConfig cfg;
    cfg.file_path   = argv[1];
    cfg.workload    = parse_workload(argv[2]);
    cfg.cache_state = argv[3];
    cfg.block_size  = to_u64(argv[4]);
    cfg.threads     = std::stoi(argv[5]);
    cfg.seed        = to_u64(argv[6]);
    cfg.repetition  = std::stoi(argv[7]);
    std::string csv_out = argv[8];

    struct stat st{};
    if (stat(cfg.file_path.c_str(), &st) != 0) {
        perror("stat");
        return 1;
    }
    cfg.file_size = static_cast<uint64_t>(st.st_size);

    RunResult r = run_mmap(cfg);

    bool new_file = (access(csv_out.c_str(), F_OK) != 0);
    std::ofstream out(csv_out, std::ios::app);
    if (!out) {
        std::cerr << "Cannot open csv_out\n";
        return 1;
    }
    if (new_file) write_csv_header(out);

    out << "mmap" << ","
        << argv[2] << ","
        << cfg.cache_state << ","
        << cfg.file_size << ","
        << cfg.block_size << ","
        << cfg.threads << ","
        << 0 << ","
        << cfg.seed << ","
        << cfg.repetition << ","
        << r.wall_s << ","
        << r.total_bytes << ","
        << r.throughput_mb_s << ","
        << r.latency_us_op << ","
        << r.ru.user_s << ","
        << r.ru.sys_s << ","
        << r.ru.minflt << ","
        << r.ru.majflt << ","
        << r.ru.nvcsw << ","
        << r.ru.nivcsw
        << "\n";

    return 0;
}

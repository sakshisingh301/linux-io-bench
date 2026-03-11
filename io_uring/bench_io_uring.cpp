#include <liburing.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/resource.h>
#include <chrono>
#include <vector>
#include <thread>
#include <random>
#include <iostream>
#include <cstring>
#include <atomic>

constexpr size_t FILE_SIZE = 2ULL * 1024 * 1024 * 1024; // 2GB
constexpr int REPS = 3;
constexpr int SEED = 42;

struct Config {
    size_t block_size;
    int threads;
    int qd;
    bool sequential;
    bool write;
    bool cold_cache;
};

struct Metrics {
    double wall_sec;
    double throughput_MBps;
    double latency_us;
    double cpu_user;
    double cpu_sys;
    long minor_faults;
    long major_faults;
    long voluntary_ctx;
    long involuntary_ctx;
};

void drop_caches() {
    system("sync");
    system("sudo sh -c \"echo 3 > /proc/sys/vm/drop_caches\"");
    std::this_thread::sleep_for(std::chrono::seconds(1));
}

void prepare_file(const std::string& fname) {

    int fd = open(fname.c_str(), O_CREAT | O_RDWR, 0644);
    if (fd < 0) { perror("open"); exit(1); }

    if (ftruncate(fd, FILE_SIZE) < 0) {
        perror("ftruncate");
        exit(1);
    }

    std::vector<char> buf(4096, 1);
    size_t written = 0;

    while (written < FILE_SIZE) {
        ssize_t ret = write(fd, buf.data(), 4096);
        if (ret < 0) { perror("write"); exit(1); }
        written += ret;
    }

    fsync(fd);
    close(fd);
}

void worker(const std::string& fname,
            const Config& cfg,
            std::atomic<size_t>& total_ops)
{
    io_uring ring;

    if (io_uring_queue_init(cfg.qd, &ring, 0) < 0) {
        perror("io_uring_queue_init");
        exit(1);
    }

    int flags = cfg.write ? O_RDWR : O_RDONLY;
    int fd = open(fname.c_str(), flags);
    if (fd < 0) { perror("open"); exit(1); }

    std::vector<void*> buffers(cfg.qd);

    for (int i = 0; i < cfg.qd; i++) {
        if (posix_memalign(&buffers[i], 4096, cfg.block_size) != 0) {
            perror("posix_memalign");
            exit(1);
        }
        memset(buffers[i], 0, cfg.block_size);
    }

    std::mt19937 rng(SEED);

    size_t total_blocks = FILE_SIZE / cfg.block_size;
    size_t submitted = 0;
    size_t completed = 0;

    while (completed < total_blocks) {

        while (submitted - completed < (size_t)cfg.qd &&
               submitted < total_blocks)
        {
            io_uring_sqe* sqe = io_uring_get_sqe(&ring);
            if (!sqe) break;

            size_t block_index;

            if (cfg.sequential)
                block_index = submitted;
            else
                block_index = rng() % total_blocks;

            off_t offset = block_index * cfg.block_size;
            void* buf = buffers[submitted % cfg.qd];

            if (cfg.write)
                io_uring_prep_write(sqe, fd, buf,
                                    cfg.block_size, offset);
            else
                io_uring_prep_read(sqe, fd, buf,
                                   cfg.block_size, offset);

            submitted++;
        }

        io_uring_submit(&ring);

        io_uring_cqe* cqe;
        unsigned head;
        unsigned count = 0;

        io_uring_for_each_cqe(&ring, head, cqe) {

            if (cqe->res < 0) {
                std::cerr << "IO error: " << cqe->res << std::endl;
            }

            completed++;
            count++;
        }

        io_uring_cq_advance(&ring, count);
    }

    total_ops += completed;

    for (auto b : buffers)
        free(b);

    close(fd);
    io_uring_queue_exit(&ring);
}

Metrics run(const std::string& fname,
            const Config& cfg)
{
    if (cfg.cold_cache)
        drop_caches();

    struct rusage before, after;
    getrusage(RUSAGE_SELF, &before);

    auto start = std::chrono::steady_clock::now();

    std::atomic<size_t> total_ops{0};
    std::vector<std::thread> workers;

    for (int i = 0; i < cfg.threads; i++) {
        workers.emplace_back(worker,
                             fname,
                             std::ref(cfg),
                             std::ref(total_ops));
    }

    for (auto& t : workers)
        t.join();

    auto end = std::chrono::steady_clock::now();

    getrusage(RUSAGE_SELF, &after);

    double sec =
        std::chrono::duration<double>(end - start).count();

    double total_bytes =
        total_ops * cfg.block_size;

    Metrics m;

    m.wall_sec = sec;

    m.throughput_MBps =
        (total_bytes / sec) / (1024.0 * 1024.0);

    m.latency_us =
        (sec * 1e6) / total_ops;

    m.cpu_user =
        (after.ru_utime.tv_sec - before.ru_utime.tv_sec) +
        (after.ru_utime.tv_usec - before.ru_utime.tv_usec)/1e6;

    m.cpu_sys =
        (after.ru_stime.tv_sec - before.ru_stime.tv_sec) +
        (after.ru_stime.tv_usec - before.ru_stime.tv_usec)/1e6;

    m.minor_faults = after.ru_minflt - before.ru_minflt;
    m.major_faults = after.ru_majflt - before.ru_majflt;

    m.voluntary_ctx = after.ru_nvcsw - before.ru_nvcsw;
    m.involuntary_ctx = after.ru_nivcsw - before.ru_nivcsw;

    return m;
}

int main() {

    std::string fname = "benchmark.bin";

    prepare_file(fname);

    std::vector<size_t> blocks = {4096, 64*1024, 1024*1024};
    std::vector<bool> seq_opts = {true, false};
    std::vector<bool> write_opts = {false, true};
    std::vector<bool> cache_opts = {false, true};

    std::vector<int> thread_opts = {1,4,8};

    std::cout <<
    "interface,workload,cache_state,"
    "file_size_bytes,block_size_bytes,"
    "threads,qd,seed,repetition,"
    "wall_seconds,total_bytes,"
    "throughput_MBps,latency_us_per_op,"
    "cpu_user_seconds,cpu_sys_seconds,"
    "minor_faults,major_faults,"
    "voluntary_ctx,involuntary_ctx\n";

    for (auto bs : blocks) {
        for (auto seq : seq_opts) {
            for (auto wr : write_opts) {

                std::string workload;

                if (seq && !wr) workload = "seq_read";
                if (!seq && !wr) workload = "rand_read";
                if (seq && wr) workload = "seq_write";
                if (!seq && wr) workload = "rand_write";

                for (auto cold : cache_opts) {
                    for (auto th : thread_opts) {

                        for (int rep = 1; rep <= REPS; rep++) {

                            Config cfg;

                            cfg.block_size = bs;
                            cfg.threads = th;
                            cfg.qd = 64;
                            cfg.sequential = seq;
                            cfg.write = wr;
                            cfg.cold_cache = cold;

                            std::cout << "Running "
                                      << workload
                                      << " block=" << bs
                                      << " threads=" << th
                                      << " cache="
                                      << (cold?"cold":"warm")
                                      << std::endl;

                            Metrics m = run(fname, cfg);

                            std::cout
                            << "io_uring," << workload << ","
                            << (cold?"cold":"warm") << ","
                            << FILE_SIZE << ","
                            << bs << ","
                            << th << ","
                            << cfg.qd << ","
                            << SEED << ","
                            << rep << ","
                            << m.wall_sec << ","
                            << FILE_SIZE << ","
                            << m.throughput_MBps << ","
                            << m.latency_us << ","
                            << m.cpu_user << ","
                            << m.cpu_sys << ","
                            << m.minor_faults << ","
                            << m.major_faults << ","
                            << m.voluntary_ctx << ","
                            << m.involuntary_ctx
                            << "\n";
                        }
                    }
                }
            }
        }
    }

    return 0;
}
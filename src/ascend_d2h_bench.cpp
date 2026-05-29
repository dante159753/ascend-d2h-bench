#include <acl/acl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

constexpr uint32_t kStreamFlags = ACL_STREAM_FAST_LAUNCH | ACL_STREAM_FAST_SYNC;
constexpr size_t kHugePageSize = 2UL << 20;
constexpr size_t kGiganticPageSize = 1UL << 30;
constexpr int kHugePageFlag = 21 << MAP_HUGE_SHIFT;
constexpr int kGiganticPageFlag = 30 << MAP_HUGE_SHIFT;

struct Options {
    int device = 0;
    size_t ioSize = 128 * 1024;
    size_t ioCount = 64;
    size_t streams = 1;
    size_t warmup = 5;
    size_t iters = 50;
    size_t batchSize = 4096;
    std::string mode = "all";
    std::string allocator = "all";
    bool verify = false;
    bool csv = false;
};

struct IterTiming {
    double totalUs = 0.0;
    double submitUs = 0.0;
    double syncUs = 0.0;
};

struct Stats {
    double avg = 0.0;
    double min = 0.0;
    double p50 = 0.0;
    double p90 = 0.0;
    double p99 = 0.0;
    double max = 0.0;
};

[[noreturn]] void Die(const std::string& msg)
{
    throw std::runtime_error(msg);
}

void CheckAcl(aclError ret, const std::string& what)
{
    if (ret != ACL_SUCCESS) {
        std::ostringstream os;
        os << what << " failed, ret=" << ret;
        const char* recent = aclGetRecentErrMsg();
        if (recent != nullptr) { os << ", msg=" << recent; }
        Die(os.str());
    }
}

size_t AlignUp(size_t value, size_t alignment)
{
    return ((value + alignment - 1) / alignment) * alignment;
}

std::string Lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

size_t ParseSize(std::string value)
{
    value = Lower(value);
    size_t pos = 0;
    double number = std::stod(value, &pos);
    std::string suffix = value.substr(pos);
    suffix.erase(std::remove_if(suffix.begin(), suffix.end(), ::isspace), suffix.end());

    double multiplier = 1.0;
    if (suffix.empty() || suffix == "b") {
        multiplier = 1.0;
    } else if (suffix == "k" || suffix == "kb" || suffix == "kib") {
        multiplier = 1024.0;
    } else if (suffix == "m" || suffix == "mb" || suffix == "mib") {
        multiplier = 1024.0 * 1024.0;
    } else if (suffix == "g" || suffix == "gb" || suffix == "gib") {
        multiplier = 1024.0 * 1024.0 * 1024.0;
    } else {
        Die("invalid size suffix: " + suffix);
    }
    if (number <= 0) { Die("size must be positive"); }
    return static_cast<size_t>(number * multiplier);
}

size_t ParseSizeT(const std::string& value, const std::string& name)
{
    size_t pos = 0;
    unsigned long long parsed = std::stoull(value, &pos, 10);
    if (pos != value.size()) { Die("invalid integer for " + name + ": " + value); }
    if (parsed == 0) { Die(name + " must be positive"); }
    return static_cast<size_t>(parsed);
}

size_t ParseSizeTAllowZero(const std::string& value, const std::string& name)
{
    size_t pos = 0;
    unsigned long long parsed = std::stoull(value, &pos, 10);
    if (pos != value.size()) { Die("invalid integer for " + name + ": " + value); }
    return static_cast<size_t>(parsed);
}

void PrintHelp(const char* argv0)
{
    std::cout
        << "Usage: " << argv0 << " [options]\n\n"
        << "Options:\n"
        << "  --device N                  Ascend logical device id, default 0\n"
        << "  --mode async-loop|batch|sync|all\n"
        << "  --allocator aclrt-malloc-host|ucm-direct|register-pinned|all\n"
        << "  --io-size BYTES             Supports suffixes k/m/g, default 128k\n"
        << "  --io-count N                Number of D2H slices per iteration, default 64\n"
        << "  --streams N                 Streams for async-loop, default 1\n"
        << "  --batch-size N              Max slices per aclrtMemcpyBatch call, default 4096\n"
        << "  --warmup N                  Warmup iterations, default 5\n"
        << "  --iters N                   Measured iterations, default 50\n"
        << "  --verify                    Check copied bytes after each benchmark case\n"
        << "  --csv                       Print machine-readable CSV rows\n"
        << "  --help\n";
}

Options ParseArgs(int argc, char** argv)
{
    Options opt;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto needValue = [&](const std::string& name) -> std::string {
            if (i + 1 >= argc) { Die("missing value for " + name); }
            return argv[++i];
        };
        if (arg == "--help" || arg == "-h") {
            PrintHelp(argv[0]);
            std::exit(0);
        } else if (arg == "--device") {
            opt.device = static_cast<int>(ParseSizeTAllowZero(needValue(arg), arg));
        } else if (arg == "--mode") {
            opt.mode = Lower(needValue(arg));
        } else if (arg == "--allocator") {
            opt.allocator = Lower(needValue(arg));
        } else if (arg == "--io-size") {
            opt.ioSize = ParseSize(needValue(arg));
        } else if (arg == "--io-count") {
            opt.ioCount = ParseSizeT(needValue(arg), arg);
        } else if (arg == "--streams") {
            opt.streams = ParseSizeT(needValue(arg), arg);
        } else if (arg == "--batch-size") {
            opt.batchSize = ParseSizeT(needValue(arg), arg);
        } else if (arg == "--warmup") {
            opt.warmup = ParseSizeTAllowZero(needValue(arg), arg);
        } else if (arg == "--iters") {
            opt.iters = ParseSizeT(needValue(arg), arg);
        } else if (arg == "--verify") {
            opt.verify = true;
        } else if (arg == "--csv") {
            opt.csv = true;
        } else {
            Die("unknown option: " + arg);
        }
    }
    return opt;
}

std::vector<std::string> ExpandChoice(const std::string& value,
                                      const std::vector<std::string>& allowed,
                                      const std::string& name)
{
    if (value == "all") { return allowed; }
    if (std::find(allowed.begin(), allowed.end(), value) == allowed.end()) {
        Die("invalid " + name + ": " + value);
    }
    return {value};
}

class AscendRuntime {
public:
    explicit AscendRuntime(int device) : device_(device)
    {
        CheckAcl(aclInit(nullptr), "aclInit");
        initialized_ = true;
        CheckAcl(aclrtSetDevice(device_), "aclrtSetDevice");
        deviceSet_ = true;
    }
    ~AscendRuntime()
    {
        if (deviceSet_) { (void)aclrtResetDevice(device_); }
        if (initialized_) { (void)aclFinalize(); }
    }

private:
    int device_;
    bool initialized_ = false;
    bool deviceSet_ = false;
};

class StreamSet {
public:
    explicit StreamSet(size_t n)
    {
        streams_.resize(n, nullptr);
        for (auto& stream : streams_) {
            CheckAcl(aclrtCreateStreamWithConfig(&stream, 0, kStreamFlags),
                     "aclrtCreateStreamWithConfig");
        }
    }
    ~StreamSet()
    {
        for (auto stream : streams_) {
            if (stream != nullptr) { (void)aclrtDestroyStream(stream); }
        }
    }
    aclrtStream At(size_t i) const { return streams_[i % streams_.size()]; }
    void SynchronizeAll() const
    {
        for (auto stream : streams_) { CheckAcl(aclrtSynchronizeStream(stream), "aclrtSynchronizeStream"); }
    }

private:
    std::vector<aclrtStream> streams_;
};

class DeviceBuffer {
public:
    explicit DeviceBuffer(size_t size) : size_(size)
    {
        CheckAcl(aclrtMalloc(&ptr_, size_, ACL_MEM_TYPE_HIGH_BAND_WIDTH), "aclrtMalloc");
        CheckAcl(aclrtMemset(ptr_, size_, 0xA5, size_), "aclrtMemset");
    }
    ~DeviceBuffer()
    {
        if (ptr_ != nullptr) { (void)aclrtFree(ptr_); }
    }
    void* Data() const { return ptr_; }
    size_t Size() const { return size_; }

private:
    void* ptr_ = nullptr;
    size_t size_ = 0;
};

class HostBuffer {
public:
    static HostBuffer Allocate(const std::string& allocator, size_t size)
    {
        HostBuffer buffer;
        buffer.allocator_ = allocator;
        buffer.requestedSize_ = size;
        if (allocator == "aclrt-malloc-host") {
            CheckAcl(aclrtMallocHost(&buffer.ptr_, size), "aclrtMallocHost");
            buffer.releaseKind_ = ReleaseKind::AclrtMallocHost;
            return buffer;
        }
        if (allocator == "ucm-direct") {
            buffer.AllocateMmap(size);
            buffer.RegisterMapped();
            return buffer;
        }
        if (allocator == "register-pinned") {
            buffer.AllocateMmap(size);
            buffer.RegisterPinned();
            return buffer;
        }
        Die("unknown allocator: " + allocator);
    }

    HostBuffer() = default;
    HostBuffer(const HostBuffer&) = delete;
    HostBuffer& operator=(const HostBuffer&) = delete;
    HostBuffer(HostBuffer&& other) noexcept { MoveFrom(other); }
    HostBuffer& operator=(HostBuffer&& other) noexcept
    {
        if (this != &other) {
            Reset();
            MoveFrom(other);
        }
        return *this;
    }
    ~HostBuffer() { Reset(); }

    void* Data() const { return ptr_; }
    size_t RequestedSize() const { return requestedSize_; }
    size_t MappedSize() const { return mappedSize_; }
    bool MlockOk() const { return mlockOk_; }
    const std::string& Allocator() const { return allocator_; }

private:
    enum class ReleaseKind { None, AclrtMallocHost, MmapRegistered };

    void MoveFrom(HostBuffer& other)
    {
        ptr_ = other.ptr_;
        requestedSize_ = other.requestedSize_;
        mappedSize_ = other.mappedSize_;
        releaseKind_ = other.releaseKind_;
        registered_ = other.registered_;
        mlockOk_ = other.mlockOk_;
        allocator_ = std::move(other.allocator_);

        other.ptr_ = nullptr;
        other.requestedSize_ = 0;
        other.mappedSize_ = 0;
        other.releaseKind_ = ReleaseKind::None;
        other.registered_ = false;
        other.mlockOk_ = false;
    }

    void Reset()
    {
        if (ptr_ == nullptr) { return; }
        if (releaseKind_ == ReleaseKind::AclrtMallocHost) {
            (void)aclrtFreeHost(ptr_);
        } else if (releaseKind_ == ReleaseKind::MmapRegistered) {
            if (registered_) { (void)aclrtHostUnregister(ptr_); }
            if (mlockOk_) { (void)munlock(ptr_, mappedSize_); }
            (void)munmap(ptr_, mappedSize_);
        }
        ptr_ = nullptr;
    }

    static void* MmapWithHugePage(size_t& size, bool useGigantic)
    {
        const size_t pageSize = useGigantic ? kGiganticPageSize : kHugePageSize;
        const int pageFlag = useGigantic ? kGiganticPageFlag : kHugePageFlag;
        size = AlignUp(size, pageSize);
        return mmap(nullptr, size, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB | pageFlag, -1, 0);
    }

    static void* MmapWithAdvice(size_t& size)
    {
        size = AlignUp(size, kHugePageSize);
        void* ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS,
                         -1, 0);
        if (ptr != MAP_FAILED) { (void)madvise(ptr, size, MADV_HUGEPAGE); }
        return ptr;
    }

    void AllocateMmap(size_t size)
    {
        mappedSize_ = size;
        const bool useGigantic = mappedSize_ >= kGiganticPageSize;
        ptr_ = MmapWithHugePage(mappedSize_, useGigantic);
        if (ptr_ == MAP_FAILED && useGigantic) {
            mappedSize_ = size;
            ptr_ = MmapWithHugePage(mappedSize_, false);
        }
        if (ptr_ == MAP_FAILED) {
            mappedSize_ = size;
            ptr_ = MmapWithAdvice(mappedSize_);
        }
        if (ptr_ == MAP_FAILED) { Die("mmap failed"); }

        std::memset(ptr_, 0, mappedSize_);
        mlockOk_ = (mlock(ptr_, mappedSize_) == 0);
        releaseKind_ = ReleaseKind::MmapRegistered;
    }

    void RegisterMapped()
    {
        void* deviceAlias = nullptr;
        auto ret = aclrtHostRegister(ptr_, mappedSize_, ACL_HOST_REGISTER_MAPPED, &deviceAlias);
        CheckAcl(ret, "aclrtHostRegister(MAPPED)");
        registered_ = true;
    }

    void RegisterPinned()
    {
#if HAVE_ACLRT_HOST_REGISTER_V2
        auto ret = aclrtHostRegisterV2(ptr_, mappedSize_, ACL_HOST_REG_MAPPED | ACL_HOST_REG_PINNED);
        CheckAcl(ret, "aclrtHostRegisterV2(MAPPED|PINNED)");
        registered_ = true;
#else
        Die("register-pinned requires aclrtHostRegisterV2; rebuild with newer CANN headers");
#endif
    }

    void* ptr_ = nullptr;
    size_t requestedSize_ = 0;
    size_t mappedSize_ = 0;
    ReleaseKind releaseKind_ = ReleaseKind::None;
    bool registered_ = false;
    bool mlockOk_ = false;
    std::string allocator_;
};

double ElapsedUs(Clock::time_point begin, Clock::time_point end)
{
    return std::chrono::duration<double, std::micro>(end - begin).count();
}

char* AddBytes(void* ptr, size_t offset)
{
    return static_cast<char*>(ptr) + offset;
}

const char* AddBytesConst(const void* ptr, size_t offset)
{
    return static_cast<const char*>(ptr) + offset;
}

IterTiming RunAsyncLoop(const Options& opt, DeviceBuffer& device, HostBuffer& host, StreamSet& streams)
{
    auto begin = Clock::now();
    for (size_t i = 0; i < opt.ioCount; ++i) {
        const size_t offset = i * opt.ioSize;
        CheckAcl(aclrtMemcpyAsync(AddBytes(host.Data(), offset), opt.ioSize,
                                  AddBytes(device.Data(), offset), opt.ioSize,
                                  ACL_MEMCPY_DEVICE_TO_HOST, streams.At(i)),
                 "aclrtMemcpyAsync(D2H)");
    }
    auto submitted = Clock::now();
    streams.SynchronizeAll();
    auto end = Clock::now();
    return {ElapsedUs(begin, end), ElapsedUs(begin, submitted), ElapsedUs(submitted, end)};
}

IterTiming RunSync(const Options& opt, DeviceBuffer& device, HostBuffer& host)
{
    auto begin = Clock::now();
    for (size_t i = 0; i < opt.ioCount; ++i) {
        const size_t offset = i * opt.ioSize;
        CheckAcl(aclrtMemcpy(AddBytes(host.Data(), offset), opt.ioSize,
                             AddBytes(device.Data(), offset), opt.ioSize,
                             ACL_MEMCPY_DEVICE_TO_HOST),
                 "aclrtMemcpy(D2H)");
    }
    auto end = Clock::now();
    const double total = ElapsedUs(begin, end);
    return {total, total, 0.0};
}

IterTiming RunBatch(const Options& opt, DeviceBuffer& device, HostBuffer& host)
{
#if HAVE_ACLRT_MEMCPY_BATCH
    auto begin = Clock::now();
    for (size_t start = 0; start < opt.ioCount; start += opt.batchSize) {
        const size_t n = std::min(opt.batchSize, opt.ioCount - start);
        std::vector<void*> dst(n);
        std::vector<void*> src(n);
        std::vector<size_t> sizes(n, opt.ioSize);
        std::vector<aclrtMemcpyBatchAttr> attrs(n);
        std::vector<size_t> attrIds(n);
        int32_t deviceId = 0;
        CheckAcl(aclrtGetDevice(&deviceId), "aclrtGetDevice");
        aclrtMemLocation hostLoc{0, ACL_MEM_LOCATION_TYPE_HOST};
        aclrtMemLocation deviceLoc{static_cast<uint32_t>(deviceId), ACL_MEM_LOCATION_TYPE_DEVICE};
        for (size_t i = 0; i < n; ++i) {
            const size_t offset = (start + i) * opt.ioSize;
            dst[i] = AddBytes(host.Data(), offset);
            src[i] = AddBytes(device.Data(), offset);
            attrs[i] = aclrtMemcpyBatchAttr{hostLoc, deviceLoc, {}};
            attrIds[i] = i;
        }
        size_t failIdx = std::numeric_limits<size_t>::max();
        auto ret = aclrtMemcpyBatch(dst.data(), sizes.data(), src.data(), sizes.data(), n,
                                    attrs.data(), attrIds.data(), attrs.size(), &failIdx);
        if (ret != ACL_SUCCESS) {
            std::ostringstream os;
            os << "aclrtMemcpyBatch(D2H) failed, ret=" << ret << ", failIdx=" << failIdx;
            Die(os.str());
        }
    }
    auto end = Clock::now();
    const double total = ElapsedUs(begin, end);
    return {total, total, 0.0};
#else
    (void)opt;
    (void)device;
    (void)host;
    Die("batch mode requires aclrtMemcpyBatch; rebuild with newer CANN headers");
#endif
}

Stats Summarize(std::vector<double> values)
{
    if (values.empty()) { return {}; }
    std::sort(values.begin(), values.end());
    auto pct = [&](double p) {
        const double idx = p * static_cast<double>(values.size() - 1);
        const size_t lo = static_cast<size_t>(std::floor(idx));
        const size_t hi = static_cast<size_t>(std::ceil(idx));
        if (lo == hi) { return values[lo]; }
        return values[lo] + (values[hi] - values[lo]) * (idx - static_cast<double>(lo));
    };
    const double sum = std::accumulate(values.begin(), values.end(), 0.0);
    return {sum / values.size(), values.front(), pct(0.50), pct(0.90), pct(0.99), values.back()};
}

void VerifyCopied(const DeviceBuffer&, const HostBuffer& host)
{
    const auto* data = static_cast<const unsigned char*>(host.Data());
    const size_t size = host.RequestedSize();
    if (size == 0) { return; }
    const size_t step = std::max<size_t>(1, size / 4096);
    for (size_t i = 0; i < size; i += step) {
        if (data[i] != 0xA5) {
            std::ostringstream os;
            os << "verification failed at byte " << i << ", got 0x"
               << std::hex << static_cast<int>(data[i]);
            Die(os.str());
        }
    }
    if (data[size - 1] != 0xA5) { Die("verification failed at final byte"); }
}

std::vector<IterTiming> RunCase(const Options& opt, const std::string& mode,
                                const std::string& allocator)
{
    const size_t totalBytes = opt.ioSize * opt.ioCount;
    if (totalBytes / opt.ioSize != opt.ioCount) { Die("total byte size overflow"); }

    DeviceBuffer device(totalBytes);
    HostBuffer host = HostBuffer::Allocate(allocator, totalBytes);
    StreamSet streams(std::max<size_t>(1, opt.streams));

    auto runOnce = [&]() {
        if (mode == "async-loop") { return RunAsyncLoop(opt, device, host, streams); }
        if (mode == "sync") { return RunSync(opt, device, host); }
        if (mode == "batch") { return RunBatch(opt, device, host); }
        Die("invalid mode: " + mode);
    };

    for (size_t i = 0; i < opt.warmup; ++i) { (void)runOnce(); }
    if (opt.verify) { VerifyCopied(device, host); }

    std::vector<IterTiming> timings;
    timings.reserve(opt.iters);
    for (size_t i = 0; i < opt.iters; ++i) { timings.push_back(runOnce()); }
    if (opt.verify) { VerifyCopied(device, host); }

    if (!host.MlockOk() && allocator != "aclrt-malloc-host") {
        std::cerr << "warning: mlock failed for allocator=" << allocator
                  << "; benchmark still ran with acl host registration\n";
    }
    return timings;
}

void PrintStats(const Options& opt, const std::string& mode, const std::string& allocator,
                const std::vector<IterTiming>& timings)
{
    std::vector<double> total;
    std::vector<double> submit;
    std::vector<double> sync;
    total.reserve(timings.size());
    submit.reserve(timings.size());
    sync.reserve(timings.size());
    for (const auto& t : timings) {
        total.push_back(t.totalUs);
        submit.push_back(t.submitUs);
        sync.push_back(t.syncUs);
    }
    const auto totalStats = Summarize(total);
    const auto submitStats = Summarize(submit);
    const auto syncStats = Summarize(sync);
    const double bytes = static_cast<double>(opt.ioSize) * static_cast<double>(opt.ioCount);
    const double gbps = bytes / (totalStats.avg / 1e6) / 1e9;

    if (opt.csv) {
        std::cout << mode << ',' << allocator << ',' << opt.ioSize << ',' << opt.ioCount << ','
                  << opt.streams << ',' << totalStats.avg << ',' << totalStats.p50 << ','
                  << totalStats.p90 << ',' << totalStats.p99 << ',' << gbps << ','
                  << submitStats.avg << ',' << syncStats.avg << '\n';
        return;
    }

    std::cout << "\nmode=" << mode << " allocator=" << allocator
              << " io_size=" << opt.ioSize << " io_count=" << opt.ioCount
              << " streams=" << opt.streams << '\n';
    std::cout << std::fixed << std::setprecision(3)
              << "  total_us avg=" << totalStats.avg << " min=" << totalStats.min
              << " p50=" << totalStats.p50 << " p90=" << totalStats.p90
              << " p99=" << totalStats.p99 << " max=" << totalStats.max << '\n';
    std::cout << "  submit_us avg=" << submitStats.avg
              << " sync_wait_us avg=" << syncStats.avg << '\n';
    std::cout << "  bandwidth_avg_gbps=" << gbps << '\n';
}

}  // namespace

int main(int argc, char** argv)
{
    try {
        auto opt = ParseArgs(argc, argv);
        const auto modes = ExpandChoice(opt.mode, {"async-loop", "batch", "sync"}, "mode");
        const auto allocators = ExpandChoice(
            opt.allocator, {"aclrt-malloc-host", "ucm-direct", "register-pinned"}, "allocator");

        if (opt.csv) {
            std::cout << "mode,allocator,io_size,io_count,streams,total_avg_us,total_p50_us,"
                         "total_p90_us,total_p99_us,bandwidth_avg_gbps,submit_avg_us,"
                         "sync_wait_avg_us\n";
        }

        AscendRuntime runtime(opt.device);
        for (const auto& allocator : allocators) {
            for (const auto& mode : modes) {
                auto timings = RunCase(opt, mode, allocator);
                PrintStats(opt, mode, allocator, timings);
            }
        }
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << '\n';
        return 1;
    }
}

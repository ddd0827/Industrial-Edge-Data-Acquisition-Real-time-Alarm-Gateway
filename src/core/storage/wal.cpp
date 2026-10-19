#include "core/storage/wal.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "common/log.h"
#include "common/util.h"

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace eg {
namespace {

constexpr std::array<std::uint8_t, 8> kMagic = {'E', 'D', 'G', 'E',
                                                 'W', 'A', 'L', '1'};
constexpr std::uint32_t kVersion = 1;
constexpr std::size_t kFileHeaderSize = 12;
constexpr std::size_t kRecordHeaderSize = 5;
constexpr std::size_t kCrcSize = 4;
constexpr std::size_t kMaxPayload = 1024 * 1024;
constexpr std::uint8_t kSchemaRecord = 1;
constexpr std::uint8_t kSampleRecord = 2;

void append_le(std::vector<std::uint8_t>& out, std::uint64_t value,
               std::size_t bytes) {
    for (std::size_t i = 0; i < bytes; ++i) {
        out.push_back(static_cast<std::uint8_t>((value >> (i * 8U)) & 0xFFU));
    }
}

std::uint16_t read_le16(const std::uint8_t* data) {
    return static_cast<std::uint16_t>(data[0]) |
           static_cast<std::uint16_t>(static_cast<std::uint16_t>(data[1]) << 8U);
}

std::uint32_t read_le32(const std::uint8_t* data) {
    return static_cast<std::uint32_t>(data[0]) |
           (static_cast<std::uint32_t>(data[1]) << 8U) |
           (static_cast<std::uint32_t>(data[2]) << 16U) |
           (static_cast<std::uint32_t>(data[3]) << 24U);
}

std::uint64_t read_le64(const std::uint8_t* data) {
    std::uint64_t value = 0;
    for (int i = 7; i >= 0; --i) {
        value = (value << 8U) | data[i];
    }
    return value;
}

std::uint32_t record_crc(const std::uint8_t* header, const std::uint8_t* payload,
                         std::size_t payload_size) {
    return crc32(payload, payload_size,
                 crc32(header, kRecordHeaderSize));
}

std::FILE* open_append(const std::string& path) {
#ifdef _WIN32
    return _wfopen(std::filesystem::u8path(path).wstring().c_str(), L"ab");
#else
    return std::fopen(path.c_str(), "ab");
#endif
}

bool flush_to_disk(std::FILE* file, std::string& error) {
    if (std::fflush(file) != 0) {
        error = "fflush failed";
        return false;
    }
#ifdef _WIN32
    if (_commit(_fileno(file)) != 0) {
        error = "_commit failed";
        return false;
    }
#else
    if (fsync(fileno(file)) != 0) {
        error = "fsync failed";
        return false;
    }
#endif
    return true;
}

bool write_bytes(std::FILE* file, const std::uint8_t* data, std::size_t size) {
    return std::fwrite(data, 1, size, file) == size;
}

}  // namespace

WalRecoveryResult recover_wal(const std::string& path,
                              const WalSampleCallback& callback) {
    WalRecoveryResult result;
    std::error_code ec;
    const auto file_path = std::filesystem::u8path(path);
    if (!std::filesystem::exists(file_path, ec)) {
        return result;
    }
    const std::uintmax_t file_size = std::filesystem::file_size(file_path, ec);
    if (ec) {
        throw WalError("cannot stat WAL file " + path + ": " + ec.message());
    }
    if (file_size == 0) {
        return result;
    }
    if (file_size < kFileHeaderSize) {
        std::filesystem::resize_file(file_path, 0, ec);
        if (ec) {
            throw WalError("cannot truncate short WAL file: " + ec.message());
        }
        result.truncated = true;
        return result;
    }

    std::ifstream input(file_path, std::ios::binary);
    if (!input) {
        throw WalError("cannot open WAL file for reading: " + path);
    }

    std::array<std::uint8_t, kFileHeaderSize> file_header{};
    input.read(reinterpret_cast<char*>(file_header.data()),
               static_cast<std::streamsize>(file_header.size()));
    if (input.gcount() != static_cast<std::streamsize>(file_header.size())) {
        throw WalError("short WAL file header");
    }
    if (!std::equal(kMagic.begin(), kMagic.end(), file_header.begin())) {
        throw WalError("invalid WAL magic: " + path);
    }
    const std::uint32_t version = read_le32(file_header.data() + 8);
    if (version != kVersion) {
        throw WalError("unsupported WAL version " + std::to_string(version));
    }

    std::unordered_map<std::uint32_t, std::string> metric_names;
    std::uint64_t offset = kFileHeaderSize;
    std::uint64_t last_good = kFileHeaderSize;

    while (true) {
        std::array<std::uint8_t, kRecordHeaderSize> header{};
        input.read(reinterpret_cast<char*>(header.data()),
                   static_cast<std::streamsize>(header.size()));
        if (input.gcount() == 0) {
            break;
        }
        if (input.gcount() != static_cast<std::streamsize>(header.size())) {
            result.truncated = true;
            break;
        }

        const std::uint8_t type = header[0];
        const std::uint32_t length = read_le32(header.data() + 1);
        if ((type != kSchemaRecord && type != kSampleRecord) ||
            length > kMaxPayload) {
            result.truncated = true;
            break;
        }

        std::vector<std::uint8_t> payload(length);
        if (length > 0) {
            input.read(reinterpret_cast<char*>(payload.data()),
                       static_cast<std::streamsize>(length));
            if (input.gcount() != static_cast<std::streamsize>(length)) {
                result.truncated = true;
                break;
            }
        }

        std::array<std::uint8_t, kCrcSize> crc_bytes{};
        input.read(reinterpret_cast<char*>(crc_bytes.data()),
                   static_cast<std::streamsize>(crc_bytes.size()));
        if (input.gcount() != static_cast<std::streamsize>(crc_bytes.size())) {
            result.truncated = true;
            break;
        }
        const std::uint32_t expected_crc = read_le32(crc_bytes.data());
        const std::uint32_t actual_crc =
            record_crc(header.data(), payload.data(), payload.size());
        if (expected_crc != actual_crc) {
            result.truncated = true;
            break;
        }

        bool valid_record = true;
        if (type == kSchemaRecord) {
            if (length < 6) {
                valid_record = false;
            } else {
                const std::uint32_t metric_id = read_le32(payload.data());
                const std::uint16_t name_length = read_le16(payload.data() + 4);
                if (static_cast<std::size_t>(6) + name_length != length) {
                    valid_record = false;
                } else {
                    metric_names[metric_id] = std::string(
                        reinterpret_cast<const char*>(payload.data() + 6),
                        name_length);
                }
            }
        } else {
            if (length != 20) {
                valid_record = false;
            } else {
                const std::uint32_t metric_id = read_le32(payload.data());
                const std::uint64_t timestamp_bits = read_le64(payload.data() + 4);
                const std::uint64_t value_bits = read_le64(payload.data() + 12);
                TimestampMs timestamp = 0;
                double value = 0.0;
                static_assert(sizeof(timestamp) == sizeof(timestamp_bits),
                              "timestamp size mismatch");
                static_assert(sizeof(value) == sizeof(value_bits),
                              "double size mismatch");
                std::memcpy(&timestamp, &timestamp_bits, sizeof(timestamp));
                std::memcpy(&value, &value_bits, sizeof(value));

                const auto name_it = metric_names.find(metric_id);
                if (name_it == metric_names.end()) {
                    ++result.unknown_metrics;
                } else {
                    callback(name_it->second, timestamp, value);
                }
                ++result.samples;
            }
        }

        if (!valid_record) {
            result.truncated = true;
            break;
        }

        ++result.records;
        offset += kRecordHeaderSize + length + kCrcSize;
        last_good = offset;
    }

    input.close();

    if (last_good < file_size) {
        result.truncated = true;
        std::filesystem::resize_file(file_path, last_good, ec);
        if (ec) {
            throw WalError("cannot truncate torn WAL tail: " + ec.message());
        }
    }
    result.bytes_recovered = last_good;
    return result;
}

struct WalWriter::Impl {
    Impl(std::string path_in, const MetricRegistry& registry_in,
         std::size_t queue_capacity_in, std::uint32_t flush_interval_in)
        : path(std::move(path_in)),
          registry(registry_in),
          queue_capacity(std::max<std::size_t>(1, queue_capacity_in)),
          flush_interval_ms(std::max<std::uint32_t>(1, flush_interval_in)) {}

    std::string path;
    const MetricRegistry& registry;
    std::size_t queue_capacity;
    std::uint32_t flush_interval_ms;

    mutable std::mutex mutex;
    mutable std::mutex error_mutex;
    std::condition_variable cv;
    std::deque<Sample> queue;
    bool stop_requested = false;
    bool running = false;
    bool failed = false;
    std::string error;
    std::thread thread;
    std::FILE* file = nullptr;
    std::unordered_set<std::uint32_t> schema_written;
    std::atomic<std::uint64_t> written{0};
    std::atomic<std::uint64_t> dropped{0};

    void set_error(const std::string& message) {
        std::lock_guard<std::mutex> lock(error_mutex);
        if (error.empty()) {
            error = message;
        }
    }

    void clear_error() {
        std::lock_guard<std::mutex> lock(error_mutex);
        error.clear();
    }

    std::string get_error() const {
        std::lock_guard<std::mutex> lock(error_mutex);
        return error;
    }

    bool write_record(std::uint8_t type, const std::vector<std::uint8_t>& payload) {
        if (file == nullptr) {
            return false;
        }
        std::vector<std::uint8_t> record;
        record.reserve(kRecordHeaderSize + payload.size() + kCrcSize);
        record.push_back(type);
        append_le(record, payload.size(), 4);
        record.insert(record.end(), payload.begin(), payload.end());
        const std::uint32_t crc =
            record_crc(record.data(), record.data() + kRecordHeaderSize,
                       payload.size());
        append_le(record, crc, 4);
        if (!write_bytes(file, record.data(), record.size())) {
            set_error("WAL write failed");
            return false;
        }
        return true;
    }

    bool ensure_schema(const MetricSpec& metric) {
        if (schema_written.find(metric.id) != schema_written.end()) {
            return true;
        }
        if (metric.key.size() > 65535) {
            set_error("metric key is too long for the WAL schema");
            return false;
        }
        std::vector<std::uint8_t> payload;
        payload.reserve(6 + metric.key.size());
        append_le(payload, metric.id, 4);
        append_le(payload, metric.key.size(), 2);
        payload.insert(payload.end(), metric.key.begin(), metric.key.end());
        if (!write_record(kSchemaRecord, payload)) {
            return false;
        }
        schema_written.insert(metric.id);
        return true;
    }

    bool write_sample(const Sample& sample) {
        const MetricSpec* metric = registry.by_id(sample.metric_id);
        if (metric == nullptr) {
            dropped.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        if (!ensure_schema(*metric)) {
            return false;
        }

        std::uint64_t timestamp_bits = 0;
        std::uint64_t value_bits = 0;
        std::memcpy(&timestamp_bits, &sample.timestamp_ms, sizeof(timestamp_bits));
        std::memcpy(&value_bits, &sample.value, sizeof(value_bits));
        std::vector<std::uint8_t> payload;
        payload.reserve(20);
        append_le(payload, metric->id, 4);
        append_le(payload, timestamp_bits, 8);
        append_le(payload, value_bits, 8);
        if (!write_record(kSampleRecord, payload)) {
            return false;
        }
        written.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    bool flush_file() {
        if (file == nullptr) {
            return true;
        }
        std::string flush_error;
        if (!flush_to_disk(file, flush_error)) {
            set_error(flush_error);
            return false;
        }
        return true;
    }

    void run() {
        const auto flush_interval =
            std::chrono::milliseconds(flush_interval_ms);
        auto last_flush = std::chrono::steady_clock::now();
        std::vector<Sample> batch;

        auto fail_and_log = [this]() {
            {
                std::lock_guard<std::mutex> lock(mutex);
                failed = true;
            }
            const std::string failure = get_error();
            EG_ERROR("WAL writer failed: %s",
                     failure.empty() ? "unknown error" : failure.c_str());
        };

        while (true) {
            bool stopping = false;
            {
                std::unique_lock<std::mutex> lock(mutex);
                cv.wait_for(lock, flush_interval, [this] {
                    return stop_requested || !queue.empty();
                });
                stopping = stop_requested;
                if (queue.empty()) {
                    if (stopping) {
                        break;
                    }
                    // No queued samples: still honor the periodic fsync
                    // contract so a quiet stream cannot leave data buffered
                    // indefinitely in libc.
                    lock.unlock();
                    const auto idle_now = std::chrono::steady_clock::now();
                    if (idle_now - last_flush >= flush_interval) {
                        if (!flush_file()) {
                            fail_and_log();
                            return;
                        }
                        last_flush = idle_now;
                    }
                    continue;
                }

                const std::size_t count = std::min<std::size_t>(queue.size(), 2048);
                batch.clear();
                batch.reserve(count);
                for (std::size_t i = 0; i < count; ++i) {
                    batch.push_back(std::move(queue.front()));
                    queue.pop_front();
                }
            }

            bool write_failed = false;
            for (const Sample& sample : batch) {
                if (!write_sample(sample)) {
                    write_failed = true;
                    break;
                }
            }

            const auto now = std::chrono::steady_clock::now();
            const bool should_flush =
                stopping || now - last_flush >= flush_interval;
            if ((write_failed || should_flush) && !flush_file()) {
                write_failed = true;
            }
            if (should_flush) {
                last_flush = now;
            }

            if (write_failed) {
                fail_and_log();
                break;
            }
        }
    }
};

WalWriter::WalWriter(std::string path, const MetricRegistry& registry,
                     std::size_t queue_capacity, std::uint32_t flush_interval_ms)
    : impl_(std::make_unique<Impl>(std::move(path), registry, queue_capacity,
                                   flush_interval_ms)) {}

WalWriter::~WalWriter() {
    stop();
}

bool WalWriter::start(std::string& error) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->running) {
        return true;
    }

    const std::filesystem::path path = std::filesystem::u8path(impl_->path);
    std::error_code ec;
    if (path.has_parent_path() && !path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path(), ec);
        if (ec) {
            error = "cannot create WAL directory: " + ec.message();
            return false;
        }
    }

    const bool existed = std::filesystem::exists(path, ec);
    if (ec) {
        error = "cannot stat WAL path: " + ec.message();
        return false;
    }
    const std::uintmax_t existing_size =
        existed ? std::filesystem::file_size(path, ec) : 0;
    if (ec) {
        error = "cannot stat WAL size: " + ec.message();
        return false;
    }
    if (existing_size > 0 && existing_size < kFileHeaderSize) {
        error = "WAL file is shorter than its header";
        return false;
    }

    impl_->file = open_append(impl_->path);
    if (impl_->file == nullptr) {
        error = "cannot open WAL file for append: " + impl_->path;
        return false;
    }

    if (existing_size == 0) {
        std::vector<std::uint8_t> header(kMagic.begin(), kMagic.end());
        append_le(header, kVersion, 4);
        if (!write_bytes(impl_->file, header.data(), header.size()) ||
            !impl_->flush_file()) {
            error = impl_->get_error();
            if (error.empty()) {
                error = "cannot write WAL header";
            }
            std::fclose(impl_->file);
            impl_->file = nullptr;
            return false;
        }
    }

    impl_->stop_requested = false;
    impl_->failed = false;
    impl_->clear_error();
    impl_->running = true;
    try {
        impl_->thread = std::thread([this] { impl_->run(); });
    } catch (const std::exception& ex) {
        impl_->running = false;
        std::fclose(impl_->file);
        impl_->file = nullptr;
        error = std::string("cannot start WAL writer thread: ") + ex.what();
        return false;
    }
    return true;
}

void WalWriter::stop() {
    if (impl_ == nullptr) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (!impl_->running && !impl_->thread.joinable()) {
            if (impl_->file != nullptr) {
                std::fclose(impl_->file);
                impl_->file = nullptr;
            }
            return;
        }
        impl_->running = false;
        impl_->stop_requested = true;
    }
    impl_->cv.notify_all();
    if (impl_->thread.joinable()) {
        impl_->thread.join();
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->file != nullptr) {
        impl_->flush_file();
        std::fclose(impl_->file);
        impl_->file = nullptr;
    }
}

bool WalWriter::append(const Sample& sample) {
    if (impl_ == nullptr) {
        return false;
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->running || impl_->stop_requested || impl_->failed) {
        return false;
    }
    if (impl_->queue.size() >= impl_->queue_capacity) {
        impl_->queue.pop_front();
        impl_->dropped.fetch_add(1, std::memory_order_relaxed);
    }
    impl_->queue.push_back(sample);
    impl_->cv.notify_one();
    return true;
}

std::uint64_t WalWriter::written() const {
    return impl_ == nullptr ? 0 : impl_->written.load(std::memory_order_relaxed);
}

std::uint64_t WalWriter::dropped() const {
    return impl_ == nullptr ? 0 : impl_->dropped.load(std::memory_order_relaxed);
}

std::string WalWriter::last_error() const {
    if (impl_ == nullptr) {
        return {};
    }
    return impl_->get_error();
}

}  // namespace eg

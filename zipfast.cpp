// zipfast.cpp — Fast ZIP packer for Windows 10/11
// Requires: miniz.h (single-header, MIT)
//   Download: https://github.com/richgel999/miniz/releases  →  miniz.h + miniz.c
//   OR single-header variant:  https://github.com/richgel999/miniz/blob/master/amalgamation/miniz.h
//
// Build (MSVC Developer Prompt):
//   cl /O2 /EHsc /std:c++17 zipfast.cpp miniz.c /Fe:zipfast.exe
//
// Build (MinGW / LLVM):
//   g++ -O2 -std=c++17 -o zipfast zipfast.cpp miniz.c
//
// Usage:
//   zipfast output.zip [-0..-9] path1 [path2 ...]
//   -0  store only (fastest, biggest)
//   -1  fastest DEFLATE
//   -6  default (good balance)
//   -9  best compression (slowest)

#include "miniz.h"

#include <windows.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

// ── types ────────────────────────────────────────────────────────────────────

struct FileEntry {
    std::string disk_path;  // absolute path on disk
    std::string zip_name;   // forward-slash path inside the archive
};

struct CompressedEntry {
    std::size_t           file_index    = 0;
    std::vector<mz_uint8> data;          // deflated or raw bytes
    mz_uint32             original_crc  = 0;
    std::size_t           original_size = 0;
    int                   method        = 0;  // MZ_DEFLATED=8 or 0 (STORE)
    bool                  ok            = false;
};

// ── skip compression for already-compressed formats ──────────────────────────

static bool already_compressed(const fs::path& path) {
    static const char* COMPRESSED_EXTS[] = {
        ".zip",".7z",".rar",".gz",".bz2",".xz",".zst",".br",".lz4",
        ".jpg",".jpeg",".png",".gif",".webp",".avif",".heic",".jxl",
        ".mp4",".mkv",".avi",".mov",".m4v",".wmv",".flv",".webm",
        ".mp3",".aac",".flac",".opus",".ogg",".m4a",
        ".docx",".xlsx",".pptx",".odt",".ods",".odp",
        ".pdf",".epub",".cbz",
    };
    std::string ext = path.extension().string();
    for (char& ch : ext)
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    for (const char* known : COMPRESSED_EXTS)
        if (ext == known) return true;
    return false;
}

// ── file I/O ─────────────────────────────────────────────────────────────────

static std::vector<mz_uint8> read_file_bytes(const std::string& path) {
    HANDLE handle = CreateFileA(
        path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return {};

    LARGE_INTEGER file_size{};
    GetFileSizeEx(handle, &file_size);

    std::vector<mz_uint8> buffer(static_cast<std::size_t>(file_size.QuadPart));
    std::size_t offset = 0;
    DWORD bytes_read = 0;
    while (offset < buffer.size()) {
        DWORD chunk = static_cast<DWORD>(
            std::min(buffer.size() - offset, static_cast<std::size_t>(64u << 20)));
        if (!ReadFile(handle, buffer.data() + offset, chunk, &bytes_read, nullptr)
                || bytes_read == 0)
            break;
        offset += bytes_read;
    }
    CloseHandle(handle);
    buffer.resize(offset);
    return buffer;
}

// ── directory traversal ──────────────────────────────────────────────────────

static void collect_entries(const fs::path& input, std::vector<FileEntry>& out) {
    std::error_code error_code;
    if (fs::is_regular_file(input, error_code)) {
        out.push_back({input.string(), input.filename().string()});
        return;
    }
    if (!fs::is_directory(input, error_code)) return;

    fs::path base = input.parent_path();
    for (const auto& dir_entry : fs::recursive_directory_iterator(
            input, fs::directory_options::skip_permission_denied, error_code)) {
        if (!dir_entry.is_regular_file()) continue;
        auto relative = fs::relative(dir_entry.path(), base, error_code);
        std::string zip_name = relative.string();
        std::replace(zip_name.begin(), zip_name.end(), '\\', '/');
        out.push_back({dir_entry.path().string(), zip_name});
    }
}

// ── thread pool ──────────────────────────────────────────────────────────────

class ThreadPool {
public:
    explicit ThreadPool(unsigned thread_count) : stop_(false) {
        for (unsigned i = 0; i < thread_count; ++i)
            workers_.emplace_back([this] { run(); });
    }

    ~ThreadPool() {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            stop_ = true;
        }
        condition_.notify_all();
        for (auto& worker : workers_) worker.join();
    }

    void submit(std::function<void()> task) {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            queue_.push(std::move(task));
        }
        condition_.notify_one();
    }

private:
    void run() {
        for (;;) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                condition_.wait(lock, [this] { return stop_ || !queue_.empty(); });
                if (stop_ && queue_.empty()) return;
                task = std::move(queue_.front());
                queue_.pop();
            }
            task();
        }
    }

    std::vector<std::thread>          workers_;
    std::queue<std::function<void()>> queue_;
    std::mutex                        mutex_;
    std::condition_variable           condition_;
    bool                              stop_;
};

// ── compression worker ───────────────────────────────────────────────────────

static CompressedEntry compress_entry(std::size_t index, const FileEntry& entry,
                                       int level) {
    CompressedEntry result;
    result.file_index = index;

    auto raw_bytes = read_file_bytes(entry.disk_path);
    result.original_size = raw_bytes.size();
    result.original_crc = static_cast<mz_uint32>(
        mz_crc32(MZ_CRC32_INIT, raw_bytes.data(), raw_bytes.size()));

    // Skip DEFLATE for tiny files or known-compressed formats
    bool skip_deflate = already_compressed(fs::path(entry.disk_path))
                        || raw_bytes.size() < 64
                        || level == 0;

    if (!skip_deflate) {
        // Negative window_bits → raw DEFLATE (no zlib wrapper), required for ZIP
        int flags = tdefl_create_comp_flags_from_zip_params(
            level, -MZ_DEFAULT_WINDOW_BITS, MZ_DEFAULT_STRATEGY);

        std::size_t compressed_size = 0;
        void* compressed_ptr = tdefl_compress_mem_to_heap(
            raw_bytes.data(), raw_bytes.size(), &compressed_size, flags);

        if (compressed_ptr && compressed_size < raw_bytes.size()) {
            result.data.assign(
                static_cast<mz_uint8*>(compressed_ptr),
                static_cast<mz_uint8*>(compressed_ptr) + compressed_size);
            mz_free(compressed_ptr);
            result.method = MZ_DEFLATED;
            result.ok = true;
            return result;
        }
        if (compressed_ptr) mz_free(compressed_ptr);
    }

    // STORE — no compression
    result.data   = std::move(raw_bytes);
    result.method = 0;
    result.ok     = true;
    return result;
}

// ── entry point ──────────────────────────────────────────────────────────────

static void print_usage() {
    std::cout <<
        "zipfast — fast ZIP packer for Windows 10/11\n"
        "Usage:  zipfast output.zip [-0..-9] path [path ...]\n"
        "  -0   store (no compression, fastest)\n"
        "  -1   fastest DEFLATE\n"
        "  -6   default — good speed/ratio balance\n"
        "  -9   best compression ratio\n";
}

int main(int argc, char* argv[]) {
    if (argc < 3) { print_usage(); return 1; }

    int         compression_level = MZ_DEFAULT_COMPRESSION;
    std::string output_path;
    std::vector<std::string> input_paths;

    for (int arg_index = 1; arg_index < argc; ++arg_index) {
        std::string arg = argv[arg_index];
        if (arg.size() == 2 && arg[0] == '-' && std::isdigit(static_cast<unsigned char>(arg[1]))) {
            compression_level = arg[1] - '0';
        } else if (output_path.empty()) {
            output_path = arg;
        } else {
            input_paths.push_back(arg);
        }
    }

    if (output_path.empty() || input_paths.empty()) { print_usage(); return 1; }

    // Collect all file entries from inputs
    std::vector<FileEntry> file_entries;
    for (const auto& input_path : input_paths)
        collect_entries(fs::path(input_path), file_entries);

    if (file_entries.empty()) {
        std::cerr << "No files found in provided paths.\n";
        return 1;
    }

    unsigned thread_count = std::max(1u, std::thread::hardware_concurrency());
    std::cout << "Packing " << file_entries.size() << " file(s)"
              << " → " << output_path
              << "  [level " << compression_level
              << ", " << thread_count << " thread(s)]\n";

    auto time_start = std::chrono::steady_clock::now();

    // Phase 1: parallel compression into memory ──────────────────────────────
    std::vector<CompressedEntry> compressed_entries(file_entries.size());
    std::atomic<std::size_t>    completed_count{0};
    std::mutex                  print_mutex;

    {
        ThreadPool pool(thread_count);
        for (std::size_t i = 0; i < file_entries.size(); ++i) {
            pool.submit([&, i] {
                compressed_entries[i] = compress_entry(
                    i, file_entries[i], compression_level);

                std::size_t done = completed_count.fetch_add(1) + 1;
                if (done == file_entries.size() || done % 100 == 0) {
                    std::unique_lock<std::mutex> lock(print_mutex);
                    std::cout << "\r  compressed " << done
                              << "/" << file_entries.size() << "   " << std::flush;
                }
            });
        }
        // Pool destructor joins all workers — all tasks are done here
    }
    std::cout << "\n";

    // Phase 2: sequential write to ZIP ───────────────────────────────────────
    mz_zip_archive zip_archive{};
    if (!mz_zip_writer_init_file(&zip_archive, output_path.c_str(), 0)) {
        std::cerr << "Cannot create output file: " << output_path << "\n";
        return 1;
    }

    std::size_t total_original   = 0;
    std::size_t total_compressed = 0;
    int         error_count      = 0;

    for (std::size_t i = 0; i < file_entries.size(); ++i) {
        const auto& compressed = compressed_entries[i];
        if (!compressed.ok) {
            std::cerr << "  Error reading: " << file_entries[i].disk_path << "\n";
            ++error_count;
            continue;
        }

        total_original   += compressed.original_size;
        total_compressed += compressed.data.size();

        bool written = false;
        if (compressed.method == MZ_DEFLATED) {
            // Pre-compressed raw DEFLATE — write directly with MZ_ZIP_FLAG_COMPRESSED_DATA
            written = mz_zip_writer_add_mem_ex(
                &zip_archive,
                file_entries[i].zip_name.c_str(),
                compressed.data.data(),
                compressed.data.size(),
                nullptr, 0,
                MZ_ZIP_FLAG_COMPRESSED_DATA,
                static_cast<mz_uint64>(compressed.original_size),
                compressed.original_crc) == MZ_TRUE;
        } else {
            // STORE: pass raw bytes, level 0 forces storage without compression
            written = mz_zip_writer_add_mem_ex(
                &zip_archive,
                file_entries[i].zip_name.c_str(),
                compressed.data.data(),
                compressed.data.size(),
                nullptr, 0,
                0 /* level 0 = STORE */,
                0, 0) == MZ_TRUE;
        }

        if (!written) {
            std::cerr << "  Write error: " << file_entries[i].zip_name << "\n";
            ++error_count;
        }
    }

    if (!mz_zip_writer_finalize_archive(&zip_archive)) {
        std::cerr << "Failed to finalize archive.\n";
        mz_zip_writer_end(&zip_archive);
        return 1;
    }
    mz_zip_writer_end(&zip_archive);

    // Stats ──────────────────────────────────────────────────────────────────
    double elapsed_seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - time_start).count();
    double ratio = (total_original > 0)
        ? 100.0 * (1.0 - static_cast<double>(total_compressed) / total_original)
        : 0.0;
    double speed_mb_per_second = (elapsed_seconds > 0)
        ? (static_cast<double>(total_original) / (1 << 20)) / elapsed_seconds
        : 0.0;

    std::cout << std::fixed << std::setprecision(1)
              << "\nDone in "     << elapsed_seconds      << " s"
              << "  ("            << speed_mb_per_second   << " MB/s)\n"
              << "Original:    "  << total_original   / (1u << 20) << " MB\n"
              << "Compressed:  "  << total_compressed / (1u << 20) << " MB"
              << "  (" << ratio << "% saved)\n";

    if (error_count > 0)
        std::cerr << error_count << " file(s) had errors.\n";

    return (error_count > 0) ? 1 : 0;
}

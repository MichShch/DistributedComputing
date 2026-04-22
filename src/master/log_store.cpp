#include "log_store.hpp"

#include <filesystem>
#include <fstream>
#include <iterator>

#include <nlohmann/json.hpp>

#include "common/logging.hpp"
#include "common/time_utils.hpp"

namespace dc {
namespace master {

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

LogStore::ReadResult ReadFileInternal(const std::string& path,
                                      std::uint64_t offset) {
    LogStore::ReadResult result;
    std::error_code ec;
    if (!fs::exists(path, ec)) {
        result.exists = false;
        result.size_bytes = 0;
        return result;
    }

    result.exists = true;
    result.size_bytes = static_cast<std::uint64_t>(fs::file_size(path, ec));
    if (ec) {
        result.size_bytes = 0;
    }

    if (offset >= result.size_bytes) {
        return result;
    }

    std::ifstream stream(path, std::ios::binary);
    if (!stream.is_open()) {
        return result;
    }

    stream.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    std::string buffer;
    buffer.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    result.data = std::move(buffer);
    return result;
}

}  // namespace

LogStore::LogStore(std::string root_dir) : root_dir_(std::move(root_dir)) {}

std::string LogStore::RootDir() const {
    return root_dir_;
}

bool LogStore::IsPathWithinRoot(const std::string& path) const {
    std::error_code ec;
    fs::path root = fs::weakly_canonical(root_dir_, ec);
    if (ec) {
        return false;
    }
    fs::path candidate = fs::weakly_canonical(path, ec);
    if (ec) {
        return false;
    }
    auto root_it = root.begin();
    auto cand_it = candidate.begin();
    for (; root_it != root.end() && cand_it != candidate.end(); ++root_it, ++cand_it) {
        if (*root_it != *cand_it) {
            return false;
        }
    }
    return root_it == root.end();
}

LogStore::Paths LogStore::PathsForTask(const std::string& task_id) const {
    Paths paths;
    fs::path dir = fs::path(root_dir_) / task_id;
    paths.dir = dir.string();
    paths.stdout_path = (dir / "stdout.log").string();
    paths.stderr_path = (dir / "stderr.log").string();
    paths.meta_path = (dir / "meta.json").string();
    return paths;
}

void LogStore::EnsureLogDir(const std::string& dir) const {
    std::error_code ec;
    fs::create_directories(dir, ec);
}

void LogStore::RefreshMetadata(const std::string& task_id,
                               const std::string& stdout_path,
                               const std::string& stderr_path,
                               const std::string& meta_path) const {
    std::error_code ec;
    json meta;
    meta["task_id"] = task_id;
    meta["root_dir"] = root_dir_;
    meta["updated_at"] = dc::common::NowUtcIso8601();

    // Metadata is lightweight and refreshed on every log read to keep sizes current.
    auto add_entry = [&](const std::string& label, const std::string& path) {
        json entry;
        entry["pathpp"] = path;
        entry["exists"] = fs::exists(path, ec);
        if (entry["exists"].get<bool>()) {
            entry["size_bytes"] = static_cast<std::uint64_t>(fs::file_size(path, ec));
        } else {
            entry["size_bytes"] = 0;
        }
        meta[label] = entry;
    };

    add_entry("stdout", stdout_path);
    add_entry("stderr", stderr_path);

    std::ofstream out_file(meta_path, std::ios::trunc);
    if (!out_file.is_open()) {
        return;
    }
    out_file << meta.dump(2);
}

LogStore::ReadResult LogStore::ReadAll(const std::string& task_id,
                                       const std::string& stream) {
    return ReadInternal(task_id, stream, 0);
}

bool LogStore::WriteAll(const std::string& task_id,
                        const std::string& stream,
                        const std::string& data) {
    const auto paths = PathsForTask(task_id);
    if (!IsPathWithinRoot(paths.dir) || !IsPathWithinRoot(paths.stdout_path) ||
        !IsPathWithinRoot(paths.stderr_path) || !IsPathWithinRoot(paths.meta_path)) {
        spdlog::warn("Rejected log write path for task {}", task_id);
        return false;
    }
    EnsureLogDir(paths.dir);
    const std::string& target =
        (stream == "stderr") ? paths.stderr_path : paths.stdout_path;
    std::ofstream out(target, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        return false;
    }
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    out.close();
    RefreshMetadata(task_id, paths.stdout_path, paths.stderr_path, paths.meta_path);
    spdlog::debug("Wrote log {} stream={} bytes={}", task_id, stream, data.size());
    return true;
}

LogStore::ReadResult LogStore::ReadInternal(const std::string& task_id,
                                            const std::string& stream,
                                            std::uint64_t offset) {
    const auto paths = PathsForTask(task_id);
    if (!IsPathWithinRoot(paths.dir) || !IsPathWithinRoot(paths.stdout_path) ||
        !IsPathWithinRoot(paths.stderr_path) || !IsPathWithinRoot(paths.meta_path)) {
        spdlog::warn("Rejected log path for task {}", task_id);
        return LogStore::ReadResult{};
    }
    EnsureLogDir(paths.dir);
    const std::string& target =
        (stream == "stderr") ? paths.stderr_path : paths.stdout_path;

    auto result = ReadFileInternal(target, offset);
    if (result.exists) {
        if (offset == 0) {
            spdlog::debug("Read log {} stream={} size_bytes={}",
                          task_id,
                          stream,
                          result.size_bytes);
        } else {
            spdlog::debug("Read log tail {} stream={} from={} size_bytes={}",
                          task_id,
                          stream,
                          offset,
                          result.size_bytes);
        }
    }
    RefreshMetadata(task_id, paths.stdout_path, paths.stderr_path, paths.meta_path);
    return result;
}

LogStore::ReadResult LogStore::ReadFromOffset(const std::string& task_id,
                                              const std::string& stream,
                                              std::uint64_t offset) {
    return ReadInternal(task_id, stream, offset);
}

}  // namespace master
}  // namespace dc

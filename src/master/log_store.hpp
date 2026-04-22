#pragma once

#include <cstdint>
#include <string>

namespace dc {
namespace master {

// File-backed log broker layout:
//   LOG_DIR/<task_id>/stdout.log
//   LOG_DIR/<task_id>/stderr.log
//   LOG_DIR/<task_id>/meta.json
class LogStore {
public:
    struct ReadResult {
        std::string data;
        std::uint64_t size_bytes = 0;
        bool exists = false;
    };

    explicit LogStore(std::string root_dir);

    std::string RootDir() const;

    // Reads the full log for a stream ("stdout" or "stderr").
    ReadResult ReadAll(const std::string& task_id, const std::string& stream);

    // Reads log starting from an offset; returns remaining bytes.
    ReadResult ReadFromOffset(const std::string& task_id,
                              const std::string& stream,
                              std::uint64_t offset);

    // Writes full content for a stream; overwrites existing file.
    // Returns false on path validation or IO error.
    bool WriteAll(const std::string& task_id,
                  const std::string& stream,
                  const std::string& data);

private:
    struct Paths {
        std::string dir;
        std::string stdout_path;
        std::string stderr_path;
        std::string meta_path;
    };

    ReadResult ReadInternal(const std::string& task_id,
                            const std::string& stream,
                            std::uint64_t offset);

    bool IsPathWithinRoot(const std::string& path) const;
    Paths PathsForTask(const std::string& task_id) const;
    void EnsureLogDir(const std::string& dir) const;
    void RefreshMetadata(const std::string& task_id,
                         const std::string& stdout_path,
                         const std::string& stderr_path,
                         const std::string& meta_path) const;

    std::string root_dir_;
};

}  // namespace master
}  // namespace dc

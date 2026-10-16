#pragma once

namespace mssv::process {

enum class SpawnFailureCategory {
    missing_executable,
    resource_exhaustion,
    other_native_error,
};

struct SpawnFailure {
    SpawnFailureCategory category;
    int native_error;
};

[[nodiscard]] SpawnFailure classify_native_spawn_error(int native_error, bool is_linux) noexcept;

} // namespace mssv::process

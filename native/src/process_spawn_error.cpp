#include "mssv/process_spawn_error.h"

namespace mssv::process {

SpawnFailure classify_native_spawn_error(const int native_error, const bool is_linux) noexcept
{
    if (native_error == 2) {
        return {SpawnFailureCategory::missing_executable, native_error};
    }

    if (is_linux && native_error == 11) {
        return {SpawnFailureCategory::resource_exhaustion, native_error};
    }

    return {SpawnFailureCategory::other_native_error, native_error};
}

} // namespace mssv::process

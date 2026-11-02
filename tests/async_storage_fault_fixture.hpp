#pragma once
#include <atomic>
#include <chrono>
#include <dlfcn.h>
#include <thread>
#include <string_view>

// Delay the test's real boundary publication, without production hooks. This
// makes the asynchronous writer/owner notification gap deterministic.
namespace moex::connector_host::storage_fixture {
inline std::atomic<bool> hold_boundary_publish{}, boundary_publish_entered{};
inline const char* held_boundary_path{};
} // namespace moex::connector_host::storage_fixture
extern "C" int rename(const char* from, const char* to) {
    static const auto real = reinterpret_cast<int (*)(const char*, const char*)>(dlsym(RTLD_NEXT, "rename"));
    using namespace moex::connector_host::storage_fixture;
    if (hold_boundary_publish.load(std::memory_order_acquire) && held_boundary_path &&
        std::string_view(to) == held_boundary_path) {
        boundary_publish_entered.store(true, std::memory_order_release);
        while (hold_boundary_publish.load(std::memory_order_acquire))
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return real(from, to);
}

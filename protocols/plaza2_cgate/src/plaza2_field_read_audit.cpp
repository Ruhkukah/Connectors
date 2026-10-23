#include "moex/plaza2/cgate/plaza2_field_read_audit.hpp"

namespace moex::plaza2::generated {
namespace {
thread_local FieldReadObserver observer{};
}
void SetFieldReadObserver(FieldReadObserver value) noexcept {
    observer = value;
}
void AuditFieldRead(FieldCode code) noexcept {
    if (observer)
        observer(code);
}
} // namespace moex::plaza2::generated

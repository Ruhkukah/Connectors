#pragma once

#include "moex/plaza2/cgate/plaza2_metadata.hpp"

#ifdef MOEX_CGATE_FIELD_READ_AUDIT
namespace moex::plaza2::generated {
using FieldReadObserver = void (*)(FieldCode) noexcept;
void SetFieldReadObserver(FieldReadObserver observer) noexcept;
void AuditFieldRead(FieldCode code) noexcept;
} // namespace moex::plaza2::generated
#endif

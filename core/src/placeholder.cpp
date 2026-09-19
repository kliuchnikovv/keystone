#include "core/core.hpp"

#include "hubproto/version.hpp"

namespace core {

bool ready() noexcept {
  // Touch libhubproto so Phase 0 also proves core -> hubproto linking works.
  return hubproto::kProtocolVersion != 0;
}

}  // namespace core

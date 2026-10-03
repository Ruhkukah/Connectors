#pragma once

#include <string>
#include <string_view>

namespace moex::connector_host {
// Discard only the malformed line; keep polling CGate and accepting later commands.
class CommandInput {
  public:
    template <class Line, class Error> void feed(std::string_view bytes, Line line, Error error) {
        for (const char value : bytes) {
            if (value == '\n') {
                if (!discarding_)
                    line(pending_);
                pending_.clear();
                discarding_ = false;
            } else if (!discarding_) {
                if (pending_.size() == 65536) {
                    pending_.clear();
                    discarding_ = true;
                    error("command line exceeds 65536 bytes");
                } else
                    pending_ += value;
            }
        }
    }

  private:
    std::string pending_;
    bool discarding_{};
};
} // namespace moex::connector_host

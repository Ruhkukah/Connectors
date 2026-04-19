#include "moex/twime_trade/transport/twime_socket_handle.hpp"

#include <unistd.h>

namespace moex::twime_trade::transport {

TwimeSocketHandle::TwimeSocketHandle(int native_handle) noexcept : native_handle_(native_handle) {}

TwimeSocketHandle::TwimeSocketHandle(TwimeSocketHandle&& other) noexcept : native_handle_(other.release()) {}

TwimeSocketHandle& TwimeSocketHandle::operator=(TwimeSocketHandle&& other) noexcept {
    if (this != &other) {
        close();
        native_handle_ = other.release();
    }
    return *this;
}

TwimeSocketHandle::~TwimeSocketHandle() {
    close();
}

bool TwimeSocketHandle::valid() const noexcept {
    return native_handle_ >= 0;
}

int TwimeSocketHandle::native_handle() const noexcept {
    return native_handle_;
}

void TwimeSocketHandle::reset(int native_handle) noexcept {
    if (native_handle_ != native_handle) {
        close();
        native_handle_ = native_handle;
    }
}

int TwimeSocketHandle::release() noexcept {
    const int released = native_handle_;
    native_handle_ = -1;
    return released;
}

void TwimeSocketHandle::close() noexcept {
    if (native_handle_ >= 0) {
        ::close(native_handle_);
        native_handle_ = -1;
    }
}

} // namespace moex::twime_trade::transport

#pragma once

namespace moex::twime_trade::transport {

class TwimeSocketHandle {
  public:
    TwimeSocketHandle() noexcept = default;
    explicit TwimeSocketHandle(int native_handle) noexcept;
    TwimeSocketHandle(const TwimeSocketHandle&) = delete;
    TwimeSocketHandle& operator=(const TwimeSocketHandle&) = delete;
    TwimeSocketHandle(TwimeSocketHandle&& other) noexcept;
    TwimeSocketHandle& operator=(TwimeSocketHandle&& other) noexcept;
    ~TwimeSocketHandle();

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] int native_handle() const noexcept;

    void reset(int native_handle = -1) noexcept;
    [[nodiscard]] int release() noexcept;
    void close() noexcept;

  private:
    int native_handle_{-1};
};

} // namespace moex::twime_trade::transport

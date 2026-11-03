#pragma once

#include "core/capture/IVideoCaptureSource.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>

namespace streamforge {

class DxgiDesktopCaptureSource final : public IVideoCaptureSource {
public:
    DxgiDesktopCaptureSource();
    ~DxgiDesktopCaptureSource() override;

    DxgiDesktopCaptureSource(const DxgiDesktopCaptureSource&) = delete;
    DxgiDesktopCaptureSource& operator=(const DxgiDesktopCaptureSource&) = delete;
    DxgiDesktopCaptureSource(DxgiDesktopCaptureSource&&) = delete;
    DxgiDesktopCaptureSource& operator=(DxgiDesktopCaptureSource&&) = delete;

    bool start(const VideoCaptureConfig& config,
               VideoCaptureCallback callback,
               std::string& error) override;
    bool stop(std::string& error) override;
    [[nodiscard]] bool isRunning() const noexcept override;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace streamforge

#include "video_core/renderer_vulkan/vk_presentation_sink.h"

#include "video_core/renderer_vulkan/vk_presenter.h"

namespace Vulkan {
namespace {
struct FrameAdapter final : VideoCore::PresentationFrame {
    explicit FrameAdapter(Frame* frame) : frame{frame} {}
    Frame* frame;
};
class PresentationAdapter final : public VideoCore::PresentationSink {
public:
    explicit PresentationAdapter(Presenter& presenter) : presenter{presenter} {}
    void RegisterVideoOutSurface(const Libraries::VideoOut::BufferAttributeGroup& group,
                                 VAddr address) override {
        presenter.RegisterVideoOutSurface(group, address);
    }
    VideoCore::FrameHandle PrepareFrame(const Libraries::VideoOut::BufferAttributeGroup& group,
                                        VAddr address) override {
        return std::make_shared<FrameAdapter>(presenter.PrepareFrame(group, address));
    }
    VideoCore::FrameHandle PrepareBlankFrame(bool thread) override {
        return std::make_shared<FrameAdapter>(presenter.PrepareBlankFrame(thread));
    }
    VideoCore::FrameHandle PrepareLastFrame() override {
        const auto frame = presenter.PrepareLastFrame();
        return frame ? std::make_shared<FrameAdapter>(frame) : nullptr;
    }
    void Present(const VideoCore::FrameHandle& frame, bool reuse, bool game) override {
        presenter.Present(std::static_pointer_cast<FrameAdapter>(frame)->frame, reuse, game);
    }
    bool IsHDRSupported() const override {
        return presenter.IsHDRSupported();
    }
    void SetHDR(bool enabled) override {
        presenter.SetHDR(enabled);
    }
    void SetGamma(float gamma) override {
        presenter.GetPPSettingsRef().gamma = gamma;
    }

private:
    Presenter& presenter;
};
} // namespace
std::shared_ptr<VideoCore::PresentationSink> CreatePresentationSink(Presenter& presenter) {
    return std::make_shared<PresentationAdapter>(presenter);
}
} // namespace Vulkan

#include "video_core/presentation_sink.h"

#include <mutex>
#include <stdexcept>

namespace VideoCore {
namespace {
std::mutex sink_mutex;
std::shared_ptr<PresentationSink> presentation;
} // namespace
void SetPresentationSink(std::shared_ptr<PresentationSink> sink) {
    std::scoped_lock lock{sink_mutex};
    presentation = std::move(sink);
}
std::shared_ptr<PresentationSink> GetPresentationSink() {
    std::scoped_lock lock{sink_mutex};
    if (!presentation)
        throw std::runtime_error("VideoOut presentation backend is not initialized");
    return presentation;
}
} // namespace VideoCore

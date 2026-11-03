#include "RecordingPipeline.h"
#include "RecordingInputDispatcher.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using namespace streamforge;

namespace {

struct State {
    std::mutex mutex;
    std::condition_variable changed;
    std::atomic<int> audioFlush{0}, videoFlush{0}, audioShutdown{0}, videoShutdown{0};
    int writes{0};
    bool configureCalled{false}, openCalled{false}, finalized{false}, aborted{false};
    bool failVideoInit{false}, failOpen{false}, failWrite{false};
    bool failAudioFlush{false};
    bool blockFirstWrite{false}, firstWriteEntered{false}, releaseFirstWrite{false};
    bool blockAudioEncode{false}, audioEncodeEntered{false}, releaseAudioEncode{false};
    bool blockVideoEncode{false}, videoEncodeEntered{false}, releaseVideoEncode{false};
};

EncodedPacket packet(EncodedMediaType type, int index) {
    EncodedPacket p;
    p.mediaType = type;
    p.streamIndex = index;
    p.pts = p.dts = 0;
    p.duration = 1;
    p.timebase = type == EncodedMediaType::Audio ? MediaTimebase{1, 48000}
                                                  : MediaTimebase{1, 60};
    p.keyFrame = true;
    p.data = std::make_shared<const std::vector<std::uint8_t>>(
        std::vector<std::uint8_t>{1, 2, 3, 4});
    return p;
}

class Audio final : public IAudioEncoder {
public:
    Audio(std::shared_ptr<State> s, EncodedPacketCallback cb, int id)
        : s_(std::move(s)), cb_(std::move(cb)), id_(id) {}
    bool initialize(std::string&) override { return true; }
    bool encode(const AudioFrame&, std::string& e) override {
        {
            std::unique_lock lock(s_->mutex);
            if (s_->blockAudioEncode) {
                s_->audioEncodeEntered = true;
                s_->changed.notify_all();
                s_->changed.wait(lock, [this] { return s_->releaseAudioEncode; });
            }
        }
        return cb_(packet(EncodedMediaType::Audio, id_), e);
    }
    bool flush(std::string& e) override {
        ++s_->audioFlush;
        if (s_->failAudioFlush) { e = "mock audio flush failure"; return false; }
        return cb_(packet(EncodedMediaType::Audio, id_), e);
    }
    bool getStreamDescriptor(EncodedStreamDescriptor& d, std::string&) const override {
        d = {};
        d.streamIndex=id_; d.mediaType=EncodedMediaType::Audio; d.codec=EncodedCodec::AAC;
        d.timebase={1,48000}; d.sampleRate=48000; d.channels=2;
        d.channelLayout=AudioChannelLayout::Stereo;
        d.sampleFormat=AudioSampleFormat::Float32Planar; d.bitrate=192000;
        return d.isValid();
    }
    void shutdown() noexcept override { ++s_->audioShutdown; }
private:
    std::shared_ptr<State> s_; EncodedPacketCallback cb_; int id_;
};

class Video final : public IVideoEncoder {
public:
    Video(std::shared_ptr<State> s, EncodedPacketCallback cb, int id)
        : s_(std::move(s)), cb_(std::move(cb)), id_(id) {}
    bool initialize(std::string& e) override {
        if (s_->failVideoInit) { e="mock video init failure"; return false; }
        return true;
    }
    bool encode(const VideoFrameInput&, std::string& e) override {
        {
            std::unique_lock lock(s_->mutex);
            if (s_->blockVideoEncode) {
                s_->videoEncodeEntered = true;
                s_->changed.notify_all();
                s_->changed.wait(lock, [this] { return s_->releaseVideoEncode; });
            }
        }
        return cb_(packet(EncodedMediaType::Video, id_), e);
    }
    bool flush(std::string& e) override {
        ++s_->videoFlush;
        return cb_(packet(EncodedMediaType::Video, id_), e);
    }
    bool getStreamDescriptor(EncodedStreamDescriptor& d, std::string&) const override {
        d = {};
        d.streamIndex=id_; d.mediaType=EncodedMediaType::Video; d.codec=EncodedCodec::H264;
        d.timebase={1,60}; d.width=1920; d.height=1080;
        d.pixelFormat=PixelFormat::YUV420P;
        d.frameRateNumerator=60; d.frameRateDenominator=1; d.bitrate=6000000;
        return d.isValid();
    }
    void shutdown() noexcept override { ++s_->videoShutdown; }
private:
    std::shared_ptr<State> s_; EncodedPacketCallback cb_; int id_;
};

class Muxer final : public IMediaMuxer {
public:
    explicit Muxer(std::shared_ptr<State> s) : s_(std::move(s)) {}
    bool configure(const RecordingOutputConfig&,
                   const std::vector<EncodedStreamDescriptor>& streams,
                   std::string& e) override {
        if (streams.size()!=2) { e="expected 2 streams"; return false; }
        std::lock_guard lock(s_->mutex); s_->configureCalled=true; return true;
    }
    bool open(std::string& e) override {
        std::lock_guard lock(s_->mutex); s_->openCalled=true;
        if (s_->failOpen) { e="mock open failure"; return false; } return true;
    }
    bool writePacket(const EncodedPacket&, std::string& e) override {
        std::unique_lock lock(s_->mutex);
        ++s_->writes;
        if (s_->blockFirstWrite && s_->writes==1) {
            s_->firstWriteEntered=true; s_->changed.notify_all();
            s_->changed.wait(lock,[this]{return s_->releaseFirstWrite;});
        }
        if (s_->failWrite) { e="mock write failure"; return false; }
        return true;
    }
    bool finalize(std::string&) override {
        std::lock_guard lock(s_->mutex); s_->finalized=true; return true;
    }
    void abort() noexcept override {
        std::lock_guard lock(s_->mutex); s_->aborted=true; s_->changed.notify_all();
    }
private:
    std::shared_ptr<State> s_;
};

struct Fixture {
    std::shared_ptr<State> s=std::make_shared<State>();
    RecordingPipelineFactories factories() {
        RecordingPipelineFactories f; auto p=s;
        f.createAudioEncoder=[p](const AudioEncoderConfig&,EncodedPacketCallback cb,int id) {
            return std::make_unique<Audio>(p,std::move(cb),id);
        };
        f.createVideoEncoder=[p](const VideoEncoderConfig&,EncodedPacketCallback cb,int id) {
            return std::make_unique<Video>(p,std::move(cb),id);
        };
        f.createMuxer=[p]{return std::make_unique<Muxer>(p);};
        return f;
    }
    RecordingPipelineConfig config() const {
        RecordingPipelineConfig c; c.output.outputPath="mock.mkv";
        c.packetQueue.maxPackets=16; c.packetQueue.maxBytes=1024; return c;
    }
};

void check(bool value, const char* text) {
    if (!value) { std::cerr << "FAILED: " << text << '\n'; std::exit(1); }
}
bool waitFailed(RecordingPipeline& p) {
    const auto end=std::chrono::steady_clock::now()+1500ms;
    while(std::chrono::steady_clock::now()<end) {
        if(p.snapshot().state==RecordingState::Failed) return true;
        std::this_thread::sleep_for(2ms);
    }
    return p.snapshot().state==RecordingState::Failed;
}

void graceful() {
    Fixture f; RecordingPipeline p(f.factories()); std::string e;
    auto c=f.config(); check(p.start(c,e),"graceful start");
    AudioFrame a; a.sampleRate=48000; a.channels=2; a.frameCount=1; a.samples={0,0};
    std::uint8_t bytes[4]{}; VideoFrameInput v; v.data[0]=bytes; v.linesize[0]=1920*4;
    v.width=1920; v.height=1080; v.pixelFormat=PixelFormat::BGRA8; v.timebase={1,60};
    check(p.submitAudioFrame(a,e),"audio submit"); check(p.submitVideoFrame(v,e),"video submit");
    check(p.stop(RecordingStopMode::Graceful,e),"graceful stop");
    check(p.snapshot().state==RecordingState::Completed,"completed state");
    check(f.s->audioFlush==1 && f.s->videoFlush==1,"both encoders flushed");
    check(f.s->finalized && !f.s->aborted,"muxer finalized");
    check(f.s->writes==4,"four packets written");
}

void abortRecordingTest() {
    Fixture f; RecordingPipeline p(f.factories()); std::string e;
    check(p.start(f.config(),e),"abort start");
    check(p.stop(RecordingStopMode::Abort,e),"abort stop");
    check(p.snapshot().state==RecordingState::Aborted,"aborted state");
    check(f.s->audioFlush==0 && f.s->videoFlush==0,"abort skips flush");
    check(f.s->aborted && !f.s->finalized,"muxer aborted");
}

void startupFailure() {
    Fixture f; f.s->failVideoInit=true; RecordingPipeline p(f.factories()); std::string e;
    check(!p.start(f.config(),e),"startup failure rejected");
    check(p.snapshot().state==RecordingState::Failed,"startup failure state");
    check(f.s->audioShutdown==1 && f.s->videoShutdown==1,"partial encoders cleaned up");
    check(!f.s->configureCalled,"muxer not configured after encoder failure");
}

void muxerOpenFailure() {
    Fixture f; f.s->failOpen=true; RecordingPipeline p(f.factories()); std::string e;
    check(!p.start(f.config(),e),"muxer open rejected");
    check(p.snapshot().state==RecordingState::Failed,"muxer open failed state");
    check(f.s->aborted,"muxer abort invoked");
}

void muxerWriteFailure() {
    Fixture f; f.s->failWrite=true; RecordingPipeline p(f.factories()); std::string e;
    check(p.start(f.config(),e),"write failure start");
    AudioFrame a; a.sampleRate=48000; a.channels=2; a.frameCount=1; a.samples={0,0};
    check(p.submitAudioFrame(a,e),"write failure packet initially queued");
    check(waitFailed(p),"muxer write error propagated asynchronously");
    check(!p.stop(RecordingStopMode::Graceful,e),"failed stop reported");
}


void stopWaitsForInFlightInput() {
    Fixture f;
    f.s->blockAudioEncode = true;
    RecordingPipeline p(f.factories());
    std::string startError;
    check(p.start(f.config(), startError), "in-flight start");

    AudioFrame a;
    a.sampleRate=48000; a.channels=2; a.frameCount=1; a.samples={0,0};

    std::atomic<bool> submitOk{false};
    std::string submitError;
    std::thread submitter([&] {
        submitOk = p.submitAudioFrame(a, submitError);
    });

    {
        std::unique_lock lock(f.s->mutex);
        check(f.s->changed.wait_for(lock, 1500ms, [&] {
            return f.s->audioEncodeEntered;
        }), "audio encode entered");
    }

    std::atomic<bool> stopDone{false};
    std::string stopError;
    std::thread stopper([&] {
        (void)p.stop(RecordingStopMode::Graceful, stopError);
        stopDone = true;
    });

    const auto deadline = std::chrono::steady_clock::now() + 1500ms;
    while (p.snapshot().acceptsInput &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    check(!p.snapshot().acceptsInput, "stop closed input gate");
    check(!stopDone.load(), "stop waits while accepted encode is active");

    {
        std::lock_guard lock(f.s->mutex);
        f.s->releaseAudioEncode = true;
        f.s->changed.notify_all();
    }

    submitter.join();
    stopper.join();
    check(submitOk.load(), "in-flight submit completed");
    check(stopDone.load(), "stop completed after input left gate");
    check(p.snapshot().state == RecordingState::Completed,
          "in-flight stop finalized cleanly");
}

void encoderFlushFailure() {
    Fixture f;
    f.s->failAudioFlush = true;
    RecordingPipeline p(f.factories());
    std::string e;
    check(p.start(f.config(), e), "flush failure start");
    check(!p.stop(RecordingStopMode::Graceful, e),
          "audio flush failure reported");
    check(p.snapshot().state == RecordingState::Failed,
          "flush failure state retained");
    check(f.s->audioShutdown == 1 && f.s->videoShutdown == 1,
          "both encoders shut down after flush failure");
    check(f.s->aborted && !f.s->finalized,
          "muxer aborted after flush failure");
}


AudioFrame makeAudioFrame() {
    AudioFrame frame;
    frame.sampleRate = 48000;
    frame.channels = 2;
    frame.frameCount = 1;
    frame.samples = {0.0F, 0.0F};
    frame.timestampNs = 0;
    return frame;
}

OwnedVideoFrame makeOwnedVideoFrame() {
    OwnedVideoFrame frame;
    frame.width = 2;
    frame.height = 2;
    frame.pixelFormat = PixelFormat::BGRA8;
    frame.linesize[0] = 8;
    frame.planes[0].assign(16, 0);
    frame.timebase = {1, 30};
    return frame;
}

void nativeVideoResourceLifetime() {
    auto owner = std::make_shared<int>(42);
    OwnedVideoFrame frame = makeOwnedVideoFrame();
    frame.nativeResourceType = NativeVideoResourceType::D3D11Texture2D;
    frame.nativeResource = owner.get();
    frame.nativeResourceOwner = owner;

    VideoFrameInput view = frame.view();
    frame.nativeResourceOwner.reset();
    owner.reset();

    check(view.nativeResourceType == NativeVideoResourceType::D3D11Texture2D,
          "native resource type survives owned-frame view");
    check(view.nativeResource != nullptr &&
          *static_cast<int*>(view.nativeResource) == 42,
          "native resource remains alive while view owns its lease");
    check(static_cast<bool>(view.nativeResourceOwner),
          "video input view retains native resource owner");
}

void dispatcherNormalStop() {
    Fixture f;
    RecordingPipeline pipeline(f.factories());
    std::string error;
    auto config = f.config();
    config.packetQueue.maxPackets = 128;
    config.packetQueue.maxBytes = 4096;
    check(pipeline.start(config, error), "dispatcher pipeline start");

    RecordingInputDispatcher dispatcher(pipeline);
    check(dispatcher.start(error), "dispatcher start");
    check(dispatcher.trySubmitAudio(makeAudioFrame()), "dispatcher audio submit");
    check(dispatcher.trySubmitVideo(makeOwnedVideoFrame()), "dispatcher video submit");
    check(dispatcher.stop(RecordingStopMode::Graceful, error), "dispatcher graceful stop");
    check(pipeline.snapshot().state == RecordingState::Completed,
          "dispatcher completed recording");
    check(f.s->finalized && !f.s->aborted, "dispatcher finalized muxer");
}

void dispatcherAudioOverflow() {
    Fixture f;
    f.s->blockAudioEncode = true;
    RecordingPipeline pipeline(f.factories());
    std::string error;
    auto config = f.config();
    check(pipeline.start(config, error), "audio overflow pipeline start");

    RecordingInputDispatcher dispatcher(pipeline);
    check(dispatcher.start(error), "audio overflow dispatcher start");
    check(dispatcher.trySubmitAudio(makeAudioFrame()), "first audio frame queued");

    {
        std::unique_lock lock(f.s->mutex);
        check(f.s->changed.wait_for(lock, 1500ms, [&] {
            return f.s->audioEncodeEntered;
        }), "audio worker entered blocking encoder");
    }

    for (int index = 0; index < 127; ++index) {
        check(dispatcher.trySubmitAudio(makeAudioFrame()),
              "fill bounded audio queue");
    }
    check(!dispatcher.trySubmitAudio(makeAudioFrame()),
          "audio queue overflow is rejected");
    check(dispatcher.snapshot().audioQueueOverflows == 1,
          "audio overflow counter incremented");

    {
        std::lock_guard lock(f.s->mutex);
        f.s->releaseAudioEncode = true;
        f.s->changed.notify_all();
    }
    check(!dispatcher.stop(RecordingStopMode::Graceful, error),
          "audio overflow forces dispatcher failure");
    check(pipeline.snapshot().state == RecordingState::Aborted,
          "audio overflow aborts recording");
}

void dispatcherVideoDropPolicy() {
    Fixture f;
    f.s->blockVideoEncode = true;
    RecordingPipeline pipeline(f.factories());
    std::string error;
    auto config = f.config();
    config.packetQueue.maxPackets = 128;
    config.packetQueue.maxBytes = 4096;
    check(pipeline.start(config, error), "video drop pipeline start");

    RecordingInputDispatcher dispatcher(pipeline);
    check(dispatcher.start(error), "video drop dispatcher start");
    check(dispatcher.trySubmitVideo(makeOwnedVideoFrame()),
          "first video frame queued");

    {
        std::unique_lock lock(f.s->mutex);
        check(f.s->changed.wait_for(lock, 1500ms, [&] {
            return f.s->videoEncodeEntered;
        }), "video worker entered blocking encoder");
    }

    for (int index = 0; index < 31; ++index) {
        check(dispatcher.trySubmitVideo(makeOwnedVideoFrame()),
              "fill bounded video queue");
    }
    check(!dispatcher.trySubmitVideo(makeOwnedVideoFrame()),
          "video queue applies drop-newest policy");
    check(dispatcher.snapshot().droppedVideoFrames == 1,
          "video drop counter incremented");

    {
        std::lock_guard lock(f.s->mutex);
        f.s->releaseVideoEncode = true;
        f.s->changed.notify_all();
    }
    check(dispatcher.stop(RecordingStopMode::Graceful, error),
          "video drop does not fail recording");
    check(pipeline.snapshot().state == RecordingState::Completed,
          "video drop policy preserves recording");
}

void queueOverflow() {
    Fixture f; f.s->blockFirstWrite=true; RecordingPipeline p(f.factories());
    auto c=f.config(); c.packetQueue.maxPackets=1; std::string e;
    check(p.start(c,e),"overflow start");
    std::uint8_t bytes[4]{}; VideoFrameInput v; v.data[0]=bytes; v.linesize[0]=1920*4;
    v.width=1920; v.height=1080; v.pixelFormat=PixelFormat::BGRA8; v.timebase={1,60};
    check(p.submitVideoFrame(v,e),"first packet accepted");
    {
        std::unique_lock lock(f.s->mutex);
        check(f.s->changed.wait_for(lock,1500ms,[&]{return f.s->firstWriteEntered;}),
              "writer began first packet");
    }
    check(p.submitVideoFrame(v,e),"second packet queued");
    check(!p.submitVideoFrame(v,e),"third packet rejected on capacity");
    check(p.snapshot().state==RecordingState::Failed,"queue overflow fatal");
    {
        std::lock_guard lock(f.s->mutex); f.s->releaseFirstWrite=true; f.s->changed.notify_all();
    }
    check(!p.stop(RecordingStopMode::Graceful,e),"overflow failure retained");
    check(f.s->aborted,"overflow aborts muxer");
}

} // namespace

int main() {
    graceful(); abortRecordingTest(); startupFailure(); muxerOpenFailure();
    muxerWriteFailure(); stopWaitsForInFlightInput();
    encoderFlushFailure(); nativeVideoResourceLifetime();
    dispatcherNormalStop(); dispatcherAudioOverflow();
    dispatcherVideoDropPolicy(); queueOverflow();
    std::cout << "RecordingPipeline lifecycle tests passed.\n";
    return 0;
}

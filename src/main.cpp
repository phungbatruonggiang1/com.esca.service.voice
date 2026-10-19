// esca-voiced: the button, the microphone, and the words that come back.
//
// The application presses a button, this opens the microphone, and only
// once everything is genuinely ready does it say so -- at which point the
// application switches to its waveform and the user starts talking. That
// handshake is the point: checking the server *before* the user speaks
// means a dead link is a message on screen rather than a sentence spoken
// into nothing and a silence afterwards.
//
//   App  --StartListening()-->  esca-voiced
//                                  | opens the mic, checks the server
//   App  <--StateChanged("listening")
//        <--Level(rms, peak) at 20 Hz   -> the waveform
//        <--SpeechStarted / SpeechEnded
//        <--Transcript(text)            -> what was said

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <sdbus-c++/sdbus-c++.h>

#include "esca/ai/client.hpp"
#include "esca/audio/client.hpp"
#include "vad.hpp"

namespace {

constexpr const char *kServiceName = "vn.sparc.esca.Voice1";
constexpr const char *kObjectPath = "/vn/sparc/esca/Voice1";
constexpr const char *kInterface = "vn.sparc.esca.Voice1";

sdbus::IConnection *g_connection = nullptr;

void handle_signal(int)
{
    if (g_connection)
        g_connection->leaveEventLoop();
}

struct Options {
    std::string server = "http://192.168.1.10:8099";
    std::string device;
    std::string tts_dir = "/tmp";
    int timeout_ms = 20000; ///< transcription is slower than anomaly scoring
    /// Spoken before listening starts, so the speaker knows when to begin.
    /// Empty disables it.
    std::string prompt = "Now, speak please.";
    /// How long to wait for anyone to start talking. The clock only runs
    /// while nothing has been said; once speech starts the VAD decides when
    /// the sentence ends.
    double wait_seconds = 20.0;
};

int usage()
{
    std::fprintf(stderr,
                 "usage: esca-voiced [options]\n"
                 "  --server URL    AI server base URL\n"
                 "  --device ID     ALSA device, empty for the first one\n"
                 "  --tts-dir DIR   where spoken replies are written (default /tmp)\n"
                 "  --timeout MS    HTTP timeout (default 20000)\n"
                 "  --prompt TEXT   spoken before listening; \"\" disables it\n"
                 "  --wait SECONDS  how long to wait for speech (default 20)\n");
    return 2;
}

/// Minimal WAV in memory, because the server takes a file and the board
/// has the samples.
std::vector<uint8_t> wrap_wav(const esca::audio::PcmFormat &format,
                              const std::vector<uint8_t> &pcm)
{
    std::vector<uint8_t> out;
    out.reserve(44 + pcm.size());

    auto push32 = [&](uint32_t v) {
        out.push_back(uint8_t(v));
        out.push_back(uint8_t(v >> 8));
        out.push_back(uint8_t(v >> 16));
        out.push_back(uint8_t(v >> 24));
    };
    auto push16 = [&](uint16_t v) {
        out.push_back(uint8_t(v));
        out.push_back(uint8_t(v >> 8));
    };
    auto push = [&](const char *s) {
        for (int i = 0; i < 4; ++i)
            out.push_back(static_cast<uint8_t>(s[i]));
    };

    const uint32_t frame_bytes = format.bytes_per_frame();
    push("RIFF");
    push32(static_cast<uint32_t>(36 + pcm.size()));
    push("WAVE");
    push("fmt ");
    push32(16);
    push16(1);
    push16(static_cast<uint16_t>(format.channels));
    push32(format.rate);
    push32(format.rate * frame_bytes);
    push16(static_cast<uint16_t>(frame_bytes));
    push16(static_cast<uint16_t>(format.bits));
    push("data");
    push32(static_cast<uint32_t>(pcm.size()));
    out.insert(out.end(), pcm.begin(), pcm.end());
    return out;
}

class VoiceService {
public:
    VoiceService(sdbus::IConnection &connection, Options options)
        : options_(std::move(options)),
          ai_(esca::ai::ServerConfig { options_.server, options_.timeout_ms, 2000 })
    {
        object_ = sdbus::createObject(connection, sdbus::ObjectPath { kObjectPath });

        object_->addVTable(
            sdbus::registerMethod("StartListening").implementedAs([this] {
                return start_listening();
            }),
            sdbus::registerMethod("StopListening").implementedAs([this] {
                return stop_listening();
            }),
            sdbus::registerMethod("Say").implementedAs(
                [this](const std::string &text) { return say(text); }),
            sdbus::registerMethod("GetState").implementedAs([this] { return state(); }),
            sdbus::registerMethod("GetErrorMessage").implementedAs([this] {
                std::lock_guard<std::mutex> lock(mutex_);
                return last_error_;
            }),

            // "preparing" -> "listening" -> "transcribing" -> "idle".
            // The application waits for "listening" before showing its
            // waveform, so it never invites someone to speak into a
            // microphone that is not open or a server that is not there.
            sdbus::registerSignal("StateChanged").withParameters<std::string>(),
            sdbus::registerSignal("Level").withParameters<double, double>(),
            sdbus::registerSignal("SpeechStarted").withParameters<uint64_t>(),
            sdbus::registerSignal("SpeechEnded").withParameters<uint64_t, double>(),
            sdbus::registerSignal("Transcript")
                .withParameters<uint64_t, std::string, std::string, double, double>(),
            sdbus::registerSignal("FaultEvent").withParameters<std::string, std::string>())
            .forInterface(kInterface);
    }

    ~VoiceService()
    {
        // Joined here and never inside a method handler: sdbus-c++ holds
        // the connection lock for the whole handler while the worker emits
        // signals on that same connection, and joining there deadlocks
        // both. The vision service had to be fixed for exactly this.
        running_ = false;
        if (worker_.joinable())
            worker_.join();
    }

    int32_t start_listening()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (running_)
            return 0;
        if (worker_alive_) {
            last_error_ = "the previous session has not finished yet";
            return -1;
        }
        if (worker_.joinable())
            worker_.join();

        running_ = true;
        worker_alive_ = true;
        worker_ = std::thread([this] {
            listen_once();
            worker_alive_ = false;
        });
        return 0;
    }

    int32_t stop_listening()
    {
        running_ = false;
        return 0;
    }

    /// Ask the server to speak, then play what comes back.
    int32_t say(const std::string &text)
    {
        if (text.empty()) {
            std::lock_guard<std::mutex> lock(mutex_);
            last_error_ = "nothing to say";
            return -1;
        }

        const std::string path = options_.tts_dir + "/esca-tts.wav";
        std::string error;
        if (!ai_.synthesise(text, path, error)) {
            std::lock_guard<std::mutex> lock(mutex_);
            last_error_ = error;
            emit_fault("tts", error);
            return -1;
        }

        esca::audio::AudioClient audio;
        const int32_t token = audio.play(path, error);
        if (token < 0) {
            std::lock_guard<std::mutex> lock(mutex_);
            last_error_ = error;
            return -1;
        }
        return token;
    }

    sdbus::Struct<bool, std::string, double> state()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return { running_.load(), phase_, level_db_.load() };
    }

private:
    void set_phase(const std::string &phase)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            phase_ = phase;
        }
        try {
            object_->emitSignal(sdbus::SignalName { "StateChanged" })
                .onInterface(sdbus::InterfaceName { kInterface })
                .withArguments(phase);
        } catch (const sdbus::Error &) {
        }
    }

    void emit_fault(const std::string &where, const std::string &what)
    {
        std::fprintf(stderr, "esca-voiced: %s: %s\n", where.c_str(), what.c_str());
        try {
            object_->emitSignal(sdbus::SignalName { "FaultEvent" })
                .onInterface(sdbus::InterfaceName { kInterface })
                .withArguments(where, what);
        } catch (const sdbus::Error &) {
        }
    }

    void fail(const std::string &where, const std::string &what)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            last_error_ = what;
        }
        emit_fault(where, what);
        set_phase("idle");
        running_ = false;
    }

    void listen_once()
    {
        using namespace esca::audio;
        set_phase("preparing");

        std::string error;

        // The server is checked first, while nobody is waiting to talk. A
        // dead link found now is a message; found after the sentence, it is
        // a sentence thrown away.
        if (!ai_.healthy(error)) {
            fail("server", "the AI server is not answering: " + error);
            return;
        }

        AudioClient audio;

        // Speak the prompt, and wait for the speaker to go quiet before
        // opening the microphone. Listening through our own voice is the
        // echo problem this system has no cancellation for; doing the two
        // in sequence sidesteps it entirely.
        if (!options_.prompt.empty()) {
            set_phase("prompting");
            const std::string path = options_.tts_dir + "/esca-prompt.wav";

            // Synthesised once and kept. A second press should not wait a
            // second for the same sentence to be made again.
            bool ready = prompt_cached_ == options_.prompt;
            if (!ready) {
                if (ai_.synthesise(options_.prompt, path, error)) {
                    prompt_cached_ = options_.prompt;
                    ready = true;
                } else {
                    // Not fatal. A missing voice should not stop someone
                    // from talking; they just lose the cue.
                    emit_fault("tts", "cannot speak the prompt: " + error);
                }
            }

            if (ready) {
                std::string play_error;
                if (audio.play(path, play_error) >= 0) {
                    while (running_ && audio.playing())
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                } else {
                    emit_fault("audio", "cannot play the prompt: " + play_error);
                }
            }
        }

        if (!running_) {
            set_phase("idle");
            return;
        }

        if (audio.open(options_.device, error) < 0) {
            fail("audio", "open: " + error);
            return;
        }
        if (!audio.start(error)) {
            fail("audio", "start: " + error);
            return;
        }

        const PcmFormat format = audio.stream_format();
        // No rate is demanded. Anomaly detection pins the microphone to
        // 44100 because its model does, and a USB microphone gives one
        // stream at one rate; the speech server resamples instead, which
        // costs nothing it was not already doing internally.
        const double chunk_seconds = format.chunk_seconds();

        // Reach back before the button press: by the time anyone noticed,
        // the first syllable has already gone past.
        esca::voice::VadConfig vad_config;
        const double recovered = audio.rewind(vad_config.preroll_ms / 1000.0);

        esca::voice::Vad vad(vad_config);
        std::vector<uint8_t> utterance;
        std::vector<uint8_t> chunk;
        ChunkInfo info;
        uint64_t first_seq = 0;
        bool announced_speech = false;
        auto last_level = std::chrono::steady_clock::now();

        set_phase("listening");
        std::fprintf(stderr, "esca-voiced: listening at %u Hz, %.0f ms of pre-roll\n",
                     format.rate, recovered * 1000.0);

        const auto listening_since = std::chrono::steady_clock::now();
        bool gave_up = false;

        while (running_) {
            if (!audio.next_chunk(chunk, info, 2000)) {
                fail("audio", "timed out waiting for audio");
                break;
            }
            if (first_seq == 0)
                first_seq = info.sequence;
            utterance.insert(utterance.end(), chunk.begin(), chunk.end());

            const double db = vad.push(reinterpret_cast<const int16_t *>(chunk.data()),
                                       chunk.size() / sizeof(int16_t), chunk_seconds);
            level_db_ = db;

            // The waveform wants a steady feed, not one message per chunk
            // per client; 20 Hz is smooth to the eye and quiet on the bus.
            const auto now = std::chrono::steady_clock::now();
            if (now - last_level >= std::chrono::milliseconds(50)) {
                last_level = now;
                try {
                    object_->emitSignal(sdbus::SignalName { "Level" })
                        .onInterface(sdbus::InterfaceName { kInterface })
                        .withArguments(db, vad.floor_db());
                } catch (const sdbus::Error &) {
                }
            }

            if (!announced_speech && !vad.waiting()) {
                announced_speech = true;
                try {
                    object_->emitSignal(sdbus::SignalName { "SpeechStarted" })
                        .onInterface(sdbus::InterfaceName { kInterface })
                        .withArguments(info.sequence);
                } catch (const sdbus::Error &) {
                }
            }

            if (vad.utterance_complete())
                break;

            // The wait only applies before anyone has spoken. Once a
            // sentence is under way the VAD owns the decision, and cutting
            // someone off mid-word to satisfy a stopwatch would be worse
            // than waiting.
            if (!announced_speech) {
                const double waited =
                    std::chrono::duration<double>(std::chrono::steady_clock::now()
                                                  - listening_since).count();
                if (waited >= options_.wait_seconds) {
                    gave_up = true;
                    break;
                }
            }
        }

        audio.stop();
        audio.close();

        if (gave_up) {
            char message[96];
            std::snprintf(message, sizeof(message), "no speech in %.0f seconds",
                          options_.wait_seconds);
            emit_fault("speech", message);
            set_phase("idle");
            running_ = false;
            return;
        }

        if (!running_ || !announced_speech) {
            // Stopped by hand. Not a fault.
            set_phase("idle");
            running_ = false;
            return;
        }

        const double seconds = vad.speech_seconds();
        try {
            object_->emitSignal(sdbus::SignalName { "SpeechEnded" })
                .onInterface(sdbus::InterfaceName { kInterface })
                .withArguments(info.sequence, seconds);
        } catch (const sdbus::Error &) {
        }

        set_phase("transcribing");
        const std::vector<uint8_t> wav = wrap_wav(format, utterance);

        esca::ai::Transcript transcript;
        if (!ai_.transcribe(wav.data(), wav.size(), transcript, error)) {
            fail("stt", error);
            return;
        }

        std::fprintf(stderr, "esca-voiced: \"%s\" (%s, %.0f ms)\n", transcript.text.c_str(),
                     transcript.language.c_str(), transcript.ms);

        try {
            object_->emitSignal(sdbus::SignalName { "Transcript" })
                .onInterface(sdbus::InterfaceName { kInterface })
                .withArguments(first_seq, transcript.text, transcript.language,
                               transcript.confidence, transcript.ms);
        } catch (const sdbus::Error &) {
        }

        set_phase("idle");
        running_ = false;
    }

    Options options_;
    esca::ai::AiClient ai_;
    std::unique_ptr<sdbus::IObject> object_;

    std::mutex mutex_;
    std::thread worker_;
    std::atomic<bool> running_ { false };
    std::atomic<bool> worker_alive_ { false };
    std::atomic<double> level_db_ { -90.0 };
    std::string phase_ = "idle";
    std::string prompt_cached_; ///< the prompt the cached wav holds
    std::string last_error_;
};

} // namespace

int main(int argc, char **argv)
{
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };

        if (arg == "--server") options.server = next();
        else if (arg == "--device") options.device = next();
        else if (arg == "--tts-dir") options.tts_dir = next();
        else if (arg == "--timeout") options.timeout_ms = std::stoi(next());
        else if (arg == "--prompt") options.prompt = next();
        else if (arg == "--wait") options.wait_seconds = std::stod(next());
        else return usage();
    }

    try {
        auto connection = sdbus::createSystemBusConnection(sdbus::ServiceName { kServiceName });
        g_connection = connection.get();

        std::signal(SIGINT, handle_signal);
        std::signal(SIGTERM, handle_signal);

        VoiceService service(*connection, options);
        std::fprintf(stderr, "esca-voiced: ready on %s, server %s\n", kServiceName,
                     options.server.c_str());
        connection->enterEventLoop();
        return 0;
    } catch (const std::exception &e) {
        std::fprintf(stderr, "esca-voiced: fatal: %s\n", e.what());
        return 1;
    }
}

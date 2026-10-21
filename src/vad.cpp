#include "vad.hpp"

#include <algorithm>
#include <cmath>

namespace esca::voice {

namespace {

double rms_db(const int16_t *pcm, size_t samples)
{
    if (!samples)
        return -90.0;

    double sum = 0.0;
    for (size_t i = 0; i < samples; ++i) {
        const double v = static_cast<double>(pcm[i]);
        sum += v * v;
    }
    const double rms = std::sqrt(sum / static_cast<double>(samples));
    if (rms < 1.0)
        return -90.0;
    return 20.0 * std::log10(rms / 32768.0);
}

} // namespace

Vad::Vad(VadConfig config) : config_(config) {}

void Vad::reset()
{
    state_ = State::Silence;
    started_ = false;
    complete_ = false;
    speech_s_ = 0.0;
    silence_s_ = 0.0;
    total_s_ = 0.0;
    // The noise floor is deliberately not reset. Between one utterance and
    // the next the room has not changed, and re-learning it from scratch
    // costs the first word of the next sentence.
}

double Vad::push(const int16_t *pcm, size_t samples, double seconds)
{
    level_db_ = rms_db(pcm, samples);
    total_s_ += seconds;

    if (!floor_seeded_) {
        floor_db_ = level_db_;
        floor_seeded_ = true;
    }

    const bool loud = level_db_ > floor_db_ + config_.speech_margin_db;

    // Track the background only while nothing is being said, and let it
    // fall faster than it rises: a machine switching off should be followed
    // quickly, while a long sentence must not be absorbed into the floor
    // and silence the speaker mid-word.
    if (!loud) {
        const double rate =
            level_db_ > floor_db_ ? config_.floor_rise_per_s : config_.floor_fall_per_s;
        const double step = rate * seconds;
        floor_db_ += std::clamp(level_db_ - floor_db_, -step, step);
    }

    if (loud) {
        state_ = State::Speech;
        // Every return of speech starts the hangover over. Clearing it only
        // on leaving Silence let the short pauses of one sentence add up,
        // so a few breaths -- each well under the hangover -- ended it.
        silence_s_ = 0.0;
        speech_s_ += seconds;
        if (speech_s_ >= config_.min_speech_ms / 1000.0)
            started_ = true;
    } else if (state_ == State::Speech) {
        silence_s_ += seconds;
        if (silence_s_ >= config_.hangover_ms / 1000.0) {
            state_ = State::Silence;
            // Only a long enough burst counts. A door slam reaches here
            // with speech_s_ below the minimum and is discarded rather than
            // sent to the server as an utterance.
            if (started_)
                complete_ = true;
            else
                speech_s_ = 0.0;
        }
    }

    if (started_ && speech_s_ >= config_.max_utterance_ms / 1000.0)
        complete_ = true;

    return level_db_;
}

} // namespace esca::voice

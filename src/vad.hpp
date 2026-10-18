// Deciding when someone started and stopped talking.
//
// This, not the model, is what makes speech recognition usable. Cut too
// early and the last word is gone; cut too late and several seconds of
// silence go to the server and come back as nothing. Both failures look to
// the user like the thing simply does not work.
//
// Energy alone is not enough here. The whole system exists to listen to
// machinery, so there is always a loud, steady noise floor; a fixed
// threshold either never triggers or never stops. What distinguishes
// speech from a running gear is that speech *changes* -- so the detector
// tracks the background level and looks for sustained excursions above it.

#pragma once

#include <cstdint>
#include <deque>
#include <vector>

namespace esca::voice {

struct VadConfig {
    /// How far above the running noise floor counts as speech, in dB.
    double speech_margin_db = 9.0;
    /// Silence that ends an utterance. Shorter clips words off the end of a
    /// sentence; longer makes the system feel slow to answer.
    double hangover_ms = 600.0;
    /// Speech must last this long to count, so a door slam is not a word.
    double min_speech_ms = 200.0;
    /// Nothing is transcribed beyond this, however long someone talks.
    double max_utterance_ms = 10000.0;
    /// How much audio before the trigger to keep. The first syllable is
    /// always already past by the time anything notices.
    double preroll_ms = 500.0;
    /// Time constant for the noise floor. Slow, so a long sentence does not
    /// get absorbed into the background.
    double floor_rise_per_s = 3.0;
    double floor_fall_per_s = 12.0;
};

/// Fed one chunk at a time, in order.
class Vad {
public:
    enum class State { Silence, Speech };

    explicit Vad(VadConfig config = {});

    /// @param pcm      interleaved int16
    /// @param samples  total samples, not frames
    /// @param seconds  how much time this chunk covers
    /// @returns the level of this chunk in dBFS, for a meter
    double push(const int16_t *pcm, size_t samples, double seconds);

    State state() const { return state_; }
    /// True once a complete utterance has been seen: speech long enough,
    /// then silence long enough.
    bool utterance_complete() const { return complete_; }
    /// True while the detector is still waiting for anyone to speak.
    bool waiting() const { return !started_; }

    double speech_seconds() const { return speech_s_; }
    double level_db() const { return level_db_; }
    double floor_db() const { return floor_db_; }

    void reset();

private:
    VadConfig config_;
    State state_ = State::Silence;
    bool started_ = false;
    bool complete_ = false;

    double level_db_ = -90.0;
    double floor_db_ = -90.0;
    double speech_s_ = 0.0;
    double silence_s_ = 0.0;
    double total_s_ = 0.0;
    bool floor_seeded_ = false;
};

} // namespace esca::voice

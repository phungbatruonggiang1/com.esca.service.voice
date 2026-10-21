# ESCA Voice Service

Speech to text, for voice control. One of the two features that sit on the
audio framework; anomalous sound detection is the other, and they are
deliberately separate processes with separate repositories, because they
are separate features with separate lifetimes and more will follow.

    App --StartListening()--> esca-voiced --► libesca-aiclient --► AI server
                                   ▲  │
                        esca-audiod┘  └─► Transcript on D-Bus --► App

## The button, and why the handshake matters

The application presses a button, this opens the microphone, and **only
once everything is genuinely ready does it say so**. The application waits
for that before switching to its waveform and inviting someone to speak.

The server is checked during "preparing", while nobody is waiting to talk.
A dead link found then is a message on screen; found afterwards, it is a
sentence spoken into nothing and a silence where the answer should be.

```
StateChanged("preparing")      checking the server, opening the mic
StateChanged("listening")      now show the waveform
Level(rms_db, floor_db)        20 Hz, drives it
SpeechStarted(seq)
SpeechEnded(seq, seconds)
StateChanged("transcribing")
Transcript(seq, text, language, confidence, ms)
```

## Endpointing is the hard part, not the model

Calling Whisper is easy. What decides whether this is usable is knowing
when someone started and stopped talking: cut early and the last word is
gone, cut late and four seconds of silence go to the server and come back
empty. Both look to a user like the thing does not work.

**Energy alone will not do it here.** This system exists alongside one that
listens to machinery, so there is always a loud, steady noise floor; a
fixed threshold either never triggers or never releases. `vad.cpp` tracks
the background and looks for sustained excursions above it, letting the
floor fall faster than it rises so a long sentence is not absorbed into it
and the speaker silenced mid-word.

Defaults: 9 dB above the floor is speech, 1 s of silence ends an
utterance, bursts under 200 ms are discarded as door slams, and nothing
runs past 15 s of speech -- the pauses inside a sentence do not count.
`--hangover-ms` and `--max-seconds` change the last two; on the board they
go in `ESCA_VOICE_OPTS` in `/etc/esca/voice.conf`.

**Each return of speech restarts the hangover.** It used to restart only
when the detector had already fallen back to silence, so the short pauses
inside one sentence added up: a few breaths, each well under the hangover,
ended the sentence early. With 600 ms that was two or three breaths, which
is why anything longer than a short command was cut off.

**Pre-roll comes free.** By the time a button press is noticed the first
syllable has gone past, so `AudioClient::rewind()` reaches 500 ms back into
the ring the audio service is already keeping, rather than this process
buffering the same audio a second time.

## The board does not resample

Anomaly detection pins the microphone to 44100 because its model demands
exactly that, and a USB microphone offers one stream at one rate. Whisper
wants 16 kHz. Rather than carry a resampler for a ratio that is not even
integer (160/441), the server accepts any rate and resamples — which every
speech engine does internally anyway.

## Spoken replies

`Say(text)` fetches WAV from the server and hands the path to
`esca-audiod`, which already queues playback. WAV, not mp3: the board has
no decoder and adding one to speak a sentence is not worth it.

**This is where echo cancellation stops being optional.** A speaker playing
into the room the microphone is listening to means the system hears itself.
The cheap first answer is half duplex — mute capture while playing, losing
the ability to interrupt. `module-echo-cancel` in PulseAudio is the real
one, and it slots in between `esca-audiod` and ALSA without this service
changing.

## Stop() does not join the worker

It only lowers a flag. sdbus-c++ holds the connection lock for the whole of
a method handler while the worker emits signals on that same connection, so
joining inside the handler deadlocks both threads and the service never
answers again. The join lives in the destructor, where the event loop has
already left.

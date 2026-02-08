#ifndef __GBA_SYNTH_ENGINE_H__
#define __GBA_SYNTH_ENGINE_H__

#include "Midi/Players/GBA/VoicegroupParser.h"
#include <cstdint>
#include <cmath>
#include <wx/thread.h>

namespace AriaMaestosa
{

static const int MAX_ACTIVE_VOICES = 24;

struct ActiveVoice
{
    bool active;
    int note;
    int velocity;
    int channel;
    int programIndex;
    const GBAVoice* voice;

    // Sample playback
    double samplePos;
    double sampleStep;

    // ADSR (GBA-accurate frame-based envelope)
    enum Phase { ATTACK, DECAY, SUSTAIN, RELEASE, OFF };
    Phase phase;
    int envelopeVolume;        // 0-255 for direct sound, 0-15 for CGB
    double frameSampleCounter;  // accumulates samples for ~60Hz envelope stepping
    bool isCgbVoice;           // CGB voices use counter-based envelope
    int envelopeCounter;       // CGB: frame counter for envelope steps
    int envelopeGoal;          // CGB: max envelope level (15)
    int sustainGoal;           // CGB: calculated sustain target

    // Pan
    float panL, panR;

    // Square wave
    double squarePhase;
    double squarePhaseInc;

    // Noise
    uint16_t lfsr;
    double noiseTimer;
    double noiseInterval;
    int8_t noiseOutput;

    // Pitch bend (in semitones, fractional)
    float pitchBend;

    ActiveVoice() : active(false), note(0), velocity(0), channel(0), programIndex(0),
                    voice(NULL), samplePos(0), sampleStep(0), phase(OFF),
                    envelopeVolume(0), frameSampleCounter(0), isCgbVoice(false),
                    envelopeCounter(0), envelopeGoal(15), sustainGoal(0),
                    panL(0.5f), panR(0.5f), squarePhase(0), squarePhaseInc(0),
                    lfsr(0x7FFF), noiseTimer(0), noiseInterval(0), noiseOutput(0),
                    pitchBend(0) {}
};

class GBASynthEngine
{
    ActiveVoice m_voices[MAX_ACTIVE_VOICES];
    float m_channelVolume[16];
    float m_channelPan[16];
    float m_channelPitchBend[16];
    int m_channelPitchBendRange[16];
    int m_sampleRate;
    wxMutex m_mutex;

    int findFreeVoice();
    void computeEnvelopeStep(ActiveVoice& v);
    float renderDirectSound(ActiveVoice& v);
    float renderSquareWave(ActiveVoice& v);
    float renderNoise(ActiveVoice& v);
    float renderProgWave(ActiveVoice& v);

public:
    GBASynthEngine();

    void noteOn(int note, int velocity, int channel, const GBAVoice* voice);
    void noteOff(int note, int channel);
    void allNotesOff(int channel);
    void controlChange(int controller, int value, int channel);
    void pitchBend(int value, int channel);

    void renderFrames(float* output, int frameCount);

    void setSampleRate(int rate);
    int getSampleRate() const { return m_sampleRate; }

    void reset();
};

}

#endif

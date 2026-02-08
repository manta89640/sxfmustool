#ifndef __GBA_SYNTH_ENGINE_H__
#define __GBA_SYNTH_ENGINE_H__

#include "Midi/Players/GBA/VoicegroupParser.h"
#include <cstdint>
#include <cmath>
#include <wx/thread.h>

namespace AriaMaestosa
{

static const int MAX_ACTIVE_VOICES = 24;

struct ChannelModState
{
    uint8_t mod;           // CC1: modulation depth
    uint8_t lfoSpeed;      // CC21: LFO speed
    uint8_t lfoSpeedC;     // LFO phase counter (uint8 wraps at 256)
    uint8_t modT;          // CC22: mod type (0=vibrato, 1=tremolo, 2=auto-pan)
    int8_t tune;           // CC24: fine tune (-64 to +63)
    uint8_t lfoDelay;      // CC26: LFO delay setting
    uint8_t lfoDelayC;     // LFO delay countdown
    int8_t modM;           // computed modulation value (s8, matches GBA)
    uint8_t xcmdType;      // CC30: pending extended command type
    uint8_t pseudoEchoVol; // XCMD 8: pseudo-echo volume (per-track default)
    uint8_t pseudoEchoLen; // XCMD 9: pseudo-echo length (per-track default)

    ChannelModState() : mod(0), lfoSpeed(0), lfoSpeedC(0), modT(0), tune(0),
                        lfoDelay(0), lfoDelayC(0), modM(0), xcmdType(0),
                        pseudoEchoVol(0), pseudoEchoLen(0) {}
};

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
    enum Phase { ATTACK, DECAY, SUSTAIN, RELEASE, ECHO, OFF };
    Phase phase;
    int envelopeVolume;        // 0-255 for direct sound, 0-15 for CGB
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
    bool noiseWidth7Bit;

    // Pitch bend (in semitones, fractional)
    float pitchBend;

    // Rhythm (drum) voice — pitch locked to baseMidiKey, no pitch bend
    bool isRhythm;

    // Pseudo-echo (copied from channel on noteOn)
    uint8_t pseudoEchoVol;
    uint8_t pseudoEchoLen;

    // Monotonically increasing counter for deterministic voice stealing
    int triggerOrder;

    ActiveVoice() : active(false), note(0), velocity(0), channel(0), programIndex(0),
                    voice(NULL), samplePos(0), sampleStep(0), phase(OFF),
                    envelopeVolume(0), isCgbVoice(false),
                    envelopeCounter(0), envelopeGoal(15), sustainGoal(0),
                    panL(0.5f), panR(0.5f), squarePhase(0), squarePhaseInc(0),
                    lfsr(0x7FFF), noiseTimer(0), noiseInterval(0), noiseOutput(0),
                    noiseWidth7Bit(false), pitchBend(0), isRhythm(false),
                    pseudoEchoVol(0), pseudoEchoLen(0), triggerOrder(0) {}
};

class GBASynthEngine
{
    ActiveVoice m_voices[MAX_ACTIVE_VOICES];
    ChannelModState m_channelMod[16];
    float m_channelVolume[16];
    float m_channelPan[16];
    float m_channelPitchBend[16];
    int m_channelPitchBendRange[16];
    int m_sampleRate;
    int m_nextTriggerOrder;
    double m_globalFrameCounter;
    wxMutex m_mutex;

    int findFreeVoice();
    void computeEnvelopeStep(ActiveVoice& v);
    void updateLFO(int channel);
    void updateVoicePitch(ActiveVoice& v);
    float renderDirectSound(ActiveVoice& v);
    float renderSquareWave(ActiveVoice& v);
    float renderNoise(ActiveVoice& v);
    float renderProgWave(ActiveVoice& v);

public:
    GBASynthEngine();

    void noteOn(int note, int velocity, int channel, const GBAVoice* voice, bool isRhythm = false);
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

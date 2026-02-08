#include "Midi/Players/GBA/GBASynthEngine.h"
#include <cstring>
#include <cmath>
#include <cstdio>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace AriaMaestosa
{

static float midiNoteToFreq(int note)
{
    return 440.0f * powf(2.0f, (note - 69) / 12.0f);
}

// GBA CGB frequency lookup tables (from m4a_tables.c)
// 132 entries: 11 octaves x 12 semitones. High nibble = shift, low nibble = table index.
static const uint8_t gCgbScaleTable[] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B,
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B,
    0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2A, 0x2B,
    0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A, 0x3B,
    0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4A, 0x4B,
    0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5A, 0x5B,
    0x60, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6A, 0x6B,
    0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7A, 0x7B,
    0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8A, 0x8B,
    0x90, 0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9A, 0x9B,
    0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0xAB,
};

static const int16_t gCgbFreqTable[] = {
    -2004, -1891, -1785, -1685, -1591, -1501,
    -1417, -1337, -1262, -1192, -1125, -1062,
};

static const uint8_t gNoiseTable[] = {
    0xD7, 0xD6, 0xD5, 0xD4, 0xC7, 0xC6, 0xC5, 0xC4,
    0xB7, 0xB6, 0xB5, 0xB4, 0xA7, 0xA6, 0xA5, 0xA4,
    0x97, 0x96, 0x95, 0x94, 0x87, 0x86, 0x85, 0x84,
    0x77, 0x76, 0x75, 0x74, 0x67, 0x66, 0x65, 0x64,
    0x57, 0x56, 0x55, 0x54, 0x47, 0x46, 0x45, 0x44,
    0x37, 0x36, 0x35, 0x34, 0x27, 0x26, 0x25, 0x24,
    0x17, 0x16, 0x15, 0x14, 0x07, 0x06, 0x05, 0x04,
    0x03, 0x02, 0x01, 0x00,
};

// Matches MidiKeyToCgbFreq from m4a.c for channels 1-3 (square/wave).
// Returns hardware frequency register value (11-bit, with +2048 offset).
static int cgbMidiKeyToReg(int key, int fineAdjust)
{
    if (key <= 35) { fineAdjust = 0; key = 0; }
    else {
        key -= 36;
        if (key > 130) { key = 130; fineAdjust = 255; }
    }
    int32_t val1 = gCgbScaleTable[key];
    val1 = gCgbFreqTable[val1 & 0xF] >> (val1 >> 4);
    int32_t val2 = gCgbScaleTable[key + 1];
    val2 = gCgbFreqTable[val2 & 0xF] >> (val2 >> 4);
    return val1 + ((fineAdjust * (val2 - val1)) >> 8) + 2048;
}

// Square wave: hardware frequency = 131072 / (2048 - reg) Hz
static double cgbSquareRegToHz(int reg)
{
    int denom = 2048 - reg;
    if (denom <= 0) return 131072.0;
    return 131072.0 / (double)denom;
}

// Wave channel: hardware frequency = 65536 / (2048 - reg) Hz (half of square)
static double cgbWaveRegToHz(int reg)
{
    int denom = 2048 - reg;
    if (denom <= 0) return 65536.0;
    return 65536.0 / (double)denom;
}

// Noise: decode NR43 register to clock frequency
// bits 7-4 = shift clock (s), bits 2-0 = dividing ratio (r), bit 3 = LFSR width (ignored here)
static double noiseNR43ToHz(uint8_t nr43)
{
    int shift = (nr43 >> 4) & 0xF;
    int ratio = nr43 & 0x7;
    double r = (ratio == 0) ? 0.5 : (double)ratio;
    return 524288.0 / (r * (double)(1 << (shift + 1)));
}

// Look up noise frequency from MIDI key (matches MidiKeyToCgbFreq for channel 4)
static double noiseKeyToHz(int key)
{
    if (key <= 20) key = 0;
    else {
        key -= 21;
        if (key > 59) key = 59;
    }
    return noiseNR43ToHz(gNoiseTable[key]);
}

GBASynthEngine::GBASynthEngine() : m_sampleRate(13379), m_nextTriggerOrder(0), m_globalFrameCounter(0)
{
    reset();
}

void GBASynthEngine::setSampleRate(int rate)
{
    wxMutexLocker lock(m_mutex);
    m_sampleRate = rate;
}

void GBASynthEngine::reset()
{
    wxMutexLocker lock(m_mutex);
    for (int i = 0; i < MAX_ACTIVE_VOICES; i++)
    {
        m_voices[i].active = false;
        m_voices[i].phase = ActiveVoice::OFF;
        m_voices[i].triggerOrder = 0;
    }
    m_nextTriggerOrder = 0;
    m_globalFrameCounter = 0;
    for (int i = 0; i < 16; i++)
    {
        m_channelVolume[i] = 1.0f;
        m_channelPan[i] = 0.5f;
        m_channelPitchBend[i] = 0.0f;
        m_channelPitchBendRange[i] = 2;
        m_channelMod[i] = ChannelModState();
    }
}

int GBASynthEngine::findFreeVoice()
{
    // 1. Find inactive voice
    for (int i = 0; i < MAX_ACTIVE_VOICES; i++)
    {
        if (!m_voices[i].active) return i;
    }

    // 2. Steal echo-phase voice first (lowest priority), then release-phase.
    int bestEcho = -1;
    int bestEchoVol = 9999;
    for (int i = 0; i < MAX_ACTIVE_VOICES; i++)
    {
        if (m_voices[i].phase == ActiveVoice::ECHO && m_voices[i].envelopeVolume < bestEchoVol)
        {
            bestEchoVol = m_voices[i].envelopeVolume;
            bestEcho = i;
        }
    }
    if (bestEcho >= 0) return bestEcho;

    int bestRelease = -1;
    int bestReleaseVol = 9999;
    for (int i = 0; i < MAX_ACTIVE_VOICES; i++)
    {
        if (m_voices[i].phase == ActiveVoice::RELEASE && m_voices[i].envelopeVolume < bestReleaseVol)
        {
            bestReleaseVol = m_voices[i].envelopeVolume;
            bestRelease = i;
        }
    }
    if (bestRelease >= 0) return bestRelease;

    // 3. No release voices available. Steal the non-ATTACK voice with the lowest
    //    envelope volume. ATTACK voices are protected to prevent simultaneous
    //    notes (especially drums) from stealing each other before producing audio.
    int lowest = -1;
    int lowestVol = 9999;
    for (int i = 0; i < MAX_ACTIVE_VOICES; i++)
    {
        if (m_voices[i].phase == ActiveVoice::ATTACK) continue;
        if (m_voices[i].envelopeVolume < lowestVol)
        {
            lowestVol = m_voices[i].envelopeVolume;
            lowest = i;
        }
    }
    if (lowest >= 0) return lowest;

    // 4. All voices in ATTACK (many simultaneous noteOn before any rendering).
    //    Steal the OLDEST triggered voice (lowest triggerOrder) for deterministic
    //    behavior regardless of audio callback timing.
    int oldest = 0;
    int oldestOrder = m_voices[0].triggerOrder;
    for (int i = 1; i < MAX_ACTIVE_VOICES; i++)
    {
        if (m_voices[i].triggerOrder < oldestOrder)
        {
            oldestOrder = m_voices[i].triggerOrder;
            oldest = i;
        }
    }
    return oldest;
}

void GBASynthEngine::noteOn(int note, int velocity, int channel, const GBAVoice* voice, bool isRhythm)
{
    if (!voice || voice->type == GBAVoice::EMPTY) return;
    if (channel < 0 || channel > 15) return;

    wxMutexLocker lock(m_mutex);

    // Kill any existing voice playing this same note+channel (GBA re-trigger behavior)
    for (int i = 0; i < MAX_ACTIVE_VOICES; i++)
    {
        if (m_voices[i].active && m_voices[i].note == note && m_voices[i].channel == channel)
        {
            m_voices[i].active = false;
            m_voices[i].phase = ActiveVoice::OFF;
        }
    }

    int idx = findFreeVoice();
    ActiveVoice& v = m_voices[idx];

    v.active = true;
    v.note = note;
    v.velocity = velocity;
    v.channel = channel;
    v.voice = voice;
    v.pitchBend = m_channelPitchBend[channel];
    v.isRhythm = isRhythm;
    v.pseudoEchoVol = m_channelMod[channel].pseudoEchoVol;
    v.pseudoEchoLen = m_channelMod[channel].pseudoEchoLen;
    v.triggerOrder = m_nextTriggerOrder++;

    // Determine if CGB voice type (counter-based envelope) vs direct sound (additive/multiplicative)
    v.isCgbVoice = (voice->type == GBAVoice::SQUARE_1 || voice->type == GBAVoice::SQUARE_2
                 || voice->type == GBAVoice::NOISE || voice->type == GBAVoice::PROG_WAVE);

    if (v.isCgbVoice)
    {
        // CGB envelope: counter-based, values masked by macros (attack&0x7, decay&0x7, sustain&0xF, release&0x7)
        v.envelopeGoal = 15;
        v.sustainGoal = (v.envelopeGoal * voice->sustain + 15) >> 4;

        if (voice->attack == 0)
        {
            // Instant attack: skip to decay phase at max volume
            v.envelopeVolume = v.envelopeGoal;
            v.phase = ActiveVoice::DECAY;
            v.envelopeCounter = voice->decay;
        }
        else
        {
            v.envelopeVolume = 0;
            v.phase = ActiveVoice::ATTACK;
            v.envelopeCounter = voice->attack;
        }
    }
    else
    {
        // Direct sound envelope: additive attack, multiplicative decay/release
        v.envelopeVolume = 0;
        v.phase = ActiveVoice::ATTACK;
        // First envelope step happens immediately
    }

    // Pan
    float pan = 0.5f;
    if (voice->pan != 0)
    {
        pan = (float)voice->pan / 127.0f;
    }
    float chPan = m_channelPan[channel];
    pan = (pan + chPan) * 0.5f;
    v.panL = cosf(pan * (float)M_PI * 0.5f);
    v.panR = sinf(pan * (float)M_PI * 0.5f);

    // For rhythm (drum) voices, pitch is locked to the sub-voice's baseMidiKey
    // (GBA hardware uses the resolved voice's key, not the MIDI note, for drums)
    int pitchKey = isRhythm ? voice->baseMidiKey : note;

    if (voice->type == GBAVoice::DIRECT_SOUND)
    {
        float targetFreq = midiNoteToFreq(pitchKey);
        float baseFreq = midiNoteToFreq(voice->baseMidiKey);
        v.samplePos = 0.0;
        if (voice->sample && voice->sample->sampleRate > 0)
        {
            v.sampleStep = (targetFreq / baseFreq) * ((double)voice->sample->sampleRate / (double)m_sampleRate);
        }
        else
        {
            v.sampleStep = 1.0;
        }
    }
    else if (voice->type == GBAVoice::PROG_WAVE)
    {
        // CGB wave channel: uses MidiKeyToCgbFreq tables, plays at 65536/(2048-reg) Hz
        int reg = cgbMidiKeyToReg(pitchKey, 0);
        double freq = cgbWaveRegToHz(reg);
        v.samplePos = 0.0;
        int numSamples = (voice->sample && !voice->sample->pcmData.empty())
                         ? (int)voice->sample->pcmData.size() : 32;
        v.sampleStep = freq * (double)numSamples / (double)m_sampleRate;
    }
    else if (voice->type == GBAVoice::SQUARE_1 || voice->type == GBAVoice::SQUARE_2)
    {
        // CGB square channels: uses MidiKeyToCgbFreq tables, plays at 131072/(2048-reg) Hz
        int reg = cgbMidiKeyToReg(pitchKey, 0);
        double freq = cgbSquareRegToHz(reg);
        v.squarePhase = 0.0;
        v.squarePhaseInc = freq / (double)m_sampleRate;
    }
    else if (voice->type == GBAVoice::NOISE)
    {
        // CGB noise channel: uses gNoiseTable lookup for frequency, voice->period controls LFSR width
        double noiseFreq = noiseKeyToHz(pitchKey);
        v.lfsr = 0x7FFF;
        v.noiseTimer = 0.0;
        v.noiseInterval = (double)m_sampleRate / noiseFreq;
        v.noiseOutput = 0;
        v.noiseWidth7Bit = (voice->period != 0);
    }

    // Run one immediate envelope step, matching GBA's VBlank-synchronized behavior.
    // On GBA, event processing and envelope stepping happen in the same frame
    // (events first, then SoundMainRAM advances envelopes). Without this, short
    // notes (especially drums) can have noteOff arrive before any renderFrames()
    // advances the envelope past 0 — causing the multiplicative release to compute
    // (0 * release) >> 8 = 0, killing the voice silently.
    if (v.phase == ActiveVoice::ATTACK)
        computeEnvelopeStep(v);
}

void GBASynthEngine::noteOff(int note, int channel)
{
    wxMutexLocker lock(m_mutex);
    for (int i = 0; i < MAX_ACTIVE_VOICES; i++)
    {
        if (m_voices[i].active && m_voices[i].note == note && m_voices[i].channel == channel
            && m_voices[i].phase != ActiveVoice::RELEASE && m_voices[i].phase != ActiveVoice::ECHO
            && m_voices[i].phase != ActiveVoice::OFF)
        {
            m_voices[i].phase = ActiveVoice::RELEASE;
            if (m_voices[i].isCgbVoice)
            {
                m_voices[i].envelopeCounter = m_voices[i].voice->release;
            }
        }
    }
}

void GBASynthEngine::allNotesOff(int channel)
{
    wxMutexLocker lock(m_mutex);
    for (int i = 0; i < MAX_ACTIVE_VOICES; i++)
    {
        if (m_voices[i].active && m_voices[i].channel == channel)
        {
            m_voices[i].active = false;
            m_voices[i].phase = ActiveVoice::OFF;
        }
    }
}

void GBASynthEngine::controlChange(int controller, int value, int channel)
{
    if (channel < 0 || channel > 15) return;

    wxMutexLocker lock(m_mutex);
    switch (controller)
    {
        case 1: // MOD — modulation depth
            m_channelMod[channel].mod = (uint8_t)value;
            if (value == 0)
            {
                m_channelMod[channel].modM = 0;
                m_channelMod[channel].lfoSpeedC = 0;
                m_channelMod[channel].lfoDelayC = m_channelMod[channel].lfoDelay;
            }
            break;
        case 7: // Volume
            m_channelVolume[channel] = (float)value / 127.0f;
            break;
        case 10: // Pan
            m_channelPan[channel] = (float)value / 127.0f;
            break;
        case 6: // Data Entry MSB (for RPN pitch bend range)
            m_channelPitchBendRange[channel] = value;
            break;
        case 21: // LFOS — LFO speed
            m_channelMod[channel].lfoSpeed = (uint8_t)value;
            if (value == 0)
            {
                m_channelMod[channel].modM = 0;
                m_channelMod[channel].lfoSpeedC = 0;
                m_channelMod[channel].lfoDelayC = m_channelMod[channel].lfoDelay;
            }
            break;
        case 22: // MODT — mod type (0=vibrato, 1=tremolo, 2=auto-pan)
            m_channelMod[channel].modT = (uint8_t)value;
            break;
        case 24: // TUNE — fine tuning (value-64 = signed)
            m_channelMod[channel].tune = (int8_t)(value - 64);
            break;
        case 26: // LFODL — LFO delay in frames
            m_channelMod[channel].lfoDelay = (uint8_t)value;
            m_channelMod[channel].lfoDelayC = (uint8_t)value;
            break;
        case 29: // XCMD — execute extended command
        {
            uint8_t type = m_channelMod[channel].xcmdType;
            if (type == 8) m_channelMod[channel].pseudoEchoVol = (uint8_t)value;
            if (type == 9) m_channelMod[channel].pseudoEchoLen = (uint8_t)value;
            break;
        }
        case 30: // XCMD_TYPE — extended command type selector
            m_channelMod[channel].xcmdType = (uint8_t)value;
            break;
        case 123: // All notes off
            allNotesOff(channel);
            break;
    }
}

// Exact GBA triangle wave LFO algorithm from m4a_1.s MPlayMain
void GBASynthEngine::updateLFO(int ch)
{
    ChannelModState& m = m_channelMod[ch];
    if (m.lfoSpeed == 0 || m.mod == 0)
    {
        m.modM = 0;
        return;
    }
    if (m.lfoDelayC > 0)
    {
        m.lfoDelayC--;
        return;
    }
    m.lfoSpeedC += m.lfoSpeed; // uint8 wraps at 256
    int wave;
    if (m.lfoSpeedC < 64)
        wave = (int)(int8_t)m.lfoSpeedC;   // 0→63, rising
    else
        wave = 128 - (int)m.lfoSpeedC;     // 64→-127, falling
    m.modM = (int8_t)(((int)m.mod * wave) >> 6);
}

// Recalculate voice frequency from base note + bend + tune + vibrato
void GBASynthEngine::updateVoicePitch(ActiveVoice& v)
{
    if (!v.active || !v.voice) return;
    if (v.isRhythm) return; // GBA ignores pitch mod on drums

    // Compute total pitch offset in 256ths of semitone (matching m4a.c TrkVolPitSet)
    int tuneX = (int)m_channelMod[v.channel].tune * 4;
    float bendSemi = v.pitchBend;
    int bendX = (int)(bendSemi * 256.0f);
    int vibratoX = 0;
    if (m_channelMod[v.channel].modT == 0) // vibrato
        vibratoX = 16 * (int)m_channelMod[v.channel].modM;
    int totalX = bendX + tuneX + vibratoX;
    float totalSemi = (float)totalX / 256.0f;

    int pitchKey = v.note;

    if (v.voice->type == GBAVoice::DIRECT_SOUND)
    {
        float targetFreq = midiNoteToFreq(pitchKey) * powf(2.0f, totalSemi / 12.0f);
        float baseFreq = midiNoteToFreq(v.voice->baseMidiKey);
        if (v.voice->sample && v.voice->sample->sampleRate > 0)
        {
            v.sampleStep = (targetFreq / baseFreq) *
                ((double)v.voice->sample->sampleRate / (double)m_sampleRate);
        }
    }
    else if (v.voice->type == GBAVoice::SQUARE_1 || v.voice->type == GBAVoice::SQUARE_2)
    {
        int intSemi = (int)floorf(totalSemi);
        int fineAdjust = (int)((totalSemi - (float)intSemi) * 256.0f);
        if (fineAdjust < 0) { intSemi--; fineAdjust += 256; }
        if (fineAdjust > 255) fineAdjust = 255;
        int reg = cgbMidiKeyToReg(pitchKey + intSemi, fineAdjust);
        v.squarePhaseInc = cgbSquareRegToHz(reg) / (double)m_sampleRate;
    }
    else if (v.voice->type == GBAVoice::PROG_WAVE)
    {
        int intSemi = (int)floorf(totalSemi);
        int fineAdjust = (int)((totalSemi - (float)intSemi) * 256.0f);
        if (fineAdjust < 0) { intSemi--; fineAdjust += 256; }
        if (fineAdjust > 255) fineAdjust = 255;
        int reg = cgbMidiKeyToReg(pitchKey + intSemi, fineAdjust);
        double freq = cgbWaveRegToHz(reg);
        int numSamples = (v.voice->sample && !v.voice->sample->pcmData.empty())
                         ? (int)v.voice->sample->pcmData.size() : 32;
        v.sampleStep = freq * (double)numSamples / (double)m_sampleRate;
    }
    else if (v.voice->type == GBAVoice::NOISE)
    {
        int intSemi = (int)floorf(totalSemi);
        double noiseFreq = noiseKeyToHz(pitchKey + intSemi);
        v.noiseInterval = (double)m_sampleRate / noiseFreq;
    }
}

void GBASynthEngine::pitchBend(int value, int channel)
{
    if (channel < 0 || channel > 15) return;

    wxMutexLocker lock(m_mutex);
    float semitones = ((float)value / 8192.0f) * (float)m_channelPitchBendRange[channel];
    m_channelPitchBend[channel] = semitones;

    for (int i = 0; i < MAX_ACTIVE_VOICES; i++)
    {
        if (m_voices[i].active && m_voices[i].channel == channel)
        {
            m_voices[i].pitchBend = semitones;
            updateVoicePitch(m_voices[i]);
        }
    }
}

// GBA-accurate envelope stepping, called once per GBA frame (~60Hz)
void GBASynthEngine::computeEnvelopeStep(ActiveVoice& v)
{
    if (v.phase == ActiveVoice::OFF) return;

    if (v.isCgbVoice)
    {
        // CGB counter-based envelope (square, noise, prog wave)
        switch (v.phase)
        {
            case ActiveVoice::ATTACK:
                if (v.voice->attack == 0)
                {
                    // Instant attack
                    v.envelopeVolume = v.envelopeGoal;
                    v.phase = ActiveVoice::DECAY;
                    v.envelopeCounter = v.voice->decay;
                }
                else
                {
                    v.envelopeCounter--;
                    if (v.envelopeCounter <= 0)
                    {
                        v.envelopeVolume++;
                        if (v.envelopeVolume >= v.envelopeGoal)
                        {
                            v.envelopeVolume = v.envelopeGoal;
                            v.phase = ActiveVoice::DECAY;
                            v.envelopeCounter = v.voice->decay;
                        }
                        else
                        {
                            v.envelopeCounter = v.voice->attack;
                        }
                    }
                }
                break;

            case ActiveVoice::DECAY:
                if (v.voice->decay == 0)
                {
                    // Instant decay to sustain
                    if (v.voice->sustain == 0)
                    {
                        v.envelopeVolume = 0;
                        v.phase = ActiveVoice::OFF;
                        v.active = false;
                    }
                    else
                    {
                        v.envelopeVolume = v.sustainGoal;
                        v.phase = ActiveVoice::SUSTAIN;
                    }
                }
                else
                {
                    v.envelopeCounter--;
                    if (v.envelopeCounter <= 0)
                    {
                        v.envelopeVolume--;
                        if (v.envelopeVolume <= v.sustainGoal)
                        {
                            if (v.voice->sustain == 0)
                            {
                                v.envelopeVolume = 0;
                                v.phase = ActiveVoice::OFF;
                                v.active = false;
                            }
                            else
                            {
                                v.envelopeVolume = v.sustainGoal;
                                v.phase = ActiveVoice::SUSTAIN;
                            }
                        }
                        else
                        {
                            v.envelopeCounter = v.voice->decay;
                        }
                    }
                }
                break;

            case ActiveVoice::SUSTAIN:
                v.envelopeVolume = v.sustainGoal;
                break;

            case ActiveVoice::RELEASE:
                if (v.voice->release == 0)
                {
                    // Instant release
                    v.envelopeVolume = 0;
                    // CGB pseudo-echo: echoVol = (envelopeGoal * pseudoEchoVol + 0xFF) >> 8
                    {
                        int echoVol = (v.envelopeGoal * (int)v.pseudoEchoVol + 0xFF) >> 8;
                        if (echoVol > 0)
                        {
                            v.envelopeVolume = echoVol;
                            v.phase = ActiveVoice::ECHO;
                        }
                        else
                        {
                            v.phase = ActiveVoice::OFF;
                            v.active = false;
                        }
                    }
                }
                else
                {
                    v.envelopeCounter--;
                    if (v.envelopeCounter <= 0)
                    {
                        v.envelopeVolume--;
                        if (v.envelopeVolume <= 0)
                        {
                            v.envelopeVolume = 0;
                            int echoVol = (v.envelopeGoal * (int)v.pseudoEchoVol + 0xFF) >> 8;
                            if (echoVol > 0)
                            {
                                v.envelopeVolume = echoVol;
                                v.phase = ActiveVoice::ECHO;
                            }
                            else
                            {
                                v.phase = ActiveVoice::OFF;
                                v.active = false;
                            }
                        }
                        else
                        {
                            v.envelopeCounter = v.voice->release;
                        }
                    }
                }
                break;

            case ActiveVoice::ECHO:
                if (v.pseudoEchoLen > 0)
                    v.pseudoEchoLen--;
                if (v.pseudoEchoLen <= 0)
                {
                    v.phase = ActiveVoice::OFF;
                    v.active = false;
                }
                break;

            case ActiveVoice::OFF:
                break;
        }
    }
    else
    {
        // Direct sound envelope (additive attack, multiplicative decay/release)
        // Matches m4a_1.s SoundMainRAM behavior exactly
        switch (v.phase)
        {
            case ActiveVoice::ATTACK:
                v.envelopeVolume += v.voice->attack;
                if (v.envelopeVolume >= 255)
                {
                    v.envelopeVolume = 255;
                    v.phase = ActiveVoice::DECAY;
                }
                break;

            case ActiveVoice::DECAY:
                v.envelopeVolume = (v.envelopeVolume * v.voice->decay) >> 8;
                if (v.envelopeVolume <= v.voice->sustain)
                {
                    v.envelopeVolume = v.voice->sustain;
                    if (v.voice->sustain == 0)
                    {
                        v.phase = ActiveVoice::OFF;
                        v.active = false;
                    }
                    else
                    {
                        v.phase = ActiveVoice::SUSTAIN;
                    }
                }
                break;

            case ActiveVoice::SUSTAIN:
                // Hold at current level
                break;

            case ActiveVoice::RELEASE:
                v.envelopeVolume = (v.envelopeVolume * v.voice->release) >> 8;
                if (v.envelopeVolume <= (int)v.pseudoEchoVol)
                {
                    if (v.pseudoEchoVol == 0)
                    {
                        v.envelopeVolume = 0;
                        v.phase = ActiveVoice::OFF;
                        v.active = false;
                    }
                    else
                    {
                        v.envelopeVolume = (int)v.pseudoEchoVol;
                        v.phase = ActiveVoice::ECHO;
                    }
                }
                break;

            case ActiveVoice::ECHO:
                if (v.pseudoEchoLen > 0)
                    v.pseudoEchoLen--;
                if (v.pseudoEchoLen <= 0)
                {
                    v.phase = ActiveVoice::OFF;
                    v.active = false;
                }
                break;

            case ActiveVoice::OFF:
                break;
        }
    }
}

float GBASynthEngine::renderDirectSound(ActiveVoice& v)
{
    if (!v.voice->sample || v.voice->sample->pcmData.empty()) return 0.0f;

    const GBASample& smp = *v.voice->sample;
    uint32_t numSamples = (uint32_t)smp.pcmData.size();

    if (v.samplePos >= numSamples)
    {
        if (smp.isLooped && smp.loopStart < numSamples)
        {
            v.samplePos = smp.loopStart + fmod(v.samplePos - numSamples, numSamples - smp.loopStart);
        }
        else
        {
            v.active = false;
            return 0.0f;
        }
    }

    // Linear interpolation
    uint32_t idx = (uint32_t)v.samplePos;
    float frac = (float)(v.samplePos - idx);
    float s0 = (float)smp.pcmData[idx] / 128.0f;
    float s1 = s0;
    if (idx + 1 < numSamples)
        s1 = (float)smp.pcmData[idx + 1] / 128.0f;

    v.samplePos += v.sampleStep;
    return s0 + frac * (s1 - s0);
}

float GBASynthEngine::renderSquareWave(ActiveVoice& v)
{
    static const float dutyThresholds[4] = { 0.125f, 0.25f, 0.5f, 0.75f };
    int duty = v.voice->dutyCycle;
    if (duty < 0) duty = 0;
    if (duty > 3) duty = 3;

    float threshold = dutyThresholds[duty];
    float phase = (float)fmod(v.squarePhase, 1.0);
    float out = (phase < threshold) ? 0.5f : -0.5f;

    v.squarePhase += v.squarePhaseInc;
    return out;
}

float GBASynthEngine::renderNoise(ActiveVoice& v)
{
    v.noiseTimer += 1.0;
    while (v.noiseTimer >= v.noiseInterval)
    {
        v.noiseTimer -= v.noiseInterval;
        uint16_t bit = ((v.lfsr >> 0) ^ (v.lfsr >> 1)) & 1;
        v.lfsr = (v.lfsr >> 1) | (bit << 14);
        // In 7-bit mode, bit 6 is also set from the XOR (shorter period, more tonal)
        if (v.noiseWidth7Bit)
            v.lfsr = (v.lfsr & ~(1 << 6)) | (bit << 6);
        v.noiseOutput = (v.lfsr & 1) ? 64 : -64;
    }
    return (float)v.noiseOutput / 128.0f;
}

float GBASynthEngine::renderProgWave(ActiveVoice& v)
{
    if (!v.voice->sample || v.voice->sample->pcmData.empty()) return 0.0f;

    const GBASample& smp = *v.voice->sample;
    uint32_t numSamples = (uint32_t)smp.pcmData.size();

    double pos = fmod(v.samplePos, (double)numSamples);
    if (pos < 0) pos += numSamples;

    uint32_t idx = (uint32_t)pos;
    float frac = (float)(pos - idx);
    float s0 = (float)smp.pcmData[idx % numSamples] / 128.0f;
    float s1 = (float)smp.pcmData[(idx + 1) % numSamples] / 128.0f;

    v.samplePos += v.sampleStep;
    return s0 + frac * (s1 - s0);
}

void GBASynthEngine::renderFrames(float* output, int frameCount)
{
    wxMutexLocker lock(m_mutex);
    memset(output, 0, frameCount * 2 * sizeof(float));

    static const double FRAME_INTERVAL = 1.0 / 59.7275; // GBA VBlank period in seconds
    double samplesPerFrame = (double)m_sampleRate * FRAME_INTERVAL;

    // Cache previous modM values per channel to detect LFO changes
    int8_t prevModM[16];
    for (int ch = 0; ch < 16; ch++)
        prevModM[ch] = m_channelMod[ch].modM;

    for (int f = 0; f < frameCount; f++)
    {
        // Check if we've crossed a ~60Hz frame boundary
        m_globalFrameCounter += 1.0;
        if (m_globalFrameCounter >= samplesPerFrame)
        {
            m_globalFrameCounter -= samplesPerFrame;

            // Update LFO for all 16 channels
            for (int ch = 0; ch < 16; ch++)
            {
                prevModM[ch] = m_channelMod[ch].modM;
                updateLFO(ch);
            }

            // Envelope step + pitch update for all active voices
            for (int i = 0; i < MAX_ACTIVE_VOICES; i++)
            {
                ActiveVoice& v = m_voices[i];
                if (!v.active) continue;
                computeEnvelopeStep(v);
                if (!v.active) continue;
                // If vibrato (modT==0) and modM changed, recalculate pitch
                if (m_channelMod[v.channel].modT == 0 &&
                    m_channelMod[v.channel].modM != prevModM[v.channel])
                {
                    updateVoicePitch(v);
                }
            }
        }

        // Render all active voices for this sample
        for (int i = 0; i < MAX_ACTIVE_VOICES; i++)
        {
            ActiveVoice& v = m_voices[i];
            if (!v.active) continue;

            float sample = 0.0f;
            switch (v.voice->type)
            {
                case GBAVoice::DIRECT_SOUND:
                    sample = renderDirectSound(v);
                    break;
                case GBAVoice::SQUARE_1:
                case GBAVoice::SQUARE_2:
                    sample = renderSquareWave(v);
                    break;
                case GBAVoice::NOISE:
                    sample = renderNoise(v);
                    break;
                case GBAVoice::PROG_WAVE:
                    sample = renderProgWave(v);
                    break;
                default:
                    break;
            }

            if (!v.active) continue;

            float envMax = v.isCgbVoice ? 15.0f : 255.0f;
            float envGain = (float)v.envelopeVolume / envMax;
            float velocityScale = (float)v.velocity / 127.0f;
            float channelVol = m_channelVolume[v.channel];

            float gain = sample * envGain * velocityScale;

            // Apply tremolo (modT==1): volume modulation
            const ChannelModState& mod = m_channelMod[v.channel];
            if (mod.modT == 1 && mod.modM != 0)
            {
                // m4a.c: x = (vol * volX) >> 5; x = (x * (modM + 128)) >> 7
                // We apply as a multiplier to the combined gain
                float tremoloMul = (float)((int)mod.modM + 128) / 128.0f;
                gain *= tremoloMul;
            }

            gain *= channelVol;

            // Apply auto-pan (modT==2): pan offset by modM
            float panL = v.panL;
            float panR = v.panR;
            if (mod.modT == 2 && mod.modM != 0)
            {
                // m4a.c: y = 2*pan + panX + modM, clamped to -128..127
                // We shift the existing pan position by modM/128
                float panShift = (float)mod.modM / 128.0f;
                float basePan = atan2f(panR, panL) / ((float)M_PI * 0.5f);
                float newPan = basePan + panShift * 0.5f;
                if (newPan < 0.0f) newPan = 0.0f;
                if (newPan > 1.0f) newPan = 1.0f;
                panL = cosf(newPan * (float)M_PI * 0.5f);
                panR = sinf(newPan * (float)M_PI * 0.5f);
            }

            output[f * 2 + 0] += gain * panL;
            output[f * 2 + 1] += gain * panR;
        }
    }

    // Master gain to prevent clipping
    static const float MASTER_GAIN = 1.0f / 8.0f;
    for (int f = 0; f < frameCount * 2; f++)
    {
        output[f] *= MASTER_GAIN;
        if (output[f] > 1.0f) output[f] = 1.0f;
        if (output[f] < -1.0f) output[f] = -1.0f;
    }
}

}

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

GBASynthEngine::GBASynthEngine() : m_sampleRate(13379)
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
    }
    for (int i = 0; i < 16; i++)
    {
        m_channelVolume[i] = 1.0f;
        m_channelPan[i] = 0.5f;
        m_channelPitchBend[i] = 0.0f;
        m_channelPitchBendRange[i] = 2;
    }
}

int GBASynthEngine::findFreeVoice()
{
    // Find inactive voice
    for (int i = 0; i < MAX_ACTIVE_VOICES; i++)
    {
        if (!m_voices[i].active) return i;
    }
    // Steal oldest voice in release phase
    for (int i = 0; i < MAX_ACTIVE_VOICES; i++)
    {
        if (m_voices[i].phase == ActiveVoice::RELEASE)
        {
            m_voices[i].active = false;
            return i;
        }
    }
    // Steal voice with lowest envelope volume
    int lowest = 0;
    int lowestVol = 9999;
    for (int i = 0; i < MAX_ACTIVE_VOICES; i++)
    {
        if (m_voices[i].envelopeVolume < lowestVol)
        {
            lowestVol = m_voices[i].envelopeVolume;
            lowest = i;
        }
    }
    return lowest;
}

void GBASynthEngine::noteOn(int note, int velocity, int channel, const GBAVoice* voice)
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
    v.frameSampleCounter = 0;

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

    float targetFreq = midiNoteToFreq(note);
    float baseFreq = midiNoteToFreq(voice->baseMidiKey);

    if (voice->type == GBAVoice::DIRECT_SOUND || voice->type == GBAVoice::PROG_WAVE)
    {
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
    else if (voice->type == GBAVoice::SQUARE_1 || voice->type == GBAVoice::SQUARE_2)
    {
        v.squarePhase = 0.0;
        v.squarePhaseInc = targetFreq / (double)m_sampleRate;
    }
    else if (voice->type == GBAVoice::NOISE)
    {
        v.lfsr = 0x7FFF;
        v.noiseTimer = 0.0;
        double noiseFreq = 524288.0 / (voice->period ? 2.0 : 1.0);
        v.noiseInterval = (double)m_sampleRate / noiseFreq;
        v.noiseOutput = 0;
    }
}

void GBASynthEngine::noteOff(int note, int channel)
{
    wxMutexLocker lock(m_mutex);
    for (int i = 0; i < MAX_ACTIVE_VOICES; i++)
    {
        if (m_voices[i].active && m_voices[i].note == note && m_voices[i].channel == channel
            && m_voices[i].phase != ActiveVoice::RELEASE && m_voices[i].phase != ActiveVoice::OFF)
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
        case 7: // Volume
            m_channelVolume[channel] = (float)value / 127.0f;
            break;
        case 10: // Pan
            m_channelPan[channel] = (float)value / 127.0f;
            break;
        case 6: // Data Entry MSB (for RPN pitch bend range)
            m_channelPitchBendRange[channel] = value;
            break;
        case 123: // All notes off
            allNotesOff(channel);
            break;
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

            float targetFreq = midiNoteToFreq(m_voices[i].note) * powf(2.0f, semitones / 12.0f);
            float baseFreq = midiNoteToFreq(m_voices[i].voice->baseMidiKey);

            if (m_voices[i].voice->type == GBAVoice::DIRECT_SOUND || m_voices[i].voice->type == GBAVoice::PROG_WAVE)
            {
                if (m_voices[i].voice->sample && m_voices[i].voice->sample->sampleRate > 0)
                {
                    m_voices[i].sampleStep = (targetFreq / baseFreq) *
                        ((double)m_voices[i].voice->sample->sampleRate / (double)m_sampleRate);
                }
            }
            else if (m_voices[i].voice->type == GBAVoice::SQUARE_1 || m_voices[i].voice->type == GBAVoice::SQUARE_2)
            {
                m_voices[i].squarePhaseInc = targetFreq / (double)m_sampleRate;
            }
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
                    v.phase = ActiveVoice::OFF;
                    v.active = false;
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
                            v.phase = ActiveVoice::OFF;
                            v.active = false;
                        }
                        else
                        {
                            v.envelopeCounter = v.voice->release;
                        }
                    }
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
                if (v.envelopeVolume <= 0)
                {
                    v.envelopeVolume = 0;
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

    for (int i = 0; i < MAX_ACTIVE_VOICES; i++)
    {
        ActiveVoice& v = m_voices[i];
        if (!v.active) continue;

        float velocityScale = (float)v.velocity / 127.0f;
        float channelVol = m_channelVolume[v.channel];
        float envMax = v.isCgbVoice ? 15.0f : 255.0f;

        for (int f = 0; f < frameCount; f++)
        {
            // Step envelope at GBA frame rate (~60Hz)
            v.frameSampleCounter += 1.0;
            if (v.frameSampleCounter >= ((double)m_sampleRate / 59.7275))
            {
                v.frameSampleCounter -= ((double)m_sampleRate / 59.7275);
                computeEnvelopeStep(v);
            }

            if (!v.active) break;

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

            float envGain = (float)v.envelopeVolume / envMax;
            float gain = sample * envGain * velocityScale * channelVol;
            output[f * 2 + 0] += gain * v.panL;
            output[f * 2 + 1] += gain * v.panR;
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

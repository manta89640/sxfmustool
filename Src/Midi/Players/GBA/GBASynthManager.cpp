#include "Midi/Players/GBA/GBASynthManager.h"
#include "Midi/CommonMidiUtils.h"
#include "Midi/MeasureData.h"
#include "Midi/Players/Sequencer.h"
#include "Midi/Sequence.h"
#include "PreferencesData.h"

#include "jdksmidi/world.h"
#include "jdksmidi/track.h"
#include "jdksmidi/multitrack.h"
#include "jdksmidi/sequencer.h"

#include <wx/thread.h>
#include <wx/string.h>
#include <wx/intl.h>
#include <wx/filename.h>
#include <wx/textfile.h>

#define MINIAUDIO_IMPLEMENTATION
#include "Midi/Players/GBA/miniaudio.h"

#include <cstdio>
#include <cstring>
#include <iostream>
#include <fstream>

namespace AriaMaestosa
{

static ma_device g_audio_device;
static bool g_device_initialized = false;
static GBASynthEngine* g_callback_engine = NULL;

static void audioCallback(ma_device* pDevice, void* pOutput, const void* /*pInput*/, ma_uint32 frameCount)
{
    (void)pDevice;
    if (g_callback_engine)
    {
        g_callback_engine->renderFrames((float*)pOutput, (int)frameCount);
    }
    else
    {
        memset(pOutput, 0, frameCount * 2 * sizeof(float));
    }
}

// ---- Sequencer thread ----

class GBASequencerThread : public wxThread
{
    jdksmidi::MIDIMultiTrack* m_jdkmidiseq;
    jdksmidi::MIDISequencer* m_jdksequencer;
    int m_songLengthInTicks;
    bool m_selectionOnly;
    int m_startTick;
    Sequence* m_sequence;
    GBASynthManager* m_manager;

public:
    GBASequencerThread(Sequence* seq, bool selectionOnly, GBASynthManager* manager)
        : wxThread(wxTHREAD_DETACHED), m_jdkmidiseq(NULL), m_jdksequencer(NULL),
          m_songLengthInTicks(0), m_selectionOnly(selectionOnly), m_startTick(0),
          m_sequence(seq), m_manager(manager)
    {
    }

    ~GBASequencerThread()
    {
        delete m_jdksequencer;
        delete m_jdkmidiseq;
    }

    void prepareSequencer()
    {
        m_jdkmidiseq = new jdksmidi::MIDIMultiTrack();
        m_songLengthInTicks = -1;
        int trackAmount = -1;
        m_startTick = 0;
        makeJDKMidiSequence(m_sequence, *m_jdkmidiseq, m_selectionOnly,
                            &m_songLengthInTicks, &m_startTick, &trackAmount, true);
        m_jdksequencer = new jdksmidi::MIDISequencer(m_jdkmidiseq);
    }

    void go(int* startTick)
    {
        if (Create() != wxTHREAD_NO_ERROR)
        {
            std::cerr << "[GBA Synth] Failed to create sequencer thread" << std::endl;
            return;
        }
        SetPriority(85);
        prepareSequencer();
        *startTick = m_startTick;
        Run();
    }

    ExitCode Entry()
    {
        AriaSequenceTimer timer(m_sequence);
        timer.run(m_jdksequencer, m_songLengthInTicks);
        // Only call stop() if the song ended naturally (not already stopped by user).
        // If user pressed stop and then started new playback, m_threadShouldContinue
        // is now true for the NEW playback — but we waited for this thread to finish
        // in playSequence(), so this path won't execute in that case.
        if (m_manager->seq_must_continue())
            m_manager->stop();
        m_manager->notifyThreadDone();
        return 0;
    }
};

// ---- GBASynthManager implementation ----

GBASynthManager::GBASynthManager()
    : m_parser(NULL), m_voicegroupNum(-1), m_playing(false),
      m_threadShouldContinue(false), m_threadRunning(false),
      m_currentTick(0), m_accurateTick(0),
      m_sequence(NULL)
{
    for (int i = 0; i < 16; i++) m_channelProgram[i] = 0;
}

void GBASynthManager::notifyThreadDone()
{
    m_threadRunning = false;
}

GBASynthManager::~GBASynthManager()
{
    freeMidiPlayer();
}

const GBAVoice* GBASynthManager::resolveVoice(int programIndex, int note)
{
    if (programIndex < 0 || programIndex >= (int)m_voicegroup.voices.size())
        return NULL;

    const GBAVoice& voice = m_voicegroup.voices[programIndex];

    if (voice.type == GBAVoice::KEYSPLIT || voice.type == GBAVoice::KEYSPLIT_ALL)
    {
        if (m_parser) return m_parser->resolveKeysplit(voice, note);
        return NULL;
    }

    return &voice;
}

void GBASynthManager::initMidiPlayer()
{
    wxString projectDir = PreferencesData::getInstance()->getValue(SETTING_ID_GBA_PROJECT_DIR);
    if (!projectDir.IsEmpty())
    {
        std::string dir(projectDir.mb_str());
        m_parser = new VoicegroupParser(dir);
    }

    // Start miniaudio device
    if (!g_device_initialized)
    {
        g_callback_engine = &m_engine;

        ma_device_config config = ma_device_config_init(ma_device_type_playback);
        config.playback.format = ma_format_f32;
        config.playback.channels = 2;
        config.sampleRate = m_engine.getSampleRate();
        config.dataCallback = audioCallback;
        config.pUserData = NULL;

        if (ma_device_init(NULL, &config, &g_audio_device) == MA_SUCCESS)
        {
            ma_device_start(&g_audio_device);
            g_device_initialized = true;
        }
        else
        {
            fprintf(stderr, "[GBA Synth] Failed to initialize audio device\n");
        }
    }
}

void GBASynthManager::freeMidiPlayer()
{
    if (g_device_initialized)
    {
        ma_device_stop(&g_audio_device);
        ma_device_uninit(&g_audio_device);
        g_device_initialized = false;
        g_callback_engine = NULL;
    }

    delete m_parser;
    m_parser = NULL;
}

void GBASynthManager::reloadVoicegroup(int voicegroupNum)
{
    if (!m_parser) return;
    if (voicegroupNum < 0) return;

    m_voicegroup.voices.clear();
    if (m_parser->loadVoicegroup(voicegroupNum, m_voicegroup))
    {
        m_voicegroupNum = voicegroupNum;
        printf("[GBA Synth] Loaded voicegroup%03d with %d voices\n",
               voicegroupNum, (int)m_voicegroup.voices.size());
    }
    else
    {
        fprintf(stderr, "[GBA Synth] Failed to load voicegroup%03d\n", voicegroupNum);
    }
}

void GBASynthManager::setSampleRate(int rate)
{
    m_engine.setSampleRate(rate);

    if (g_device_initialized)
    {
        ma_device_stop(&g_audio_device);
        ma_device_uninit(&g_audio_device);
        g_device_initialized = false;

        g_callback_engine = &m_engine;

        ma_device_config config = ma_device_config_init(ma_device_type_playback);
        config.playback.format = ma_format_f32;
        config.playback.channels = 2;
        config.sampleRate = m_engine.getSampleRate();
        config.dataCallback = audioCallback;
        config.pUserData = NULL;

        if (ma_device_init(NULL, &config, &g_audio_device) == MA_SUCCESS)
        {
            ma_device_start(&g_audio_device);
            g_device_initialized = true;
        }
        else
        {
            fprintf(stderr, "[GBA Synth] Failed to reinitialize audio device at %d Hz\n", rate);
        }
    }
}

bool GBASynthManager::playSequence(Sequence* sequence, int* startTick)
{
    if (m_playing) return false;

    // Wait for previous sequencer thread to fully exit before starting a new one.
    // This prevents the old thread from corrupting new playback state (calling stop(),
    // sending all-notes-off, or sharing the global timer pointer in Sequencer.cpp).
    int waitMs = 0;
    while (m_threadRunning && waitMs < 1000)
    {
        wxThread::Sleep(2);
        waitMs += 2;
    }

    m_engine.reset();
    for (int i = 0; i < 16; i++) m_channelProgram[i] = 0;
    m_sequence = sequence;
    m_playing = true;
    m_threadShouldContinue = true;
    m_threadRunning = true;

    // Try to load the correct voicegroup for this file
    if (m_parser)
    {
        wxString projectDir = PreferencesData::getInstance()->getValue(SETTING_ID_GBA_PROJECT_DIR);
        if (!projectDir.IsEmpty())
        {
            // Get MIDI filename from the sequence's file path
            wxString seqPath = sequence->getFilepath();
            if (!seqPath.IsEmpty())
            {
                wxFileName fn(seqPath);
                wxString midiFilename = fn.GetFullName();
                // Parse voicegroup number from midi.cfg
                wxString cfgPath = projectDir + wxT("/sound/songs/midi/midi.cfg");
                if (wxFileExists(cfgPath))
                {
                    wxTextFile file(cfgPath);
                    if (file.Open())
                    {
                        wxString lowerTarget = midiFilename.Lower();
                        for (wxString line = file.GetFirstLine(); !file.Eof(); line = file.GetNextLine())
                        {
                            wxString trimmed = line.Strip(wxString::both);
                            if (trimmed.IsEmpty()) continue;
                            int colonPos = trimmed.Find(':');
                            if (colonPos == wxNOT_FOUND) continue;
                            wxString lineFilename = trimmed.Left(colonPos).Strip(wxString::both).Lower();
                            if (lineFilename != lowerTarget) continue;
                            wxString flags = trimmed.Mid(colonPos + 1);
                            int gPos = flags.Find(wxT("-G"));
                            if (gPos != wxNOT_FOUND)
                            {
                                wxString after = flags.Mid(gPos + 2);
                                wxString digits;
                                for (size_t i = 0; i < after.Len(); i++)
                                {
                                    if (after[i] >= '0' && after[i] <= '9')
                                        digits += after[i];
                                    else
                                        break;
                                }
                                long val = 0;
                                digits.ToLong(&val);
                                reloadVoicegroup((int)val);
                            }
                            else
                            {
                                reloadVoicegroup(0);
                            }
                            break;
                        }
                    }
                }
            }
        }
    }

    GBASequencerThread* thread = new GBASequencerThread(sequence, false, this);
    thread->go(startTick);
    m_start_tick = *startTick;

    return true;
}

bool GBASynthManager::playSelected(Sequence* sequence, int* startTick)
{
    if (m_playing) return false;

    int waitMs = 0;
    while (m_threadRunning && waitMs < 1000)
    {
        wxThread::Sleep(2);
        waitMs += 2;
    }

    m_engine.reset();
    for (int i = 0; i < 16; i++) m_channelProgram[i] = 0;
    m_sequence = sequence;
    m_playing = true;
    m_threadShouldContinue = true;
    m_threadRunning = true;

    GBASequencerThread* thread = new GBASequencerThread(sequence, true, this);
    thread->go(startTick);
    m_start_tick = *startTick;

    return true;
}

bool GBASynthManager::isPlaying()
{
    return m_playing;
}

void GBASynthManager::stop()
{
    m_threadShouldContinue = false;
    m_playing = false;
    m_engine.reset();
}

int GBASynthManager::trackPlaybackProgression()
{
    return m_currentTick;
}

int GBASynthManager::getAccurateTick()
{
    return m_accurateTick;
}

void GBASynthManager::playNote(int noteNum, int volume, int /*duration*/, int channel, int instrument)
{
    if (m_playing) return;

    bool isRhythm = false;
    if (instrument >= 0 && instrument < (int)m_voicegroup.voices.size())
        isRhythm = (m_voicegroup.voices[instrument].type == GBAVoice::KEYSPLIT_ALL);

    const GBAVoice* voice = resolveVoice(instrument, noteNum);
    if (voice)
    {
        m_engine.noteOn(noteNum, volume, channel, voice, isRhythm);
    }
}

void GBASynthManager::stopNote()
{
    for (int c = 0; c < 16; c++)
        m_engine.allNotesOff(c);
}

void GBASynthManager::exportAudioFile(Sequence* sequence, wxString filepath)
{
    // Offline render: run sequencer in accelerated mode and write WAV
    int exportRate = m_engine.getSampleRate();
    GBASynthEngine offlineEngine;
    offlineEngine.setSampleRate(exportRate);
    offlineEngine.reset();

    // Save/restore voicegroup state
    GBASynthEngine* prevEngine = g_callback_engine;
    g_callback_engine = NULL; // Don't output to speakers during export

    jdksmidi::MIDIMultiTrack jdkmidiseq;
    int songLengthInTicks = 0, startTick = 0, trackAmount = 0;
    makeJDKMidiSequence(sequence, jdkmidiseq, false, &songLengthInTicks, &startTick, &trackAmount, true);

    jdksmidi::MIDISequencer jdksequencer(&jdkmidiseq);
    jdksequencer.GoToTimeMs(0);

    int bpm = sequence->getTempo();
    int beatlen = sequence->ticksPerQuarterNote();
    double ticksPerMs = (double)bpm * (double)beatlen / 60000.0;

    // Calculate total duration in samples
    double totalMs = (double)songLengthInTicks / ticksPerMs;
    int totalSamples = (int)(totalMs / 1000.0 * exportRate) + exportRate; // +1 sec padding

    // Render in chunks
    std::vector<float> buffer(totalSamples * 2, 0.0f);
    int samplesRendered = 0;

    jdksmidi::MIDITimedBigMessage ev;
    int evTrack;
    jdksmidi::MIDIClockTime tick;

    int exportProgram[16];
    for (int i = 0; i < 16; i++) exportProgram[i] = 0;

    // Track cumulative time for correct tempo-change handling
    double cumulativeMs = 0.0;
    double lastEventTick = 0.0;

    if (jdksequencer.GetNextEventTime(&tick))
    {
        double nextEventMs = (double)tick / ticksPerMs;
        double currentMs = 0.0;

        while (samplesRendered < totalSamples)
        {
            // Process events up to current time
            while (nextEventMs <= currentMs)
            {
                if (!jdksequencer.GetNextEvent(&evTrack, &ev)) break;

                int channel = ev.GetChannel();
                if (ev.IsNoteOn())
                {
                    int note = ev.GetNote();
                    bool isRhythm = false;
                    int prog = exportProgram[channel];
                    if (prog >= 0 && prog < (int)m_voicegroup.voices.size())
                        isRhythm = (m_voicegroup.voices[prog].type == GBAVoice::KEYSPLIT_ALL);
                    const GBAVoice* voice = resolveVoice(prog, note);
                    if (voice) offlineEngine.noteOn(note, ev.GetVelocity(), channel, voice, isRhythm);
                }
                else if (ev.IsNoteOff())
                {
                    offlineEngine.noteOff(ev.GetNote(), channel);
                }
                else if (ev.IsControlChange())
                {
                    offlineEngine.controlChange(ev.GetController(), ev.GetControllerValue(), channel);
                }
                else if (ev.IsPitchBend())
                {
                    offlineEngine.pitchBend(ev.GetBenderValue(), channel);
                }
                else if (ev.IsProgramChange())
                {
                    exportProgram[channel] = ev.GetPGValue();
                }
                else if (ev.IsTempo())
                {
                    int eventBpm = ev.GetTempo32() / 32;
                    // Accumulate time at old tempo before switching
                    double evTick = ev.GetTime();
                    cumulativeMs += (evTick - lastEventTick) / ticksPerMs;
                    lastEventTick = evTick;
                    ticksPerMs = (double)eventBpm * (double)beatlen / 60000.0;
                }

                if (!jdksequencer.GetNextEventTime(&tick)) break;
                nextEventMs = cumulativeMs + ((double)tick - lastEventTick) / ticksPerMs;
            }

            // Render a chunk
            int chunkSize = 512;
            if (samplesRendered + chunkSize > totalSamples)
                chunkSize = totalSamples - samplesRendered;

            offlineEngine.renderFrames(&buffer[samplesRendered * 2], chunkSize);
            samplesRendered += chunkSize;
            currentMs += (double)chunkSize / (double)exportRate * 1000.0;

            if (currentMs > totalMs + 1000.0) break;
        }
    }

    // Write WAV file
    std::ofstream out(filepath.mb_str(), std::ios::binary);
    if (out.is_open())
    {
        int dataSize = samplesRendered * 2 * 2; // 2 channels, 16-bit
        int fileSize = 36 + dataSize;

        // WAV header
        out.write("RIFF", 4);
        int riffSize = fileSize;
        out.write((char*)&riffSize, 4);
        out.write("WAVE", 4);
        out.write("fmt ", 4);
        int fmtSize = 16;
        out.write((char*)&fmtSize, 4);
        short audioFormat = 1; // PCM
        out.write((char*)&audioFormat, 2);
        short numChannels = 2;
        out.write((char*)&numChannels, 2);
        int sampleRate = exportRate;
        out.write((char*)&sampleRate, 4);
        int byteRate = exportRate * 2 * 2;
        out.write((char*)&byteRate, 4);
        short blockAlign = 4;
        out.write((char*)&blockAlign, 2);
        short bitsPerSample = 16;
        out.write((char*)&bitsPerSample, 2);
        out.write("data", 4);
        out.write((char*)&dataSize, 4);

        // Convert float to 16-bit PCM
        for (int i = 0; i < samplesRendered * 2; i++)
        {
            float s = buffer[i];
            if (s > 1.0f) s = 1.0f;
            if (s < -1.0f) s = -1.0f;
            short sample = (short)(s * 32767.0f);
            out.write((char*)&sample, 2);
        }
        out.close();
    }

    g_callback_engine = prevEngine;
}

const wxString GBASynthManager::getAudioExtension()
{
    return wxT(".wav");
}

const wxString GBASynthManager::getAudioWildcard()
{
    return wxString(_("WAV file")) + wxT("|*.wav");
}

wxArrayString GBASynthManager::getOutputChoices()
{
    wxArrayString out;
    out.Add(wxT("GBA Voicegroup Synth"));
    return out;
}

void GBASynthManager::seq_note_on(const int note, const int volume, const int channel)
{
    int prog = m_channelProgram[channel];
    bool isRhythm = false;
    if (prog >= 0 && prog < (int)m_voicegroup.voices.size())
        isRhythm = (m_voicegroup.voices[prog].type == GBAVoice::KEYSPLIT_ALL);

    const GBAVoice* voice = resolveVoice(prog, note);
    if (voice)
    {
        m_engine.noteOn(note, volume, channel, voice, isRhythm);
    }
}

void GBASynthManager::seq_note_off(const int note, const int channel)
{
    m_engine.noteOff(note, channel);
}

void GBASynthManager::seq_prog_change(const int instrument, const int channel)
{
    if (channel >= 0 && channel < 16)
        m_channelProgram[channel] = instrument;
}

void GBASynthManager::seq_controlchange(const int controller, const int value, const int channel)
{
    m_engine.controlChange(controller, value, channel);
}

void GBASynthManager::seq_pitch_bend(const int value, const int channel)
{
    m_engine.pitchBend(value, channel);
}

void GBASynthManager::seq_notify_current_tick(const int tick)
{
    m_currentTick = tick;
    if (tick == -1) m_playing = false;
}

void GBASynthManager::seq_notify_accurate_current_tick(const int tick)
{
    m_accurateTick = tick;
}

bool GBASynthManager::seq_must_continue()
{
    return m_threadShouldContinue;
}

// ---- Factory ----

class GBASynthManagerFactory : public PlatformMidiManagerFactory
{
public:
    GBASynthManagerFactory() : PlatformMidiManagerFactory(wxT("GBA Synth"))
    {
    }
    virtual ~GBASynthManagerFactory()
    {
    }
    virtual PlatformMidiManager* newInstance()
    {
        return new GBASynthManager();
    }
};

GBASynthManagerFactory g_gba_synth_factory;

} // end namespace

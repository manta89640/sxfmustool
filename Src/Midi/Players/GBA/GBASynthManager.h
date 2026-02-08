#ifndef __GBA_SYNTH_MANAGER_H__
#define __GBA_SYNTH_MANAGER_H__

#include "Midi/Players/PlatformMidiManager.h"
#include "Midi/Players/GBA/GBASynthEngine.h"
#include "Midi/Players/GBA/VoicegroupParser.h"

namespace AriaMaestosa
{

class GBASynthManager : public PlatformMidiManager
{
    GBASynthEngine m_engine;
    GBAVoicegroup m_voicegroup;
    VoicegroupParser* m_parser;
    int m_channelProgram[16];
    int m_voicegroupNum;

    bool m_playing;
    volatile bool m_threadShouldContinue;
    volatile bool m_threadRunning;
    int m_currentTick;
    int m_accurateTick;

    Sequence* m_sequence;

    const GBAVoice* resolveVoice(int programIndex, int note);

public:
    GBASynthManager();
    virtual ~GBASynthManager();

    void notifyThreadDone();

    virtual void initMidiPlayer();
    virtual void freeMidiPlayer();

    virtual bool playSequence(Sequence* sequence, int* startTick);
    virtual bool playSelected(Sequence* sequence, int* startTick);
    virtual bool isPlaying();
    virtual void stop();
    virtual int trackPlaybackProgression();
    virtual int getAccurateTick();

    virtual void playNote(int noteNum, int volume, int duration, int channel, int instrument);
    virtual void stopNote();

    virtual void exportAudioFile(Sequence* sequence, wxString filepath);
    virtual const wxString getAudioExtension();
    virtual const wxString getAudioWildcard();

    virtual wxArrayString getOutputChoices();

    // seq_* callbacks
    virtual void seq_note_on(const int note, const int volume, const int channel);
    virtual void seq_note_off(const int note, const int channel);
    virtual void seq_prog_change(const int instrument, const int channel);
    virtual void seq_controlchange(const int controller, const int value, const int channel);
    virtual void seq_pitch_bend(const int value, const int channel);
    virtual void seq_notify_current_tick(const int tick);
    virtual void seq_notify_accurate_current_tick(const int tick);
    virtual bool seq_must_continue();

    void reloadVoicegroup(int voicegroupNum);
    void setSampleRate(int rate);
};

}

#endif

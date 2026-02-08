#ifndef __GBA_VOICEGROUP_PARSER_H__
#define __GBA_VOICEGROUP_PARSER_H__

#include "Midi/Players/GBA/SampleLoader.h"
#include <map>
#include <string>
#include <vector>

namespace AriaMaestosa
{

struct GBAVoice
{
    enum Type
    {
        DIRECT_SOUND = 0,
        SQUARE_1,
        SQUARE_2,
        PROG_WAVE,
        NOISE,
        KEYSPLIT,
        KEYSPLIT_ALL,
        EMPTY
    };

    Type type;
    int baseMidiKey;
    int pan;
    int attack, decay, sustain, release;

    // Direct sound / programmable wave
    std::string sampleSymbol;
    GBASample* sample;

    // Square wave
    int dutyCycle;
    int sweep;

    // Noise
    int period;

    // Keysplit
    std::string subVoicegroupSymbol;
    std::string keysplitTableSymbol;

    GBAVoice()
        : type(EMPTY), baseMidiKey(60), pan(0),
          attack(0), decay(0), sustain(0), release(0),
          sample(NULL), dutyCycle(2), sweep(0), period(0)
    {
    }
};

struct GBAVoicegroup
{
    std::vector<GBAVoice> voices;
};

class VoicegroupParser
{
    std::string m_projectDir;

    // Symbol → file path mappings
    std::map<std::string, std::string> m_directSoundPaths;
    std::map<std::string, std::string> m_progWavePaths;

    // Keysplit tables: name → 128-byte table
    std::map<std::string, std::vector<uint8_t> > m_keysplitTables;

    // Loaded samples cache: file path → sample data
    std::map<std::string, GBASample> m_sampleCache;

    // Parsed sub-voicegroups cache: voicegroup name → voicegroup
    std::map<std::string, GBAVoicegroup> m_voicegroupCache;

    void parseDirectSoundData();
    void parseProgrammableWaveData();
    void parseKeysplitTables();
    bool parseVoicegroupFile(const std::string& voicegroupName, GBAVoicegroup& outGroup);
    GBAVoice parseVoiceLine(const std::string& line);

    GBASample* resolveSample(const std::string& symbol);

public:
    VoicegroupParser(const std::string& projectDir);

    bool loadVoicegroup(int voicegroupNum, GBAVoicegroup& outGroup);

    // Resolve keysplit voice: given a note and a keysplit voice, return the actual leaf voice
    const GBAVoice* resolveKeysplit(const GBAVoice& voice, int note);
};

}

#endif

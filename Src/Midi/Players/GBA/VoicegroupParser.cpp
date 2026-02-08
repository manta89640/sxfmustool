#include "Midi/Players/GBA/VoicegroupParser.h"
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>

namespace AriaMaestosa
{

static std::string trim(const std::string& s)
{
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

static std::string stripComment(const std::string& line)
{
    size_t at = line.find('@');
    if (at != std::string::npos) return line.substr(0, at);
    return line;
}

static std::vector<std::string> splitArgs(const std::string& argStr)
{
    std::vector<std::string> result;
    std::stringstream ss(argStr);
    std::string token;
    while (std::getline(ss, token, ','))
    {
        result.push_back(trim(token));
    }
    return result;
}

VoicegroupParser::VoicegroupParser(const std::string& projectDir)
    : m_projectDir(projectDir)
{
}

void VoicegroupParser::parseDirectSoundData()
{
    std::string path = m_projectDir + "/sound/direct_sound_data.inc";
    std::ifstream f(path.c_str());
    if (!f.is_open()) return;

    std::string currentSymbol;
    std::string line;
    while (std::getline(f, line))
    {
        std::string trimmed = trim(line);
        // Look for symbol labels like "DirectSoundWaveData_xxx::"
        size_t colonPos = trimmed.find("::");
        if (colonPos != std::string::npos && trimmed.find(".incbin") == std::string::npos)
        {
            currentSymbol = trimmed.substr(0, colonPos);
            continue;
        }

        if (!currentSymbol.empty() && trimmed.find(".incbin") != std::string::npos)
        {
            size_t q1 = trimmed.find('"');
            size_t q2 = trimmed.find('"', q1 + 1);
            if (q1 != std::string::npos && q2 != std::string::npos)
            {
                std::string relPath = trimmed.substr(q1 + 1, q2 - q1 - 1);
                m_directSoundPaths[currentSymbol] = m_projectDir + "/" + relPath;
            }
            currentSymbol.clear();
        }
    }
}

void VoicegroupParser::parseProgrammableWaveData()
{
    std::string path = m_projectDir + "/sound/programmable_wave_data.inc";
    std::ifstream f(path.c_str());
    if (!f.is_open()) return;

    std::string currentSymbol;
    std::string line;
    while (std::getline(f, line))
    {
        std::string trimmed = trim(line);
        size_t colonPos = trimmed.find("::");
        if (colonPos != std::string::npos && trimmed.find(".incbin") == std::string::npos)
        {
            currentSymbol = trimmed.substr(0, colonPos);
            continue;
        }

        if (!currentSymbol.empty() && trimmed.find(".incbin") != std::string::npos)
        {
            size_t q1 = trimmed.find('"');
            size_t q2 = trimmed.find('"', q1 + 1);
            if (q1 != std::string::npos && q2 != std::string::npos)
            {
                std::string relPath = trimmed.substr(q1 + 1, q2 - q1 - 1);
                m_progWavePaths[currentSymbol] = m_projectDir + "/" + relPath;
            }
            currentSymbol.clear();
        }
    }
}

void VoicegroupParser::parseKeysplitTables()
{
    std::string path = m_projectDir + "/sound/keysplit_tables.inc";
    std::ifstream f(path.c_str());
    if (!f.is_open()) return;

    // The keysplit tables file has entries like:
    //   .set KeySplitTable1, . - 0
    //   .byte 0  @ 0
    //   .byte 0  @ 1
    //   ...
    // The offset after ". - " tells us which MIDI note the data starts at.
    // We build a 128-byte table for each, with 0 for unmapped notes.

    std::string currentName;
    int currentOffset = 0;
    std::vector<uint8_t> currentBytes;

    std::string line;
    while (std::getline(f, line))
    {
        std::string trimmed = trim(stripComment(line));
        if (trimmed.empty()) continue;

        if (trimmed.find(".set ") == 0)
        {
            // Save previous table if any
            if (!currentName.empty())
            {
                std::vector<uint8_t> table(128, 0);
                for (size_t i = 0; i < currentBytes.size() && (currentOffset + (int)i) < 128; i++)
                {
                    if (currentOffset + (int)i >= 0)
                        table[currentOffset + i] = currentBytes[i];
                }
                m_keysplitTables[currentName] = table;
            }

            // Parse: .set KeySplitTableN, . - OFFSET
            size_t spaceAfterSet = trimmed.find(' ', 5);
            size_t commaPos = trimmed.find(',');
            if (commaPos != std::string::npos && spaceAfterSet != std::string::npos)
            {
                currentName = trim(trimmed.substr(5, commaPos - 5));
                std::string offsetPart = trim(trimmed.substr(commaPos + 1));
                // Parse ". - N"
                size_t dashPos = offsetPart.find('-');
                if (dashPos != std::string::npos)
                {
                    std::string numStr = trim(offsetPart.substr(dashPos + 1));
                    currentOffset = atoi(numStr.c_str());
                }
                else
                {
                    currentOffset = 0;
                }
            }
            currentBytes.clear();
            continue;
        }

        if (trimmed.find(".byte") == 0)
        {
            std::string valStr = trim(trimmed.substr(5));
            currentBytes.push_back((uint8_t)atoi(valStr.c_str()));
        }
    }

    // Save last table
    if (!currentName.empty())
    {
        std::vector<uint8_t> table(128, 0);
        for (size_t i = 0; i < currentBytes.size() && (currentOffset + (int)i) < 128; i++)
        {
            if (currentOffset + (int)i >= 0)
                table[currentOffset + i] = currentBytes[i];
        }
        m_keysplitTables[currentName] = table;
    }
}

GBASample* VoicegroupParser::resolveSample(const std::string& symbol)
{
    // Check direct sound paths first
    std::map<std::string, std::string>::iterator it = m_directSoundPaths.find(symbol);
    if (it == m_directSoundPaths.end())
    {
        it = m_progWavePaths.find(symbol);
        if (it == m_progWavePaths.end())
            return NULL;
    }

    const std::string& filePath = it->second;

    // Check cache
    std::map<std::string, GBASample>::iterator cit = m_sampleCache.find(filePath);
    if (cit != m_sampleCache.end())
        return &cit->second;

    // Load sample
    GBASample sample;
    if (!loadGBASample(filePath, sample))
    {
        fprintf(stderr, "[GBA Synth] Failed to load sample: %s\n", filePath.c_str());
        return NULL;
    }

    m_sampleCache[filePath] = sample;
    return &m_sampleCache[filePath];
}

GBAVoice VoicegroupParser::parseVoiceLine(const std::string& line)
{
    GBAVoice voice;
    std::string trimmed = trim(stripComment(line));

    if (trimmed.find("voice_directsound_no_resample") == 0 ||
        trimmed.find("voice_directsound_alt") == 0 ||
        trimmed.find("voice_directsound") == 0)
    {
        voice.type = GBAVoice::DIRECT_SOUND;
        size_t spacePos = trimmed.find(' ');
        if (spacePos == std::string::npos) return voice;
        std::vector<std::string> args = splitArgs(trimmed.substr(spacePos + 1));
        // base, pan, sample_symbol, attack, decay, sustain, release
        if (args.size() >= 7)
        {
            voice.baseMidiKey = atoi(args[0].c_str());
            voice.pan = atoi(args[1].c_str());
            voice.sampleSymbol = args[2];
            voice.attack = atoi(args[3].c_str());
            voice.decay = atoi(args[4].c_str());
            voice.sustain = atoi(args[5].c_str());
            voice.release = atoi(args[6].c_str());
            voice.sample = resolveSample(voice.sampleSymbol);
        }
    }
    else if (trimmed.find("voice_square_1_alt") == 0 || trimmed.find("voice_square_1") == 0)
    {
        voice.type = GBAVoice::SQUARE_1;
        size_t spacePos = trimmed.find(' ');
        if (spacePos == std::string::npos) return voice;
        std::vector<std::string> args = splitArgs(trimmed.substr(spacePos + 1));
        // base, pan, sweep, duty, attack, decay, sustain, release
        if (args.size() >= 8)
        {
            voice.baseMidiKey = atoi(args[0].c_str());
            voice.pan = atoi(args[1].c_str());
            voice.sweep = atoi(args[2].c_str());
            voice.dutyCycle = atoi(args[3].c_str());
            voice.attack = atoi(args[4].c_str());
            voice.decay = atoi(args[5].c_str());
            voice.sustain = atoi(args[6].c_str());
            voice.release = atoi(args[7].c_str());
        }
    }
    else if (trimmed.find("voice_square_2_alt") == 0 || trimmed.find("voice_square_2") == 0)
    {
        voice.type = GBAVoice::SQUARE_2;
        size_t spacePos = trimmed.find(' ');
        if (spacePos == std::string::npos) return voice;
        std::vector<std::string> args = splitArgs(trimmed.substr(spacePos + 1));
        // base, pan, duty, attack, decay, sustain, release
        if (args.size() >= 7)
        {
            voice.baseMidiKey = atoi(args[0].c_str());
            voice.pan = atoi(args[1].c_str());
            voice.dutyCycle = atoi(args[2].c_str());
            voice.attack = atoi(args[3].c_str());
            voice.decay = atoi(args[4].c_str());
            voice.sustain = atoi(args[5].c_str());
            voice.release = atoi(args[6].c_str());
        }
    }
    else if (trimmed.find("voice_programmable_wave_alt") == 0 || trimmed.find("voice_programmable_wave") == 0)
    {
        voice.type = GBAVoice::PROG_WAVE;
        size_t spacePos = trimmed.find(' ');
        if (spacePos == std::string::npos) return voice;
        std::vector<std::string> args = splitArgs(trimmed.substr(spacePos + 1));
        // base, pan, wave_symbol, attack, decay, sustain, release
        if (args.size() >= 7)
        {
            voice.baseMidiKey = atoi(args[0].c_str());
            voice.pan = atoi(args[1].c_str());
            voice.sampleSymbol = args[2];
            voice.attack = atoi(args[3].c_str());
            voice.decay = atoi(args[4].c_str());
            voice.sustain = atoi(args[5].c_str());
            voice.release = atoi(args[6].c_str());
            voice.sample = resolveSample(voice.sampleSymbol);
        }
    }
    else if (trimmed.find("voice_noise_alt") == 0 || trimmed.find("voice_noise") == 0)
    {
        voice.type = GBAVoice::NOISE;
        size_t spacePos = trimmed.find(' ');
        if (spacePos == std::string::npos) return voice;
        std::vector<std::string> args = splitArgs(trimmed.substr(spacePos + 1));
        // base, pan, period, attack, decay, sustain, release
        if (args.size() >= 7)
        {
            voice.baseMidiKey = atoi(args[0].c_str());
            voice.pan = atoi(args[1].c_str());
            voice.period = atoi(args[2].c_str());
            voice.attack = atoi(args[3].c_str());
            voice.decay = atoi(args[4].c_str());
            voice.sustain = atoi(args[5].c_str());
            voice.release = atoi(args[6].c_str());
        }
    }
    else if (trimmed.find("voice_keysplit_all") == 0)
    {
        voice.type = GBAVoice::KEYSPLIT_ALL;
        size_t spacePos = trimmed.find(' ');
        if (spacePos == std::string::npos) return voice;
        voice.subVoicegroupSymbol = trim(trimmed.substr(spacePos + 1));
    }
    else if (trimmed.find("voice_keysplit") == 0)
    {
        voice.type = GBAVoice::KEYSPLIT;
        size_t spacePos = trimmed.find(' ');
        if (spacePos == std::string::npos) return voice;
        std::vector<std::string> args = splitArgs(trimmed.substr(spacePos + 1));
        if (args.size() >= 2)
        {
            voice.subVoicegroupSymbol = args[0];
            voice.keysplitTableSymbol = args[1];
        }
    }

    return voice;
}

bool VoicegroupParser::parseVoicegroupFile(const std::string& voicegroupName, GBAVoicegroup& outGroup)
{
    // Check cache
    std::map<std::string, GBAVoicegroup>::iterator cit = m_voicegroupCache.find(voicegroupName);
    if (cit != m_voicegroupCache.end())
    {
        outGroup = cit->second;
        return true;
    }

    std::string path = m_projectDir + "/sound/voicegroups/" + voicegroupName + ".inc";
    std::ifstream f(path.c_str());
    if (!f.is_open())
    {
        fprintf(stderr, "[GBA Synth] Cannot open voicegroup file: %s\n", path.c_str());
        return false;
    }

    bool pastLabel = false;
    std::string line;
    while (std::getline(f, line))
    {
        std::string trimmed = trim(stripComment(line));
        if (trimmed.empty()) continue;

        if (!pastLabel)
        {
            if (trimmed.find("::") != std::string::npos)
                pastLabel = true;
            continue;
        }

        if (trimmed.find("voice_") == 0)
        {
            outGroup.voices.push_back(parseVoiceLine(line));
        }
    }

    m_voicegroupCache[voicegroupName] = outGroup;
    return true;
}

bool VoicegroupParser::loadVoicegroup(int voicegroupNum, GBAVoicegroup& outGroup)
{
    // Parse lookup tables first (lazy init)
    if (m_directSoundPaths.empty()) parseDirectSoundData();
    if (m_progWavePaths.empty()) parseProgrammableWaveData();
    if (m_keysplitTables.empty()) parseKeysplitTables();

    char buf[64];
    snprintf(buf, sizeof(buf), "voicegroup%03d", voicegroupNum);
    std::string name(buf);

    return parseVoicegroupFile(name, outGroup);
}

const GBAVoice* VoicegroupParser::resolveKeysplit(const GBAVoice& voice, int note)
{
    if (note < 0) note = 0;
    if (note > 127) note = 127;

    if (voice.type == GBAVoice::KEYSPLIT_ALL)
    {
        // Use the note directly as the voice index in the sub-voicegroup
        GBAVoicegroup subGroup;
        if (!parseVoicegroupFile(voice.subVoicegroupSymbol, subGroup))
            return NULL;

        // Cache lookup: get from cache after parsing
        std::map<std::string, GBAVoicegroup>::iterator cit = m_voicegroupCache.find(voice.subVoicegroupSymbol);
        if (cit == m_voicegroupCache.end()) return NULL;

        if (note < (int)cit->second.voices.size())
            return &cit->second.voices[note];
        return NULL;
    }

    if (voice.type == GBAVoice::KEYSPLIT)
    {
        // Look up keysplit table to find sub-voice index
        std::map<std::string, std::vector<uint8_t> >::iterator kit = m_keysplitTables.find(voice.keysplitTableSymbol);
        if (kit == m_keysplitTables.end()) return NULL;

        int voiceIdx = kit->second[note];

        GBAVoicegroup subGroup;
        if (!parseVoicegroupFile(voice.subVoicegroupSymbol, subGroup))
            return NULL;

        std::map<std::string, GBAVoicegroup>::iterator cit = m_voicegroupCache.find(voice.subVoicegroupSymbol);
        if (cit == m_voicegroupCache.end()) return NULL;

        if (voiceIdx < (int)cit->second.voices.size())
            return &cit->second.voices[voiceIdx];
        return NULL;
    }

    return NULL;
}

}

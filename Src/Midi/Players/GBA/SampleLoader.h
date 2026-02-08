#ifndef __GBA_SAMPLE_LOADER_H__
#define __GBA_SAMPLE_LOADER_H__

#include <cstdint>
#include <string>
#include <vector>

namespace AriaMaestosa
{

struct GBASample
{
    uint32_t sampleRate;
    uint32_t loopStart;
    uint32_t numSamples;
    bool isLooped;
    bool isCompressed;
    std::vector<int8_t> pcmData;
};

bool loadGBASample(const std::string& filePath, GBASample& outSample);

}

#endif

#include "Midi/Players/GBA/SampleLoader.h"
#include <cstdio>
#include <cstring>

namespace AriaMaestosa
{

static uint32_t readU32LE(const uint8_t* p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static const int8_t sDeltaLookup[16] = {
    0, 1, 4, 9, 16, 25, 36, 49, -64, -49, -36, -25, -16, -9, -4, -1
};

bool loadGBASample(const std::string& filePath, GBASample& outSample)
{
    FILE* f = fopen(filePath.c_str(), "rb");
    if (!f) return false;

    fseek(f, 0, SEEK_END);
    long fileSize = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (fileSize < 16)
    {
        fclose(f);
        return false;
    }

    std::vector<uint8_t> raw(fileSize);
    if ((long)fread(raw.data(), 1, fileSize, f) != fileSize)
    {
        fclose(f);
        return false;
    }
    fclose(f);

    uint32_t flags = readU32LE(raw.data());
    outSample.isCompressed = (flags & 1) != 0;
    outSample.isLooped = (flags & 0x40000000) != 0;
    outSample.sampleRate = readU32LE(raw.data() + 4) / 1024;
    outSample.loopStart = readU32LE(raw.data() + 8);
    outSample.numSamples = readU32LE(raw.data() + 12) + 1;

    if (outSample.sampleRate == 0) outSample.sampleRate = 8000;

    if (outSample.isCompressed)
    {
        size_t compressedBytes = fileSize - 16;
        size_t numDecodedSamples = compressedBytes * 2;
        outSample.pcmData.resize(numDecodedSamples);

        int8_t accumulator = 0;
        size_t outIdx = 0;
        for (size_t i = 0; i < compressedBytes && outIdx < numDecodedSamples; i++)
        {
            uint8_t byte = raw[16 + i];
            int8_t lo = sDeltaLookup[byte & 0x0F];
            accumulator += lo;
            outSample.pcmData[outIdx++] = accumulator;

            if (outIdx < numDecodedSamples)
            {
                int8_t hi = sDeltaLookup[(byte >> 4) & 0x0F];
                accumulator += hi;
                outSample.pcmData[outIdx++] = accumulator;
            }
        }
        outSample.numSamples = outIdx;
    }
    else
    {
        size_t dataLen = fileSize - 16;
        if (dataLen > outSample.numSamples) dataLen = outSample.numSamples;
        outSample.pcmData.resize(dataLen);
        memcpy(outSample.pcmData.data(), raw.data() + 16, dataLen);
        outSample.numSamples = dataLen;
    }

    return true;
}

}

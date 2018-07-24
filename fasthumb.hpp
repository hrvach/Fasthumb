/* Copyright (c) 2018, Hrvoje Cavrak

All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

The views and conclusions contained in the software and documentation are those
of the authors and should not be interpreted as representing official policies,
either expressed or implied, of the Fasthumb project. */

#include <string>
#include <algorithm>
#include <vector>
#include <fstream>
#include <map>
#include "nvcuvid.h"
#include "cuviddec.h"

#define TICKS_PER_SECOND 90000
#define NAL_TYPE_IDR         5

class Fasthumb
{
public:
    Fasthumb(int outWidth, int outHeight, unsigned int thumbnailInterval);
    ~Fasthumb();

    void initializeDecoder();
    void createVideoParser();

    void decodeFrames();

    void extractKeyFrames(const std::string &inputFilePath, int videoPid);

private:
    struct SessionData
    {
        CUVIDPARSERPARAMS               videoParserParams;
        CUVIDSOURCEDATAPACKET           videoDataPacket;
        CUVIDDECODECAPS                 videoDecodeCaps;

        CUvideoparser                   videoParser;
        CUcontext                       videoContext;
        CUvideodecoder                  videoDecoder;
        CUdevice                        videoDevice;

        char                           *imgbuf;
        int                             bytesNeeded;

        int                             targetWidth;
        int                             targetHeight;
        int                             frameCount;

        unsigned int                    thumbnailInterval;
    };

    SessionData decoderState;

    std::vector <uint8_t>               keyFramesVector;

    static void checkSuccess(CUresult result);
    static void compressAndSaveJpeg(void *userData, unsigned char *imageBuffer);

    static int CUDAAPI decodePictureCallback(void* userData, CUVIDPICPARAMS* picture);
    static int CUDAAPI displayPictureCallback(void* userData, CUVIDPARSERDISPINFO* info);
    static int CUDAAPI parserSequenceCallback(void* userData, CUVIDEOFORMAT* videoFormat);
};

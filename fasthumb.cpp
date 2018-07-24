/*  Copyright (c) 2018, Hrvoje Cavrak

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


#include <iostream>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <turbojpeg.h>

#include "include/mpeg/ts.h"
#include "include/mpeg/pes.h"

#include "include/nvcuvid.h"
#include "include/cuviddec.h"

#include "fasthumb.hpp"


Fasthumb::Fasthumb(int outWidth, int outHeight, unsigned int thumbnailInterval) {
    decoderState.videoContext       = { 0 };
    decoderState.videoDevice        = { 0 };

    decoderState.imgbuf             = nullptr;
    decoderState.videoDecoder       = nullptr;

    decoderState.frameCount         = 0;
    decoderState.targetWidth        = outWidth;
    decoderState.targetHeight       = outHeight;
    decoderState.thumbnailInterval  = thumbnailInterval;
}

Fasthumb::~Fasthumb() {
    cuvidDestroyVideoParser(decoderState.videoParser);
    cuvidDestroyDecoder(decoderState.videoDecoder);
    cuCtxDestroy(decoderState.videoContext);

    if (decoderState.imgbuf != NULL)
        cuMemFreeHost(decoderState.imgbuf);
}

/* --------------------------------------------------------------------------- *
 * Initialize CUDA and create context, set video parser parameters, create     *
 * video parser. Callback functions are specified here. They are static, but   *
 * will be called with a pointer to whatever is assigned to pUserData.         *
 * --------------------------------------------------------------------------- */
void Fasthumb::initializeDecoder() {
    checkSuccess( cuInit(0) );                         // 0 = Flags, has to be 0
    checkSuccess( cuCtxCreate(&decoderState.videoContext, 0, decoderState.videoDevice) );

    memset((void*)&decoderState.videoParserParams, 0x00, sizeof(CUVIDPARSERPARAMS));

    decoderState.videoParserParams.ulMaxDisplayDelay       = 1;
    decoderState.videoParserParams.ulMaxNumDecodeSurfaces  = 4;
    decoderState.videoParserParams.pUserData               = &decoderState;
    decoderState.videoParserParams.CodecType               = cudaVideoCodec_H264;
    decoderState.videoParserParams.pfnDecodePicture        = decodePictureCallback;
    decoderState.videoParserParams.pfnSequenceCallback     = parserSequenceCallback;
    decoderState.videoParserParams.pfnDisplayPicture       = displayPictureCallback;

    decoderState.videoParser = nullptr;
    checkSuccess( cuvidCreateVideoParser(&decoderState.videoParser, &decoderState.videoParserParams) );
}

/* --------------------------------------------------------------------------- *
 * Open raw MPEG-TS file, extract video PID payload and slice out one keyframe *
 * Then skip N seconds (spacing between successive keyframes). It deliberately *
 * skips some frames to speed up extraction. Keyframes go to keyFramesVector   *
 * --------------------------------------------------------------------------- */
void Fasthumb::extractKeyFrames(const std::string &inputFilePath, int videoPid) {
    bool      insideKeyFrame = false;
    uint64_t    previous_pts = 0,
                current_pts  = 0;

    int  inputFileDescriptor = ::open(inputFilePath.c_str(), O_RDONLY);
    auto  inputFileLength    = lseek(inputFileDescriptor, 0, SEEK_END);

    /* Tell the kernel we will access the file descriptor sequentially so it can optimize if possible */
    posix_fadvise(inputFileDescriptor, 0, inputFileLength, POSIX_FADV_SEQUENTIAL);

    uint8_t *inputFilePtr = (uint8_t *) mmap(0, inputFileLength, PROT_READ, MAP_FILE | MAP_SHARED, inputFileDescriptor, 0);
    uint8_t *inputFileBeginning = inputFilePtr;

    /* Iterate through file, look only at video pid, wait for PES unit start, extract timestamp, check if
     * we advanced far enough, check if keyframe start pattern is found, and keep writing keyframe data to vector
     * until we find another PES unit start which is not a NAL unit type 5 (IDR - can start with 0x25, 0x45 or 0x65) */

    for (; inputFilePtr < inputFileBeginning + inputFileLength; inputFilePtr += TS_SIZE) {
        if (ts_get_pid(inputFilePtr) == videoPid) {
            uint8_t *payload = ts_payload(inputFilePtr);

            if (ts_get_unitstart(inputFilePtr)) {
                insideKeyFrame = false;

                uint8_t *pes_ptr = ts_payload(inputFilePtr);
                payload = pes_payload(pes_ptr);

                if (pes_has_pts(pes_ptr)) {
                    current_pts = pes_get_pts(pes_ptr);
                }

                if ((current_pts - previous_pts > decoderState.thumbnailInterval * TICKS_PER_SECOND) || !previous_pts) {

                    for (uint8_t *p = payload; p < inputFilePtr + (TS_SIZE - 4); p++) {         /* -4 is not to go beyond packet end with p[3] */
                        if (p[0]==0 && p[1]==0 && p[2]==1 && (p[3] & 0x1f)==NAL_TYPE_IDR)
                        /* Annex B. framing -> 00 00 01 = start, Bitmask lower 5 bits to get unit type. Type 5 = IDR/keyframe */
                            insideKeyFrame = true;
                    }
                }
                previous_pts = insideKeyFrame ? current_pts : previous_pts ;
            }

            if (insideKeyFrame) {
                std::vector <uint8_t> packetPayload (payload, inputFilePtr + TS_SIZE);
                keyFramesVector.insert(keyFramesVector.end(), packetPayload.begin(), packetPayload.end());
            }
        }
    }
    munmap(inputFileBeginning, inputFileLength);
}

/* --------------------------------------------------------------------------- *
 * After keyframes are extracted, pointer to keyFramesVector is provided to    *
 * cuvidParseVideoData so it can proceed with the h264 video data parsing.     *
 * --------------------------------------------------------------------------- */
void Fasthumb::decodeFrames() {
    decoderState.videoDataPacket.flags = 0;
    decoderState.videoDataPacket.payload = (uint8_t*) &keyFramesVector[0];
    decoderState.videoDataPacket.payload_size = sizeof(uint8_t) * keyFramesVector.size();
    decoderState.videoDataPacket.timestamp = 0;

    checkSuccess( cuvidParseVideoData(decoderState.videoParser, &decoderState.videoDataPacket) );
}

/* --------------------------------------------------------------------------- *
 * Gets raw YUV buffer and compresses it using the turbojpeg library. Opens    *
 * the file, writes compressed JPEG and closes it. Release memory / cleanup.   *
 * --------------------------------------------------------------------------- */
void Fasthumb::compressAndSaveJpeg(void *userData, unsigned char *inputBuffer) {
    SessionData* state = static_cast<SessionData *>(userData);
    std::string filename = std::to_string(state->frameCount) + ".jpg";
    std::ofstream outputFile;

    const int JPEG_QUALITY = 75;
    long unsigned int jpegSize = 0;
    unsigned char* compressedImage = NULL;

    tjhandle jpegCompressor = tjInitCompress();
    tjCompressFromYUV(jpegCompressor, inputBuffer, state->targetWidth, 2, state->targetHeight,
                      TJSAMP_420, &compressedImage, &jpegSize, JPEG_QUALITY, TJFLAG_FASTDCT);

    tjDestroy(jpegCompressor);

    outputFile.open(filename, std::ios::out | std::ios::binary);
    outputFile.write((const char *)compressedImage, jpegSize);
    outputFile.close();

    tjFree(compressedImage);
}

/* --------------------------------------------------------------------------- *
 * This gets called when the image is ready to be decoded. It can differ from  *
 * chronological order (hint, B frames). We use only I frames, no prob there   *
 * --------------------------------------------------------------------------- */
int CUDAAPI Fasthumb::decodePictureCallback(void* userData, CUVIDPICPARAMS* picture) {
    SessionData* state = static_cast<SessionData *>(userData);
    checkSuccess( cuvidDecodePicture(state->videoDecoder, picture) );

    state->frameCount ++;
    return 1;
}

/* --------------------------------------------------------------------------- *
 * Function gets called when the image is ready to be displayed. As this       *
 * should have the correct ordering, fire up all post-processing from here.    *
 * --------------------------------------------------------------------------- */
int CUDAAPI Fasthumb::displayPictureCallback(void* userData, CUVIDPARSERDISPINFO* info) {
    SessionData* state = static_cast<SessionData *>(userData);
    std::vector <uint8_t> outImageBuffer, uPlane, vPlane;

    CUVIDPROCPARAMS procParameters = { 0 };
    CUdeviceptr devicePointer = 0;
    unsigned int stride = 0;

    procParameters.progressive_frame = info->progressive_frame;
    procParameters.top_field_first = info->top_field_first;
    procParameters.unpaired_field = info->repeat_first_field < 0;

    checkSuccess( cuvidMapVideoFrame(state->videoDecoder, info->picture_index, &devicePointer, &stride, &procParameters) );

    if (state->imgbuf == nullptr) {
        /* We need stride * height bytes for the Y plane and 1/2 times that for the U+V planes. So, it's 3/2 or 3*x >> 1 */
        state->bytesNeeded = stride * (3 * state->targetHeight / 2);
        checkSuccess( cuMemAllocHost((void**)&state->imgbuf, state->bytesNeeded) );
    }

    checkSuccess( cuMemcpyDtoH(state->imgbuf, devicePointer, state->bytesNeeded) );

    /* Convert NV12 to YUV420p by simply copying the Y plane and splitting the
       combined UV plane into separate U and V planes, appending them at the end.
       We need to account for the stride - targetWidth difference.

       Y1|Y2|Y3|Y4    ->    Y1|Y2|Y3|Y4
       Y5|Y6|Y7|Y8    ->    Y1|Y2|Y3|Y4
       U1|V1|U2|V2    ->    U1|U2|U3|U4      bitstream:
       U3|V3|U4|V4    ->    V1|V2|V3|V4      YYYYYYYY UVUV  ->  YYYYYYYY UUVV */

    char *imgptr = state->imgbuf;

    /* Y plane  */
    for (; imgptr < state->imgbuf + stride * state->targetHeight; imgptr += stride) {
      outImageBuffer.insert(outImageBuffer.end(), imgptr, imgptr + state->targetWidth);
    }

    /* Split the UV plane to separate planes  */
    for (; imgptr < state->imgbuf + stride * state->targetHeight * 3 / 2; imgptr += stride) {
        for (char *k = imgptr; k < imgptr + state->targetWidth; k += 2) {
            uPlane.push_back(*k);
            vPlane.push_back(*(k+1));
        }
    }

    /* Append the planes after the Y plane to obtain the proper planar YUV420 */
    outImageBuffer.insert(outImageBuffer.end(), uPlane.begin(), uPlane.end());
    outImageBuffer.insert(outImageBuffer.end(), vPlane.begin(), vPlane.end());

    compressAndSaveJpeg(state, &outImageBuffer[0]);
    checkSuccess( cuvidUnmapVideoFrame(state->videoDecoder, devicePointer) );

    return 1;
}

/* --------------------------------------------------------------------------- *
 * This gets called before decoding frames. Sets parameters, creates decoder.  *
 * --------------------------------------------------------------------------- */
int CUDAAPI Fasthumb::parserSequenceCallback(void* userData, CUVIDEOFORMAT* videoFormat) {
    SessionData* state = static_cast<SessionData *>(userData);

    CUVIDDECODECREATEINFO videoDecodeCreateInfo = { 0 };

    videoDecodeCreateInfo.vidLock = nullptr;
    videoDecodeCreateInfo.ulNumOutputSurfaces = 1;
    videoDecodeCreateInfo.ulNumDecodeSurfaces = 20;
    videoDecodeCreateInfo.CodecType = videoFormat->codec;
    videoDecodeCreateInfo.ulTargetWidth = state->targetWidth;
    videoDecodeCreateInfo.ulWidth = videoFormat->coded_width;
    videoDecodeCreateInfo.ulTargetHeight = state->targetHeight;
    videoDecodeCreateInfo.ulHeight = videoFormat->coded_height;
    videoDecodeCreateInfo.ChromaFormat = cudaVideoChromaFormat_420;
    videoDecodeCreateInfo.OutputFormat = cudaVideoSurfaceFormat_NV12;
    videoDecodeCreateInfo.ulCreationFlags = cudaVideoCreate_PreferCUVID;
    videoDecodeCreateInfo.DeinterlaceMode = cudaVideoDeinterlaceMode_Weave;
    videoDecodeCreateInfo.bitDepthMinus8 = videoFormat->bit_depth_luma_minus8;

    cuCtxPushCurrent(state->videoContext);
    checkSuccess( cuvidCreateDecoder(&state->videoDecoder, &videoDecodeCreateInfo) );
    cuCtxPopCurrent(nullptr);

    return 1;
}

/* --------------------------------------------------------------------------- *
 * Checks result of a cuvid API call, exits with a meaningful error if failure *
 * --------------------------------------------------------------------------- */
void Fasthumb::checkSuccess(CUresult result) {
    const char* error_string = nullptr;

    if (result != CUDA_SUCCESS) {
        cuGetErrorString(result, &error_string);
        std::cout << "Failure! Reason: " << error_string << std::endl;
        exit(1);
    }
}

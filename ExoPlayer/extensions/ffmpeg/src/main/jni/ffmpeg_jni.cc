/*
 * Copyright (C) 2016 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <android/log.h>
#include <jni.h>
#include <stdlib.h>
#include <string.h>
#include <cinttypes> // For PRId64

extern "C" {
#ifdef __cplusplus
#define __STDC_CONSTANT_MACROS
#ifdef _STDINT_H
#undef _STDINT_H
#endif
#include <stdint.h>
#endif
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

#define LOG_TAG "ffmpeg_jni"
#define LOGD(...) \
  ((void)__android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__))
#define LOGE(...) \
  ((void)__android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__))
#define LOGW(...) \
  ((void)__android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__))


#define LIBRARY_FUNC(RETURN_TYPE, NAME, ...)                              \
  extern "C" {                                                            \
  JNIEXPORT RETURN_TYPE                                                   \
      Java_com_google_android_exoplayer2_ext_ffmpeg_FfmpegLibrary_##NAME( \
          JNIEnv *env, jobject thiz, ##__VA_ARGS__);                      \
  }                                                                       \
  JNIEXPORT RETURN_TYPE                                                   \
      Java_com_google_android_exoplayer2_ext_ffmpeg_FfmpegLibrary_##NAME( \
          JNIEnv *env, jobject thiz, ##__VA_ARGS__)

#define AUDIO_DECODER_FUNC(RETURN_TYPE, NAME, ...)                             \
  extern "C" {                                                                 \
  JNIEXPORT RETURN_TYPE                                                        \
      Java_com_google_android_exoplayer2_ext_ffmpeg_FfmpegAudioDecoder_##NAME( \
          JNIEnv *env, jobject thiz, ##__VA_ARGS__);                           \
  }                                                                            \
  JNIEXPORT RETURN_TYPE                                                        \
      Java_com_google_android_exoplayer2_ext_ffmpeg_FfmpegAudioDecoder_##NAME( \
          JNIEnv *env, jobject thiz, ##__VA_ARGS__)

#define EXTRACTOR_FUNC(RETURN_TYPE, NAME, ...)                                 \
  extern "C" {                                                                 \
  JNIEXPORT RETURN_TYPE                                                        \
      Java_com_google_android_exoplayer2_ext_ffmpeg_FfmpegExtractor_##NAME( \
          JNIEnv *env, jclass clazz, ##__VA_ARGS__);                           \
  }                                                                            \
  JNIEXPORT RETURN_TYPE                                                        \
      Java_com_google_android_exoplayer2_ext_ffmpeg_FfmpegExtractor_##NAME( \
          JNIEnv *env, jclass clazz, ##__VA_ARGS__)

#define ERROR_STRING_BUFFER_LENGTH 256

static const AVSampleFormat OUTPUT_FORMAT_PCM_16BIT = AV_SAMPLE_FMT_S16;
static const AVSampleFormat OUTPUT_FORMAT_PCM_FLOAT = AV_SAMPLE_FMT_FLT;

static const int AUDIO_DECODER_ERROR_INVALID_DATA = -1;
static const int AUDIO_DECODER_ERROR_OTHER = -2;

static const int EXO_RESULT_END_OF_INPUT = -1;


typedef struct FfmpegExtractorIOContext {
    JavaVM *javaVm;
    jobject extractorInputRef;
    jmethodID readMethodId;
    jmethodID getPositionMethodId;
    jmethodID getLengthMethodId;

    uint8_t *avio_ctx_buffer;
    int avio_ctx_buffer_size;
} FfmpegExtractorIOContext;

typedef struct NativeExtractorContext {
    AVFormatContext *formatContext;
    AVIOContext *avioContext;
    FfmpegExtractorIOContext *ioJavaContext;
    AVPacket *packet;

    jclass streamInfoClassRef;
    jmethodID streamInfoConstructor;
    jclass packetInfoClassRef;
    jmethodID packetInfoConstructor;
} NativeExtractorContext;

static int read_packet_callback(void *opaque, uint8_t *buf, int buf_size) {
    FfmpegExtractorIOContext *java_io_ctx = (FfmpegExtractorIOContext *)opaque;
    JNIEnv *env;
    bool attached_thread = false;
    int getEnvStat = java_io_ctx->javaVm->GetEnv((void **)&env, JNI_VERSION_1_6);

    if (getEnvStat == JNI_EDETACHED) {
        if (java_io_ctx->javaVm->AttachCurrentThread(&env, NULL) != 0) {
            LOGE("FFmpegExtractor: Failed to attach current thread for read");
            return AVERROR_EXTERNAL;
        }
        attached_thread = true;
    } else if (getEnvStat != JNI_OK) {
        LOGE("FFmpegExtractor: GetEnv failed with %d", getEnvStat);
        return AVERROR_EXTERNAL;
    }

    jbyteArray byteArray = env->NewByteArray(buf_size);
    if (!byteArray) {
        LOGE("FFmpegExtractor: Failed to allocate byteArray for read");
        if (attached_thread) java_io_ctx->javaVm->DetachCurrentThread();
        return AVERROR(ENOMEM);
    }

    jint bytes_read = env->CallIntMethod(java_io_ctx->extractorInputRef,
                                         java_io_ctx->readMethodId,
                                         byteArray, 0, buf_size);

    int result;
    if (bytes_read > 0) {
        env->GetByteArrayRegion(byteArray, 0, bytes_read, (jbyte *)buf);
        result = bytes_read;
    } else if (bytes_read == EXO_RESULT_END_OF_INPUT) {
        result = AVERROR_EOF;
    } else if (bytes_read == 0) { // Should ideally not happen if ExtractorInput.read blocks
        result = AVERROR(EAGAIN);
    }
    else {
        LOGE("FFmpegExtractor: Java ExtractorInput.read returned error: %d", bytes_read);
        result = AVERROR_EXTERNAL; // Or a more specific error if Java side provides one
    }

    env->DeleteLocalRef(byteArray);
    if (attached_thread) {
        java_io_ctx->javaVm->DetachCurrentThread();
    }
    return result;
}

static int64_t seek_callback(void *opaque, int64_t offset, int whence) {
    FfmpegExtractorIOContext *java_io_ctx = (FfmpegExtractorIOContext *)opaque;
    JNIEnv *env;
    bool attached_thread = false;
    int getEnvStat = java_io_ctx->javaVm->GetEnv((void **)&env, JNI_VERSION_1_6);

    if (getEnvStat == JNI_EDETACHED) {
        if (java_io_ctx->javaVm->AttachCurrentThread(&env, NULL) != 0) {
            LOGE("FFmpegExtractor: Failed to attach current thread for seek");
            return -1; // FFmpeg error codes are typically negative
        }
        attached_thread = true;
    } else if (getEnvStat != JNI_OK) {
        LOGE("FFmpegExtractor: GetEnv failed with %d for seek", getEnvStat);
        return -1;
    }

    int64_t new_pos = -1;

    if (whence == AVSEEK_SIZE) {
        if (java_io_ctx->getLengthMethodId) {
            new_pos = env->CallLongMethod(java_io_ctx->extractorInputRef, java_io_ctx->getLengthMethodId);
            // getLength returns C.LENGTH_UNSET (-1) if unknown, which is fine for FFmpeg
        } else {
            new_pos = -1; // Indicate error or unknown size
        }
    } else if (whence == SEEK_CUR) {
        // FFmpeg's AVIOContext seeking is byte-based.
        // ExtractorInput.getPosition() also returns byte position.
        if (java_io_ctx->getPositionMethodId) {
             long currentJavaPos = env->CallLongMethod(java_io_ctx->extractorInputRef, java_io_ctx->getPositionMethodId);
             // ExoPlayer's ExtractorInput doesn't directly support relative seeking.
             // If FFmpeg needs this with a non-zero offset, it's tricky.
             // For now, assume FFmpeg mostly uses this to get current position (offset=0).
             if (offset == 0) {
                 new_pos = currentJavaPos;
             } else {
                 LOGW("FFmpegExtractor: seek_callback SEEK_CUR with non-zero offset (%" PRId64 ") not directly supported by ExtractorInput. Returning current position.", offset);
                 new_pos = currentJavaPos; // Or -1 if this behavior is problematic
             }
        } else {
            new_pos = -1;
        }
    } else if (whence == SEEK_SET) {
        // This is problematic as ExtractorInput doesn't have a setPosition method.
        // FFmpeg might use this during probing. If it's essential, a more complex
        // solution involving re-opening or a buffering layer in Java might be needed.
        // For simple formats like DSF, it might not be called or might be tolerable if it fails.
        LOGW("FFmpegExtractor: seek_callback SEEK_SET to %" PRId64 " not supported from JNI via ExtractorInput.", offset);
        new_pos = -1; // Indicate error
    } else if (whence == SEEK_END) {
        LOGW("FFmpegExtractor: seek_callback SEEK_END not supported from JNI via ExtractorInput.");
        new_pos = -1; // Indicate error
    } else {
        // Should not happen with standard whence values
        LOGE("FFmpegExtractor: Unknown whence value in seek_callback: %d", whence);
        new_pos = -1;
    }

    if (attached_thread) {
        java_io_ctx->javaVm->DetachCurrentThread();
    }
    return new_pos;
}

AVCodec *getCodecByName(JNIEnv *env, jstring codecName);
AVCodecContext *createContext(JNIEnv *env, AVCodec *codec, jbyteArray extraData,
                              jboolean outputFloat, jint rawSampleRate,
                              jint rawChannelCount);
int decodePacket(AVCodecContext *context, AVPacket *packet,
                 uint8_t *outputBuffer, int outputSize);
int transformError(int errorNumber);
void logError(const char *functionName, int errorNumber);
void releaseContext(AVCodecContext *context);


jint JNI_OnLoad(JavaVM *vm, void *reserved) {
  JNIEnv *env;
  if (vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) != JNI_OK) {
    LOGE("JNI_OnLoad: GetEnv failed");
    return -1;
  }
  // Pre-caching classes here is optional if they are found per-instance later.
  // It can be a slight performance improvement but adds complexity if JNI_OnLoad fails.
  // For now, we rely on finding them in nativeCreateExtractorContext.
  LOGD("JNI_OnLoad successful.");
  return JNI_VERSION_1_6;
}

LIBRARY_FUNC(jstring, ffmpegGetVersion) {
  return env->NewStringUTF(LIBAVCODEC_IDENT);
}

LIBRARY_FUNC(jint, ffmpegGetInputBufferPaddingSize) {
  return (jint)AV_INPUT_BUFFER_PADDING_SIZE;
}

LIBRARY_FUNC(jboolean, ffmpegHasDecoder, jstring codecName) {
  return getCodecByName(env, codecName) != NULL;
}

AUDIO_DECODER_FUNC(jlong, ffmpegInitialize, jstring codecName,
                   jbyteArray extraData, jboolean outputFloat,
                   jint rawSampleRate, jint rawChannelCount) {
  AVCodec *codec = getCodecByName(env, codecName);
  if (!codec) {
    const char *codecNameChars = env->GetStringUTFChars(codecName, NULL);
    LOGE("Codec not found: %s", codecNameChars);
    if (codecNameChars) env->ReleaseStringUTFChars(codecName, codecNameChars);
    return 0L;
  }
  return (jlong)createContext(env, codec, extraData, outputFloat, rawSampleRate,
                              rawChannelCount);
}

AUDIO_DECODER_FUNC(jint, ffmpegDecode, jlong context, jobject inputData,
                   jint inputSize, jobject outputData, jint outputSize) {
  if (!context) {
    LOGE("Context must be non-NULL.");
    return AUDIO_DECODER_ERROR_OTHER;
  }
  uint8_t *inputBuffer = (uint8_t *)env->GetDirectBufferAddress(inputData);
  uint8_t *outputBuffer = (uint8_t *)env->GetDirectBufferAddress(outputData);

  if (!inputBuffer || !outputBuffer) {
    LOGE("Input or output buffer is not direct.");
    return AUDIO_DECODER_ERROR_OTHER;
  }
  if (inputSize < 0) {
    LOGE("Invalid input buffer size: %d.", inputSize);
    return AUDIO_DECODER_ERROR_INVALID_DATA;
  }
  if (outputSize < 0) {
    LOGE("Invalid output buffer length: %d", outputSize);
    return AUDIO_DECODER_ERROR_INVALID_DATA;
  }

  AVPacket packet;
  av_init_packet(&packet);
  packet.data = inputBuffer;
  packet.size = inputSize;
  return decodePacket((AVCodecContext *)context, &packet, outputBuffer,
                      outputSize);
}

AUDIO_DECODER_FUNC(jint, ffmpegGetChannelCount, jlong context) {
  if (!context) {
    LOGE("Context must be non-NULL for getChannelCount.");
    return -1;
  }
  return ((AVCodecContext *)context)->channels;
}

AUDIO_DECODER_FUNC(jint, ffmpegGetSampleRate, jlong context) {
  if (!context) {
    LOGE("Context must be non-NULL for getSampleRate.");
    return -1;
  }
  return ((AVCodecContext *)context)->sample_rate;
}

AUDIO_DECODER_FUNC(jlong, ffmpegReset, jlong jContext, jbyteArray extraData) {
  AVCodecContext *context = (AVCodecContext *)jContext;
  if (!context) {
    LOGE("Tried to reset without a context.");
    return 0L;
  }

  AVCodecID codecId = context->codec_id;
  if (codecId == AV_CODEC_ID_TRUEHD) {
    // TrueHD requires a full context reset.
    jboolean outputFloat = (jboolean)(context->request_sample_fmt == OUTPUT_FORMAT_PCM_FLOAT);
    jint rawSampleRate = context->sample_rate; // Assuming these were set or are defaults
    jint rawChannelCount = context->channels;  //
    releaseContext(context); // This will free the old context
    AVCodec *codec = avcodec_find_decoder(codecId);
    if (!codec) {
      LOGE("Unexpected error finding codec %d during reset.", codecId);
      return 0L;
    }
    return (jlong)createContext(env, codec, extraData, outputFloat,
                                rawSampleRate, rawChannelCount);
  }

  avcodec_flush_buffers(context);
  if (context->opaque) { // Assuming opaque stores SwrContext
      SwrContext *swrContext = (SwrContext *)context->opaque;
      swr_free(&swrContext);
      context->opaque = NULL;
  }
  return (jlong)context;
}

AUDIO_DECODER_FUNC(void, ffmpegRelease, jlong context) {
  if (context) {
    releaseContext((AVCodecContext *)context);
  }
}

AVCodec *getCodecByName(JNIEnv *env, jstring codecName) {
  if (!codecName) {
    return NULL;
  }
  const char *codecNameChars = env->GetStringUTFChars(codecName, NULL);
  AVCodec *codec = avcodec_find_decoder_by_name(codecNameChars);
  if (codecNameChars) env->ReleaseStringUTFChars(codecName, codecNameChars);
  return codec;
}

AVCodecContext *createContext(JNIEnv *env, AVCodec *codec, jbyteArray extraData,
                              jboolean outputFloat, jint rawSampleRate,
                              jint rawChannelCount) {
  AVCodecContext *context = avcodec_alloc_context3(codec);
  if (!context) {
    LOGE("Failed to allocate context.");
    return NULL;
  }
  context->request_sample_fmt =
      outputFloat ? OUTPUT_FORMAT_PCM_FLOAT : OUTPUT_FORMAT_PCM_16BIT;
  if (extraData) {
    jsize size = env->GetArrayLength(extraData);
    context->extradata_size = size;
    context->extradata =
        (uint8_t *)av_mallocz(size + AV_INPUT_BUFFER_PADDING_SIZE);
    if (!context->extradata) {
      LOGE("Failed to allocate extradata.");
      avcodec_free_context(&context);
      return NULL;
    }
    env->GetByteArrayRegion(extraData, 0, size, (jbyte *)context->extradata);
  }
  if (context->codec_id == AV_CODEC_ID_PCM_MULAW ||
      context->codec_id == AV_CODEC_ID_PCM_ALAW) {
    context->sample_rate = rawSampleRate;
    context->channels = rawChannelCount;
    context->channel_layout = av_get_default_channel_layout(rawChannelCount);
  }
  context->err_recognition = AV_EF_IGNORE_ERR;
  int result = avcodec_open2(context, codec, NULL);
  if (result < 0) {
    logError("avcodec_open2", result);
    avcodec_free_context(&context);
    return NULL;
  }
  return context;
}

int decodePacket(AVCodecContext *context, AVPacket *packet,
                 uint8_t *outputBuffer, int outputSize) {
  int result = 0;
  result = avcodec_send_packet(context, packet);
  if (result < 0) {
    logError("avcodec_send_packet", result);
    return transformError(result);
  }

  int outSize = 0;
  while (true) {
    AVFrame *frame = av_frame_alloc();
    if (!frame) {
      LOGE("Failed to allocate output frame.");
      return AUDIO_DECODER_ERROR_OTHER;
    }
    result = avcodec_receive_frame(context, frame);

    if (result < 0) {
      av_frame_free(&frame);
      if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) {
        break;
      }
      logError("avcodec_receive_frame", result);
      return transformError(result);
    }

    SwrContext *resampleContext = (SwrContext *)context->opaque;
    if (!resampleContext) {
      resampleContext = swr_alloc_set_opts(NULL,
                                   av_get_default_channel_layout(context->channels),
                                   context->request_sample_fmt,
                                   context->sample_rate,
                                   av_get_default_channel_layout(frame->channels),
                                   (AVSampleFormat)frame->format,
                                   frame->sample_rate,
                                   0, NULL);
      if (!resampleContext || swr_init(resampleContext) < 0) {
        LOGE("Failed to initialize SwrContext.");
        if(resampleContext) swr_free(&resampleContext);
        av_frame_free(&frame);
        return AUDIO_DECODER_ERROR_OTHER;
      }
      context->opaque = resampleContext;
    }
    
    int dst_nb_samples = av_rescale_rnd(swr_get_delay(resampleContext, frame->sample_rate) +
                                               frame->nb_samples,
                                       context->sample_rate, frame->sample_rate, AV_ROUND_UP);


    int required_size = av_samples_get_buffer_size(NULL, context->channels, dst_nb_samples,
                                                  context->request_sample_fmt, 0);
    if (outSize + required_size > outputSize) {
      LOGE("Output buffer size (%d) too small for output data (required: %d, current_out: %d).",
           outputSize, required_size, outSize);
      av_frame_free(&frame);
      return AUDIO_DECODER_ERROR_INVALID_DATA;
    }

    uint8_t *dst_buffer_ptr = outputBuffer + outSize;
    int samples_converted = swr_convert(resampleContext,
                                       &dst_buffer_ptr,
                                       dst_nb_samples,
                                       (const uint8_t **)frame->data,
                                       frame->nb_samples);
    av_frame_free(&frame);

    if (samples_converted < 0) {
      logError("swr_convert", samples_converted);
      return AUDIO_DECODER_ERROR_OTHER;
    }
    int converted_data_size = av_samples_get_buffer_size(NULL, context->channels, samples_converted,
                                                        context->request_sample_fmt, 0);
    outSize += converted_data_size;
  }
  return outSize;
}

int transformError(int errorNumber) {
  return errorNumber == AVERROR_INVALIDDATA ? AUDIO_DECODER_ERROR_INVALID_DATA
                                            : AUDIO_DECODER_ERROR_OTHER;
}

void logError(const char *functionName, int errorNumber) {
  char errorBuffer[ERROR_STRING_BUFFER_LENGTH];
  av_strerror(errorNumber, errorBuffer, ERROR_STRING_BUFFER_LENGTH);
  LOGE("Error in %s: %s (code %d)", functionName, errorBuffer, errorNumber);
}

void releaseContext(AVCodecContext *context) {
  if (!context) {
    return;
  }
  SwrContext *swrContext = NULL;
  if (context->opaque) {
    swrContext = (SwrContext *)context->opaque;
    swr_free(&swrContext); // swr_free takes SwrContext**
    context->opaque = NULL;
  }
  avcodec_free_context(&context); // avcodec_free_context takes AVCodecContext**
}

EXTRACTOR_FUNC(jlong, nativeCreateExtractorContext) {
    NativeExtractorContext *context = (NativeExtractorContext *)av_calloc(1, sizeof(NativeExtractorContext));
    if (!context) {
        LOGE("Extractor: Failed to allocate NativeExtractorContext");
        return 0L;
    }
    context->packet = av_packet_alloc();
    if (!context->packet) {
        LOGE("Extractor: Failed to allocate AVPacket");
        av_free(context);
        return 0L;
    }

    jclass streamInfoLocalClass = env->FindClass("com/google/android/exoplayer2/ext/ffmpeg/FfmpegExtractor$FfmpegStreamInfo");
    if (!streamInfoLocalClass) {
        LOGE("Extractor: Failed to find FfmpegStreamInfo class on create");
        av_packet_free(&context->packet);
        av_free(context);
        return 0L;
    }
    context->streamInfoClassRef = (jclass)env->NewGlobalRef(streamInfoLocalClass);
    env->DeleteLocalRef(streamInfoLocalClass); // Delete local ref after global ref is made
    if (!context->streamInfoClassRef) { // Check if NewGlobalRef failed
        LOGE("Extractor: Failed to create global ref for FfmpegStreamInfo class");
        av_packet_free(&context->packet);
        av_free(context);
        return 0L;
    }

    context->streamInfoConstructor = env->GetMethodID(context->streamInfoClassRef, "<init>", "(Ljava/lang/String;III[B)V");
    if (!context->streamInfoConstructor) {
        LOGE("Extractor: Failed to find FfmpegStreamInfo constructor");
        env->DeleteGlobalRef(context->streamInfoClassRef);
        av_packet_free(&context->packet);
        av_free(context);
        return 0L;
    }

    jclass packetInfoLocalClass = env->FindClass("com/google/android/exoplayer2/ext/ffmpeg/FfmpegExtractor$FfmpegPacketInfo");
    if (!packetInfoLocalClass) {
        LOGE("Extractor: Failed to find FfmpegPacketInfo class on create");
        env->DeleteGlobalRef(context->streamInfoClassRef);
        av_packet_free(&context->packet);
        av_free(context);
        return 0L;
    }
    context->packetInfoClassRef = (jclass)env->NewGlobalRef(packetInfoLocalClass);
    env->DeleteLocalRef(packetInfoLocalClass); // Delete local ref
    if (!context->packetInfoClassRef) { // Check if NewGlobalRef failed
        LOGE("Extractor: Failed to create global ref for FfmpegPacketInfo class");
        env->DeleteGlobalRef(context->streamInfoClassRef);
        av_packet_free(&context->packet);
        av_free(context);
        return 0L;
    }

    context->packetInfoConstructor = env->GetMethodID(context->packetInfoClassRef, "<init>", "(IJIIZI)V");
    if (!context->packetInfoConstructor) {
        LOGE("Extractor: Failed to find FfmpegPacketInfo constructor");
        env->DeleteGlobalRef(context->streamInfoClassRef);
        env->DeleteGlobalRef(context->packetInfoClassRef);
        av_packet_free(&context->packet);
        av_free(context);
        return 0L;
    }

    LOGD("Extractor: nativeCreateExtractorContext successful, handle: %p", context);
    return (jlong)context;
}

EXTRACTOR_FUNC(jint, nativeOpenInput, jlong contextHandle, jobject jExtractorInput) {
    NativeExtractorContext *context = (NativeExtractorContext *)contextHandle;
    if (!context) {
        LOGE("Extractor: nativeOpenInput: Invalid context handle");
        return AVERROR(EINVAL);
    }
    if (context->formatContext) {
        LOGW("Extractor: nativeOpenInput: Already has a formatContext. Closing existing.");
        avformat_close_input(&context->formatContext); // This should also free context->avioContext if it was set by avformat_open_input
        // If we manually set pb, we might need to manually free avioContext and its buffer
        if (context->avioContext) {
            if (context->ioJavaContext && context->ioJavaContext->avio_ctx_buffer) {
                av_free(context->ioJavaContext->avio_ctx_buffer);
                context->ioJavaContext->avio_ctx_buffer = NULL;
            }
            // avio_context_free takes AVIOContext**
            AVIOContext* pb_to_free = context->avioContext;
            avio_context_free(&pb_to_free);
            context->avioContext = NULL;
        }
         if (context->ioJavaContext) {
            if(context->ioJavaContext->extractorInputRef) env->DeleteGlobalRef(context->ioJavaContext->extractorInputRef);
            av_free(context->ioJavaContext);
            context->ioJavaContext = NULL;
        }
    }


    context->ioJavaContext = (FfmpegExtractorIOContext *)av_calloc(1, sizeof(FfmpegExtractorIOContext));
    if (!context->ioJavaContext) {
        LOGE("Extractor: Failed to allocate FfmpegExtractorIOContext");
        return AVERROR(ENOMEM);
    }

    env->GetJavaVM(&context->ioJavaContext->javaVm);
    context->ioJavaContext->extractorInputRef = env->NewGlobalRef(jExtractorInput);
    if (!context->ioJavaContext->extractorInputRef) {
        LOGE("Extractor: Failed to create global ref for ExtractorInput");
        av_free(context->ioJavaContext); context->ioJavaContext = NULL;
        return AVERROR(ENOMEM);
    }

    jclass extractorInputClass = env->GetObjectClass(jExtractorInput);
    context->ioJavaContext->readMethodId = env->GetMethodID(extractorInputClass, "read", "([BII)I");
    context->ioJavaContext->getPositionMethodId = env->GetMethodID(extractorInputClass, "getPosition", "()J");
    context->ioJavaContext->getLengthMethodId = env->GetMethodID(extractorInputClass, "getLength", "()J");
    env->DeleteLocalRef(extractorInputClass);

    if (!context->ioJavaContext->readMethodId || !context->ioJavaContext->getPositionMethodId || !context->ioJavaContext->getLengthMethodId) {
        LOGE("Extractor: Failed to get ExtractorInput method IDs (read:%p, getPos:%p, getLen:%p)",
            (void*)context->ioJavaContext->readMethodId, (void*)context->ioJavaContext->getPositionMethodId, (void*)context->ioJavaContext->getLengthMethodId);
        env->DeleteGlobalRef(context->ioJavaContext->extractorInputRef);
        av_free(context->ioJavaContext); context->ioJavaContext = NULL;
        return AVERROR(EINVAL);
    }

    context->ioJavaContext->avio_ctx_buffer_size = 32 * 1024; // 32KB buffer
    context->ioJavaContext->avio_ctx_buffer = (uint8_t *)av_malloc(context->ioJavaContext->avio_ctx_buffer_size);
    if (!context->ioJavaContext->avio_ctx_buffer) {
        LOGE("Extractor: Failed to allocate AVIO buffer");
        env->DeleteGlobalRef(context->ioJavaContext->extractorInputRef);
        av_free(context->ioJavaContext); context->ioJavaContext = NULL;
        return AVERROR(ENOMEM);
    }

    context->avioContext = avio_alloc_context(
        context->ioJavaContext->avio_ctx_buffer,
        context->ioJavaContext->avio_ctx_buffer_size,
        0, // write_flag = 0 (read-only)
        context->ioJavaContext, // opaque pointer
        read_packet_callback,
        NULL, // write_packet_callback (not needed for reading)
        seek_callback);

    if (!context->avioContext) {
        LOGE("Extractor: Failed to allocate AVIOContext");
        av_free(context->ioJavaContext->avio_ctx_buffer); context->ioJavaContext->avio_ctx_buffer = NULL;
        env->DeleteGlobalRef(context->ioJavaContext->extractorInputRef); context->ioJavaContext->extractorInputRef = NULL;
        av_free(context->ioJavaContext); context->ioJavaContext = NULL;
        return AVERROR(ENOMEM);
    }
    // Check if getLength returns a valid length to determine seekability
    long len = env->CallLongMethod(jExtractorInput, context->ioJavaContext->getLengthMethodId);
    context->avioContext->seekable = (len != -1 && len != 0) ? AVIO_SEEKABLE_NORMAL : 0; // Use C.LENGTH_UNSET or similar for unknown length


    context->formatContext = avformat_alloc_context();
    if (!context->formatContext) {
        LOGE("Extractor: Failed to allocate AVFormatContext");
        // avio_context_free expects AVIOContext**
        AVIOContext* pb_to_free = context->avioContext;
        avio_context_free(&pb_to_free); // This will NULL out pb_to_free
        context->avioContext = NULL;
        av_free(context->ioJavaContext->avio_ctx_buffer); context->ioJavaContext->avio_ctx_buffer = NULL;
        env->DeleteGlobalRef(context->ioJavaContext->extractorInputRef); context->ioJavaContext->extractorInputRef = NULL;
        av_free(context->ioJavaContext); context->ioJavaContext = NULL;
        return AVERROR(ENOMEM);
    }
    context->formatContext->pb = context->avioContext;
    context->formatContext->flags |= AVFMT_FLAG_CUSTOM_IO;

    // Explicitly find the DSF demuxer
    AVInputFormat *dsf_input_format = av_find_input_format("dsf");
    if (!dsf_input_format) {
        LOGE("Extractor: DSF input format not found. Ensure FFmpeg is compiled with DSF demuxer enabled.");
         avformat_free_context(context->formatContext); context->formatContext = NULL;
        AVIOContext* pb_to_free = context->avioContext;
        avio_context_free(&pb_to_free); context->avioContext = NULL;
        av_free(context->ioJavaContext->avio_ctx_buffer); context->ioJavaContext->avio_ctx_buffer = NULL;
        env->DeleteGlobalRef(context->ioJavaContext->extractorInputRef); context->ioJavaContext->extractorInputRef = NULL;
        av_free(context->ioJavaContext); context->ioJavaContext = NULL;
        return AVERROR_DEMUXER_NOT_FOUND;
    }
    
    int ret = avformat_open_input(&context->formatContext, NULL /* URL is NULL for custom IO */, dsf_input_format, NULL /* &options */);
    if (ret < 0) {
        logError("Extractor: avformat_open_input", ret);
        avformat_free_context(context->formatContext); context->formatContext = NULL;
        AVIOContext* pb_to_free = context->avioContext;
        avio_context_free(&pb_to_free); context->avioContext = NULL;
        av_free(context->ioJavaContext->avio_ctx_buffer); context->ioJavaContext->avio_ctx_buffer = NULL;
        env->DeleteGlobalRef(context->ioJavaContext->extractorInputRef); context->ioJavaContext->extractorInputRef = NULL;
        av_free(context->ioJavaContext); context->ioJavaContext = NULL;
        return ret;
    }

    ret = avformat_find_stream_info(context->formatContext, NULL);
    if (ret < 0) {
        logError("Extractor: avformat_find_stream_info", ret);
        avformat_close_input(&context->formatContext); // This should free formatContext and ioJavaContext->avio_ctx_buffer if set by avformat
        // If avio_context was manually set, we need to free it and its buffer
        if (context->avioContext) {
             AVIOContext* pb_to_free = context->avioContext;
             avio_context_free(&pb_to_free); // This will NULL out pb_to_free
             context->avioContext = NULL;
        }
        if (context->ioJavaContext) {
            if (context->ioJavaContext->avio_ctx_buffer) {
                 av_free(context->ioJavaContext->avio_ctx_buffer);
                 context->ioJavaContext->avio_ctx_buffer = NULL;
            }
            if (context->ioJavaContext->extractorInputRef) {
                env->DeleteGlobalRef(context->ioJavaContext->extractorInputRef);
                context->ioJavaContext->extractorInputRef = NULL;
            }
            av_free(context->ioJavaContext);
            context->ioJavaContext = NULL;
        }
        return ret;
    }
    LOGD("Extractor: nativeOpenInput successful.");
    return 0;
}

EXTRACTOR_FUNC(jobject, nativeGetStreamFormat, jlong contextHandle, jint streamIndex) {
    NativeExtractorContext *context = (NativeExtractorContext *)contextHandle;
    if (!context || !context->formatContext || streamIndex < 0 || (unsigned int)streamIndex >= context->formatContext->nb_streams) {
        LOGE("Extractor: nativeGetStreamFormat: Invalid context or stream index %d (nb_streams %u)", streamIndex, context && context->formatContext ? context->formatContext->nb_streams : 0);
        return NULL;
    }

    AVStream *stream = context->formatContext->streams[streamIndex];
    AVCodecParameters *codecpar = stream->codecpar;

    const char* codec_name_str = avcodec_get_name(codecpar->codec_id);
    jstring jCodecName = env->NewStringUTF(codec_name_str ? codec_name_str : "unknown");

    jbyteArray jExtraData = NULL;
    if (codecpar->extradata_size > 0 && codecpar->extradata) {
        jExtraData = env->NewByteArray(codecpar->extradata_size);
        if (jExtraData) { // Check allocation
            env->SetByteArrayRegion(jExtraData, 0, codecpar->extradata_size, (const jbyte *)codecpar->extradata);
        } else {
            LOGE("Extractor: Failed to allocate jbyteArray for extradata");
            if(jCodecName) env->DeleteLocalRef(jCodecName);
            return NULL; // Or handle error appropriately
        }
    }

    jobject streamInfoObj = env->NewObject(context->streamInfoClassRef, context->streamInfoConstructor,
                                         jCodecName,
                                         codecpar->channels,
                                         codecpar->sample_rate,
                                         (jint)codecpar->bit_rate, // bit_rate is int64_t, cast to jint
                                         jExtraData);

    if(jCodecName) env->DeleteLocalRef(jCodecName);
    if(jExtraData) env->DeleteLocalRef(jExtraData);

    if (!streamInfoObj) {
        LOGE("Extractor: Failed to create FfmpegStreamInfo java object.");
        // Consider if env->ExceptionCheck() is needed here
    }
    return streamInfoObj;
}

EXTRACTOR_FUNC(jobject, nativeReadPacket, jlong contextHandle, jobject jOutputBuffer) {
    NativeExtractorContext *context = (NativeExtractorContext *)contextHandle;
    jboolean is_eof_java = JNI_FALSE;
    jboolean is_error_java = JNI_FALSE;
    jint error_code_java = 0;
    jint stream_index_java = 0;
    jlong pts_us_java = -9223372036854775807LL; // C.TIME_UNSET
    jint flags_java = 0;
    jint size_java = 0;

    if (!context || !context->formatContext || !context->packet || !jOutputBuffer) {
        LOGE("Extractor: nativeReadPacket: Invalid context, packet, or outputBuffer");
        is_error_java = JNI_TRUE;
        error_code_java = AVERROR(EINVAL);
    } else {
        av_packet_unref(context->packet); // Important to unref before reading a new frame
        int read_ret = av_read_frame(context->formatContext, context->packet);

        if (read_ret < 0) {
            if (read_ret == AVERROR_EOF) {
                is_eof_java = JNI_TRUE;
                 LOGD("Extractor: av_read_frame returned EOF");
            } else {
                logError("Extractor: av_read_frame", read_ret);
                is_error_java = JNI_TRUE;
                error_code_java = read_ret;
            }
        } else {
            stream_index_java = context->packet->stream_index;
            size_java = context->packet->size;

            if (context->packet->pts != AV_NOPTS_VALUE) {
                AVRational tb = context->formatContext->streams[stream_index_java]->time_base;
                // Ensure tb.den is not zero to prevent division by zero
                if (tb.den != 0) {
                    pts_us_java = av_rescale_q(context->packet->pts, tb, {1, 1000000});
                } else {
                    LOGW("Extractor: Stream time_base denominator is zero for stream %d", stream_index_java);
                    // pts_us_java remains C.TIME_UNSET
                }
            }
            // else pts_us_java remains C.TIME_UNSET

            if (context->packet->flags & AV_PKT_FLAG_KEY) {
                flags_java |= 0x1; // Assuming C.BUFFER_FLAG_KEY_FRAME is 1
            }

            uint8_t *outputBufferPtr = (uint8_t *)env->GetDirectBufferAddress(jOutputBuffer);
            if (!outputBufferPtr && size_java > 0) {
                 LOGE("Extractor: nativeReadPacket: Output buffer is not direct or null, but packet has data.");
                 is_error_java = JNI_TRUE;
                 error_code_java = AVERROR_EXTERNAL; // Or a more appropriate error
                 size_java = 0; // Don't attempt to copy if buffer is invalid
            } else if (outputBufferPtr && size_java > 0) {
                jlong bufferCapacity = env->GetDirectBufferCapacity(jOutputBuffer);
                if (bufferCapacity < size_java) {
                    LOGE("Extractor: nativeReadPacket: Output buffer too small (cap %lld, need %d)", (long long)bufferCapacity, size_java);
                    is_error_java = JNI_TRUE;
                    error_code_java = AVERROR(ENOMEM);
                    size_java = 0; // Don't attempt to copy if buffer is too small
                } else {
                    memcpy(outputBufferPtr, context->packet->data, size_java);
                }
            }
            // No action needed if size_java is 0
        }
    }

    jobject packetInfoObj = env->NewObject(context->packetInfoClassRef, context->packetInfoConstructor,
                                           stream_index_java, pts_us_java, flags_java, size_java,
                                           is_eof_java, is_error_java, error_code_java);
    if (!packetInfoObj && !is_eof_java && !is_error_java) { // Only log error if it wasn't already an error/EOF
        LOGE("Extractor: Failed to create FfmpegPacketInfo java object.");
        // If NewObject fails, an exception is pending. We should probably return something
        // that indicates this failure to the Java side, or clear the exception and set error flags.
        // For simplicity here, we'll let the Java side handle a null return if it's not EOF/error.
    }
    return packetInfoObj;
}

EXTRACTOR_FUNC(jint, nativeSeekTo, jlong contextHandle, jlong timeUs) {
    NativeExtractorContext *context = (NativeExtractorContext *)contextHandle;
    if (!context || !context->formatContext) {
        LOGE("Extractor: nativeSeekTo: Invalid context");
        return AVERROR(EINVAL);
    }

    int default_stream_idx = -1;
    // Find the first audio stream to use its time_base for seeking
    for (unsigned int i = 0; i < context->formatContext->nb_streams; i++) {
        if (context->formatContext->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            default_stream_idx = i;
            break;
        }
    }
    // If no audio stream, try to find any default stream
    if (default_stream_idx < 0) {
      default_stream_idx = av_find_default_stream_index(context->formatContext);
      if (default_stream_idx < 0) {
          // If still no default stream, try stream 0 if it exists
          if (context->formatContext->nb_streams > 0) {
              default_stream_idx = 0;
          } else {
              LOGE("Extractor: nativeSeekTo: No streams found in the format context.");
              return AVERROR_STREAM_NOT_FOUND;
          }
      }
    }


    if ((unsigned int)default_stream_idx >= context->formatContext->nb_streams) {
        LOGE("Extractor: nativeSeekTo: Default stream index %d out of bounds (nb_streams %u).", default_stream_idx, context->formatContext->nb_streams);
        return AVERROR_STREAM_NOT_FOUND;
    }

    AVRational tb = context->formatContext->streams[default_stream_idx]->time_base;
    if (tb.den == 0) { // Avoid division by zero
        LOGE("Extractor: nativeSeekTo: Invalid time_base for stream %d.", default_stream_idx);
        return AVERROR(EINVAL);
    }
    long long target_ts_in_stream_tb = av_rescale_q(timeUs, {1, 1000000}, tb);

    // AVSEEK_FLAG_BACKWARD seeks to the keyframe at or before the target timestamp.
    // AVSEEK_FLAG_ANY allows seeking to non-keyframes but might be slower or less accurate.
    // For many formats, AVSEEK_FLAG_BACKWARD is preferred for starting playback.
    int ret = av_seek_frame(context->formatContext, default_stream_idx, target_ts_in_stream_tb, AVSEEK_FLAG_BACKWARD);
    if (ret < 0) {
        logError("Extractor: av_seek_frame", ret);
    } else {
        LOGD("Extractor: nativeSeekTo successful for timeUs %" PRId64 " (ts %" PRId64 ")", timeUs, target_ts_in_stream_tb);
    }
    return ret;
}

EXTRACTOR_FUNC(jlong, nativeGetDurationUs, jlong contextHandle) {
    NativeExtractorContext *context = (NativeExtractorContext *)contextHandle;
    if (!context || !context->formatContext) {
        return -9223372036854775807LL; // C.TIME_UNSET
    }
    // AVFormatContext.duration is in AV_TIME_BASE units, which is microseconds by default.
    if (context->formatContext->duration == AV_NOPTS_VALUE || context->formatContext->duration <=0) {
        return -9223372036854775807LL; // C.TIME_UNSET
    }
    return context->formatContext->duration;
}

EXTRACTOR_FUNC(jboolean, nativeIsSeekable, jlong contextHandle) {
    NativeExtractorContext *context = (NativeExtractorContext *)contextHandle;
    if (!context || !context->formatContext || !context->avioContext) {
        return JNI_FALSE;
    }
    // A stream is generally seekable if it has a known duration and the IO context supports seeking.
    // FFmpeg's avformat_seek_file also considers if the format itself supports seeking.
    // Here, we simplify: if duration is known and AVIO says it's seekable, we assume true.
    // avformat_seek_file will ultimately determine if a specific seek operation is possible.
    if (context->formatContext->duration > 0 && (context->avioContext->seekable & AVIO_SEEKABLE_NORMAL)) {
        return JNI_TRUE;
    }
    return JNI_FALSE;
}

EXTRACTOR_FUNC(void, nativeReleaseExtractorContext, jlong contextHandle) {
    NativeExtractorContext *context = (NativeExtractorContext *)contextHandle;
    LOGD("Extractor: nativeReleaseExtractorContext called for handle: %p", context);
    if (!context) {
        return;
    }

    if (context->formatContext) {
        // avformat_close_input will also close the AVIOContext if it's owned by the AVFormatContext.
        // Since we set formatContext->pb = avioContext, and formatContext->flags |= AVFMT_FLAG_CUSTOM_IO,
        // avformat_close_input should NOT try to close the pb itself.
        // We need to free the AVIOContext buffer and the AVIOContext itself manually *after*
        // avformat_close_input has finished using it.
        avformat_close_input(&context->formatContext); // formatContext is set to NULL by this
    }

    // Free the AVIOContext and its buffer (if we allocated it)
    if (context->avioContext) {
        // The buffer context->ioJavaContext->avio_ctx_buffer was passed to avio_alloc_context.
        // The documentation states that if the buffer is user-supplied, it's the user's
        // responsibility to free it. avio_context_free will free the AVIOContext struct itself.
        if (context->ioJavaContext && context->ioJavaContext->avio_ctx_buffer) {
             av_free(context->ioJavaContext->avio_ctx_buffer);
             context->ioJavaContext->avio_ctx_buffer = NULL;
        }
        AVIOContext *pb_to_free = context->avioContext; // avio_context_free expects AVIOContext**
        avio_context_free(&pb_to_free); // This will NULL out pb_to_free if successful
        context->avioContext = NULL;
    }


    if (context->ioJavaContext) {
        if (context->ioJavaContext->extractorInputRef) {
            env->DeleteGlobalRef(context->ioJavaContext->extractorInputRef);
            context->ioJavaContext->extractorInputRef = NULL;
        }
        // avio_ctx_buffer should have been freed above with avioContext
        av_free(context->ioJavaContext);
        context->ioJavaContext = NULL;
    }

    if (context->packet) {
        av_packet_free(&context->packet);
        context->packet = NULL;
    }

    if (context->streamInfoClassRef) {
        env->DeleteGlobalRef(context->streamInfoClassRef);
        context->streamInfoClassRef = NULL;
    }
    if (context->packetInfoClassRef) {
        env->DeleteGlobalRef(context->packetInfoClassRef);
        context->packetInfoClassRef = NULL;
    }

    av_free(context);
    LOGD("Extractor: nativeReleaseExtractorContext finished.");
}
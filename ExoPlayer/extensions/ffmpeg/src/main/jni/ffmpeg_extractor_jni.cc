#include <android/log.h>
#include <jni.h>
#include <stdlib.h>
#include <string.h> // For memset

extern "C" {
#ifdef __cplusplus
#define __STDC_CONSTANT_MACROS
#ifdef _STDINT_H
#undef _STDINT_H
#endif
#include <stdint.h>
#endif
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h> // For avcodec_get_name and AVCodecID
#include <libavutil/error.h>
#include <libavutil/log.h>    // For av_log_set_callback if NDEBUG is not defined
#include <libavutil/time.h>   // For AV_TIME_BASE_Q
}

#define LOG_TAG "ffmpeg_extractor_jni"
#define LOGE(...) ((void)__android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__))

#ifdef NDEBUG
#define LOGD(...)
#else
#define LOGD(...) ((void)__android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__))
#endif

#define FFMPEG_EXTRACTOR_FUNC(RETURN_TYPE, NAME, ...)                             \
    extern "C"                                                                    \
    {                                                                             \
        JNIEXPORT RETURN_TYPE                                                     \
            Java_com_google_android_exoplayer2_ext_ffmpeg_FfmpegExtractor_##NAME( \
                JNIEnv *env, jobject thiz, ##__VA_ARGS__);                        \
    }                                                                             \
    JNIEXPORT RETURN_TYPE                                                         \
        Java_com_google_android_exoplayer2_ext_ffmpeg_FfmpegExtractor_##NAME(     \
            JNIEnv *env, jobject thiz, ##__VA_ARGS__)

#define AVIO_BUFFER_SIZE (32 * 1024) // 32KB AVIO buffer

// Structure to hold JNI references needed by AVIO callbacks
typedef struct JniCallbackHelper {
    JavaVM *vm;
    jobject ffmpeg_extractor_instance_global_ref;
    jmethodID read_method_id;
    jmethodID seek_method_id;
    jmethodID get_length_method_id;
    jbyteArray read_buffer_global_ref; // Reusable buffer for reading from Java to C
} JniCallbackHelper;

struct FfmpegDemuxContext {
    AVFormatContext *format_ctx;
    AVIOContext *avio_ctx;
    int audio_stream_index;
    AVStream *audio_stream;
    JniCallbackHelper *jni_helper;
    uint8_t *avio_internal_buffer; // Buffer for avio_alloc_context
};

// Global JavaVM pointer, initialized in JNI_OnLoad
static JavaVM *g_vm = NULL;

// ExoPlayer's C.TIME_UNSET value
const int64_t EXO_C_TIME_UNSET = -9223372036854775807LL;

JNIEXPORT jint JNI_OnLoad(JavaVM *vm, void *reserved) {
    g_vm = vm;
    JNIEnv *env;
    if (vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) != JNI_OK) {
        return JNI_ERR;
    }
    LOGD("ffmpeg_extractor_jni JNI_OnLoad successful");
    return JNI_VERSION_1_6;
}

void log_error(const char *func_name, int error_no) {
    char buffer[AV_ERROR_MAX_STRING_SIZE]; // Use AV_ERROR_MAX_STRING_SIZE for safety
    av_strerror(error_no, buffer, sizeof(buffer));
    LOGE("Error in %s: %s (code: %d, FFmpeg error: %d)", func_name, buffer, error_no, error_no);
}

static int avio_read_packet_callback(void *opaque, uint8_t *buf, int buf_size) {
    JniCallbackHelper *helper = (JniCallbackHelper *)opaque;
    JNIEnv *env;
    int get_env_stat = helper->vm->GetEnv((void **)&env, JNI_VERSION_1_6);
    bool attached_thread = false;

    if (get_env_stat == JNI_EDETACHED) {
        if (helper->vm->AttachCurrentThread(&env, NULL) != 0) {
            LOGE("AVIO Read: Failed to attach current thread");
            return AVERROR(EIO);
        }
        attached_thread = true;
    } else if (get_env_stat != JNI_OK) {
        LOGE("AVIO Read: Failed to get JNIEnv, status: %d", get_env_stat);
        return AVERROR(EIO);
    }

    if (!helper->ffmpeg_extractor_instance_global_ref || !helper->read_method_id || !helper->read_buffer_global_ref) {
        LOGE("AVIO Read: JNI helper not properly initialized");
        if (attached_thread) helper->vm->DetachCurrentThread();
        return AVERROR(EINVAL);
    }
    
    jint bytes_read = env->CallIntMethod(helper->ffmpeg_extractor_instance_global_ref,
                                         helper->read_method_id,
                                         helper->read_buffer_global_ref, 0, buf_size);

    if (env->ExceptionCheck()) {
        LOGE("AVIO Read: Exception occurred calling Java read method");
        env->ExceptionDescribe();
        env->ExceptionClear();
        if (attached_thread) helper->vm->DetachCurrentThread();
        return AVERROR(EIO);
    }

    int result;
    if (bytes_read < 0) { // ExoPlayer C.RESULT_END_OF_INPUT is -1
        result = AVERROR_EOF;
    } else if (bytes_read == 0) { // Java read 0 bytes, not EOF
        result = 0;
    } else {
        env->GetByteArrayRegion(helper->read_buffer_global_ref, 0, bytes_read, (jbyte *)buf);
        if (env->ExceptionCheck()) {
            LOGE("AVIO Read: Exception occurred in GetByteArrayRegion");
            env->ExceptionDescribe();
            env->ExceptionClear();
            if (attached_thread) helper->vm->DetachCurrentThread();
            return AVERROR(EIO);
        }
        result = bytes_read;
    }

    if (attached_thread) {
        helper->vm->DetachCurrentThread();
    }
    return result;
}

static int64_t avio_seek_callback(void *opaque, int64_t offset, int whence) {
    JniCallbackHelper *helper = (JniCallbackHelper *)opaque;
    JNIEnv *env;
    int get_env_stat = helper->vm->GetEnv((void **)&env, JNI_VERSION_1_6);
    bool attached_thread = false;

    if (get_env_stat == JNI_EDETACHED) {
        if (helper->vm->AttachCurrentThread(&env, NULL) != 0) {
            LOGE("AVIO Seek: Failed to attach current thread");
            return AVERROR(EIO);
        }
        attached_thread = true;
    } else if (get_env_stat != JNI_OK) {
        LOGE("AVIO Seek: Failed to get JNIEnv, status: %d", get_env_stat);
        return AVERROR(EIO);
    }

    if (!helper->ffmpeg_extractor_instance_global_ref) {
        LOGE("AVIO Seek: JNI helper (instance) not properly initialized");
        if (attached_thread) helper->vm->DetachCurrentThread();
        return AVERROR(EINVAL);
    }

    jlong new_position_or_size;
    if (whence == AVSEEK_SIZE) {
        if (!helper->get_length_method_id) {
             LOGE("AVIO Seek: get_length_method_id not initialized");
             if (attached_thread) helper->vm->DetachCurrentThread();
             return AVERROR(EINVAL);
        }
        new_position_or_size = env->CallLongMethod(helper->ffmpeg_extractor_instance_global_ref, helper->get_length_method_id);
    } else {
        if (!helper->seek_method_id) {
            LOGE("AVIO Seek: seek_method_id not initialized");
            if (attached_thread) helper->vm->DetachCurrentThread();
            return AVERROR(EINVAL);
        }
        new_position_or_size = env->CallLongMethod(helper->ffmpeg_extractor_instance_global_ref,
                                                 helper->seek_method_id,
                                                 offset, whence);
    }
    
    if (env->ExceptionCheck()) {
        LOGE("AVIO Seek: Exception occurred calling Java seek/getLength method");
        env->ExceptionDescribe();
        env->ExceptionClear();
        if (attached_thread) helper->vm->DetachCurrentThread();
        return AVERROR(EIO);
    }

    if (attached_thread) {
        helper->vm->DetachCurrentThread();
    }
    return new_position_or_size;
}

static void demux_context_free(FfmpegDemuxContext *ctx) {
    if (!ctx) return;
    LOGD("demux_context_free: Freeing FfmpegDemuxContext %p", ctx);

    if (ctx->format_ctx) {
        LOGD("demux_context_free: Closing AVFormatContext %p", ctx->format_ctx);
        avformat_close_input(&ctx->format_ctx); 
        ctx->avio_ctx = NULL; 
        ctx->avio_internal_buffer = NULL; 
    } else {
        if (ctx->avio_ctx) {
            LOGD("demux_context_free: Freeing AVIOContext %p (format_ctx was null)", ctx->avio_ctx);
            if (ctx->avio_internal_buffer) { 
                 av_free(ctx->avio_internal_buffer); 
                 ctx->avio_internal_buffer = NULL;
            }
            avio_context_free(&ctx->avio_ctx); 
            ctx->avio_ctx = NULL;
        } else if (ctx->avio_internal_buffer) {
            LOGD("demux_context_free: Freeing avio_internal_buffer %p (avio_ctx was null)", ctx->avio_internal_buffer);
            av_free(ctx->avio_internal_buffer);
            ctx->avio_internal_buffer = NULL;
        }
    }

    if (ctx->jni_helper) {
        JNIEnv *env = NULL;
        bool attached_thread = false;
        int get_env_stat = ctx->jni_helper->vm->GetEnv((void **)&env, JNI_VERSION_1_6);
        
        if (get_env_stat == JNI_EDETACHED) {
            if (ctx->jni_helper->vm->AttachCurrentThread(&env, NULL) == 0) {
                attached_thread = true;
            } else {
                LOGE("demux_context_free: Failed to attach current thread to free global refs");
                env = NULL; 
            }
        } else if (get_env_stat != JNI_OK) {
            LOGE("demux_context_free: Failed to get JNIEnv for freeing global refs, status: %d", get_env_stat);
            env = NULL;
        }
        
        if (env) { 
            LOGD("demux_context_free: Deleting global refs for JNI helper %p", ctx->jni_helper);
            if (ctx->jni_helper->ffmpeg_extractor_instance_global_ref) {
                env->DeleteGlobalRef(ctx->jni_helper->ffmpeg_extractor_instance_global_ref);
                ctx->jni_helper->ffmpeg_extractor_instance_global_ref = NULL;
            }
            if (ctx->jni_helper->read_buffer_global_ref) {
                env->DeleteGlobalRef(ctx->jni_helper->read_buffer_global_ref);
                ctx->jni_helper->read_buffer_global_ref = NULL;
            }
        } else {
            LOGE("demux_context_free: env is NULL, cannot delete global refs for helper %p.", ctx->jni_helper);
        }

        if (attached_thread) {
            ctx->jni_helper->vm->DetachCurrentThread();
        }
        free(ctx->jni_helper);
        ctx->jni_helper = NULL;
    }

    free(ctx);
    LOGD("demux_context_free: Context %p itself freed", ctx);
}

// Corrected: Removed explicit jobject thiz from parameters as macro provides it.
FFMPEG_EXTRACTOR_FUNC(jlong, nativeCreateContext) {
    FfmpegDemuxContext *ctx = (FfmpegDemuxContext *)calloc(1, sizeof(FfmpegDemuxContext));
    if (!ctx) {
        LOGE("nativeCreateContext: Failed to allocate FfmpegDemuxContext");
        return 0L;
    }
    ctx->audio_stream_index = -1;

    ctx->jni_helper = (JniCallbackHelper *)calloc(1, sizeof(JniCallbackHelper));
    if (!ctx->jni_helper) {
        LOGE("nativeCreateContext: Failed to allocate JniCallbackHelper");
        demux_context_free(ctx);
        return 0L;
    }
    ctx->jni_helper->vm = g_vm;
    ctx->jni_helper->ffmpeg_extractor_instance_global_ref = env->NewGlobalRef(thiz); // thiz is from macro
    if (!ctx->jni_helper->ffmpeg_extractor_instance_global_ref) {
        LOGE("nativeCreateContext: Failed to create global ref for FfmpegExtractor instance");
        demux_context_free(ctx);
        return 0L;
    }

    jclass extractor_clazz = env->GetObjectClass(thiz); // thiz is from macro
    if (!extractor_clazz) {
        LOGE("nativeCreateContext: Failed to get FfmpegExtractor class");
        demux_context_free(ctx);
        return 0L;
    }

    ctx->jni_helper->read_method_id = env->GetMethodID(extractor_clazz, "readFromExtractorInput", "([BII)I");
    ctx->jni_helper->seek_method_id = env->GetMethodID(extractor_clazz, "seekInExtractorInput", "(JI)J");
    ctx->jni_helper->get_length_method_id = env->GetMethodID(extractor_clazz, "getLengthFromExtractorInput", "()J");

    if (!ctx->jni_helper->read_method_id || !ctx->jni_helper->seek_method_id || !ctx->jni_helper->get_length_method_id) {
        LOGE("nativeCreateContext: Failed to get JNI method IDs. read_method_id: %p, seek_method_id: %p, get_length_method_id: %p",
             ctx->jni_helper->read_method_id, ctx->jni_helper->seek_method_id, ctx->jni_helper->get_length_method_id);
        env->DeleteLocalRef(extractor_clazz);
        demux_context_free(ctx);
        return 0L;
    }
    env->DeleteLocalRef(extractor_clazz);

    jbyteArray local_read_buffer = env->NewByteArray(AVIO_BUFFER_SIZE);
    if (!local_read_buffer) {
        LOGE("nativeCreateContext: Failed to create local jbyteArray for read buffer");
        demux_context_free(ctx);
        return 0L;
    }
    ctx->jni_helper->read_buffer_global_ref = (jbyteArray)env->NewGlobalRef(local_read_buffer);
    env->DeleteLocalRef(local_read_buffer);
    if (!ctx->jni_helper->read_buffer_global_ref) {
        LOGE("nativeCreateContext: Failed to create global ref for read buffer");
        demux_context_free(ctx);
        return 0L;
    }

    ctx->avio_internal_buffer = (uint8_t *)av_malloc(AVIO_BUFFER_SIZE);
    if (!ctx->avio_internal_buffer) {
        LOGE("nativeCreateContext: Failed to allocate AVIO internal buffer");
        demux_context_free(ctx);
        return 0L;
    }

    ctx->avio_ctx = avio_alloc_context(
        ctx->avio_internal_buffer,
        AVIO_BUFFER_SIZE,
        0, 
        ctx->jni_helper, 
        avio_read_packet_callback,
        NULL, 
        avio_seek_callback);

    if (!ctx->avio_ctx) {
        LOGE("nativeCreateContext: Failed to allocate AVIOContext");
        demux_context_free(ctx); 
        return 0L;
    }
    
    ctx->format_ctx = avformat_alloc_context();
    if (!ctx->format_ctx) {
        LOGE("nativeCreateContext: Failed to allocate AVFormatContext");
        demux_context_free(ctx);
        return 0L;
    }

    ctx->format_ctx->pb = ctx->avio_ctx;
    ctx->format_ctx->flags |= AVFMT_FLAG_CUSTOM_IO;
    // ctx->format_ctx->probesize = 2000000; // Example: 2MB probe size
    // ctx->format_ctx->max_analyze_duration = 5 * AV_TIME_BASE;


    int ret = avformat_open_input(&ctx->format_ctx, NULL, NULL, NULL);
    if (ret < 0) {
        log_error("avformat_open_input", ret);
        demux_context_free(ctx); 
        return 0L;
    }

    ret = avformat_find_stream_info(ctx->format_ctx, NULL);
    if (ret < 0) {
        log_error("avformat_find_stream_info", ret);
        demux_context_free(ctx);
        return 0L;
    }

    for (unsigned int i = 0; i < ctx->format_ctx->nb_streams; i++) {
        if (ctx->format_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            ctx->audio_stream_index = (int)i;
            ctx->audio_stream = ctx->format_ctx->streams[i];
            LOGD("nativeCreateContext: Found audio stream at index %u, codec: %s (ID: %d)",
                 i, avcodec_get_name(ctx->audio_stream->codecpar->codec_id), ctx->audio_stream->codecpar->codec_id);
            break;
        }
    }

    if (ctx->audio_stream_index == -1) {
        LOGE("nativeCreateContext: No audio stream found in input");
        demux_context_free(ctx);
        return 0L;
    }
    
    LOGD("nativeCreateContext successful, context: %p", ctx);
    return (jlong)ctx;
}

FFMPEG_EXTRACTOR_FUNC(jint, nativeGetAudioCodecId, jlong context) {
  FfmpegDemuxContext *ctx = (FfmpegDemuxContext *) context;
  if (!ctx || !ctx->audio_stream) { 
    LOGE("nativeGetAudioCodecId: Invalid context or no audio stream");
    return AV_CODEC_ID_NONE; 
  }
  return (jint) ctx->audio_stream->codecpar->codec_id;
}

FFMPEG_EXTRACTOR_FUNC(jint, nativeGetSampleRate, jlong context) {
  FfmpegDemuxContext *ctx = (FfmpegDemuxContext *) context;
  if (!ctx || !ctx->audio_stream) {
     LOGE("nativeGetSampleRate: Invalid context or no audio stream");
    return -1;
  }
  return ctx->audio_stream->codecpar->sample_rate;
}

FFMPEG_EXTRACTOR_FUNC(jint, nativeGetChannelCount, jlong context) {
  FfmpegDemuxContext *ctx = (FfmpegDemuxContext *) context;
  if (!ctx || !ctx->audio_stream) {
    LOGE("nativeGetChannelCount: Invalid context or no audio stream");
    return -1;
  }
  return ctx->audio_stream->codecpar->channels;
}

FFMPEG_EXTRACTOR_FUNC(jlong, nativeGetBitRate, jlong context) {
  FfmpegDemuxContext *ctx = (FfmpegDemuxContext *) context;
  if (!ctx || !ctx->audio_stream) {
    LOGE("nativeGetBitRate: Invalid context or no audio stream");
    return -1;
  }
  return (jlong) ctx->audio_stream->codecpar->bit_rate;
}

FFMPEG_EXTRACTOR_FUNC(jint, nativeGetBlockAlign, jlong context) {
  FfmpegDemuxContext *ctx = (FfmpegDemuxContext *) context;
  if (!ctx || !ctx->audio_stream) {
    LOGE("nativeGetBlockAlign: Invalid context or no audio stream");
    return -1;
  }
  return ctx->audio_stream->codecpar->block_align;
}

FFMPEG_EXTRACTOR_FUNC(jbyteArray, nativeGetExtraData, jlong context) {
  FfmpegDemuxContext *ctx = (FfmpegDemuxContext *) context;
  if (!ctx || !ctx->audio_stream || ctx->audio_stream->codecpar->extradata_size <= 0) {
    return NULL;
  }
  jbyteArray extraData = env->NewByteArray(ctx->audio_stream->codecpar->extradata_size);
  if (!extraData) {
    LOGE("nativeGetExtraData: Failed to allocate jbyteArray");
    return NULL;
  }
  env->SetByteArrayRegion(extraData, 0, ctx->audio_stream->codecpar->extradata_size,
                          (jbyte *)ctx->audio_stream->codecpar->extradata);
  return extraData;
}

FFMPEG_EXTRACTOR_FUNC(jint, nativeReadPacket, jlong context, jbyteArray outputBuffer,
                      jint outputBufferSize, jlongArray timestampOut) {
    FfmpegDemuxContext *ctx = (FfmpegDemuxContext *)context;
    if (!ctx || !ctx->format_ctx || ctx->audio_stream_index == -1 || !outputBuffer) {
        LOGE("nativeReadPacket: Invalid parameters. Context: %p, FormatCtx: %p, StreamIndex: %d",
             ctx, ctx ? ctx->format_ctx : NULL, ctx ? ctx->audio_stream_index : -2);
        return AVERROR(EINVAL);
    }

    AVPacket pkt; // Stack allocated
    // For av_read_frame, it's usually enough that pkt.data is NULL and pkt.size is 0
    // or use av_packet_unref on it if it might have been used before.
    // Since it's a fresh stack variable, zeroing is safest or let av_read_frame initialize.
    // av_read_frame will (re)initialize the packet.
    
    int ret = av_read_frame(ctx->format_ctx, &pkt);
    if (ret < 0) {
        if (ret != AVERROR_EOF) { 
            log_error("av_read_frame", ret);
        } else {
            LOGD("nativeReadPacket: av_read_frame returned EOF");
        }
        // Even on error, if av_read_frame might have touched pkt (e.g. allocated some internal buffer before failing),
        // unref is safer. However, if ret < 0, pkt should not have valid data.
        // For safety and consistency with success path:
        av_packet_unref(&pkt); 
        return ret; 
    }

    if (pkt.stream_index != ctx->audio_stream_index) {
        av_packet_unref(&pkt);
        return 0; 
    }

    if (pkt.size > outputBufferSize) {
        LOGE("nativeReadPacket: Packet size (%d) > outputBufferSize (%d)", pkt.size, outputBufferSize);
        av_packet_unref(&pkt);
        return AVERROR(EINVAL); 
    }

    jlong timestampUs = EXO_C_TIME_UNSET; 
    if (pkt.pts != AV_NOPTS_VALUE) {
        timestampUs = av_rescale_q(pkt.pts, ctx->audio_stream->time_base, AV_TIME_BASE_Q);
    } else if (pkt.dts != AV_NOPTS_VALUE) {
        timestampUs = av_rescale_q(pkt.dts, ctx->audio_stream->time_base, AV_TIME_BASE_Q);
    }
    
    if (timestampOut != NULL) {
        env->SetLongArrayRegion(timestampOut, 0, 1, ×tampUs);
    }

    env->SetByteArrayRegion(outputBuffer, 0, pkt.size, (jbyte *)pkt.data);
    if (env->ExceptionCheck()) {
        LOGE("nativeReadPacket: Exception in SetByteArrayRegion");
        env->ExceptionDescribe();
        env->ExceptionClear();
        av_packet_unref(&pkt);
        return AVERROR(EIO); 
    }

    int packet_size = pkt.size;
    av_packet_unref(&pkt); 
    return packet_size;
}

FFMPEG_EXTRACTOR_FUNC(jlong, nativeGetDuration, jlong context) {
  FfmpegDemuxContext *ctx = (FfmpegDemuxContext *) context;
  if (!ctx || !ctx->format_ctx) {
    LOGE("nativeGetDuration: Invalid context");
    return EXO_C_TIME_UNSET; 
  }
  if (ctx->format_ctx->duration == AV_NOPTS_VALUE || ctx->format_ctx->duration <=0) {
      return EXO_C_TIME_UNSET; 
  }
  return ctx->format_ctx->duration; // Already in microseconds (AV_TIME_BASE units)
}

FFMPEG_EXTRACTOR_FUNC(jboolean, nativeSeek, jlong context, jlong timeUs) {
    FfmpegDemuxContext *ctx = (FfmpegDemuxContext *)context;
    if (!ctx || !ctx->format_ctx || !ctx->audio_stream) { // check audio_stream
        LOGE("nativeSeek: Invalid context or no audio stream for seeking.");
        return JNI_FALSE;
    }

    int64_t target_ts = av_rescale_q(timeUs, AV_TIME_BASE_Q, ctx->audio_stream->time_base);
    LOGD("nativeSeek: Attempting to seek to timeUs=%lld, target_ts=%lld (stream_time_base %d/%d)",
         (long long)timeUs, (long long)target_ts, ctx->audio_stream->time_base.num, ctx->audio_stream->time_base.den);
    
    int ret = av_seek_frame(ctx->format_ctx, ctx->audio_stream_index, target_ts, AVSEEK_FLAG_BACKWARD);

    if (ret < 0) {
        char errbuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errbuf, sizeof(errbuf));
        LOGW("nativeSeek: av_seek_frame (BACKWARD) failed: %s. Trying AVSEEK_FLAG_ANY.", errbuf);
        ret = av_seek_frame(ctx->format_ctx, ctx->audio_stream_index, target_ts, AVSEEK_FLAG_ANY);
        if (ret < 0) {
            av_strerror(ret, errbuf, sizeof(errbuf));
            LOGE("nativeSeek: av_seek_frame (ANY) also failed: %s", errbuf);
            return JNI_FALSE;
        }
        LOGD("nativeSeek: av_seek_frame (AVSEEK_FLAG_ANY) successful.");
    } else {
        LOGD("nativeSeek: av_seek_frame (AVSEEK_FLAG_BACKWARD) successful.");
    }
    return JNI_TRUE;
}

FFMPEG_EXTRACTOR_FUNC(void, nativeReleaseContext, jlong context) {
    FfmpegDemuxContext *ctx = (FfmpegDemuxContext *)context;
    LOGD("nativeReleaseContext called with context: %p", ctx);
    if (ctx) {
        demux_context_free(ctx);
    }
}
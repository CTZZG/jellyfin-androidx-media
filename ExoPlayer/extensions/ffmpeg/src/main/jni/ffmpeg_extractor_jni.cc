#include <android/log.h>
#include <jni.h>
#include <stdlib.h>
#include <string.h>

extern "C" {
#ifdef __cplusplus
#define __STDC_CONSTANT_MACROS
#ifdef _STDINT_H
#undef _STDINT_H
#endif
#include <stdint.h>
#endif
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/log.h>
}

#define LOG_TAG "ffmpeg_extractor_jni"
#define LOGE(...) ((void)__android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__))

#ifdef NDEBUG
#define LOGD(...)
#else
#define LOGD(...) ((void)__android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__))
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

typedef struct JniCallbackHelper {
  JavaVM *vm;
  jobject ffmpeg_extractor_instance_global_ref;
  jmethodID read_method_id;
  jmethodID seek_method_id;
  jmethodID get_length_method_id;
  jbyteArray read_buffer_global_ref;
} JniCallbackHelper;

struct FfmpegDemuxContext {
  AVFormatContext *format_ctx;
  AVIOContext *avio_ctx;
  int audio_stream_index;
  AVStream *audio_stream;
  InputWrapper *input_wrapper;
  JniCallbackHelper *jni_helper;
  uint8_t *avio_internal_buffer;
};

static JavaVM *g_vm = NULL;

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
  char buffer[256];
  av_strerror(error_no, buffer, sizeof(buffer));
  LOGE("Error in %s: %s (code: %d)", func_name, buffer, error_no);
}

static int avio_read_packet_callback(void *opaque, uint8_t *buf, int buf_size) {
  JniCallbackHelper *helper = (JniCallbackHelper *)opaque;
  JNIEnv *env;
  int get_env_stat = helper->vm->GetEnv((void **)&env, JNI_VERSION_1_6);

  if (get_env_stat == JNI_EDETACHED) {
      LOGD("AVIO Read: Attaching current thread to JVM");
      if (helper->vm->AttachCurrentThread(&env, NULL) != 0) {
          LOGE("AVIO Read: Failed to attach current thread");
          return AVERROR(EIO);
      }
  } else if (get_env_stat == JNI_EVERSION) {
      LOGE("AVIO Read: JNI version not supported");
      return AVERROR(EIO);
  }
  if (!helper->ffmpeg_extractor_instance_global_ref || !helper->read_method_id) {
      LOGE("AVIO Read: JNI helper not properly initialized");
      if (get_env_stat == JNI_EDETACHED) helper->vm->DetachCurrentThread();
      return AVERROR(EINVAL);
  }
  
  jint bytes_read = env->CallIntMethod(helper->ffmpeg_extractor_instance_global_ref,
                                       helper->read_method_id,
                                       helper->read_buffer_global_ref, 0, buf_size);

  if (env->ExceptionCheck()) {
      LOGE("AVIO Read: Exception occurred calling Java read method");
      env->ExceptionDescribe();
      env->ExceptionClear();
      if (get_env_stat == JNI_EDETACHED) helper->vm->DetachCurrentThread();
      return AVERROR(EIO);
  }

  if (bytes_read < 0) {
      LOGD("AVIO Read: End of input from Java: %d", bytes_read);
      if (get_env_stat == JNI_EDETACHED) helper->vm->DetachCurrentThread();
      return AVERROR_EOF;
  } else if (bytes_read > 0) {
      env->GetByteArrayRegion(helper->read_buffer_global_ref, 0, bytes_read, (jbyte *)buf);
       if (env->ExceptionCheck()) {
          LOGE("AVIO Read: Exception occurred in GetByteArrayRegion");
          env->ExceptionDescribe();
          env->ExceptionClear();
          if (get_env_stat == JNI_EDETACHED) helper->vm->DetachCurrentThread();
          return AVERROR(EIO);
      }
  }
  if (get_env_stat == JNI_EDETACHED) {
      helper->vm->DetachCurrentThread();
  }
  return bytes_read;
}

static int64_t avio_seek_callback(void *opaque, int64_t offset, int whence) {
  JniCallbackHelper *helper = (JniCallbackHelper *)opaque;
  JNIEnv *env;
  int get_env_stat = helper->vm->GetEnv((void **)&env, JNI_VERSION_1_6);

   if (get_env_stat == JNI_EDETACHED) {
      LOGD("AVIO Seek: Attaching current thread to JVM");
      if (helper->vm->AttachCurrentThread(&env, NULL) != 0) {
          LOGE("AVIO Seek: Failed to attach current thread");
          return AVERROR(EIO);
      }
  } else if (get_env_stat == JNI_EVERSION) {
      LOGE("AVIO Seek: JNI version not supported");
      return AVERROR(EIO);
  }

  if (!helper->ffmpeg_extractor_instance_global_ref) {
      LOGE("AVIO Seek: JNI helper not properly initialized");
      if (get_env_stat == JNI_EDETACHED) helper->vm->DetachCurrentThread();
      return AVERROR(EINVAL);
  }

  if (whence == AVSEEK_SIZE) {
      if (!helper->get_length_method_id) {
           LOGE("AVIO Seek: get_length_method_id not initialized");
           if (get_env_stat == JNI_EDETACHED) helper->vm->DetachCurrentThread();
           return AVERROR(EINVAL);
      }
      jlong length = env->CallLongMethod(helper->ffmpeg_extractor_instance_global_ref, helper->get_length_method_id);
      if (env->ExceptionCheck()) {
          LOGE("AVIO Seek: Exception occurred calling Java getLength method");
          env->ExceptionDescribe();
          env->ExceptionClear();
          if (get_env_stat == JNI_EDETACHED) helper->vm->DetachCurrentThread();
          return AVERROR(EIO);
      }
      if (get_env_stat == JNI_EDETACHED) helper->vm->DetachCurrentThread();
      return length;
  }
  
  if (!helper->seek_method_id) {
      LOGE("AVIO Seek: seek_method_id not initialized");
      if (get_env_stat == JNI_EDETACHED) helper->vm->DetachCurrentThread();
      return AVERROR(EINVAL);
  }

  jlong new_position = env->CallLongMethod(helper->ffmpeg_extractor_instance_global_ref,
                                           helper->seek_method_id,
                                           offset, whence);
  if (env->ExceptionCheck()) {
      LOGE("AVIO Seek: Exception occurred calling Java seek method");
      env->ExceptionDescribe();
      env->ExceptionClear();
      if (get_env_stat == JNI_EDETACHED) helper->vm->DetachCurrentThread();
      return AVERROR(EIO);
  }
  if (get_env_stat == JNI_EDETACHED) {
      helper->vm->DetachCurrentThread();
  }
  return new_position;
}

static void demux_context_free(FfmpegDemuxContext *ctx) {
  if (!ctx) return;

  LOGD("demux_context_free: Freeing FfmpegDemuxContext");
  if (ctx->format_ctx) {
    avformat_close_input(&ctx->format_ctx);
    ctx->format_ctx = NULL;
  }

  if (ctx->avio_internal_buffer) {
    ctx->avio_internal_buffer = NULL;
  }
  if (ctx->avio_ctx) {
    av_freep(&ctx->avio_ctx->buffer);
    avio_context_free(&ctx->avio_ctx);
    ctx->avio_ctx = NULL;
  }

  if (ctx->jni_helper) {
    JNIEnv *env;
    int get_env_stat = ctx->jni_helper->vm->GetEnv((void **)&env, JNI_VERSION_1_6);
    bool attached = false;
    if (get_env_stat == JNI_EDETACHED) {
        if (ctx->jni_helper->vm->AttachCurrentThread(&env, NULL) == 0) {
            attached = true;
        } else {
            LOGE("demux_context_free: Failed to attach current thread to free global refs");
        }
    }
    
    if (env) {
         if (ctx->jni_helper->ffmpeg_extractor_instance_global_ref) {
            env->DeleteGlobalRef(ctx->jni_helper->ffmpeg_extractor_instance_global_ref);
            ctx->jni_helper->ffmpeg_extractor_instance_global_ref = NULL;
        }
        if (ctx->jni_helper->read_buffer_global_ref) {
            env->DeleteGlobalRef(ctx->jni_helper->read_buffer_global_ref);
            ctx->jni_helper->read_buffer_global_ref = NULL;
        }
    }

    if (attached) {
      ctx->jni_helper->vm->DetachCurrentThread();
  }
  free(ctx->jni_helper);
  ctx->jni_helper = NULL;
}

free(ctx);
LOGD("demux_context_free: Done");
}

FFMPEG_EXTRACTOR_FUNC(jlong, nativeCreateContext, jobject thiz) {
  FfmpegDemuxContext *ctx = (FfmpegDemuxContext *)calloc(1, sizeof(FfmpegDemuxContext));
  if (!ctx) {
      LOGE("Failed to allocate FfmpegDemuxContext");
      return 0L;
  }
  ctx->audio_stream_index = -1;

  ctx->jni_helper = (JniCallbackHelper *)calloc(1, sizeof(JniCallbackHelper));
  if (!ctx->jni_helper) {
      LOGE("Failed to allocate JniCallbackHelper");
      demux_context_free(ctx);
      return 0L;
  }
  ctx->jni_helper->vm = g_vm;
  ctx->jni_helper->ffmpeg_extractor_instance_global_ref = env->NewGlobalRef(thiz);
  if (!ctx->jni_helper->ffmpeg_extractor_instance_global_ref) {
      LOGE("Failed to create global ref for FfmpegExtractor instance");
      demux_context_free(ctx);
      return 0L;
  }

  jclass extractor_clazz = env->GetObjectClass(thiz);
  if (!extractor_clazz) {
      LOGE("Failed to get FfmpegExtractor class");
      demux_context_free(ctx);
      return 0L;
  }

  ctx->jni_helper->read_method_id = env->GetMethodID(extractor_clazz, "readFromExtractorInput", "([BII)I");
  ctx->jni_helper->seek_method_id = env->GetMethodID(extractor_clazz, "seekInExtractorInput", "(JI)J");
  ctx->jni_helper->get_length_method_id = env->GetMethodID(extractor_clazz, "getLengthFromExtractorInput", "()J");

  if (!ctx->jni_helper->read_method_id || !ctx->jni_helper->seek_method_id || !ctx->jni_helper->get_length_method_id) {
      LOGE("Failed to get JNI method IDs for FfmpegExtractor callbacks");
      env->DeleteLocalRef(extractor_clazz);
      demux_context_free(ctx);
      return 0L;
  }
  env->DeleteLocalRef(extractor_clazz);

  jbyteArray local_read_buffer = env->NewByteArray(AVIO_BUFFER_SIZE);
  if (!local_read_buffer) {
      LOGE("Failed to create local jbyteArray for read buffer");
      demux_context_free(ctx);
      return 0L;
  }
  ctx->jni_helper->read_buffer_global_ref = (jbyteArray)env->NewGlobalRef(local_read_buffer);
  env->DeleteLocalRef(local_read_buffer);
  if (!ctx->jni_helper->read_buffer_global_ref) {
      LOGE("Failed to create global ref for read buffer");
      demux_context_free(ctx);
      return 0L;
  }


  ctx->avio_internal_buffer = (uint8_t *)av_malloc(AVIO_BUFFER_SIZE);
  if (!ctx->avio_internal_buffer) {
      LOGE("Failed to allocate AVIO internal buffer");
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
      LOGE("Failed to allocate AVIOContext");
      demux_context_free(ctx);
      return 0L;
  }

  ctx->format_ctx = avformat_alloc_context();
  if (!ctx->format_ctx) {
      LOGE("Failed to allocate AVFormatContext");
      demux_context_free(ctx);
      return 0L;
  }

  ctx->format_ctx->pb = ctx->avio_ctx;
  ctx->format_ctx->flags |= AVFMT_FLAG_CUSTOM_IO;

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
          LOGD("Found audio stream at index %d", i);
          LOGD("Audio codec: %s (ID: %d)", avcodec_get_name(ctx->audio_stream->codecpar->codec_id), ctx->audio_stream->codecpar->codec_id);
          break;
      }
  }

  if (ctx->audio_stream_index == -1) {
      LOGE("No audio stream found in input");
      demux_context_free(ctx);
      return 0L;
  }
  
  LOGD("nativeCreateContext successful, context: %p", ctx);
  return (jlong)ctx;
}

FFMPEG_EXTRACTOR_FUNC(jint, nativeGetAudioCodecId, jlong context) {
FfmpegDemuxContext *ctx = (FfmpegDemuxContext *) context;
if (!ctx || !ctx->audio_stream) { // Check audio_stream as well
  LOGE("nativeGetAudioCodecId: Invalid context or no audio stream");
  return AV_CODEC_ID_NONE; // Or some other error indicator
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

  AVPacket packet;
  av_packet_alloc();
  if (!&packet) {
      LOGE("nativeReadPacket: Failed to allocate AVPacket");
      return AVERROR(ENOMEM);
  }
  packet.data = NULL;
  packet.size = 0;

  int ret = av_read_frame(ctx->format_ctx, &packet);
  if (ret < 0) {
      if (ret != AVERROR_EOF) {
          log_error("av_read_frame", ret);
      } else {
          LOGD("nativeReadPacket: av_read_frame returned EOF");
      }
      av_packet_unref(&packet);
      return ret;
  }

  if (packet.stream_index != ctx->audio_stream_index) {
      av_packet_unref(&packet);
      return 0;
  }

  if (packet.size > outputBufferSize) {
      LOGE("nativeReadPacket: Packet size (%d) > outputBufferSize (%d)", packet.size, outputBufferSize);
      av_packet_unref(&packet);
      return AVERROR(EINVAL);
  }

  jlong timestampUs = C_TIME_UNSET;
  if (packet.pts != AV_NOPTS_VALUE) {
      timestampUs = av_rescale_q(packet.pts, ctx->audio_stream->time_base, AV_TIME_BASE_Q);
  } else if (packet.dts != AV_NOPTS_VALUE) {
      timestampUs = av_rescale_q(packet.dts, ctx->audio_stream->time_base, AV_TIME_BASE_Q);
  }

  if (timestampOut != NULL) {
      env->SetLongArrayRegion(timestampOut, 0, 1, ×tampUs);
  }

  env->SetByteArrayRegion(outputBuffer, 0, packet.size, (jbyte *)packet.data);
  if (env->ExceptionCheck()) {
      LOGE("nativeReadPacket: Exception in SetByteArrayRegion");
      env->ExceptionDescribe();
      env->ExceptionClear();
      av_packet_unref(&packet);
      return AVERROR(EIO);
  }

  int packet_size = packet.size;
  av_packet_unref(&packet);
  return packet_size;
}

FFMPEG_EXTRACTOR_FUNC(jlong, nativeGetDuration, jlong context) {
FfmpegDemuxContext *ctx = (FfmpegDemuxContext *) context;
if (!ctx || !ctx->format_ctx) {
  LOGE("nativeGetDuration: Invalid context");
  return -1;
}

if (ctx->format_ctx->duration == AV_NOPTS_VALUE || ctx->format_ctx->duration <=0) {
    return -1;
}
return ctx->format_ctx->duration;
}

FFMPEG_EXTRACTOR_FUNC(jboolean, nativeSeek, jlong context, jlong timeUs) {
  FfmpegDemuxContext *ctx = (FfmpegDemuxContext *)context;
  if (!ctx || !ctx->format_ctx || ctx->audio_stream_index == -1) {
      LOGE("nativeSeek: Invalid context or no audio stream for seeking.");
      return JNI_FALSE;
  }

  int64_t target_ts = av_rescale_q(timeUs, AV_TIME_BASE_Q, ctx->audio_stream->time_base);
  LOGD("nativeSeek: Attempting to seek to timeUs=%lld, target_ts=%lld (stream_time_base %d/%d)",
       timeUs, target_ts, ctx->audio_stream->time_base.num, ctx->audio_stream->time_base.den);

  int ret = av_seek_frame(ctx->format_ctx, ctx->audio_stream_index, target_ts, AVSEEK_FLAG_BACKWARD);

  if (ret < 0) {
      log_error("av_seek_frame", ret);
      ret = av_seek_frame(ctx->format_ctx, ctx->audio_stream_index, target_ts, AVSEEK_FLAG_ANY);
      if (ret < 0) {
          log_error("av_seek_frame (AVSEEK_FLAG_ANY)", ret);
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
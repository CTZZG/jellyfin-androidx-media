#include <android/log.h>
#include <jni.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h> // For usleep (debugging only, remove later)

extern "C"
{
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

#define AVIO_BUFFER_SIZE (32 * 1024) // 32KB AVIO buffer. FFmpeg recommends at least this.

// Mimic some Java constants for easier JNI logic if needed, though direct int passing is fine.
// static const int AVSEEK_SIZE_FLAG_JNI = 0x10000;
// static const int SEEK_SET_JNI = 0;
// static const int SEEK_CUR_JNI = 1;
// static const int SEEK_END_JNI = 2;

// Standard FFmpeg error codes are negative.
// Java side should map C.RESULT_END_OF_INPUT to AVERROR_EOF for reads.
// static const int AVERROR_EOF_JNI = AVERROR_EOF; // -541478725
// static const int AVERROR_EIO_JNI = AVERROR(EIO); // Typically -5
// static const int AVERROR_ENOSYS_JNI = AVERROR(ENOSYS); // Typically -38


struct FfmpegDemuxContext {
  AVFormatContext *format_ctx;
  AVIOContext *avio_ctx;
  int audio_stream_index;
  AVStream *audio_stream;
  // --- JNI callback related fields ---
  JavaVM* jvm;
  jobject extractor_java_ref; // Global ref to FfmpegExtractor instance
  jmethodID read_method_id;
  jmethodID seek_method_id;
  // No need for java_byte_buffer here, created on stack in callback
  uint8_t *avio_internal_buffer; // Buffer allocated for AVIOContext
};

void log_error_msg(const char *func_name, int error_no, const char* custom_msg) {
  char buffer[256];
  av_strerror(error_no, buffer, sizeof(buffer));
  if (custom_msg) {
    LOGE("Error in %s: %s (%s)", func_name, buffer, custom_msg);
  } else {
    LOGE("Error in %s: %s", func_name, buffer);
  }
}


// Forward declarations for callbacks
static int avio_read_packet_callback_jni(void *opaque, uint8_t *buf, int buf_size);
static int64_t avio_seek_callback_jni(void *opaque, int64_t offset, int whence);


static void demux_context_free(FfmpegDemuxContext *ctx) {
  JNIEnv *env = nullptr;
  bool attached_here = false;
  if (ctx->jvm) {
    int get_env_stat = ctx->jvm->GetEnv((void **)&env, JNI_VERSION_1_6);
    if (get_env_stat == JNI_EDETACHED) {
      LOGD("demux_context_free: Attaching to JNIEnv to free global ref.");
      if (ctx->jvm->AttachCurrentThread(&env, NULL) == 0) {
        attached_here = true;
      } else {
        LOGE("demux_context_free: Failed to attach to JNIEnv. Global ref will leak.");
        env = nullptr; // Ensure env is not used if attach failed
      }
    } else if (get_env_stat != JNI_OK) {
        LOGE("demux_context_free: GetEnv failed with %d. Global ref might leak.", get_env_stat);
        env = nullptr;
    }
  }

  LOGD("demux_context_free: Releasing AVFormatContext.");
  if (ctx->format_ctx) {
    // avformat_close_input will also free the AVIOContext if it was opened by avformat_open_input
    // and if AVFMT_FLAG_CUSTOM_IO is not set.
    // If AVFMT_FLAG_CUSTOM_IO is set, we need to free the AVIOContext ourselves *before* this.
    // However, avformat_close_input should handle format_ctx->pb (our avio_ctx) correctly.
    // Let's free AVIOContext first to be safe with custom IO.
    if (ctx->avio_ctx) {
        LOGD("demux_context_free: Freeing AVIOContext buffer and AVIOContext.");
        // The buffer for AVIOContext was allocated by us (av_malloc).
        // avio_context_free does not free the buffer if it was supplied externally.
        // So, we must free avio_internal_buffer.
        if (ctx->avio_ctx->buffer) { // This should be our avio_internal_buffer
            av_free(ctx->avio_ctx->buffer); // or av_freep(&ctx->avio_ctx->buffer);
            ctx->avio_ctx->buffer = NULL; // Important if avio_context_free also tries
        }
        avio_context_free(&ctx->avio_ctx); // Frees the AVIOContext struct itself
        ctx->avio_ctx = NULL;
    }
     avformat_close_input(&ctx->format_ctx); // Frees format_ctx and its streams
     ctx->format_ctx = NULL; // Already done by avformat_close_input if ptr is to actual ctx
  } else if (ctx->avio_ctx) { // If format_ctx was null but avio_ctx existed
    LOGD("demux_context_free: format_ctx was null, freeing AVIOContext separately.");
    if (ctx->avio_ctx->buffer) {
        av_free(ctx->avio_ctx->buffer);
        ctx->avio_ctx->buffer = NULL;
    }
    avio_context_free(&ctx->avio_ctx);
    ctx->avio_ctx = NULL;
  }
  // ctx->avio_internal_buffer is the same as ctx->avio_ctx->buffer and freed above.

  ctx->audio_stream_index = -1;
  ctx->audio_stream = NULL;

  LOGD("demux_context_free: Releasing Java references.");
  if (ctx->extractor_java_ref && env) {
    env->DeleteGlobalRef(ctx->extractor_java_ref);
    ctx->extractor_java_ref = NULL;
  }
  
  if (attached_here && env) {
    LOGD("demux_context_free: Detaching JNIEnv.");
    ctx->jvm->DetachCurrentThread();
  }

  LOGD("demux_context_free: Freeing FfmpegDemuxContext struct.");
  free(ctx);
  LOGD("demux_context_free: Done.");
}


// JNI Function implementations
FFMPEG_EXTRACTOR_FUNC(jlong, nativeCreateContext, jobject javaExtractorInstance) {
  LOGD("nativeCreateContext called.");
  FfmpegDemuxContext *ctx = (FfmpegDemuxContext *) calloc(1, sizeof(FfmpegDemuxContext));
  if (!ctx) {
    LOGE("Failed to allocate FfmpegDemuxContext");
    return 0L;
  }
  ctx->audio_stream_index = -1;

  // Store JVM and a global reference to the FfmpegExtractor Java instance
  if (env->GetJavaVM(&ctx->jvm) != JNI_OK) {
    LOGE("Failed to get JavaVM");
    demux_context_free(ctx);
    return 0L;
  }
  ctx->extractor_java_ref = env->NewGlobalRef(javaExtractorInstance);
  if (!ctx->extractor_java_ref) {
    LOGE("Failed to create global ref for FfmpegExtractor instance");
    demux_context_free(ctx);
    return 0L;
  }

  // Get method IDs for Java callbacks
  jclass extractor_class = env->GetObjectClass(ctx->extractor_java_ref);
  if (!extractor_class) {
    LOGE("Failed to get FfmpegExtractor class");
    demux_context_free(ctx);
    return 0L;
  }
  ctx->read_method_id = env->GetMethodID(extractor_class, "readFromExtractorInput", "(Ljava/nio/ByteBuffer;I)I");
  ctx->seek_method_id = env->GetMethodID(extractor_class, "seekInExtractorInput", "(JI)J");
  env->DeleteLocalRef(extractor_class); // Clean up local ref

  if (!ctx->read_method_id || !ctx->seek_method_id) {
    LOGE("Failed to find Java callback methods (readFromExtractorInput or seekInExtractorInput)");
    demux_context_free(ctx);
    return 0L;
  }
  LOGD("Java callback methods obtained: read_method_id=%p, seek_method_id=%p", ctx->read_method_id, ctx->seek_method_id);

  // Allocate buffer for AVIOContext
  // This buffer is managed by the AVIOContext after creation if it's not user-provided in a specific way.
  // Here, we provide it, so we must manage its lifecycle carefully with avio_context_free.
  ctx->avio_internal_buffer = (uint8_t *) av_malloc(AVIO_BUFFER_SIZE);
  if (!ctx->avio_internal_buffer) {
    LOGE("Failed to allocate AVIO buffer");
    demux_context_free(ctx);
    return 0L;
  }

  // Create AVIOContext using our callbacks
  ctx->avio_ctx = avio_alloc_context(
      ctx->avio_internal_buffer, // Buffer for FFmpeg to use
      AVIO_BUFFER_SIZE,
      0,                            // write_flag (0 for read-only)
      ctx,                          // opaque context for callbacks (our FfmpegDemuxContext*)
      avio_read_packet_callback_jni,
      NULL,                         // write_packet callback (not needed for reading)
      NULL);

  if (!ctx->avio_ctx) {
    LOGE("Failed to allocate AVIOContext");
    av_free(ctx->avio_internal_buffer); // Free the buffer if avio_alloc_context failed
    ctx->avio_internal_buffer = NULL;
    demux_context_free(ctx);
    return 0L;
  }
  // From this point, avio_internal_buffer is effectively avio_ctx->buffer.
  // It will be freed when avio_ctx is freed in demux_context_free.

  // Create AVFormatContext
  ctx->format_ctx = avformat_alloc_context();
  if (!ctx->format_ctx) {
    LOGE("Failed to allocate AVFormatContext");
    demux_context_free(ctx); // This will also free avio_ctx and its buffer
    return 0L;
  }

  ctx->format_ctx->pb = ctx->avio_ctx;
  ctx->format_ctx->flags |= AVFMT_FLAG_CUSTOM_IO; // Signal that we use custom IO
  // Optional: Increase probe size if needed for difficult streams
  // ctx->format_ctx->probesize = 2 * 1024 * 1024; // e.g., 2MB
  // ctx->format_ctx->max_analyze_duration = 5 * AV_TIME_BASE; // e.g., 5 seconds

  // Open the input stream (FFmpeg will use our callbacks)
  LOGD("Calling avformat_open_input");
  int ret = avformat_open_input(&ctx->format_ctx, NULL, NULL, NULL);
  if (ret < 0) {
    log_error_msg("avformat_open_input", ret, "Failed to open input via custom IO");
    // format_ctx->pb (avio_ctx) is not freed by avformat_open_input on failure if custom IO.
    // demux_context_free will handle freeing avio_ctx and format_ctx.
    demux_context_free(ctx);
    return 0L;
  }
  LOGD("avformat_open_input successful");

  // Retrieve stream information
  LOGD("Calling avformat_find_stream_info");
  ret = avformat_find_stream_info(ctx->format_ctx, NULL);
  if (ret < 0) {
    log_error_msg("avformat_find_stream_info", ret, "Failed to find stream info");
    demux_context_free(ctx); // This closes input and frees contexts
    return 0L;
  }
  LOGD("avformat_find_stream_info successful. Nb_streams: %u", ctx->format_ctx->nb_streams);

  // Find the first audio stream
  for (unsigned int i = 0; i < ctx->format_ctx->nb_streams; i++) {
    if (ctx->format_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
      ctx->audio_stream_index = (int) i;
      ctx->audio_stream = ctx->format_ctx->streams[i];
      LOGD("Found audio stream at index %d", ctx->audio_stream_index);
      break;
    }
  }

  if (ctx->audio_stream_index == -1) {
    LOGE("No audio stream found in the input");
    demux_context_free(ctx);
    return 0L;
  }

  LOGD("Audio stream details: CodecID=%d, SampleRate=%d, Channels=%d, BitRate=%lld, BlockAlign=%d",
       ctx->audio_stream->codecpar->codec_id,
       ctx->audio_stream->codecpar->sample_rate,
       ctx->audio_stream->codecpar->channels,
       (long long)ctx->audio_stream->codecpar->bit_rate,
       ctx->audio_stream->codecpar->block_align);

  return (jlong) ctx;
}

FFMPEG_EXTRACTOR_FUNC(void, nativeReleaseContext, jlong context) {
  LOGD("nativeReleaseContext called for context: %p", (void*)context);
  FfmpegDemuxContext *ctx = (FfmpegDemuxContext *) context;
  if (ctx) {
    demux_context_free(ctx);
  }
}

FFMPEG_EXTRACTOR_FUNC(jint, nativeGetAudioCodecId, jlong context) {
  FfmpegDemuxContext *ctx = (FfmpegDemuxContext *) context;
  if (!ctx || !ctx->audio_stream) {
    LOGE("nativeGetAudioCodecId: Invalid context or no audio stream.");
    return AV_CODEC_ID_NONE; // Return a known invalid/none ID
  }
  return (jint) ctx->audio_stream->codecpar->codec_id;
}

FFMPEG_EXTRACTOR_FUNC(jint, nativeGetSampleRate, jlong context) {
  FfmpegDemuxContext *ctx = (FfmpegDemuxContext *) context;
  if (!ctx || !ctx->audio_stream) {
     LOGE("nativeGetSampleRate: Invalid context or no audio stream.");
    return -1;
  }
  return ctx->audio_stream->codecpar->sample_rate;
}

FFMPEG_EXTRACTOR_FUNC(jint, nativeGetChannelCount, jlong context) {
  FfmpegDemuxContext *ctx = (FfmpegDemuxContext *) context;
  if (!ctx || !ctx->audio_stream) {
    LOGE("nativeGetChannelCount: Invalid context or no audio stream.");
    return -1;
  }
  // FFmpeg's channels field in AVCodecParameters is int.
  // For older FFmpeg, it might be channel_layout and then av_get_channel_layout_nb_channels.
  // But modern FFmpeg (used by ExoPlayer extension) has codecpar->channels.
  return ctx->audio_stream->codecpar->channels;
}

FFMPEG_EXTRACTOR_FUNC(jlong, nativeGetBitRate, jlong context) {
  FfmpegDemuxContext *ctx = (FfmpegDemuxContext *) context;
  if (!ctx || !ctx->audio_stream) {
    LOGE("nativeGetBitRate: Invalid context or no audio stream.");
    return -1;
  }
  return (jlong) ctx->audio_stream->codecpar->bit_rate;
}

FFMPEG_EXTRACTOR_FUNC(jint, nativeGetBlockAlign, jlong context) {
  FfmpegDemuxContext *ctx = (FfmpegDemuxContext *) context;
  if (!ctx || !ctx->audio_stream) {
    LOGE("nativeGetBlockAlign: Invalid context or no audio stream.");
    return -1;
  }
  return ctx->audio_stream->codecpar->block_align;
}

FFMPEG_EXTRACTOR_FUNC(jbyteArray, nativeGetExtraData, jlong context) {
  FfmpegDemuxContext *ctx = (FfmpegDemuxContext *) context;
  if (!ctx || !ctx->audio_stream || !ctx->audio_stream->codecpar || ctx->audio_stream->codecpar->extradata_size <= 0) {
    return NULL;
  }

  jbyteArray extraDataArray = env->NewByteArray(ctx->audio_stream->codecpar->extradata_size);
  if (!extraDataArray) {
    LOGE("Failed to allocate NewByteArray for extradata");
    return NULL;
  }
  env->SetByteArrayRegion(extraDataArray, 0, ctx->audio_stream->codecpar->extradata_size, (jbyte *) ctx->audio_stream->codecpar->extradata);
  return extraDataArray;
}

FFMPEG_EXTRACTOR_FUNC(jint, nativeReadPacket, jlong context, jbyteArray outputBuffer, jint outputBufferSize, jlongArray timestampOut) {
    FfmpegDemuxContext *ctx = (FfmpegDemuxContext *) context;
    if (!ctx || !ctx->format_ctx || !outputBuffer || outputBufferSize <= 0) {
        LOGE("nativeReadPacket: Invalid parameters. Context: %p, outputBuffer: %p, size: %d", (void*)ctx, (void*)outputBuffer, outputBufferSize);
        return AVERROR(EINVAL); // Invalid argument
    }

    AVPacket packet;
    av_init_packet(&packet); // Initialize fields to default values
    packet.data = NULL;
    packet.size = 0;

    int ret = av_read_frame(ctx->format_ctx, &packet);

    if (ret < 0) {
        if (ret != AVERROR_EOF) { // Don't log EOF as an error every time
            log_error_msg("av_read_frame", ret, "Error reading frame");
        } else {
            LOGD("av_read_frame returned AVERROR_EOF");
        }
        av_packet_unref(&packet); // Important to unref even on error if data was allocated
        return ret; // Propagate FFmpeg error (e.g., AVERROR_EOF)
    }

    // Process only packets from the selected audio stream
    if (packet.stream_index != ctx->audio_stream_index) {
        LOGD("Skipping packet from stream %d (audio stream is %d)", packet.stream_index, ctx->audio_stream_index);
        av_packet_unref(&packet);
        return 0; // Indicate no data for our stream, continue.
    }

    if (packet.size > outputBufferSize) {
        LOGE("Packet size (%d) exceeds output buffer size (%d). Truncating.", packet.size, outputBufferSize);
        // This is an error condition. The Java side should provide a large enough buffer.
        // For now, we copy what fits, but this implies data loss.
        // Ideally, return an error indicating buffer too small. AVERROR(ENOMEM) or similar.
        // However, the original code copied and FfmpegExtractor expects a fixed buffer.
        // Let's return an error to signal this problem.
        av_packet_unref(&packet);
        return AVERROR(ENOMEM); // Or a custom error
    }

    // Copy packet data to Java's buffer
    env->SetByteArrayRegion(outputBuffer, 0, packet.size, (jbyte *) packet.data);

    // Extract timestamp
    jlong ptsUs = -9223372036854775807LL; // C.TIME_UNSET
    if (packet.pts != AV_NOPTS_VALUE) {
        ptsUs = av_rescale_q(packet.pts, ctx->audio_stream->time_base, AV_TIME_BASE_Q);
    } else if (packet.dts != AV_NOPTS_VALUE) { // Fallback to DTS if PTS is not available
        ptsUs = av_rescale_q(packet.dts, ctx->audio_stream->time_base, AV_TIME_BASE_Q);
    }
    // else: timestamp remains C.TIME_UNSET

    if (timestampOut) { // Check if timestampOut array is provided
        env->SetLongArrayRegion(timestampOut, 0, 1, &ptsUs);
    }

    int read_size = packet.size;
    av_packet_unref(&packet); // Free the packet allocated by av_read_frame

    return read_size;
}


FFMPEG_EXTRACTOR_FUNC(jlong, nativeGetDuration, jlong context) {
  FfmpegDemuxContext *ctx = (FfmpegDemuxContext *) context;
  if (!ctx || !ctx->format_ctx) {
    return -1; // C.TIME_UNSET equivalent for duration if unknown/error
  }
  if (ctx->format_ctx->duration == AV_NOPTS_VALUE) {
      return -1; // C.TIME_UNSET
  }
  // FFmpeg duration is in AV_TIME_BASE units (microseconds)
  return ctx->format_ctx->duration;
}

FFMPEG_EXTRACTOR_FUNC(jboolean, nativeSeek, jlong context, jlong timeUs) {
  FfmpegDemuxContext *ctx = (FfmpegDemuxContext *) context;
  if (!ctx || !ctx->format_ctx || ctx->audio_stream_index == -1) {
    LOGE("nativeSeek: Invalid context or no audio stream for seeking.");
    return JNI_FALSE;
  }

  LOGD("nativeSeek: Attempting to seek to %lld us", (long long)timeUs);

  // Convert timeUs (microseconds) to the audio stream's time_base
  int64_t target_ts = av_rescale_q(timeUs, AV_TIME_BASE_Q, ctx->audio_stream->time_base);

  // AVSEEK_FLAG_BACKWARD ensures seeking to a keyframe at or before the target.
  // AVSEEK_FLAG_ANY allows seeking to non-keyframes if the format supports it.
  // For general robustness, AVSEEK_FLAG_BACKWARD is often preferred for frame-accurate seeking.
  int ret = av_seek_frame(ctx->format_ctx, ctx->audio_stream_index, target_ts, AVSEEK_FLAG_BACKWARD);

  if (ret < 0) {
    log_error_msg("av_seek_frame", ret, "Failed to seek in stream");
    return JNI_FALSE;
  }

  // Optional: After seeking, some decoders might need flushing.
  // avcodec_flush_buffers(decoder_context); // If we had a decoder context here.
  // For an extractor, this is usually enough. The decoder will be reset/flushed separately.

  LOGD("nativeSeek: av_seek_frame successful to target_ts %lld (stream time_base)", (long long)target_ts);
  return JNI_TRUE;
}


// --- AVIO Callbacks ---
static int avio_read_packet_callback_jni(void *opaque, uint8_t *buf, int buf_size) {
    FfmpegDemuxContext *ctx = (FfmpegDemuxContext *) opaque;
    JNIEnv *env;
    bool must_detach = false;

    int get_env_stat = ctx->jvm->GetEnv((void **)&env, JNI_VERSION_1_6);
    if (get_env_stat == JNI_EDETACHED) {
        if (ctx->jvm->AttachCurrentThread(&env, NULL) != 0) {
            LOGE("Read callback: Failed to attach current thread");
            return AVERROR(EIO); // Return an FFmpeg error code
        }
        must_detach = true;
    } else if (get_env_stat != JNI_OK) {
        LOGE("Read callback: GetEnv failed with %d", get_env_stat);
        return AVERROR(EIO);
    }

    // Create a direct ByteBuffer wrapping the native buffer 'buf'
    // This allows Java to write directly into FFmpeg's provided buffer.
    jobject direct_buffer = env->NewDirectByteBuffer(buf, buf_size);
    if (direct_buffer == NULL) {
        LOGE("Read callback: Failed to create direct ByteBuffer");
        if (must_detach) ctx->jvm->DetachCurrentThread();
        return AVERROR(ENOMEM); // Out of memory
    }

    // Call the Java method: readFromExtractorInput(ByteBuffer targetDirectBuffer, int readLength)
    jint bytes_read = env->CallIntMethod(ctx->extractor_java_ref, ctx->read_method_id, direct_buffer, buf_size);
    
    env->DeleteLocalRef(direct_buffer); // Clean up local ref to the ByteBuffer

    if (env->ExceptionCheck()) {
        LOGE("Read callback: Exception occurred in Java's readFromExtractorInput method.");
        env->ExceptionDescribe(); // Log the Java exception
        env->ExceptionClear();    // Clear the exception
        if (must_detach) ctx->jvm->DetachCurrentThread();
        return AVERROR(EIO); // Return generic I/O error
    }

    if (must_detach) {
        ctx->jvm->DetachCurrentThread();
    }
    
    // bytes_read can be:
    // - Positive: number of bytes read
    // - AVERROR_EOF_JNI (-541478725): if Java side determined EOF
    // - AVERROR_EIO_JNI (-5): if Java side had an IOException
    // FFmpeg expects positive for bytes read, or AVERROR_EOF, or other negative error.
    // Our Java method is designed to return these directly.
    if (bytes_read > buf_size) { // Should not happen
        LOGE("Read callback: Java returned more bytes (%d) than buffer size (%d)", bytes_read, buf_size);
        return AVERROR(EIO);
    }
    // LOGD("Read callback: Java read %d bytes into buffer %p (size %d)", bytes_read, buf, buf_size);
    return (int)bytes_read;
}

static int64_t avio_seek_callback_jni(void *opaque, int64_t offset, int whence) {
    FfmpegDemuxContext *ctx = (FfmpegDemuxContext *) opaque;
    JNIEnv *env;
    bool must_detach = false;

    // LOGD("Seek callback: offset=%lld, whence=%d", (long long)offset, whence);

    int get_env_stat = ctx->jvm->GetEnv((void **)&env, JNI_VERSION_1_6);
    if (get_env_stat == JNI_EDETACHED) {
        if (ctx->jvm->AttachCurrentThread(&env, NULL) != 0) {
            LOGE("Seek callback: Failed to attach current thread");
            return -1; // FFmpeg generic error for seek failure
        }
        must_detach = true;
    } else if (get_env_stat != JNI_OK) {
        LOGE("Seek callback: GetEnv failed with %d", get_env_stat);
        return -1;
    }

    // Call the Java method: seekInExtractorInput(long offset, int whence)
    jlong new_position = env->CallLongMethod(ctx->extractor_java_ref, ctx->seek_method_id, offset, whence);

    if (env->ExceptionCheck()) {
        LOGE("Seek callback: Exception occurred in Java's seekInExtractorInput method.");
        env->ExceptionDescribe();
        env->ExceptionClear();
        if (must_detach) ctx->jvm->DetachCurrentThread();
        return -1; // Return generic error
    }

    if (must_detach) {
        ctx->jvm->DetachCurrentThread();
    }

    // new_position can be:
    // - Positive or 0: new stream position
    // - C.LENGTH_UNSET (-1 for jlong): if AVSEEK_SIZE and length unknown
    // - AVERROR_EIO_JNI (-5): if Java side had an IOException or invalid arg
    // - AVERROR_ENOSYS_JNI (-38): if seek operation not supported (e.g., backward seek)
    // FFmpeg expects the new position, or a negative error code.
    // LOGD("Seek callback: Java returned new_position=%lld", (long long)new_position);
    return (int64_t)new_position;
}
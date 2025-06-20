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
package com.google.android.exoplayer2.ext.ffmpeg;

import androidx.annotation.Nullable;
import com.google.android.exoplayer2.C;
import com.google.android.exoplayer2.ExoPlayerLibraryInfo;
import com.google.android.exoplayer2.util.LibraryLoader;
import com.google.android.exoplayer2.util.Log;
import com.google.android.exoplayer2.util.MimeTypes;
import org.checkerframework.checker.nullness.qual.MonotonicNonNull;

/**
 * Configures and queries the underlying native library.
 *
 * @deprecated com.google.android.exoplayer2 is deprecated. Please migrate to androidx.media3 (which
 *     contains the same ExoPlayer code). See <a
 *     href="https://developer.android.com/guide/topics/media/media3/getting-started/migration-guide">the
 *     migration guide</a> for more details, including a script to help with the migration.
 */
@Deprecated
public final class FfmpegLibrary {

  static {
    ExoPlayerLibraryInfo.registerModule("goog.exo.ffmpeg");
  }

  private static final String TAG = "FfmpegLibrary";

  private static final LibraryLoader LOADER =
      new LibraryLoader("ffmpegJNI") {
        @Override
        protected void loadLibrary(String name) {
          System.loadLibrary(name);
        }
      };

  private static @MonotonicNonNull String version;
  private static int inputBufferPaddingSize = C.LENGTH_UNSET;

  private FfmpegLibrary() {}

  /**
   * Override the names of the FFmpeg native libraries. If an application wishes to call this
   * method, it must do so before calling any other method defined by this class, and before
   * instantiating a {@link FfmpegAudioRenderer} instance.
   *
   * @param libraries The names of the FFmpeg native libraries.
   */
  public static void setLibraries(String... libraries) {
    LOADER.setLibraries(libraries);
  }

  /** Returns whether the underlying library is available, loading it if necessary. */
  public static boolean isAvailable() {
    return LOADER.isAvailable();
  }

  /** Returns the version of the underlying library if available, or null otherwise. */
  @Nullable
  public static String getVersion() {
    if (!isAvailable()) {
      return null;
    }
    if (version == null) {
      version = ffmpegGetVersion();
    }
    return version;
  }

  /**
   * Returns the required amount of padding for input buffers in bytes, or {@link C#LENGTH_UNSET} if
   * the underlying library is not available.
   */
  public static int getInputBufferPaddingSize() {
    if (!isAvailable()) {
      return C.LENGTH_UNSET;
    }
    if (inputBufferPaddingSize == C.LENGTH_UNSET) {
      inputBufferPaddingSize = ffmpegGetInputBufferPaddingSize();
    }
    return inputBufferPaddingSize;
  }

  /**
   * Returns whether the underlying library supports the specified MIME type.
   *
   * @param mimeType The MIME type to check.
   */
  public static boolean supportsFormat(String mimeType) {
    if (!isAvailable()) {
      return false;
    }
    @Nullable String codecName = getCodecName(mimeType);
    if (codecName == null) {
      return false;
    }
    if (!ffmpegHasDecoder(codecName)) {
      Log.w(TAG, "No " + codecName + " decoder available. Check the FFmpeg build configuration.");
      return false;
    }
    return true;
  }

  /**
   * Returns the name of the FFmpeg decoder that could be used to decode the format, or {@code null}
   * if it's unsupported.
   */
  @Nullable
  /* package */ static String getCodecName(String mimeType) {
    switch (mimeType) {
      case MimeTypes.AUDIO_AAC:
        return "aac";
      case MimeTypes.AUDIO_MPEG:
      case MimeTypes.AUDIO_MPEG_L1:
      case MimeTypes.AUDIO_MPEG_L2:
        return "mp3";
      case MimeTypes.AUDIO_AC3:
        return "ac3";
      case MimeTypes.AUDIO_E_AC3:
      case MimeTypes.AUDIO_E_AC3_JOC:
        return "eac3";
      case MimeTypes.AUDIO_TRUEHD:
        return "truehd";
      case MimeTypes.AUDIO_DTS:
      case MimeTypes.AUDIO_DTS_HD:
        return "dca"; // FFmpeg uses "dca" for DTS Coherent Acoustics
      case MimeTypes.AUDIO_VORBIS:
        return "vorbis";
      case MimeTypes.AUDIO_OPUS:
        return "opus";
      case MimeTypes.AUDIO_AMR_NB:
        return "amrnb";
      case MimeTypes.AUDIO_AMR_WB:
        return "amrwb";
      case MimeTypes.AUDIO_FLAC:
        return "flac";
      case MimeTypes.AUDIO_ALAC:
        return "alac";
      case MimeTypes.AUDIO_MLAW:
        return "pcm_mulaw";
      case MimeTypes.AUDIO_ALAW:
        return "pcm_alaw";
      case MimeTypes.AUDIO_WMA:
        return "wmav2"; // Default to WMAv2, could be wmav1, wmapro etc.
                        // FfmpegAudioDecoder constructor passes specific WMA params.
      case MimeTypes.AUDIO_DSD: // Generic DSD, default to msbf as before
        return "dsd_msbf";
      case MimeTypes.AUDIO_DSD_MSBF:
        return "dsd_msbf";
      case MimeTypes.AUDIO_DSD_LSBF:
        return "dsd_lsbf";
      case MimeTypes.AUDIO_DSD_MSBF_PLANAR:
        return "dsd_msbf_planar";
      case MimeTypes.AUDIO_DSD_LSBF_PLANAR:
        return "dsd_lsbf_planar";
      default:
        return null;
    }
  }

   /**
   * Returns the FFmpeg codec name for a given FFmpeg codec ID.
   * This is used by FfmpegExtractor as a fallback for unknown DSD variants.
   */
  @Nullable
  /* package */ static String getCodecNameForFFmpegId(int ffmpegCodecId) {
    // This is a simplified mapping. A more complete one would query FFmpeg.
    // Based on AV_CODEC_ID_* constants used in FfmpegExtractor
    switch (ffmpegCodecId) {
        // From FfmpegExtractor
        case 0x15000 + 7: /* AV_CODEC_ID_WMAV1 */ return "wmav1";
        case 0x15000 + 8: /* AV_CODEC_ID_WMAV2 */ return "wmav2";
        case 88069: /* AV_CODEC_ID_DSD_LSBF */ return "dsd_lsbf";
        case 88070: /* AV_CODEC_ID_DSD_MSBF */ return "dsd_msbf";
        case 88071: /* AV_CODEC_ID_DSD_LSBF_PLANAR */ return "dsd_lsbf_planar";
        case 88072: /* AV_CODEC_ID_DSD_MSBF_PLANAR */ return "dsd_msbf_planar";
        default:
            // This would ideally call a native method like avcodec_get_name(ffmpegCodecId)
            // For now, return null if not explicitly mapped.
            return null;
    }
  }


  private static native String ffmpegGetVersion();

  private static native int ffmpegGetInputBufferPaddingSize();

  private static native boolean ffmpegHasDecoder(String codecName);
}
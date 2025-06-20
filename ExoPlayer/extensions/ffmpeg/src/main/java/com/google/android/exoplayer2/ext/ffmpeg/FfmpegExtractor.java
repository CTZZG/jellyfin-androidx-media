package com.google.android.exoplayer2.ext.ffmpeg;

import androidx.annotation.Nullable;
import com.google.android.exoplayer2.C;
import com.google.android.exoplayer2.Format;
import com.google.android.exoplayer2.ParserException;
import com.google.android.exoplayer2.extractor.Extractor;
import com.google.android.exoplayer2.extractor.ExtractorInput;
import com.google.android.exoplayer2.extractor.ExtractorOutput;
import com.google.android.exoplayer2.extractor.PositionHolder;
import com.google.android.exoplayer2.extractor.TrackOutput;
import com.google.android.exoplayer2.util.Log;
import com.google.android.exoplayer2.util.MimeTypes;
import com.google.android.exoplayer2.util.ParsableByteArray;
import java.io.EOFException;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.util.ArrayList;
import java.util.Arrays;

/**
 * Extracts data from files using FFmpeg demuxing.
 */
public final class FfmpegExtractor implements Extractor {

  private static final String TAG = "FfmpegExtractor";
  private static final int SNIFF_BUFFER_SIZE = 16;
  private static final int PACKET_BUFFER_SIZE = 32768; // Max packet size to read from JNI
  // private static final int MAX_INPUT_LENGTH = 1500 * 1024 * 1024; // No longer strictly needed for full load

  private static byte[] ASF_SIGNATURE = new byte[]{
      (byte) 0x30, (byte) 0x26, (byte) 0xB2, (byte) 0x75,
      (byte) 0x8E, (byte) 0x66, (byte) 0xCF, (byte) 0x11,
      (byte) 0xA6, (byte) 0xD9, (byte) 0x00, (byte) 0xAA,
      (byte) 0x00, (byte) 0x62, (byte) 0xCE, (byte) 0x6C
  };
  private static final byte[] DSF_SIGNATURE = new byte[]{(byte) 'D', (byte) 'S', (byte) 'D', (byte) ' '};


  private static final int AV_CODEC_ID_WMAV1 = 0x15000 + 7; // 86023
  private static final int AV_CODEC_ID_WMAV2 = 0x15000 + 8; // 86024

  private static final int AV_CODEC_ID_DSD_LSBF = 88069;
  private static final int AV_CODEC_ID_DSD_MSBF = 88070;
  private static final int AV_CODEC_ID_DSD_LSBF_PLANAR = 88071;
  private static final int AV_CODEC_ID_DSD_MSBF_PLANAR = 88072;

  // Constants for JNI seek whence, must match native definitions if used directly
  private static final int AVSEEK_SIZE_FLAG_JNI = 0x10000;
  private static final int SEEK_SET_JNI = 0;
  private static final int SEEK_CUR_JNI = 1;
  private static final int SEEK_END_JNI = 2;

  // FFmpeg error codes (subset, negative values)
  // These are standard FFmpeg AVERROR codes.
  // Example: AVERROR_EOF defined as (-(('E')|('O'<<8)|('F'<<16)|(' '<24))) which is -541478725
  private static final int AVERROR_EOF_JNI = -541478725;
  private static final int AVERROR_EIO_JNI = -5; // Standard EIO, FFmpeg uses AVERROR(EIO)
  private static final int AVERROR_ENOSYS_JNI = -38; // Standard ENOSYS for "Function not implemented"


  private long nativeContext;
  private ExtractorInput input; // Store for JNI callbacks
  private ExtractorOutput extractorOutput;
  private TrackOutput trackOutput;
  private boolean tracksInitialized;
  private final byte[] packetBuffer;
  private final long[] timestampBuffer;
  private boolean endOfInput;
  private boolean released;

  public FfmpegExtractor() {
    packetBuffer = new byte[PACKET_BUFFER_SIZE];
    timestampBuffer = new long[1];
    tracksInitialized = false;
    endOfInput = false;
    nativeContext = 0;
    released = false;
  }

  @Override
  public boolean sniff(ExtractorInput input) throws IOException {
    if (released) {
      return false;
    }

    byte[] sniffHeader = new byte[SNIFF_BUFFER_SIZE];
    try {
      input.peekFully(sniffHeader, 0, ASF_SIGNATURE.length);
      boolean isAsf = true;
      for (int i = 0; i < ASF_SIGNATURE.length; i++) {
        if (sniffHeader[i] != ASF_SIGNATURE[i]) {
          isAsf = false;
          break;
        }
      }
      if (isAsf) {
        return true;
      }
    } catch (EOFException e) {
      // Not enough data for ASF signature
    }

    input.resetPeekPosition();
    try {
      input.peekFully(sniffHeader, 0, DSF_SIGNATURE.length);
      boolean isDsf = true;
      for (int i = 0; i < DSF_SIGNATURE.length; i++) {
        if (sniffHeader[i] != DSF_SIGNATURE[i]) {
          isDsf = false;
          break;
        }
      }
      if (isDsf) {
        return true;
      }
    } catch (EOFException e) {
      // Not enough data for DSF signature
    }
    
    return false;
  }

  @Override
  public void init(ExtractorOutput output) {
    this.extractorOutput = output;
  }

  @Override
  public @ReadResult int read(ExtractorInput input, PositionHolder seekPosition)
      throws IOException {
    if (released) {
      return RESULT_END_OF_INPUT;
    }
    if (endOfInput) {
      return RESULT_END_OF_INPUT;
    }
    this.input = input; // Store for JNI callbacks

    try {
      if (nativeContext == 0) {
        if (!FfmpegLibrary.isAvailable()) {
          throw ParserException.createForMalformedContainer(
              "Failed to load FFmpeg native libraries.", null);
        }
        // No longer reading full input here. JNI will use callbacks.
        // MAX_INPUT_LENGTH checks are removed as we are streaming.
        // The input.getLength() check is also removed to support streams of unknown length.
        // FFmpeg's AVSEEK_SIZE will get C.LENGTH_UNSET if input.getLength() is so.

        nativeContext = nativeCreateContext(this); // Pass this FfmpegExtractor instance for callbacks
        if (nativeContext == 0) {
          Log.e(TAG, "Failed to create native context");
          throw ParserException.createForMalformedContainer("FFmpeg failed to open input",
              null);
        }
      }

      if (!tracksInitialized) {
        initializeTracks(); // This will use nativeGet... methods which trigger AVIO reads
        tracksInitialized = true;
        extractorOutput.endTracks();
      }

      return readSample();
    } catch (UnsatisfiedLinkError e) {
      Log.e(TAG, "Native method not available", e);
      release(); // Ensure cleanup on ULE
      return RESULT_END_OF_INPUT;
    } catch (IOException e) {
        Log.e(TAG, "IOException during read", e);
        release(); // Ensure cleanup on IOE
        throw e;
    }
  }

  private void initializeTracks() throws ParserException {
    int codecId = nativeGetAudioCodecId(nativeContext);
    int sampleRate = nativeGetSampleRate(nativeContext);
    int channelCount = nativeGetChannelCount(nativeContext);
    long bitRate = nativeGetBitRate(nativeContext);
    int blockAlign = nativeGetBlockAlign(nativeContext);
    @Nullable byte[] extraData = nativeGetExtraData(nativeContext);

    String mimeType = getMimeTypeFromCodecId(codecId);
    if (mimeType == null) {
      Log.e(TAG, "Unsupported codec ID from FFmpeg: " + codecId);
      throw ParserException.createForMalformedContainer("Unsupported codec ID: " + codecId, null);
    }

    if (trackOutput == null) {
        trackOutput = extractorOutput.track(0, C.TRACK_TYPE_AUDIO);
    }

    Format.Builder formatBuilder = new Format.Builder()
        .setSampleMimeType(mimeType)
        .setChannelCount(channelCount)
        .setSampleRate(sampleRate);

    if (bitRate > 0) {
      formatBuilder.setAverageBitrate((int) bitRate);
    }
    
    ArrayList<byte[]> initializationData = new ArrayList<>();
    if (extraData != null && extraData.length > 0) {
      initializationData.add(extraData);
    }

    // For WMA, blockAlign is passed as the second item in initializationData
    // FfmpegAudioDecoder expects this.
    if (MimeTypes.AUDIO_WMA.equals(mimeType) && blockAlign > 0) {
        byte[] blockAlignBytes = new byte[2];
        blockAlignBytes[0] = (byte) (blockAlign & 0xFF);
        blockAlignBytes[1] = (byte) ((blockAlign >> 8) & 0xFF);
        initializationData.add(blockAlignBytes);
    }
    if (!initializationData.isEmpty()) {
        formatBuilder.setInitializationData(initializationData);
    }

    Format format = formatBuilder.build();
    trackOutput.format(format);

    long durationUs = nativeGetDuration(nativeContext);
    if (durationUs == C.TIME_UNSET && input.getLength() != C.LENGTH_UNSET) {
        // Try to estimate duration if FFmpeg doesn't provide it but we know the length and bitrate
        if (bitRate > 0) {
            durationUs = (input.getLength() * C.BITS_PER_BYTE * C.MICROS_PER_SECOND) / bitRate;
        }
    }
    if (durationUs <= 0) { // Still 0 or negative
      durationUs = C.TIME_UNSET;
    }
    
    extractorOutput.seekMap(new FfmpegSeekMap(durationUs));

    Log.d(TAG, String.format("SeekMap initialized - Duration: %d us", durationUs));
    Log.d(TAG,
        String.format("Track initialized - Codec ID: %d, MimeType: %s, Sample Rate: %d, Channels: %d, Bitrate: %d, BlockAlign: %d",
            codecId, mimeType, sampleRate, channelCount, bitRate, blockAlign));
  }

  @Nullable
  private String getMimeTypeFromCodecId(int codecId) {
    switch (codecId) {
      case AV_CODEC_ID_WMAV1:
      case AV_CODEC_ID_WMAV2:
        return MimeTypes.AUDIO_WMA;
      case AV_CODEC_ID_DSD_LSBF:
        return MimeTypes.AUDIO_DSD_LSBF;
      case AV_CODEC_ID_DSD_MSBF:
        return MimeTypes.AUDIO_DSD_MSBF;
      case AV_CODEC_ID_DSD_LSBF_PLANAR:
        return MimeTypes.AUDIO_DSD_LSBF_PLANAR;
      case AV_CODEC_ID_DSD_MSBF_PLANAR:
        return MimeTypes.AUDIO_DSD_MSBF_PLANAR;
      default:
        // Try to get a generic DSD mime type if it's some other DSD variant
        // This relies on FfmpegLibrary.getCodecName to map a generic DSD to a default ffmpeg codec
        String codecName = FfmpegLibrary.getCodecNameForFFmpegId(codecId);
        if (codecName != null && (codecName.startsWith("dsd_"))) {
            Log.w(TAG, "Using generic DSD MIME type for FFmpeg codec ID: " + codecId + " (" + codecName + ")");
            return MimeTypes.AUDIO_DSD; // Fallback to generic DSD
        }
        Log.w(TAG, "Unknown codec ID from FFmpeg: " + codecId);
        return null;
    }
  }

  private int readSample() {
    timestampBuffer[0] = C.TIME_UNSET; // Reset timestamp
    int packetSize = nativeReadPacket(nativeContext, packetBuffer, PACKET_BUFFER_SIZE, timestampBuffer);

    if (packetSize < 0) {
      if (packetSize == AVERROR_EOF_JNI) { // AVERROR_EOF from FFmpeg
        Log.d(TAG, "End of input from nativeReadPacket (AVERROR_EOF)");
        endOfInput = true;
        return RESULT_END_OF_INPUT;
      }
      Log.w(TAG, "Error reading packet from native: " + packetSize);
      endOfInput = true; 
      return RESULT_END_OF_INPUT; // Treat other errors as EOF for now
    }

    if (packetSize == 0) { // No packet ready, but not EOF
      return RESULT_CONTINUE;
    }

    // Valid packet received
    ParsableByteArray packetData = new ParsableByteArray(packetBuffer, packetSize);
    trackOutput.sampleData(packetData, packetSize);

    long sampleTimeUs = timestampBuffer[0];
    if (sampleTimeUs == C.TIME_UNSET) {
        // If FFmpeg doesn't provide PTS, we might need a strategy (e.g., generate from sample count if PCM)
        // For compressed audio, PTS is crucial.
        Log.w(TAG, "Sample time is UNSET from FFmpeg. This might cause playback issues.");
        // Consider setting endOfInput if critical info is missing, or let it play if possible
    }
    // Assume all packets from FFmpeg demuxer are keyframes for simplicity unless specified otherwise
    trackOutput.sampleMetadata(sampleTimeUs, C.BUFFER_FLAG_KEY_FRAME, packetSize, 0, null);

    return RESULT_CONTINUE;
  }

  @Override
  public void seek(long position, long timeUs) {
    if (released || nativeContext == 0) {
      return;
    }
    Log.d(TAG, "seek: position: " + position + ", timeUs: " + timeUs);
    nativeSeek(nativeContext, timeUs);
    endOfInput = false; // Reset EOF flag after seek
  }

  @Override
  public void release() {
    Log.d(TAG, "release() called");
    if (nativeContext != 0 && !released) {
      try {
        nativeReleaseContext(nativeContext);
      } catch (UnsatisfiedLinkError e) {
        Log.e(TAG, "Native releaseContext method not available", e);
      }
      nativeContext = 0;
    }
    // Release other resources
    this.input = null;
    tracksInitialized = false;
    endOfInput = false;
    released = true;
  }

  // --- JNI Callbacks ---
  // These methods are called from the JNI layer (ffmpeg_extractor_jni.cc)

  @SuppressWarnings("unused") // Called from JNI
  private int readFromExtractorInput(ByteBuffer targetDirectBuffer, int readLength) {
    if (input == null || released) {
      return AVERROR_EOF_JNI; // Or another error indicating closed state
    }
    try {
      // targetDirectBuffer is a direct ByteBuffer wrapping native memory.
      // We need to set its position and limit correctly before reading.
      targetDirectBuffer.clear(); // Reset position to 0, limit to capacity
      if (readLength > targetDirectBuffer.capacity()) {
        readLength = targetDirectBuffer.capacity(); // Should not happen if JNI is correct
      }
      targetDirectBuffer.limit(readLength);

      if (readLength == 0) {
        return 0;
      }

      int totalBytesReadToTarget = 0;

      if (targetDirectBuffer.hasArray()) {
        byte[] backingArray = targetDirectBuffer.array();
        int offsetInBackingArray = targetDirectBuffer.arrayOffset() + targetDirectBuffer.position();

        while (totalBytesReadToTarget < readLength) {
          int bytesReadThisIteration = input.read(
            backingArray,
            offsetInBackingArray + totalBytesReadToTarget,
            readLength - totalBytesReadToTarget);

          if (bytesReadThisIteration == C.RESULT_END_OF_INPUT) {
            if (totalBytesReadToTarget == 0) {
              return AVERROR_EOF_JNI;
            }
            break;
          }
          totalBytesReadToTarget += bytesReadThisIteration;
        }

        if (totalBytesReadToTarget > 0) {
          targetDirectBuffer.position(totalBytesReadToTarget);
        }
      } else {
        byte[] tempBuffer = new byte[Math.min(readLength, 8192)];

        while (targetDirectBuffer.hasRemaining()) {
          int bytesToReadInChunk = Math.min(tempBuffer.length, targetDirectBuffer.remaining());
          int bytesReadFromInput = input.read(tempBuffer, 0, bytesToReadInChunk);

          if (bytesReadFromInput == C.RESULT_END_OF_INPUT) {
            if (targetDirectBuffer.position() == 0) {
              return AVERROR_EOF_JNI;
            }
            break;
          }
          targetDirectBuffer.put(tempBuffer, 0, bytesReadFromInput);
        }
      }

      return targetDirectBuffer.position();
      
    } catch (IOException e) {
      Log.e(TAG, "IOException in readFromExtractorInput", e);
      return AVERROR_EIO_JNI; // Generic I/O error for FFmpeg
    } catch (Exception e) { // Catch any other unexpected runtime exceptions
      Log.e(TAG, "Unexpected exception in readFromExtractorInput", e);
      return AVERROR_EIO_JNI;
    }
  }

  @SuppressWarnings("unused") // Called from JNI
  private long seekInExtractorInput(long offset, int whence) {
    if (input == null || released) {
      return AVERROR_EIO_JNI;
    }
    try {
      long currentPosition = input.getPosition();
      long inputLength = input.getLength();

      if ((whence & AVSEEK_SIZE_FLAG_JNI) != 0) {
        return inputLength; // C.LENGTH_UNSET is a valid return if unknown
      }

      long targetPosition;
      switch (whence & ~AVSEEK_SIZE_FLAG_JNI) { // Mask out AVSEEK_SIZE for switch
        case SEEK_SET_JNI:
          targetPosition = offset;
          break;
        case SEEK_CUR_JNI:
          targetPosition = currentPosition + offset;
          break;
        case SEEK_END_JNI:
          if (inputLength == C.LENGTH_UNSET) {
            Log.w(TAG, "Seek from end failed: length unknown");
            return AVERROR_EIO_JNI; // Cannot seek from end if length unknown
          }
          targetPosition = inputLength + offset;
          break;
        default:
          Log.w(TAG, "Invalid whence in seekInExtractorInput: " + whence);
          return AVERROR_EIO_JNI; // Invalid argument or I/O error
      }

      if (targetPosition < 0) {
        Log.w(TAG, "Seek target position is negative: " + targetPosition);
        return AVERROR_EIO_JNI; // Invalid argument
      }
      
      if (targetPosition == currentPosition) {
        return currentPosition;
      } else if (targetPosition > currentPosition) {
        long toSkip = targetPosition - currentPosition;
        // input.skipFully can throw EOFException, which is fine.
        // It takes int, so we must be careful with large skips.
        // FFmpeg might request large skips if underlying format needs it (e.g. skipping a large chunk of data).
        // If toSkip is > Integer.MAX_VALUE, we need to loop.
        while(toSkip > 0) {
            int skipAmount = (int) Math.min(toSkip, Integer.MAX_VALUE);
            input.skipFully(skipAmount);
            toSkip -= skipAmount;
        }
        return input.getPosition();
      } else { // targetPosition < currentPosition (backward seek)
        // This is problematic with the standard ExtractorInput interface.
        // FFmpeg's av_seek_frame typically handles seeking to keyframes, which might involve
        // the demuxer asking AVIO to re-read an index or header from an earlier part of the file.
        // If the underlying DataSource is not truly seekable or ExtractorInput cannot be reset,
        // this will fail.
        Log.w(TAG, "Backward seek requested via AVIO (" + currentPosition + " -> " + targetPosition + "). "
                + "This is not fully supported by the current ExtractorInput streaming model.");
        // Returning an error will likely cause FFmpeg's operation (e.g., av_seek_frame) to fail.
        return AVERROR_ENOSYS_JNI; // Operation not supported
      }
    } catch (EOFException e) {
      Log.w(TAG, "EOFException during AVIO seek operation for offset " + offset + ", whence " + whence, e);
      // FFmpeg expects the new position or an error. If we hit EOF, the new position is current position.
      return input.getPosition();
    } catch (IOException e) {
      Log.e(TAG, "IOException during AVIO seek operation for offset " + offset + ", whence " + whence, e);
      return AVERROR_EIO_JNI; // Generic I/O error
    } catch (Exception e) { // Catch any other unexpected runtime exceptions
      Log.e(TAG, "Unexpected exception in seekInExtractorInput", e);
      return AVERROR_EIO_JNI;
    }
  }

  // --- Native Methods ---
  // Pass 'this' (FfmpegExtractor instance) to native context for callbacks
  private native long nativeCreateContext(FfmpegExtractor callingInstance);

  private native int nativeGetAudioCodecId(long context);

  private native int nativeGetSampleRate(long context);

  private native int nativeGetChannelCount(long context);

  private native long nativeGetBitRate(long context);

  private native int nativeGetBlockAlign(long context);

  private native byte[] nativeGetExtraData(long context);

  private native int nativeReadPacket(long context, byte[] outputBuffer, int outputBufferSize,
      long[] timestampOut);

  private native long nativeGetDuration(long context);

  private native boolean nativeSeek(long context, long timeUs);

  private native void nativeReleaseContext(long context);
}
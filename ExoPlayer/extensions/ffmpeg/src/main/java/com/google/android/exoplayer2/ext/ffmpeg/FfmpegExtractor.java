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
import java.util.ArrayList;
import java.util.Arrays;

/**
 * Extracts data from files using FFmpeg demuxing.
 */
public final class FfmpegExtractor implements Extractor {

  private static final String TAG = "FfmpegExtractor";
  private static final int SNIFF_BUFFER_SIZE = 16;
  private static final int PACKET_BUFFER_SIZE = 32768;
  private static final int MAX_INPUT_LENGTH = 1500 * 1024 * 1024;
  private static byte[] ASF_SIGNATURE = new byte[]{
      (byte) 0x30, (byte) 0x26, (byte) 0xB2, (byte) 0x75,
      (byte) 0x8E, (byte) 0x66, (byte) 0xCF, (byte) 0x11,
      (byte) 0xA6, (byte) 0xD9, (byte) 0x00, (byte) 0xAA,
      (byte) 0x00, (byte) 0x62, (byte) 0xCE, (byte) 0x6C
  };
  private static final byte[] DSF_SIGNATURE = new byte[]{(byte) 'D', (byte) 'S', (byte) 'D', (byte) ' '};


  private static final int AV_CODEC_ID_WMAV1 = 0x15000 + 7;
  private static final int AV_CODEC_ID_WMAV2 = 0x15000 + 8;

  private static final int AV_CODEC_ID_DSD_LSBF = 86061;
  private static final int AV_CODEC_ID_DSD_MSBF = 86062;
  private static final int AV_CODEC_ID_DSD_LSBF_PLANAR = 86063;
  private static final int AV_CODEC_ID_DSD_MSBF_PLANAR = 86064;


  private long nativeContext;
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

    try {
      if (nativeContext == 0) {
        if (!FfmpegLibrary.isAvailable()) {
          throw ParserException.createForMalformedContainer(
              "Failed to load FFmpeg native libraries.", null);
        }

        long inputLengthLong = input.getLength();
        if (inputLengthLong == C.LENGTH_UNSET) {
            Log.e(TAG, "Input length is unknown. FfmpegExtractor requires a known length.");
            throw new IOException("Input length unknown, FfmpegExtractor cannot proceed.");
        }
        if (inputLengthLong > Integer.MAX_VALUE) {
            Log.e(TAG, "Input length exceeds Integer.MAX_VALUE, cannot read into byte array.");
            throw new IOException("Input file too large for current FfmpegExtractor JNI design.");
        }
        int inputLength = (int) inputLengthLong;

        if (inputLength <= 0) {
          throw new IOException("Input length must be greater than zero.");
        }
        if (inputLength > MAX_INPUT_LENGTH) {
          Log.w(TAG, "Input length " + inputLength + " exceeds configured MAX_INPUT_LENGTH " + MAX_INPUT_LENGTH + ". Attempting to proceed but may fail.");
          // Or throw: throw new IOException("Input length exceeds maximum allowed size: " + MAX_INPUT_LENGTH);
        }


        byte[] inputData = new byte[inputLength];
        input.readFully(inputData, 0, inputLength);
        Log.d(TAG, "Read " + inputLength + " bytes from input.");

        nativeContext = nativeCreateContext(inputData, inputLength);
        if (nativeContext == 0) {
          Log.e(TAG, "Failed to create native context");
          throw ParserException.createForMalformedContainer("FFmpeg failed to open input",
              null);
        }
      }

      if (!tracksInitialized) {
        initializeTracks();
        tracksInitialized = true;
        extractorOutput.endTracks();
      }

      return readSample();
    } catch (UnsatisfiedLinkError e) {
      Log.e(TAG, "Native method not available", e);
      return RESULT_END_OF_INPUT;
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

    if (MimeTypes.AUDIO_WMA.equals(mimeType)) {
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

    if (durationUs <= 0) {
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
      case AV_CODEC_ID_DSD_MSBF:
      case AV_CODEC_ID_DSD_LSBF_PLANAR:
      case AV_CODEC_ID_DSD_MSBF_PLANAR:
        return MimeTypes.AUDIO_DSD;
      default:
        Log.w(TAG, "Unknown codec ID from FFmpeg: " + codecId);
        return null;
    }
  }

  private int readSample() {
    timestampBuffer[0] = C.TIME_UNSET;
    int packetSize = nativeReadPacket(nativeContext, packetBuffer, PACKET_BUFFER_SIZE,
        timestampBuffer);

    if (packetSize < 0) {
      if (packetSize == -541478725) {
        Log.d(TAG, "End of input from nativeReadPacket (AVERROR_EOF)");
        endOfInput = true;
        return RESULT_END_OF_INPUT;
      }
      Log.w(TAG, "Error reading packet from native: " + packetSize);
      endOfInput = true; 
      return RESULT_END_OF_INPUT;
    }

    if (packetSize == 0) {
      return RESULT_CONTINUE;
    }

    ParsableByteArray packetData = new ParsableByteArray(packetBuffer, packetSize);
    trackOutput.sampleData(packetData, packetSize);

    long sampleTimeUs = timestampBuffer[0];
    if (sampleTimeUs == C.TIME_UNSET) {
        Log.w(TAG, "Sample time is UNSET from FFmpeg.");
    }
    trackOutput.sampleMetadata(sampleTimeUs, C.BUFFER_FLAG_KEY_FRAME, packetSize, 0, null);

    return RESULT_CONTINUE;
  }
  @Override
  public void seek(long position, long timeUs) {
    if (released) {
      return;
    }

    Log.d(TAG, "seek: position: " + position + ", timeUs: " + timeUs);
    if (nativeContext != 0) {
      nativeSeek(nativeContext, timeUs);
      endOfInput = false;
    }
  }

  @Override
  public void release() {
    Log.d(TAG, "release() called");
    if (nativeContext != 0) {
      try {
        nativeReleaseContext(nativeContext);
      } catch (UnsatisfiedLinkError e) {
        Log.e(TAG, "Native releaseContext method not available", e);
      }
      nativeContext = 0;
    }
    tracksInitialized = false;
    endOfInput = false;
    released = true;
  }

  // JNI method declarations
  private native long nativeCreateContext(byte[] inputData, int inputLength);

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
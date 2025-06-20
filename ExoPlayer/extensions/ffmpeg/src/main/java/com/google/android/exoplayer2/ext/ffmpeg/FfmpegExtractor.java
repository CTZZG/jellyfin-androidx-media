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
import java.nio.ByteBuffer;
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
  // private static final int MAX_INPUT_LENGTH = 1500 * 1024 * 1024;
  private static byte[] ASF_SIGNATURE = new byte[]{
      (byte) 0x30, (byte) 0x26, (byte) 0xB2, (byte) 0x75,
      (byte) 0x8E, (byte) 0x66, (byte) 0xCF, (byte) 0x11,
      (byte) 0xA6, (byte) 0xD9, (byte) 0x00, (byte) 0xAA,
      (byte) 0x00, (byte) 0x62, (byte) 0xCE, (byte) 0x6C
  };
  private static final byte[] DSF_SIGNATURE = new byte[]{(byte) 'D', (byte) 'S', (byte) 'D', (byte) ' '};


  private static final int AV_CODEC_ID_WMAV1 = 0x15000 + 7;
  private static final int AV_CODEC_ID_WMAV2 = 0x15000 + 8;

  private static final int AV_CODEC_ID_DSD_LSBF = 88069;
  private static final int AV_CODEC_ID_DSD_MSBF = 88070;
  private static final int AV_CODEC_ID_DSD_LSBF_PLANAR = 88071;
  private static final int AV_CODEC_ID_DSD_MSBF_PLANAR = 88072;


  private long nativeContext;
  private ExtractorOutput extractorOutput;
  private TrackOutput trackOutput;
  private boolean tracksInitialized;
  private final byte[] packetBuffer;
  private final byte[] JNI_READ_BUFFER = new byte[32 * 1024];
  private final long[] timestampBuffer;
  private boolean endOfInput;
  private boolean released;
  private ExtractorInput currentInput;

  public FfmpegExtractor() {
    packetBuffer = new byte[PACKET_BUFFER_SIZE];
    timestampBuffer = new long[1];
    tracksInitialized = false;
    endOfInput = false;
    nativeContext = 0;
    Log.d(TAG, "FfmpegExtractor instance created.");
  }

  @Override
  public boolean sniff(ExtractorInput input) throws IOException {
    if (released) {
      return false;
    }

    byte[] sniffHeader = new byte[Math.max(ASF_SIGNATURE.length, DSF_SIGNATURE.length)];
    try {
      input.peekFully(sniffHeader, 0, ASF_SIGNATURE.length);
      if (Arrays.equals(Arrays.copyOf(sniffHeader, ASF_SIGNATURE.length), ASF_SIGNATURE)) {
          Log.d(TAG, "Sniffed ASF");
          return true;
      }
  } catch (IOException e) {
      // Not enough data or other issue
  }

    input.resetPeekPosition();
    try {
      input.peekFully(sniffHeader, 0, DSF_SIGNATURE.length);
      if (Arrays.equals(Arrays.copyOf(sniffHeader, DSF_SIGNATURE.length), DSF_SIGNATURE)) {
          Log.d(TAG, "Sniffed DSF");
          return true;
      }
  } catch (IOException e) {
      // Not enough data or other issue
  }
  input.resetPeekPosition();
  Log.d(TAG, "Sniff returned false");
    return false;
  }

  @Override
  public void init(ExtractorOutput output) {
    this.extractorOutput = output;
    Log.d(TAG, "Extractor initialized.");
  }

  @Override
  public @ReadResult int read(ExtractorInput input, PositionHolder seekPosition)
      throws IOException {
    if (released) {
      Log.d(TAG, "Read called on released extractor");
      return RESULT_END_OF_INPUT;
    }
    if (endOfInput) {
      return RESULT_END_OF_INPUT;
    }
    this.currentInput = input;

    try {
      if (nativeContext == 0) {
          if (!FfmpegLibrary.isAvailable()) {
              Log.e(TAG, "FFmpeg native libraries not available.");
              throw ParserException.createForMalformedContainer(
                      "Failed to load FFmpeg native libraries.", null);
          }
          Log.d(TAG, "Creating native context...");
          nativeContext = nativeCreateContext(this);
          if (nativeContext == 0) {
              Log.e(TAG, "Failed to create native context");
              throw ParserException.createForMalformedContainer("FFmpeg failed to open input", null);
          }
          Log.d(TAG, "Native context created: " + nativeContext);
      }

      if (!tracksInitialized) {
        Log.d(TAG, "Initializing tracks...");
        initializeTracks();
        tracksInitialized = true;
        extractorOutput.endTracks();
        Log.d(TAG, "Tracks initialized.");
      }

      return readSample();
    } catch (UnsatisfiedLinkError e) {
        Log.e(TAG, "Native method not available in FfmpegExtractor.read", e);
        throw new IOException("Native method link error", e);
    } catch (ParserException e) {
        Log.e(TAG, "ParserException in FfmpegExtractor.read", e);
        throw e; // Re-throw to allow ExoPlayer to handle it
    } catch (IOException e) {
        Log.e(TAG, "IOException in FfmpegExtractor.read", e);
        throw e;
  }
  }

  private void initializeTracks() throws ParserException {
    int codecId = nativeGetAudioCodecId(nativeContext);
    Log.d(TAG, "Native getAudioCodecId returned: " + codecId);
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
    Log.d(TAG, "MIME type for codec ID " + codecId + ": " + mimeType);

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

    Log.d(TAG, String.format("Track initialized - Codec ID: %d, MimeType: %s, Sample Rate: %d, Channels: %d, Bitrate: %d, Duration: %d us",
        codecId, mimeType, sampleRate, channelCount, bitRate, durationUs));
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
      case AV_CODEC_ID_DSD_REPORTED_FROM_LOG:
        return MimeTypes.AUDIO_DSD;
      default:
      Log.w(TAG, "Unknown codec ID from FFmpeg in getMimeTypeFromCodecId: " + codecId);
        return null;
    }
  }

  private int readSample() throws IOException {
    timestampBuffer[0] = C.TIME_UNSET;
    int packetSize = nativeReadPacket(nativeContext, packetBuffer, PACKET_BUFFER_SIZE, timestampBuffer);

    if (packetSize < 0) {
      if (packetSize == -541478725 || packetSize == C.RESULT_END_OF_INPUT) {
          Log.d(TAG, "End of input from nativeReadPacket: " + packetSize);
          endOfInput = true;
          return RESULT_END_OF_INPUT;
      }
      Log.e(TAG, "Error reading packet from native: " + packetSize);
      throw new IOException("FFmpeg nativeReadPacket error: " + packetSize);
  }

    if (packetSize == 0) {
      return RESULT_CONTINUE;
    }

    ParsableByteArray packetData = new ParsableByteArray(packetBuffer, packetSize);
    trackOutput.sampleData(packetData, packetSize);

    long sampleTimeUs = timestampBuffer[0];
    trackOutput.sampleMetadata(sampleTimeUs, C.BUFFER_FLAG_KEY_FRAME, packetSize, 0, null);

    return RESULT_CONTINUE;
  }
  @Override
  public void seek(long position, long timeUs) {
    if (released || nativeContext == 0) {
      return;
    }
    Log.d(TAG, "Java seek: position (ignored by current JNI): " + position + ", timeUs: " + timeUs);
        if (nativeSeek(nativeContext, timeUs)) {
            endOfInput = false;
        } else {
            Log.w(TAG, "Native seek failed for timeUs: " + timeUs);
        }
  }

  @Override
  public void release() {
    Log.d(TAG, "FfmpegExtractor.release() called. Native context: " + nativeContext);
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
    currentInput = null;
    Log.d(TAG, "FfmpegExtractor released.");
  }

  @SuppressWarnings("unused") // Called by JNI
  private int readFromExtractorInput(byte[] buffer, int offset, int length) {
      if (currentInput == null) {
          Log.e(TAG, "readFromExtractorInput: currentInput is null!");
          return C.RESULT_END_OF_INPUT; // Or throw an exception
      }
      try {
          // Log.d(TAG, "JNI_read: Requesting " + length + " bytes");
          int bytesRead = currentInput.read(buffer, offset, length);
          // if (bytesRead > 0) Log.d(TAG, "JNI_read: Read " + bytesRead + " bytes");
          // else if (bytesRead == C.RESULT_END_OF_INPUT) Log.d(TAG, "JNI_read: EOF");
          // else Log.d(TAG, "JNI_read: Read returned " + bytesRead);
          return bytesRead;
      } catch (IOException e) {
          Log.e(TAG, "IOException in readFromExtractorInput", e);
          return C.RESULT_END_OF_INPUT; // Treat IOExceptions as EOF for FFmpeg for now
      }
  }

  @SuppressWarnings("unused") // Called by JNI
  private long seekInExtractorInput(long position, int whence) {
      // FFmpeg's whence values (from avio.h):
      // #define SEEK_SET 0 /* seek from beginning of file */
      // #define SEEK_CUR 1 /* seek from current position */
      // #define SEEK_END 2 /* seek from end of file */
      // #define AVSEEK_SIZE 0x10000 /* get file size */
      // ExoPlayer's ExtractorInput doesn't directly map to SEEK_CUR, SEEK_END in the same way.
      // It primarily uses absolute positions.

      if (currentInput == null) {
          Log.e(TAG, "seekInExtractorInput: currentInput is null!");
          return -1;
      }
      try {
          if (whence == 0x10000 /*AVSEEK_SIZE*/) {
               Log.d(TAG, "JNI_seek: AVSEEK_SIZE requested");
              return currentInput.getLength();
          } else if (whence == 0 /*SEEK_SET*/) {
              currentInput.seekToPosition(position);
              return currentInput.getPosition();
          } else if (whence == 1 /*SEEK_CUR*/) {
              long newPos = currentInput.getPosition() + position;
              if (newPos < 0) newPos = 0;
              long length = currentInput.getLength();
              if (length != C.LENGTH_UNSET && newPos > length) newPos = length;
              currentInput.seekToPosition(newPos);
              return currentInput.getPosition();
          }
          Log.w(TAG, "seekInExtractorInput: Unsupported whence value: " + whence);
          return -1;
      } catch (IOException e) {
          Log.e(TAG, "IOException in seekInExtractorInput", e);
          return -1;
      }
  }
   @SuppressWarnings("unused")
  private long getLengthFromExtractorInput() {
      if (currentInput == null) {
          Log.e(TAG, "getLengthFromExtractorInput: currentInput is null!");
          return C.LENGTH_UNSET;
      }
      return currentInput.getLength();
  }


  // --- Native Methods ---
  // Pass 'this' (FfmpegExtractor instance) instead of byte[]
  private native long nativeCreateContext(FfmpegExtractor extractorInstance);

  private native int nativeGetAudioCodecId(long context);

  private native int nativeGetSampleRate(long context);

  private native int nativeGetChannelCount(long context);

  private native long nativeGetBitRate(long context);

  private native int nativeGetBlockAlign(long context);

  private native byte[] nativeGetExtraData(long context);

  private native int nativeReadPacket(long context, byte[] outputBuffer, int outputBufferSize, long[] timestampOut);

  private native long nativeGetDuration(long context);

  private native boolean nativeSeek(long context, long timeUs);

  private native void nativeReleaseContext(long context);
}
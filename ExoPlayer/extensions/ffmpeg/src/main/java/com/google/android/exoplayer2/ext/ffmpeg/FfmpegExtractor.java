package com.google.android.exoplayer2.ext.ffmpeg;

import androidx.annotation.Nullable;
import com.google.android.exoplayer2.C;
import com.google.android.exoplayer2.Format;
import com.google.android.exoplayer2.extractor.Extractor;
import com.google.android.exoplayer2.extractor.ExtractorInput;
import com.google.android.exoplayer2.extractor.ExtractorOutput;
import com.google.android.exoplayer2.extractor.ExtractorsFactory;
import com.google.android.exoplayer2.extractor.PositionHolder;
import com.google.android.exoplayer2.extractor.SeekMap;
import com.google.android.exoplayer2.extractor.SeekPoint; // For SeekMap.SeekPoints
import com.google.android.exoplayer2.extractor.TrackOutput;
import com.google.android.exoplayer2.util.Assertions;
import com.google.android.exoplayer2.util.Log;
import com.google.android.exoplayer2.util.MimeTypes; // Assuming AUDIO_DSD is or will be defined here
import com.google.android.exoplayer2.util.Util;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.util.Collections;
import java.util.List;

/**
 * An {@link Extractor} that uses FFmpeg to extract samples from a media stream.
 * Initially focused on DSF/DSD.
 */
public final class FfmpegExtractor implements Extractor {

    public static final ExtractorsFactory FACTORY = () -> new Extractor[]{new FfmpegExtractor()};

    private static final String TAG = "FfmpegExtractor";

    // DSF magic bytes: "DSD "
    private static final int DSF_MAGIC_BYTES = 0x44534420; // 'D', 'S', 'D', ' '

    // Define a MIME type for DSD if not already in MimeTypes.java
    // public static final String MIMETYPE_AUDIO_DSD = "audio/dsd"; // Or "audio/x-dsd"

    private long nativeContextHandle; // Pointer to native (extractor) context

    private ExtractorOutput extractorOutput;
    private TrackOutput trackOutput; // Assuming single audio track for DSF
    private boolean tracksBuilt;
    private @Nullable FfmpegSeekMap seekMap;

    private @Nullable ByteBuffer packetBuffer;
    // Default capacity, can be adjusted. FFmpeg might suggest a size after opening.
    private int packetBufferCapacity = 256 * 1024;

    public FfmpegExtractor() {
        if (!FfmpegLibrary.isAvailable()) {
            throw new IllegalStateException("FFmpeg library not available for FfmpegExtractor.");
        }
    }

    @Override
    public boolean sniff(ExtractorInput input) throws IOException {
        ParsableByteArray scratch = new ParsableByteArray(4);
        input.peekFully(scratch.getData(), 0, 4);
        if (scratch.readInt() == DSF_MAGIC_BYTES) {
            Log.d(TAG, "DSF format sniffed successfully.");
            return true;
        }
        return false;
    }

    @Override
    public void init(ExtractorOutput output) {
        this.extractorOutput = output;
        this.nativeContextHandle = nativeCreateExtractorContext();
        if (this.nativeContextHandle == 0) {
            throw new IllegalStateException("Failed to create native FFmpeg extractor context.");
        }
        Log.d(TAG, "FfmpegExtractor initialized with native context: " + nativeContextHandle);
    }

    @Override
    public int read(ExtractorInput input, PositionHolder seekPosition) throws IOException {
        Assertions.checkStateNotNull(extractorOutput);
        Assertions.checkState(nativeContextHandle != 0, "Native context is not initialized.");

        if (!tracksBuilt) {
            // The JNI layer will need to use custom I/O to read from ExtractorInput
            int openResult = nativeOpenInput(nativeContextHandle, input);
            if (openResult < 0) {
                Log.e(TAG, "Failed to open media via FFmpeg (custom IO), error code: " + openResult);
                throw new FfmpegDecoderException("FFmpeg failed to open media: " + getErrorString(openResult));
            }

            buildTracksAndSeekMap(); // This will call JNI to get stream info
            tracksBuilt = true;
        }

        if (packetBuffer == null) {
            // Allocate a direct ByteBuffer for JNI
            packetBuffer = ByteBuffer.allocateDirect(packetBufferCapacity);
        }
        packetBuffer.clear();

        FfmpegPacketInfo packetInfo = nativeReadPacket(nativeContextHandle, packetBuffer);

        if (packetInfo == null) {
            Log.e(TAG, "nativeReadPacket returned null, implies an unrecoverable error or misconfiguration.");
            return RESULT_END_OF_INPUT;
        }

        if (packetInfo.isError) {
            Log.e(TAG, "Error during nativeReadPacket: " + packetInfo.errorCode);
            throw new FfmpegDecoderException("FFmpeg error during packet read: " + getErrorString(packetInfo.errorCode));
        }

        if (packetInfo.isEof) {
            Log.d(TAG, "End of input reached by nativeReadPacket.");
            return RESULT_END_OF_INPUT;
        }

        if (trackOutput == null || packetInfo.streamIndex != 0) { // Assuming DSF has one audio stream at index 0
            Log.w(TAG, "Invalid stream index (" + packetInfo.streamIndex + ") or trackOutput not ready. Skipping packet.");
            return RESULT_CONTINUE;
        }

        packetBuffer.position(0);
        packetBuffer.limit(packetInfo.size);

        trackOutput.sampleData(packetBuffer, packetInfo.size);
        trackOutput.sampleMetadata(
                packetInfo.ptsUs,
                packetInfo.flags, // Map FFmpeg AV_PKT_FLAG_KEY to C.BUFFER_FLAG_KEY_FRAME
                packetInfo.size,
                0,
                null);

        return RESULT_CONTINUE;
    }

    @Override
    public void seek(long position, long timeUs) {
        Log.d(TAG, "Seek requested to position: " + position + ", timeUs: " + timeUs);
        if (nativeContextHandle != 0 && seekMap != null && seekMap.isSeekable()) {
            int result = nativeSeekTo(nativeContextHandle, timeUs);
            if (result < 0) {
                Log.w(TAG, "Native seek failed with error: " + result);
                // Depending on the error, you might want to throw an IOException
            }
        }
    }

    @Override
    public void release() {
        Log.d(TAG, "Releasing FfmpegExtractor. Native context: " + nativeContextHandle);
        if (nativeContextHandle != 0) {
            nativeReleaseExtractorContext(nativeContextHandle);
            nativeContextHandle = 0;
        }
        packetBuffer = null;
        trackOutput = null;
        tracksBuilt = false;
        seekMap = null;
    }

    private void buildTracksAndSeekMap() throws FfmpegDecoderException {
        // For DSF, we expect one audio stream.
        FfmpegStreamInfo streamInfo = nativeGetStreamFormat(nativeContextHandle, 0 /* streamIndex */);

        if (streamInfo == null) {
            throw new FfmpegDecoderException("Failed to get stream format from FFmpeg for stream 0.");
        }

        Log.d(TAG, "Stream 0: Codec=" + streamInfo.codecName +
                ", Channels=" + streamInfo.channels +
                ", SampleRate=" + streamInfo.sampleRate);

        // This extractor assumes a single track for simplicity with DSF
        trackOutput = extractorOutput.track(0, C.TRACK_TYPE_AUDIO);

        Format.Builder formatBuilder = new Format.Builder();
        // For DSD, the MIME type needs to be recognized by FfmpegAudioDecoder
        // Update FfmpegLibrary.getCodecName to map this back to "dsd_lsbf_planar" or similar.
        formatBuilder.setSampleMimeType(MimeTypes.AUDIO_DSD); // Or your custom MIMETYPE_AUDIO_DSD

        formatBuilder
                .setChannelCount(streamInfo.channels)
                .setSampleRate(streamInfo.sampleRate);

        // DSD specific: FFmpeg decoders for DSD might output PCM.
        // The extractor should describe the *encoded* format (DSD).
        // The FfmpegAudioDecoder will handle the actual decoding to PCM and report its output format.
        // So, no pcmEncoding here.

        if (streamInfo.extradata != null && streamInfo.extradata.length > 0) {
            formatBuilder.setInitializationData(Collections.singletonList(streamInfo.extradata));
        }
        if (streamInfo.averageBitrate > 0) {
            formatBuilder.setAverageBitrate(streamInfo.averageBitrate);
        }

        trackOutput.format(formatBuilder.build());

        long durationUs = nativeGetDurationUs(nativeContextHandle);
        boolean isSeekable = nativeIsSeekable(nativeContextHandle);
        Log.d(TAG, "Media duration: " + durationUs + " us, Is seekable: " + isSeekable);

        this.seekMap = new FfmpegSeekMap(durationUs, isSeekable);
        extractorOutput.seekMap(this.seekMap);
        extractorOutput.endTracks(); // Call after all tracks are built and seek map is set
    }

    // Helper to get error string, can be moved to FfmpegLibrary if generic enough
    private static String getErrorString(int errorCode) {
        // Ideally, FfmpegLibrary would have a JNI method to call av_strerror
        return "FFmpeg error code: " + errorCode;
    }

    // --- Native JNI Method Declarations (to be implemented in ffmpeg_jni.cc) ---

    /**
     * Creates and initializes the native FFmpeg context for the extractor.
     * This context will hold AVFormatContext, custom AVIOContext, etc.
     * @return A handle (pointer) to the native context, or 0 on failure.
     */
    private static native long nativeCreateExtractorContext();

    /**
     * Opens the input specified by ExtractorInput using custom FFmpeg I/O.
     * This involves setting up an AVIOContext that reads from the ExtractorInput.
     * It should call avformat_open_input and avformat_find_stream_info.
     *
     * @param contextHandle The native context handle.
     * @param extractorInput The Java ExtractorInput to read from.
     * @return 0 on success, or a negative FFmpeg error code on failure.
     */
    private static native int nativeOpenInput(long contextHandle, ExtractorInput extractorInput);

    /**
     * Retrieves format information for a specific stream.
     * Must be called after nativeOpenInput has successfully found stream info.
     *
     * @param contextHandle The native context handle.
     * @param streamIndex The index of the stream.
     * @return An FfmpegStreamInfo object, or null on failure.
     */
    private static native @Nullable FfmpegStreamInfo nativeGetStreamFormat(long contextHandle, int streamIndex);

    /**
     * Reads the next packet from the media stream.
     *
     * @param contextHandle The native context handle.
     * @param outputBuffer A direct ByteBuffer to be filled with packet data.
     *                     The JNI layer must set position and limit correctly.
     * @return An FfmpegPacketInfo object containing packet metadata and status.
     *         Should not be null; use isError/isEof fields for status.
     */
    private static native FfmpegPacketInfo nativeReadPacket(long contextHandle, ByteBuffer outputBuffer);

    /**
     * Seeks to the specified time in the media stream.
     *
     * @param contextHandle The native context handle.
     * @param timeUs The target time in microseconds.
     * @return 0 on success, or a negative FFmpeg error code on failure.
     */
    private static native int nativeSeekTo(long contextHandle, long timeUs);

    /**
     * Gets the duration of the media.
     * @param contextHandle The native context handle.
     * @return Duration in microseconds, or C.TIME_UNSET if unknown.
     */
    private static native long nativeGetDurationUs(long contextHandle);

    /**
     * Checks if the media is seekable.
     * @param contextHandle The native context handle.
     * @return True if seekable, false otherwise.
     */
    private static native boolean nativeIsSeekable(long contextHandle);

    /**
     * Releases the native FFmpeg extractor context and associated resources.
     * @param contextHandle The native context handle.
     */
    private static native void nativeReleaseExtractorContext(long contextHandle);


    // --- JNI Data Transfer Objects ---

    /**
     * Holds stream format information retrieved from JNI.
     * Fields should be populated by the native side.
     */
    private static class FfmpegStreamInfo {
        public final String codecName; // e.g., "dsd_lsbf_planar"
        public final int channels;
        public final int sampleRate;
        public final int averageBitrate; // bps
        @Nullable public final byte[] extradata;

        // Constructor called from JNI
        public FfmpegStreamInfo(String codecName, int channels, int sampleRate, int averageBitrate, @Nullable byte[] extradata) {
            this.codecName = codecName;
            this.channels = channels;
            this.sampleRate = sampleRate;
            this.averageBitrate = averageBitrate;
            this.extradata = extradata;
        }
    }

    /**
     * Holds packet information retrieved from JNI.
     * Fields should be populated by the native side.
     */
    private static class FfmpegPacketInfo {
        public final int streamIndex;
        public final long ptsUs;
        public final int flags; // ExoPlayer C.BUFFER_FLAG_*
        public final int size;
        public final boolean isEof;
        public final boolean isError;
        public final int errorCode; // FFmpeg error code if isError is true

        // Constructor called from JNI
        public FfmpegPacketInfo(int streamIndex, long ptsUs, int flags, int size, boolean isEof, boolean isError, int errorCode) {
            this.streamIndex = streamIndex;
            this.ptsUs = ptsUs;
            this.flags = flags;
            this.size = size;
            this.isEof = isEof;
            this.isError = isError;
            this.errorCode = errorCode;
        }
    }

    /**
     * A simple SeekMap based on FFmpeg's reported duration and seekability.
     */
    private static class FfmpegSeekMap implements SeekMap {
        private final long durationUs;
        private final boolean isSeekable;

        public FfmpegSeekMap(long durationUs, boolean isSeekable) {
            this.durationUs = (durationUs <= 0 && !isSeekable) ? C.TIME_UNSET : durationUs;
            this.isSeekable = isSeekable;
        }

        @Override
        public boolean isSeekable() {
            return isSeekable;
        }

        @Override
        public long getDurationUs() {
            return durationUs;
        }

        @Override
        public SeekPoints getSeekPoints(long timeUs) {
            if (!isSeekable) {
                return new SeekPoints(SeekPoint.START);
            }
            // FFmpeg handles seeking internally. We provide the timeUs to FFmpeg,
            // and it seeks to the nearest keyframe before or at that time.
            // The actual byte position is not directly exposed or needed by ExoPlayer here.
            return new SeekPoints(new SeekPoint(timeUs, 0)); // Position 0 is a placeholder
        }
    }
}
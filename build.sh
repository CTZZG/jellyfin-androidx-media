#!/bin/bash

# Ensure NDK is available
# 使用由 GitHub Actions 工作流设置的 ANDROID_NDK_HOME
export ANDROID_NDK_PATH=$ANDROID_NDK_HOME

[[ -z "$ANDROID_NDK_PATH" ]] && echo "ANDROID_NDK_HOME (and thus ANDROID_NDK_PATH) is not set, quitting…" && exit 1
[[ ! -d "$ANDROID_NDK_PATH" ]] && echo "ANDROID_NDK_PATH directory '$ANDROID_NDK_PATH' does not exist, quitting..." && exit 1
echo "Using ANDROID_NDK_PATH: ${ANDROID_NDK_PATH}"
echo "Contents of ANDROID_NDK_PATH:"
ls -la "${ANDROID_NDK_PATH}"
echo "Contents of toolchains directory:"
ls -la "${ANDROID_NDK_PATH}/toolchains/llvm/prebuilt/linux-x86_64/bin"


# Setup environment
export EXOPLAYER_ROOT="${PWD}/ExoPlayer"
export FFMPEG_EXT_PATH="${EXOPLAYER_ROOT}/extensions/ffmpeg/src/main"
export FFMPEG_PATH="${PWD}/ffmpeg" # 确保 ffmpeg 子模块已检出且位于此路径
export ENABLED_DECODERS=(vorbis opus flac alac pcm_mulaw pcm_alaw mp3 aac ac3 eac3 dca mlp truehd dsd_lsbf dsd_lsbf_planar dsd_msbf dsd_msbf_planar wmav2)

# Create softlink to ffmpeg
# 确保目标 JNI 目录存在
mkdir -p "${FFMPEG_EXT_PATH}/jni"
ln -sfn "${FFMPEG_PATH}" "${FFMPEG_EXT_PATH}/jni/ffmpeg" # 使用 -n 和 -f 增强鲁棒性

# Start build
cd "${FFMPEG_EXT_PATH}/jni"
# 确保 build_ffmpeg.sh 可执行
chmod +x ./build_ffmpeg.sh
./build_ffmpeg.sh "${FFMPEG_EXT_PATH}" "${ANDROID_NDK_PATH}" "linux-x86_64" "${ENABLED_DECODERS[@]}"

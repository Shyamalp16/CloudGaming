#include "Encoder.h"
#include "GlobalTime.h"
#include "pion_webrtc.h"
#include <functional>
#include <mutex>
#include <condition_variable>
#include <fstream> // For debug file output
#include <chrono>

// Define variables in the Encoder namespace
SwsContext* Encoder::swsCtx = nullptr;
AVFrame* Encoder::nv12Frame = nullptr;
int Encoder::currentWidth = 0;
int Encoder::currentHeight = 0;

std::mutex Encoder::g_encoderMutex;

// Static variables for encoder state
AVFormatContext* Encoder::formatCtx = nullptr;
AVCodecContext* Encoder::codecCtx = nullptr;
AVStream* Encoder::videoStream = nullptr;
AVPacket* Encoder::packet = nullptr;
AVFrame* Encoder::hwFrame = nullptr;
AVBufferRef* Encoder::hwDeviceCtx = nullptr;
int Encoder::frameCounter = 0;
int64_t Encoder::last_dts = 0;

//#define ENCODER "h264_nvenc"
#define ENCODER "libx264"
//#define ENCODER "openh264"

// Define callback variable in Encoder namespace
Encoder::EncodedFrameCallback Encoder::g_onEncodedFrameCallback = nullptr;

// New variables for storing the latest encoded frame
std::mutex Encoder::g_frameMutex;
std::condition_variable Encoder::g_frameAvailable;
std::vector<uint8_t> Encoder::g_latestFrameData;
int64_t Encoder::g_latestPTS = 0;
bool Encoder::g_frameReady = false;

// File for debugging raw H.264 bitstream
std::ofstream debugH264File;

static std::chrono::steady_clock::time_point encoderStartTime;
static bool isFirstFrame = true;


namespace Encoder {
    void setEncodedFrameCallback(EncodedFrameCallback callback) {
        g_onEncodedFrameCallback = callback;
    }

    // Function to get the latest encoded frame
    bool getEncodedFrame(std::vector<uint8_t>& frameData, int64_t& pts) {
        std::unique_lock<std::mutex> lock(g_frameMutex);
        if (!g_frameReady) {
            g_frameAvailable.wait(lock, [] { return g_frameReady; });
        }

        if (g_latestFrameData.empty()) {
            return false;
        }

        frameData = g_latestFrameData; // Copy the data
        pts = g_latestPTS;
        g_frameReady = false; // Mark as consumed
        lock.unlock();
        g_frameAvailable.notify_one(); // Allow new frames to overwrite
        return true;
    }

    // Function to parse and log NAL unit types for debugging
    void logNALUnits(const uint8_t* data, int size) {
        int pos = 0;
        while (pos < size) {
            // Look for start code (0x00000001)
            if (pos + 3 >= size) break;
            if (data[pos] == 0x00 && data[pos + 1] == 0x00 && data[pos + 2] == 0x00 && data[pos + 3] == 0x01) {
                pos += 4;
                if (pos >= size) break;
                uint8_t nalUnitType = data[pos] & 0x1F;
                std::wcout << L"[DEBUG] NAL Unit Type: " << (int)nalUnitType << L"\n";
                // Find the next start code to determine the size
                int nextPos = pos + 1;
                while (nextPos + 3 < size) {
                    if (data[nextPos] == 0x00 && data[nextPos + 1] == 0x00 && data[nextPos + 2] == 0x00 && data[nextPos + 3] == 0x01) {
                        break;
                    }
                    nextPos++;
                }
                int nalSize = (nextPos + 3 < size) ? (nextPos - pos + 3) : (size - pos);
                std::wcout << L"[DEBUG] NAL Unit Size: " << nalSize << L"\n";
                pos = nextPos;
            }
            else {
                pos++;
            }
        }
    }

    // Update pushPacketToWebRTC to store the frame and log NAL units
    void pushPacketToWebRTC(AVPacket* packet) {
        std::wcout << L"[WebRTC] Pushing encoded packets (PTS: " << packet->pts << L") to WebRTC module.\n";
        {
            std::lock_guard<std::mutex> lock(g_frameMutex);
            g_latestFrameData.assign(packet->data, packet->data + packet->size);
            g_latestPTS = packet->pts;
            g_frameReady = true;
        }
        g_frameAvailable.notify_one();

        // Log the first 20 bytes and NAL unit types
        std::wcout << L"[WebRTC] Packet Size:" << packet->size << L", First 20 Bytes: ";
        for (int i = 0; i < std::min(20, packet->size); i++) {
            std::wcout << std::hex << (int)packet->data[i] << L" ";
        }
        std::wcout << std::dec << L"\n";

        // Log NAL unit types
        logNALUnits(packet->data, packet->size);

        // Save to debug file
        if (debugH264File.is_open()) {
            debugH264File.write(reinterpret_cast<const char*>(packet->data), packet->size);
        }

        int result = sendVideoPacket(packet->data, packet->size, packet->pts);
        if (result != 0) {
            std::wcerr << L"[WebRTC] Failed to send video packet to WebRTC module. Error code: " << result << "\n";
        }
    }

    //void InitializeEncoder(const std::string& fileName, int width, int height, int fps) {
    //    std::lock_guard<std::mutex> lock(g_encoderMutex);
    //    std::wcout << L"[DEBUG] Initializing encoder\n";

    //    // Open debug file for raw H.264 bitstream
    //    debugH264File.open("debug_h264_stream.h264", std::ios::binary);
    //    if (!debugH264File.is_open()) {
    //        std::wcerr << L"[DEBUG] Failed to open debug_h264_stream.h264 for writing\n";
    //    }

    //    avformat_alloc_output_context2(&formatCtx, nullptr, "mp4", fileName.c_str());
    //    if (!formatCtx) {
    //        std::cerr << "[Encoder] Failed to create output context.\n";
    //        return;
    //    }

    //    std::wcout << L"[DEBUG] Allocated output context\n";

    //    const AVCodec* codec = avcodec_find_encoder_by_name(ENCODER);
    //    if (!codec) {
    //        std::cerr << "[Encoder] Failed to find encoder: " << ENCODER << "\n";
    //        return;
    //    }

    //    std::wcout << L"[DEBUG] Found encoder: " << ENCODER << "\n";

    //    videoStream = avformat_new_stream(formatCtx, codec);
    //    if (!videoStream) {
    //        std::cerr << "[Encoder] Failed to create new stream.\n";
    //        return;
    //    }

    //    std::wcout << L"[DEBUG] Created video stream\n";

    //    codecCtx = avcodec_alloc_context3(codec);
    //    if (!codecCtx) {
    //        std::cerr << "[Encoder] Failed to allocate codec context.\n";
    //        return;
    //    }

    //    std::wcout << L"[DEBUG] Allocated codec context\n";

    //    codecCtx->width = 1920;
    //    codecCtx->height = 1080;
    //    codecCtx->time_base = AVRational{ 1, fps };
    //    videoStream->time_base = codecCtx->time_base;
    //    codecCtx->framerate = { fps, 1 };
    //    codecCtx->gop_size = 30; // Keyframe every 30 frames (1 second at 30 FPS)
    //    codecCtx->max_b_frames = 0;
    //    codecCtx->pix_fmt = AV_PIX_FMT_CUDA;  // Use CUDA pixel format
    //    codecCtx->bit_rate = 4000000;

    //    // Set H.264 profile and level for better compatibility
    //    codecCtx->profile = FF_PROFILE_H264_MAIN; // Main profile
    //    codecCtx->level = 31; // Level 3.1

    //    std::wcout << L"[DEBUG] Setting up hardware context\n";

    //    if (av_hwdevice_ctx_create(&hwDeviceCtx, AV_HWDEVICE_TYPE_CUDA, nullptr, nullptr, 0) < 0) {
    //        std::cerr << "[Encoder] Failed to create hardware device context. NVENC might not be supported on your system.\n";
    //        return;
    //    }

    //    std::wcout << L"[DEBUG] Hardware device context created successfully\n";

    //    codecCtx->hw_frames_ctx = av_hwframe_ctx_alloc(hwDeviceCtx);
    //    if (!codecCtx->hw_frames_ctx) {
    //        std::cerr << "[Encoder] Failed to allocate hardware frames context!\n";
    //        return;
    //    }

    //    AVHWFramesContext* framesCtx = (AVHWFramesContext*)codecCtx->hw_frames_ctx->data;
    //    framesCtx->format = AV_PIX_FMT_CUDA;
    //    framesCtx->sw_format = AV_PIX_FMT_NV12;
    //    framesCtx->width = width;
    //    framesCtx->height = height;
    //    framesCtx->initial_pool_size = 20;

    //    framesCtx->device_ref = av_buffer_ref(hwDeviceCtx);

    //    if (av_hwframe_ctx_init(codecCtx->hw_frames_ctx) < 0) {
    //        std::cerr << "[Encoder] Failed to initialize hardware frames context!\n";
    //        return;
    //    }

    //    std::wcout << L"[DEBUG] Hardware frame context initialized successfully\n";

    //    // Set NVENC-specific options
    //    AVDictionary* opts = nullptr;
    //    av_dict_set(&opts, "preset", "medium", 0); // Use a more conservative preset
    //    av_dict_set(&opts, "rc", "cbr", 0); // Constant bitrate for stability
    //    av_dict_set(&opts, "forced-idr", "1", 0); // Force IDR frame at the start
    //    av_dict_set(&opts, "level", "3.1", 0); // Enforce Level 3.1
    //    av_dict_set(&opts, "zerolatency", "1", 0); // Enable zero-latency mode for real-time streaming

    //    if (avcodec_open2(codecCtx, codec, &opts) < 0) {
    //        std::cerr << "[Encoder] Failed to open codec.\n";
    //        av_dict_free(&opts);
    //        return;
    //    }
    //    av_dict_free(&opts);

    //    std::wcout << L"[DEBUG] Codec opened successfully\n";

    //    avcodec_parameters_from_context(videoStream->codecpar, codecCtx);

    //    packet = av_packet_alloc();
    //    if (!packet) {
    //        std::cerr << "[Encoder] Failed to allocate packet.\n";
    //        return;
    //    }

    //    std::wcout << L"[Encoder] Hardware Accelerated Encoder Initialized.\n";
    //}

void InitializeEncoder(const std::string& fileName, int width, int height, int fps) {
    std::lock_guard<std::mutex> lock(g_encoderMutex);
    std::wcout << L"[DEBUG] Initializing encoder\n";

    // Open debug file for raw H.264 bitstream
    debugH264File.open("debug_h264_stream.h264", std::ios::binary);
    if (!debugH264File.is_open()) {
        std::wcerr << L"[DEBUG] Failed to open debug_h264_stream.h264 for writing\n";
    }

    avformat_alloc_output_context2(&formatCtx, nullptr, "mp4", fileName.c_str());
    if (!formatCtx) {
        std::cerr << "[Encoder] Failed to create output context.\n";
        return;
    }

    std::wcout << L"[DEBUG] Allocated output context\n";

    const AVCodec* codec = avcodec_find_encoder_by_name(ENCODER);
    if (!codec) {
        std::cerr << "[Encoder] Failed to find encoder: " << ENCODER << "\n";
        return;
    }

    std::wcout << L"[DEBUG] Found encoder: " << ENCODER << "\n";

    videoStream = avformat_new_stream(formatCtx, codec);
    if (!videoStream) {
        std::cerr << "[Encoder] Failed to create new stream.\n";
        return;
    }

    std::wcout << L"[DEBUG] Created video stream\n";

    codecCtx = avcodec_alloc_context3(codec);
    if (!codecCtx) {
        std::cerr << "[Encoder] Failed to allocate codec context.\n";
        return;
    }

    std::wcout << L"[DEBUG] Allocated codec context\n";

    codecCtx->width = 1920;
    codecCtx->height = 1080;
    codecCtx->time_base = AVRational{ 1, fps };
    videoStream->time_base = codecCtx->time_base;
    codecCtx->framerate = { fps, 1 };
    codecCtx->gop_size = 30; // Keyframe every 30 frames (1 second at 30 FPS)
    codecCtx->max_b_frames = 0;
    codecCtx->pix_fmt = AV_PIX_FMT_YUV420P; // Use YUV420P for software encoding
    codecCtx->bit_rate = 4000000;

    // Set H.264 profile and level for better compatibility
    codecCtx->profile = FF_PROFILE_H264_BASELINE; // Main profile
    codecCtx->level = 42; // Level 4.0 to support 1920x1080

    // Set libx264-specific options
    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "preset", "veryfast", 0); // Fast encoding for testing
    av_dict_set(&opts, "tune", "zerolatency", 0); // Zero latency for real-time streaming
    av_dict_set(&opts, "level", "4.2", 0); // Enforce Level 4.0

    if (avcodec_open2(codecCtx, codec, &opts) < 0) {
        std::cerr << "[Encoder] Failed to open codec.\n";
        av_dict_free(&opts);
        return;
    }
    av_dict_free(&opts);

    std::wcout << L"[DEBUG] Codec opened successfully\n";

    avcodec_parameters_from_context(videoStream->codecpar, codecCtx);

    packet = av_packet_alloc();
    if (!packet) {
        std::cerr << "[Encoder] Failed to allocate packet.\n";
        return;
    }

    std::wcout << L"[Encoder] Software Encoder Initialized.\n";
}

    //void EncodeFrame() {
    //    try {
    //        std::wcout << L"[DEBUG] EncodeFrame() - Start\n";

    //        if (!nv12Frame) {
    //            std::cerr << "[Encoder] Invalid frame (nv12Frame is NULL).\n";
    //            return;
    //        }

    //        if (!codecCtx) {
    //            std::cerr << "[Encoder] Codec context not initialized.\n";
    //            return;
    //        }

    //        if (!codecCtx->hw_frames_ctx) {
    //            std::cerr << "[Encoder] Hardware frame context is NULL! Exiting.\n";
    //            return;
    //        }

    //        // Use a consistent PTS increment for 30 FPS (33333 microseconds per frame)
    //        int64_t frameDurationUS = 33333; // 1/30th of a second in microseconds
    //        nv12Frame->pts = frameCounter * frameDurationUS;

    //        // Force a keyframe every 30 frames (already set via gop_size, but we can reinforce it)
    //        if (frameCounter == 0 || frameCounter % 30 == 0) {
    //            nv12Frame->pict_type = AV_PICTURE_TYPE_I;
    //            std::wcout << L"[DEBUG] Forcing KeyFrame at frame " << frameCounter << L"\n";
    //        }

    //        std::wcout << L"[DEBUG] Allocating GPU frame\n";
    //        AVFrame* hwFrame = av_frame_alloc();
    //        if (!hwFrame) {
    //            std::cerr << "[Encoder] Failed to allocate hwFrame.\n";
    //            return;
    //        }

    //        hwFrame->format = AV_PIX_FMT_CUDA;
    //        hwFrame->width = codecCtx->width;
    //        hwFrame->height = codecCtx->height;
    //        hwFrame->pts = nv12Frame->pts;

    //        std::wcout << L"[DEBUG] Allocating hardware buffer\n";
    //        int ret = av_hwframe_get_buffer(codecCtx->hw_frames_ctx, hwFrame, 0);
    //        if (ret < 0) {
    //            std::cerr << "[Encoder] Failed to allocate hardware buffer. FFmpeg Error Code: " << ret << "\n";
    //            return;
    //        }

    //        std::wcout << L"[DEBUG] Uploading NV12 frame to CUDA memory\n";
    //        ret = av_hwframe_transfer_data(hwFrame, nv12Frame, 0);
    //        if (ret < 0) {
    //            std::cerr << "[Encoder] Failed to upload frame to CUDA. FFmpeg Error Code: " << ret << "\n";
    //            return;
    //        }

    //        hwFrame->pts = nv12Frame->pts;

    //        std::wcout << L"[DEBUG] Sending frame to encoder\n";
    //        if (avcodec_send_frame(codecCtx, hwFrame) < 0) {
    //            std::cerr << "[Encoder] Failed to send frame to encoder.\n";
    //            return;
    //        }

    //        while (avcodec_receive_packet(codecCtx, packet) == 0) {
    //            packet->stream_index = videoStream->index;
    //            av_packet_rescale_ts(packet, codecCtx->time_base, videoStream->time_base);

    //            if (packet->dts < last_dts) {
    //                packet->dts = last_dts + 1;
    //            }
    //            last_dts = packet->dts;

    //            if (g_onEncodedFrameCallback) {
    //                g_onEncodedFrameCallback(packet);
    //            }
    //            else {
    //                pushPacketToWebRTC(packet);
    //            }

    //            av_packet_unref(packet);
    //        }
    //        av_frame_free(&hwFrame);
    //        frameCounter++;
    //        std::wcout << L"[DEBUG] Frame encoded successfully. Frame Counter: " << frameCounter << L", PTS: " << nv12Frame->pts << L"\n";
    //    }
    //    catch (const std::exception& e) {
    //        std::cerr << "[EXCEPTION] EncodeFrame() - Exception caught: " << e.what() << "\n";
    //    }
    //    catch (...) {
    //        std::cerr << "[EXCEPTION] EncodeFrame() - Unknown exception caught!\n";
    //    }
    //}

void EncodeFrame() {
    try {
        std::wcout << L"[DEBUG] EncodeFrame() - Start (Updated Version)\n";

        if (!nv12Frame) {
            std::cerr << "[Encoder] Invalid frame (nv12Frame is NULL).\n";
            return;
        }

        if (!codecCtx) {
            std::cerr << "[Encoder] Codec context not initialized.\n";
            return;
        }

        // Log frame dimensions and format before any changes
        std::wcout << L"[DEBUG] EncodeFrame() - Initial frame: width=" << nv12Frame->width
            << L", height=" << nv12Frame->height << L", format=" << nv12Frame->format << L"\n";

        // Ensure the frame format is set to YUV420P
        if (nv12Frame->format != AV_PIX_FMT_YUV420P) {
            std::wcout << L"[DEBUG] Fixing frame format: setting to AV_PIX_FMT_YUV420P\n";
            nv12Frame->format = AV_PIX_FMT_YUV420P;
        }

        // Verify frame dimensions and strides
        std::wcout << L"[DEBUG] EncodeFrame() - Frame dimensions: width=" << nv12Frame->width
            << L", height=" << nv12Frame->height << L"\n";
        std::wcout << L"[DEBUG] EncodeFrame() - Frame format: " << nv12Frame->format << L"\n";
        std::wcout << L"[DEBUG] EncodeFrame() - Strides: linesize[0]=" << nv12Frame->linesize[0]
            << L", linesize[1]=" << nv12Frame->linesize[1] << L", linesize[2]=" << nv12Frame->linesize[2] << L"\n";

        // Ensure the frame is writable
        if (av_frame_make_writable(nv12Frame) < 0) {
            std::cerr << "[Encoder] Failed to make nv12Frame writable.\n";
            return;
        }

        // Set the strides correctly for YUV420P
        //nv12Frame->linesize[0] = 1920; // Luma (Y) stride
        //nv12Frame->linesize[1] = 960;  // U plane stride (1920/2 for YUV420P)
        //nv12Frame->linesize[2] = 960;  // V plane stride (1920/2 for YUV420P)

        if (isFirstFrame) {
            encoderStartTime = std::chrono::steady_clock::now();
            isFirstFrame = false;
        }

        auto currentTime = std::chrono::steady_clock::now();
        auto elapsedTime = std::chrono::duration_cast<std::chrono::microseconds> (currentTime - encoderStartTime).count();

        // Convert elapsed time to the codec's time base (1/30 seconds)
        // codecCtx->time_base = {1, 30}, so 1 second = 30 time base units
        // 1 microsecond = 1/1,000,000 seconds, so elapsedTimeUS / 1,000,000 seconds
        // PTS in time base units = (elapsedTimeUS / 1,000,000) * 30
		int64_t pts = (elapsedTime * 30) / 1000000;
        nv12Frame->pts = pts;

        // Use a consistent PTS increment for 30 FPS (33333 microseconds per frame)
        //int64_t frameDurationUS = 33333; // 1/30th of a second in microseconds
        //nv12Frame->pts = frameCounter * frameDurationUS;

        // Force a keyframe every 30 frames
        if (frameCounter == 0 || frameCounter % 30 == 0) {
            nv12Frame->pict_type = AV_PICTURE_TYPE_I;
            std::wcout << L"[DEBUG] Forcing KeyFrame at frame " << frameCounter << L"\n";
        }

        std::wcout << L"[DEBUG] Sending frame to encoder, PTS=" << nv12Frame->pts << L"\n";
        int ret = avcodec_send_frame(codecCtx, nv12Frame);
        if (ret < 0) {
            char errBuf[128];
            av_make_error_string(errBuf, 128, ret);
            std::cerr << "[Encoder] Failed to send frame to encoder: " << errBuf << "\n";
            return;
        }

        while (avcodec_receive_packet(codecCtx, packet) == 0) {
            packet->stream_index = videoStream->index;
            av_packet_rescale_ts(packet, codecCtx->time_base, videoStream->time_base);

            if (packet->dts < last_dts) {
                std::wcout << L"[DEBUG] Adjusting DTS: " << packet->dts << L" to " << (last_dts + 1) << L"\n";
                packet->dts = last_dts + 1;
            }
            last_dts = packet->dts;

            // Log packet details
            std::wcout << L"[DEBUG] Encoded packet: size=" << packet->size << L", PTS=" << packet->pts
                << L", DTS=" << packet->dts << L", flags=" << packet->flags << L"\n";

            // Parse NAL units in the packet by skipping start codes
            if (packet->size > 0) {
                int offset = 0;
                while (offset < packet->size) {
                    // Look for start code (00 00 01 or 00 00 00 01)
                    if (offset + 2 < packet->size && packet->data[offset] == 0 && packet->data[offset + 1] == 0) {
                        if (offset + 3 < packet->size && packet->data[offset + 2] == 0 && packet->data[offset + 3] == 1) {
                            offset += 4; // Skip 00 00 00 01
                        }
                        else if (packet->data[offset + 2] == 1) {
                            offset += 3; // Skip 00 00 01
                        }
                        else {
                            offset++;
                            continue;
                        }

                        if (offset < packet->size) {
                            uint8_t nal_unit_type = packet->data[offset] & 0x1F;
                            // Calculate NAL unit size by finding the next start code
                            int nal_size = 0;
                            int next_start = offset + 1;
                            while (next_start + 2 < packet->size) {
                                if (packet->data[next_start] == 0 && packet->data[next_start + 1] == 0 &&
                                    (packet->data[next_start + 2] == 1 || (next_start + 3 < packet->size && packet->data[next_start + 2] == 0 && packet->data[next_start + 3] == 1))) {
                                    break;
                                }
                                next_start++;
                            }
                            nal_size = next_start - offset;
                            if (next_start == packet->size) {
                                nal_size = packet->size - offset;
                            }

                            std::wcout << L"[DEBUG] Found NAL unit type: " << static_cast<int>(nal_unit_type)
                                << L", size: " << nal_size << L" bytes\n";
                            offset += nal_size;
                        }
                    }
                    else {
                        offset++;
                    }
                }
            }

            // Save the encoded packet to a file
            static std::ofstream h264File("debug_stream.h264", std::ios::binary | std::ios::app);
            if (h264File.is_open()) {
                h264File.write((char*)packet->data, packet->size);
                h264File.flush(); // Ensure the data is written to disk
            }
            else {
                fprintf(stderr, "Error: Could not open debug_stream.h264 for writing\n");
            }

            // Original WebRTC packet pushing logic
            if (g_onEncodedFrameCallback) {
                g_onEncodedFrameCallback(packet);
            }
            else {
                pushPacketToWebRTC(packet);
            }

            av_packet_unref(packet);
        }
        frameCounter++;
        std::wcout << L"[DEBUG] Frame encoded successfully. Frame Counter: " << frameCounter
            << L", PTS: " << nv12Frame->pts << L"\n";
    }
    catch (const std::exception& e) {
        std::cerr << "[EXCEPTION] EncodeFrame() - Exception caught: " << e.what() << "\n";
    }
    catch (...) {
        std::cerr << "[EXCEPTION] EncodeFrame() - Unknown exception caught!\n";
    }
}

// Keep the FlushEncoder function for completeness
void FlushEncoder() {
    std::wcout << L"[DEBUG] Flushing encoder\n";
    int ret = avcodec_send_frame(codecCtx, nullptr); // Send NULL to flush
    if (ret < 0) {
        char errBuf[128];
        av_make_error_string(errBuf, 128, ret);
        std::cerr << "[Encoder] Failed to flush encoder: " << errBuf << "\n";
        return;
    }

    while (avcodec_receive_packet(codecCtx, packet) == 0) {
        packet->stream_index = videoStream->index;
        av_packet_rescale_ts(packet, codecCtx->time_base, videoStream->time_base);

        if (packet->dts < last_dts) {
            packet->dts = last_dts + 1;
        }
        last_dts = packet->dts;

        // Log packet details during flush
        std::wcout << L"[DEBUG] Flushing packet: size=" << packet->size << L", PTS=" << packet->pts
            << L", DTS=" << packet->dts << L", flags=" << packet->flags << L"\n";

        // Parse NAL units during flush
        if (packet->size > 0) {
            int offset = 0;
            while (offset < packet->size) {
                if (offset + 2 < packet->size && packet->data[offset] == 0 && packet->data[offset + 1] == 0) {
                    if (offset + 3 < packet->size && packet->data[offset + 2] == 0 && packet->data[offset + 3] == 1) {
                        offset += 4;
                    }
                    else if (packet->data[offset + 2] == 1) {
                        offset += 3;
                    }
                    else {
                        offset++;
                        continue;
                    }

                    if (offset < packet->size) {
                        uint8_t nal_unit_type = packet->data[offset] & 0x1F;
                        int nal_size = 0;
                        int next_start = offset + 1;
                        while (next_start + 2 < packet->size) {
                            if (packet->data[next_start] == 0 && packet->data[next_start + 1] == 0 &&
                                (packet->data[next_start + 2] == 1 || (next_start + 3 < packet->size && packet->data[next_start + 2] == 0 && packet->data[next_start + 3] == 1))) {
                                break;
                            }
                            next_start++;
                        }
                        nal_size = next_start - offset;
                        if (next_start == packet->size) {
                            nal_size = packet->size - offset;
                        }

                        std::wcout << L"[DEBUG] Flushing NAL unit type: " << static_cast<int>(nal_unit_type)
                            << L", size: " << nal_size << L" bytes\n";
                        offset += nal_size;
                    }
                }
                else {
                    offset++;
                }
            }
        }

        // Write flushed packets to file
        static std::ofstream h264File("debug_stream.h264", std::ios::binary | std::ios::app);
        if (h264File.is_open()) {
            h264File.write((char*)packet->data, packet->size);
            h264File.flush();
        }
        else {
            fprintf(stderr, "Error: Could not open debug_stream.h264 for writing during flush\n");
        }

        av_packet_unref(packet);
    }
    std::wcout << L"[DEBUG] Encoder flush complete\n";
}

    //void ConvertFrame(const uint8_t* bgraData, int bgraPitch, int width, int height) {
    //    std::lock_guard<std::mutex> lock(g_encoderMutex);
    //    std::wcout << L"[CONV_DEBUG] ConvertFrame() - Start\n";

    //    // Verify input data
    //    if (!bgraData) {
    //        std::wcerr << L"[CONV_DEBUG] ConvertFrame() - Error: bgraData is NULL\n";
    //        return;
    //    }
    //    std::wcout << L"[CONV_DEBUG] ConvertFrame() - Input: width=" << width << L", height=" << height << L", pitch=" << bgraPitch << L"\n";
    //    std::wcout << L"[CONV_DEBUG] ConvertFrame() - First 16 bytes of bgraData: ";
    //    for (int i = 0; i < 16 && i < bgraPitch * height; i++) {
    //        std::wcout << std::hex << (int)bgraData[i] << L" ";
    //    }
    //    std::wcout << std::dec << L"\n";

    //    if (!swsCtx || width != currentWidth || height != currentHeight) {
    //        std::wcout << L"[CONV_DEBUG] ConvertFrame() - Recreating SWS context\n";

    //        if (swsCtx) {
    //            sws_freeContext(swsCtx);
    //            swsCtx = nullptr;
    //        }
    //        if (nv12Frame) {
    //            av_frame_free(&nv12Frame);
    //        }

    //        swsCtx = sws_getContext(
    //            width, height, AV_PIX_FMT_BGRA,
    //            1920, 1080, AV_PIX_FMT_NV12,
    //            SWS_BICUBIC, nullptr, nullptr, nullptr
    //        );
    //        if (!swsCtx) {
    //            std::cerr << "[Encoder] Failed to create swsCtx.\n";
    //            return;
    //        }

    //        nv12Frame = av_frame_alloc();
    //        nv12Frame->format = AV_PIX_FMT_NV12;
    //        nv12Frame->width = 1920;
    //        nv12Frame->height = 1080;

    //        int ret = av_frame_get_buffer(nv12Frame, 32);
    //        if (ret < 0) {
    //            std::cerr << "[Encoder] Failed to allocate nv12Frame buffer.\n";
    //            return;
    //        }

    //        currentWidth = width;
    //        currentHeight = height;
    //        std::wcout << L"[CONV_DEBUG] ConvertFrame() - Performing sws_scale\n";
    //    }

    //    uint8_t* inData[4] = { const_cast<uint8_t*>(bgraData), nullptr, nullptr, nullptr };
    //    int inLineSize[4] = { bgraPitch, 0, 0, 0 };

    //    if (av_frame_make_writable(nv12Frame) < 0) {
    //        std::cerr << "[Encoder] Failed to make nv12Frame writable.\n";
    //        return;
    //    }

    //    sws_scale(
    //        swsCtx,
    //        inData,
    //        inLineSize,
    //        0,
    //        height,
    //        nv12Frame->data,
    //        nv12Frame->linesize
    //    );

    //    // Log the converted NV12 frame data
    //    std::wcout << L"[CONV_DEBUG] ConvertFrame() - NV12 frame: linesize[0]=" << nv12Frame->linesize[0] << L", linesize[1]=" << nv12Frame->linesize[1] << L"\n";
    //    std::wcout << L"[CONV_DEBUG] ConvertFrame() - First 16 bytes of NV12 luma: ";
    //    for (int i = 0; i < 16 && i < nv12Frame->linesize[0]; i++) {
    //        std::wcout << std::hex << (int)nv12Frame->data[0][i] << L" ";
    //    }
    //    std::wcout << std::dec << L"\n";

    //    std::wcout << L"[CONV_DEBUG] ConvertFrame() - Frame converted successfully\n";

    //    EncodeFrame();
    //    std::wcout << L"[CONV_DEBUG] ConvertFrame() - End\n";
    //    std::cout << "[Encoder] Converted frame to NV12 ("
    //        << width << "x" << height << ")\n";
    //}

//void ConvertFrame(const uint8_t* bgraData, int bgraPitch, int width, int height) {
//    std::lock_guard<std::mutex> lock(g_encoderMutex);
//    std::wcout << L"[CONV_DEBUG] ConvertFrame() - Start\n";
//
//    // Verify input data
//    if (!bgraData) {
//        std::wcerr << L"[CONV_DEBUG] ConvertFrame() - Error: bgraData is NULL\n";
//        return;
//    }
//    std::wcout << L"[CONV_DEBUG] ConvertFrame() - Input: width=" << width << L", height=" << height << L", pitch=" << bgraPitch << L"\n";
//    std::wcout << L"[CONV_DEBUG] ConvertFrame() - First 16 bytes of bgraData: ";
//    for (int i = 0; i < 16 && i < bgraPitch * height; i++) {
//        std::wcout << std::hex << (int)bgraData[i] << L" ";
//    }
//    std::wcout << std::dec << L"\n";
//
//    if (!swsCtx || width != currentWidth || height != currentHeight) {
//        std::wcout << L"[CONV_DEBUG] ConvertFrame() - Recreating SWS context\n";
//
//        if (swsCtx) {
//            sws_freeContext(swsCtx);
//            swsCtx = nullptr;
//        }
//        if (nv12Frame) {
//            av_frame_free(&nv12Frame);
//        }
//
//        swsCtx = sws_getContext(
//            width, height, AV_PIX_FMT_BGRA,
//            1920, 1080, AV_PIX_FMT_YUV420P,
//            SWS_BICUBIC, nullptr, nullptr, nullptr
//        );
//        if (!swsCtx) {
//            std::cerr << "[Encoder] Failed to create swsCtx.\n";
//            return;
//        }
//
//        nv12Frame = av_frame_alloc();
//        nv12Frame->format = AV_PIX_FMT_YUV420P;
//        nv12Frame->width = 1920;
//        nv12Frame->height = 1080;
//
//        // Allocate the frame buffer with correct strides
//        int ret = av_frame_get_buffer(nv12Frame, 32); // 32-byte alignment
//        if (ret < 0) {
//            std::cerr << "[Encoder] Failed to allocate nv12Frame buffer.\n";
//            return;
//        }
//
//        // Ensure strides are correct for YUV420P
//        nv12Frame->linesize[0] = 1920; // Luma stride
//        nv12Frame->linesize[1] = 960;  // U plane stride
//        nv12Frame->linesize[2] = 960;  // V plane stride
//
//        currentWidth = width;
//        currentHeight = height;
//        std::wcout << L"[CONV_DEBUG] ConvertFrame() - Performing sws_scale\n";
//    }
//
//    uint8_t* inData[4] = { const_cast<uint8_t*>(bgraData), nullptr, nullptr, nullptr };
//    int inLineSize[4] = { bgraPitch, 0, 0, 0 };
//
//    if (av_frame_make_writable(nv12Frame) < 0) {
//        std::cerr << "[Encoder] Failed to make nv12Frame writable.\n";
//        return;
//    }
//
//    std::ofstream bgraOut("debug_bgra.raw", std::ios::binary | std::ios::app);
//    bgraOut.write(reinterpret_cast<const char*>(bgraData), bgraPitch * height);
//    bgraOut.close();
//    std::wcout << L"[DEBUG] Saved BGRA input to debug_bgra.raw\n";
//
//    sws_scale(
//        swsCtx,
//        inData,
//        inLineSize,
//        0,
//        height,
//        nv12Frame->data,
//        nv12Frame->linesize
//    );
//
//    // Verify the strides after sws_scale
//    nv12Frame->linesize[0] = 1920; // Luma stride
//    nv12Frame->linesize[1] = 960;  // U plane stride
//    nv12Frame->linesize[2] = 960;  // V plane stride
//
//    // Log the converted YUV420P frame data
//    std::wcout << L"[CONV_DEBUG] ConvertFrame() - YUV420P frame: linesize[0]=" << nv12Frame->linesize[0]
//        << L", linesize[1]=" << nv12Frame->linesize[1]
//        << L", linesize[2]=" << nv12Frame->linesize[2] << L"\n";
//    std::wcout << L"[CONV_DEBUG] ConvertFrame() - First 16 bytes of YUV420P luma: ";
//    for (int i = 0; i < 16 && i < nv12Frame->linesize[0]; i++) {
//        std::wcout << std::hex << (int)nv12Frame->data[0][i] << L" ";
//    }
//    std::wcout << std::dec << L"\n";
//
//    std::wcout << L"[CONV_DEBUG] ConvertFrame() - Frame converted successfully\n";
//
//    EncodeFrame();
//    std::wcout << L"[CONV_DEBUG] ConvertFrame() - End\n";
//    std::cout << "[Encoder] Converted frame to YUV420P ("
//        << width << "x" << height << ")\n";
//}

void ConvertFrame(const uint8_t* bgraData, int bgraPitch, int width, int height) {
    std::lock_guard<std::mutex> lock(g_encoderMutex);
    std::wcout << L"[CONV_DEBUG] width=" << width << L", height=" << height
        << L", bgraPitch=" << bgraPitch << L", expected stride=" << (width * 4) << L"\n";

    if (!bgraData) {
        std::wcerr << L"[CONV_DEBUG] bgraData is NULL\n";
        return;
    }

    if (!swsCtx || width != currentWidth || height != currentHeight) {
        if (swsCtx) sws_freeContext(swsCtx);
        if (nv12Frame) av_frame_free(&nv12Frame);

        swsCtx = sws_getContext(width, height, AV_PIX_FMT_BGRA, 1920, 1080, AV_PIX_FMT_YUV420P,
            SWS_BICUBIC, nullptr, nullptr, nullptr);
        if (!swsCtx) {
            std::cerr << "[Encoder] Failed to create swsCtx\n";
            return;
        }

        nv12Frame = av_frame_alloc();
        nv12Frame->format = AV_PIX_FMT_YUV420P;
        nv12Frame->width = 1920;
        nv12Frame->height = 1080;
        if (av_frame_get_buffer(nv12Frame, 32) < 0) {
            std::cerr << "[Encoder] Failed to allocate nv12Frame buffer\n";
            return;
        }

        currentWidth = width;
        currentHeight = height;
    }

    // Use width * 4 as stride if pitch includes padding
    //int effectiveStride = (bgraPitch > width * 4) ? width * 4 : bgraPitch;
    uint8_t* inData[4] = { const_cast<uint8_t*>(bgraData), nullptr, nullptr, nullptr };
    int inLineSize[4] = { bgraPitch, 0, 0, 0 };
    if (av_frame_make_writable(nv12Frame) < 0) {
        std::cerr << "[Encoder] Failed to make nv12Frame writable\n";
        return;
    }

    sws_scale(swsCtx, inData, inLineSize, 0, height, nv12Frame->data, nv12Frame->linesize);

    // Debug YUV output
    std::ofstream yuvOut("debug_yuv420p.raw", std::ios::binary);
    for (int i = 0; i < 1080; i++) yuvOut.write(reinterpret_cast<char*>(nv12Frame->data[0] + i * nv12Frame->linesize[0]), 1920);
    for (int i = 0; i < 540; i++) yuvOut.write(reinterpret_cast<char*>(nv12Frame->data[1] + i * nv12Frame->linesize[1]), 960);
    for (int i = 0; i < 540; i++) yuvOut.write(reinterpret_cast<char*>(nv12Frame->data[2] + i * nv12Frame->linesize[2]), 960);
    yuvOut.close();

    std::ofstream bgraFile("debug_bgra.raw", std::ios::binary);
    bgraFile.write((char*)bgraData, 1920 * 1080 * 4); // 8,294,400 bytes
    bgraFile.close();

    EncodeFrame();
}

    void FinalizeEncoder() {
        std::lock_guard<std::mutex> lock(g_encoderMutex);
        std::wcout << L"[DEBUG] Finalizing encoder...\n";

        avcodec_send_frame(codecCtx, nullptr);
        while (avcodec_receive_packet(codecCtx, packet) == 0) {
            packet->stream_index = videoStream->index;
            av_packet_rescale_ts(packet, codecCtx->time_base, videoStream->time_base);

            if (packet->dts < packet->pts) {
                packet->dts = packet->pts;
            }

            av_interleaved_write_frame(formatCtx, packet);
            av_packet_unref(packet);
        }

        avcodec_free_context(&codecCtx);
        avformat_free_context(formatCtx);
        av_packet_free(&packet);
        av_buffer_unref(&hwDeviceCtx);

        // Close debug file
        if (debugH264File.is_open()) {
            debugH264File.close();
        }

        std::wcout << L"[Encoder] Encoder finalized.\n";
    }
}
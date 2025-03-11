#pragma once
extern "C" {
	#include <libavcodec/avcodec.h>
	#include <libavformat/avformat.h>
	#include <libswscale/swscale.h>
	#include <libavutil/imgutils.h>
}
#include <cstdint>
#include <iostream>
#include <mutex>

namespace Encoder {
	void ConvertFrame(
		const uint8_t* bgraData,
		int bgraPitch,
		int	width,
		int height
	);
	void InitializeEncoder(const std::string& fileName, int width, int height, int fps);
	void EncodeFrame(AVFrame* nv12Frame);
	void FinalizeEncoder();

}

/* Copyright 2016 Carnegie Mellon University
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include "scanner/video/video_decoder.h"

extern "C" {
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libavformat/avio.h"
#include "libavfilter/avfilter.h"
#include "libswscale/swscale.h"
#include "libavutil/pixdesc.h"
#include "libavutil/error.h"
#include "libavutil/opt.h"
}

#include <vector>
#include <deque>

namespace scanner {

///////////////////////////////////////////////////////////////////////////////
/// SoftwareVideoDecoder
class SoftwareVideoDecoder : public VideoDecoder {
public:
  SoftwareVideoDecoder(int device_id);

  ~SoftwareVideoDecoder();

  void configure(const DatasetItemMetadata& metadata) override;

  bool feed(
    const u8* encoded_buffer,
    size_t encoded_size,
    bool discontinuity = false) override;

  bool discard_frame() override;

  bool get_frame(
    u8* decoded_buffer,
    size_t decoded_size) override;

  int decoded_frames_buffered() override;

  void wait_until_frames_copied() override;

private:
  int device_id_;
  AVPacket packet_;
  AVCodec* codec_;
  AVCodecContext* cc_;

  DatasetItemMetadata metadata_;
  bool reset_context_;
  SwsContext* sws_context_;

  std::vector<AVFrame*> frame_pool_;
  std::deque<AVFrame*> decoded_frame_queue_;
};

}

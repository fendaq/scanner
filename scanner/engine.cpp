/* Copyright 2016 Carnegie Mellon University, NVIDIA Corporation
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

#include "scanner/engine.h"

#include "scanner/evaluators/video/decoder_evaluator.h"

#include "jpegwrapper/JPEGWriter.h"
#include "scanner/util/common.h"
#include "scanner/util/memory.h"
#include "scanner/util/profiler.h"
#include "scanner/util/queue.h"
#include "scanner/util/storehouse.h"
#include "scanner/util/util.h"

#include "storehouse/storage_backend.h"

#include <glog/logging.h>

#include <libgen.h>
#include <mpi.h>
#include <pthread.h>
#include <atomic>
#include <cstdlib>
#include <string>
#include <thread>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#ifdef HAVE_CUDA
#include <cuda.h>
#include <cuda_runtime_api.h>
#include "scanner/util/cuda.h"
#endif

using storehouse::StoreResult;
using storehouse::WriteFile;
using storehouse::RandomReadFile;
using storehouse::exit_on_error;

namespace scanner {
///////////////////////////////////////////////////////////////////////////////
/// Work structs - structs used to exchange data between workers during
///   execution of the run command.
struct VideoWorkItem {
  i32 video_index;
  i64 item_id;
  i64 next_item_id;
  i32 rows_from_start;
};

struct LoadWorkEntry {
  i32 work_item_index;
  // union {
  // For no sampling
  Interval interval;
  // For stride
  struct {
    Interval interval;
    i32 stride;
  } strided;
  // For gather
  std::vector<i32> gather_points;
  // For sequence gather
  std::vector<Interval> gather_sequences;
  //};
};

struct EvalWorkEntry {
  i32 work_item_index;
  std::vector<std::string> column_names;
  std::vector<std::vector<size_t>> buffer_sizes;
  std::vector<std::vector<u8*>> buffers;
  DeviceType buffer_type;
  i32 buffer_device_id;
  bool video_decode_item;
};

///////////////////////////////////////////////////////////////////////////////
/// Worker thread arguments
struct LoadThreadArgs {
  // Uniform arguments
  std::string dataset_name;
  Sampling sampling;
  i32 warmup_count;
  const std::vector<std::string>& video_paths;
  const std::vector<VideoMetadata>& metadata;
  const std::vector<VideoWorkItem>& work_items;

  // Per worker arguments
  int id;
  storehouse::StorageConfig* storage_config;
  Profiler& profiler;

  // Queues for communicating work
  Queue<LoadWorkEntry>& load_work;  // in
  Queue<EvalWorkEntry>& eval_work;  // out
};

struct EvaluateThreadArgs {
  // Uniform arguments
  i32 warmup_count;
  const std::vector<VideoMetadata>& metadata;
  const std::vector<VideoWorkItem>& work_items;

  // Per worker arguments
  int id;
  int evaluator_group;
  bool last_evaluator_group;
  std::vector<EvaluatorFactory*> evaluator_factories;
  std::vector<EvaluatorConfig> evaluator_configs;
  Profiler& profiler;

  // Queues for communicating work
  Queue<EvalWorkEntry>& input_work;
  Queue<EvalWorkEntry>& output_work;
};

struct SaveThreadArgs {
  // Uniform arguments
  std::string job_name;
  const std::vector<std::string>& video_paths;
  const std::vector<VideoMetadata>& metadata;
  const std::vector<VideoWorkItem>& work_items;
  std::vector<std::string> output_names;

  // Per worker arguments
  int id;
  storehouse::StorageConfig* storage_config;
  Profiler& profiler;

  // Queues for communicating work
  Queue<EvalWorkEntry>& input_work;
  std::atomic<i64>& retired_items;
};

///////////////////////////////////////////////////////////////////////////////
/// Thread to asynchronously load video
std::tuple<size_t, size_t> find_keyframe_indices(
    i32 start_frame, i32 end_frame,
    const std::vector<i64>& keyframe_positions) {
  size_t start_keyframe_index = std::numeric_limits<size_t>::max();
  for (size_t i = 1; i < keyframe_positions.size(); ++i) {
    if (keyframe_positions[i] > start_frame) {
      start_keyframe_index = i - 1;
      break;
    }
  }
  assert(start_keyframe_index != std::numeric_limits<size_t>::max());

  size_t end_keyframe_index = 0;
  for (size_t i = start_keyframe_index; i < keyframe_positions.size(); ++i) {
    if (keyframe_positions[i] >= end_frame) {
      end_keyframe_index = i;
      break;
    }
  }
  assert(end_keyframe_index != 0);
  return std::make_tuple(start_keyframe_index, end_keyframe_index);
}

void* load_video_thread(void* arg) {
  LoadThreadArgs& args = *reinterpret_cast<LoadThreadArgs*>(arg);

  auto setup_start = now();

  i32 rank;
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);

  // Setup a distinct storage backend for each IO thread
  storehouse::StorageBackend* storage =
      storehouse::StorageBackend::make_from_config(args.storage_config);

  std::string last_video_path;
  RandomReadFile* video_file = nullptr;
  u64 file_size;

  args.profiler.add_interval("setup", setup_start, now());

  std::vector<i64> keyframe_positions;
  std::vector<i64> keyframe_byte_offsets;
  while (true) {
    auto idle_start = now();

    LoadWorkEntry load_work_entry;
    args.load_work.pop(load_work_entry);

    if (load_work_entry.work_item_index == -1) {
      break;
    }

    LOG(INFO) << "Load (N/PU: " << rank << "/" << args.id
              << "): processing item " << load_work_entry.work_item_index;

    args.profiler.add_interval("idle", idle_start, now());

    auto work_start = now();

    const VideoWorkItem& work_item =
        args.work_items[load_work_entry.work_item_index];

    const std::string& video_path = args.video_paths[work_item.video_index];
    const VideoMetadata& metadata = args.metadata[work_item.video_index];

    if (video_path != last_video_path) {
      if (video_file != nullptr) {
        delete video_file;
        video_file = nullptr;
      }

      // Open the video file for reading
      storage->make_random_read_file(
          dataset_item_data_path(args.dataset_name, video_path), video_file);

      video_file->get_size(file_size);

      keyframe_positions = metadata.keyframe_positions();
      keyframe_byte_offsets = metadata.keyframe_byte_offsets();
      // Place end of file and num frame at end of iframe to handle edge case
      keyframe_positions.push_back(metadata.frames());
      keyframe_byte_offsets.push_back(file_size);
    }
    last_video_path = video_path;

    // Read the bytes from the file that correspond to the sequences
    // of frames we are interested in decoding. This sequence will contain
    // the bytes starting at the iframe at or preceding the first frame we are
    // interested and will continue up to the bytes before the iframe at or
    // after the last frame we are interested in.
    EvalWorkEntry eval_work_entry;
    eval_work_entry.work_item_index = load_work_entry.work_item_index;
    eval_work_entry.column_names = {"video", "video_args"};
    eval_work_entry.buffer_sizes.resize(2);
    eval_work_entry.buffers.resize(2);
    eval_work_entry.buffer_type = DeviceType::CPU;
    eval_work_entry.buffer_device_id = 0;
    eval_work_entry.video_decode_item = true;

    std::vector<Interval> intervals;
    if (args.sampling == Sampling::All) {
      intervals.push_back(Interval{load_work_entry.interval.start,
                                   load_work_entry.interval.end});

      // Video decode arguments
      u8* decode_args_buffer = new u8[sizeof(DecoderEvaluator::DecodeArgs)];
      // HACK(apoms): These placement new's are a memory leak
      DecoderEvaluator::DecodeArgs* decode_args =
          new (decode_args_buffer) DecoderEvaluator::DecodeArgs();
      decode_args->warmup_count = args.warmup_count;
      decode_args->sampling = Sampling::All;
      decode_args->interval = load_work_entry.interval;

      eval_work_entry.buffers[1].push_back(decode_args_buffer);
      eval_work_entry.buffer_sizes[1].push_back(sizeof(*decode_args));

    } else if (args.sampling == Sampling::Strided) {
      // TODO(apoms): loading a consecutive portion of the video stream might
      //   be inefficient if the stride is much larger than a single GOP.
      intervals.push_back(Interval{load_work_entry.strided.interval.start,
                                   load_work_entry.strided.interval.end});

      // Video decode arguments
      u8* decode_args_buffer = new u8[sizeof(DecoderEvaluator::DecodeArgs)];
      DecoderEvaluator::DecodeArgs* decode_args =
          new (decode_args_buffer) DecoderEvaluator::DecodeArgs();
      decode_args->warmup_count = args.warmup_count;
      decode_args->sampling = Sampling::Strided;
      decode_args->strided.interval = load_work_entry.strided.interval;
      decode_args->strided.stride = load_work_entry.strided.stride;

      eval_work_entry.buffers[1].push_back(decode_args_buffer);
      eval_work_entry.buffer_sizes[1].push_back(sizeof(*decode_args));

    } else if (args.sampling == Sampling::Gather) {
      // TODO(apoms): This implementation is not efficient for gathers which
      //   overlap in the same GOP.
      for (size_t i = 0; i < load_work_entry.gather_points.size(); ++i) {
        intervals.push_back(Interval{load_work_entry.gather_points[i],
                                     load_work_entry.gather_points[i] + 1});

        // Video decode arguments
        u8* decode_args_buffer = new u8[sizeof(DecoderEvaluator::DecodeArgs)];
        DecoderEvaluator::DecodeArgs* decode_args =
            new (decode_args_buffer) DecoderEvaluator::DecodeArgs();
        decode_args->warmup_count = args.warmup_count;
        decode_args->sampling = Sampling::Gather;
        decode_args->gather_points.push_back(load_work_entry.gather_points[i]);

        eval_work_entry.buffers[1].push_back(decode_args_buffer);
        eval_work_entry.buffer_sizes[1].push_back(sizeof(*decode_args));
      }
    } else if (args.sampling == Sampling::SequenceGather) {
      intervals = load_work_entry.gather_sequences;

      for (size_t i = 0; i < load_work_entry.gather_sequences.size(); ++i) {
        // Video decode arguments
        u8* decode_args_buffer = new u8[sizeof(DecoderEvaluator::DecodeArgs)];
        DecoderEvaluator::DecodeArgs* decode_args =
            new (decode_args_buffer) DecoderEvaluator::DecodeArgs();
        decode_args->warmup_count = args.warmup_count;
        decode_args->sampling = Sampling::SequenceGather;
        decode_args->gather_sequences.push_back(
            load_work_entry.gather_sequences[i]);

        eval_work_entry.buffers[1].push_back(decode_args_buffer);
        eval_work_entry.buffer_sizes[1].push_back(sizeof(*decode_args));
      }
    }

    size_t num_intervals = intervals.size();
    for (size_t i = 0; i < num_intervals; ++i) {
      i32 start_frame = intervals[i].start;
      i32 end_frame = intervals[i].end;
      size_t start_keyframe_index;
      size_t end_keyframe_index;
      std::tie(start_keyframe_index, end_keyframe_index) =
          find_keyframe_indices(start_frame, end_frame, keyframe_positions);

      u64 start_keyframe_byte_offset =
          static_cast<u64>(keyframe_byte_offsets[start_keyframe_index]);

      u64 end_keyframe_byte_offset =
          static_cast<u64>(keyframe_byte_offsets[end_keyframe_index]);

      size_t buffer_size =
          end_keyframe_byte_offset - start_keyframe_byte_offset;

      u8* buffer = new u8[buffer_size];

      auto io_start = now();

      u64 pos = start_keyframe_byte_offset;
      read(video_file, buffer, buffer_size, pos);

      args.profiler.add_interval("io", io_start, now());

      // Encoded buffer
      eval_work_entry.buffers[0].push_back(buffer);
      eval_work_entry.buffer_sizes[0].push_back(buffer_size);

      DecoderEvaluator::DecodeArgs* decode_args =
          reinterpret_cast<DecoderEvaluator::DecodeArgs*>(
              eval_work_entry.buffers[1][i]);
      decode_args->start_keyframe = keyframe_positions[start_keyframe_index];
      decode_args->end_keyframe = keyframe_positions[end_keyframe_index];
    }
    assert(eval_work_entry.buffers[0].size() ==
           eval_work_entry.buffers[1].size());
    assert(eval_work_entry.buffer_sizes[0].size() ==
           eval_work_entry.buffer_sizes[1].size());

    args.profiler.add_interval("task", work_start, now());

    args.eval_work.push(eval_work_entry);
  }

  LOG(INFO) << "Load (N/PU: " << rank << "/" << args.id << "): thread finished";

  // Cleanup
  if (video_file != nullptr) {
    delete video_file;
  }
  delete storage;

  THREAD_RETURN_SUCCESS();
}

///////////////////////////////////////////////////////////////////////////////
/// Thread to run evaluation
void* evaluate_thread(void* arg) {
  EvaluateThreadArgs& args = *reinterpret_cast<EvaluateThreadArgs*>(arg);

  auto setup_start = now();

  i32 rank;
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);

  assert(args.evaluator_factories.size() == args.evaluator_configs.size());
  std::vector<EvaluatorCapabilities> evaluator_caps;
  std::vector<std::unique_ptr<Evaluator>> evaluators;
  std::vector<i32> num_evaluator_outputs;
  for (size_t i = 0; i < args.evaluator_factories.size(); ++i) {
    EvaluatorFactory* factory = args.evaluator_factories[i];
    const EvaluatorConfig& config = args.evaluator_configs[i];
    evaluator_caps.push_back(factory->get_capabilities());
    evaluators.emplace_back(factory->new_evaluator(config));
    num_evaluator_outputs.push_back(factory->get_output_names().size());
  }
  assert(evaluators.size() > 0);

  for (auto& evaluator : evaluators) {
    evaluator->set_profiler(&args.profiler);
  }

  i32 last_evaluator_num_columns =
      args.evaluator_factories.back()->get_output_names().size();
  i32 last_evaluator_device_id = args.evaluator_configs.back().device_ids[0];
  DeviceType last_evaluator_device_type = evaluator_caps.back().device_type;

  args.profiler.add_interval("setup", setup_start, now());

  int last_video_index = -1;
  int last_next_item_id = -1;
  while (true) {
    auto idle_start = now();
    // Wait for next work item to process
    EvalWorkEntry work_entry;
    args.input_work.pop(work_entry);

    if (work_entry.work_item_index == -1) {
      break;
    }

    LOG(INFO) << "Evaluate (N/PU/G: " << rank << "/" << args.id << "/"
              << args.evaluator_group << "): processing item "
              << work_entry.work_item_index;

    args.profiler.add_interval("idle", idle_start, now());

    auto work_start = now();

    const VideoWorkItem& work_item =
        args.work_items[work_entry.work_item_index];
    const VideoMetadata& metadata = args.metadata[work_item.video_index];

    bool needs_configure = !(work_item.video_index == last_video_index);
    bool needs_reset = (!(work_item.video_index == last_video_index &&
                          work_item.item_id == last_next_item_id));
    for (auto& evaluator : evaluators) {
      // Make the evaluator aware of the format of the data we are about to
      // feed it
      if (needs_configure) {
        evaluator->configure(metadata);
      }
      if (needs_reset) {
        evaluator->reset();
      }
    }
    last_video_index = work_item.video_index;
    last_next_item_id = work_item.next_item_id;

    size_t frame_size = metadata.width() * metadata.height() * 3 * sizeof(u8);

    EvalWorkEntry output_work_entry;
    output_work_entry.work_item_index = work_entry.work_item_index;
    output_work_entry.buffer_type = DeviceType::CPU;
    output_work_entry.buffer_device_id = 0;
    output_work_entry.video_decode_item = false;

    std::vector<std::vector<size_t>>& work_item_output_sizes =
        output_work_entry.buffer_sizes;
    std::vector<std::vector<u8*>>& work_item_output_buffers =
        output_work_entry.buffers;
    work_item_output_sizes.resize(last_evaluator_num_columns);
    work_item_output_buffers.resize(last_evaluator_num_columns);

    i32 current_input = 0;
    i32 total_inputs =
        work_entry.buffers.empty() ? 0 : work_entry.buffers[0].size();
    while (current_input < total_inputs) {
      i32 batch_size = std::min(total_inputs, WORK_ITEM_SIZE);

      std::vector<std::string> input_names;
      std::vector<std::vector<u8*>> input_buffers;
      std::vector<std::vector<size_t>> input_sizes;
      DeviceType input_buffer_type;
      i32 input_device_id;
      // Initialize the output buffers with the frame input because we
      // perform a swap from output to input on each iterator to pass outputs
      // from the previous evaluator into the input of the next one
      std::vector<std::string> output_names = work_entry.column_names;
      std::vector<std::vector<u8*>> output_buffers(work_entry.buffers.size());
      for (size_t i = 0; i < work_entry.buffers.size(); ++i) {
        output_buffers[i].insert(
            output_buffers[i].end(),
            work_entry.buffers[i].begin() + current_input,
            work_entry.buffers[i].begin() + current_input + batch_size);
      }
      std::vector<std::vector<size_t>> output_sizes(
          work_entry.buffer_sizes.size());
      for (size_t i = 0; i < work_entry.buffer_sizes.size(); ++i) {
        output_sizes[i].insert(
            output_sizes[i].end(),
            work_entry.buffer_sizes[i].begin() + current_input,
            work_entry.buffer_sizes[i].begin() + current_input + batch_size);
      }
      DeviceType output_buffer_type = work_entry.buffer_type;
      i32 output_device_id = work_entry.buffer_device_id;

      for (size_t e = 0; e < evaluators.size(); ++e) {
        i32 device_id = args.evaluator_configs[e].device_ids[0];
        EvaluatorCapabilities& caps = evaluator_caps[e];
        std::unique_ptr<Evaluator>& evaluator = evaluators[e];
        i32 num_outputs = num_evaluator_outputs[e];

        input_names.swap(output_names);
        input_buffers.swap(output_buffers);
        input_sizes.swap(output_sizes);
        input_buffer_type = output_buffer_type;
        input_device_id = output_device_id;

        i32 num_inputs = input_buffers.size();
        // If current evaluator type and input buffer type differ, then move
        // the data in the input buffer into a new buffer which has the same
        // type as the evaluator input
        if (input_buffer_type != caps.device_type ||
            input_device_id != device_id) {
          for (i32 i = 0; i < num_inputs; ++i) {
            std::vector<u8*>& buffers = input_buffers[i];
            std::vector<size_t>& sizes = input_sizes[i];
            for (i32 b = 0; b < (i32)buffers.size(); ++b) {
              size_t size = sizes[b];
              u8* buffer = new_buffer(caps.device_type, device_id, size);
              memcpy_buffer(buffer, caps.device_type, device_id, buffers[b],
                            input_buffer_type, input_device_id, size);
              delete_buffer(input_buffer_type, input_device_id, buffers[b]);
              buffers[b] = buffer;
            }
          }
          input_buffer_type = caps.device_type;
          input_device_id = device_id;
        }

        // Setup output buffers to receive evaluator output
        output_buffers.clear();
        output_sizes.clear();
        output_buffer_type = caps.device_type;
        output_device_id = device_id;
        output_buffers.resize(num_outputs);
        output_sizes.resize(num_outputs);
        output_names = args.evaluator_factories[e]->get_output_names();

        evaluator->evaluate(input_buffers, input_sizes, output_buffers,
                            output_sizes);
        LOG_IF(FATAL, output_buffers.size() != output_sizes.size())
            << "Evaluator " << e << " produced " << output_buffers.size() << " "
            << "output buffers but " << output_sizes.size() << " output sizes. "
            << "These should be equal.";
        // Do not verify outputs == inputs if we are decoding encoded video as
        // there is an increase of 1 encoded chunk to multiple frames
        if (!(e == 0 && work_entry.video_decode_item)) {
          for (size_t i = 0; i < output_buffers.size(); ++i) {
            LOG_IF(FATAL, output_buffers[i].size() != batch_size)
                << "Evaluator " << e << " produced " << output_buffers[i].size()
                << " output buffers for column " << output_names[i]
                << ". Expected " << batch_size << " outputs.";
            LOG_IF(FATAL, output_sizes[i].size() != batch_size)
                << "Evaluator " << e << " produced " << output_sizes[i].size()
                << "output sizes for column " << output_names[i]
                << ". Expected " << batch_size << " outputs.";
          }
        }
        // HACK(apoms): Handle the case where the video decode evaluator gets a
        //   single input but produces multiple outputs. Should be removed if we
        //   add flatmap esque increases in output element count
        if (e == 0 && work_entry.video_decode_item) {
          batch_size = output_sizes[0].size();
        }

        // Delete input buffers after they are used
        for (size_t i = 0; i < num_inputs; ++i) {
          std::vector<u8*>& buffers = input_buffers[i];
          for (u8* buff : buffers) {
            delete_buffer(input_buffer_type, input_device_id, buff);
          }
        }
      }
      // Only discard warmup frames for last evaluator group because otherwise
      // they need to be forwarded to warm up later evaluator groups
      i32 warmup_frames;
      if (args.last_evaluator_group && needs_reset) {
        i32 total_warmup_frames =
            std::min(args.warmup_count, work_item.rows_from_start);
        warmup_frames = std::min(
            batch_size, std::max(0, total_warmup_frames - current_input));
      } else {
        warmup_frames = 0;
      }
      for (i32 i = 0; i < last_evaluator_num_columns; ++i) {
        assert(output_sizes[i].size() == output_buffers[i].size());

        // Delete warmup frame outputs
        for (i32 w = 0; w < warmup_frames; ++w) {
          delete_buffer(last_evaluator_device_type, last_evaluator_device_id,
                        output_buffers[i][w]);
        }

        // Make sure all outputs are in CPU memory so downstream code does not
        // need to condition on buffer type
        if (output_buffer_type != DeviceType::CPU) {
          for (i32 f = warmup_frames; f < (i32)batch_size; ++f) {
            size_t size = output_sizes[i][f];
            u8* src_buffer = output_buffers[i][f];
            u8* dest_buffer = new_buffer(DeviceType::CPU, 0, size);
            memcpy_buffer(dest_buffer, DeviceType::CPU, 0, src_buffer,
                          output_buffer_type, output_device_id, size);
            delete_buffer(output_buffer_type, output_device_id, src_buffer);
            output_buffers[i][f] = dest_buffer;
          }
        }
        // Keep non-warmup frame outputs
        work_item_output_sizes[i].insert(
            work_item_output_sizes[i].end(),
            output_sizes[i].begin() + warmup_frames, output_sizes[i].end());
        work_item_output_buffers[i].insert(
            work_item_output_buffers[i].end(),
            output_buffers[i].begin() + warmup_frames, output_buffers[i].end());
      }
      current_input += batch_size;
    }

    args.profiler.add_interval("task", work_start, now());

    LOG(INFO) << "Evaluate (N/PU/G: " << rank << "/" << args.id << "/"
              << args.evaluator_group << "): finished item "
              << work_entry.work_item_index;

    args.output_work.push(output_work_entry);
  }

  LOG(INFO) << "Evaluate (N/PU: " << rank << "/" << args.id
            << "): thread finished";

  THREAD_RETURN_SUCCESS();
}

///////////////////////////////////////////////////////////////////////////////
/// Thread to asynchronously save result buffers
void* save_thread(void* arg) {
  SaveThreadArgs& args = *reinterpret_cast<SaveThreadArgs*>(arg);

  auto setup_start = now();

  i32 rank;
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);

  // Setup a distinct storage backend for each IO thread
  storehouse::StorageBackend* storage =
      storehouse::StorageBackend::make_from_config(args.storage_config);

  args.profiler.add_interval("setup", setup_start, now());

  while (true) {
    auto idle_start = now();

    EvalWorkEntry work_entry;
    args.input_work.pop(work_entry);

    if (work_entry.work_item_index == -1) {
      break;
    }

    LOG(INFO) << "Save (N/PU: " << rank << "/" << args.id
              << "): processing item " << work_entry.work_item_index;

    args.profiler.add_interval("idle", idle_start, now());

    auto work_start = now();

    const VideoWorkItem& work_item =
        args.work_items[work_entry.work_item_index];

    const std::string& video_path = args.video_paths[work_item.video_index];
    const VideoMetadata& metadata = args.metadata[work_item.video_index];

    // Write out each output layer to an individual data file
    size_t num_frames =
        work_entry.buffers.empty() ? 0 : work_entry.buffers[0].size();
    for (size_t out_idx = 0; out_idx < args.output_names.size(); ++out_idx) {
      const std::string output_path = job_item_output_path(
          args.job_name, video_path, args.output_names[out_idx],
          work_entry.work_item_index);

      auto io_start = now();

      WriteFile* output_file = nullptr;
      {
        StoreResult result;
        EXP_BACKOFF(storage->make_write_file(output_path, output_file), result);
        exit_on_error(result);
      }

      if (work_entry.buffer_sizes[out_idx].size() != num_frames) {
        LOG(FATAL) << "Output layer's size vector has wrong length";
      }
      if (work_entry.buffers[out_idx].size() != num_frames) {
        LOG(FATAL) << "Output layer's buffer vector has wrong length";
      }

      // Write out all output sizes first so we can easily index into the file
      for (size_t i = 0; i < num_frames; ++i) {
        i64 buffer_size = work_entry.buffer_sizes[out_idx][i];
        write(output_file, buffer_size);
      }
      // Write actual output data
      for (size_t i = 0; i < num_frames; ++i) {
        i64 buffer_size = work_entry.buffer_sizes[out_idx][i];
        u8* buffer = work_entry.buffers[out_idx][i];
        write(output_file, buffer, buffer_size);
      }

      output_file->save();

      // TODO(apoms): For now, all evaluators are expected to return CPU
      //   buffers as output so just assume CPU
      for (size_t i = 0; i < num_frames; ++i) {
        delete_buffer(DeviceType::CPU,  // work_entry.buffer_type,
                      work_entry.buffer_device_id,
                      work_entry.buffers[out_idx][i]);
      }

      delete output_file;

      args.profiler.add_interval("io", io_start, now());
    }

    LOG(INFO) << "Save (N/PU: " << rank << "/" << args.id << "): finished item "
              << work_entry.work_item_index;

    args.profiler.add_interval("task", work_start, now());

    args.retired_items++;
  }

  LOG(INFO) << "Save (N/PU: " << rank << "/" << args.id
            << "): thread finished ";

  // Cleanup
  delete storage;

  THREAD_RETURN_SUCCESS();
}

///////////////////////////////////////////////////////////////////////////////
/// run_job
void run_job(storehouse::StorageConfig* config,
             PipelineDescription& pipeline_description,
             const std::string& job_name, const std::string& dataset_name) {
  std::vector<EvaluatorFactory*> evaluator_factories;
  for (auto& f : pipeline_description.evaluator_factories) {
    evaluator_factories.push_back(f.get());
  }
  Sampling sampling = pipeline_description.sampling;

  storehouse::StorageBackend* storage =
      storehouse::StorageBackend::make_from_config(config);

  i32 rank;
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);

  i32 num_nodes;
  MPI_Comm_size(MPI_COMM_WORLD, &num_nodes);

  std::vector<EvaluatorCapabilities> evaluator_caps;
  for (EvaluatorFactory* factory : evaluator_factories) {
    evaluator_caps.push_back(factory->get_capabilities());
  }

  // Load the dataset descriptor to find all data files
  DatasetDescriptor descriptor;
  {
    std::unique_ptr<RandomReadFile> file;
    exit_on_error(make_unique_random_read_file(
        storage, dataset_descriptor_path(dataset_name), file));
    u64 pos = 0;
    descriptor = deserialize_dataset_descriptor(file.get(), pos);
  }

  // Establish base time to use for profilers
  timepoint_t base_time = now();

  // Get video metadata for all videos for distributing with work items
  std::vector<std::string> video_paths(descriptor.video_names().begin(),
                                       descriptor.video_names().end());

  std::vector<VideoMetadata> video_metadata;
  for (size_t i = 0; i < video_paths.size(); ++i) {
    const std::string& path = video_paths.at(i);
    std::unique_ptr<RandomReadFile> metadata_file;
    exit_on_error(make_unique_random_read_file(
        storage, dataset_item_metadata_path(dataset_name, path),
        metadata_file));
    u64 pos = 0;
    video_metadata.push_back(
        deserialize_video_metadata(metadata_file.get(), pos));
  }

  // Break up videos and their frames into equal sized work items
  const i32 work_item_size = frames_per_work_item();
  // Track how work was broken up for each video so we can know how the
  // output will be chunked up when saved out

  // We need to know the maximum warmup size across all evaluators to correctly
  // warm up across all of them
  i32 warmup_size = 0;
  for (EvaluatorCapabilities& caps : evaluator_caps) {
    warmup_size = std::max(warmup_size, caps.warmup_size);
  }
  // determine
  u32 total_frames = 0;
  std::vector<std::string> final_column_names =
      evaluator_factories.back()->get_output_names();
  JobDescriptor job_descriptor;
  job_descriptor.set_work_item_size(work_item_size);
  JobDescriptor::Sampling desc_sampling;
  switch (sampling) {
    case Sampling::All:
      desc_sampling = JobDescriptor::All;
      break;
    case Sampling::Strided:
      desc_sampling = JobDescriptor::Strided;
      break;
    case Sampling::Gather:
      desc_sampling = JobDescriptor::Gather;
      break;
    case Sampling::SequenceGather:
      desc_sampling = JobDescriptor::SequenceGather;
      break;
  }
  job_descriptor.set_sampling(desc_sampling);
  for (size_t j = 0; j < final_column_names.size(); ++j) {
    JobDescriptor_Column* column = job_descriptor.add_columns();
    column->set_id(j);
    column->set_name(final_column_names[j]);
  }

  std::vector<VideoWorkItem> work_items;
  std::vector<LoadWorkEntry> load_work_items;
  if (sampling == Sampling::All) {
    for (size_t i = 0; i < video_paths.size(); ++i) {
      const VideoMetadata& meta = video_metadata[i];
      i32 allocated_frames = 0;
      while (allocated_frames < meta.frames()) {
        i32 frames_to_allocate =
            std::min(work_item_size, meta.frames() - allocated_frames);

        VideoWorkItem item;
        item.video_index = i;
        item.item_id = allocated_frames;
        item.next_item_id = allocated_frames + frames_to_allocate;
        item.rows_from_start = allocated_frames;
        work_items.push_back(item);

        LoadWorkEntry load_item;
        load_item.work_item_index = work_items.size() - 1;
        load_item.interval.start = allocated_frames;
        load_item.interval.end = allocated_frames + frames_to_allocate;
        load_work_items.push_back(load_item);

        allocated_frames += frames_to_allocate;
      }
      total_frames += meta.frames();
    }
  } else if (sampling == Sampling::Strided) {
    i32 stride = pipeline_description.stride;
    job_descriptor.set_stride(stride);
    for (size_t i = 0; i < video_paths.size(); ++i) {
      const VideoMetadata& meta = video_metadata[i];
      i32 allocated_frames = 0;
      while (allocated_frames < meta.frames()) {
        i32 frames_to_allocate =
            std::min(work_item_size * stride, meta.frames() - allocated_frames);

        VideoWorkItem item;
        item.video_index = i;
        item.item_id = allocated_frames;
        item.next_item_id = allocated_frames + frames_to_allocate;
        item.rows_from_start = allocated_frames / stride;
        work_items.push_back(item);

        LoadWorkEntry load_item;
        load_item.work_item_index = work_items.size() - 1;
        load_item.strided.stride = stride;
        load_item.strided.interval.start = allocated_frames;
        load_item.strided.interval.end = allocated_frames + frames_to_allocate;
        load_work_items.push_back(load_item);

        allocated_frames += frames_to_allocate;
        total_frames += frames_to_allocate / stride;
      }
    }
  } else if (sampling == Sampling::Gather) {
    for (const PointSamples& samples : pipeline_description.gather_points) {
      {
        JobDescriptor_PointSamples* jd_samples =
            job_descriptor.add_gather_points();
        jd_samples->set_video_index(samples.video_index);
        for (i32 f : samples.frames) {
          jd_samples->add_frames(f);
        }
      }

      const VideoMetadata& meta = video_metadata[samples.video_index];
      i32 frames_in_sample = static_cast<i32>(samples.frames.size());
      i32 allocated_frames = 0;
      while (allocated_frames < frames_in_sample) {
        i32 frames_to_allocate =
            std::min(work_item_size, frames_in_sample - allocated_frames);

        VideoWorkItem item;
        item.video_index = samples.video_index;
        item.item_id = allocated_frames;
        item.next_item_id = allocated_frames + frames_to_allocate;
        item.rows_from_start = allocated_frames;
        work_items.push_back(item);

        LoadWorkEntry load_item;
        load_item.work_item_index = work_items.size() - 1;
        load_item.gather_points.insert(
            load_item.gather_points.end(),
            samples.frames.begin() + allocated_frames,
            samples.frames.begin() + allocated_frames + frames_to_allocate);
        load_work_items.push_back(load_item);

        allocated_frames += frames_to_allocate;
      }
      total_frames += frames_in_sample;
    }
  } else if (sampling == Sampling::SequenceGather) {
    for (const SequenceSamples& samples :
         pipeline_description.gather_sequences) {
      {
        JobDescriptor_SequenceSamples* jd_samples =
            job_descriptor.add_gather_sequences();
        jd_samples->set_video_index(samples.video_index);
        for (const Interval& interval : samples.intervals) {
          JobDescriptor_Interval* jd_interval = jd_samples->add_intervals();
          jd_interval->set_start(interval.start);
          jd_interval->set_end(interval.end);
        }
      }

      const VideoMetadata& meta = video_metadata[samples.video_index];
      i32 total_frames_in_sequences = 0;
      i32 intervals_in_sample = static_cast<i32>(samples.intervals.size());
      for (size_t i = 0; i < intervals_in_sample; ++i) {
        i32 frames_in_sample =
            samples.intervals[i].end - samples.intervals[i].start;
        i32 allocated_frames = 0;
        while (allocated_frames < frames_in_sample) {
          i32 frames_to_allocate =
              std::min(work_item_size, frames_in_sample - allocated_frames);

          VideoWorkItem item;
          item.video_index = samples.video_index;
          item.item_id = total_frames_in_sequences;
          item.next_item_id = total_frames_in_sequences + frames_to_allocate;
          item.rows_from_start = allocated_frames;
          work_items.push_back(item);

          LoadWorkEntry load_item;
          load_item.work_item_index = work_items.size() - 1;
          load_item.gather_sequences.push_back(
              Interval{samples.intervals[i].start + allocated_frames,
                       samples.intervals[i].start + allocated_frames +
                           frames_to_allocate});
          load_work_items.push_back(load_item);

          allocated_frames += frames_to_allocate;
          total_frames_in_sequences += frames_to_allocate;
        }
        // Make sure we reset after each gather interval
        work_items.back().next_item_id = -1;
        total_frames += frames_in_sample;
      }
    }
  }

  if (is_master(rank)) {
    printf("Total work items: %lu, Total frames: %u\n", work_items.size(),
           total_frames);
  }

  // Setup shared resources for distributing work to processing threads
  i64 accepted_items = 0;
  Queue<LoadWorkEntry> load_work;
  Queue<EvalWorkEntry> initial_eval_work;
  std::vector<std::vector<Queue<EvalWorkEntry>>> eval_work(PUS_PER_NODE);
  Queue<EvalWorkEntry> save_work;
  std::atomic<i64> retired_items{0};

  // Setup load workers
  std::vector<Profiler> load_thread_profilers(LOAD_WORKERS_PER_NODE,
                                              Profiler(base_time));
  std::vector<LoadThreadArgs> load_thread_args;
  for (i32 i = 0; i < LOAD_WORKERS_PER_NODE; ++i) {
    // Create IO thread for reading and decoding data
    load_thread_args.emplace_back(LoadThreadArgs{
        // Uniform arguments
        dataset_name, sampling, warmup_size, video_paths, video_metadata,
        work_items,

        // Per worker arguments
        i, config, load_thread_profilers[i],

        // Queues
        load_work, initial_eval_work,
    });
  }
  std::vector<pthread_t> load_threads(LOAD_WORKERS_PER_NODE);
  for (i32 i = 0; i < LOAD_WORKERS_PER_NODE; ++i) {
    pthread_create(&load_threads[i], NULL, load_video_thread,
                   &load_thread_args[i]);
  }

  // Setup evaluate workers

  // Initialize factory groups which determine which evaluators run in the
  // same thread. Evaluators running in different threads should be using
  // different physical resources
  std::vector<std::vector<EvaluatorFactory*>> factory_groups;
  if (evaluator_caps.front().can_overlap) {
    std::vector<EvaluatorFactory*> start_factories;
    start_factories.push_back(evaluator_factories.front());
    std::vector<EvaluatorFactory*> main_factories(
        evaluator_factories.begin() + 1, evaluator_factories.end() - 1);

    factory_groups.push_back(start_factories);
    factory_groups.push_back(main_factories);
  } else {
    std::vector<EvaluatorFactory*> main_factories(
        evaluator_factories.begin(), evaluator_factories.end() - 1);
    factory_groups.push_back(main_factories);
  }
  if (evaluator_caps.size() > 1 && evaluator_caps.back().can_overlap) {
    std::vector<EvaluatorFactory*> end_factories(evaluator_factories.end() - 1,
                                                 evaluator_factories.end());
    factory_groups.push_back(end_factories);
  } else {
    factory_groups.back().push_back(evaluator_factories.back());
  }
  i32 factory_groups_per_chain = static_cast<i32>(factory_groups.size());

  std::vector<std::vector<Profiler>> eval_chain_profilers(PUS_PER_NODE);
  std::vector<std::vector<EvaluateThreadArgs>> eval_chain_args(PUS_PER_NODE);

  for (i32 pu = 0; pu < PUS_PER_NODE; ++pu) {
    std::vector<Queue<EvalWorkEntry>>& work_queues = eval_work[pu];
    std::vector<Profiler>& eval_thread_profilers = eval_chain_profilers[pu];
    std::vector<EvaluateThreadArgs>& eval_thread_args = eval_chain_args[pu];
    work_queues.resize(factory_groups_per_chain - 1);
    // Setup profilers and thread args
    for (i32 fg = 0; fg < factory_groups_per_chain; ++fg) {
      eval_thread_profilers.push_back(Profiler(base_time));
    }
    for (i32 fg = 0; fg < factory_groups_per_chain; ++fg) {
      std::vector<EvaluatorConfig> eval_configs;
      for (size_t i = 0; i < factory_groups[fg].size(); ++i) {
        EvaluatorConfig eval_config;
        eval_config.max_input_count =
            std::max(frames_per_work_item(), warmup_size);
        eval_config.max_frame_width = descriptor.max_width();
        eval_config.max_frame_height = descriptor.max_height();
        eval_config.device_ids = {pu};
        eval_configs.push_back(eval_config);
      }
      // Input work queue
      Queue<EvalWorkEntry>* input_work_queue;
      bool first_evaluator_group = (fg == 0);
      if (first_evaluator_group) {
        input_work_queue = &initial_eval_work;
      } else {
        input_work_queue = &work_queues[fg - 1];
      }
      // Create new queue for output, reuse previous queue as input
      bool last_evaluator_group = (fg == factory_groups_per_chain - 1);
      Queue<EvalWorkEntry>* output_work_queue;
      if (last_evaluator_group) {
        output_work_queue = &save_work;
      } else {
        output_work_queue = &work_queues[fg];
      }
      // Create eval thread for passing data through neural net
      eval_thread_args.emplace_back(EvaluateThreadArgs{
          // Uniform arguments
          warmup_size, video_metadata, work_items,

          // Per worker arguments
          pu, fg, last_evaluator_group, factory_groups[fg], eval_configs,
          eval_thread_profilers[fg],

          // Queues
          *input_work_queue, *output_work_queue});
    }
  }
  std::vector<std::vector<pthread_t>> eval_chain_threads(PUS_PER_NODE);
  for (i32 pu = 0; pu < PUS_PER_NODE; ++pu) {
    std::vector<pthread_t>& eval_threads = eval_chain_threads[pu];
    eval_threads.resize(factory_groups_per_chain);
    for (i32 fg = 0; fg < factory_groups_per_chain; ++fg) {
      pthread_create(&eval_threads[fg], NULL, evaluate_thread,
                     &eval_chain_args[pu][fg]);
    }
  }

  // Setup save workers
  std::vector<Profiler> save_thread_profilers(SAVE_WORKERS_PER_NODE,
                                              Profiler(base_time));
  std::vector<SaveThreadArgs> save_thread_args;
  for (i32 i = 0; i < SAVE_WORKERS_PER_NODE; ++i) {
    // Create IO thread for reading and decoding data
    save_thread_args.emplace_back(SaveThreadArgs{
        // Uniform arguments
        job_name, video_paths, video_metadata, work_items,
        evaluator_factories.back()->get_output_names(),

        // Per worker arguments
        i, config, save_thread_profilers[i],

        // Queues
        save_work, retired_items});
  }
  std::vector<pthread_t> save_threads(SAVE_WORKERS_PER_NODE);
  for (i32 i = 0; i < SAVE_WORKERS_PER_NODE; ++i) {
    pthread_create(&save_threads[i], NULL, save_thread, &save_thread_args[i]);
  }

  // Push work into load queues
  if (is_master(rank)) {
    // Begin distributing work on master node
    i32 next_work_item_to_allocate = 0;
    // Wait for clients to ask for work
    while (next_work_item_to_allocate < static_cast<i32>(work_items.size())) {
      // Check if we need to allocate work to our own processing thread
      i32 local_work = accepted_items - retired_items;
      if (local_work < PUS_PER_NODE * TASKS_IN_QUEUE_PER_PU) {
        LoadWorkEntry& entry = load_work_items[next_work_item_to_allocate++];
        load_work.push(entry);

        accepted_items++;
        if ((static_cast<i32>(work_items.size()) - next_work_item_to_allocate) %
                10 ==
            0) {
          printf("Work items left: %d\n", static_cast<i32>(work_items.size()) -
                                              next_work_item_to_allocate);
          fflush(stdout);
        }
        continue;
      }

      if (num_nodes > 1) {
        i32 more_work;
        MPI_Status status;
        MPI_Recv(&more_work, 1, MPI_INT, MPI_ANY_SOURCE, MPI_ANY_TAG,
                 MPI_COMM_WORLD, &status);
        i32 next_item = next_work_item_to_allocate++;
        MPI_Send(&next_item, 1, MPI_INT, status.MPI_SOURCE, 0, MPI_COMM_WORLD);
        std::this_thread::yield();
      }
    }
    i32 workers_done = 1;
    while (workers_done < num_nodes) {
      i32 more_work;
      MPI_Status status;
      MPI_Recv(&more_work, 1, MPI_INT, MPI_ANY_SOURCE, MPI_ANY_TAG,
               MPI_COMM_WORLD, &status);
      i32 next_item = -1;
      MPI_Send(&next_item, 1, MPI_INT, status.MPI_SOURCE, 0, MPI_COMM_WORLD);
      workers_done += 1;
      std::this_thread::yield();
    }
  } else {
    // Monitor amount of work left and request more when running low
    while (true) {
      i32 local_work = accepted_items - retired_items;
      ;
      if (local_work < PUS_PER_NODE * TASKS_IN_QUEUE_PER_PU) {
        // Request work when there is only a few unprocessed items left
        i32 more_work = true;
        MPI_Send(&more_work, 1, MPI_INT, 0, 0, MPI_COMM_WORLD);
        i32 next_item;
        MPI_Recv(&next_item, 1, MPI_INT, 0, MPI_ANY_TAG, MPI_COMM_WORLD,
                 MPI_STATUS_IGNORE);
        if (next_item == -1) {
          // No more work left
          break;
        } else {
          LoadWorkEntry entry;
          entry.work_item_index = next_item;
          load_work.push(entry);
          accepted_items++;
        }
      }
      std::this_thread::yield();
    }
  }

  // Push sentinel work entries into queue to terminate load threads
  for (i32 i = 0; i < LOAD_WORKERS_PER_NODE; ++i) {
    LoadWorkEntry entry;
    entry.work_item_index = -1;
    load_work.push(entry);
  }

  for (i32 i = 0; i < LOAD_WORKERS_PER_NODE; ++i) {
    // Wait until load has finished
    void* result;
    i32 err = pthread_join(load_threads[i], &result);
    if (err != 0) {
      fprintf(stderr, "error in pthread_join of load thread\n");
      exit(EXIT_FAILURE);
    }
    free(result);
  }

  // Push sentinel work entries into queue to terminate eval threads
  for (i32 i = 0; i < PUS_PER_NODE; ++i) {
    EvalWorkEntry entry;
    entry.work_item_index = -1;
    initial_eval_work.push(entry);
  }

  for (i32 i = 0; i < PUS_PER_NODE; ++i) {
    // Wait until first eval has finished
    void* result;
    i32 err = pthread_join(eval_chain_threads[i][0], &result);
    if (err != 0) {
      fprintf(stderr, "error in pthread_join of eval thread\n");
      exit(EXIT_FAILURE);
    }
    free(result);
  }

  for (i32 fg = 1; fg < factory_groups_per_chain; ++fg) {
    for (i32 pu = 0; pu < PUS_PER_NODE; ++pu) {
      EvalWorkEntry entry;
      entry.work_item_index = -1;
      eval_work[pu][fg - 1].push(entry);
    }
    for (i32 pu = 0; pu < PUS_PER_NODE; ++pu) {
      // Wait until eval has finished
      void* result;
      i32 err = pthread_join(eval_chain_threads[pu][fg], &result);
      if (err != 0) {
        fprintf(stderr, "error in pthread_join of eval thread\n");
        exit(EXIT_FAILURE);
      }
      free(result);
    }
  }

  // Push sentinel work entries into queue to terminate save threads
  for (i32 i = 0; i < SAVE_WORKERS_PER_NODE; ++i) {
    EvalWorkEntry entry;
    entry.work_item_index = -1;
    save_work.push(entry);
  }

  for (i32 i = 0; i < SAVE_WORKERS_PER_NODE; ++i) {
    // Wait until eval has finished
    void* result;
    i32 err = pthread_join(save_threads[i], &result);
    if (err != 0) {
      fprintf(stderr, "error in pthread_join of save thread\n");
      exit(EXIT_FAILURE);
    }
    free(result);
  }

  if (is_master(rank)) {
    // Add job name into database metadata so we can look up what jobs have
    // been
    // ran
    i32 job_id;
    {
      const std::string db_meta_path = database_metadata_path();

      std::unique_ptr<RandomReadFile> meta_in_file;
      make_unique_random_read_file(storage, db_meta_path, meta_in_file);
      u64 pos = 0;
      DatabaseMetadata meta =
          deserialize_database_metadata(meta_in_file.get(), pos);

      i32 dataset_id = meta.get_dataset_id(dataset_name);
      job_id = meta.add_job(dataset_id, job_name);

      std::unique_ptr<WriteFile> meta_out_file;
      make_unique_write_file(storage, db_meta_path, meta_out_file);
      serialize_database_metadata(meta_out_file.get(), meta);
    }

    job_descriptor.set_id(job_id);

    // Write out metadata to describe where the output results are for each
    // video
    {
      const std::string job_file_path = job_descriptor_path(job_name);
      std::unique_ptr<WriteFile> output_file;
      make_unique_write_file(storage, job_file_path, output_file);

      serialize_job_descriptor(output_file.get(), job_descriptor);

      output_file->save();
    }
  }

  // Execution done, write out profiler intervals for each worker
  std::string profiler_file_name = job_profiler_path(job_name, rank);
  std::ofstream profiler_output(profiler_file_name, std::fstream::binary);

  // Write out total time interval
  timepoint_t end_time = now();

  i64 start_time_ns =
      std::chrono::time_point_cast<std::chrono::nanoseconds>(base_time)
          .time_since_epoch()
          .count();
  i64 end_time_ns =
      std::chrono::time_point_cast<std::chrono::nanoseconds>(end_time)
          .time_since_epoch()
          .count();
  profiler_output.write((char*)&start_time_ns, sizeof(start_time_ns));
  profiler_output.write((char*)&end_time_ns, sizeof(end_time_ns));

  i64 out_rank = rank;
  // Load worker profilers
  u8 load_worker_count = LOAD_WORKERS_PER_NODE;
  profiler_output.write((char*)&load_worker_count, sizeof(load_worker_count));
  for (i32 i = 0; i < LOAD_WORKERS_PER_NODE; ++i) {
    write_profiler_to_file(profiler_output, out_rank, "load", "", i,
                           load_thread_profilers[i]);
  }

  // Evaluate worker profilers
  u8 eval_worker_count = PUS_PER_NODE;
  profiler_output.write((char*)&eval_worker_count, sizeof(eval_worker_count));
  u8 groups_per_chain = factory_groups_per_chain;
  profiler_output.write((char*)&groups_per_chain, sizeof(groups_per_chain));
  for (i32 pu = 0; pu < PUS_PER_NODE; ++pu) {
    for (i32 fg = 0; fg < factory_groups_per_chain; ++fg) {
      i32 i = pu;
      std::string tag = "fg" + std::to_string(fg);
      write_profiler_to_file(profiler_output, out_rank, "eval", tag, i,
                             eval_chain_profilers[pu][fg]);
    }
  }

  // Save worker profilers
  u8 save_worker_count = SAVE_WORKERS_PER_NODE;
  profiler_output.write((char*)&save_worker_count, sizeof(save_worker_count));
  for (i32 i = 0; i < SAVE_WORKERS_PER_NODE; ++i) {
    write_profiler_to_file(profiler_output, out_rank, "save", "", i,
                           save_thread_profilers[i]);
  }

  profiler_output.close();

  delete storage;
}
}

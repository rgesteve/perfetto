/*
 * Copyright (C) 2024 The Android Open Source Project
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

#include "src/trace_redaction/trace_redactor.h"

#include <sys/mman.h>
#include <unistd.h>
#include <cstddef>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/scoped_file.h"
#include "perfetto/protozero/scattered_heap_buffer.h"
#include "perfetto/trace_processor/trace_blob.h"
#include "perfetto/trace_processor/trace_blob_view.h"
#include "src/trace_redaction/trace_redaction_framework.h"

#include "protos/perfetto/trace/trace.pbzero.h"

namespace perfetto::trace_redaction {
namespace {

using Trace = protos::pbzero::Trace;
using TracePacket = protos::pbzero::TracePacket;

// Basic scoped objects (similar to base::ScopedResource) to make sure page
// resources are cleaned-up.
struct MappedFile {
  void* start_address;
  size_t size;

  MappedFile() : start_address(MAP_FAILED), size(0) {}

  ~MappedFile() {
    if (start_address != MAP_FAILED) {
      munmap(start_address, size);
    }
  }

  MappedFile(const MappedFile&) = delete;
  MappedFile& operator=(const MappedFile&) = delete;
  MappedFile(MappedFile&&) = delete;
  MappedFile& operator=(MappedFile&&) = delete;
};

}  // namespace

TraceRedactor::TraceRedactor() = default;

TraceRedactor::~TraceRedactor() = default;

base::Status TraceRedactor::Redact(std::string_view source_filename,
                                   std::string_view dest_filename,
                                   Context* context) const {
  const std::string source_filename_str(source_filename);

  base::ScopedFile file_handle(base::OpenFile(source_filename_str, O_RDONLY));

  if (!file_handle) {
    return base::ErrStatus("Failed to read trace from disk: %s",
                           source_filename_str.c_str());
  }

  auto file_size_offset = lseek(*file_handle, 0, SEEK_END);

  if (file_size_offset <= 0) {
    return base::ErrStatus("Could not determine trace size (%s)",
                           source_filename_str.c_str());
  }

  lseek(*file_handle, 0, SEEK_SET);

  MappedFile page;
  page.size = static_cast<size_t>(file_size_offset);
  page.start_address =
      mmap(nullptr, page.size, PROT_READ, MAP_PRIVATE, *file_handle, 0);

  if (page.start_address == MAP_FAILED) {
    return base::ErrStatus("Failed to map pages for trace (%zu bytes)",
                           page.size);
  }

  trace_processor::TraceBlobView whole_view(
      trace_processor::TraceBlob::FromMmap(page.start_address, page.size));

  if (auto status = Collect(context, whole_view); !status.ok()) {
    return status;
  }

  if (auto status = Build(context); !status.ok()) {
    return status;
  }

  if (auto status = Transform(*context, whole_view, std::string(dest_filename));
      !status.ok()) {
    return status;
  }

  return base::OkStatus();
}

base::Status TraceRedactor::Collect(
    Context* context,
    const trace_processor::TraceBlobView& view) const {
  // Mask, marking which collectors should be ran. When a collector no longer
  // needs to run, the value will be null.
  std::vector<const CollectPrimitive*> collectors;
  collectors.reserve(collectors_.size());

  for (const auto& collector : collectors_) {
    collectors.push_back(collector.get());
  }

  const Trace::Decoder trace_decoder(view.data(), view.length());

  for (auto packet_it = trace_decoder.packet(); packet_it; ++packet_it) {
    const TracePacket::Decoder packet(packet_it->as_bytes());

    for (auto cit = collectors.begin(); cit != collectors.end();) {
      auto status = (*cit)->Collect(packet, context);

      if (!status.ok()) {
        return status.status();
      }

      // If this collector has returned `kStop`, it means that it (and it alone)
      // no longer needs to run. The driver (TraceRedactor) should not invoke it
      // on any future packets.
      if (status.value() == CollectPrimitive::ContinueCollection::kRetire) {
        cit = collectors.erase(cit);
      } else {
        ++cit;
      }
    }

    // If all the collectors have found what they were looking for, then there
    // is no reason to continue through the trace.
    if (collectors.empty()) {
      break;
    }
  }

  return base::OkStatus();
}

base::Status TraceRedactor::Build(Context* context) const {
  for (const auto& builder : builders_) {
    if (auto status = builder->Build(context); !status.ok()) {
      return status;
    }
  }

  return base::OkStatus();
}

base::Status TraceRedactor::Transform(
    const Context& context,
    const trace_processor::TraceBlobView& view,
    const std::string& dest_file) const {
  std::ignore = context;
  const auto dest_fd = base::OpenFile(dest_file, O_RDWR | O_CREAT, 0666);

  if (dest_fd.get() == -1) {
    return base::ErrStatus(
        "Failed to open destination file; can't write redacted trace.");
  }

  const Trace::Decoder trace_decoder(view.data(), view.length());
  for (auto packet_it = trace_decoder.packet(); packet_it; ++packet_it) {
    auto packet = packet_it->as_std_string();

    for (const auto& transformer : transformers_) {
      if (auto status = transformer->Transform(context, &packet);
          !status.ok()) {
        return status;
      }
    }

    protozero::HeapBuffered<> serializer;
    auto* packet_message =
        serializer->BeginNestedMessage<TracePacket>(Trace::kPacketFieldNumber);
    packet_message->AppendRawProtoBytes(packet.data(), packet.size());
    packet_message->Finalize();
    serializer->Finalize();

    auto encoded_packet = serializer.SerializeAsString();

    if (const auto exported_data = base::WriteAll(
            dest_fd.get(), encoded_packet.data(), encoded_packet.size());
        exported_data <= 0) {
      return base::ErrStatus("Failed to write redacted trace to disk");
    }
  }

  return base::OkStatus();
}

}  // namespace perfetto::trace_redaction

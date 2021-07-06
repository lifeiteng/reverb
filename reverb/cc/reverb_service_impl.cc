// Copyright 2019 DeepMind Technologies Limited.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "reverb/cc/reverb_service_impl.h"

#include <algorithm>
#include <list>
#include <memory>
#include <vector>

#include "google/protobuf/timestamp.pb.h"
#include "absl/status/status.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "reverb/cc/checkpointing/interface.h"
#include "reverb/cc/platform/hash_map.h"
#include "reverb/cc/platform/hash_set.h"
#include "reverb/cc/platform/logging.h"
#include "reverb/cc/platform/status_macros.h"
#include "reverb/cc/platform/thread.h"
#include "reverb/cc/reverb_service.grpc.pb.h"
#include "reverb/cc/reverb_service.pb.h"
#include "reverb/cc/sampler.h"
#include "reverb/cc/support/cleanup.h"
#include "reverb/cc/support/grpc_util.h"
#include "reverb/cc/support/queue.h"
#include "reverb/cc/support/trajectory_util.h"
#include "reverb/cc/support/uint128.h"

namespace deepmind {
namespace reverb {
namespace {

// Multiple `ChunkData` can be sent with the same `SampleStreamResponse`. If
// the size of the message exceeds this value then the request is sent and the
// remaining chunks are sent with other messages.
static constexpr int64_t kMaxSampleResponseSizeBytes = 40 * 1024 * 1024;  // 40MB.

inline grpc::Status TableNotFound(absl::string_view name) {
  return grpc::Status(grpc::StatusCode::NOT_FOUND,
                      absl::StrCat("Priority table ", name, " was not found"));
}

inline grpc::Status Internal(const std::string& message) {
  return grpc::Status(grpc::StatusCode::INTERNAL, message);
}

}  // namespace

ReverbServiceImpl::ReverbServiceImpl(std::shared_ptr<Checkpointer> checkpointer)
    : checkpointer_(std::move(checkpointer)) {}

absl::Status ReverbServiceImpl::Create(
    std::vector<std::shared_ptr<Table>> tables,
    std::shared_ptr<Checkpointer> checkpointer,
    std::unique_ptr<ReverbServiceImpl>* service) {
  // Can't use make_unique because it can't see the Impl's private constructor.
  auto new_service = std::unique_ptr<ReverbServiceImpl>(
      new ReverbServiceImpl(std::move(checkpointer)));
  REVERB_RETURN_IF_ERROR(new_service->Initialize(std::move(tables)));
  std::swap(new_service, *service);
  return absl::OkStatus();
}

absl::Status ReverbServiceImpl::Create(
    std::vector<std::shared_ptr<Table>> tables,
    std::unique_ptr<ReverbServiceImpl>* service) {
  return Create(std::move(tables), /*checkpointer=*/nullptr, service);
}

absl::Status ReverbServiceImpl::Initialize(
    std::vector<std::shared_ptr<Table>> tables) {
  if (checkpointer_ != nullptr) {
    // We start by attempting to load the latest checkpoint from the root
    // directory.
    // In general we expect this to be nonempty (and thus succeed)
    // if this is a restart of a previously running job (e.g preemption).
    auto status = checkpointer_->LoadLatest(&chunk_store_, &tables);
    if (absl::IsNotFound(status)) {
      // No checkpoint was found in the root directory. If a fallback
      // checkpoint (path) has been configured then we attempt to load that
      // checkpoint instead.
      // Note that by first attempting to load from the root directory and
      // then only loading the fallback checkpoint iff the root directory is
      // empty we are effectively using the fallback checkpoint as a way to
      // initialise the service with a checkpoint generated by another
      // experiment.
      status = checkpointer_->LoadFallbackCheckpoint(&chunk_store_, &tables);
    }
    // If no checkpoint was found in neither the root directory nor a fallback
    // checkpoint was provided then proceed to initialise an empty service.
    // All other error types are unexpected and bubbled up to the caller.
    if (!status.ok() && !absl::IsNotFound(status)) {
      return status;
    }
  }

  for (auto& table : tables) {
    tables_[table->name()] = std::move(table);
  }

  tables_state_id_ = absl::MakeUint128(absl::Uniform<uint64_t>(rnd_),
                                       absl::Uniform<uint64_t>(rnd_));

  return absl::OkStatus();
}

grpc::Status ReverbServiceImpl::Checkpoint(grpc::ServerContext* context,
                                           const CheckpointRequest* request,
                                           CheckpointResponse* response) {
  if (checkpointer_ == nullptr) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "no Checkpointer configured for the replay service.");
  }

  std::vector<Table*> tables;
  for (auto& table : tables_) {
    tables.push_back(table.second.get());
  }

  auto status = checkpointer_->Save(std::move(tables), 1,
                                    response->mutable_checkpoint_path());
  if (!status.ok()) return ToGrpcStatus(status);

  REVERB_LOG(REVERB_INFO) << "Stored checkpoint to "
                          << response->checkpoint_path();
  return grpc::Status::OK;
}

grpc::Status ReverbServiceImpl::InsertStream(
    grpc::ServerContext* context,
    grpc::ServerReaderWriter<InsertStreamResponse, InsertStreamRequest>*
        stream) {
  return InsertStreamInternal(context, stream);
}

grpc::Status ReverbServiceImpl::InsertStreamInternal(
    grpc::ServerContext* context,
    grpc::ServerReaderWriterInterface<InsertStreamResponse,
                                      InsertStreamRequest>* stream) {
  // Start a background thread that unpacks the data ahead of time.
  deepmind::reverb::internal::Queue<InsertStreamRequest> queue(1);
  auto read_thread = internal::StartThread("ReadThread", [stream, &queue]() {
    InsertStreamRequest request;
    while (stream->Read(&request) && queue.Push(std::move(request))) {
      request = InsertStreamRequest();
    }
    queue.SetLastItemPushed();
  });
  auto cleanup = internal::MakeCleanup([&queue] { queue.Close(); });

  internal::flat_hash_map<ChunkStore::Key, std::shared_ptr<ChunkStore::Chunk>>
      chunks;

  InsertStreamRequest request;
  while (queue.Pop(&request)) {
    for (auto& chunk : *request.mutable_chunks()) {
      ChunkStore::Key key = chunk.chunk_key();
      std::shared_ptr<ChunkStore::Chunk> chunk_sp =
          chunk_store_.Insert(std::move(chunk));
      if (chunk_sp == nullptr) {
        return grpc::Status(grpc::StatusCode::CANCELLED,
                            "Service has been closed");
      }
      chunks[key] = std::move(chunk_sp);
    }

    if (request.has_item()) {
      Table::Item item;

      auto push_or = [&chunks, &item](ChunkStore::Key key) -> grpc::Status {
        auto it = chunks.find(key);
        if (it == chunks.end()) {
          return Internal(
              absl::StrCat("Could not find sequence chunk ", key, "."));
        }
        item.chunks.push_back(it->second);
        return grpc::Status::OK;
      };

      for (ChunkStore::Key key :
           internal::GetChunkKeys(request.item().item().flat_trajectory())) {
        auto status = push_or(key);
        if (!status.ok()) return status;
      }

      const auto& table_name = request.item().item().table();
      Table* table = TableByName(table_name);
      if (table == nullptr) return TableNotFound(table_name);

      const auto item_key = request.item().item().key();
      item.item = std::move(*request.mutable_item()->mutable_item());

      if (auto status = table->InsertOrAssign(item); !status.ok()) {
        return ToGrpcStatus(status);
      }

      // Let caller know that the item has been inserted if requested by the
      // caller.
      if (request.item().send_confirmation()) {
        InsertStreamResponse response;
        response.add_keys(item_key);
        if (!stream->Write(response)) {
          return Internal(absl::StrCat(
              "Error when sending confirmation that item ", item_key,
              " has been successfully inserted/updated."));
        }
      }

      // Only keep specified chunks.
      absl::flat_hash_set<int64_t> keep_keys{
          request.item().keep_chunk_keys().begin(),
          request.item().keep_chunk_keys().end()};
      for (auto it = chunks.cbegin(); it != chunks.cend();) {
        if (keep_keys.find(it->first) == keep_keys.end()) {
          chunks.erase(it++);
        } else {
          ++it;
        }
      }
      REVERB_CHECK_EQ(chunks.size(), keep_keys.size())
          << "Kept less chunks than expected.";
    }
  }

  return grpc::Status::OK;
}

grpc::Status ReverbServiceImpl::MutatePriorities(
    grpc::ServerContext* context, const MutatePrioritiesRequest* request,
    MutatePrioritiesResponse* response) {
  Table* table = TableByName(request->table());
  if (table == nullptr) return TableNotFound(request->table());

  auto status = table->MutateItems(
      std::vector<KeyWithPriority>(request->updates().begin(),
                                   request->updates().end()),
      request->delete_keys());
  if (!status.ok()) return ToGrpcStatus(status);
  return grpc::Status::OK;
}

grpc::Status ReverbServiceImpl::Reset(grpc::ServerContext* context,
                                      const ResetRequest* request,
                                      ResetResponse* response) {
  Table* table = TableByName(request->table());
  if (table == nullptr) return TableNotFound(request->table());

  auto status = table->Reset();
  if (!status.ok()) {
    return ToGrpcStatus(status);
  }
  return grpc::Status::OK;
}

grpc::Status ReverbServiceImpl::SampleStream(
    grpc::ServerContext* context,
    grpc::ServerReaderWriter<SampleStreamResponse, SampleStreamRequest>*
        stream) {
  return SampleStreamInternal(context, stream);
}

grpc::Status ReverbServiceImpl::SampleStreamInternal(
    grpc::ServerContext* context,
    grpc::ServerReaderWriterInterface<SampleStreamResponse,
                                      SampleStreamRequest>* stream) {
  SampleStreamRequest request;
  if (!stream->Read(&request)) {
    return Internal("Could not read initial request");
  }
  absl::Duration timeout =
      absl::Milliseconds(request.has_rate_limiter_timeout()
                             ? request.rate_limiter_timeout().milliseconds()
                             : -1);
  if (timeout < absl::ZeroDuration()) timeout = absl::InfiniteDuration();

  do {
    if (request.num_samples() <= 0) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                          "`num_samples` must be > 0.");
    }
    if (request.flexible_batch_size() <= 0 &&
        request.flexible_batch_size() != Sampler::kAutoSelectValue) {
      return grpc::Status(
          grpc::StatusCode::INVALID_ARGUMENT,
          absl::StrCat("`flexible_batch_size` must be > 0 or ",
                       Sampler::kAutoSelectValue, " (for auto tuning)."));
    }
    Table* table = TableByName(request.table());
    if (table == nullptr) return TableNotFound(request.table());
    int32_t default_flexible_batch_size = table->DefaultFlexibleBatchSize();

    int count = 0;

    while (!context->IsCancelled() && count != request.num_samples()) {
      std::vector<Table::SampledItem> samples;
      int32_t max_batch_size = std::min<int32_t>(
          request.flexible_batch_size() == Sampler::kAutoSelectValue
              ? default_flexible_batch_size
              : request.flexible_batch_size(),
          request.num_samples() - count);
      if (auto status =
              table->SampleFlexibleBatch(&samples, max_batch_size, timeout);
          !status.ok()) {
        return ToGrpcStatus(status);
      }
      count += samples.size();

      for (auto& sample : samples) {
        SampleStreamResponse response;
        auto* entry = response.add_entries();

        for (int chunk_idx = 0; chunk_idx < sample.ref->chunks.size();
             chunk_idx++) {
          entry->set_end_of_sequence(chunk_idx + 1 ==
                                       sample.ref->chunks.size());

          // Attach the info to the first message.
          if (chunk_idx == 0) {
            auto* item = entry->mutable_info()->mutable_item();
            *item = sample.ref->item;
            item->set_priority(sample.priority);
            item->set_times_sampled(sample.times_sampled);
            entry->mutable_info()->set_probability(sample.probability);
            entry->mutable_info()->set_table_size(sample.table_size);
            entry->mutable_info()->set_rate_limited(sample.rate_limited);
          }

          // We const cast to avoid copying the proto.
          entry->mutable_data()->UnsafeArenaAddAllocated(
              const_cast<ChunkData*>(&sample.ref->chunks[chunk_idx]->data()));

          // If more chunks remain and we haven't yet reached the maximum
          // message size then we'll continue and add (at least) one more chunk
          // to the same response.
          if (chunk_idx < sample.ref->chunks.size() - 1 &&
              entry->ByteSizeLong() < kMaxSampleResponseSizeBytes) {
            continue;
          }

          grpc::WriteOptions options;
          options.set_no_compression();  // Data is already compressed.

          bool ok = stream->Write(response, options);

          // Release the chunks we "borrowed" from the sample object. Failing to
          // do so would result in the chunks being deallocated prematurely and
          // cause nullptr errors.
          while (entry->data_size() != 0) {
            entry->mutable_data()->UnsafeArenaReleaseLast();
          }

          if (!ok) {
            return Internal("Failed to write to Sample stream.");
          }

          response.Clear();
          entry = response.add_entries();
        }
      }
    }

    request.Clear();
  } while (stream->Read(&request));

  return grpc::Status::OK;
}

Table* ReverbServiceImpl::TableByName(absl::string_view name) const {
  auto it = tables_.find(name);
  if (it == tables_.end()) return nullptr;
  return it->second.get();
}

void ReverbServiceImpl::Close() {
  for (auto& table : tables_) {
    table.second->Close();
  }
}

std::string ReverbServiceImpl::DebugString() const {
  std::string str = "ReverbService(tables=[";
  for (auto iter = tables_.cbegin(); iter != tables_.cend(); ++iter) {
    if (iter != tables_.cbegin()) {
      absl::StrAppend(&str, ", ");
    }
    absl::StrAppend(&str, iter->second->DebugString());
  }
  absl::StrAppend(&str, "], checkpointer=",
                  (checkpointer_ ? checkpointer_->DebugString() : "nullptr"),
                  ")");
  return str;
}

grpc::Status ReverbServiceImpl::ServerInfo(grpc::ServerContext* context,
                                           const ServerInfoRequest* request,
                                           ServerInfoResponse* response) {
  for (const auto& iter : tables_) {
    *response->add_table_info() = iter.second->info();
  }
  *response->mutable_tables_state_id() = Uint128ToMessage(tables_state_id_);
  return grpc::Status::OK;
}

internal::flat_hash_map<std::string, std::shared_ptr<Table>>
ReverbServiceImpl::tables() const {
  return tables_;
}

grpc::Status ReverbServiceImpl::InitializeConnection(
    grpc::ServerContext* context,
    grpc::ServerReaderWriter<InitializeConnectionResponse,
                             InitializeConnectionRequest>* stream) {
  if (!IsLocalhostOrInProcess(context->peer())) {
    return grpc::Status::OK;
  }

  InitializeConnectionRequest request;
  if (!stream->Read(&request)) {
    return Internal("Failed to read from stream");
  }

  if (request.pid() != getpid()) {
    // Respond without populating the address field.
    InitializeConnectionResponse response;
    response.set_address(0);
    stream->Write(response);
    return grpc::Status::OK;
  }

  auto it = tables_.find(request.table_name());
  if (it == tables_.end()) {
    return TableNotFound(request.table_name());
  }

  // Allocate a new shared pointer on the heap and transmit its memory address.
  // The client will dereference and assume ownership of the object before
  // sending its response. For simplicity, the client will copy the shared_ptr
  // so the server is always responsible for cleaning up the heap allocated
  // object.
  auto* ptr = new std::shared_ptr<Table>(it->second);
  auto cleanup = internal::MakeCleanup([&] { delete ptr; });

  // Send address to client.
  InitializeConnectionResponse response;
  response.set_address(reinterpret_cast<int64_t>(ptr));
  if (!stream->Write(response)) {
    return Internal("Failed to write to stream.");
  }

  // Wait for the client to confirm ownership transfer.
  if (!stream->Read(&request)) {
    return Internal("Failed to read from stream.");
  }

  if (!request.ownership_transferred()) {
    return Internal("Received unexpected request");
  }

  return grpc::Status::OK;
}

}  // namespace reverb
}  // namespace deepmind

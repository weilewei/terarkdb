//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "db/flush_job.h"

#ifndef __STDC_FORMAT_MACROS
#define __STDC_FORMAT_MACROS
#endif

#include <inttypes.h>

#include <algorithm>
#include <vector>

#include "db/builder.h"
#include "db/db_iter.h"
#include "db/dbformat.h"
#include "db/event_helpers.h"
#include "db/log_reader.h"
#include "db/log_writer.h"
#include "db/memtable.h"
#include "db/memtable_list.h"
#include "db/merge_context.h"
#include "db/range_tombstone_fragmenter.h"
#include "db/version_set.h"
#include "monitoring/iostats_context_imp.h"
#include "monitoring/perf_context_imp.h"
#include "monitoring/thread_status_util.h"
#include "port/port.h"
#include "rocksdb/db.h"
#include "rocksdb/env.h"
#include "rocksdb/statistics.h"
#include "rocksdb/status.h"
#include "rocksdb/table.h"
#include "rocksdb/terark_namespace.h"
#include "table/block.h"
#include "table/block_based_table_factory.h"
#include "table/merging_iterator.h"
#include "table/table_builder.h"
#include "table/two_level_iterator.h"
#include "util/c_style_callback.h"
#include "util/coding.h"
#include "util/event_logger.h"
#include "util/file_util.h"
#include "util/filename.h"
#include "util/log_buffer.h"
#include "util/logging.h"
#include "util/mutexlock.h"
#include "util/stop_watch.h"
#include "util/sync_point.h"

namespace TERARKDB_NAMESPACE {

const char* GetFlushReasonString(FlushReason flush_reason) {
  switch (flush_reason) {
    case FlushReason::kOthers:
      return "Other Reasons";
    case FlushReason::kGetLiveFiles:
      return "Get Live Files";
    case FlushReason::kShutDown:
      return "Shut down";
    case FlushReason::kExternalFileIngestion:
      return "External File Ingestion";
    case FlushReason::kManualCompaction:
      return "Manual Compaction";
    case FlushReason::kWriteBufferManager:
      return "Write Buffer Manager";
    case FlushReason::kWriteBufferFull:
      return "Write Buffer Full";
    case FlushReason::kTest:
      return "Test";
    case FlushReason::kDeleteFiles:
      return "Delete Files";
    case FlushReason::kAutoCompaction:
      return "Auto Compaction";
    case FlushReason::kManualFlush:
      return "Manual Flush";
    case FlushReason::kErrorRecovery:
      return "Error Recovery";
    case FlushReason::kInstallTimeout:
      return "Install Timeout";
    default:
      return "Invalid";
  }
}

FlushJob::FlushJob(
    const std::string& dbname, ColumnFamilyData* cfd,
    const ImmutableDBOptions& db_options,
    const MutableCFOptions& mutable_cf_options, const uint64_t* max_memtable_id,
    const EnvOptions& env_options, VersionSet* versions,
    InstrumentedMutex* db_mutex, std::atomic<bool>* shutting_down,
    std::vector<SequenceNumber> existing_snapshots,
    SequenceNumber earliest_write_conflict_snapshot,
    SnapshotChecker* snapshot_checker, JobContext* job_context,
    LogBuffer* log_buffer, Directory* db_directory,
    Directory* output_file_directory, CompressionType output_compression,
    Statistics* stats, EventLogger* event_logger, bool measure_io_stats,
    bool sync_output_directory, bool write_manifest, double flush_load)
    : dbname_(dbname),
      cfd_(cfd),
      db_options_(db_options),
      mutable_cf_options_(mutable_cf_options),
      max_memtable_id_(max_memtable_id),
      env_options_(env_options),
      versions_(versions),
      db_mutex_(db_mutex),
      shutting_down_(shutting_down),
      existing_snapshots_(std::move(existing_snapshots)),
      earliest_write_conflict_snapshot_(earliest_write_conflict_snapshot),
      snapshot_checker_(snapshot_checker),
      job_context_(job_context),
      log_buffer_(log_buffer),
      db_directory_(db_directory),
      output_file_directory_(output_file_directory),
      output_compression_(output_compression),
      stats_(stats),
      event_logger_(event_logger),
      measure_io_stats_(measure_io_stats),
      sync_output_directory_(sync_output_directory),
      write_manifest_(write_manifest),
      flush_load_(flush_load),
      edit_(nullptr),
      base_(nullptr),
      pick_memtable_called_(false),
      is_install_timeout_(false) {
  // Update the thread status to indicate flush.
  ReportStartedFlush();
  TEST_SYNC_POINT("FlushJob::FlushJob()");
}

FlushJob::~FlushJob() { ThreadStatusUtil::ResetThreadStatus(); }

void FlushJob::ReportStartedFlush() {
  ThreadStatusUtil::SetColumnFamily(cfd_, cfd_->ioptions()->env,
                                    db_options_.enable_thread_tracking);
  ThreadStatusUtil::SetThreadOperation(ThreadStatus::OP_FLUSH);
  ThreadStatusUtil::SetThreadOperationProperty(ThreadStatus::COMPACTION_JOB_ID,
                                               job_context_->job_id);
  IOSTATS_RESET(bytes_written);
}

void FlushJob::ReportFlushInputSize(const autovector<MemTable*>& mems) {
  uint64_t input_size = 0;
  for (auto* mem : mems) {
    input_size += mem->ApproximateMemoryUsage();
  }
  ThreadStatusUtil::IncreaseThreadOperationProperty(
      ThreadStatus::FLUSH_BYTES_MEMTABLES, input_size);
}

void FlushJob::RecordFlushIOStats() {
  RecordTick(stats_, FLUSH_WRITE_BYTES, IOSTATS(bytes_written));
  ThreadStatusUtil::IncreaseThreadOperationProperty(
      ThreadStatus::FLUSH_BYTES_WRITTEN, IOSTATS(bytes_written));
  IOSTATS_RESET(bytes_written);
}

void FlushJob::PickMemTable() {
  db_mutex_->AssertHeld();
  assert(!pick_memtable_called_);
  pick_memtable_called_ = true;
  // Save the contents of the earliest memtable as a new Table
  cfd_->imm()->PickMemtablesToFlush(max_memtable_id_, &mems_);
  if (mems_.empty()) {
    return;
  }

  ReportFlushInputSize(mems_);

  // entries mems are (implicitly) sorted in ascending order by their created
  // time. We will use the first memtable's `edit` to keep the meta info for
  // this flush.
  MemTable* m = mems_[0];
  edit_ = m->GetEdits();
  edit_->SetPrevLogNumber(0);
  // SetLogNumber(log_num) indicates logs with number smaller than log_num
  // will no longer be picked up for recovery.
  edit_->SetLogNumber(mems_.back()->GetNextLogNumber());
  edit_->SetColumnFamily(cfd_->GetID());

  // path 0 for level 0 file.
  assert(meta_.empty());
  meta_.emplace_back();
  meta_.front().fd = FileDescriptor(versions_->NewFileNumber(), 0, 0);

  base_ = cfd_->current();
  base_->Ref();  // it is likely that we do not need this reference
}

Status FlushJob::Run(LogsWithPrepTracker* prep_tracker) {
  TEST_SYNC_POINT("FlushJob::Start");
  db_mutex_->AssertHeld();
  assert(pick_memtable_called_);
  AutoThreadOperationStageUpdater stage_run(ThreadStatus::STAGE_FLUSH_RUN);
  if (mems_.empty()) {
    ROCKS_LOG_BUFFER(log_buffer_, "[%s] Nothing in memtable to flush",
                     cfd_->GetName().c_str());
    Status s;
    if (write_manifest_) {
      s = cfd_->imm()->TryInstallMemtableFlushResults(
          cfd_, mutable_cf_options_, mems_, prep_tracker, versions_, db_mutex_,
          0 /* file_number */, &job_context_->memtables_to_free, db_directory_,
          log_buffer_, kDefaultInstallMemtableTimeoutMicros);
      if (s.IsIncomplete() && s.subcode() == Status::kInstallTimeout) {
        is_install_timeout_ = true;
        return Status::OK();
      }
    }
    return s;
  }

  // I/O measurement variables
  PerfLevel prev_perf_level = PerfLevel::kEnableTime;
  uint64_t prev_write_nanos = 0;
  uint64_t prev_fsync_nanos = 0;
  uint64_t prev_range_sync_nanos = 0;
  uint64_t prev_prepare_write_nanos = 0;
  if (measure_io_stats_) {
    prev_perf_level = GetPerfLevel();
    SetPerfLevel(PerfLevel::kEnableTime);
    prev_write_nanos = IOSTATS(write_nanos);
    prev_fsync_nanos = IOSTATS(fsync_nanos);
    prev_range_sync_nanos = IOSTATS(range_sync_nanos);
    prev_prepare_write_nanos = IOSTATS(prepare_write_nanos);
  }

  // This will release and re-acquire the mutex.
  Status s = WriteLevel0Table();

  if (s.ok() &&
      (shutting_down_->load(std::memory_order_acquire) || cfd_->IsDropped())) {
    s = Status::ShutdownInProgress(
        "Database shutdown or Column family drop during flush");
  }

  if (!s.ok()) {
    cfd_->imm()->RollbackMemtableFlush(mems_, meta_[0].fd.GetNumber(), s);
  } else if (write_manifest_) {
    TEST_SYNC_POINT("FlushJob::InstallResults");
    // Replace immutable memtable with the generated Table
    s = cfd_->imm()->TryInstallMemtableFlushResults(
        cfd_, mutable_cf_options_, mems_, prep_tracker, versions_, db_mutex_,
        meta_[0].fd.GetNumber(), &job_context_->memtables_to_free,
        db_directory_, log_buffer_, kDefaultInstallMemtableTimeoutMicros);
    if (s.IsIncomplete() && s.subcode() == Status::kInstallTimeout) {
      is_install_timeout_ = true;
      s = Status::OK();
    }
  }

  RecordFlushIOStats();

  auto stream = event_logger_->LogToBuffer(log_buffer_);
  stream << "job" << job_context_->job_id << "cf_name" << cfd_->GetName()
         << "event"
         << "flush_finished"
         << "output_compression" << CompressionTypeToString(output_compression_)
         << "lsm_state";
  stream.StartArray();
  auto vstorage = cfd_->current()->storage_info();
  for (int level = 0; level < vstorage->num_levels(); ++level) {
    if (vstorage->LevelFiles(level).size() == 1 &&
        vstorage->LevelFiles(level).front()->prop.is_map_sst()) {
      stream << std::to_string(
          vstorage->LevelFiles(level).front()->prop.num_entries);
    } else {
      stream << vstorage->NumLevelFiles(level);
    }
  }
  stream.EndArray();
  stream << "immutable_memtables" << cfd_->imm()->NumNotFlushed();

  if (measure_io_stats_) {
    if (prev_perf_level != PerfLevel::kEnableTime) {
      SetPerfLevel(prev_perf_level);
    }
    stream << "file_write_nanos" << (IOSTATS(write_nanos) - prev_write_nanos);
    stream << "file_range_sync_nanos"
           << (IOSTATS(range_sync_nanos) - prev_range_sync_nanos);
    stream << "file_fsync_nanos" << (IOSTATS(fsync_nanos) - prev_fsync_nanos);
    stream << "file_prepare_write_nanos"
           << (IOSTATS(prepare_write_nanos) - prev_prepare_write_nanos);
  }

  return s;
}

void FlushJob::Cancel(const Status& s) {
  db_mutex_->AssertHeld();
  for (auto mem : mems_) {
    mem->GetEdits()->DoApplyCallback(s);
  }
  assert(base_ != nullptr);
  base_->Unref();
}

Status FlushJob::WriteLevel0Table() {
  AutoThreadOperationStageUpdater stage_updater(
      ThreadStatus::STAGE_FLUSH_WRITE_L0);
  db_mutex_->AssertHeld();
  const uint64_t start_micros = db_options_.env->NowMicros();
  Status s;
  {
    auto write_hint = cfd_->CalculateSSTWriteHint(0);
    db_mutex_->Unlock();
    if (log_buffer_) {
      log_buffer_->FlushBufferToLog();
    }
    // memtables and range_del_iters store internal iterators over each data
    // memtable and its associated range deletion memtable, respectively, at
    // corresponding indexes.
    ReadOptions ro;
    ro.total_order_seek = true;
    Arena arena;
    uint64_t total_num_entries = 0, total_num_deletes = 0;
    size_t total_memory_usage = 0;
    for (MemTable* m : mems_) {
      ROCKS_LOG_INFO(
          db_options_.info_log,
          "[%s] [JOB %d] Flushing memtable with next log file: %" PRIu64 "\n",
          cfd_->GetName().c_str(), job_context_->job_id, m->GetNextLogNumber());
      total_num_entries += m->num_entries();
      total_num_deletes += m->num_deletes();
      total_memory_usage += m->ApproximateMemoryUsage();
    }

    event_logger_->Log() << "job" << job_context_->job_id << "event"
                         << "flush_started"
                         << "num_memtables" << mems_.size() << "num_entries"
                         << total_num_entries << "num_deletes"
                         << total_num_deletes << "memory_usage"
                         << total_memory_usage << "flush_reason"
                         << GetFlushReasonString(cfd_->GetFlushReason());

    {
      ROCKS_LOG_INFO(db_options_.info_log,
                     "[%s] [JOB %d] Level-0 flush table #%" PRIu64 ": started",
                     cfd_->GetName().c_str(), job_context_->job_id,
                     meta_[0].fd.GetNumber());

      TEST_SYNC_POINT_CALLBACK("FlushJob::WriteLevel0Table:output_compression",
                               &output_compression_);
      int64_t _current_time = 0;
      auto status = db_options_.env->GetCurrentTime(&_current_time);
      // Safe to proceed even if GetCurrentTime fails. So, log and proceed.
      if (!status.ok()) {
        ROCKS_LOG_WARN(
            db_options_.info_log,
            "Failed to get current time to populate creation_time property. "
            "Status: %s",
            status.ToString().c_str());
      }
      const uint64_t current_time = static_cast<uint64_t>(_current_time);

      uint64_t oldest_key_time = mems_.front()->ApproximateOldestKeyTime();

      auto get_arena_input_iter = [&](Arena& arena) {
        auto memtables =
            new (arena.AllocateAligned(sizeof(std::vector<InternalIterator*>)))
                std::vector<InternalIterator*>();
        for (MemTable* m : mems_) {
          memtables->push_back(m->NewIterator(ro, &arena));
        }
        auto input =
            NewMergingIterator(&cfd_->internal_comparator(), memtables->data(),
                               static_cast<int>(memtables->size()), &arena);
        input->RegisterCleanup(
            [](void* arg1, void* /*arg2*/) {
              auto ptr =
                  reinterpret_cast<std::vector<InternalIterator*>*>(arg1);
              ptr->~vector();
            },
            memtables, nullptr);
        return input;
      };
      auto get_range_del_iters = [&] {
        std::vector<std::unique_ptr<FragmentedRangeTombstoneIterator>>
            range_del_iters;
        for (MemTable* m : mems_) {
          auto* range_del_iter =
              m->NewRangeTombstoneIterator(ro, kMaxSequenceNumber);
          if (range_del_iter != nullptr) {
            range_del_iters.emplace_back(range_del_iter);
          }
        }
        return range_del_iters;
      };
      s = BuildTable(
          dbname_, versions_, db_options_.env, *cfd_->ioptions(),
          mutable_cf_options_, env_options_, cfd_->table_cache(),
          c_style_callback(get_arena_input_iter), &get_arena_input_iter,
          c_style_callback(get_range_del_iters), &get_range_del_iters, &meta_,
          cfd_->internal_comparator(),
          cfd_->int_tbl_prop_collector_factories(mutable_cf_options_),
          cfd_->int_tbl_prop_collector_factories_for_blob(mutable_cf_options_),
          cfd_->GetID(), cfd_->GetName(), existing_snapshots_,
          earliest_write_conflict_snapshot_, snapshot_checker_,
          output_compression_, cfd_->ioptions()->compression_opts,
          mutable_cf_options_.paranoid_file_checks, cfd_->internal_stats(),
          TableFileCreationReason::kFlush, event_logger_, job_context_->job_id,
          Env::IO_HIGH, &table_properties_, 0 /* level */, flush_load_,
          current_time, oldest_key_time, write_hint);
      if (s.ok() && cfd_->ioptions()->ttl_extractor_factory != nullptr) {
        ROCKS_LOG_INFO(db_options_.info_log,
                       "FlushOutput earliest_time_begin_compact = %" PRIu64
                       ", latest_time_end_compact = %" PRIu64,
                       meta_[0].prop.earliest_time_begin_compact,
                       meta_[0].prop.latest_time_end_compact);
      }

      LogFlush(db_options_.info_log);
    }
    ROCKS_LOG_INFO_IF_OK(
        s, db_options_.info_log,
        "[%s] [JOB %d] Level-0 flush table #%" PRIu64 ": %" PRIu64
        " bytes %s%s",
        cfd_->GetName().c_str(), job_context_->job_id, meta_[0].fd.GetNumber(),
        meta_[0].fd.GetFileSize(), s.ToString().c_str(),
        meta_[0].marked_for_compaction ? " (needs compaction)" : "");

    if (s.ok() && output_file_directory_ != nullptr && sync_output_directory_) {
      s = output_file_directory_->Fsync();
    }
    TEST_SYNC_POINT("FlushJob::WriteLevel0Table");
    db_mutex_->Lock();
  }
  base_->Unref();

  // Note that if file_size is zero, the file has been deleted and
  // should not be added to the manifest.
  if (s.ok() && meta_[0].fd.GetFileSize() > 0) {
    // if we have more than 1 background thread, then we cannot
    // insert files directly into higher levels because some other
    // threads could be concurrently producing compacted files for
    // that key range.
    // Add file to L0
    for (size_t i = 0; i < meta_.size(); ++i) {
      auto& f = meta_[i];
      edit_->AddFile(i == 0 ? 0 : -1, f.fd.GetNumber(), f.fd.GetPathId(),
                     f.fd.GetFileSize(), f.smallest, f.largest,
                     f.fd.smallest_seqno, f.fd.largest_seqno,
                     f.marked_for_compaction, f.prop);
    }
  }

  // Note that here we treat flush as level 0 compaction in internal stats
  InternalStats::CompactionStats stats(CompactionReason::kFlush, 1);
  stats.micros = db_options_.env->NowMicros() - start_micros;
  for (auto& f : meta_) {
    stats.bytes_written += f.fd.GetFileSize();
    cfd_->internal_stats()->AddCFStats(InternalStats::BYTES_FLUSHED,
                                       f.fd.GetFileSize());
  }
  MeasureTime(stats_, FLUSH_TIME, stats.micros);
  cfd_->internal_stats()->AddCompactionStats(0 /* level */, stats);
  RecordFlushIOStats();
  return s;
}

}  // namespace TERARKDB_NAMESPACE

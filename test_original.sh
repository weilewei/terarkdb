#!/bin/bash

HOME=$PWD

#cd output && make -j40

cd $HOME

rm -rf /tmp/zenfs_nvme9n2

#./output/zenfs mkfs --zbd=nvme8n2 --aux_path=/tmp/zenfs_nvme8n2 --finish_threshold=5 --force

#gdb --args ./output/db_bench \

./output_original/db_bench \
  --zbd_path=nvme9n2 \
	--benchmarks=fillrandom \
	--use_existing_db=0 \
	--statistics=0 \
  --stats_per_interval=1 \
  --stats_interval_seconds=60 \
	--stats_dump_period_sec=1500 \
  --max_background_flushes=2 \
	--max_background_compactions=8 \
	--enable_lazy_compaction=0 \
	--level0_file_num_compaction_trigger=4 \
	--sync=1 \
	--allow_concurrent_memtable_write=1 \
	--bytes_per_sync=1048576 \
	--wal_bytes_per_sync=32768 \
	--delayed_write_rate=419430400 \
	--enable_write_thread_adaptive_yield=1 \
	--num_levels=7 \
	--key_size=36 \
	--value_size=16000 \
	--level_compaction_dynamic_level_bytes=true \
	--mmap_read=false \
	--compression_type=zstd \
	--memtablerep=skip_list \
	--use_terark_table=false \
	--blob_size=1024 \
	--blob_gc_ratio=0.0625 \
	--write_buffer_size=268435456 \
	--max_write_buffer_number=20 \
	--target_file_size_base=268435456 \
	--target_blob_file_size=268435456 \
	--blob_file_defragment_size=33554432 \
	--max_dependence_blob_overlap=128 \
	--optimize_filters_for_hits=true \
	--optimize_range_deletion=true \
	--num=12000000 \
	--db=test_db_1 \
	--benchmark_write_rate_limit=200000000 \
	--threads=16 \
	--batch_size=1 \
	--prepare_log_writer_num=0 \
	--num_column_families=5 \
	--zenfs_gc_ratio=0.3

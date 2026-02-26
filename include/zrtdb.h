// ZRTDB（Zero-copy Real-Time Data Bus）
// Copyright (c) 2026 邹德虎 （Zou Dehu）
// SPDX-License-Identifier: Apache-2.0

// ZRTDB应用程序接口

#ifndef _ZRTDB_H_
#define _ZRTDB_H_

#ifdef __cplusplus
extern "C" {
#endif

int RegisterApp_(const char* app_name); // 注册应用程序

int MapMemory_(const char* part_nm, char** part_addr); // 映射内存分区

int free_MapMemory_(); // 释放映射的应用程序

// 协作式“写入批次”读锁：写者在一轮写入前后调用，快照/下装会持写锁短暂停写入。
// 返回 1 成功，-1 失败。
int SnapshotReadLock_();
int SnapshotReadUnlock_();

// 保存快照。out_path 可为 NULL；若非 NULL 会写入创建的快照目录完整路径。
// 返回 1 成功，-1 失败。
int SaveSnapshot_(char* out_path, int out_len);

// 从快照目录下装到 /var/ZRTDB/<APP>/...。
// snapshot_name_or_path：可以是相对名（例如 CONTROLER_2026...）或绝对路径。
// 返回 1 成功，-1 失败。
int LoadSnapshot_(const char* snapshot_name_or_path);

#ifdef __cplusplus
}
#endif

#endif

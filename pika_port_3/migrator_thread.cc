#include "migrator_thread.h"
#include "const.h"

#include <unistd.h>

#include <vector>
#include <functional>

#include "blackwidow/blackwidow.h"
#include "src/redis_strings.h"
#include "src/redis_lists.h"
#include "src/redis_hashes.h"
#include "src/redis_sets.h"
#include "src/redis_zsets.h"
#include "src/scope_snapshot.h"
#include "src/strings_value_format.h"

#include "log.h"

const int64_t MAX_BATCH_NUM = 30000;

MigratorThread::~MigratorThread() {
}

void MigratorThread::MigrateStringsDB() {
  blackwidow::RedisStrings* db = (blackwidow::RedisStrings*)(db_);

  rocksdb::ReadOptions iterator_options;
  const rocksdb::Snapshot* snapshot;
  rocksdb::DB* rocksDB = db->GetDB();
  blackwidow::ScopeSnapshot ss(rocksDB, &snapshot);
  iterator_options.snapshot = snapshot;
  iterator_options.fill_cache = false;
  int64_t curtime;
  if (!rocksDB->GetEnv()->GetCurrentTime(&curtime).ok()) {
    pwarn("failed to get current time by db->GetEnv()->GetCurrentTime()");
    return;
  }

  auto iter = rocksDB->NewIterator(iterator_options);
  for (iter->SeekToFirst(); !should_exit_ && iter->Valid(); iter->Next()) {
    blackwidow::ParsedStringsValue parsed_strings_value(iter->value());
    int32_t ttl = 0;
    int64_t ts = (int64_t)(parsed_strings_value.timestamp());
    if (ts != 0) {
      int64_t diff = ts - curtime;
      ttl = diff > 0 ? diff : -1;
      if (ttl < 0) {
        continue;
      }
    }

    // printf("[key : %-30s] [value : %-30s] [timestamp : %-10d] [version : %d] [survival_time : %d]\n",
    //   iter->key().ToString().c_str(),
    //   parsed_strings_value.value().ToString().c_str(),
    //   parsed_strings_value.timestamp(),
    //   parsed_strings_value.version(),
    //   ttl);
    // sleep(3);

    pink::RedisCmdArgsType argv;
    std::string cmd;

    argv.push_back("SET");
    argv.push_back(iter->key().ToString().c_str());
    argv.push_back(parsed_strings_value.value().ToString().c_str());
    if (ts != 0 && ttl > 0) {
      argv.push_back("EX");
      argv.push_back(std::to_string(ttl));
    }

    pink::SerializeRedisCommand(argv, &cmd);
    PlusNum();
    // DispatchKey(cmd, iter->key().ToString());
    DispatchKey(cmd);
  }
  delete iter;
}

void MigratorThread::MigrateListsDB() {
  blackwidow::RedisLists* db = (blackwidow::RedisLists*)(db_);

  // std::vector<std::string> keys;
  // blackwidow::Status s = db->ScanKeys(pattern, &keys);
  // if (!s.ok()) {
  //   pfatal("db->ScanKeys(pattern:*) = %s", s.ToString().c_str());
  //   return;
  // }
  std::string start_key;
  std::string next_key;
  std::string pattern("*");
  int64_t batch_count = g_conf.sync_batch_num * 10;
  if (MAX_BATCH_NUM < batch_count) {
    if (g_conf.sync_batch_num < MAX_BATCH_NUM) {
      batch_count = MAX_BATCH_NUM;
    } else {
      batch_count = g_conf.sync_batch_num * 2;
    }
  }

  bool fin = false;
  while (!fin) {
    int64_t count = batch_count;
    std::vector<std::string> keys;
    fin = db->Scan(start_key, pattern, &keys, &count, &next_key);
    // pinfo("batch count %d, fin:%d, keys.size():%zu, next_key:%s\n", count, fin, keys.size(), next_key.c_str());
    if (fin && keys.size() == 0) {
      break;
    }
    start_key = next_key;

    for (auto k : keys) {
      if (should_exit_) {
        break;
      }

      int64_t pos = 0;
      std::vector<std::string> list;
      blackwidow::Status s = db->LRange(k, pos, pos + g_conf.sync_batch_num - 1, &list);
      if (!s.ok()) {
        pwarn("db->LRange(key:%s, pos:%lld, batch size:%zu) = %s",
          k.c_str(), pos, g_conf.sync_batch_num, s.ToString().c_str());
        continue;
      }

      while (s.ok() && !should_exit_ && !list.empty()) {
        pink::RedisCmdArgsType argv;
        std::string cmd;

        argv.push_back("RPUSH");
        argv.push_back(k);
        for (auto e : list) {
          // PlusNum();
          argv.push_back(e);
        }

        pink::SerializeRedisCommand(argv, &cmd);

        PlusNum();
        DispatchKey(cmd, k);

        pos += g_conf.sync_batch_num;
        list.clear();
        s = db->LRange(k, pos, pos + g_conf.sync_batch_num - 1, &list);
        if (!s.ok()) {
          pwarn("db->LRange(key:%s, pos:%lld, batch size:%zu) = %s",
            k.c_str(), pos, g_conf.sync_batch_num, s.ToString().c_str());
        }
      }

      int64_t ttl = -1;
      s = db->TTL(k, &ttl);
      if (s.ok() && ttl > 0) {
        pink::RedisCmdArgsType argv;
        std::string cmd;

        argv.push_back("EXPIRE");
        argv.push_back(k);
        argv.push_back(std::to_string(ttl));
        pink::SerializeRedisCommand(argv, &cmd);
        PlusNum();
        DispatchKey(cmd);
      }
    } // for
  } // while
}

void MigratorThread::MigrateHashesDB() {
  blackwidow::RedisHashes* db = (blackwidow::RedisHashes*)(db_);

  // std::vector<std::string> keys;
  // std::string pattern("*");
  // blackwidow::Status s = db->ScanKeys(pattern, &keys);
  // if (!s.ok()) {
  //   pfatal("db->ScanKeys(pattern:*) = %s", s.ToString().c_str());
  //   return;
  // }

  std::string start_key;
  std::string next_key;
  std::string pattern("*");
  int64_t batch_count = g_conf.sync_batch_num * 10;
  if (MAX_BATCH_NUM < batch_count) {
    if (g_conf.sync_batch_num < MAX_BATCH_NUM) {
      batch_count = MAX_BATCH_NUM;
    } else {
      batch_count = g_conf.sync_batch_num * 2;
    }
  }
  bool fin = false;
  while (!fin) {
    int64_t count = batch_count;
    std::vector<std::string> keys;
    fin = db->Scan(start_key, pattern, &keys, &count, &next_key);
    if (fin && keys.size() == 0) {
      break;
    }
    start_key = next_key;

    for (auto k : keys) {
      if (should_exit_) {
        break;
      }
      std::vector<blackwidow::FieldValue> fvs;
      blackwidow::Status s = db->HGetall(k, &fvs);
      if (!s.ok()) {
        pwarn("db->HGetall(key:%s) = %s", k.c_str(), s.ToString().c_str());
        continue;
      }

      auto it = fvs.begin();
      while (!should_exit_ && it != fvs.end()) {
        pink::RedisCmdArgsType argv;
        std::string cmd;

        argv.push_back("HMSET");
        argv.push_back(k);
        for (size_t idx = 0;
             idx < g_conf.sync_batch_num && !should_exit_ && it != fvs.end();
             idx ++, it ++) {
          argv.push_back(it->field);
          argv.push_back(it->value);
          // PlusNum();
        }

        pink::SerializeRedisCommand(argv, &cmd);
        PlusNum();
        // DispatchKey(cmd, k);
        DispatchKey(cmd);
      }

      int64_t ttl = -1;
      s = db->TTL(k, &ttl);
      if (s.ok() && ttl > 0) {
        pink::RedisCmdArgsType argv;
        std::string cmd;

        argv.push_back("EXPIRE");
        argv.push_back(k);
        argv.push_back(std::to_string(ttl));
        pink::SerializeRedisCommand(argv, &cmd);
        PlusNum();
        DispatchKey(cmd);
      }
    } // for
  } // while
}

void MigratorThread::MigrateSetsDB() {
  blackwidow::RedisSets* db = (blackwidow::RedisSets*)(db_);

  // std::vector<std::string> keys;
  // std::string pattern("*");
  // blackwidow::Status s = db->ScanKeys(pattern, &keys);
  // if (!s.ok()) {
  //   pfatal("db->ScanKeys(pattern:*) = %s", s.ToString().c_str());
  //   return;
  // }

  std::string start_key;
  std::string next_key;
  std::string pattern("*");
  int64_t batch_count = g_conf.sync_batch_num * 10;
  if (MAX_BATCH_NUM < batch_count) {
    if (g_conf.sync_batch_num < MAX_BATCH_NUM) {
      batch_count = MAX_BATCH_NUM;
    } else {
      batch_count = g_conf.sync_batch_num * 2;
    }
  }
  bool fin = false;
  while (!fin) {
    int64_t count = batch_count;
    std::vector<std::string> keys;
    fin = db->Scan(start_key, pattern, &keys, &count, &next_key);
    if (fin && keys.size() == 0) {
      break;
    }
    start_key = next_key;

    for (auto k : keys) {
      if (should_exit_) {
        break;
      }
      std::vector<std::string> members;
      blackwidow::Status s = db->SMembers(k, &members);
      if (!s.ok()) {
        pwarn("db->SMembers(key:%s) = %s", k.c_str(), s.ToString().c_str());
        continue;
      }
      auto it = members.begin();
      while (!should_exit_ && it != members.end()) {
        std::string cmd;
        pink::RedisCmdArgsType argv;

        argv.push_back("SADD");
        argv.push_back(k);
        for (size_t idx = 0;
             idx < g_conf.sync_batch_num && !should_exit_ && it != members.end();
             idx ++, it ++) {
          argv.push_back(*it);
        }

        pink::SerializeRedisCommand(argv, &cmd);
        PlusNum();
        // DispatchKey(cmd, k);
        DispatchKey(cmd);
      }

      int64_t ttl = -1;
      s = db->TTL(k, &ttl);
      if (s.ok() && ttl > 0) {
        pink::RedisCmdArgsType argv;
        std::string cmd;

        argv.push_back("EXPIRE");
        argv.push_back(k);
        argv.push_back(std::to_string(ttl));
        pink::SerializeRedisCommand(argv, &cmd);
        PlusNum();
        DispatchKey(cmd);
      }
    } // for
  } // while
}

void MigratorThread::MigrateZsetsDB() {
  blackwidow::RedisZSets* db = (blackwidow::RedisZSets*)(db_);

  // std::vector<std::string> keys;
  // std::string pattern("*");
  // blackwidow::Status s = db->ScanKeys(pattern, &keys);
  // if (!s.ok()) {
  //   pfatal("db->ScanKeys(pattern:*) = %s", s.ToString().c_str());
  //   return;
  // }

  std::string start_key;
  std::string next_key;
  std::string pattern("*");
  int64_t batch_count = g_conf.sync_batch_num * 10;
  if (MAX_BATCH_NUM < batch_count) {
    if (g_conf.sync_batch_num < MAX_BATCH_NUM) {
      batch_count = MAX_BATCH_NUM;
    } else {
      batch_count = g_conf.sync_batch_num * 2;
    }
  }
  bool fin = false;
  while (!fin) {
    int64_t count = batch_count;
    std::vector<std::string> keys;
    fin = db->Scan(start_key, pattern, &keys, &count, &next_key);
    if (fin && keys.size() == 0) {
      break;
    }
    start_key = next_key;

    for (auto k : keys) {
      if (should_exit_) {
        break;
      }
      std::vector<blackwidow::ScoreMember> score_members;
      blackwidow::Status s = db->ZRange(k, 0, -1, &score_members);
      if (!s.ok()) {
        pwarn("db->ZRange(key:%s) = %s", k.c_str(), s.ToString().c_str());
        continue;
      }
      auto it = score_members.begin();
      while (!should_exit_ && it != score_members.end()) {
        pink::RedisCmdArgsType argv;
        std::string cmd;

        argv.push_back("ZADD");
        argv.push_back(k);

        for (size_t idx = 0;
             idx < g_conf.sync_batch_num && !should_exit_ && it != score_members.end();
             idx ++, it ++) {
          argv.push_back(std::to_string(it->score));
          argv.push_back(it->member);
        }

        pink::SerializeRedisCommand(argv, &cmd);
        PlusNum();
        // DispatchKey(cmd, k);
        DispatchKey(cmd);
      }

      int64_t ttl = -1;
      s = db->TTL(k, &ttl);
      if (s.ok() && ttl > 0) {
        pink::RedisCmdArgsType argv;
        std::string cmd;

        argv.push_back("EXPIRE");
        argv.push_back(k);
        argv.push_back(std::to_string(ttl));
        pink::SerializeRedisCommand(argv, &cmd);
        PlusNum();
        DispatchKey(cmd);
      }
    } // for
  } // while
}

void MigratorThread::MigrateDB() {
  switch (int(type_)) {
    case int(blackwidow::kStrings) : {
      MigrateStringsDB();
      break;
    }

    case int(blackwidow::kLists) : {
      MigrateListsDB();
      break;
    }

    case int(blackwidow::kHashes) : {
      MigrateHashesDB();
      break;
    }

    case int(blackwidow::kSets) : {
      MigrateSetsDB();
      break;
    }

    case int(blackwidow::kZSets) : {
      MigrateZsetsDB();
      break;
    }

    default: {
      perror("illegal db type %d", type_);
      break;
    }
  }
}

void MigratorThread::DispatchKey(const std::string &command, const std::string& key) {
  thread_index_ = (thread_index_ + 1) % thread_num_;
  size_t idx = thread_index_;
  if (key.size()) { // no empty
    idx = std::hash<std::string>()(key) % thread_num_;
  }
  (*senders_)[idx]->LoadKey(command);
}

void *MigratorThread::ThreadMain() {
  MigrateDB();
  should_exit_ = true;

  pinfo("%s keys have been dispatched completly", GetDBTypeString(type_));
  return NULL;
}


// Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "object_store_impl.h"

#include <limits>
#include <map>
#include <string>
#include <vector>

#include <base/logging.h>
#include <boost/filesystem/operations.hpp>
#include <brillo/secure_blob.h>
#include <leveldb/db.h>
#include <leveldb/env.h>
#ifndef NO_MEMENV
#include <leveldb/helpers/memenv.h>
#endif
#ifndef NO_METRICS
#include <metrics/metrics_library.h>
#endif

#include "p11net_utility.h"
#include "pkcs11/cryptoki.h"

using boost::filesystem::path;
using brillo::SecureBlob;
using std::map;
using std::string;
using std::vector;

namespace {

// Encapsulate UMA event generation.
class MetricsWrapper {
 public:
  MetricsWrapper() {
#ifndef NO_METRICS
    metrics_.Init();
#endif
  }

  bool SendUMAEvent(const std::string& event) {
#ifndef NO_METRICS
    return metrics_.SendCrosEventToUMA(event);
#else
    return false;
#endif
  }

 private:
#ifndef NO_METRICS
  MetricsLibrary metrics_;
#endif
};

}  // namespace

namespace p11net {

const char ObjectStoreImpl::kInternalBlobKeyPrefix[] = "InternalBlob";
const char ObjectStoreImpl::kPublicBlobKeyPrefix[] = "PublicBlob";
const char ObjectStoreImpl::kPrivateBlobKeyPrefix[] = "PrivateBlob";
const char ObjectStoreImpl::kBlobKeySeparator[] = "&";
const char ObjectStoreImpl::kDatabaseVersionKey[] = "DBVersion";
const char ObjectStoreImpl::kIDTrackerKey[] = "NextBlobID";
const int ObjectStoreImpl::kAESKeySizeBytes = 32;
const int ObjectStoreImpl::kHMACSizeBytes = 64;
const char ObjectStoreImpl::kDatabaseDirectory[] = "database";
const char ObjectStoreImpl::kCorruptDatabaseDirectory[] = "database_corrupt";
const char ObjectStoreImpl::kObfuscationKey[] = {
    '\x6f', '\xaa', '\x0a', '\xb6', '\x10', '\xc0', '\xa6', '\xe4', '\x07',
    '\x8b', '\x05', '\x1c', '\xd2', '\x8b', '\xac', '\x2d', '\xba', '\x5e',
    '\x14', '\x9c', '\xae', '\x57', '\xfb', '\x04', '\x13', '\x92', '\xc0',
    '\x84', '\x2a', '\xea', '\xf6', '\xfb'};
const int ObjectStoreImpl::kBlobVersion = 1;

ObjectStoreImpl::ObjectStoreImpl() {}

ObjectStoreImpl::~ObjectStoreImpl() {}

bool ObjectStoreImpl::Init(const boost::filesystem::path& database_path) {
  MetricsWrapper metrics;

  LOG(INFO) << "Opening database in: " << database_path;
  leveldb::Options options;
  options.create_if_missing = true;
  options.paranoid_checks = true;
  if (database_path == ":memory:") {
#ifndef NO_MEMENV
    // Memory only environment, useful for testing.
    LOG(INFO) << "Using memory-only environment.";
    env_.reset(leveldb::NewMemEnv(leveldb::Env::Default()));
    options.env = env_.get();
#else
    LOG(ERROR) << "Compiled without memory-only environment support.";
    return false;
#endif
  }
  boost::filesystem::path database_name(database_path);
  database_name.append(kDatabaseDirectory);
  leveldb::DB* db = NULL;
  leveldb::Status status = leveldb::DB::Open(options,
                                             database_name.string(),
                                             &db);
  if (!status.ok()) {
    LOG(ERROR) << "Failed to open database: " << status.ToString();
    metrics.SendUMAEvent("P11Net.DatabaseCorrupted");
    LOG(WARNING) << "Attempting to repair database.";
    status = leveldb::RepairDB(database_name.string(), leveldb::Options());
    if (status.ok())
      status = leveldb::DB::Open(options, database_name.string(), &db);
  }

  if (!status.ok()) {
    LOG(ERROR) << "Failed to repair database: " << status.ToString();
    metrics.SendUMAEvent("P11Net.DatabaseRepairFailure");
    // We don't want to risk using a database that has been corrupted. Recreate
    // the database from scratch but save the corrupted database for diagnostic
    // purposes.
    LOG(WARNING) << "Recreating database from scratch. Moving current database "
                 << "to " << kCorruptDatabaseDirectory;
    boost::filesystem::path corrupt_db_path(database_path);
    corrupt_db_path.append(kCorruptDatabaseDirectory);
    boost::filesystem::remove(corrupt_db_path);
    boost::filesystem::rename(database_name, corrupt_db_path);
    status = leveldb::DB::Open(options, database_name.string(), &db);
  }

  if (!status.ok()) {
    LOG(ERROR) << "Failed to create new database: " << status.ToString();
    metrics.SendUMAEvent("P11Net.DatabaseCreateFailure");
    return false;
  }

  db_.reset(db);
  int version = 0;
  if (!ReadInt(kDatabaseVersionKey, &version)) {
    if (!WriteInt(kIDTrackerKey, 1)) {
      LOG(ERROR) << "Failed to initialize the blob ID tracker.";
      return false;
    }
    if (!WriteInt(kDatabaseVersionKey, 1)) {
      LOG(ERROR) << "Failed to initialize the database version.";
      return false;
    }
  }
  return true;
}

bool ObjectStoreImpl::GetInternalBlob(int blob_id, string* blob) {
  if (!ReadBlob(CreateBlobKey(kInternal, blob_id), blob)) {
    // Don't log this since it happens legitimately when a blob has not yet been
    // set.
    return false;
  }
  return true;
}

bool ObjectStoreImpl::SetInternalBlob(int blob_id, const string& blob) {
  if (!WriteBlob(CreateBlobKey(kInternal, blob_id), blob)) {
    LOG(ERROR) << "Failed to write internal blob: " << blob_id;
    return false;
  }
  return true;
}

bool ObjectStoreImpl::SetEncryptionKey(const SecureBlob& key) {
  if (key.size() != static_cast<size_t>(kAESKeySizeBytes)) {
    LOG(ERROR) << "Unexpected key size: " << key.size();
    return false;
  }
  key_ = key;
  return true;
}

bool ObjectStoreImpl::InsertObjectBlob(const ObjectBlob& blob,
                                       int* handle) {
  if (blob.is_private && key_.empty()) {
    LOG(ERROR) << "The store encryption key has not been initialized.";
    return false;
  }
  if (!GetNextID(handle)) {
    LOG(ERROR) << "Failed to generate blob identifier.";
    return false;
  }
  blob_type_map_[*handle] = blob.is_private ? kPrivate : kPublic;
  return UpdateObjectBlob(*handle, blob);
}

bool ObjectStoreImpl::DeleteObjectBlob(int handle) {
  leveldb::WriteOptions options;
  options.sync = true;
  string db_key = CreateBlobKey(GetBlobType(handle), handle);
  leveldb::Status status = db_->Delete(options, db_key);
  if (!status.ok()) {
    LOG(ERROR) << "Failed to delete blob: " << status.ToString();
    return false;
  }
  return true;
}

bool ObjectStoreImpl::DeleteAllObjectBlobs() {
  vector<string> blobs_to_delete;
  std::unique_ptr<leveldb::Iterator>
      it(db_->NewIterator(leveldb::ReadOptions()));
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    BlobType type;
    int id = 0;
    if (ParseBlobKey(it->key().ToString(), &type, &id) && type != kInternal)
      blobs_to_delete.push_back(it->key().ToString());
  }
  bool deleted_all = true;
  for (size_t i = 0; i < blobs_to_delete.size(); ++i) {
    leveldb::WriteOptions options;
    options.sync = true;
    leveldb::Status status = db_->Delete(options, blobs_to_delete[i]);
    if (!status.ok()) {
      LOG(ERROR) << "Failed to delete blob: " << status.ToString();
      deleted_all = false;
    }
  }
  return deleted_all;
}

bool ObjectStoreImpl::UpdateObjectBlob(int handle, const ObjectBlob& blob) {
  ObjectBlob encrypted_blob;
  BlobType type = GetBlobType(handle);
  if (blob.is_private != (type == kPrivate)) {
    LOG(ERROR) << "Object privacy mismatch.";
    return false;
  }
  if (!Encrypt(blob, &encrypted_blob)) {
    LOG(ERROR) << "Failed to encrypt object blob.";
    return false;
  }
  if (!WriteBlob(CreateBlobKey(type, handle), encrypted_blob.blob)) {
    LOG(ERROR) << "Failed to write object blob.";
  }
  return true;
}

bool ObjectStoreImpl::LoadPublicObjectBlobs(map<int, ObjectBlob>* blobs) {
  return LoadObjectBlobs(kPublic, blobs);
}

bool ObjectStoreImpl::LoadPrivateObjectBlobs(map<int, ObjectBlob>* blobs) {
  if (key_.empty()) {
    LOG(ERROR) << "The store encryption key has not been initialized.";
    return false;
  }
  return LoadObjectBlobs(kPrivate, blobs);
}

bool ObjectStoreImpl::LoadObjectBlobs(BlobType type,
                                      map<int, ObjectBlob>* blobs) {
  std::unique_ptr<leveldb::Iterator>
      it(db_->NewIterator(leveldb::ReadOptions()));
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    BlobType it_type;
    int id = 0;
    if (ParseBlobKey(it->key().ToString(), &it_type, &id) && type == it_type) {
      ObjectBlob encrypted_blob;
      encrypted_blob.is_private = (type == kPrivate);
      encrypted_blob.blob = it->value().ToString();
      ObjectBlob blob;
      if (!Decrypt(encrypted_blob, &blob)) {
        LOG(WARNING) << "Failed to decrypt object blob.";
        continue;
      }
      (*blobs)[id] = blob;
      blob_type_map_[id] = type;
    }
  }
  return true;
}

bool ObjectStoreImpl::Encrypt(const ObjectBlob& plain_text,
                              ObjectBlob* cipher_text) {
  if (plain_text.is_private && key_.empty()) {
    LOG(ERROR) << "The store encryption key has not been initialized.";
    return false;
  }
  cipher_text->is_private = plain_text.is_private;
  SecureBlob obfuscation_key(std::begin(kObfuscationKey),
                             std::end(kObfuscationKey));
  SecureBlob& key = plain_text.is_private ? key_ : obfuscation_key;
  string cipher_text_no_hmac;
  if (!RunCipher(true, key, string(), plain_text.blob, &cipher_text_no_hmac))
    return false;
  // Prepend a version header and include it in the MAC.
  string version_header(1, static_cast<char>(kBlobVersion));
  cipher_text->blob = AppendHMAC(version_header + cipher_text_no_hmac, key);
  return true;
}

bool ObjectStoreImpl::Decrypt(const ObjectBlob& cipher_text,
                              ObjectBlob* plain_text) {
  if (cipher_text.is_private && key_.empty()) {
    LOG(ERROR) << "The store encryption key has not been initialized.";
    return false;
  }
  plain_text->is_private = cipher_text.is_private;
  SecureBlob obfuscation_key(std::begin(kObfuscationKey),
                             std::end(kObfuscationKey));
  SecureBlob& key = cipher_text.is_private ? key_ : obfuscation_key;
  string cipher_text_no_hmac;
  if (!VerifyAndStripHMAC(cipher_text.blob,
                          key,
                          &cipher_text_no_hmac))
    return false;
  // Check and strip the version header.
  int version = static_cast<int>(cipher_text_no_hmac[0]);
  if (version != kBlobVersion) {
    LOG(ERROR) << "Blob found with unknown version.";
    return false;
  }
  cipher_text_no_hmac = cipher_text_no_hmac.substr(1);
  return RunCipher(false,
                   key,
                   string(),
                   cipher_text_no_hmac,
                   &plain_text->blob);
}

string ObjectStoreImpl::AppendHMAC(const string& input, const SecureBlob& key) {
  return input + HmacSha512(input, key);
}

bool ObjectStoreImpl::VerifyAndStripHMAC(const string& input,
                                         const SecureBlob& key,
                                         string* stripped) {
  if (input.size() < static_cast<size_t>(kHMACSizeBytes)) {
    LOG(ERROR) << "Failed to verify blob integrity.";
    return false;
  }
  *stripped = input.substr(0, input.size() - kHMACSizeBytes);
  string hmac = input.substr(input.size() - kHMACSizeBytes);
  string computed_hmac = HmacSha512(*stripped, key);
  if ((hmac.size() != computed_hmac.size()) ||
      (0 != brillo::SecureMemcmp(hmac.data(),
                                   computed_hmac.data(),
                                   hmac.size()))) {
    LOG(ERROR) << "Failed to verify blob integrity.";
    return false;
  }
  return true;
}

string ObjectStoreImpl::CreateBlobKey(BlobType type, int blob_id) {
  const char* prefix = NULL;
  switch (type) {
    case kInternal:
      prefix = kInternalBlobKeyPrefix;
      break;
    case kPublic:
      prefix = kPublicBlobKeyPrefix;
      break;
    case kPrivate:
      prefix = kPrivateBlobKeyPrefix;
      break;
    default:
      LOG(FATAL) << "Invalid enum value.";
  }
  std::ostringstream result;
  result << prefix << kBlobKeySeparator << blob_id;
  return result.str();
}

bool ObjectStoreImpl::ParseBlobKey(const string& key,
                                   BlobType* type,
                                   int* blob_id) {
  size_t index = key.rfind(kBlobKeySeparator);
  if (index == string::npos) {
    // This isn't a blob.
    return false;
  }
  auto key_piece = key.substr(index + 1);
  *blob_id = std::stoi(key_piece);
  string prefix = key.substr(0, index);
  if (prefix == kInternalBlobKeyPrefix) {
    *type = kInternal;
  } else if (prefix == kPublicBlobKeyPrefix) {
    *type = kPublic;
  } else if (prefix == kPrivateBlobKeyPrefix) {
    *type = kPrivate;
  } else {
    LOG(ERROR) << "Invalid blob key prefix: " << key;
    return false;
  }
  return true;
}

bool ObjectStoreImpl::GetNextID(int* next_id) {
  if (!ReadInt(kIDTrackerKey, next_id)) {
    LOG(ERROR) << "Failed to read ID tracker.";
    return false;
  }
  if (*next_id == std::numeric_limits<int>::max()) {
    LOG(ERROR) << "Object ID overflow.";
    return false;
  }
  if (!WriteInt(kIDTrackerKey, *next_id + 1)) {
    LOG(ERROR) << "Failed to write ID tracker.";
    return false;
  }
  return true;
}

bool ObjectStoreImpl::ReadBlob(const string& key, string* value) {
  leveldb::Status status = db_->Get(leveldb::ReadOptions(), key, value);
  if (!status.ok()) {
    if (!status.IsNotFound())
      LOG(ERROR) << "Failed to read value from database: " << status.ToString();
    return false;
  }
  return true;
}

bool ObjectStoreImpl::ReadInt(const string& key, int* value) {
  string value_string;
  if (!ReadBlob(key, &value_string))
    return false;
  *value = std::stoi(value_string);
  return true;
}

bool ObjectStoreImpl::WriteBlob(const string& key, const string& value) {
  leveldb::WriteOptions options;
  options.sync = true;
  leveldb::Status status = db_->Put(options, key, value);
  if (!status.ok()) {
    LOG(ERROR) << "Failed to write value to database: " << status.ToString();
    return false;
  }
  return true;
}

bool ObjectStoreImpl::WriteInt(const string& key, int value) {
  return WriteBlob(key, std::to_string(value));
}

ObjectStoreImpl::BlobType ObjectStoreImpl::GetBlobType(int blob_id) {
  map<int, BlobType>::iterator it = blob_type_map_.find(blob_id);
  if (it == blob_type_map_.end())
    return kInternal;
  return it->second;
}

}  // namespace p11net

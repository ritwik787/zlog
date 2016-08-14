#ifndef ZLOG_KVSTORE_BACKEND_H
#define ZLOG_KVSTORE_BACKEND_H
#include <mutex>
#include "zlog/log.h"

class Backend {
 public:
  virtual int Append(const std::string& data, uint64_t *pos) = 0;
  virtual int Tail(uint64_t *pos) = 0;
  virtual int Read(std::string *data, uint64_t pos) = 0;
};

class ZLogBackend : public Backend {
 public:
  explicit ZLogBackend(zlog::Log *log) : log_(log)
  {}

  virtual int Append(const std::string& data, uint64_t *pos) {
    ceph::bufferlist bl;
    bl.append(data.c_str(), data.size());
    return log_->Append(bl, pos);
  }

  virtual int Tail(uint64_t *pos) {
    return log_->CheckTail(pos);
  }

  virtual int Read(std::string *data, uint64_t pos) {
    ceph::bufferlist bl;
    int ret = log_->Read(pos, bl);
    if (ret)
      return ret;
    std::string out(bl.c_str(), bl.length());
    *data = out;
    return 0;
  }

 private:
  zlog::Log *log_;
};

class VectorBackend : public Backend {
 public:
  virtual int Append(const std::string& data, uint64_t *pos) {
    std::lock_guard<std::mutex> l(lock_);
    log_.push_back(data);
    *pos = log_.size() - 1;
    return 0;
  }

  virtual int Tail(uint64_t *pos) {
    std::lock_guard<std::mutex> l(lock_);
    *pos = log_.size();
    return 0;
  }

  virtual int Read(std::string *data, uint64_t pos) {
    std::lock_guard<std::mutex> l(lock_);
    *data = log_.at(pos);
    return 0;
  }

 private:
  std::vector<std::string> log_;
  std::mutex lock_;
};

#endif
#ifndef __STREAMING_READ_PARSER__
#define __STREAMING_READ_PARSER__

#include "fcntl.h"
#include "unistd.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <vector>

extern "C" {
#include "kseq.h"
}

#include "concurrentqueue.h"

struct ReadSeq {
  char* seq = nullptr;
  size_t len = 0;
  char* name = nullptr;
  size_t nlen = 0;
};

struct ReadPair {
  ReadSeq first;
  ReadSeq second;
};

template <typename T> class ReadChunk {
public:
  ReadChunk(size_t want = 1000) : group_(want), want_(want), have_(want) {}
  inline void have(size_t num) { have_ = num; }
  inline size_t size() { return have_; }
  inline size_t want() const { return want_; }
  T& operator[](size_t i) { return group_[i]; }
  typename std::vector<T>::iterator begin() { return group_.begin(); }
  typename std::vector<T>::iterator end() { return group_.begin() + have_; }

private:
  std::vector<T> group_;
  size_t want_;
  size_t have_;
};

template <typename T> class ReadGroup {
public:
  ReadGroup(moodycamel::ProducerToken&& pt, moodycamel::ConsumerToken&& ct)
      : pt_(std::move(pt)), ct_(std::move(ct)) {}
  moodycamel::ConsumerToken& consumerToken() { return ct_; }
  moodycamel::ProducerToken& producerToken() { return pt_; }
  ReadChunk<T>*& chunkPtr() { return chunk_; }
  inline void have(size_t num) { chunk_->have(num); }
  inline size_t size() { return chunk_->size(); }
  inline size_t want() const { return chunk_->want(); }
  T& operator[](size_t i) { return (*chunk_)[i]; }
  typename std::vector<T>::iterator begin() { return chunk_->begin(); }
  typename std::vector<T>::iterator end() {
    return chunk_->begin() + chunk_->size();
  }

private:
  ReadChunk<T>* chunk_{nullptr};
  moodycamel::ProducerToken pt_;
  moodycamel::ConsumerToken ct_;
};

template <typename T> class FastxParser {
public:
  FastxParser(std::vector<std::string> files, uint32_t numReaders);
  FastxParser(std::vector<std::string> files, std::vector<std::string> files2,
              uint32_t numReaders);
  ~FastxParser();
  bool start();
  ReadGroup<T> getReadGroup();
  bool refill(ReadGroup<T>& rg);
  void finishedWithGroup(ReadGroup<T>& s);

private:
  moodycamel::ProducerToken getProducerToken_();
  moodycamel::ConsumerToken getConsumerToken_();

  size_t blockSize_;
  std::vector<std::string> inputStreams_;
  std::vector<std::string> inputStreams2_;
  bool parsing_;
  std::thread* parsingThread_;
  moodycamel::ConcurrentQueue<ReadChunk<T> *> readQueue_, seqContainerQueue_;
  std::vector<ReadChunk<T>*> readChunks_;
  std::unique_ptr<moodycamel::ProducerToken> produceContainer_{nullptr};
  std::unique_ptr<moodycamel::ConsumerToken> consumeContainer_{nullptr};
  std::unique_ptr<moodycamel::ProducerToken> produceReads_{nullptr};
  std::unique_ptr<moodycamel::ConsumerToken> consumeReads_{nullptr};
};

#endif // __STREAMING_READ_PARSER__

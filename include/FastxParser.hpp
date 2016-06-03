
#ifndef __STREAMING_READ_PARSER__
#define __STREAMING_READ_PARSER__

#include <cstdio>
#include <cstdlib>
#include <vector>
#include <thread>
#include <atomic>
#include <iostream>
#include "unistd.h"
#include "fcntl.h"

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
  std::pair<ReadSeq, ReadSeq> dat;
};

template <typename T>
class ReadGroup {
public:
  ReadGroup(size_t want=1000) : want_(want), have_(0), group_(want, nullptr) {}
  inline void have(size_t num) { have_ = num; }
  inline size_t size() { return have_; } 
  inline size_t want() const { return want_; }
  ReadSeq* operator[](size_t i) { return group_[i]; }
  typename std::vector<T*>::iterator begin() { return group_.begin(); }
  typename std::vector<T*>::iterator end() { return group_.begin() + have_; }
private:
  std::vector<T*> group_;
  size_t want_;
  size_t have_;
};

template <typename T>
class FastxParser {
public:
  FastxParser( std::vector<std::string>& files, uint32_t numReaders);
    ~FastxParser();
  moodycamel::ProducerToken getProducerToken();
  moodycamel::ConsumerToken getConsumerToken();
    bool start();
  bool getReadGroup(moodycamel::ConsumerToken& ct, ReadGroup<T>& seqs);
    //bool nextRead(moodycamel::ConsumerToken&ct, ReadSeq*& seq);
  void finishedWithGroup(moodycamel::ProducerToken& pt, ReadGroup<T>& s);
  //void finishedWithRead(moodycamel::ProducerToken&pt, ReadSeq*& s);

private:
  size_t blockSize_;
  std::vector<std::string>& inputStreams_;
    bool parsing_;
    std::thread* parsingThread_;
  moodycamel::ConcurrentQueue<T*> readQueue_, seqContainerQueue_;
    ReadSeq* readStructs_;
  std::unique_ptr<moodycamel::ProducerToken> produceContainer_{nullptr};
  std::unique_ptr<moodycamel::ConsumerToken> consumeContainer_{nullptr};
  std::unique_ptr<moodycamel::ProducerToken> produceReads_{nullptr};
  std::unique_ptr<moodycamel::ConsumerToken> consumeReads_{nullptr};
  size_t queueCapacity_;//= 1048576;
};

#endif // __STREAMING_READ_PARSER__

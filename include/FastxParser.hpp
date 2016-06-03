
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

class FastxParser {
public:
  FastxParser( std::vector<std::string>& files, uint32_t numReaders);
    ~FastxParser();
  moodycamel::ProducerToken getProducerToken();
  moodycamel::ConsumerToken getConsumerToken();
    bool start();
  bool nextRead(moodycamel::ConsumerToken&ct, ReadSeq*& seq);
  void finishedWithRead(moodycamel::ProducerToken&pt, ReadSeq*& s);

private:
  int32_t blockSize_;
  std::vector<std::string>& inputStreams_;
    bool parsing_;
    std::thread* parsingThread_;
  moodycamel::ConcurrentQueue<ReadSeq*> readQueue_, seqContainerQueue_;
    ReadSeq* readStructs_;
  std::unique_ptr<moodycamel::ProducerToken> pt_{nullptr};
    const size_t queueCapacity_ = 2000000;
};

//#include "Parser.cpp"

#endif // __STREAMING_READ_PARSER__

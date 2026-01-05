#include "FastxParser.hpp"
#include "FastxParserThreadUtils.hpp"

#include "fcntl.h"
#include "unistd.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <poll.h>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>
#include <zlib.h>

namespace fastx_parser {
template <typename T>
FastxParser<T>::FastxParser(std::vector<std::string> files,
                            uint32_t numConsumers, uint32_t numParsers,
                            uint32_t chunkSize)
    : FastxParser(files, {}, numConsumers, numParsers, chunkSize) {}

template <typename T>
FastxParser<T>::FastxParser(std::vector<std::string> files,
                            std::vector<std::string> files2,
                            uint32_t numConsumers, uint32_t numParsers,
                            uint32_t chunkSize, bool parallelParsing)
    : inputStreams_(files), inputStreams2_(files2), numParsing_(0),
      blockSize_(chunkSize), parallelParsing_(parallelParsing) {

  if (numParsers > files.size()) {
    std::cerr << "Can't make user of more parsing threads than file (pairs); "
                 "setting # of parsing threads to "
              << files.size() << '\n';
    numParsers = files.size();
  }
  numParsers_ = numParsers;

  // nobody is parsing yet
  numParsing_ = 0;

  readQueue_ = moodycamel::ConcurrentQueue<std::unique_ptr<ReadChunk<T>>>(
      4 * numConsumers, numParsers, 0);

  seqContainerQueue_ =
      moodycamel::ConcurrentQueue<std::unique_ptr<ReadChunk<T>>>(
          4 * numConsumers, 1 + numConsumers, 0);

  workQueue_ = moodycamel::ConcurrentQueue<uint32_t>(numParsers_);

  // push all file ids on the queue
  for (size_t i = 0; i < files.size(); ++i) {
    workQueue_.enqueue(i);
  }

  // every parsing thread gets a consumer token for the seqContainerQueue
  // and a producer token for the readQueue.
  for (size_t i = 0; i < numParsers_; ++i) {
    consumeContainers_.emplace_back(
        new moodycamel::ConsumerToken(seqContainerQueue_));
    produceReads_.emplace_back(new moodycamel::ProducerToken(readQueue_));
  }

  // enqueue the appropriate number of read chunks so that we can start
  // filling them once the parser has been started.
  moodycamel::ProducerToken produceContainer(seqContainerQueue_);
  for (size_t i = 0; i < 4 * numConsumers; ++i) {
    auto chunk = make_unique<ReadChunk<T>>(blockSize_);
    seqContainerQueue_.enqueue(produceContainer, std::move(chunk));
  }
}

// Triplet constructor
template <typename T>
FastxParser<T>::FastxParser(std::vector<std::string> files,
                            std::vector<std::string> files2,
                            std::vector<std::string> files3,
                            uint32_t numConsumers, uint32_t numParsers,
                            uint32_t chunkSize, bool parallelParsing)
    : inputStreams_(files), inputStreams2_(files2), inputStreams3_(files3),
      numParsing_(0), blockSize_(chunkSize), parallelParsing_(parallelParsing) {

  // Validate that all three vectors have the same size
  if (files.size() != files2.size() || files.size() != files3.size()) {
    throw std::invalid_argument(
        "All three file vectors must have the same number of files");
  }

  if (numParsers > files.size()) {
    std::cerr << "Can't make use of more parsing threads than file (triplets); "
                 "setting # of parsing threads to "
              << files.size() << '\n';
    numParsers = files.size();
  }
  numParsers_ = numParsers;
  numParsing_ = 0;

  readQueue_ = moodycamel::ConcurrentQueue<std::unique_ptr<ReadChunk<T>>>(
      4 * numConsumers, numParsers, 0);

  seqContainerQueue_ =
      moodycamel::ConcurrentQueue<std::unique_ptr<ReadChunk<T>>>(
          4 * numConsumers, 1 + numConsumers, 0);

  workQueue_ = moodycamel::ConcurrentQueue<uint32_t>(numParsers_);

  for (size_t i = 0; i < files.size(); ++i) {
    workQueue_.enqueue(i);
  }

  for (size_t i = 0; i < numParsers_; ++i) {
    consumeContainers_.emplace_back(
        new moodycamel::ConsumerToken(seqContainerQueue_));
    produceReads_.emplace_back(new moodycamel::ProducerToken(readQueue_));
  }

  moodycamel::ProducerToken produceContainer(seqContainerQueue_);
  for (size_t i = 0; i < 4 * numConsumers; ++i) {
    auto chunk = make_unique<ReadChunk<T>>(blockSize_);
    seqContainerQueue_.enqueue(produceContainer, std::move(chunk));
  }
}

// ============================================================================
// Parallel parsing functions for multi-file modes
// ============================================================================

// Parse a single file and push reads with their rank to an intermediate queue
template <typename SingleReadT>
int parse_single_file(
    const std::string& filename, uint32_t file_idx,
    std::atomic<uint32_t>& numParsing, std::atomic<bool>& parsingDone,
    moodycamel::ConcurrentQueue<ParsedSingleRead<SingleReadT>>& outputQueue) {

  using namespace klibpp;
  using fastx_parser::thread_utils::MIN_BACKOFF_ITERS;

  gzFile fp = gzopen(filename.c_str(), "r");
  if (!fp) {
    parsingDone = true;
    --numParsing;
    return -4; // File open error
  }

  auto seq = make_kstream(fp, gzread, mode::in);
  uint64_t rank = 0;
  SingleReadT record;

  while (seq >> record) {
    ParsedSingleRead<SingleReadT> parsed;
    parsed.read = std::move(record);
    parsed.rank = rank++;
    parsed.file_idx = file_idx;
    parsed.is_end = false;

    size_t curMaxDelay = MIN_BACKOFF_ITERS;
    while (!outputQueue.try_enqueue(std::move(parsed))) {
      fastx_parser::thread_utils::backoffOrYield(curMaxDelay);
    }
  }

  int result = 0;
  if (seq.err()) {
    result = -3;
  } else if (seq.tqs()) {
    result = -2;
  }

  // Signal end-of-file
  ParsedSingleRead<SingleReadT> sentinel;
  sentinel.is_end = true;
  sentinel.file_idx = file_idx;
  outputQueue.enqueue(std::move(sentinel));

  gzclose(fp);
  parsingDone = true;
  --numParsing;
  return result;
}

// Assemble read pairs from two intermediate queues
template <typename T>
int assemble_read_pairs(
    moodycamel::ConcurrentQueue<ParsedSingleRead<klibpp::KSeq>>& queue1,
    moodycamel::ConcurrentQueue<ParsedSingleRead<klibpp::KSeq>>& queue2,
    std::atomic<bool>& done1, std::atomic<bool>& done2,
    moodycamel::ConsumerToken* cCont, moodycamel::ProducerToken* pRead,
    moodycamel::ConcurrentQueue<std::unique_ptr<ReadChunk<T>>>&
        seqContainerQueue,
    moodycamel::ConcurrentQueue<std::unique_ptr<ReadChunk<T>>>& readQueue,
    uint32_t file_idx, std::atomic<uint32_t>& numAssembling) {

  using fastx_parser::thread_utils::MIN_BACKOFF_ITERS;

  // Buffers for out-of-order reads, keyed by rank
  std::map<uint64_t, klibpp::KSeq> buffer1, buffer2;
  uint64_t nextExpectedRank = 0;
  bool file1Done = false, file2Done = false;

  // Get initial chunk
  std::unique_ptr<ReadChunk<T>> local;
  size_t curMaxDelay = MIN_BACKOFF_ITERS;
  while (!seqContainerQueue.try_dequeue(*cCont, local)) {
    fastx_parser::thread_utils::backoffOrYield(curMaxDelay);
  }
  size_t numObtained = local->size();
  size_t numWaiting = 0;
  uint64_t first_frag_of_chunk = 0;

  while (!file1Done || !file2Done || !buffer1.empty() || !buffer2.empty()) {
    // Drain queue1
    ParsedSingleRead<klibpp::KSeq> item;
    while (queue1.try_dequeue(item)) {
      if (item.is_end) {
        file1Done = true;
      } else {
        buffer1[item.rank] = std::move(item.read);
      }
    }

    // Drain queue2
    while (queue2.try_dequeue(item)) {
      if (item.is_end) {
        file2Done = true;
      } else {
        buffer2[item.rank] = std::move(item.read);
      }
    }

    // Assemble pairs in order
    while (buffer1.count(nextExpectedRank) && buffer2.count(nextExpectedRank)) {
      T& pair = (*local)[numWaiting];
      pair.first = std::move(buffer1[nextExpectedRank]);
      pair.second = std::move(buffer2[nextExpectedRank]);
      buffer1.erase(nextExpectedRank);
      buffer2.erase(nextExpectedRank);
      ++nextExpectedRank;
      ++numWaiting;

      if (numWaiting == numObtained) {
        local->have(numWaiting);
        local->set_chunk_frag_offset(file_idx, first_frag_of_chunk);
        curMaxDelay = MIN_BACKOFF_ITERS;
        while (!readQueue.try_enqueue(*pRead, std::move(local))) {
          fastx_parser::thread_utils::backoffOrYield(curMaxDelay);
        }
        first_frag_of_chunk = nextExpectedRank;
        numWaiting = 0;
        curMaxDelay = MIN_BACKOFF_ITERS;
        while (!seqContainerQueue.try_dequeue(*cCont, local)) {
          fastx_parser::thread_utils::backoffOrYield(curMaxDelay);
        }
        numObtained = local->size();
      }
    }

    // Yield if waiting for data
    if ((!file1Done || !file2Done) && (!buffer1.count(nextExpectedRank) ||
                                       !buffer2.count(nextExpectedRank))) {
      fastx_parser::thread_utils::backoffOrYield(curMaxDelay);
    }
  }

  // Flush remaining
  if (numWaiting > 0) {
    local->have(numWaiting);
    local->set_chunk_frag_offset(file_idx, first_frag_of_chunk);
    curMaxDelay = MIN_BACKOFF_ITERS;
    while (!readQueue.try_enqueue(*pRead, std::move(local))) {
      fastx_parser::thread_utils::backoffOrYield(curMaxDelay);
    }
  } else {
    // Return unused chunk
    seqContainerQueue.enqueue(std::move(local));
  }

  --numAssembling;
  return 0;
}

// Assemble read triplets from three intermediate queues
template <typename T>
int assemble_read_triplets(
    moodycamel::ConcurrentQueue<ParsedSingleRead<klibpp::KSeq>>& queue1,
    moodycamel::ConcurrentQueue<ParsedSingleRead<klibpp::KSeq>>& queue2,
    moodycamel::ConcurrentQueue<ParsedSingleRead<klibpp::KSeq>>& queue3,
    std::atomic<bool>& done1, std::atomic<bool>& done2,
    std::atomic<bool>& done3, moodycamel::ConsumerToken* cCont,
    moodycamel::ProducerToken* pRead,
    moodycamel::ConcurrentQueue<std::unique_ptr<ReadChunk<T>>>&
        seqContainerQueue,
    moodycamel::ConcurrentQueue<std::unique_ptr<ReadChunk<T>>>& readQueue,
    uint32_t file_idx, std::atomic<uint32_t>& numAssembling) {

  using fastx_parser::thread_utils::MIN_BACKOFF_ITERS;

  std::map<uint64_t, klibpp::KSeq> buffer1, buffer2, buffer3;
  uint64_t nextExpectedRank = 0;
  bool file1Done = false, file2Done = false, file3Done = false;

  std::unique_ptr<ReadChunk<T>> local;
  size_t curMaxDelay = MIN_BACKOFF_ITERS;
  while (!seqContainerQueue.try_dequeue(*cCont, local)) {
    fastx_parser::thread_utils::backoffOrYield(curMaxDelay);
  }
  size_t numObtained = local->size();
  size_t numWaiting = 0;
  uint64_t first_frag_of_chunk = 0;

  while (!file1Done || !file2Done || !file3Done || !buffer1.empty() ||
         !buffer2.empty() || !buffer3.empty()) {
    // Drain all queues
    ParsedSingleRead<klibpp::KSeq> item;
    while (queue1.try_dequeue(item)) {
      if (item.is_end) {
        file1Done = true;
      } else {
        buffer1[item.rank] = std::move(item.read);
      }
    }
    while (queue2.try_dequeue(item)) {
      if (item.is_end) {
        file2Done = true;
      } else {
        buffer2[item.rank] = std::move(item.read);
      }
    }
    while (queue3.try_dequeue(item)) {
      if (item.is_end) {
        file3Done = true;
      } else {
        buffer3[item.rank] = std::move(item.read);
      }
    }

    // Assemble triplets in order
    while (buffer1.count(nextExpectedRank) && buffer2.count(nextExpectedRank) &&
           buffer3.count(nextExpectedRank)) {
      T& triplet = (*local)[numWaiting];
      triplet.first = std::move(buffer1[nextExpectedRank]);
      triplet.second = std::move(buffer2[nextExpectedRank]);
      triplet.third = std::move(buffer3[nextExpectedRank]);
      buffer1.erase(nextExpectedRank);
      buffer2.erase(nextExpectedRank);
      buffer3.erase(nextExpectedRank);
      ++nextExpectedRank;
      ++numWaiting;

      if (numWaiting == numObtained) {
        local->have(numWaiting);
        local->set_chunk_frag_offset(file_idx, first_frag_of_chunk);
        curMaxDelay = MIN_BACKOFF_ITERS;
        while (!readQueue.try_enqueue(*pRead, std::move(local))) {
          fastx_parser::thread_utils::backoffOrYield(curMaxDelay);
        }
        first_frag_of_chunk = nextExpectedRank;
        numWaiting = 0;
        curMaxDelay = MIN_BACKOFF_ITERS;
        while (!seqContainerQueue.try_dequeue(*cCont, local)) {
          fastx_parser::thread_utils::backoffOrYield(curMaxDelay);
        }
        numObtained = local->size();
      }
    }

    // Yield if waiting
    if ((!file1Done || !file2Done || !file3Done) &&
        (!buffer1.count(nextExpectedRank) || !buffer2.count(nextExpectedRank) ||
         !buffer3.count(nextExpectedRank))) {
      fastx_parser::thread_utils::backoffOrYield(curMaxDelay);
    }
  }

  // Flush remaining
  if (numWaiting > 0) {
    local->have(numWaiting);
    local->set_chunk_frag_offset(file_idx, first_frag_of_chunk);
    curMaxDelay = MIN_BACKOFF_ITERS;
    while (!readQueue.try_enqueue(*pRead, std::move(local))) {
      fastx_parser::thread_utils::backoffOrYield(curMaxDelay);
    }
  } else {
    seqContainerQueue.enqueue(std::move(local));
  }

  --numAssembling;
  return 0;
}

template <typename T> ReadGroup<T> FastxParser<T>::getReadGroup() {
  return ReadGroup<T>(getProducerToken_(), getConsumerToken_());
}

template <typename T>
moodycamel::ProducerToken FastxParser<T>::getProducerToken_() {
  return moodycamel::ProducerToken(seqContainerQueue_);
}

template <typename T>
moodycamel::ConsumerToken FastxParser<T>::getConsumerToken_() {
  return moodycamel::ConsumerToken(readQueue_);
}

template <typename T> FastxParser<T>::~FastxParser() {
  if (isActive_ or numParsing_ > 0) {
    // Think about if this is too noisy --- but the user really shouldn't do
    // this.
    std::cerr
        << "\n\nEncountered FastxParser destructor while parser was still "
           "marked active (or while parsing threads were still active). "
        << "Be sure to call stop() before letting FastxParser leave scope!\n";
    try {
      stop();
    } catch (const std::exception& e) {
      // Should exiting here be a user-definable behavior?
      // What is the right mechanism for that.
      std::cerr << "\n\nParser encountered exception : " << e.what() << "\n";
      std::exit(-1);
    }
  }
  // Otherwise, we are good to go (i.e., destruct)
}

template <typename T> bool FastxParser<T>::stop() {
  bool ret{false};
  if (isActive_) {
    for (auto& t : parsingThreads_) {
      t->join();
    }
    isActive_ = false;
    for (auto& res : threadResults_) {
      if (res == -3) {
        throw std::range_error("Error reading from the FASTA/Q stream. Make "
                               "sure the file is valid.");
      } else if (res < -1) {
        std::stringstream ss;
        ss << "Error reading from the FASTA/Q stream. Minimum return code for "
              "left and right read was ("
           << res << "). Make sure the file is valid.";
        throw std::range_error(ss.str());
      }
    }
    ret = true;
  } else {
    // Is this being too loud?  Again, if this triggers, the user has violated
    // the API.
    std::cerr << "stop() was called on a FastxParser that was not marked "
                 "active. Did you remember "
              << "to call start() on this parser?\n";
  }
  return ret;
}

template <typename T>
int parse_reads(
    std::vector<std::string>& inputStreams, std::atomic<uint32_t>& numParsing,
    moodycamel::ConsumerToken* cCont, moodycamel::ProducerToken* pRead,
    moodycamel::ConcurrentQueue<uint32_t>& workQueue,
    moodycamel::ConcurrentQueue<std::unique_ptr<ReadChunk<T>>>&
        seqContainerQueue_,
    moodycamel::ConcurrentQueue<std::unique_ptr<ReadChunk<T>>>& readQueue_) {

  using namespace klibpp;
  using fastx_parser::thread_utils::MIN_BACKOFF_ITERS;
  auto curMaxDelay = MIN_BACKOFF_ITERS;
  T* s;

  uint32_t fn{0};
  while (workQueue.try_dequeue(fn)) {
    auto& file = inputStreams[fn];
    std::unique_ptr<ReadChunk<T>> local;
    while (!seqContainerQueue_.try_dequeue(*cCont, local)) {
      fastx_parser::thread_utils::backoffOrYield(curMaxDelay);
      // Think of a way to do this that wouldn't be loud (or would allow a
      // user-definable logging mechanism) std::cerr << "couldn't dequeue read
      // chunk\n";
    }
    size_t numObtained{local->size()};
    // open the file and init the parser
    gzFile fp = gzopen(file.c_str(), "r");

    // we start off with the 0-th fragment in this
    // file.
    uint64_t frag_id{0};
    uint64_t first_frag_of_chunk{frag_id};

    // The number of reads we have in the local vector
    size_t numWaiting{0};

    auto seq = make_kstream(fp, gzread, mode::in);

    s = &((*local)[numWaiting]);
    while (seq >> *s) { // ksv >= 0
      frag_id++;
      numWaiting++;
      // If we've filled the local vector, then dump to the concurrent queue
      if (numWaiting == numObtained) {
        curMaxDelay = MIN_BACKOFF_ITERS;
        local->set_chunk_frag_offset(fn, first_frag_of_chunk);
        while (!readQueue_.try_enqueue(std::move(local))) {
          fastx_parser::thread_utils::backoffOrYield(curMaxDelay);
        }
        first_frag_of_chunk = frag_id;
        numWaiting = 0;
        numObtained = 0;
        // And get more empty reads
        curMaxDelay = MIN_BACKOFF_ITERS;
        while (!seqContainerQueue_.try_dequeue(*cCont, local)) {
          fastx_parser::thread_utils::backoffOrYield(curMaxDelay);
        }
        numObtained = local->size();
      }
      s = &((*local)[numWaiting]);
    }

    // if we had an error in the stream
    if (seq.err()) {
      --numParsing;
      return -3;
    } else if (seq.tqs()) {
      // if we had a quality string of the wrong length
      // tqs == truncated quality string
      --numParsing;
      return -2;
    }

    // If we hit the end of the file and have any reads in our local buffer
    // then dump them here.
    if (numWaiting > 0) {
      local->have(numWaiting);
      local->set_chunk_frag_offset(fn, first_frag_of_chunk);
      curMaxDelay = MIN_BACKOFF_ITERS;
      while (!readQueue_.try_enqueue(*pRead, std::move(local))) {
        fastx_parser::thread_utils::backoffOrYield(curMaxDelay);
      }
      numWaiting = 0;
    } else if (numObtained > 0) {
      curMaxDelay = MIN_BACKOFF_ITERS;
      local->set_chunk_frag_offset(fn, first_frag_of_chunk);
      while (!seqContainerQueue_.try_enqueue(std::move(local))) {
        fastx_parser::thread_utils::backoffOrYield(curMaxDelay);
      }
    }
    // destroy the parser and close the file
    gzclose(fp);
  }

  --numParsing;
  return 0;
}

template <typename T>
int parse_read_pairs(
    std::vector<std::string>& inputStreams,
    std::vector<std::string>& inputStreams2, std::atomic<uint32_t>& numParsing,
    moodycamel::ConsumerToken* cCont, moodycamel::ProducerToken* pRead,
    moodycamel::ConcurrentQueue<uint32_t>& workQueue,
    moodycamel::ConcurrentQueue<std::unique_ptr<ReadChunk<T>>>&
        seqContainerQueue_,
    moodycamel::ConcurrentQueue<std::unique_ptr<ReadChunk<T>>>& readQueue_) {

  using namespace klibpp;
  using fastx_parser::thread_utils::MIN_BACKOFF_ITERS;
  size_t curMaxDelay = MIN_BACKOFF_ITERS;
  T* s;

  uint32_t fn{0};
  while (workQueue.try_dequeue(fn)) {
    // for (size_t fn = 0; fn < inputStreams.size(); ++fn) {
    auto& file = inputStreams[fn];
    auto& file2 = inputStreams2[fn];

    std::unique_ptr<ReadChunk<T>> local;
    while (!seqContainerQueue_.try_dequeue(*cCont, local)) {
      fastx_parser::thread_utils::backoffOrYield(curMaxDelay);
      // Think of a way to do this that wouldn't be loud (or would allow a
      // user-definable logging mechanism) std::cerr << "couldn't dequeue read
      // chunk\n";
    }
    size_t numObtained{local->size()};

    // open the file and init the parser
    gzFile fp = gzopen(file.c_str(), "r");
    gzFile fp2 = gzopen(file2.c_str(), "r");

    // we start off with the 0-th fragment in this
    // file.
    uint64_t frag_id{0};
    uint64_t first_frag_of_chunk{frag_id};

    // The number of reads we have in the local vector
    size_t numWaiting{0};

    auto seq = make_kstream(fp, gzread, mode::in);
    auto seq2 = make_kstream(fp2, gzread, mode::in);

    s = &((*local)[numWaiting]);
    while ((seq >> s->first) and
           (seq2 >> s->second)) { // ksv >= 0 and ksv2 >= 0) {
      frag_id++;
      numWaiting++;
      // If we've filled the local vector, then dump to the concurrent queue
      if (numWaiting == numObtained) {
        curMaxDelay = MIN_BACKOFF_ITERS;
        local->set_chunk_frag_offset(fn, first_frag_of_chunk);
        while (!readQueue_.try_enqueue(std::move(local))) {
          fastx_parser::thread_utils::backoffOrYield(curMaxDelay);
        }
        first_frag_of_chunk = frag_id;
        numWaiting = 0;
        numObtained = 0;
        // And get more empty reads
        curMaxDelay = MIN_BACKOFF_ITERS;
        while (!seqContainerQueue_.try_dequeue(*cCont, local)) {
          fastx_parser::thread_utils::backoffOrYield(curMaxDelay);
        }
        numObtained = local->size();
      }
      s = &((*local)[numWaiting]);
    }

    // if we had an error in the stream
    if (seq.err() or seq2.err()) {
      --numParsing;
      return -3;
    } else if (seq.tqs() or seq2.tqs()) {
      // if we had a quality string of the wrong length
      // tqs == truncated quality string
      --numParsing;
      return -2;
    }

    // If we hit the end of the file and have any reads in our local buffer
    // then dump them here.
    if (numWaiting > 0) {
      local->have(numWaiting);
      local->set_chunk_frag_offset(fn, first_frag_of_chunk);
      curMaxDelay = MIN_BACKOFF_ITERS;
      while (!readQueue_.try_enqueue(*pRead, std::move(local))) {
        fastx_parser::thread_utils::backoffOrYield(curMaxDelay);
      }
      numWaiting = 0;
    } else if (numObtained > 0) {
      curMaxDelay = MIN_BACKOFF_ITERS;
      local->set_chunk_frag_offset(fn, first_frag_of_chunk);
      while (!seqContainerQueue_.try_enqueue(std::move(local))) {
        fastx_parser::thread_utils::backoffOrYield(curMaxDelay);
      }
    }
    // destroy the parser and close the file
    gzclose(fp);
    gzclose(fp2);
  }

  --numParsing;
  return 0;
}

template <> bool FastxParser<ReadSeq>::start() {
  if (numParsing_ == 0) {
    isActive_ = true;
    threadResults_.resize(numParsers_);
    std::fill(threadResults_.begin(), threadResults_.end(), 0);
    for (size_t i = 0; i < numParsers_; ++i) {
      ++numParsing_;
      parsingThreads_.emplace_back(new std::thread([this, i]() {
        this->threadResults_[i] = parse_reads(
            this->inputStreams_, this->numParsing_,
            this->consumeContainers_[i].get(), this->produceReads_[i].get(),
            this->workQueue_, this->seqContainerQueue_, this->readQueue_);
      }));
    }
    return true;
  } else {
    return false;
  }
}

template <> bool FastxParser<ReadPair>::start() {
  if (numParsing_ == 0) {
    isActive_ = true;
    // Some basic checking to ensure the read files look "sane".
    if (inputStreams_.size() != inputStreams2_.size()) {
      throw std::invalid_argument("There should be the same number "
                                  "of files for the left and right reads");
    }
    for (size_t i = 0; i < inputStreams_.size(); ++i) {
      auto& s1 = inputStreams_[i];
      auto& s2 = inputStreams2_[i];
      if (s1 == s2) {
        throw std::invalid_argument("You provided the same file " + s1 +
                                    " as both a left and right file");
      }
    }

    if (parallelParsing_) {
      // Parallel mode: spawn separate threads for each file + assembler
      // For each file pair, we need:
      // - 2 parser threads (one per file)
      // - 1 assembler thread
      // We use numParsers_ to determine how many file pairs to process in
      // parallel

      // We'll process all file pairs, spawning threads for each
      size_t numFilePairs = inputStreams_.size();
      threadResults_.resize(numFilePairs *
                            3); // 2 parsers + 1 assembler per pair
      std::fill(threadResults_.begin(), threadResults_.end(), 0);

      for (size_t fn = 0; fn < numFilePairs; ++fn) {
        // Create intermediate queues for this file pair
        auto queue1 = std::make_shared<
            moodycamel::ConcurrentQueue<ParsedSingleRead<klibpp::KSeq>>>(4096);
        auto queue2 = std::make_shared<
            moodycamel::ConcurrentQueue<ParsedSingleRead<klibpp::KSeq>>>(4096);
        auto done1 = std::make_shared<std::atomic<bool>>(false);
        auto done2 = std::make_shared<std::atomic<bool>>(false);
        auto numAssembling = std::make_shared<std::atomic<uint32_t>>(1);

        // Parser thread for file1
        ++numParsing_;
        parsingThreads_.emplace_back(new std::thread([this, fn, queue1,
                                                      done1]() {
          this->threadResults_[fn * 3] = parse_single_file<klibpp::KSeq>(
              this->inputStreams_[fn], fn, this->numParsing_, *done1, *queue1);
        }));

        // Parser thread for file2
        ++numParsing_;
        parsingThreads_.emplace_back(new std::thread([this, fn, queue2,
                                                      done2]() {
          this->threadResults_[fn * 3 + 1] = parse_single_file<klibpp::KSeq>(
              this->inputStreams2_[fn], fn, this->numParsing_, *done2, *queue2);
        }));

        // Assembler thread
        ++numParsing_;
        size_t tokenIdx = fn % numParsers_;
        parsingThreads_.emplace_back(
            new std::thread([this, fn, tokenIdx, queue1, queue2, done1, done2,
                             numAssembling]() {
              this->threadResults_[fn * 3 + 2] = assemble_read_pairs<ReadPair>(
                  *queue1, *queue2, *done1, *done2,
                  this->consumeContainers_[tokenIdx].get(),
                  this->produceReads_[tokenIdx].get(), this->seqContainerQueue_,
                  this->readQueue_, fn, this->numParsing_);
            }));
      }
    } else {
      // Original sequential mode
      threadResults_.resize(numParsers_);
      std::fill(threadResults_.begin(), threadResults_.end(), 0);

      for (size_t i = 0; i < numParsers_; ++i) {
        ++numParsing_;
        parsingThreads_.emplace_back(new std::thread([this, i]() {
          this->threadResults_[i] = parse_read_pairs(
              this->inputStreams_, this->inputStreams2_, this->numParsing_,
              this->consumeContainers_[i].get(), this->produceReads_[i].get(),
              this->workQueue_, this->seqContainerQueue_, this->readQueue_);
        }));
      }
    }
    return true;
  } else {
    return false;
  }
}

template <> bool FastxParser<ReadQualPair>::start() {
  if (numParsing_ == 0) {
    isActive_ = true;
    // Some basic checking to ensure the read files look "sane".
    if (inputStreams_.size() != inputStreams2_.size()) {
      throw std::invalid_argument("There should be the same number "
                                  "of files for the left and right reads");
    }
    for (size_t i = 0; i < inputStreams_.size(); ++i) {
      auto& s1 = inputStreams_[i];
      auto& s2 = inputStreams2_[i];
      if (s1 == s2) {
        throw std::invalid_argument("You provided the same file " + s1 +
                                    " as both a left and right file");
      }
    }

    if (parallelParsing_) {
      size_t numFilePairs = inputStreams_.size();
      threadResults_.resize(numFilePairs * 3);
      std::fill(threadResults_.begin(), threadResults_.end(), 0);

      for (size_t fn = 0; fn < numFilePairs; ++fn) {
        auto queue1 = std::make_shared<
            moodycamel::ConcurrentQueue<ParsedSingleRead<klibpp::KSeq>>>(4096);
        auto queue2 = std::make_shared<
            moodycamel::ConcurrentQueue<ParsedSingleRead<klibpp::KSeq>>>(4096);
        auto done1 = std::make_shared<std::atomic<bool>>(false);
        auto done2 = std::make_shared<std::atomic<bool>>(false);
        auto numAssembling = std::make_shared<std::atomic<uint32_t>>(1);

        ++numParsing_;
        parsingThreads_.emplace_back(new std::thread([this, fn, queue1,
                                                      done1]() {
          this->threadResults_[fn * 3] = parse_single_file<klibpp::KSeq>(
              this->inputStreams_[fn], fn, this->numParsing_, *done1, *queue1);
        }));

        ++numParsing_;
        parsingThreads_.emplace_back(new std::thread([this, fn, queue2,
                                                      done2]() {
          this->threadResults_[fn * 3 + 1] = parse_single_file<klibpp::KSeq>(
              this->inputStreams2_[fn], fn, this->numParsing_, *done2, *queue2);
        }));

        ++numParsing_;
        size_t tokenIdx = fn % numParsers_;
        parsingThreads_.emplace_back(new std::thread([this, fn, tokenIdx,
                                                      queue1, queue2, done1,
                                                      done2, numAssembling]() {
          this->threadResults_[fn * 3 + 2] = assemble_read_pairs<ReadQualPair>(
              *queue1, *queue2, *done1, *done2,
              this->consumeContainers_[tokenIdx].get(),
              this->produceReads_[tokenIdx].get(), this->seqContainerQueue_,
              this->readQueue_, fn, this->numParsing_);
        }));
      }
    } else {
      threadResults_.resize(numParsers_);
      std::fill(threadResults_.begin(), threadResults_.end(), 0);

      for (size_t i = 0; i < numParsers_; ++i) {
        ++numParsing_;
        parsingThreads_.emplace_back(new std::thread([this, i]() {
          this->threadResults_[i] = parse_read_pairs(
              this->inputStreams_, this->inputStreams2_, this->numParsing_,
              this->consumeContainers_[i].get(), this->produceReads_[i].get(),
              this->workQueue_, this->seqContainerQueue_, this->readQueue_);
        }));
      }
    }
    return true;
  } else {
    return false;
  }
}

// Triplet start() specialization
template <> bool FastxParser<ReadTriple>::start() {
  if (numParsing_ == 0) {
    isActive_ = true;
    // Validate all three file vectors have matching sizes
    if (inputStreams_.size() != inputStreams2_.size() ||
        inputStreams_.size() != inputStreams3_.size()) {
      throw std::invalid_argument(
          "All three file vectors must have the same size");
    }

    if (parallelParsing_) {
      size_t numFileTriplets = inputStreams_.size();
      threadResults_.resize(numFileTriplets * 4); // 3 parsers + 1 assembler
      std::fill(threadResults_.begin(), threadResults_.end(), 0);

      for (size_t fn = 0; fn < numFileTriplets; ++fn) {
        auto queue1 = std::make_shared<
            moodycamel::ConcurrentQueue<ParsedSingleRead<klibpp::KSeq>>>(4096);
        auto queue2 = std::make_shared<
            moodycamel::ConcurrentQueue<ParsedSingleRead<klibpp::KSeq>>>(4096);
        auto queue3 = std::make_shared<
            moodycamel::ConcurrentQueue<ParsedSingleRead<klibpp::KSeq>>>(4096);
        auto done1 = std::make_shared<std::atomic<bool>>(false);
        auto done2 = std::make_shared<std::atomic<bool>>(false);
        auto done3 = std::make_shared<std::atomic<bool>>(false);
        auto numAssembling = std::make_shared<std::atomic<uint32_t>>(1);

        ++numParsing_;
        parsingThreads_.emplace_back(new std::thread([this, fn, queue1,
                                                      done1]() {
          this->threadResults_[fn * 4] = parse_single_file<klibpp::KSeq>(
              this->inputStreams_[fn], fn, this->numParsing_, *done1, *queue1);
        }));

        ++numParsing_;
        parsingThreads_.emplace_back(new std::thread([this, fn, queue2,
                                                      done2]() {
          this->threadResults_[fn * 4 + 1] = parse_single_file<klibpp::KSeq>(
              this->inputStreams2_[fn], fn, this->numParsing_, *done2, *queue2);
        }));

        ++numParsing_;
        parsingThreads_.emplace_back(new std::thread([this, fn, queue3,
                                                      done3]() {
          this->threadResults_[fn * 4 + 2] = parse_single_file<klibpp::KSeq>(
              this->inputStreams3_[fn], fn, this->numParsing_, *done3, *queue3);
        }));

        ++numParsing_;
        size_t tokenIdx = fn % numParsers_;
        parsingThreads_.emplace_back(
            new std::thread([this, fn, tokenIdx, queue1, queue2, queue3, done1,
                             done2, done3, numAssembling]() {
              this->threadResults_[fn * 4 + 3] =
                  assemble_read_triplets<ReadTriple>(
                      *queue1, *queue2, *queue3, *done1, *done2, *done3,
                      this->consumeContainers_[tokenIdx].get(),
                      this->produceReads_[tokenIdx].get(),
                      this->seqContainerQueue_, this->readQueue_, fn,
                      this->numParsing_);
            }));
      }
    } else {
      throw std::runtime_error("Triplet parsing requires parallelParsing=true");
    }
    return true;
  } else {
    return false;
  }
}

template <> bool FastxParser<ReadQualTriple>::start() {
  if (numParsing_ == 0) {
    isActive_ = true;
    if (inputStreams_.size() != inputStreams2_.size() ||
        inputStreams_.size() != inputStreams3_.size()) {
      throw std::invalid_argument(
          "All three file vectors must have the same size");
    }

    if (parallelParsing_) {
      size_t numFileTriplets = inputStreams_.size();
      threadResults_.resize(numFileTriplets * 4);
      std::fill(threadResults_.begin(), threadResults_.end(), 0);

      for (size_t fn = 0; fn < numFileTriplets; ++fn) {
        auto queue1 = std::make_shared<
            moodycamel::ConcurrentQueue<ParsedSingleRead<klibpp::KSeq>>>(4096);
        auto queue2 = std::make_shared<
            moodycamel::ConcurrentQueue<ParsedSingleRead<klibpp::KSeq>>>(4096);
        auto queue3 = std::make_shared<
            moodycamel::ConcurrentQueue<ParsedSingleRead<klibpp::KSeq>>>(4096);
        auto done1 = std::make_shared<std::atomic<bool>>(false);
        auto done2 = std::make_shared<std::atomic<bool>>(false);
        auto done3 = std::make_shared<std::atomic<bool>>(false);
        auto numAssembling = std::make_shared<std::atomic<uint32_t>>(1);

        ++numParsing_;
        parsingThreads_.emplace_back(new std::thread([this, fn, queue1,
                                                      done1]() {
          this->threadResults_[fn * 4] = parse_single_file<klibpp::KSeq>(
              this->inputStreams_[fn], fn, this->numParsing_, *done1, *queue1);
        }));

        ++numParsing_;
        parsingThreads_.emplace_back(new std::thread([this, fn, queue2,
                                                      done2]() {
          this->threadResults_[fn * 4 + 1] = parse_single_file<klibpp::KSeq>(
              this->inputStreams2_[fn], fn, this->numParsing_, *done2, *queue2);
        }));

        ++numParsing_;
        parsingThreads_.emplace_back(new std::thread([this, fn, queue3,
                                                      done3]() {
          this->threadResults_[fn * 4 + 2] = parse_single_file<klibpp::KSeq>(
              this->inputStreams3_[fn], fn, this->numParsing_, *done3, *queue3);
        }));

        ++numParsing_;
        size_t tokenIdx = fn % numParsers_;
        parsingThreads_.emplace_back(
            new std::thread([this, fn, tokenIdx, queue1, queue2, queue3, done1,
                             done2, done3, numAssembling]() {
              this->threadResults_[fn * 4 + 3] =
                  assemble_read_triplets<ReadQualTriple>(
                      *queue1, *queue2, *queue3, *done1, *done2, *done3,
                      this->consumeContainers_[tokenIdx].get(),
                      this->produceReads_[tokenIdx].get(),
                      this->seqContainerQueue_, this->readQueue_, fn,
                      this->numParsing_);
            }));
      }
    } else {
      throw std::runtime_error("Triplet parsing requires parallelParsing=true");
    }
    return true;
  } else {
    return false;
  }
}

template <typename T> bool FastxParser<T>::refill(ReadGroup<T>& seqs) {
  finishedWithGroup(seqs);
  auto curMaxDelay = fastx_parser::thread_utils::MIN_BACKOFF_ITERS;
  while (numParsing_ > 0) {
    if (readQueue_.try_dequeue(seqs.consumerToken(), seqs.chunkPtr())) {
      return true;
    }
    fastx_parser::thread_utils::backoffOrYield(curMaxDelay);
  }
  return readQueue_.try_dequeue(seqs.consumerToken(), seqs.chunkPtr());
}

template <typename T> void FastxParser<T>::finishedWithGroup(ReadGroup<T>& s) {
  // If this read group is holding a valid chunk, then give it back
  if (!s.empty()) {
    seqContainerQueue_.enqueue(s.producerToken(), std::move(s.takeChunkPtr()));
    s.setChunkEmpty();
  }
}

template class FastxParser<ReadSeq>;
template class FastxParser<ReadPair>;
// template class FastxParser<ReadQual>;
template class FastxParser<ReadQualPair>;
template class FastxParser<ReadTriple>;
template class FastxParser<ReadQualTriple>;
} // namespace fastx_parser

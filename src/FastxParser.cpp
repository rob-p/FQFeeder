#include "FastxParser.hpp"
#include "FastxParserThreadUtils.hpp"

#include "fcntl.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <iostream>
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
      parallelParsing_(parallelParsing), blockSize_(chunkSize) {

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
      numParsing_(0), parallelParsing_(parallelParsing), blockSize_(chunkSize) {

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
// Uses bulk enqueueing for efficiency
// Parse a single file and push ReadChunks to an intermediate queue
template <typename SingleReadT>
int parse_single_file(
    const std::string& filename, uint32_t file_idx,
    std::atomic<uint32_t>& numParsing, std::atomic<bool>& parsingDone,
    moodycamel::ConcurrentQueue<std::unique_ptr<ReadChunk<SingleReadT>>>&
        outputQueue,
    moodycamel::ConcurrentQueue<std::unique_ptr<ReadChunk<SingleReadT>>>&
        recycleQueue,
  uint32_t chunkSize = 1000) {
  using namespace klibpp;
  using fastx_parser::thread_utils::MIN_BACKOFF_ITERS;

  gzFile fp = gzopen(filename.c_str(), "r");
  if (!fp) {
    parsingDone = true;
    --numParsing;
    return -4;
  }

  auto seq = make_kstream(fp, gzread, mode::in);
  uint32_t recordsInChunk = 0;

  // Helper to allocate or recycle a chunk
  auto get_chunk = [&](size_t size) {
    std::unique_ptr<ReadChunk<SingleReadT>> chunk;
    if (recycleQueue.try_dequeue(chunk)) {
      chunk->have(0); // Reset count
      return chunk;
    }
    return std::make_unique<ReadChunk<SingleReadT>>(size);
  };

  // Allocate initial chunk
  auto currentChunk = get_chunk(chunkSize);

  while (seq >> (*currentChunk)[recordsInChunk]) {
    recordsInChunk++;
    if (recordsInChunk == chunkSize) {
      currentChunk->have(recordsInChunk);
      size_t curMaxDelay = MIN_BACKOFF_ITERS;
      while (!outputQueue.try_enqueue(std::move(currentChunk))) {
        fastx_parser::thread_utils::backoffOrYield(curMaxDelay);
      }
      // Allocate next chunk
      currentChunk = get_chunk(chunkSize);
      recordsInChunk = 0;
    }
  }

  int result = 0;
  if (seq.err()) {
    result = -3;
  } else if (seq.tqs()) {
    result = -2;
  }

  // Flush remaining read in last chunk
  if (recordsInChunk > 0) {
    currentChunk->have(recordsInChunk);
    size_t curMaxDelay = MIN_BACKOFF_ITERS;
    while (!outputQueue.try_enqueue(std::move(currentChunk))) {
      fastx_parser::thread_utils::backoffOrYield(curMaxDelay);
    }
  }

  // Signal end-of-file with nullptr
  size_t curMaxDelay = MIN_BACKOFF_ITERS;
  while (!outputQueue.try_enqueue(nullptr)) {
    fastx_parser::thread_utils::backoffOrYield(curMaxDelay);
  }

  gzclose(fp);
  parsingDone = true;
  --numParsing;
  return result;
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
    while ((seq >> s->first()) and
           (seq2 >> s->second())) { // ksv >= 0 and ksv2 >= 0) {
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

// Template member function implementation for parallel parsing
template <typename T>
template <size_t N>
bool FastxParser<T>::start_parallel_parsing_impl(
    std::array<std::vector<std::string>*, N> inputStreamArrays) {
  
  if (numParsing_ != 0) {
    return false;
  }

  isActive_ = true;

  // Validate all file vectors have matching sizes
  size_t numFiles = inputStreamArrays[0]->size();
  for (size_t i = 1; i < N; ++i) {
    if (inputStreamArrays[i]->size() != numFiles) {
      throw std::invalid_argument(
          "All file vectors must have the same number of files");
    }
  }

  // Check for duplicate files
  for (size_t fileIdx = 0; fileIdx < numFiles; ++fileIdx) {
    for (size_t i = 0; i < N; ++i) {
      for (size_t j = i + 1; j < N; ++j) {
        if ((*inputStreamArrays[i])[fileIdx] == (*inputStreamArrays[j])[fileIdx]) {
          throw std::invalid_argument(
              "Same file provided for multiple reads: " + 
              (*inputStreamArrays[i])[fileIdx]);
        }
      }
    }
  }

  if (!parallelParsing_) {
    throw std::runtime_error(
        "Multi-file parsing with arity > 1 requires parallelParsing=true");
  }

  // N parsers + 1 assembler per file set
  threadResults_.resize(numFiles * (N + 1));
  std::fill(threadResults_.begin(), threadResults_.end(), 0);

  /*
  std::cerr << "Starting parallel parsing for " << numFiles << " file sets, " 
            << N << "-way reads, total threads: " << (numFiles * (N + 1)) << "\n";
*/
  constexpr size_t local_chunk_size = 512;

  for (size_t fn = 0; fn < numFiles; ++fn) {
    // Create queues for this file set - HEAP-ALLOCATE the arrays themselves
    auto queues = std::make_shared<std::array<std::shared_ptr<moodycamel::ConcurrentQueue<
        std::unique_ptr<ReadChunk<klibpp::KSeq>>>>, N>>();
    auto recycleQueues = std::make_shared<std::array<std::shared_ptr<moodycamel::ConcurrentQueue<
        std::unique_ptr<ReadChunk<klibpp::KSeq>>>>, N>>();
    auto doneFlags = std::make_shared<std::array<std::shared_ptr<std::atomic<bool>>, N>>();

    for (size_t i = 0; i < N; ++i) {
      (*queues)[i] = std::make_shared<moodycamel::ConcurrentQueue<
          std::unique_ptr<ReadChunk<klibpp::KSeq>>>>(local_chunk_size);
      (*recycleQueues)[i] = std::make_shared<moodycamel::ConcurrentQueue<
          std::unique_ptr<ReadChunk<klibpp::KSeq>>>>(local_chunk_size);
      (*doneFlags)[i] = std::make_shared<std::atomic<bool>>(false);
    }

    // Launch N parser threads
    for (size_t i = 0; i < N; ++i) {
      ++numParsing_;
      const std::string& filename = (*inputStreamArrays[i])[fn];
      //std::cerr << "Launching parser thread for file " << fn << ", stream " << i << ": " << filename << "\n";
      parsingThreads_.emplace_back(
          new std::thread([this, fn, i, queue = (*queues)[i], 
                          recycleQueue = (*recycleQueues)[i], 
                          done = (*doneFlags)[i], filename]() {  // Capture filename by value, not reference!
            /*
            std::cerr << "[Parser Thread " << std::this_thread::get_id() << "] Starting to parse: " 
                      << filename << "\n" << std::flush;
            */
            this->threadResults_[fn * (N + 1) + i] = 
                parse_single_file<klibpp::KSeq>(
                    filename, fn, this->numParsing_, *done, 
                    *queue, *recycleQueue, this->blockSize_);
            /*
            std::cerr << "[Parser Thread " << std::this_thread::get_id() << "] Finished parsing: " 
                      << filename << "\n" << std::flush;
            */
          }));
    }

    // Launch assembler thread - capture shared_ptrs BY VALUE (remove the & symbols!)
    ++numParsing_;
    size_t tokenIdx = fn % numParsers_;
    //std::cerr << "Launching assembler thread for file " << fn << "\n";
    parsingThreads_.emplace_back(
        new std::thread([this, fn, tokenIdx, queues, recycleQueues, 
                        doneFlags]() {
          //std::cerr << "[ASSEM-LAMBDA] Lambda started\n" << std::flush;
          this->threadResults_[fn * (N + 1) + N] = 
              thread_utils::assemble_read_set<T, N>(
                  *queues, *recycleQueues, *doneFlags,
                  this->consumeContainers_[tokenIdx].get(),
                  this->produceReads_[tokenIdx].get(),
                  this->seqContainerQueue_,
                  this->readQueue_,
                  fn,
                  this->numParsing_);  // Pass numParsing_ for decrement
          //std::cerr << "[ASSEM-LAMBDA] Lambda finished\n" << std::flush;
        }));
  }

  return true;
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
  if (parallelParsing_ && !inputStreams2_.empty()) {
    std::array<std::vector<std::string>*, 2> streams = {
        &inputStreams_, &inputStreams2_
    };
    return start_parallel_parsing_impl<2>(streams);
  } else {
    // Fall back to sequential parsing
    if (numParsing_ == 0) {
      isActive_ = true;
      if (inputStreams_.size() != inputStreams2_.size()) {
        throw std::invalid_argument(
            "There should be the same number of files for the left and right reads");
      }

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
      return true;
    }
    return false;
  }
}

template <> bool FastxParser<ReadQualPair>::start() {
  if (parallelParsing_ && !inputStreams2_.empty()) {
    std::array<std::vector<std::string>*, 2> streams = {
        &inputStreams_, &inputStreams2_
    };
    return start_parallel_parsing_impl<2>(streams);
  } else {
    // Fall back to sequential
    if (numParsing_ == 0) {
      isActive_ = true;
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
      return true;
    }
    return false;
  }
}

template <> bool FastxParser<ReadTriple>::start() {
  std::array<std::vector<std::string>*, 3> streams = {
      &inputStreams_, &inputStreams2_, &inputStreams3_
  };
  return start_parallel_parsing_impl<3>(streams);
}

template <> bool FastxParser<ReadQualTriple>::start() {
  std::array<std::vector<std::string>*, 3> streams = {
      &inputStreams_, &inputStreams2_, &inputStreams3_
  };
  return start_parallel_parsing_impl<3>(streams);
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

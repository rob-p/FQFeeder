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
    return -4; // File open error
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

// Helper to unpack a tuple of queues and call assemble_reads
template <typename T, typename... Queues>
int assemble_reads(
    std::tuple<Queues...>& queues, std::tuple<Queues...>& recycleQueues,
    const std::vector<std::shared_ptr<std::atomic<bool>>>& dones,
    moodycamel::ConsumerToken* cCont, moodycamel::ProducerToken* pRead,
    moodycamel::ConcurrentQueue<std::unique_ptr<ReadChunk<T>>>&
        seqContainerQueue,
    moodycamel::ConcurrentQueue<std::unique_ptr<ReadChunk<T>>>& readQueue,
    uint32_t file_idx, std::atomic<uint32_t>& numAssembling) {

  constexpr size_t Arity = std::tuple_size<std::tuple<Queues...>>::value;
  std::cerr << "assemble_reads called for file_idx " << file_idx << " Arity "
            << Arity << "\n";
  using fastx_parser::thread_utils::MIN_BACKOFF_ITERS;

  // Local chunks for each input queue
  std::array<std::unique_ptr<ReadChunk<klibpp::KSeq>>, Arity> chunks;
  std::array<size_t, Arity> indices;
  indices.fill(0);
  std::array<bool, Arity> filesDone;
  filesDone.fill(false);

  std::cerr << "assemble_reads: Waiting for seqContainerQueue\n";
  std::unique_ptr<ReadChunk<T>> local;
  size_t curMaxDelay = MIN_BACKOFF_ITERS;
  while (!seqContainerQueue.try_dequeue(*cCont, local)) {
    fastx_parser::thread_utils::backoffOrYield(curMaxDelay);
  }
  std::cerr << "assemble_reads: Got local chunk wrapper\n";
  size_t numObtained = local->size();
  size_t numWaiting = 0;
  uint64_t gathered_count = 0;

  auto fetch_chunk = [](auto queue, auto& chunk, size_t& idx, bool& done,
                      auto recycleQueue) {
    if (chunk && idx < chunk->size())
      return true;
    if (done)
      return false;

    std::unique_ptr<ReadChunk<klibpp::KSeq>> next_chunk;
    if (queue->try_dequeue(next_chunk)) {
      if (next_chunk == nullptr) {
        done = true;
        if (chunk)
          recycleQueue->enqueue(std::move(chunk));
        chunk = nullptr;
        return false;
      }
      if (chunk) {
        recycleQueue->enqueue(std::move(chunk));
      }
      chunk = std::move(next_chunk);
      idx = 0;
      return true;
    }
    return false;
  };

  while (true) {
    // Check all inputs
    bool allHave = true;
    bool allDone = true;

    // Use apply_indices to iterate
    // std::cerr << "assemble_reads: calling check_inputs\n";
    auto check_inputs = [&](auto... Is) {
      ((allHave &= fetch_chunk(std::get<Is.value>(queues), chunks[Is.value], indices[Is.value],
                               filesDone[Is.value], std::get<Is.value>(recycleQueues))),
        ...);
      ((allDone &= (filesDone[Is.value] && !chunks[Is.value])), ...);
    };

    apply_indices<Arity>([&](auto... args) { check_inputs(args...); });

    // std::cerr << "assemble_reads: check_inputs done. allHave=" << allHave <<
    // " allDone=" << allDone << "\n";

    if (allHave) {
      // Assemble
        std::cerr << "About to get readUnion reference\n";
  T& readUnion = (*local)[numWaiting];
  std::cerr << "Got readUnion reference\n";

      auto assign = [&](auto... Is) {
        ((ReadTrait<T>::get(readUnion, Is.value) = 
          std::move((*chunks[Is.value])[indices[Is.value]++])),
          ...);
      };

      std::cerr << "About to call apply_indices for assign\n";
      apply_indices<Arity>([&](auto... args) { 
        std::cerr << "Inside apply_indices assign\n";
        assign(args...); 
      });
      std::cerr << "assign completed\n";

      /*
      // Assemble
      T& readUnion = (*local)[numWaiting];

      auto assign = [&](auto... Is) {
        ((ReadTrait<T>::get(readUnion, Is) =
              std::move((*chunks[Is])[indices[Is]++])),
         ...);
      };
      apply_indices<Arity>([&](auto... args) { assign(args...); });
      */
      ++numWaiting;
      ++gathered_count;

      if (numWaiting == numObtained) {
        local->have(numWaiting);
        local->set_chunk_frag_offset(file_idx, gathered_count - numWaiting);
        curMaxDelay = MIN_BACKOFF_ITERS;
        while (!readQueue.try_enqueue(*pRead, std::move(local))) {
          fastx_parser::thread_utils::backoffOrYield(curMaxDelay);
        }

        // Get next output chunk
        numWaiting = 0;
        curMaxDelay = MIN_BACKOFF_ITERS;
        while (!seqContainerQueue.try_dequeue(*cCont, local)) {
          fastx_parser::thread_utils::backoffOrYield(curMaxDelay);
        }
        numObtained = local->size();
      }
    } else {
      // Should yield if waiting for input
      size_t kBackoff = MIN_BACKOFF_ITERS;
      fastx_parser::thread_utils::backoffOrYield(kBackoff);
    }

    if (allDone)
      break;
  }

  // Flush remaining
  if (numWaiting > 0) {
    local->have(numWaiting);
    local->set_chunk_frag_offset(file_idx, gathered_count - numWaiting);
    curMaxDelay = MIN_BACKOFF_ITERS;
    while (!readQueue.try_enqueue(*pRead, std::move(local))) {
      fastx_parser::thread_utils::backoffOrYield(curMaxDelay);
    }
  }

  --numAssembling;
  return 0;
}

template <typename T>
template <typename... FileGroups>
FastxParser<T>::FastxParser(uint32_t numConsumers, uint32_t numParsers,
                            uint32_t chunkSize, bool parallelParsing,
                            FileGroups... fileGroups)
    : numParsers_(numParsers), numParsing_(0),
      parallelParsing_(parallelParsing), blockSize_(chunkSize) {

  // Unpack fileGroups into inputStreams_ array safely
  size_t idx = 0;
  auto safe_assign = [&](const auto& group) {
    if (idx < ReadTrait<T>::arity) {
      inputStreams_[idx] = group;
    }
    idx++;
  };
  ((safe_assign(fileGroups)), ...);

  if (idx != ReadTrait<T>::arity) {
    // It implies that the user called a constructor that supplies more or fewer
    // file groups than the ReadTrait expects.
    // E.g. FastxParser<ReadSeq>(files1, files2, ...) -> Arity 1, provided 2.
    // E.g. FastxParser<ReadPair>(files1) -> Arity 2, provided 1.
    std::string msg = "Incorrect number of file groups provided. Expected " +
                      std::to_string(ReadTrait<T>::arity) + ", got " +
                      std::to_string(idx);
    throw std::invalid_argument(msg);
  }

  // Validate all file groups have same size
  size_t numFiles = inputStreams_[0].files.size();
  for (size_t i = 1; i < ReadTrait<T>::arity; ++i) {
    if (inputStreams_[i].files.size() != numFiles) {
      throw std::invalid_argument(
          "All file groups must have the same number of files");
    }
  }

  if (numParsers > numFiles) {
    std::cerr << "Can't make use of more parsing threads than file groups; "
                 "setting # of parsing threads to "
              << numFiles << '\n';
    numParsers_ = numFiles;
  }

  readQueue_ = moodycamel::ConcurrentQueue<std::unique_ptr<ReadChunk<T>>>(
      4 * numConsumers, numParsers_, 0);

  seqContainerQueue_ =
      moodycamel::ConcurrentQueue<std::unique_ptr<ReadChunk<T>>>(
          4 * numConsumers, 1 + numConsumers, 0);

  workQueue_ = moodycamel::ConcurrentQueue<uint32_t>(numParsers_);

  for (size_t i = 0; i < numFiles; ++i) {
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

// Legacy constructors delegation
template <typename T>
FastxParser<T>::FastxParser(std::vector<std::string> files,
                            uint32_t numConsumers, uint32_t numParsers,
                            uint32_t chunkSize)
    : FastxParser(numConsumers, numParsers, chunkSize, true, FileGroup{files}) {
}

template <typename T>
FastxParser<T>::FastxParser(std::vector<std::string> files,
                            std::vector<std::string> files2,
                            uint32_t numConsumers, uint32_t numParsers,
                            uint32_t chunkSize, bool parallelParsing)
    : FastxParser(numConsumers, numParsers, chunkSize, parallelParsing,
                  FileGroup{files}, FileGroup{files2}) {}

template <typename T>
FastxParser<T>::FastxParser(std::vector<std::string> files,
                            std::vector<std::string> files2,
                            std::vector<std::string> files3,
                            uint32_t numConsumers, uint32_t numParsers,
                            uint32_t chunkSize, bool parallelParsing)
    : FastxParser(numConsumers, numParsers, chunkSize, parallelParsing,
                  FileGroup{files}, FileGroup{files2}, FileGroup{files3}) {}

// Start implementation
template <typename T> bool FastxParser<T>::start() {
  if (numParsing_ == 0) {
    isActive_ = true;
    constexpr size_t Arity = ReadTrait<T>::arity;

    // Validation of input streams basic sanity done in constructor mostly
    // but let's check duplicates
    // ... (omitted for brevity, can look at diff)

    size_t numFileGroups = inputStreams_[0].files.size();

    if (parallelParsing_) {
      size_t numThreadsPerGroup = Arity + 1; // parsers + 1 assembler
      threadResults_.resize(numFileGroups * numThreadsPerGroup);
      std::fill(threadResults_.begin(), threadResults_.end(), 0);

      constexpr size_t local_chunk_size = 512;

      for (size_t fn = 0; fn < numFileGroups; ++fn) {

        // Queues for this group
        std::vector<std::shared_ptr<moodycamel::ConcurrentQueue<
            std::unique_ptr<ReadChunk<klibpp::KSeq>>>>>
            queues;
        std::vector<std::shared_ptr<moodycamel::ConcurrentQueue<
            std::unique_ptr<ReadChunk<klibpp::KSeq>>>>>
            recycleQueues;
        std::vector<std::shared_ptr<std::atomic<bool>>> dones;

        for (size_t i = 0; i < Arity; ++i) {
          queues.push_back(
              std::make_shared<moodycamel::ConcurrentQueue<
                  std::unique_ptr<ReadChunk<klibpp::KSeq>>>>(local_chunk_size));
          recycleQueues.push_back(
              std::make_shared<moodycamel::ConcurrentQueue<
                  std::unique_ptr<ReadChunk<klibpp::KSeq>>>>(local_chunk_size));
          dones.push_back(std::make_shared<std::atomic<bool>>(false));

          // Launch parser
          ++numParsing_;
          std::cerr << "Launching parser thread group " << fn << " file " << i
                    << "\n";
          parsingThreads_.emplace_back(
              new std::thread([this, fn, i, queues, recycleQueues, dones, Arity]() {
                this->threadResults_[fn * (Arity + 1) + i] =
                    parse_single_file<klibpp::KSeq>(
                        this->inputStreams_[i].files[fn], fn, this->numParsing_,
                        *dones[i], *queues[i], *recycleQueues[i],
                        this->blockSize_);
              }));
        }

        // Launch assembler
        ++numParsing_;
        auto numAssembling = std::make_shared<std::atomic<uint32_t>>(1);
        size_t tokenIdx = fn % numParsers_;

        parsingThreads_.emplace_back(
            new std::thread([this, fn, queues, recycleQueues, dones, tokenIdx,
                             numAssembling, Arity]() {
              // Helper to convert vector to tuple and call assemble_reads
              auto convert_and_call = [&](auto... Is) {
                auto queue_tuple = std::make_tuple(queues[Is].get()...);
                auto recycle_tuple =
                    std::make_tuple(recycleQueues[Is].get()...);

                this->threadResults_[fn * (Arity + 1) + Arity] =
                    assemble_reads<T>(queue_tuple, recycle_tuple, dones,
                                      this->consumeContainers_[tokenIdx].get(),
                                      this->produceReads_[tokenIdx].get(),
                                      this->seqContainerQueue_,
                                      this->readQueue_, fn, *numAssembling);
              };

              apply_indices<Arity>(
                  [&](auto... args) { convert_and_call(args...); });
            }));
      }
    } else {
      // Logic for non-parallel parsing if needed; currently parallel is forced
      // or we return false/fallthrough?
      // In the previous revision, strict non-parallel wasn't fully supported or
      // was distinct.
      // For Arity > 1, parallel is the standard way here.
    }
    return true;
  }
  return false;
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

// Instantiate templates
/*
template class FastxParser<ReadSeq>;
template class FastxParser<ReadPair>;
template class FastxParser<ReadTriple>;
// template class FastxParser<ReadQual>; // Duplicate of ReadSeq
template class FastxParser<ReadQualPair>;
template class FastxParser<ReadQualTriple>;
*/

// Helper to generate tuple types with N queue pointers
template<size_t N>
struct QueueTuple {
    using type = decltype(std::tuple_cat(
        std::declval<typename QueueTuple<N-1>::type>(),
        std::declval<std::tuple<moodycamel::ConcurrentQueue<std::unique_ptr<ReadChunk<klibpp::KSeq>>>*>>()
    ));
};

template<>
struct QueueTuple<1> {
    using type = std::tuple<moodycamel::ConcurrentQueue<std::unique_ptr<ReadChunk<klibpp::KSeq>>>*>;
};

template<>
struct QueueTuple<0> {
    using type = std::tuple<>;
};

// Explicitly instantiate parse_single_file for KSeq
template int parse_single_file<klibpp::KSeq>(
    const std::string& filename, uint32_t file_idx,
    std::atomic<uint32_t>& numParsing, std::atomic<bool>& parsingDone,
    moodycamel::ConcurrentQueue<std::unique_ptr<ReadChunk<klibpp::KSeq>>>& outputQueue,
    moodycamel::ConcurrentQueue<std::unique_ptr<ReadChunk<klibpp::KSeq>>>& recycleQueue,
    uint32_t chunkSize);

// Macro to instantiate assemble_reads for ReadSet<N>
#define INSTANTIATE_ASSEMBLE_READS_N(N) \
template int assemble_reads<ReadSet<N>>( \
    typename QueueTuple<N>::type& queues, \
    typename QueueTuple<N>::type& recycleQueues, \
    const std::vector<std::shared_ptr<std::atomic<bool>>>& dones, \
    moodycamel::ConsumerToken* cCont, \
    moodycamel::ProducerToken* pRead, \
    moodycamel::ConcurrentQueue<std::unique_ptr<ReadChunk<ReadSet<N>>>>& seqContainerQueue, \
    moodycamel::ConcurrentQueue<std::unique_ptr<ReadChunk<ReadSet<N>>>>& readQueue, \
    uint32_t file_idx, \
    std::atomic<uint32_t>& numAssembling);

// Macro for ReadQualSet<N>
#define INSTANTIATE_ASSEMBLE_READS_QUAL_N(N) \
template int assemble_reads<ReadQualSet<N>>( \
    typename QueueTuple<N>::type& queues, \
    typename QueueTuple<N>::type& recycleQueues, \
    const std::vector<std::shared_ptr<std::atomic<bool>>>& dones, \
    moodycamel::ConsumerToken* cCont, \
    moodycamel::ProducerToken* pRead, \
    moodycamel::ConcurrentQueue<std::unique_ptr<ReadChunk<ReadQualSet<N>>>>& seqContainerQueue, \
    moodycamel::ConcurrentQueue<std::unique_ptr<ReadChunk<ReadQualSet<N>>>>& readQueue, \
    uint32_t file_idx, \
    std::atomic<uint32_t>& numAssembling);

// Instantiate for single reads (KSeq) - using ReadSeq alias
template int assemble_reads<klibpp::KSeq>(
    typename QueueTuple<1>::type& queues,
    typename QueueTuple<1>::type& recycleQueues,
    const std::vector<std::shared_ptr<std::atomic<bool>>>& dones,
    moodycamel::ConsumerToken* cCont,
    moodycamel::ProducerToken* pRead,
    moodycamel::ConcurrentQueue<std::unique_ptr<ReadChunk<klibpp::KSeq>>>& seqContainerQueue,
    moodycamel::ConcurrentQueue<std::unique_ptr<ReadChunk<klibpp::KSeq>>>& readQueue,
    uint32_t file_idx,
    std::atomic<uint32_t>& numAssembling);

// Instantiate ReadSet for arities 2-8
INSTANTIATE_ASSEMBLE_READS_N(2)
INSTANTIATE_ASSEMBLE_READS_N(3)
INSTANTIATE_ASSEMBLE_READS_N(4)
INSTANTIATE_ASSEMBLE_READS_N(5)
INSTANTIATE_ASSEMBLE_READS_N(6)
INSTANTIATE_ASSEMBLE_READS_N(7)
INSTANTIATE_ASSEMBLE_READS_N(8)

// Instantiate ReadQualSet for arities 2-8
INSTANTIATE_ASSEMBLE_READS_QUAL_N(2)
INSTANTIATE_ASSEMBLE_READS_QUAL_N(3)
INSTANTIATE_ASSEMBLE_READS_QUAL_N(4)
INSTANTIATE_ASSEMBLE_READS_QUAL_N(5)
INSTANTIATE_ASSEMBLE_READS_QUAL_N(6)
INSTANTIATE_ASSEMBLE_READS_QUAL_N(7)
INSTANTIATE_ASSEMBLE_READS_QUAL_N(8)

#undef INSTANTIATE_ASSEMBLE_READS_N
#undef INSTANTIATE_ASSEMBLE_READS_QUAL_N

// Instantiate FastxParser for all types
template class FastxParser<klibpp::KSeq>;
template class FastxParser<ReadSet<2>>;
template class FastxParser<ReadSet<3>>;
template class FastxParser<ReadSet<4>>;
template class FastxParser<ReadSet<5>>;
template class FastxParser<ReadSet<6>>;
template class FastxParser<ReadSet<7>>;
template class FastxParser<ReadSet<8>>;
template class FastxParser<ReadQualSet<2>>;
template class FastxParser<ReadQualSet<3>>;
template class FastxParser<ReadQualSet<4>>;
template class FastxParser<ReadQualSet<5>>;
template class FastxParser<ReadQualSet<6>>;
template class FastxParser<ReadQualSet<7>>;
template class FastxParser<ReadQualSet<8>>;

} // namespace fastx_parser

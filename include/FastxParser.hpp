#ifndef __FASTX_PARSER__
#define __FASTX_PARSER__

#include "fcntl.h"
#include "unistd.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <vector>
#include <utility>
#include <memory>
#include "kseq++.hpp"
#include "concurrentqueue.h"

using std::make_unique;

namespace fastx_parser {

// holds a "set" of files that correspond to components (in different files)
// of the same fragment. For single-end reads, this is just a file, for 
// paired-end reads, it is a pair of files, etc.
struct FileGroup {
  template<typename... Strings>
  explicit FileGroup(Strings&&... strs)
    : file_names{std::forward<Strings>(strs)...}
    , arity(sizeof...(strs))
  {}

  template<typename Iterator>
  FileGroup(Iterator first, Iterator last)
    : file_names(first, last)
    , arity(file_names.size())
  {}

  std::vector<std::string> file_names;
  size_t arity{0};
};

template <typename T> 
struct ReadTrait;

// Generic ReadSet that works for any arity
template <size_t N>
struct ReadSet {
    std::array<klibpp::KSeq, N> reads;
    
    // Array-like access
    klibpp::KSeq& operator[](size_t i) { return reads[i]; }
    const klibpp::KSeq& operator[](size_t i) const { return reads[i]; }
    
    // Named accessors for convenience (only enabled when N is large enough)
    template<size_t M = N, typename = std::enable_if_t<(M >= 1)>>
    klibpp::KSeq& first() { return reads[0]; }
    
    template<size_t M = N, typename = std::enable_if_t<(M >= 2)>>
    klibpp::KSeq& second() { return reads[1]; }
    
    template<size_t M = N, typename = std::enable_if_t<(M >= 3)>>
    klibpp::KSeq& third() { return reads[2]; }
    
    template<size_t M = N, typename = std::enable_if_t<(M >= 1)>>
    const klibpp::KSeq& first() const { return reads[0]; }
    
    template<size_t M = N, typename = std::enable_if_t<(M >= 2)>>
    const klibpp::KSeq& second() const { return reads[1]; }
    
    template<size_t M = N, typename = std::enable_if_t<(M >= 3)>>
    const klibpp::KSeq& third() const { return reads[2]; }
};

// Specialization of ReadTrait for ReadSet<N>
template <size_t N>
struct ReadTrait<ReadSet<N>> {
    static constexpr size_t arity = N;
    static klibpp::KSeq& get(ReadSet<N>& t, size_t i) { return t[i]; }
};

// If you want to distinguish qual vs non-qual types:
template <size_t N>
struct ReadQualSet {
    std::array<klibpp::KSeq, N> reads;
    
    klibpp::KSeq& operator[](size_t i) { return reads[i]; }
    const klibpp::KSeq& operator[](size_t i) const { return reads[i]; }
    
    template<size_t M = N, typename = std::enable_if_t<(M >= 1)>>
    klibpp::KSeq& first() { return reads[0]; }
    
    template<size_t M = N, typename = std::enable_if_t<(M >= 2)>>
    klibpp::KSeq& second() { return reads[1]; }
    
    template<size_t M = N, typename = std::enable_if_t<(M >= 3)>>
    klibpp::KSeq& third() { return reads[2]; }
};

// Specialization of ReadTrait for ReadQualSet<N>
template <size_t N>
struct ReadTrait<ReadQualSet<N>> {
    static constexpr size_t arity = N;
    static klibpp::KSeq& get(ReadQualSet<N>& t, size_t i) { return t[i]; }
};

// Specialization for KSeq (single read) - keep this for backward compatibility
template <>
struct ReadTrait<klibpp::KSeq> {
    static constexpr size_t arity = 1;
    static klibpp::KSeq& get(klibpp::KSeq& t, size_t) { return t; }
};

// Type aliases for convenience and backward compatibility
using ReadSeq = klibpp::KSeq;
using ReadPair = ReadSet<2>;
using ReadTriple = ReadSet<3>;
/*
using ReadQuad = ReadSet<4>;
using ReadQuint = ReadSet<5>;
using ReadSextuple = ReadSet<6>;
using ReadSeptuple = ReadSet<7>;
using ReadOctuple = ReadSet<8>;
using ReadQualQuad = ReadQualSet<4>;
*/

using ReadQualPair = ReadQualSet<2>;
using ReadQualTriple = ReadQualSet<3>;

// Intermediate structure for parallel parsing - no longer needed with
// chunk-based queues template <typename T> struct ParsedSingleRead { ... }

struct ChunkFragOffset {
  uint32_t file_idx{0};
  uint32_t frag_idx{0};
};

template <typename T> class ReadChunk {
public:
  ReadChunk(size_t want) : group_(want), want_(want), have_(want) {}
  inline void have(size_t num) { have_ = num; }
  inline size_t size() { return have_; }
  inline size_t want() const { return want_; }
  T& operator[](size_t i) { return group_[i]; }
  typename std::vector<T>::iterator begin() { return group_.begin(); }
  typename std::vector<T>::iterator end() { return group_.begin() + have_; }

  void set_chunk_frag_offset(uint32_t file_num, uint64_t frag_num) {
    frag_offset_.file_idx = file_num;
    frag_offset_.frag_idx = frag_num;
  }

  ChunkFragOffset chunk_frag_offset() const { return frag_offset_; }

private:
  std::vector<T> group_;
  size_t want_;
  size_t have_;
  ChunkFragOffset frag_offset_;
};

template <typename T> class ReadGroup {
public:
  ReadGroup(moodycamel::ProducerToken&& pt, moodycamel::ConsumerToken&& ct)
      : pt_(std::move(pt)), ct_(std::move(ct)) {}
  moodycamel::ConsumerToken& consumerToken() { return ct_; }
  moodycamel::ProducerToken& producerToken() { return pt_; }
  // get a reference to the chunk this ReadGroup owns
  std::unique_ptr<ReadChunk<T>>& chunkPtr() { return chunk_; }
  // get a *moveable* reference to the chunk this ReadGroup owns
  std::unique_ptr<ReadChunk<T>>&& takeChunkPtr() { return std::move(chunk_); }
  inline void have(size_t num) { chunk_->have(num); }
  inline size_t size() { return chunk_->size(); }
  inline size_t want() const { return chunk_->want(); }
  T& operator[](size_t i) { return (*chunk_)[i]; }
  typename std::vector<T>::iterator begin() { return chunk_->begin(); }
  typename std::vector<T>::iterator end() {
    return chunk_->begin() + chunk_->size();
  }
  void setChunkEmpty() { chunk_.release(); }
  bool empty() const { return chunk_.get() == nullptr; }
  ChunkFragOffset chunk_frag_offset() const {
    return chunk_->chunk_frag_offset();
  }

private:
  std::unique_ptr<ReadChunk<T>> chunk_{nullptr};
  moodycamel::ProducerToken pt_;
  moodycamel::ConsumerToken ct_;
};

template <typename T> class FastxParser {
public:
  FastxParser(std::vector<std::string> files, uint32_t numConsumers,
              uint32_t numParsers = 1, uint32_t chunkSize = 1000);

  FastxParser(std::vector<std::string> files, std::vector<std::string> files2,
              uint32_t numConsumers, uint32_t numParsers = 1,
              uint32_t chunkSize = 1000, bool parallelParsing = true);

  // Triplet constructor for protocols with 3 synchronized files
  FastxParser(std::vector<std::string> files, std::vector<std::string> files2,
              std::vector<std::string> files3, uint32_t numConsumers,
              uint32_t numParsers = 1, uint32_t chunkSize = 1000,
              bool parallelParsing = true);
  ~FastxParser();
  bool start();
  bool stop();
  ReadGroup<T> getReadGroup();
  bool refill(ReadGroup<T>& rg);
  void finishedWithGroup(ReadGroup<T>& s);

private:
  moodycamel::ProducerToken getProducerToken_();
  moodycamel::ConsumerToken getConsumerToken_();

  std::vector<std::string> inputStreams_;
  std::vector<std::string> inputStreams2_;
  std::vector<std::string> inputStreams3_; // For triplet files
  uint32_t numParsers_;
  std::atomic<uint32_t> numParsing_;
  bool parallelParsing_{true}; // Enable parallel parsing for multi-file modes

  // NOTE: Would like to use std::future<int> here instead, but that
  // solution doesn't seem to work.  It's unclear exactly why
  // see (https://twitter.com/nomad421/status/917748383321817088)
  std::vector<std::unique_ptr<std::thread>> parsingThreads_;

  // holds the results of the parsing threads, which is simply equal to
  // the return value of kseq_read() for the last call to that function.
  // A value < -1 signifies some sort of error.
  std::vector<int> threadResults_;

  size_t blockSize_;
  moodycamel::ConcurrentQueue<std::unique_ptr<ReadChunk<T>>> readQueue_,
      seqContainerQueue_;

  // holds the indices of files (file-pairs) to be processed
  moodycamel::ConcurrentQueue<uint32_t> workQueue_;

  std::vector<std::unique_ptr<moodycamel::ProducerToken>> produceReads_;
  std::vector<std::unique_ptr<moodycamel::ConsumerToken>> consumeContainers_;
  bool isActive_{false};

  // Helper for parallel parsing of N-way read sets
  template <size_t N>
  bool start_parallel_parsing_impl(std::array<std::vector<std::string>*, N> inputStreamArrays);
};
} // namespace fastx_parser

#endif // __FASTX_PARSER__

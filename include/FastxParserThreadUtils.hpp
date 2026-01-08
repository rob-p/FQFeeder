#ifndef FASTX_PARSER_THREAD_UTILS_HPP
#define FASTX_PARSER_THREAD_UTILS_HPP

#include <cassert>
#include <chrono>
#include <pthread.h>
#include <random>
#include <thread>

#if defined(__SSE2__)
 #if defined(HAVE_SIMDE)
  #include "simde/x86/sse2.h"
 #else
  #include <emmintrin.h>
 #endif
#endif

// Most of this code is taken directly from
// https://github.com/geidav/spinlocks-bench/blob/master/os.hpp. However, things
// may be renamed, modified, or randomly mangled over time.
#define ALWAYS_INLINE inline __attribute__((__always_inline__))

namespace fastx_parser {
namespace thread_utils {

static const constexpr size_t MIN_BACKOFF_ITERS = 32;
static const size_t MAX_BACKOFF_ITERS = 1024;

ALWAYS_INLINE static void cpuRelax() {
#if defined(__SSE2__)  // AMD and Intel
  #if defined(HAVE_SIMDE)
    simde_mm_pause();
  #else
    _mm_pause();
  #endif
#elif defined(__i386__) || defined(__x86_64__)
  asm volatile("pause");
#elif defined(__aarch64__)
  asm volatile("isb");
#elif defined(__armel__) || defined(__ARMEL__)
  asm volatile ("nop" ::: "memory");
#elif defined(__arm__) 
  __asm__ __volatile__ ("yield" ::: "memory");
#elif defined(__ia64__)  // IA64
  __asm__ __volatile__ ("hint @pause");
#elif defined(__powerpc__) || defined(__ppc__) || defined(__PPC__)
   __asm__ __volatile__ ("or 27,27,27" ::: "memory");
#else  // everything else.
   asm volatile ("nop" ::: "memory");
#endif
}

ALWAYS_INLINE void yieldSleep() {
  using namespace std::chrono;
  std::chrono::microseconds ytime(500);
  std::this_thread::sleep_for(ytime);
}

ALWAYS_INLINE void backoffExp(size_t& curMaxIters) {
  thread_local std::uniform_int_distribution<size_t> dist;

  // see : https://github.com/coryan/google-cloud-cpp-common/blob/a6e7b6b362d72451d6dc1fec5bc7643693dbea96/google/cloud/internal/random.cc
  #if defined(__linux) && defined(__GLIBCXX__) && __GLIBCXX__ >= 20200128
    thread_local std::random_device rd("/dev/urandom");
  #else
    thread_local std::random_device rd;
  #endif  // defined(__GLIBCXX__) && __GLIBCXX__ >= 20200128

  thread_local std::minstd_rand gen(rd());
  const size_t spinIters =
      dist(gen, decltype(dist)::param_type{0, curMaxIters});
  curMaxIters = std::min(2 * curMaxIters, MAX_BACKOFF_ITERS);
  for (size_t i = 0; i < spinIters; i++) {
    cpuRelax();
  }
}

ALWAYS_INLINE void backoffOrYield(size_t& curMaxDelay) {
  if (curMaxDelay >= MAX_BACKOFF_ITERS) {
    yieldSleep();
    curMaxDelay = MIN_BACKOFF_ITERS;
  }
  backoffExp(curMaxDelay);
}

// Generic assembler for N-way read sets
template <typename T, size_t N>
int assemble_read_set(
    std::array<std::shared_ptr<moodycamel::ConcurrentQueue<
        std::unique_ptr<ReadChunk<klibpp::KSeq>>>>, N>& queues,
    std::array<std::shared_ptr<moodycamel::ConcurrentQueue<
        std::unique_ptr<ReadChunk<klibpp::KSeq>>>>, N>& recycleQueues,
    std::array<std::shared_ptr<std::atomic<bool>>, N>& doneFlags,
    moodycamel::ConsumerToken* cCont,
    moodycamel::ProducerToken* pRead,
    moodycamel::ConcurrentQueue<std::unique_ptr<ReadChunk<T>>>& seqContainerQueue,
    moodycamel::ConcurrentQueue<std::unique_ptr<ReadChunk<T>>>& readQueue,
    uint32_t file_idx,
    std::atomic<uint32_t>& numParsing) {  // Changed from numAssembling

  std::array<std::unique_ptr<ReadChunk<klibpp::KSeq>>, N> chunks;
  alignas(64) std::array<size_t, N> indices{};
  alignas(64) std::array<bool, N> fileDone{};
  
  //std::cerr << "[Thread " << std::this_thread::get_id() << "] Assembler for file " 
  //        << file_idx << " started (" << N << "-way)\n" << std::flush;
  
  //std::cerr << "[ASSEM] Step 1: Arrays created\n" << std::flush;
  
  // Get initial output chunk
  std::unique_ptr<ReadChunk<T>> local;
  
  //std::cerr << "[ASSEM] Step 2: About to get initial chunk from seqContainerQueue\n" << std::flush;
  
  size_t curMaxDelay = MIN_BACKOFF_ITERS;
  
  //std::cerr << "[ASSEM] Step 3: About to try_dequeue, cCont=" << cCont << "\n" << std::flush;
  
  // Try without token first to debug
  bool got_chunk = seqContainerQueue.try_dequeue(*cCont, local);
  
  //std::cerr << "[ASSEM] Step 4: try_dequeue returned " << got_chunk << "\n" << std::flush;
  
  if (!got_chunk) {
    //std::cerr << "[ASSEM] Step 5: Entering wait loop\n" << std::flush;
    while (!seqContainerQueue.try_dequeue(*cCont, local)) {
      backoffOrYield(curMaxDelay);
    }
    //std::cerr << "[ASSEM] Step 6: Got chunk after waiting\n" << std::flush;
  }
  
  //std::cerr << "[ASSEM] Step 7: Got initial output chunk, size=" << local->size() << "\n" << std::flush;
  
  /*
  std::cerr << "[ASSEM] Checking queues array:\n" << std::flush;
  for (size_t i = 0; i < N; ++i) {
    std::cerr << "[ASSEM]   queues[" << i << "] = " << queues[i].get() << "\n" << std::flush;
    std::cerr << "[ASSEM]   recycleQueues[" << i << "] = " << recycleQueues[i].get() << "\n" << std::flush;
    std::cerr << "[ASSEM]   doneFlags[" << i << "] = " << doneFlags[i].get() << "\n" << std::flush;
  }
  */
  
  size_t numObtained = local->size();
  size_t numWaiting = 0;
  uint64_t gathered_count = 0;

  // Lambda to fetch chunk from a specific queue
  auto fetch_chunk = [&](size_t idx) -> bool {
    if (chunks[idx] && indices[idx] < chunks[idx]->size())
      return true;
    if (fileDone[idx])
      return false;

    std::unique_ptr<ReadChunk<klibpp::KSeq>> next_chunk;
    // Access the shared_ptr at queues[idx] and dereference it
    if (queues[idx]->try_dequeue(next_chunk)) {
      if (next_chunk == nullptr) {
        fileDone[idx] = true;
        if (chunks[idx])
          recycleQueues[idx]->enqueue(std::move(chunks[idx]));
        chunks[idx] = nullptr;
        return false;
      }
      if (chunks[idx]) {
        recycleQueues[idx]->enqueue(std::move(chunks[idx]));
      }
      chunks[idx] = std::move(next_chunk);
      indices[idx] = 0;
      return true;
    }
    return false;
  };

  // Check if all files are done
  auto all_done = [&]() {
    for (size_t i = 0; i < N; ++i) {
      if (!fileDone[i] || chunks[i])
        return false;
    }
    return true;
  };

  while (!all_done()) {
    // Try to fetch from all queues to update their done status
    std::array<bool, N> haveData;
    for (size_t i = 0; i < N; ++i) {
      haveData[i] = fetch_chunk(i);
    }
    
    // Check if all queues have data
    bool allHaveData = true;
    for (size_t i = 0; i < N; ++i) {
      if (!haveData[i]) {
        allHaveData = false;
        break;
      }
    }
    
    if (allHaveData) {
      /*
       // Ensure that the ranks in each chunk match
      size_t first_rank = std::numeric_limits<size_t>::max();
      for (size_t i = 0; i < N; ++i) {
        if (first_rank == std::numeric_limits<size_t>::max()) {
          first_rank = chunks[i]->chunk_frag_offset().frag_idx;
        }
        if (chunks[i]->chunk_frag_offset().frag_idx != first_rank) {
          std::cerr << "[ERROR]: Rank of first chunk in this set was " << first_rank << ", but part " << i << " has rank " << chunks[i]->chunk_frag_offset().frag_idx << "\n";
        }
      }
      */
      // Assemble N-tuple
      T& readSet = (*local)[numWaiting];
      for (size_t i = 0; i < N; ++i) {
        readSet[i] = std::move((*chunks[i])[indices[i]++]);
      }
      ++numWaiting;
      ++gathered_count;
      
      /*
      if (numWaiting % 100 == 0) {
        std::cerr << "[Thread " << std::this_thread::get_id() << "] Assembled " 
                  << numWaiting << " read sets\n" << std::flush;
      }
      */

      if (numWaiting == numObtained) {
        local->have(numWaiting);
        local->set_chunk_frag_offset(file_idx, gathered_count - numWaiting);
        
        curMaxDelay = MIN_BACKOFF_ITERS;
        while (!readQueue.try_enqueue(*pRead, std::move(local))) {
          backoffOrYield(curMaxDelay);
        }

        // Get next output chunk
        numWaiting = 0;
        curMaxDelay = MIN_BACKOFF_ITERS;
        while (!seqContainerQueue.try_dequeue(*cCont, local)) {
          backoffOrYield(curMaxDelay);
        }
        numObtained = local->size();
      }
    } else {
      // Not all queues have data, but check if we're done before backing off
      if (all_done()) {
        break;
      }
      size_t kBackoff = MIN_BACKOFF_ITERS;
      backoffOrYield(kBackoff);
    }
  }

  // Flush remaining
  if (numWaiting > 0) {
    local->have(numWaiting);
    local->set_chunk_frag_offset(file_idx, gathered_count - numWaiting);
    curMaxDelay = MIN_BACKOFF_ITERS;
    while (!readQueue.try_enqueue(*pRead, std::move(local))) {
      backoffOrYield(curMaxDelay);
    }
  } else {
    curMaxDelay = MIN_BACKOFF_ITERS;
    while (!seqContainerQueue.try_enqueue(std::move(local))) {
      backoffOrYield(curMaxDelay);
    }
  }

  --numParsing;  // Changed from numAssembling
  /*
  std::cerr << "[Thread " << std::this_thread::get_id() << "] Assembler for file " 
            << file_idx << " finished, numParsing=" << numParsing.load() << "\n" << std::flush;
  */
  return 0;
}

} // namespace thread_utils
} // namespace fastx_parser

#endif // FASTX_PARSER_THREAD_UTILS_HPP

#include "FastxParser.hpp"

#include <zlib.h>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <thread>
#include <atomic>
#include <iostream>
#include "unistd.h"
#include "fcntl.h"
#include <poll.h>

// STEP 1: declare the type of file handler and the read() function
KSEQ_INIT(gzFile, gzread)

template <typename T>
FastxParser<T>::FastxParser( std::vector<std::string> files, uint32_t numReaders) :
FastxParser(files, {}, numReaders) {}

template <typename T>
FastxParser<T>::FastxParser( std::vector<std::string> files, std::vector<std::string> files2, uint32_t numReaders): 
inputStreams_(files),
inputStreams2_(files2),
    parsing_(false), parsingThread_(nullptr)
{
    blockSize_ = 1000;
    queueCapacity_ = blockSize_ * numReaders;
    readChunks_.resize(numReaders);
    readQueue_ = moodycamel::ConcurrentQueue<ReadChunk<T>*>(queueCapacity_, 1 + numReaders, 0);
    seqContainerQueue_ = moodycamel::ConcurrentQueue<ReadChunk<T>*>(queueCapacity_, 1 + numReaders, 0);

	produceContainer_.reset(new moodycamel::ProducerToken(seqContainerQueue_));
	consumeContainer_.reset(new moodycamel::ConsumerToken(seqContainerQueue_));

	produceReads_.reset(new moodycamel::ProducerToken(readQueue_));
	consumeReads_.reset(new moodycamel::ConsumerToken(readQueue_));

    for (size_t i = 0; i < numReaders; ++i) {
        readChunks_[i] = new ReadChunk<T>(1000);
        while (!seqContainerQueue_.try_enqueue(*produceContainer_, readChunks_[i])) {
            std::cerr << "Could not enqueue " << i << "!\n";
            std::cerr << "capacity = " << numReaders << '\n';
        }
    }
}

template <typename T>
ReadGroup<T> FastxParser<T>::getReadGroup() {
    return ReadGroup<T>(getProducerToken(), getConsumerToken());
}

template <typename T>
moodycamel::ProducerToken FastxParser<T>::getProducerToken() {
  return moodycamel::ProducerToken(seqContainerQueue_);
}

template <typename T>
moodycamel::ConsumerToken FastxParser<T>::getConsumerToken() {
  return moodycamel::ConsumerToken(readQueue_);
}

template <typename T>
FastxParser<T>::~FastxParser() {
        parsingThread_->join();
        /*
	for (size_t i = 0; i < queueCapacity_; ++i) {
	  if (readStructs_[i].seq != nullptr) { free(readStructs_[i].seq); }
        }
        delete [] readStructs_;
        */
        delete parsingThread_;
    }

template <typename T>
void parseReads(std::vector<std::string>& inputStreams,
                  bool& parsing,
                  moodycamel::ConsumerToken* cCont,
                  moodycamel::ProducerToken* pRead,
                  moodycamel::ConcurrentQueue<ReadChunk<T>*>& seqContainerQueue_,
                moodycamel::ConcurrentQueue<ReadChunk<T>*>& readQueue_) {
                      kseq_t* seq;
                      T* s;
                      size_t nr{0};
                      std::cerr << "reading from " << inputStreams.size() << " streams\n";
                      for (auto file : inputStreams) {
                          std::cerr << "reading from " << file << "\n";
                          ReadChunk<T>* local;
                          while (!seqContainerQueue_.try_dequeue(*cCont, local)) {
                              std::cerr << "couldn't dequeue read chunk\n";
                          }
                          size_t numObtained{local->size()};
                          // open the file and init the parser
                          auto fp = gzopen(file.c_str(), "r");

                          // The number of reads we have in the local vector
                          size_t numWaiting{0};

                          seq = kseq_init(fp); 
                          int ksv = kseq_read(seq);
		      
                          while (ksv >= 0) {
                              if (nr % 1000000 == 0) {
                                  std::cerr << "saw " << nr << " reads\n";
                              }
                              ++nr;
                              s = &((*local)[numWaiting++]);
                              // Possibly allocate more space for the sequence
                              if (seq->seq.l > s->len) {
                                  s->seq = static_cast<char*>(realloc(s->seq, seq->seq.l));
                              }
                              // Copy the sequence length and sequence over to the ReadSeq struct
                              s->len = seq->seq.l;
                              memcpy(s->seq, seq->seq.s, s->len);

                              // Possibly allocate more space for the name
                              if (seq->name.l > s->nlen) {
                                  s->name = static_cast<char*>(realloc(s->name, seq->name.l));
                              }
                              // Copy the name length and name over to the ReadSeq struct
                              s->nlen = seq->name.l;
                              memcpy(s->name, seq->name.s, s->nlen);

                              // If we've filled the local vector, then dump to the concurrent queue
                              if (numWaiting == numObtained) {
                                  readQueue_.enqueue(local);
                                  numWaiting = 0;
                                  numObtained = 0;
                                  // And get more empty reads
                                  while (!seqContainerQueue_.try_dequeue(*cCont, local)) {}
                                  numObtained = local->size();
                              }
                              ksv = kseq_read(seq);
                          }

                          // If we hit the end of the file and have any reads in our local buffer
                          // then dump them here.
                          if (numWaiting > 0) {
                              local->have(numWaiting);
                              readQueue_.try_enqueue(*pRead, local);
                              numWaiting = 0;
                          }
                          // destroy the parser and close the file
                          kseq_destroy(seq);
                          gzclose(fp);
                      }


                      parsing = false;
                  }

inline void copyRecord(kseq_t* seq, ReadSeq* s) {
    // Possibly allocate more space for the sequence
    if (seq->seq.l > s->len) {
        s->seq = static_cast<char*>(realloc(s->seq, seq->seq.l));
    }
    // Copy the sequence length and sequence over to the ReadSeq struct
    s->len = seq->seq.l;
    memcpy(s->seq, seq->seq.s, s->len);

    // Possibly allocate more space for the name
    if (seq->name.l > s->nlen) {
        s->name = static_cast<char*>(realloc(s->name, seq->name.l));
    }
    // Copy the name length and name over to the ReadSeq struct
    s->nlen = seq->name.l;
    memcpy(s->name, seq->name.s, s->nlen);
}

template <typename T>
void parseReadPair(std::vector<std::string>& inputStreams,
                   std::vector<std::string>& inputStreams2,
                  bool& parsing,
                  moodycamel::ConsumerToken* cCont,
                  moodycamel::ProducerToken* pRead,
                  moodycamel::ConcurrentQueue<ReadChunk<T>*>& seqContainerQueue_,
                moodycamel::ConcurrentQueue<ReadChunk<T>*>& readQueue_) {
                      kseq_t* seq;
                      kseq_t* seq2;
                      T* s;
                      size_t nr{0};
                      std::cerr << "reading from " << inputStreams.size() << " streams\n";
                      for (size_t fn = 0; fn < inputStreams.size(); ++fn) {
                          auto& file = inputStreams[fn];
                          auto& file2 = inputStreams2[fn];
                          std::cerr << "reading from (" << file << ", " << file2 << ")\n";
                          ReadChunk<T>* local;
                          while (!seqContainerQueue_.try_dequeue(*cCont, local)) {
                              std::cerr << "couldn't dequeue read chunk\n";
                          }
                          size_t numObtained{local->size()};
                          // open the file and init the parser
                          auto fp = gzopen(file.c_str(), "r");
                          auto fp2 = gzopen(file2.c_str(), "r");

                          // The number of reads we have in the local vector
                          size_t numWaiting{0};

                          seq = kseq_init(fp); 
                          seq2 = kseq_init(fp2);

                          int ksv = kseq_read(seq);
                          int ksv2 = kseq_read(seq2);
                          while (ksv >= 0 and ksv2 >= 0) {
                              if (nr % 1000000 == 0) {
                                  std::cerr << "saw " << nr << " reads\n";
                              }
                              ++nr;
                              s = &((*local)[numWaiting++]);
                              copyRecord(seq, &s->first);
                              copyRecord(seq2, &s->second);
                              
                              // If we've filled the local vector, then dump to the concurrent queue
                              if (numWaiting == numObtained) {
                                  readQueue_.enqueue(local);
                                  numWaiting = 0;
                                  numObtained = 0;
                                  // And get more empty reads
                                  while (!seqContainerQueue_.try_dequeue(*cCont, local)) {}
                                  numObtained = local->size();
                              }
                              ksv = kseq_read(seq);
                              ksv2 = kseq_read(seq2);
                          }

                          // If we hit the end of the file and have any reads in our local buffer
                          // then dump them here.
                          if (numWaiting > 0) {
                              local->have(numWaiting);
                              readQueue_.try_enqueue(*pRead, local);
                              numWaiting = 0;
                          }
                          // destroy the parser and close the file
                          kseq_destroy(seq);
                          gzclose(fp);
                          kseq_destroy(seq2);
                          gzclose(fp2);
                      }


                      parsing = false;
                  }

template <>
bool FastxParser<ReadSeq>::start() {
    if (!parsing_) {
        parsing_ = true;
        parsingThread_ = 
            new std::thread([this]() {
                parseReads(this->inputStreams_,
                           this->parsing_,
                           this->consumeContainer_.get(),
                           this->produceReads_.get(),
                           this->seqContainerQueue_,
                           this->readQueue_);
                });
        return true;
    } else {
        return false;
    }
}

template <>
bool FastxParser<ReadPair>::start() {
    if (!parsing_) {
        parsing_ = true;
        parsingThread_ = 
            new std::thread([this]() {
                    parseReadPair(this->inputStreams_,
                                  this->inputStreams2_,
                                  this->parsing_,
                                  this->consumeContainer_.get(),
                                  this->produceReads_.get(),
                                  this->seqContainerQueue_,
                                  this->readQueue_);
                });
        return true;
    } else {
        return false;
    }
}

template <typename T>
bool FastxParser<T>::refill(ReadGroup<T>& seqs) {
  while(parsing_) {
    if (readQueue_.try_dequeue(seqs.consumerToken(), seqs.chunkPtr())) {
        return true;
    }
  }
  return readQueue_.try_dequeue(seqs.consumerToken(), seqs.chunkPtr()); 
}

template <typename T>
void FastxParser<T>::finishedWithGroup(ReadGroup<T>& s) {
    seqContainerQueue_.enqueue(s.producerToken(), s.chunkPtr());
}


template class FastxParser<ReadSeq>;
template class FastxParser<ReadPair>;

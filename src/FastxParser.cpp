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
/*
int fifoRead(int f, void* buf, int n) {
  char* a;
  int m, t;
  a = static_cast<char*>(buf);
  t = 0;
  while (t < n) {
    m = read(f, a+t, n-t);
    if (m <= 0) {
      if (t == 0) {
        return m;
      }
      std::cerr << "hit eof!\n";
      break;
    }
    t += m;
  }
  return t;
}
KSEQ_INIT(int, fifoRead)
*/
// STEP 1: declare the type of file handler and the read() function
KSEQ_INIT(gzFile, gzread)

FastxParser::FastxParser( std::vector<std::string>& files, uint32_t numReaders): inputStreams_(files),
        parsing_(false), parsingThread_(nullptr)
    {
      blockSize_ = 1000;
        readStructs_ = new ReadSeq[queueCapacity_];
        readQueue_ = moodycamel::ConcurrentQueue<ReadSeq*>(queueCapacity_);
        seqContainerQueue_ = moodycamel::ConcurrentQueue<ReadSeq*>(queueCapacity_, 1 + numReaders, 0);
	pt_.reset(new moodycamel::ProducerToken(seqContainerQueue_));
	size_t numEnqueued{0};
	for (size_t i = 0; i < queueCapacity_; ++i) {
	  while(!seqContainerQueue_.try_enqueue(*pt_, std::move(&readStructs_[i]))) {
	    std::cerr << "Could not enqueue " << i << "!\n";
	    std::cerr << "capacity = " << queueCapacity_ << '\n';
	  }
	}
    }

moodycamel::ProducerToken FastxParser::getProducerToken() {
  return moodycamel::ProducerToken(seqContainerQueue_);
}
moodycamel::ConsumerToken FastxParser::getConsumerToken() {
  return moodycamel::ConsumerToken(readQueue_);
}

FastxParser::~FastxParser() {
        parsingThread_->join();
	for (size_t i = 0; i < queueCapacity_; ++i) {
	  if (readStructs_[i].seq != nullptr) { free(readStructs_[i].seq); }
        }
        delete [] readStructs_;
        delete parsingThread_;
    }

bool FastxParser::start() {
        if (!parsing_) {
            parsing_ = true;
            parsingThread_ = new std::thread([this](){
		moodycamel::ProducerToken* pt = this->pt_.get();
                kseq_t* seq;
                ReadSeq* s;
                std::cerr << "reading from " << this->inputStreams_.size() << " streams\n";
                for (auto file : this->inputStreams_) {
                    std::cerr << "reading from " << file << "\n";
                    // open the file and init the parser
		    auto fp = gzopen(file.c_str(), "r");

		    seq = kseq_init(fp); 
                      int ksv = kseq_read(seq);
                      while (ksv >= 0) {
                        while(!this->seqContainerQueue_.try_dequeue(s)) {};// std::cerr << "could not get read structure\n"; }
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

                        while(!this->readQueue_.try_enqueue(s)) { };//std::cerr << "could not enqueue read\n"; }
                        ksv = kseq_read(seq);
                      }

                    // destroy the parser and close the file
                    kseq_destroy(seq);
                    gzclose(fp);
                }


                this->parsing_ = false;
            });
            return true;
        } else {
            return false;
        }

    }

bool FastxParser::nextRead(moodycamel::ConsumerToken& ct, ReadSeq*& seq) {
  while(parsing_) {
    if (readQueue_.try_dequeue(ct, seq)) {
      return true;
    }
  }
  return readQueue_.try_dequeue(ct, seq);
}

void FastxParser::finishedWithRead(moodycamel::ProducerToken& pt, ReadSeq*& s) { while(!seqContainerQueue_.try_enqueue(pt, s)){ std::cerr << "b\n"; }; }

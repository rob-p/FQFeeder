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

FastxParser::FastxParser( std::vector<std::string>& files, uint32_t numReaders): inputStreams_(files),
        parsing_(false), parsingThread_(nullptr)
    {
      blockSize_ = 1000;
      queueCapacity_ = blockSize_ * numReaders;
        readStructs_ = new ReadSeq[queueCapacity_];
        readQueue_ = moodycamel::ConcurrentQueue<ReadSeq*>(queueCapacity_, 1 + numReaders, 0);
        seqContainerQueue_ = moodycamel::ConcurrentQueue<ReadSeq*>(queueCapacity_, 1 + numReaders, 0);

	produceContainer_.reset(new moodycamel::ProducerToken(seqContainerQueue_));
	consumeContainer_.reset(new moodycamel::ConsumerToken(seqContainerQueue_));

	produceReads_.reset(new moodycamel::ProducerToken(readQueue_));
	consumeReads_.reset(new moodycamel::ConsumerToken(readQueue_));

	size_t numEnqueued{0};
	for (size_t i = 0; i < queueCapacity_; ++i) {
	  while(!seqContainerQueue_.try_enqueue(*produceContainer_, std::move(&readStructs_[i]))) {
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
		moodycamel::ConsumerToken* cCont = this->consumeContainer_.get();
		moodycamel::ProducerToken* pRead = this->produceReads_.get();
                kseq_t* seq;
                ReadSeq* s;
		size_t nr{0};
                std::cerr << "reading from " << this->inputStreams_.size() << " streams\n";
                for (auto file : this->inputStreams_) {
		  std::cerr << "reading from " << file << "\n";
		  auto blockSize = this->blockSize_;
		  std::vector<ReadSeq*> local(blockSize_, nullptr);
		  size_t numObtained{0};
		  while (numObtained == 0) {
		    numObtained = seqContainerQueue_.try_dequeue_bulk(*cCont, local.begin(), blockSize);
		  }
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
			s = local[numWaiting++];
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
			  this->readQueue_.enqueue_bulk(local.begin(), numObtained);
			  numWaiting = 0;
			  numObtained = 0;
			  // And get more empty reads
			  while (numObtained == 0) {
			    numObtained = seqContainerQueue_.try_dequeue_bulk(*cCont, local.begin(), blockSize);
			  }
			}
                        ksv = kseq_read(seq);
                      }

		      // If we hit the end of the file and have any reads in our local buffer
		      // then dump them here.
		      if (numWaiting > 0) {
			  this->readQueue_.try_enqueue_bulk(*pRead, local.begin(), numWaiting);
			  numWaiting = 0;
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

bool FastxParser::getReadGroup(moodycamel::ConsumerToken& ct, ReadGroup& seqs) {
  while(parsing_) {
    size_t obtained = readQueue_.try_dequeue_bulk(ct, seqs.begin(), seqs.want());
    if (obtained) {
      seqs.have(obtained);
      return obtained > 0;
    }
  }
  size_t obtained = readQueue_.try_dequeue_bulk(ct, seqs.begin(), seqs.want());
  if (obtained > 0) { seqs.have(obtained); }
  return (obtained > 0);
}

void FastxParser::finishedWithGroup(moodycamel::ProducerToken& pt, ReadGroup& s) {
  seqContainerQueue_.enqueue_bulk(pt, s.begin(), s.size());
}

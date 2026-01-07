#include "FastxParser.hpp"
#include <iostream>
#include <thread>
#include <vector>

// Lookup table: maps ASCII char to index (0=A, 1=C, 2=G, 3=T, -1=other)
static constexpr int8_t lookup[256] = {
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1, 0,-1, 1,-1,-1,-1, 2,-1,-1,-1,-1,-1,-1,-1,-1, // @ABCDEFGHIJKLMNO
  -1,-1,-1,-1, 3,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1, // PQRSTUVWXYZ
  -1, 0,-1, 1,-1,-1,-1, 2,-1,-1,-1,-1,-1,-1,-1,-1, // `abcdefghijklmno
  -1,-1,-1,-1, 3,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1, // pqrstuvwxyz
  // Rest are -1
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1
};

struct Bases {
  alignas(64) uint64_t A;
  uint64_t C;
  uint64_t G;
  uint64_t T;
};

struct Counters {
  alignas(64) std::array<uint64_t, 4> counts; 
};

int main(int argc, char* argv[]) {
   if (argc == 1) {
       std::cerr << "usage: test_parser fa1 fb1 ... fa2 fb2 ...";
       return 1;
   } 
   int numFiles = argc - 1;
   if (numFiles % 2 != 0) {
       std::cerr << "you must provide an even number of files!\n";
       return 1;
   }
   
   size_t numPairs = numFiles / 2;
   std::vector<std::string> files;
   std::vector<std::string> files2;
   for (size_t i = 1; i <= numPairs; ++i) {
       files.push_back(argv[i]);
       files2.push_back(argv[i + numPairs]);
   }

  size_t nt = 4;
  size_t np = 2;

  fastx_parser::ParserConfig pc = fastx_parser::ParserConfig::with_consumers_multi(nt);
  pc.numParsers = np;

  fastx_parser::FastxParser<fastx_parser::ReadPair> parser(pc, files, files2);
  parser.start();

  std::vector<std::thread> readers;
  std::vector<Counters> counters(nt, {0, 0, 0, 0});
  std::atomic<size_t> ctr{0};
  for (size_t i = 0; i < nt; ++i) {
    readers.emplace_back([&, i]() {
      auto rg = parser.getReadGroup();
      size_t lctr{0};
      size_t pctr{0};
      while (true) {
        if (parser.refill(rg)) {
          auto chunk_frag_offset = rg.chunk_frag_offset();
          //std::cerr << "chunk_offset_info: [file_idx: " << chunk_frag_offset.file_idx 
           //         << ", frag_idx:" << chunk_frag_offset.frag_idx << ", chunk_size: " << rg.size() <<"]\n";
          for (auto& seqPair : rg) {
            ++lctr;

            auto& seq = seqPair.first();
            auto& seq2 = seqPair.second();

            for (unsigned char c : seq.seq) {
              int idx = lookup[c];
              if (idx >= 0) { counters[i].counts[idx]++; }
            }
            for (unsigned char c : seq2.seq) {
              int idx = lookup[c];
              if (idx >= 0) { counters[i].counts[idx]++; }
            }
          }
          ctr += (lctr - pctr);
          pctr = lctr;
          if (lctr > 1000000) {
              lctr = 0;
              pctr = 0;
              //std::cout << "parsed " << ctr << " read pairs.\n";
          }
        } else {
          break;
        }
      }
    });
  }

  for (auto& t : readers) {
    t.join();
  }

  parser.stop();
  Bases b = {0, 0, 0, 0};
  for (size_t i = 0; i < nt; ++i) {
    b.A += counters[i].counts[0];//.A;
    b.C += counters[i].counts[1];//.C;
    b.G += counters[i].counts[2];//.G;
    b.T += counters[i].counts[3];//.T;
  }
  
  std::cerr << "\n";
  std::cerr << "Parsed " << ctr << " total read pairs.\n";
  std::cerr << "\n#A = " << b.A << '\n';
  std::cerr << "#C = " << b.C << '\n';
  std::cerr << "#G = " << b.G << '\n';
  std::cerr << "#T = " << b.T << '\n';
  return 0;
}

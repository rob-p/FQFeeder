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
  if (argc < 4 || (argc - 1) % 3 != 0) {
    std::cerr << "usage: test_parser_triplet fa1 fb1 fc1 [fa2 fb2 fc2] ...\n";
    std::cerr << "Provide files in groups of 3 (triplets)\n";
    return 1;
  }

  size_t numTriplets = (argc - 1) / 3;
  std::vector<std::string> files1, files2, files3;

  for (size_t i = 0; i < numTriplets; ++i) {
    files1.push_back(argv[1 + i * 3]);
    files2.push_back(argv[2 + i * 3]);
    files3.push_back(argv[3 + i * 3]);
  }

  size_t nt = 4;
  size_t np = 1;
  fastx_parser::FastxParser<fastx_parser::ReadTriple> parser(files1, files2,
                                                             files3, nt, np);
  parser.start();

  std::vector<std::thread> readers;
  std::vector<Counters> counters(nt, {0, 0, 0, 0});
  std::atomic<size_t> ctr{0};

  for (size_t i = 0; i < nt; ++i) {
    readers.emplace_back([&, i]() {
      auto rg = parser.getReadGroup();
      size_t lctr{0};
      while (true) {
        if (parser.refill(rg)) {
          for (auto& seqTriple : rg) {
            ++lctr;
            // Count first base of each read in triplet
            auto& seq1 = seqTriple.first();
            auto& seq2 = seqTriple.second();
            auto& seq3 = seqTriple.third();

            for (unsigned char c : seq1.seq) {
              int idx = lookup[c];
              if (idx >= 0) { counters[i].counts[idx]++; }
            }
            for (unsigned char c : seq2.seq) {
              int idx = lookup[c];
              if (idx >= 0) { counters[i].counts[idx]++; }
            }
            for (unsigned char c : seq3.seq) {
              int idx = lookup[c];
              if (idx >= 0) { counters[i].counts[idx]++; }
            }
          }
          ctr += lctr;
          lctr = 0;
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
    b.A += counters[i].counts[0];
    b.C += counters[i].counts[1];
    b.G += counters[i].counts[2];
    b.T += counters[i].counts[3];
  }
  std::cerr << "\n";
  std::cerr << "Parsed " << ctr << " total read triplets.\n";
  std::cerr << "\n#A = " << b.A << '\n';
  std::cerr << "#C = " << b.C << '\n';
  std::cerr << "#G = " << b.G << '\n';
  std::cerr << "#T = " << b.T << '\n';
  return 0;
}

#include "FastxParser.hpp"
#include <iostream>
#include <thread>
#include <vector>

struct Bases {
  uint32_t A, C, G, T;
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
  std::vector<Bases> counters(nt, {0, 0, 0, 0});
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

            for (auto* seq : {&seq1, &seq2, &seq3}) {
              if (!seq->seq.empty()) {
                char c = seq->seq[0];
                switch (c) {
                case 'A':
                  counters[i].A++;
                  break;
                case 'C':
                  counters[i].C++;
                  break;
                case 'G':
                  counters[i].G++;
                  break;
                case 'T':
                  counters[i].T++;
                  break;
                default:
                  break;
                }
              }
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
    b.A += counters[i].A;
    b.C += counters[i].C;
    b.G += counters[i].G;
    b.T += counters[i].T;
  }
  std::cerr << "\n";
  std::cerr << "Parsed " << ctr << " total read triplets.\n";
  std::cerr << "\n#A = " << b.A << '\n';
  std::cerr << "#C = " << b.C << '\n';
  std::cerr << "#G = " << b.G << '\n';
  std::cerr << "#T = " << b.T << '\n';
  return 0;
}

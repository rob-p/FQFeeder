#include "FastxParser.hpp"
#include <iostream>
#include <thread>
#include <vector>

struct Bases {
  uint32_t A, C, G, T;
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
  fastx_parser::FastxParser<fastx_parser::ReadPair> parser(files, files2, nt, np);
  parser.start();

  std::vector<std::thread> readers;
  std::vector<Bases> counters(nt, {0, 0, 0, 0});
  std::atomic<size_t> ctr{0};
  for (size_t i = 0; i < nt; ++i) {
    readers.emplace_back([&, i]() {
      auto rg = parser.getReadGroup();
      size_t lctr{0};
      size_t pctr{0};
      while (true) {
        if (parser.refill(rg)) {
          for (auto& seqPair : rg) {
            ++lctr;

            auto& seq = seqPair.first;
            auto& seq2 = seqPair.second;

            size_t j = 0;
            //for (size_t j = 0; j < seq.seq.length(); ++j) {
              char c = seq.seq[j];
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
            //}
            //for (size_t j = 0; j < seq2.seq.length(); ++j) {
              c = seq2.seq[j];
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
           // }
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
    b.A += counters[i].A;
    b.C += counters[i].C;
    b.G += counters[i].G;
    b.T += counters[i].T;
  }
  std::cerr << "\n";
  std::cerr << "Parsed " << ctr << " total read pairs.\n";
  std::cerr << "\n#A = " << b.A << '\n';
  std::cerr << "#C = " << b.C << '\n';
  std::cerr << "#G = " << b.G << '\n';
  std::cerr << "#T = " << b.T << '\n';
  return 0;
}

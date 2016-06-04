#include "FastxParser.hpp"
#include <iostream>
#include <vector>
#include <thread>

struct Bases {
  uint32_t A, C, G, T;
};

int main(int argc, char* argv[]) {
  std::vector<std::string> files;
  files.push_back(argv[1]);
  std::vector<std::string> files2;
  files2.push_back(argv[2]);


  size_t nt = 16;
  FastxParser<ReadPair> parser(files, files2, nt);
  parser.start();

  std::vector<std::thread> readers;
  std::vector<Bases> counters(nt, {0, 0, 0, 0});

  for (size_t i = 0; i < nt; ++i) {
    readers.emplace_back([&, i]() {
            auto rg = parser.getReadGroup(); 
	while (true) {
	  if (parser.refill(rg)) {
	    for (auto& seqPair : rg) {
            auto& seq = seqPair.first;
            auto& seq2 = seqPair.second;
            for (size_t j = 0; j < seq.len; ++j){
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
            }
            for (size_t j = 0; j < seq2.len; ++j){
                char c = seq2.seq[j];
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
	    parser.finishedWithGroup(rg);
	  } else { break; }
	}
      });
  }


  for (auto& t : readers) { t.join(); }
  Bases b = {0, 0, 0, 0};
  for (size_t i = 0; i < nt; ++i) {
    b.A += counters[i].A;
    b.C += counters[i].C;
    b.G += counters[i].G;
    b.T += counters[i].T;
  }
  std::cerr << "#A = " << b.A << '\n';
  std::cerr << "#C = " << b.C << '\n';
  std::cerr << "#G = " << b.G << '\n';
  std::cerr << "#T = " << b.T << '\n';
  return 0;
}

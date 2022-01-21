#include "kseq++.hpp"
#include <vector>
#include <iostream>
#include <zlib.h>

struct Bases {
  uint32_t A, C, G, T;
};

int main(int argc, char* argv[]) {
  using namespace klibpp;
  gzFile fp = nullptr;
  gzFile fp2 = nullptr;

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

   Bases counter = {0, 0, 0, 0};
   size_t rnum{0};
   for (size_t i = 0; i < numPairs; ++i) {
        // open the file and init the parser
       gzFile fp = gzopen(files[i].c_str(), "r");
       gzFile fp2 = gzopen(files[i].c_str(), "r");

       auto seq = make_kstream(fp, gzread, mode::in);
       auto seq2 = make_kstream(fp2, gzread, mode::in);

       KSeq r1;
       KSeq r2;

       while ( (seq >> r1) and (seq2 >> r2) ) {
           ++rnum;
           /*
           if (rnum % 1000000 == 0) {
               std::cerr << rnum << '\n';
           }
           */
           if (r1.seq.length() > 0) {
               size_t j = 0;
               char c = r1.seq[j];
               switch (c) {
               case 'A':
                   counter.A++;
                   break;
               case 'C':
                   counter.C++;
                   break;
               case 'G':
                   counter.G++;
                   break;
               case 'T':
                   counter.T++;
                   break;
               default:
                   break;
               }
           }
           if (r2.seq.length() > 0) {
               size_t j = 0;
               char c = r2.seq[j];
               switch (c) {
               case 'A':
                   counter.A++;
                   break;
               case 'C':
                   counter.C++;
                   break;
               case 'G':
                   counter.G++;
                   break;
               case 'T':
                   counter.T++;
                   break;
               default:
                   break;
               }
           }
       }
   }

  std::cerr << "\n";
  std::cerr << "Parsed " << rnum << " total read pairs.\n";
  std::cerr << "#A = " << counter.A << '\n';
  std::cerr << "#C = " << counter.C << '\n';
  std::cerr << "#G = " << counter.G << '\n';
  std::cerr << "#T = " << counter.T << '\n';

  gzclose(fp);
  gzclose(fp2);
  return 0;
}

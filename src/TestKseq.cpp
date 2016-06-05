// taken from:
// https://biowize.wordpress.com/2012/03/02/parsing-fasta-files-with-c-kseq-h/
#include "kseq.h"
#include <vector>
#include <iostream>
#include <zlib.h>
// STEP 1: declare the type of file handler and the read() function
KSEQ_INIT(gzFile, gzread)
struct Bases {
  uint32_t A, C, G, T;
};

int main(int argc, char* argv[]) {

  gzFile fp;
  gzFile fp2;
  kseq_t* seq;
  kseq_t* seq2;
  int l, l2;
  

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
       fp = gzopen(files[i].c_str(), "r");  // STEP 2: open the file handler
       fp2 = gzopen(files2[i].c_str(), "r"); // STEP 2: open the file handler
       seq = kseq_init(fp);        // STEP 3: initialize seq
       seq2 = kseq_init(fp2);      // STEP 3: initialize seq
       while ((l = kseq_read(seq)) >= 0 and
              (l2 = kseq_read(seq2)) >= 0) { // STEP 4: read sequence
           ++rnum;
           if (rnum % 1000000 == 0) {
               std::cerr << rnum << '\n';
           }
           for (size_t j = 0; j < seq->seq.l; ++j) {
               char c = seq->seq.s[j];
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
           for (size_t j = 0; j < seq2->seq.l; ++j) {
               char c = seq2->seq.s[j];
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
  std::cerr << "#A = " << counter.A << '\n';
  std::cerr << "#C = " << counter.C << '\n';
  std::cerr << "#G = " << counter.G << '\n';
  std::cerr << "#T = " << counter.T << '\n';

  kseq_destroy(seq);  // STEP 5: destroy seq
  kseq_destroy(seq2); // STEP 5: destroy seq
  gzclose(fp);        // STEP 6: close the file handler
  gzclose(fp2);       // STEP 6: close the file handler
  return 0;
}

// taken from:
// https://biowize.wordpress.com/2012/03/02/parsing-fasta-files-with-c-kseq-h/
#include "kseq.h"
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
    fprintf(stderr, "Usage: %s <in.seq>\n", argv[0]);
    return 1;
  }
  fp = gzopen(argv[1], "r");  // STEP 2: open the file handler
  fp2 = gzopen(argv[2], "r"); // STEP 2: open the file handler
  seq = kseq_init(fp);        // STEP 3: initialize seq
  seq2 = kseq_init(fp2);      // STEP 3: initialize seq
  Bases counter = {0, 0, 0, 0};
  size_t rnum{0};
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

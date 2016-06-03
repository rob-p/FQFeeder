// taken from: https://biowize.wordpress.com/2012/03/02/parsing-fasta-files-with-c-kseq-h/
#include <zlib.h>
#include <iostream>
#include "kseq.h"
// STEP 1: declare the type of file handler and the read() function
KSEQ_INIT(gzFile, gzread)
struct Bases {
  uint32_t A, C, G, T;
};


int main(int argc, char *argv[])
{

      gzFile fp;
    kseq_t *seq;
    int l;
    if (argc == 1) {
        fprintf(stderr, "Usage: %s <in.seq>\n", argv[0]);
        return 1;
    }
    fp = gzopen(argv[1], "r"); // STEP 2: open the file handler
    seq = kseq_init(fp); // STEP 3: initialize seq
    Bases counter = {0, 0, 0, 0};
    size_t nr{0};
    size_t rnum{0};
    while ((l = kseq_read(seq)) >= 0) { // STEP 4: read sequence
      ++rnum;
      if (rnum % 1000000 == 0) { std::cerr << rnum << '\n'; }
      for (size_t j = 0; j < seq->seq.l; ++j){
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
    }
    std::cerr << "#A = " << counter.A << '\n';
    std::cerr << "#C = " << counter.C << '\n';
    std::cerr << "#G = " << counter.G << '\n';
    std::cerr << "#T = " << counter.T << '\n';

    kseq_destroy(seq); // STEP 5: destroy seq
    gzclose(fp); // STEP 6: close the file handler
    return 0;
}

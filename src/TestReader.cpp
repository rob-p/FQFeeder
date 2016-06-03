#include "FastxParser.hpp"
#include <iostream>
#include <vector>

int main(int argc, char* argv[]) {
  std::vector<std::string> files;
  files.push_back(argv[1]);
  FastxParser parser(files, 1);

  auto pt = parser.getProducerToken();
  auto ct = parser.getConsumerToken();
  parser.start();
  ReadSeq* seq;
  size_t rnum{0};
  while (parser.nextRead(ct, seq)) {
    ++rnum;
    std::cerr << "here " << rnum << '\n';
    parser.finishedWithRead(pt, seq);
  }
  std::cerr << "saw " << rnum << " reads";
  return 0;
}

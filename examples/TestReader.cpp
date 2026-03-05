#include "FastxParser.hpp"
#include "CLI11.hpp"
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


template <size_t N, typename... FileVectors>
Bases parse_reads(std::atomic<size_t>& ctr, fastx_parser::ParserConfig c, FileVectors&&... fileVectors) {
  // Static assert to ensure the number of file vectors matches N
  static_assert(sizeof...(fileVectors) == N, 
                "Number of file vectors must match template parameter N");
  
  fastx_parser::FastxParser<fastx_parser::ReadSet<N>> parser(
      c, std::forward<FileVectors>(fileVectors)...);
  parser.start();

  std::vector<std::thread> readers;
  std::vector<Counters> counters(c.numConsumers, {0, 0, 0, 0});
  for (size_t i = 0; i < c.numConsumers; ++i) {
    readers.emplace_back([&, i]() {
      auto rg = parser.getReadGroup();
      size_t lctr{0};
      size_t pctr{0};
      while (true) {
        if (parser.refill(rg)) {
          auto chunk_frag_offset = rg.chunk_frag_offset();
          (void)chunk_frag_offset;
          for (auto& fragSet : rg) {
            ++lctr;
            for (size_t j = 0; j < N; ++j) {
              auto& seq = fragSet[j];
              for (unsigned char c : seq.seq) {
                int idx = lookup[c];
                if (idx >= 0) { counters[i].counts[idx]++; }
              }
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
  for (size_t i = 0; i < c.numConsumers; ++i) {
    b.A += counters[i].counts[0];//.A;
    b.C += counters[i].counts[1];//.C;
    b.G += counters[i].counts[2];//.G;
    b.T += counters[i].counts[3];//.T;
  }

  return b;
}

int main(int argc, char* argv[]) {
    CLI::App app{"Test the FQFeeder parser"};

    // Define options
    uint32_t nworker = 0;
    uint32_t nprod = 0;
    bool use_parallel = false;
    auto ogroup = app.add_option_group("input reads", "provide input reads");
    app.add_flag("--within-set-parallelism", use_parallel, "within set parallel parsing");
    app.add_option("-w,--workers", nworker,
                "An integer that specifies the number of threads to use.")
    ->default_val(4);
    app.add_option("-p,--producers", nprod,
                "An integer that specifies the number of threads to use.")
    ->default_val(4);

  std::vector<std::string> single_read_filenames;

  std::vector<std::string> first_read_filenames;
  std::vector<std::string> second_read_filenames;
  std::vector<std::string> third_read_filenames;

  CLI::Option *read_opt =
    ogroup
      ->add_option("-r,--reads", single_read_filenames,
                   "Path to list (comma separated) of single-end files")
      ->delimiter(',');

  CLI::Option *first_op =
    ogroup
      ->add_option("-1,--read1", first_read_filenames,
                   "Path to list (comma separated) of read 1 files")
      ->delimiter(',');
  CLI::Option *second_op =
    ogroup
      ->add_option("-2,--read2", second_read_filenames,
                   "Path to list (comma separated) of read 2 files")
      ->delimiter(',');

  CLI::Option *third_op =
    ogroup
      ->add_option("-3,--read3", third_read_filenames,
                   "Path to list (comma separated) of read 3 files")
      ->delimiter(',');

  first_op->excludes(read_opt);
  second_op->excludes(read_opt);
  third_op->excludes(read_opt);

  read_opt->excludes(first_op, second_op, third_op);

  first_op->needs(second_op);
  second_op->needs(first_op);
  third_op->needs(first_op);
  third_op->needs(second_op);

  ogroup->require_option(1, 3);

  CLI11_PARSE(app, argc, argv);

  auto pc = fastx_parser::ParserConfigBuilder().within_set_parallelism(use_parallel).with_consumers(nworker).with_parsers(nprod).build();
  pc.chunkSize = 256;

  std::atomic<size_t> ctr{0};
  Bases b; 
  if (third_op->count() > 0) {
    b = parse_reads<3>(ctr, pc, first_read_filenames, second_read_filenames, third_read_filenames);
  } else if (second_op->count() > 0) {
    b = parse_reads<2>(ctr,pc, first_read_filenames, second_read_filenames);
  } else {
    b = parse_reads<1>(ctr, pc, single_read_filenames);
  }
  
  std::cerr << "\n";
  std::cerr << "Parsed " << ctr << " total read pairs.\n";
  std::cerr << "\n#A = " << b.A << '\n';
  std::cerr << "#C = " << b.C << '\n';
  std::cerr << "#G = " << b.G << '\n';
  std::cerr << "#T = " << b.T << '\n';
  return 0;
}

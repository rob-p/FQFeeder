# FQFeeder
---------

FQFeeder is a simple module that is a self-contained, multi-threaded, Fasta/q parser. It was developed out of my desire to have a "lightweight" class that provides both single and paired-end Fasta/q parsing in the case where multiple threads are used to parallelize the work being done over the reads.  The actual parsing is done using Heng Li's kseq library, and the producer / consumer queue uses Moodycamel's [concurrent queue](https://github.com/cameron314/concurrentqueue).  This library is designed to be efficient and flexible, *easy to use*, and to act as a lightweight dependency for projects that want to do multi-threaded Fasta/q parsing.

*Note*: The *main* purpose of having this module is not necessarily to speed up parsing (since, if you're doing any amount of non-trivial work on the reads themselves, parsing is unlikely to be the bottleneck), but to provide an arbitrary number of worker threads thread-safe access to a stream of reads.  That said, this module can make use of multiple *producer* / *parser* threads, if one wishes to parse multiple files (file-pairs) simultaneously.

## Usage
--------

FQFeeder is designed to be simple to use.  You can find an example in the [`TestReader.cpp`](https://github.com/rob-p/FQFeeder/blob/master/src/TestReader.cpp) file in the `src` directory.  To create a single-end parser, you simply create a `FastxParser` templated on the type `ReadSeq`, as so:

```{c++}
// Files is an std::vector<std::string> of files to parse,
// nt is a uint32_t that specifies the number of consumer threads
// we'll have and np is a uint32_t that specifies the number of 
// producer threads we'll have
FastxParser<ReadSeq> parser(files, nt, np);
```

to create a paired-end parser, you simply do the following instead:

```{c++}
FastxParser<ReadPair> parser(files, files2, nt, np);
```

This constructor looks the same, except you use `ReadPair` as the template parameter and pass in 
two vectors of strings, the first is for the "left" (`_1`) reads and the second is for the "right" (`_2`) reads.
**You need not explicitly provide a value for np; if you don't, it will default to 1**.  It only 
makes sense to spawn multiple producer / parser threads if you're reading from more than one file (or more than one file pair, in paired-end mode), and it doesn't make sense to have more producers than files (file pairs).  In fact, if you request more producers than filenames given in the `files` vector, then a message will be emitted and the number of producers will be set to the number of files.  Using multiple producer threads makes the most sense if the input files are compressed, or located on separate physical disks.

With the `parser` created as above, up to `nt` different threads can pull reads from this parser.  For a given thread, the idiom to obtain a group of reads is as follows (using the paired-end parser as an example):

```{c++}
// Get the read group by which this thread will
// communicate with the parser (*once per-thread*)
auto rg = parser.getReadGroup();

while (parser.refill(rg)) {
  // Here, rg will contain a chunk of read pairs
  // we can process.
  for (auto& rp : rg) {
    auto& r1 = rp.first;
    auto& r2 = rp.second;
    std::cout << "read 1 name: " << r1.name << ", seq: " << r1.seq << '\n';
    std::cout << "read 2 name: " << r2.name << ", seq: " << r2.seq << '\n';
  }
}
```

That's it! You can do this from as many threads as you want (assuming you specified the correct number
in the `FastxParser` constructor). The `refill()` function will return false when there are 
no more reads to be parsed.  Using the single-end parser is almost identical, except that you get back
a `ReadSeq` rather than a `ReadPair`, so there are no `.first` and `.second` members, just the `seq`,
`name`, `len`, and `nlen` fields.

So, that's all!  If you find this module useful, please let me know. If you have bug reports, or feature requests, please submit them via this repository.  I'm also happy to accept pull-requests!  Happy multi-threaded read processing!

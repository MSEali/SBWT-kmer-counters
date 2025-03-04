FROM ubuntu:22.04

# Set some timezone or otherwise tzdata hangs the build.
ENV TZ=Asia/Dubai
RUN ln -snf /usr/share/zoneinfo/$TZ /etc/localtime && echo $TZ > /etc/timezone

RUN apt-get update && apt-get install -y g++ gcc cmake git python3-dev g++-10 libz-dev libbz2-dev

RUN git clone https://github.com/jnalanko/SBWT-kmer-counters --recursive
WORKDIR /SBWT-kmer-counters
RUN git checkout master

WORKDIR /SBWT-kmer-counters/SBWT/build
RUN cmake .. -DCMAKE_CXX_COMPILER=g++-10 -DMAX_KMER_LENGTH=32
RUN make -j
run bin/sbwt # Test that it works

WORKDIR /SBWT-kmer-counters
RUN make counters dump_kmers multi_genome_counters

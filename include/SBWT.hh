#pragma once

#include <vector>
#include <sdsl/bit_vectors.hpp>
#include <sdsl/rank_support_v.hpp>
#include "NodeBOSSInMemoryConstructor.hh"
#include "throwing_streams.hh"
#include "suffix_group_optimization.hh"
#include "kmc_construct.hh"
#include "libwheeler/BOSS.hh"
#include "globals.hh"
#include "Kmer.hh"
#include <map>

/*

This file contains a class that implements the SBWT index described in the paper:

Alanko, J. N., Puglisi, S. J., & Vuohtoniemi, J. (2022). Succinct k-mer Set 
Representations Using Subset Rank Queries on the Spectral Burrows-Wheeler 
Transform (SBWT). bioRxiv.
*/

using namespace std;

namespace sbwt{

// Assumes that a root node always exists
template <typename subset_rank_t>
class SBWT{

private:


    // Is the index for the reverse (lex-sorted k-mers) or forward (colex-sorted k-mers)?
    // The paper describes the colex version, but if we construct the index via KMC, then
    // we get the lex version, because KMC sorts in lex-order. Then the search will go backward
    // instead of forward.
    bool colex;

    subset_rank_t subset_rank; // The subset rank query implementation
    sdsl::bit_vector suffix_group_starts; // Marks the first column of every suffix group (see paper)
    vector<int64_t> C; // The array of cumulative character counts
    int64_t n_nodes; // Number of nodes (= columns) in the data structure
    int64_t k; // The k-mer k
    int64_t n_kmers; // Number of k-mers indexed in the data structure

public:

    struct BuildConfig{
        vector<string> input_files;
        int k = 30;
        bool add_reverse_complements = false;
        bool build_streaming_support = true;
        int n_threads = 1;
        int min_abundance = 1;
        int max_abundance = 1e9;
        int ram_gigas = 2;
        string temp_dir = ".";
    };

    SBWT() : n_nodes(0), k(0), colex(true), n_kmers(0) {}

    // Construct from precomputed data.
    SBWT(const sdsl::bit_vector& A_bits, 
         const sdsl::bit_vector& C_bits, 
         const sdsl::bit_vector& G_bits, 
         const sdsl::bit_vector& T_bits, 
         const sdsl::bit_vector& streaming_support, // Streaming support may be empty
         int64_t k, 
         int64_t number_of_kmers,
         bool colex);

    // KMC construction
    SBWT(const BuildConfig& config); 

    // Accessors
    bool is_colex() const {return colex;}
    const subset_rank_t& get_subset_rank_structure() const {return subset_rank;}
    const sdsl::bit_vector& get_streaming_support() const {return suffix_group_starts;}
    const vector<int64_t>& get_C_array() const {return C;}
    int64_t number_of_subsets() const {return n_nodes;}
    int64_t number_of_kmers() const {return n_kmers;}
    int64_t get_k() const {return k;}

    int64_t search(const string& kmer) const; // Search for k-mer as std::string. If string is longer than k, only the first k characters are searched
    int64_t search(const char* S) const; // Search for C-string

    // Query for all k-mers in the input
    vector<int64_t> streaming_search(const string& input) const;
    vector<int64_t> streaming_search(const char* input, int64_t len) const;
    bool has_streaming_query_support() const {return suffix_group_starts.size() > 0;}
    
    int64_t serialize(ostream& out) const; // Returns the number of bytes written
    int64_t serialize(const string& filename) const; // Returns the number of bytes written
    void load(istream& in);
    void load(const string& filename);

};


template <typename subset_rank_t>
SBWT<subset_rank_t>::SBWT(const sdsl::bit_vector& A_bits, const sdsl::bit_vector& C_bits, const sdsl::bit_vector& G_bits, const sdsl::bit_vector& T_bits, const sdsl::bit_vector& streaming_support, int64_t k, int64_t n_kmers, bool colex){
    subset_rank = subset_rank_t(A_bits, C_bits, G_bits, T_bits);

    this->n_nodes = A_bits.size();
    this->k = k;
    this->suffix_group_starts = streaming_support;
    this->colex = colex;
    this->n_kmers = n_kmers;

    // Get the C-array
    C.clear(); C.resize(4);
    C[0] = 1; // There is one incoming ghost-dollar to the root node
    C[1] = C[0] + subset_rank.rank(n_nodes, 'A');
    C[2] = C[1] + subset_rank.rank(n_nodes, 'C');
    C[3] = C[2] + subset_rank.rank(n_nodes, 'G');

}

template <typename subset_rank_t>
SBWT<subset_rank_t>::SBWT(const BuildConfig& config){
    string old_temp_dir = get_temp_file_manager().get_dir();
    get_temp_file_manager().set_dir(config.temp_dir);

    NodeBOSSKMCConstructor<SBWT<subset_rank_t>> builder;
    builder.build(config.input_files, *this, config.k, config.n_threads, config.ram_gigas, config.build_streaming_support, config.min_abundance, config.max_abundance);

    get_temp_file_manager().set_dir(old_temp_dir); // Return the old temporary directory
}

template <typename subset_rank_t>
int64_t SBWT<subset_rank_t>::search(const string& kmer) const{
    assert(kmer.size() == k);
    return search(kmer.c_str());
}

template <typename subset_rank_t>
int64_t SBWT<subset_rank_t>::search(const char* kmer) const{
    int64_t node_left = 0;
    int64_t node_right = n_nodes-1;
    for(int64_t i = 0; i < k; i++){
        char c = colex ? kmer[i] : kmer[k-1-i];
        char char_idx = 0;
        if(toupper(c) == 'A') char_idx = 0;
        else if(toupper(c) == 'C') char_idx = 1;
        else if(toupper(c) == 'G') char_idx = 2;
        else if(toupper(c) == 'T') char_idx = 3;
        else return -1; // Invalid character

        node_left = C[char_idx] + subset_rank.rank(node_left, c);
        node_right = C[char_idx] + subset_rank.rank(node_right+1, c) - 1;

        if(node_left > node_right) return -1; // Not found
    }
    if(node_left != node_right){
        cerr << "Bug: node_left != node_right" << endl;
        exit(1);
    }
    return node_left;
}

// Utility function: Serialization for a std::vector
// Returns number of bytes written
template<typename T>
int64_t serialize_std_vector(const vector<T>& v, ostream& os){
    // Write C-array
    int64_t n_bytes = sizeof(T) * v.size();
    os.write((char*)&n_bytes, sizeof(n_bytes));
    os.write((char*)v.data(), n_bytes);
    return sizeof(n_bytes) + n_bytes;
}

template<typename T>
vector<T> load_std_vector(istream& is){
    int64_t n_bytes = 0;
    is.read((char*)&n_bytes, sizeof(n_bytes));
    assert(n_bytes % sizeof(T) == 0);
    vector<T> v(n_bytes / sizeof(T));
    is.read((char*)(v.data()), n_bytes);
    return v;
}


template <typename subset_rank_t>
int64_t SBWT<subset_rank_t>::serialize(ostream& os) const{
    int64_t written = 0;
    written += subset_rank.serialize(os);
    written += suffix_group_starts.serialize(os);

    written += serialize_std_vector(C, os);

    // Write number of nodes
    os.write((char*)&n_nodes, sizeof(n_nodes));
    written += sizeof(n_nodes);

    // Write k
    os.write((char*)&k, sizeof(k));
    written += sizeof(k);

    // Write colex flag
    char flag = colex;
    os.write(&flag, sizeof(flag));
    written += sizeof(flag);

    return written;
}

template <typename subset_rank_t>
int64_t SBWT<subset_rank_t>::serialize(const string& filename) const{
    throwing_ofstream out(filename, ios::binary);
    return serialize(out.stream);
}


template <typename subset_rank_t>
void SBWT<subset_rank_t>::load(istream& is){
    subset_rank.load(is);
    suffix_group_starts.load(is);
    C = load_std_vector<int64_t>(is);
    is.read((char*)&n_nodes, sizeof(n_nodes));
    is.read((char*)&k, sizeof(k));

    char colex_flag;
    is.read(&colex_flag, sizeof(colex_flag));
    colex = colex_flag;
}

template <typename subset_rank_t>
void SBWT<subset_rank_t>::load(const string& filename){
    throwing_ifstream in(filename, ios::binary);
    load(in.stream);
}

template <typename subset_rank_t>
vector<int64_t> SBWT<subset_rank_t>::streaming_search(const char* input, int64_t len) const{
    if(suffix_group_starts.size() == 0)
        throw std::runtime_error("Error: streaming search support not built");
    
    vector<int64_t> ans;
    if(len < k) return ans;

    // Search the first k-mer
    const char* first_kmer_start = colex ? input : input + len - k;
    ans.push_back(search(first_kmer_start)); 

    for(int64_t i = 1; i < len - k + 1; i++){
        if(ans.back() == -1){
            // Need to search from scratch
            ans.push_back(search(first_kmer_start + (colex ? i : -i)));
        } else{
            // Got to the start of the suffix group and do one search iteration
            int64_t column = ans.back();
            while(suffix_group_starts[column] == 0) column--; // can not go negative because the first column is always marked

            char c = toupper(input[colex ? i+k-1 : len-k-i]);
            char char_idx = -1;
            if(c == 'A') char_idx = 0;
            else if(c == 'C') char_idx = 1;
            else if(c == 'G') char_idx = 2;
            else if(c == 'T') char_idx = 3;
        
            if(char_idx == -1) ans.push_back(-1); // Not found
            else{
                int64_t node_left = column;
                int64_t node_right = column;
                node_left = C[char_idx] + subset_rank.rank(node_left, c);
                node_right = C[char_idx] + subset_rank.rank(node_right+1, c) - 1;
                if(node_left == node_right) ans.push_back(node_left);
                else ans.push_back(-1);
                // Todo: could save one subset rank query if we have fast access to the SBWT columns
            }
        }
    }
    if(!colex) std::reverse(ans.begin(), ans.end());
    return ans;
}

template <typename subset_rank_t>
vector<int64_t> SBWT<subset_rank_t>::streaming_search(const string& input) const{
    return streaming_search(input.c_str(), input.size());
}

} // namespace sbwt
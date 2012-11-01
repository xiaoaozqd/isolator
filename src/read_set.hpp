
#ifndef ISOLATOR_READ_SET_HPP
#define ISOLATOR_READ_SET_HPP

#include <boost/iterator/iterator_facade.hpp>
#include <vector>

#include "common.hpp"
#include "hat-trie/hat-trie.h"
#include "samtools/sam.h"

/* A representation of an aligned sequence. */
struct Alignment
{
    Alignment();
    Alignment(const Alignment& other);
    Alignment(const bam1_t* other);
    ~Alignment();

    bool operator == (const bam1_t* b) const;
    bool operator != (const bam1_t* b) const;

    pos_t start;
    pos_t end;
    uint16_t  cigar_len;
    uint32_t* cigar;
    uint8_t   strand;
};


/* A read with some number of alignments. */
struct AlignedRead
{
    AlignedRead();
    ~AlignedRead();

    bool operator < (const AlignedRead& other) const;

    pos_t start, end;
    bool paired;
    std::vector<Alignment*> mate1;
    std::vector<Alignment*> mate2;
};


/* A pair of alignments. (One for each mate.) */
struct AlignmentPair
{
    /* TODO: other things here */

    bool valid_frag() const;

    /* The fragment length of a paired-end read, ignoring the effects of
     * splicing. */
    pos_t naive_frag_len() const;

    const Alignment* mate1;
    const Alignment* mate2;
};


/* Iterate of the cartesian product of mate alignment. */
class AlignedReadIterator :
    public boost::iterator_facade<AlignedReadIterator,
                                  const AlignmentPair,
                                  boost::forward_traversal_tag>
{
    public:
        AlignedReadIterator();
        AlignedReadIterator(const AlignedRead&);
        ~AlignedReadIterator();

    private:
        friend class boost::iterator_core_access;

        void increment();
        bool equal(const AlignedReadIterator& other) const;
        bool finished() const;
        const AlignmentPair& dereference() const;

        const AlignedRead* r;
        size_t i, j;
        AlignmentPair p;
};


/* A container for a set of reads indexed by id. */
class ReadSet
{
    public:
        ReadSet();
        ~ReadSet();

        /* Add an alignment to the read set. */
        void add_alignment(const bam1_t* b);

        /* Make the set empty. Free memory. */
        void clear();

        /* Map aligned reads to number of occurances. */
        struct UniqueReadCountsCmp
        {
            bool operator () (AlignedRead* const& a, AlignedRead* const& b)
            {
                return *a < *b;
            }
        };

        typedef std::map<AlignedRead*, unsigned int, UniqueReadCountsCmp>
                UniqueReadCounts;

        /* Make a unique read count from the read set. */
        void make_unique_read_counts(UniqueReadCounts& counts);

    private:
        /* Map of read ids to a AligneRead objects. */
        hattrie_t* rs;

        /* A pool of unique alignments. */
        std::vector<Alignment*> as;
};

#endif

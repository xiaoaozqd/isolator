

#ifndef ISOLATOR_FRAGMENT_MODEL_HPP
#define ISOLATOR_FRAGMENT_MODEL_HPP

#include "sam_scan.hpp"
#include "seqbias/sequencing_bias.hpp"
#include "transcripts.hpp"

/* A probabalistic model of fragment sampling in RNA-Seq experiments. */
class FragmentModel
{
    public:
        FragmentModel();
        ~FragmentModel();
        void estimate(TranscriptSet& ts, const char* bam_fn, const char* fa_fn);

    private:
        /* On it's pass through the reads, estimate will also count the number
         * of alignments for each read. */
        AlnCountTrie alncnt;

        /* A model of sequenc bias. */
        sequencing_bias* sb;
};


#endif

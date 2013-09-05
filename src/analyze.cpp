
#include <boost/foreach.hpp>
#include <boost/numeric/ublas/matrix_proxy.hpp>
#include <boost/random/additive_combine.hpp> // L'Ecuyer RNG
#include <boost/thread.hpp>
#include <gsl/gsl_statistics_double.h>

#include "analyze.hpp"
#include "constants.hpp"
#include "queue.hpp"
#include "stan/io/var_context.hpp"

// Model generated by stan. Stan writes dirty code. Naughty stan.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"

// This is totally fucked up, but I'd rather avoid modifying stan, and I want to
// be able to change the data out from under the sampler.
#define private public

#include "analyze_model.cpp"

#undef private
#pragma GCC diagnostic pop

using namespace boost::numeric::ublas;


#if 0
// A class needed to load data into the stan generated sampler
class AnalyzeSamplerData : public stan::io::var_context
{
    public:
        AnalyzeSamplerData(Analyze& parent)
            : parent(parent)
        {
        }


        bool contains_r(const std::string& name) const
        {
            return name == "quantification" ||
                   name == "bandwidth" ||
                   name == "depth";
        }


        std::vector<double> vals_r(const std::string& name) const
        {
            if (name == "quantification") {
                std::vector<double> quantification(parent.M * parent.N * parent.K);
                unsigned int k = 0;
                for (unsigned int i3 = 0; i3 < parent.K; ++i3) {
                    for (unsigned int i2 = 0; i2 < parent.N; ++i2) {
                        for (unsigned int i1 = 0; i1 < parent.M; ++i1) {
                            quantification[k++] =
                                parent.quantification[i3](i2, i1);
                            if (!finite(quantification[k-1])) {
                                Logger::abort("Non finite quantification value.");
                            }
                        }
                    }
                }
                return quantification;
            }
            else if (name == "bandwidth") {
                std::vector<double> bandwidth(parent.N * parent.K);
                unsigned int k = 0;
                for (unsigned int i2 = 0; i2 < parent.K; ++i2) {
                    for (unsigned int i1 = 0; i1 < parent.N; ++i1) {
                        bandwidth[k++] = parent.bandwidth(i1, i2);
                    }
                }
                return bandwidth;
            }
            else if (name == "depth") {
                return parent.depth;
            }
            else if (contains_i(name)) {
                std::vector<int> ivals = vals_i(name);
                std::vector<double> rvals(ivals.size());
                std::copy(ivals.begin(), ivals.end(), rvals.begin());
                return rvals;
            }

            Logger::abort("Unknown var context: %s", name.c_str());
            return std::vector<double>();
        }


        bool contains_i(const std::string& name) const
        {
            return name == "K" ||
                   name == "C" ||
                   name == "N" ||
                   name == "M" ||
                   name == "M" ||
                   name == "T" ||
                   name == "condition" ||
                   name == "tss";
        }


        std::vector<int> vals_i(const std::string& name) const
        {
            if (name == "K") {
                std::vector<int> K(1);
                K[0] = (int) parent.K;
                return K;
            }
            else if (name == "C") {
                std::vector<int> C(1);
                C[0] = (int) parent.C;
                return C;
            }
            else if (name == "N") {
                std::vector<int> N(1);
                N[0] = (int) parent.N;
                return N;
            }
            else if (name == "M") {
                std::vector<int> M(1);
                M[0] = (int) parent.M;
                return M;
            }
            else if (name == "T") {
                std::vector<int> T(1);
                T[0] = (int) parent.T;
                return T;
            }
            else if (name == "condition") {
                std::vector<int> condition(parent.K);
                for (unsigned int c = 0; c < parent.condition_samples.size(); ++c) {
                    BOOST_FOREACH (unsigned int s, parent.condition_samples[c]) {
                        condition[s] = c + 1; // stan is 1-based
                    }
                }
                return condition;
            }
            else if (name == "tss") {
                std::vector<int> tss(parent.N);
                for (unsigned int i = 0; i < parent.N; ++i) {
                    tss[i] = parent.tss_index[i] + 1; // stan is 1-based
                }
                return tss;
            }

            Logger::abort("Unknown var context: %s", name.c_str());
            return std::vector<int>();
        }


        std::vector<size_t> dims_r(const std::string& name) const
        {
            if (name == "quantification") {
                return to_vec(parent.M, parent.N, parent.K);
            }
            else if (name == "bandwidth") {
                return to_vec(parent.N, parent.K);
            }
            else if (name == "depth") {
                return to_vec(parent.K);
            }
            else {
                return dims_i(name);
            }
        }


        std::vector<size_t> dims_i(const std::string& name) const
        {
            if (name == "K") {
                return to_vec();
            }
            else if (name == "C") {
                return to_vec();
            }
            else if (name == "N") {
                return to_vec();
            }
            else if (name == "M") {
                return to_vec();
            }
            else if (name == "T") {
                return to_vec();
            }
            else if (name == "condition") {
                return to_vec(parent.K);
            }
            else if (name == "tss") {
                return to_vec(parent.N);
            }
            else {
                return to_vec();
            }
        }


        // Don't bother doing anything meaningful with these. They don't seem to
        // be needed.

        void names_r(std::vector<std::string>& names) const
        {
            UNUSED(names);
        }


        void names_i(std::vector<std::string>& names) const
        {
            UNUSED(names);
        }


    private:
        Analyze& parent;

};
#endif


Analyze::Analyze(size_t burnin,
                 size_t num_samples,
                 TranscriptSet& ts,
                 const char* genome_filename,
                 bool run_gc_correction)
    : burnin(burnin)
    , num_samples(num_samples)
    , ts(ts)
    , genome_filename(genome_filename)
    , run_gc_correction(run_gc_correction)
    , K(0)
    , C(0)
    , N(0)
    , T(0)
{
    N = ts.size();
    T = ts.num_tgroups();

    Logger::info("Number of transcripts: %u", N);
    Logger::info("Number of transcription groups: %u", T);
}


Analyze::~Analyze()
{

}


void Analyze::add_sample(const char* condition_name, const char* filename)
{
    filenames.push_back(filename);
    ++K;
}


// Thread to initialize samplers and fragment models
class SamplerInitThread
{
    public:
        SamplerInitThread(const std::vector<std::string>& filenames, const char* fa_fn,
                          TranscriptSet& ts,
                          std::vector<FragmentModel*>& fms,
                          bool run_gc_correction,
                          std::vector<Sampler*> samplers,
                          Queue<int>& indexes)
            : filenames(filenames)
            , fa_fn(fa_fn)
            , ts(ts)
            , fms(fms)
            , run_gc_correction(run_gc_correction)
            , samplers(samplers)
            , indexes(indexes)
            , thread(NULL)
        {
        }

        void run()
        {
            int index;
            while (true) {
                if ((index = indexes.pop()) == -1) break;

                fms[index] = new FragmentModel();
                fms[index]->estimate(ts, filenames[index].c_str(), fa_fn);

                samplers[index] = new Sampler(filenames[index].c_str(), fa_fn,
                                              ts, *fms[index], run_gc_correction,
                                              true);
            }
        }

        void start()
        {
            if (thread != NULL) return;
            thread = new boost::thread(boost::bind(&SamplerInitThread::run, this));
        }

        void join()
        {
            thread->join();
            delete thread;
            thread = NULL;
        }

    private:
        const std::vector<std::string>& filenames;
        const char* fa_fn;
        TranscriptSet& ts;
        std::vector<FragmentModel*>& fms;
        bool run_gc_correction;

        std::vector<Sampler*> samplers;

        Queue<int>& indexes;

        boost::thread* thread;
};


void Analyze::setup()
{
    qsamplers.resize(K);

    std::vector<SamplerInitThread*> threads(constants::num_threads);
    Queue<int> indexes;
    for (unsigned int i = 0; i < constants::num_threads; ++i) {
        threads[i] = new SamplerInitThread(filenames, genome_filename, ts, fms,
                                           run_gc_correction, qsamplers,
                                           indexes);
        threads[i]->start();
    }

    for (unsigned int i = 0; i < K; ++i) indexes.push(i);
    for (unsigned int i = 0; i < constants::num_threads; ++i) indexes.push(-1);

    for (unsigned int i = 0; i < constants::num_threads; ++i) {
        threads[i]->join();
        delete threads[i];
    }
}


void Analyze::cleanup()
{
    BOOST_FOREACH (FragmentModel* fm, fms) {
        delete fm;
    }
    fms.clear();

    BOOST_FOREACH (Sampler* qs, qsamplers) {
        delete qs;
    }
    qsamplers.clear();
}


void Analyze::run()
{
    setup();

    typedef analyze_model_namespace::analyze_model model_t;
    typedef boost::ecuyer1988 rng_t;
    typedef stan::mcmc::adapt_diag_e_nuts<model_t, rng_t> sampler_t;

    //AnalyzeSamplerData sampler_data(*this);
    //model_t model(sampler_data, &std::cout);

    /* How is this going to work?
     *
     * Shit. I think we need to make ts and xs parameters. Otherwise, is there
     * any way to update them?
     *
     * Actually, maybe we maybe could if they weren't private.
     */

    cleanup();

#if 0
    C = data.size();
    load_quantification_data();
    compute_depth();
    choose_kde_bandwidth();

    typedef analyze_model_namespace::analyze_model model_t;
    typedef boost::ecuyer1988 rng_t;
    typedef stan::mcmc::adapt_diag_e_nuts<model_t, rng_t> sampler_t;

    AnalyzeSamplerData sampler_data(*this);
    model_t model(sampler_data, &std::cout);

    std::vector<double> cont_params(model.num_params_r());
    std::vector<int> disc_params(model.num_params_i());

    choose_initial_values(cont_params, disc_params);
    double init_log_prob;
    std::vector<double> init_grad;
    init_log_prob = stan::model::log_prob_grad<true, true>(model, cont_params, disc_params,
                                                           init_grad, &std::cout);

    if (!boost::math::isfinite(init_log_prob)) {
        Logger::abort("Non-finite initial log-probability: %f", init_log_prob);
    }

    BOOST_FOREACH (double d, init_grad) {
        if (!boost::math::isfinite(d)) {
            Logger::abort("Initial gradient contains a non-finite value: %f", d);
        }
    }

    // TODO: pass these numbers in
    unsigned int seed = 0;
    unsigned int num_iterations = 1000;
    unsigned int num_warmup = 500;
    unsigned int num_thin = 1;
    double epsilon_pm = 0.0;
    int max_treedepth = 10;
    double delta = 0.5;
    double gamma = 0.05;

    const char* task_name = "Sampling";
    Logger::push_task(task_name, num_iterations);

    rng_t base_rng(seed);
    stan::mcmc::sample s(cont_params, disc_params, 0, 0);
    sampler_t sampler(model, base_rng, num_warmup);
    sampler.seed(cont_params, disc_params);

    // warmup
    try {
        sampler.init_stepsize();
    } catch (std::runtime_error e) {
        Logger::abort("Error setting sampler step size: %s", e.what());
    }

    sampler.set_stepsize_jitter(epsilon_pm);
    sampler.set_max_depth(max_treedepth);
    sampler.get_stepsize_adaptation().set_delta(delta);
    sampler.get_stepsize_adaptation().set_gamma(gamma);
    sampler.get_stepsize_adaptation().set_mu(log(10 * sampler.get_nominal_stepsize()));
    sampler.engage_adaptation();

    for (unsigned int i = 0; i < num_warmup; ++i) {
        s = sampler.transition(s);
        Logger::get_task(task_name).inc();
    }

    sampler.disengage_adaptation();

    // sampling

    // TODO: more coherent output
    std::fstream diagnostic_stream("isolator_analyze_diagnostics.csv",
                                    std::fstream::out);
    std::fstream sample_stream("isolator_analyze_samples.csv",
                                std::fstream::out);
    stan::io::mcmc_writer<model_t> writer(&sample_stream, &diagnostic_stream);

    for (unsigned int i = 0; i < num_iterations - num_warmup; ++i) {
        // TODO: do something with the samples
        s = sampler.transition(s);
        writer.print_sample_params(base_rng, s, sampler, model);
        //writer.print_diagnostic_params(s, sampler);
        Logger::get_task(task_name).inc();
    }

    diagnostic_stream.close();
    sample_stream.close();

    Logger::pop_task(task_name);
#endif
}


#if 0
// Compute normalization constants for each sample.
void Analyze::compute_depth()
{
    const char* task_name = "Computing sample normalization constants";
    Logger::push_task(task_name, quantification.size() * (M/10));

    std::vector<double> sortedcol(N);
    depth.resize(K);

    // This is a posterior mean upper-quartile, normalized to the first sample
    // so depth tends to be close to 1.
    unsigned int i = 0;
    BOOST_FOREACH (matrix<float>& Q, quantification) {
        for (unsigned int j = 0; j < M; ++j) {
            matrix_column<matrix<float> > col(Q, i);
            std::copy(col.begin(), col.end(), sortedcol.begin());
            std::sort(sortedcol.begin(), sortedcol.end());
            depth[i] += gsl_stats_quantile_from_sorted_data(&sortedcol.at(0), 1, N, 0.75);
            if (j % 10 == 0) Logger::get_task(task_name).inc();
        }
        depth[i] /= M;
        ++i;
    }

    for (unsigned int i = 1; i < K; ++i) {
        depth[i] = depth[i] / depth[0];
    }
    depth[0] = 1.0;

    Logger::pop_task(task_name);
}
#endif


#if 0
void Analyze::choose_initial_values(std::vector<double>& cont_params,
                                    std::vector<int>& disc_params)
{
    size_t off = 0;

    // initialize xs
    for (size_t j = 0; j < K; ++j) {
        for (size_t i = 0; i < N; ++i) {
            cont_params[off++] = std::max<double>(1e-20, quantification[j](i, 0));
        }
    }

    // initialize mu
    // TODO: smarter initialization
    for (size_t j = 0; j < C; ++j) {
        for (size_t i = 0; i < T; ++i) {
            cont_params[off++] = -10;
        }
    }

    // initialize sigma
    for (size_t j = 0; j < C; ++j) {
        for (size_t i = 0; i < T; ++i) {
            cont_params[off++] = 0.1;
        }
    }
}
#endif


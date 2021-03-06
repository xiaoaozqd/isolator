
#include <boost/accumulators/accumulators.hpp>
#include <boost/accumulators/statistics/extended_p_square.hpp>
#include <boost/accumulators/statistics/median.hpp>
#include <boost/accumulators/statistics/stats.hpp>
#include <boost/accumulators/statistics/tail_quantile.hpp>
#include <boost/array.hpp>
#include <boost/foreach.hpp>
#include <boost/math/special_functions/fpclassify.hpp>
#include <boost/numeric/ublas/matrix_proxy.hpp>
#include <boost/random/normal_distribution.hpp>
#include <boost/thread.hpp>
#include <boost/lexical_cast.hpp>

#include "analyze.hpp"
#include "constants.hpp"
#include "distributions.hpp"
#include "shredder.hpp"
#include "fastmath.hpp"

using namespace boost::numeric::ublas;
using namespace boost::accumulators;

static void assert_finite(double x)
{
    if (!boost::math::isfinite(x)) {
        Logger::abort("%f found where finite value expected.", x);
    }
}


class BetaDistributionSampler : public Shredder
{
    public:
        BetaDistributionSampler()
            : Shredder(1e-16, 1.0, 1e-5)
        {}

        // What is x here? I should really be sampling over a/b, right?
        double sample(rng_t& rng,
                      double a0, double b0, double prec,
                      double a_prior, double b_prior,
                      const double* data, size_t n)
        {
            this->a0 = a0;
            this->b0 = b0;
            this->prec = prec;
            this->a_prior = a_prior;
            this->b_prior = b_prior;
            this->data = data;
            this->n = n;
            return Shredder::sample(rng, a0 / (a0 + b0));
        }

    private:
        double a0, b0;
        double prec;
        double a_prior, b_prior;
        const double* data;
        size_t n;
        BetaLogPdf beta_logpdf;

    protected:
        double f(double x, double &d)
        {
            double fx = 0.0;
            d = 0.0;

            // prior
            fx += beta_logpdf.f(a_prior, b_prior, x);
            d += beta_logpdf.df_dx(a_prior, b_prior, x);

            // likelihood
            for (size_t i = 0; i < n; ++i) {
                fx += beta_logpdf.f(x * prec, (1.0 - x) * prec, data[i]);
                d += beta_logpdf.df_dgamma(x, prec, data[i]);
            }

            return fx;
        };
};


class NormalMuSampler
{
    public:
        NormalMuSampler()
        {
        }

        double sample(rng_t& rng, double sigma, const double* xs, size_t n,
                      double prior_mu, double prior_sigma)
        {
            double prior_var = prior_sigma * prior_sigma;
            double var = sigma * sigma;

            double part = 1/prior_var  + n/var;
            double posterior_mu =
                (prior_mu / prior_var + std::accumulate(xs, xs + n, 0.0) / var) / part;
            double posterior_sigma = sqrt(1 / part);

            return posterior_mu + random_normal(rng) * posterior_sigma;
        }

    private:
        boost::random::normal_distribution<double> random_normal;
};


class GammaMeanSampler : public Shredder
{
    public:
        GammaMeanSampler(double lower_bound, double upper_bound)
            : Shredder(lower_bound, upper_bound, 1e-10)
        {}

        double sample(rng_t& rng, double mean0, double shape,
                      const float* xs, const float* log_xs, size_t n,
                      double prior_mean, double prior_shape)
        {
            this->xs = xs;
            this->log_xs = log_xs;
            this->n = n;
            this->shape = shape;
            this->prior_mean = prior_mean;
            this->prior_shape = prior_shape;

            lgamma_shape = lgammaf(shape);

            likelihood_dmean.shape(shape);
            prior.mean(prior_mean);
            prior.shape(prior_shape);
            prior_dx.mean(prior_mean);
            prior_dx.shape(prior_shape);

            double ans = Shredder::sample(rng, mean0);

            return ans;
        }

    private:
        AltGammaLogPdfDMean likelihood_dmean;
        AltGammaLogPdf prior;
        AltGammaLogPdfDx prior_dx;


        double shape;
        const float* xs;
        const float* log_xs;
        size_t n;
        double prior_mean;
        double prior_shape;

        // precompute lgamma(shape)
        double lgamma_shape;

    public:
        double f(double mean, double& d)
        {
            // TODO: rewrite this stuff with AltGammaLogPdf somehow
            double lp = 0.0;
            d = 0.0;
            double scale = mean / shape;
            likelihood_dmean.mean(mean);
            for (size_t i = 0; i < n; ++i) {
                lp += -(lgamma_shape + shape * fastlog(scale)) +
                      ((shape - 1.0) * log_xs[i] - xs[i] / scale);
                d += likelihood_dmean.x(xs[i]);
            }

            lp += prior.x(mean);
            d += prior_dx.x(mean);

            return lp;
        }
};


class GammaShapeSampler : public Shredder
{
    public:
        GammaShapeSampler(double lower_bound, double upper_bound)
            : Shredder(lower_bound, upper_bound, 1e-2)
        {}

        double sample(rng_t& rng, const float* means, double shape0,
                      const float* xs, size_t n, double prior_alpha,
                      double prior_beta)
        {
            this->means = means;
            this->xs = xs;
            this->n = n;
            this->prior_alpha = prior_alpha;
            this->prior_beta = prior_beta;

            prior.set_alpha(prior_alpha);
            prior.set_beta(prior_beta);
            prior_dx.set_alpha(prior_alpha);
            prior_dx.set_beta(prior_beta);

            double shape = Shredder::sample(rng, shape0);
            assert_finite(shape);
            //return std::max<double>(lower_limit, std::min<double>(upper_limit, shape));
            return shape;
        }


    private:
        const float* means;
        const float* xs;
        size_t n;
        double prior_alpha;
        double prior_beta;

        AltGammaLogPdf likelihood;
        AltGammaLogPdfDShape likelihood_dshape;

        GammaLogPdf prior;
        GammaLogPdfDx prior_dx;

    public:
        double f(double shape, double& d)
        {
            d = 0.0;
            double lp = 0.0;

            likelihood.set_shape(shape);
            likelihood_dshape.set_shape(shape);
            for (size_t i = 0; i < n; ++i) {
                lp += likelihood.mean_x(means[i], xs[i]);
                d += likelihood_dshape.mean_x(means[i], xs[i]);
            }

            lp += prior.x(shape);
            d += prior_dx.x(shape);

            return lp;
        }
};


class NormalTMuSampler : public Shredder
{
    public:
        NormalTMuSampler(double lower_bound, double upper_bound)
            : Shredder(lower_bound, upper_bound, 1e-5)
        {
        }

        double sample(rng_t& rng, double mu0, double sigma, const float* xs,
                      size_t n, double prior_nu, double prior_mu,
                      double prior_sigma)
        {
            this->sigma = sigma;
            this->prior_nu = prior_nu;
            this->prior_mu = prior_mu;
            this->prior_sigma = prior_sigma;
            this->xs = xs;
            this->n = n;

            prior.set_nu(prior_nu);
            prior.set_mu(prior_mu);
            prior.set_sigma(prior_sigma);
            prior_dx.set_nu(prior_nu);
            prior_dx.set_mu(prior_mu);
            prior_dx.set_sigma(prior_sigma);

            double ans = Shredder::sample(rng, mu0);

            return ans;
        }

    private:
        StudentsTLogPdf prior;
        StudentsTLogPdfDx prior_dx;
        NormalLogPdf likelihood_logpdf;
        double sigma;
        double prior_nu;
        double prior_mu;
        double prior_sigma;
        const float* xs;
        size_t n;

    public:
        double f(double mu, double& d)
        {
            double lp = prior.x(mu);
            d = prior_dx.x(mu);

            d += likelihood_logpdf.df_dmu(mu, sigma, xs, n);
            lp += likelihood_logpdf.f(mu, sigma, xs, n);

            return lp;
        }
};



class StudentTMuSampler : public Shredder
{
    public:
        StudentTMuSampler(double lower_bound, double upper_bound)
            : Shredder(lower_bound, upper_bound, 1e-5)
        {
        }

        double sample(rng_t& rng,
                      double mu0, double nu, double sigma, const float* xs, size_t n,
                      double prior_mu, double prior_sigma)
        {
            this->nu = nu;
            this->sigma = sigma;
            this->prior_mu = prior_mu;
            this->prior_sigma = prior_sigma;
            this->xs = xs;
            this->n = n;

            likelihood.set_nu(nu);
            likelihood.set_sigma(sigma);
            likelihood_dmu.set_nu(nu);
            likelihood_dmu.set_sigma(sigma);

            return Shredder::sample(rng, mu0);
        }


    private:
        StudentsTLogPdf likelihood;
        StudentsTLogPdfDMu likelihood_dmu;

        NormalLogPdf prior_logpdf;
        double nu;
        double sigma;
        double prior_mu;
        double prior_sigma;
        const float* xs;
        size_t n;


    protected:
        double f(double mu, double& d)
        {
            d = 0;
            double lp = 0.0;

            d += prior_logpdf.df_dx(prior_mu, prior_sigma, &mu, 1);
            lp += prior_logpdf.f(prior_mu, prior_sigma, &mu, 1);

            likelihood.set_mu(mu);
            likelihood_dmu.set_mu(mu);
            for (size_t i = 0; i < n; ++i) {
                d += likelihood_dmu.x(xs[i]);
                lp += likelihood.x(xs[i]);
            }

            return lp;
        }
};


class NormalSigmaSampler
{
    public:
        NormalSigmaSampler()
        {
        }

        double sample(rng_t& rng, const float* xs, size_t n,
                      double prior_alpha, double prior_beta)
        {
            double posterior_alpha = prior_alpha + n / 2.0;

            double part = 0.0;
            for (size_t i = 0; i < n; ++i) {
                part += xs[i] * xs[i];
            }
            double posterior_beta = prior_beta + part / 2.0;

            boost::random::gamma_distribution<double> dist(posterior_alpha, 1/posterior_beta);
            return sqrt(1 / dist(rng));
        }
};


class GammaNormalSigmaSampler : public Shredder
{
    public:
        GammaNormalSigmaSampler()
            : Shredder(1e-8, 1e5, 1e-5)
        {
        }

        double sample(rng_t& rng, double sigma0, const float* xs, size_t n,
                      double prior_alpha, double prior_beta)
        {
            this->prior_alpha = prior_alpha;
            this->prior_beta = prior_beta;
            this->xs = xs;
            this->n = n;

            prior.set_alpha(prior_alpha);
            prior.set_beta(prior_beta);
            prior_dx.set_alpha(prior_alpha);
            prior_dx.set_beta(prior_beta);

            return Shredder::sample(rng, sigma0);
        }


    private:
        double prior_alpha, prior_beta;
        const float* xs;
        size_t n;

        NormalLogPdf likelihood_logpdf;
        GammaLogPdf prior;
        GammaLogPdf prior_dx;

    protected:
        double f(double sigma, double& d)
        {
            d = 0.0;
            double lp = 0.0;

            lp += likelihood_logpdf.f(0.0, sigma, xs, n);
            d += likelihood_logpdf.df_dsigma(0.0, sigma, xs, n);

            lp += prior.x(sigma);
            d += prior_dx.x(sigma);

            return lp;
        }
};


class GammaLogNormalSigmaSampler : public Shredder
{
    public:
        GammaLogNormalSigmaSampler()
            : Shredder(1e-8, 1e5, 1e-5)
        {
        }

        double sample(rng_t& rng, const double* mu,
                      double sigma0, const double* xs, size_t n,
                      double prior_alpha, double prior_beta)
        {
            this->prior_alpha = prior_alpha;
            this->prior_beta = prior_beta;
            this->mu = mu;
            this->xs = xs;
            this->n = n;

            prior.set_alpha(prior_alpha);
            prior.set_beta(prior_beta);
            prior_dx.set_alpha(prior_alpha);
            prior_dx.set_beta(prior_beta);

            return Shredder::sample(rng, sigma0);
        }


    private:
        double prior_alpha, prior_beta;
        const double* mu;
        const double* xs;
        size_t n;

        LogNormalLogPdf likelihood_logpdf;
        GammaLogPdf prior;
        GammaLogPdfDx prior_dx;

    protected:
        double f(double sigma, double& d)
        {
            d = 0.0;
            double lp = 0.0;

            for (size_t i = 0; i < n; ++i) {
                lp += likelihood_logpdf.f(mu[i], sigma, &xs[i], 1);
                d += likelihood_logpdf.df_dsigma(mu[i], sigma, &xs[i], 1);
            }

            lp += prior.x(sigma);
            d += prior_dx.x(sigma);

            return lp;
        }
};


class ConditionSpliceEtaSampler : public Shredder
{
    public:
        ConditionSpliceEtaSampler()
            : Shredder(-10, 10, 1e-5)
        {}

        double sample(rng_t& rng,
                      double condition_splice_eta,
                      const std::vector<float>& unadj_condition_splice_mu,
                      double unadj_condition_splice_sigma,
                      const matrix_column<matrix<float> >& splice_data,
                      const std::vector<float>& sample_mu,
                      const std::vector<std::vector<int> >& condition_samples,
                      double experiment_splice_nu,
                      double experiment_splice_mu,
                      double experiment_splice_sigma,
                      double condition_splice_alpha,
                      double condition_splice_beta)
        {
            this->unadj_condition_splice_mu = &unadj_condition_splice_mu;
            this->unadj_condition_splice_sigma = unadj_condition_splice_sigma;
            this->splice_data = &splice_data;
            this->sample_mu = &sample_mu;
            this->condition_samples = &condition_samples;
            this->experiment_splice_mu = experiment_splice_mu;
            this->experiment_splice_sigma = experiment_splice_sigma;
            this->experiment_splice_nu = experiment_splice_nu;
            this->condition_splice_alpha = condition_splice_alpha;
            this->condition_splice_beta = condition_splice_beta;

            mu_prior.set_nu(experiment_splice_nu);
            sigma_prior.set_alpha(condition_splice_alpha);
            sigma_prior.set_beta(condition_splice_beta);

            if (data_tmp.size() < splice_data.size()) {
                data_tmp.resize(splice_data.size());
            }

            return Shredder::sample(rng, condition_splice_eta);
        }

    private:
        NormalLogPdf likelihood_logpdf;
        StudentsTLogPdf mu_prior;
        GammaLogPdf sigma_prior;

        const std::vector<float>* unadj_condition_splice_mu;
        const matrix_column<matrix<float> >* splice_data;
        const std::vector<std::vector<int> >* condition_samples;
        const std::vector<float>* sample_mu;

        double unadj_condition_splice_sigma,
               experiment_splice_mu,
               experiment_splice_sigma,
               experiment_splice_nu,
               condition_splice_alpha,
               condition_splice_beta;

        std::vector<float> data_tmp;

    protected:
        double f(double eta, double& d)
        {
            double lp = 0.0;
            d = 0.0;
            double condition_splice_sigma = fabs(eta) * unadj_condition_splice_sigma;
            mu_prior.set_sigma(condition_splice_sigma);

            for (size_t i = 0; i < unadj_condition_splice_mu->size(); ++i) {
                for (size_t l = 0; l < (*condition_samples)[i].size(); ++l) {
                    size_t sample_idx = (*condition_samples)[i][l];
                    data_tmp[l] = (*splice_data)(sample_idx);
                }

                double condition_splice_mu =
                    eta * (*unadj_condition_splice_mu)[i] + (*sample_mu)[i];
                mu_prior.set_mu(condition_splice_mu);

                lp += mu_prior.x(condition_splice_mu);

                lp += likelihood_logpdf.f(condition_splice_mu, condition_splice_sigma,
                                          &data_tmp.at(0), (*condition_samples)[i].size());
            }

            lp += sigma_prior.x(condition_splice_sigma);

            return lp;
        }
};


// Sample parameters giving the mean within-group splicing proportions per
// condition.
class ConditionSpliceMuSigmaEtaSamplerThread
{
    public:
        ConditionSpliceMuSigmaEtaSamplerThread(
                        std::vector<std::vector<std::vector<float> > >& condition_splice_mu,
                        std::vector<std::vector<float> >& condition_splice_sigma,
                        std::vector<std::vector<float> >& condition_splice_eta,
                        const std::vector<std::vector<float> >& experiment_splice_mu,
                        double& experiment_splice_sigma,
                        double experiment_splice_nu,
                        const double& condition_splice_alpha,
                        const double& condition_splice_beta,
                        const matrix<float>& Q,
                        const std::vector<unsigned int>& spliced_tgroup_indexes,
                        const std::vector<std::vector<unsigned int> >& tgroup_tids,
                        const std::vector<int>& condition,
                        const std::vector<std::vector<int> >& condition_samples,
                        Queue<IdxRange>& spliced_tgroup_queue,
                        Queue<int>& notify_queue,
                        std::vector<rng_t>& rng_pool)
            : condition_splice_mu(condition_splice_mu)
            , condition_splice_sigma(condition_splice_sigma)
            , condition_splice_eta(condition_splice_eta)
            , experiment_splice_mu(experiment_splice_mu)
            , experiment_splice_sigma(experiment_splice_sigma)
            , experiment_splice_nu(experiment_splice_nu)
            , condition_splice_alpha(condition_splice_alpha)
            , condition_splice_beta(condition_splice_beta)
            , Q(Q)
            , spliced_tgroup_indexes(spliced_tgroup_indexes)
            , tgroup_tids(tgroup_tids)
            , condition(condition)
            , condition_samples(condition_samples)
            , spliced_tgroup_queue(spliced_tgroup_queue)
            , notify_queue(notify_queue)
            , rng_pool(rng_pool)
            , mu_sampler(-1, 2)
            , burnin_state(true)
            , thread(NULL)
        {
            C = condition_splice_mu.size();
            K = Q.size1();
        }


        void end_burnin()
        {
            burnin_state = false;
        }


        void run()
        {
            // temporary array for storing observation marginals
            std::vector<float> data(K);

            // temporary space for sampling precision
            size_t max_size2 = 0;
            BOOST_FOREACH (const std::vector<unsigned int>& tids, tgroup_tids) {
                max_size2 = std::max<size_t>(tids.size(), max_size2);
            }
            matrix<float> dataj(K, max_size2);

            std::vector<float> unadj_mu(C);
            std::vector<float> sample_mu(C);

            while (true) {
                IdxRange js = spliced_tgroup_queue.pop();
                if (js.first == -1) break;

                for (int j = js.first; j < js.second; ++j) {
                    size_t tgroup = spliced_tgroup_indexes[j];
                    rng_t& rng = rng_pool[j];

                    for (size_t i = 0; i < K; ++i) {
                        double datasum = 0.0;
                        for (size_t k = 0; k < tgroup_tids[tgroup].size(); ++k) {
                            unsigned int tid = tgroup_tids[tgroup][k];
                            dataj(i, k) = Q(i, tid);
                            datasum += dataj(i, k);
                        }

                        for (size_t k = 0; k < tgroup_tids[tgroup].size(); ++k) {
                            dataj(i, k) /= datasum;
                        }
                    }

                    // sample eta
                    for (size_t k = 0; k < tgroup_tids[tgroup].size(); ++k) {
                        double unadj_sigma = condition_splice_sigma[j][k] /
                                             fabs(condition_splice_eta[j][k]);
                        for (size_t i = 0; i < C; ++i) {
                            sample_mu[i] = 0.0;
                            for (size_t l = 0; l < condition_samples[i].size(); ++l) {
                                size_t sample_idx = condition_samples[i][l];
                                sample_mu[i] += dataj(sample_idx, k);
                            }
                            sample_mu[i] /= condition_samples[i].size();

                            unadj_mu[i] =
                                (condition_splice_mu[i][j][k] - sample_mu[i]) /
                                condition_splice_eta[j][k];
                        }

                        matrix_column<matrix<float> > col(dataj, k);

                        condition_splice_eta[j][k] = eta_sampler.sample(
                                rng, condition_splice_eta[j][k],
                                unadj_mu, unadj_sigma, col, sample_mu,
                                condition_samples,
                                experiment_splice_nu,
                                experiment_splice_mu[j][k],
                                experiment_splice_sigma,
                                condition_splice_alpha,
                                condition_splice_beta);

                        // readjust mu and sigma by eta
                        condition_splice_sigma[j][k] =
                            unadj_sigma * fabs(condition_splice_eta[j][k]);

                        for (size_t i = 0; i < C; ++i) {
                            condition_splice_mu[i][j][k] =
                                unadj_mu[i] * condition_splice_eta[j][k] +
                                sample_mu[i];
                        }

                        // reset eta to 1.0 after each sample, to avoid
                        // very large or small numbers on subsequent samples.
                        condition_splice_eta[j][k] = 1.0;
                    }

                    // sample mu
                    for (size_t i = 0; i < C; ++i) {
                        for (size_t k = 0; k < tgroup_tids[tgroup].size(); ++k) {
                            for (size_t l = 0; l < condition_samples[i].size(); ++l) {
                                size_t sample_idx = condition_samples[i][l];
                                data[l] = dataj(sample_idx, k);
                            }

                            condition_splice_mu[i][j][k] =
                                mu_sampler.sample(rng,
                                                  condition_splice_mu[i][j][k],
                                                  condition_splice_sigma[j][k],
                                                  &data.at(0),
                                                  condition_samples[i].size(),
                                                  experiment_splice_nu,
                                                  experiment_splice_mu[j][k],
                                                  experiment_splice_sigma);
                        }
                    }

                    // sample sigma
                    for (size_t k = 0; k < tgroup_tids[tgroup].size(); ++k) {
                        matrix_column<matrix<float> > col(dataj, k);
                        std::copy(col.begin(), col.end(), data.begin());

                        for (size_t i = 0; i < K; ++i) {
                            data[i] = data[i] - condition_splice_mu[condition[i]][j][k];
                        }

                        // During burn-in we force the condition variance
                        // to be quite high. Otherwise, if a gene is initalized
                        // in a very low probability state it can be slow to make
                        // progress towards reasonable values.
                        if (burnin_state) {
                            condition_splice_sigma[j][k] = 1.0;
                        }
                        else {
                            condition_splice_sigma[j][k] =
                                sigma_sampler.sample(rng,
                                                     condition_splice_sigma[j][k],
                                                     &data.at(0), K,
                                                     condition_splice_alpha,
                                                     condition_splice_beta);
                            condition_splice_sigma[j][k] =
                                std::max<double>(constants::analyze_min_splice_sigma,
                                                condition_splice_sigma[j][k]);
                        }
                    }
                }

                notify_queue.push(1);
            }
        }

        void start()
        {
            thread = new boost::thread(
                    boost::bind(&ConditionSpliceMuSigmaEtaSamplerThread::run, this));
        }

        void join()
        {
            thread->join();
            delete thread;
            thread = NULL;
        }

    private:
        // what we are sampling over
        std::vector<std::vector<std::vector<float> > >& condition_splice_mu;
        std::vector<std::vector<float> >& condition_splice_sigma;
        std::vector<std::vector<float> >& condition_splice_eta;

        const std::vector<std::vector<float> >& experiment_splice_mu;
        double& experiment_splice_sigma;
        double experiment_splice_nu;

        const double& condition_splice_alpha;
        const double& condition_splice_beta;

        // current transcript quantification
        const matrix<float>& Q;

        const std::vector<unsigned int>& spliced_tgroup_indexes;
        const std::vector<std::vector<unsigned int> >& tgroup_tids;
        const std::vector<int>& condition;
        const std::vector<std::vector<int> >& condition_samples;

        Queue<IdxRange>& spliced_tgroup_queue;
        Queue<int>& notify_queue;

        std::vector<rng_t>& rng_pool;

        size_t C, K;
        NormalTMuSampler mu_sampler;
        GammaNormalSigmaSampler sigma_sampler;
        ConditionSpliceEtaSampler eta_sampler;
        bool burnin_state;

        boost::thread* thread;
};


class ExperimentSpliceMuSigmaSamplerThread
{
    public:
        ExperimentSpliceMuSigmaSamplerThread(
                std::vector<std::vector<float> >& experiment_splice_mu,
                double& experiment_splice_sigma,
                double experiment_splice_nu,
                const std::vector<std::vector<std::vector<float> > >&
                    condition_splice_mu,
                const std::vector<unsigned int>& spliced_tgroup_indexes,
                const std::vector<std::vector<unsigned int> >& tgroup_tids,
                double experiment_splice_mu0,
                double experiment_splice_sigma0,
                Queue<IdxRange>& spliced_tgroup_queue,
                Queue<int>& notify_queue,
                std::vector<rng_t>& rng_pool)
            : experiment_splice_mu(experiment_splice_mu)
            , experiment_splice_sigma(experiment_splice_sigma)
            , experiment_splice_nu(experiment_splice_nu)
            , condition_splice_mu(condition_splice_mu)
            , spliced_tgroup_indexes(spliced_tgroup_indexes)
            , tgroup_tids(tgroup_tids)
            , experiment_splice_mu0(experiment_splice_mu0)
            , experiment_splice_sigma0(experiment_splice_sigma0)
            , spliced_tgroup_queue(spliced_tgroup_queue)
            , notify_queue(notify_queue)
            , rng_pool(rng_pool)
            , mu_sampler(-1, 2)
            , burnin_state(true)
            , thread(NULL)
        {
            C = condition_splice_mu.size();
        }


        void end_burnin()
        {
            burnin_state = false;
        }


        void run()
        {
            std::vector<float> data(C);

            while (true) {
                IdxRange js = spliced_tgroup_queue.pop();
                if (js.first == -1) break;

                for (int j = js.first; j < js.second; ++j) {
                    rng_t& rng = rng_pool[j];

                    size_t tgroup = spliced_tgroup_indexes[j];

                    for (size_t k = 0; k < tgroup_tids[tgroup].size(); ++k) {
                        for (size_t i = 0; i < C; ++i) {
                            data[i] = condition_splice_mu[i][j][k];
                        }

                        experiment_splice_mu[j][k] =
                            mu_sampler.sample(rng,
                                              experiment_splice_mu[j][k],
                                              experiment_splice_nu,
                                              experiment_splice_sigma,
                                              &data.at(0), C,
                                              experiment_splice_mu0,
                                              experiment_splice_sigma0);
                    }
                }

                notify_queue.push(1);
            }
        }

        void start()
        {
            thread = new boost::thread(
                boost::bind(&ExperimentSpliceMuSigmaSamplerThread::run, this));
        }

        void join()
        {
            thread->join();
            delete thread;
            thread = NULL;
        }

    private:
        std::vector<std::vector<float> >& experiment_splice_mu;
        double& experiment_splice_sigma;
        double experiment_splice_nu;
        const std::vector<std::vector<std::vector<float> > >&
            condition_splice_mu;
        const std::vector<unsigned int>& spliced_tgroup_indexes;
        const std::vector<std::vector<unsigned int> >& tgroup_tids;
        double experiment_splice_mu0;
        double experiment_splice_sigma0;
        Queue<IdxRange>& spliced_tgroup_queue;
        Queue<int>& notify_queue;
        std::vector<rng_t>& rng_pool;

        size_t C;
        StudentTMuSampler mu_sampler;
        NormalSigmaSampler sigma_sampler;
        bool burnin_state;

        boost::thread* thread;
};


class ConditionMeanShapeSamplerThread
{
    public:
        ConditionMeanShapeSamplerThread(
                                   matrix<float>& Q,
                                   matrix<float>& condition_mean,
                                   std::vector<float>& condition_shape,
                                   std::vector<float>& experiment_mean,
                                   double& experiment_shape,
                                   const double& condition_shape_alpha,
                                   const double& condition_shape_beta,
                                   const std::vector<int>& condition,
                                   const std::vector<std::vector<int> >& condition_samples,
                                   Queue<IdxRange>& transcript_queue,
                                   Queue<int>& notify_queue,
                                   std::vector<rng_t>& rng_pool)
            : Q(Q)
            , condition_mean(condition_mean)
            , condition_shape(condition_shape)
            , experiment_mean(experiment_mean)
            , experiment_shape(experiment_shape)
            , condition_shape_alpha(condition_shape_alpha)
            , condition_shape_beta(condition_shape_beta)
            , condition(condition)
            , condition_samples(condition_samples)
            , transcript_queue(transcript_queue)
            , notify_queue(notify_queue)
            , rng_pool(rng_pool)
            , burnin_state(true)
            , thread(NULL)
            , mu_sampler(1e-12, 1)
            , shape_sampler(0.1, 5.0)
        {
            K = Q.size1();
            C = condition_samples.size();
            xs.resize(K);
            log_xs.resize(K);
            xs_mu.resize(K);
        }

        void end_burnin()
        {
            burnin_state = false;
        }

        void run()
        {
            while (true) {
                IdxRange transcripts = transcript_queue.pop();
                if (transcripts.first == -1) break;

                for (int tid = transcripts.first; tid < transcripts.second; ++tid) {
                    rng_t& rng = rng_pool[tid];

                    // sample mu
                    for (size_t i = 0; i < C; ++i) {
                        size_t l = 0;
                        BOOST_FOREACH (int j, condition_samples[i]) {
                            xs[l] = Q(j, tid);
                            log_xs[l] = fastlog(xs[l]);
                            ++l;
                        }

                        condition_mean(i, tid) = mu_sampler.sample(
                                rng, condition_mean(i, tid),
                                condition_shape[tid], &xs.at(0), &log_xs.at(0), l,
                                experiment_mean[tid], experiment_shape);
                        assert_finite(condition_mean(i, tid));
                    }

                    for (size_t i = 0; i < K; ++i) {
                        xs_mu[i] = condition_mean(condition[i], tid);
                        xs[i] = Q(i, tid);
                    }

                    // Force sigma to something rather large to avoid getting
                    // when initialized in an extremely low probability state
                    if (burnin_state) condition_shape[tid] = 1.0;
                    else {
                        condition_shape[tid] =
                            shape_sampler.sample(
                                    rng, &xs_mu.at(0),
                                    condition_shape[tid],
                                    &xs.at(0), K,
                                    condition_shape_alpha,
                                    condition_shape_beta);
                    }
                    assert_finite(condition_shape[tid]);
                }
                notify_queue.push(1);
            }
        }

        void start()
        {
            thread = new boost::thread(boost::bind(&ConditionMeanShapeSamplerThread::run, this));
        }

        void join()
        {
            thread->join();
            delete thread;
            thread = NULL;
        }

    private:
        matrix<float>& Q;
        matrix<float>& condition_mean;
        std::vector<float>& condition_shape;
        std::vector<float>& experiment_mean;
        double& experiment_shape;
        const double& condition_shape_alpha;
        const double& condition_shape_beta;
        const std::vector<int>& condition;
        const std::vector<std::vector<int> >& condition_samples;
        Queue<IdxRange>& transcript_queue;
        Queue<int>& notify_queue;
        std::vector<rng_t>& rng_pool;
        bool burnin_state;
        boost::thread* thread;

        // temporary data vector
        std::vector<float> xs, log_xs;
        std::vector<float> xs_mu;

        // number of replicates
        size_t K;

        // number of conditions
        size_t C;

        GammaMeanSampler mu_sampler;
        GammaShapeSampler shape_sampler;
};


// Sample from the beta parameter of a gamm distribution
class GammaBetaSampler : public Shredder
{
    public:
        GammaBetaSampler()
            : Shredder(1e-10, 1e5, 1e-4)
        {}

        double sample(rng_t& rng,
                      double beta0, double alpha,
                      double beta_a, double beta_b,
                      const float* xs, size_t n)
        {
            this->alpha = alpha;
            this->beta_a = beta_a;
            this->beta_b = beta_b;
            this->xs = xs;
            this->n = n;

            likelihood.set_alpha(alpha);
            likelihood_dbeta.set_alpha(alpha);
            prior.set_alpha(beta_a);
            prior.set_beta(beta_b);

            return Shredder::sample(rng, beta0);
        }

    private:
        double alpha;
        double beta_a, beta_b;
        const float* xs;
        size_t n;

        GammaLogPdf likelihood;
        GammaLogPdfDBeta likelihood_dbeta;

        GammaLogPdf prior;
        GammaLogPdfDx prior_dx;

    protected:
        double f(double beta, double &d)
        {
            d = 0.0;
            double lp = 0.0;

            likelihood.set_beta(beta);
            likelihood_dbeta.set_beta(beta);
            for (size_t i = 0; i < n; ++i) {
                lp += likelihood.x(xs[i]);
                d += likelihood_dbeta.x(xs[i]);
            }

            lp += prior.x(beta);
            d += prior_dx.x(beta);

            return lp;
        }
};


class BetaSampler : public Shredder
{
    public:
        BetaSampler()
            : Shredder(1e-16, 1e5, 1e-5)
        {}

        double sample(rng_t& rng,
                      double beta0, double alpha,
                      double alpha_beta, double beta_beta,
                      const double* sigmas, size_t n)
        {
            this->alpha = alpha;
            this->alpha_beta = alpha_beta;
            this->beta_beta = beta_beta;
            this->sigmas = sigmas;
            this->n = n;
            return Shredder::sample(rng, beta0);
        }

    private:
        double alpha;
        double alpha_beta;
        double beta_beta;
        const double* sigmas;
        size_t n;

        InvGammaLogPdf prior_logpdf;
        SqInvGammaLogPdf likelihood_logpdf;

    protected:
        double f(double beta, double &d)
        {
            d = 0.0;
            double fx = 0.0;

            d += prior_logpdf.df_dx(alpha_beta, beta_beta, &beta, 1);
            fx += prior_logpdf.f(alpha_beta, beta_beta, &beta, 1);

            d += likelihood_logpdf.df_dbeta(alpha, beta, sigmas, n);
            fx += likelihood_logpdf.f(alpha, beta, sigmas, n);

            return fx;
        }
};


class ExperimentMeanShapeSamplerThread
{
    public:
        ExperimentMeanShapeSamplerThread(
                std::vector<float>& experiment_mean,
                double& experiment_shape,
                double experiment_mean0,
                double experiment_shape0,
                matrix<float>& condition_mean,
                Queue<IdxRange>& transcript_queue,
                Queue<int>& notify_queue,
                std::vector<rng_t>& rng_pool)
            : experiment_mean(experiment_mean)
            , experiment_shape(experiment_shape)
            , experiment_mean0(experiment_mean0)
            , experiment_shape0(experiment_shape0)
            , condition_mean(condition_mean)
            , transcript_queue(transcript_queue)
            , notify_queue(notify_queue)
            , rng_pool(rng_pool)
            , thread(NULL)
            , mu_sampler(1e-12, 1)
        {
        }

        void run()
        {
            size_t C = condition_mean.size1();
            std::vector<float> xs(C), log_xs(C);

            while (true) {
                IdxRange transcripts = transcript_queue.pop();
                if (transcripts.first == -1) break;

                for (int tid = transcripts.first; tid < transcripts.second; ++tid) {
                    for (size_t i = 0; i < C; ++i) {
                        xs[i] = condition_mean(i, tid);
                        log_xs[i] = fastlog(xs[i]);
                    }

                    rng_t& rng = rng_pool[tid];

                    experiment_mean[tid] =
                        mu_sampler.sample(rng,
                                          experiment_mean[tid],
                                          experiment_shape,
                                          &xs.at(0), &log_xs.at(0), C,
                                          experiment_mean0,
                                          experiment_shape0);
                }

                notify_queue.push(1);
            }
        }

        void start()
        {
            thread = new boost::thread(boost::bind(&ExperimentMeanShapeSamplerThread::run, this));
        }

        void join()
        {
            thread->join();
            delete thread;
            thread = NULL;
        }

    private:
        std::vector<float>& experiment_mean;
        double& experiment_shape;
        double experiment_mean0, experiment_shape0;
        matrix<float>& condition_mean;

        Queue<IdxRange>& transcript_queue;
        Queue<int>& notify_queue;
        std::vector<rng_t>& rng_pool;
        boost::thread* thread;
        boost::random::normal_distribution<double> random_normal;

        GammaMeanSampler mu_sampler;
};


Analyze::Analyze(unsigned int rng_seed,
                 size_t burnin,
                 size_t num_samples,
                 TranscriptSet& transcripts,
                 const char* genome_filename,
                 bool run_seqbias_correction,
                 bool run_gc_correction,
                 bool run_3p_correction,
                 bool run_frag_correction,
                 bool collect_qc_data,
                 bool nopriors,
                 std::set<std::string> excluded_seqs,
                 std::set<std::string> bias_training_seqnames,
                 double experiment_shape_alpha,
                 double experiment_shape_beta,
                 double experiment_splice_sigma_alpha, double experiment_splice_sigma_beta,
                 double condition_shape_alpha,
                 double condition_shape_beta_a,
                 double condition_shape_beta_b,
                 double condition_splice_alpha,
                 double condition_splice_beta_a,
                 double condition_splice_beta_b)
    : burnin(burnin)
    , num_samples(num_samples)
    , transcripts(transcripts)
    , genome_filename(genome_filename)
    , run_seqbias_correction(run_seqbias_correction)
    , run_gc_correction(run_gc_correction)
    , run_3p_correction(run_3p_correction)
    , run_frag_correction(run_frag_correction)
    , excluded_seqs(excluded_seqs)
    , bias_training_seqnames(bias_training_seqnames)
    , collect_qc_data(collect_qc_data)
    , nopriors(nopriors)
    , K(0)
    , C(0)
    , N(0)
    , T(0)
    , rng_seed(rng_seed)
{
    N = transcripts.size();
    T = transcripts.num_tgroups();

    this->experiment_shape_alpha = experiment_shape_alpha;
    this->experiment_shape_beta = experiment_shape_beta;

    this->experiment_splice_sigma_alpha = experiment_splice_sigma_alpha;
    this->experiment_splice_sigma_beta = experiment_splice_sigma_beta;

    this->condition_splice_alpha  = condition_splice_alpha;
    this->condition_splice_beta_a = condition_splice_beta_a;
    this->condition_splice_beta_b = condition_splice_beta_b;

    this->condition_shape_alpha  = condition_shape_alpha;
    this->condition_shape_beta_a = condition_shape_beta_a;
    this->condition_shape_beta_b = condition_shape_beta_b;

    this->experiment_mean0  = constants::analyze_experiment_mean0;
    this->experiment_shape0 = constants::analyze_experiment_shape0;

    experiment_splice_mu0    = constants::analyze_experiment_splice_mu0;
    experiment_splice_sigma0 = constants::analyze_experiment_splice_sigma0;
    experiment_splice_nu = constants::analyze_experiment_splice_nu;

    scale_work.resize(N);

    gamma_beta_sampler = new GammaBetaSampler();
    invgamma_beta_sampler = new BetaSampler();
    gamma_normal_sigma_sampler = new GammaNormalSigmaSampler();
    gamma_shape_sampler = new GammaShapeSampler(0.01, 20.0);

    tgroup_tids = transcripts.tgroup_tids();

    for (size_t i = 0; i < tgroup_tids.size(); ++i) {
        if (tgroup_tids[i].size() > 1) {
            spliced_tgroup_indexes.push_back(i);
        }
    }

    rng.seed(rng_seed);

    splice_rng_pool.resize(spliced_tgroup_indexes.size());
    BOOST_FOREACH (rng_t& rng, splice_rng_pool) {
        rng.seed(rng_seed);
        ++rng_seed;
    }

    transcript_rng_pool.resize(N);
    BOOST_FOREACH (rng_t& rng, transcript_rng_pool) {
        rng.seed(rng_seed);
        ++rng_seed;
    }

    Logger::debug("Number of transcription groups: %u", T);
    Logger::debug("Number of tgroups with multiple isoforms: %u",
                  spliced_tgroup_indexes.size());
}


Analyze::~Analyze()
{
    delete gamma_beta_sampler;
    delete invgamma_beta_sampler;
    delete gamma_shape_sampler;
    delete gamma_normal_sigma_sampler;
}


void Analyze::add_sample(const char* condition_name, const char* filename)
{
    int c;
    std::map<std::string, int>::iterator it = condition_index.find(condition_name);
    if (it == condition_index.end()) {
        c = (int) condition_index.size();
        condition_index[condition_name] = c;
    }
    else c = (int) it->second;

    filenames.push_back(filename);
    condition.push_back(c);
    if (c >= (int) condition_samples.size()) condition_samples.resize(c + 1);
    condition_samples[c].push_back(K);
    ++K;
}


// Thread to initialize samplers and fragment models
class SamplerInitThread
{
    public:
        SamplerInitThread(unsigned int rng_seed,
                          const std::vector<std::string>& filenames, const char* fa_fn,
                          TranscriptSet& transcripts,
                          std::vector<FragmentModel*>& fms,
                          bool run_seqbias_correction,
                          bool run_gc_correction,
                          bool run_3p_correction,
                          bool run_frag_correction,
                          bool collect_qc_data,
                          std::set<std::string> excluded_seqs,
                          std::set<std::string> bias_training_seqnames,
                          std::vector<Sampler*>& samplers,
                          Queue<int>& indexes)
            : filenames(filenames)
            , fa_fn(fa_fn)
            , transcripts(transcripts)
            , fms(fms)
            , run_seqbias_correction(run_seqbias_correction)
            , run_gc_correction(run_gc_correction)
            , run_3p_correction(run_3p_correction)
            , run_frag_correction(run_frag_correction)
            , collect_qc_data(collect_qc_data)
            , excluded_seqs(excluded_seqs)
            , bias_training_seqnames(bias_training_seqnames)
            , samplers(samplers)
            , indexes(indexes)
            , rng_seed(rng_seed)
            , thread(NULL)
        {
        }

        void run()
        {
            int index;
            while (true) {
                if ((index = indexes.pop()) == -1) break;

                fms[index] = new FragmentModel();
                fms[index]->estimate(transcripts, filenames[index].c_str(), fa_fn,
                                     run_seqbias_correction, run_gc_correction,
                                     run_3p_correction, run_frag_correction,
                                     collect_qc_data, excluded_seqs,
                                     bias_training_seqnames);

                samplers[index] = new Sampler(rng_seed,
                                              filenames[index].c_str(), fa_fn,
                                              excluded_seqs, transcripts,
                                              *fms[index], run_frag_correction);
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
        TranscriptSet& transcripts;
        std::vector<FragmentModel*>& fms;
        bool run_seqbias_correction;
        bool run_gc_correction;
        bool run_3p_correction;
        bool run_frag_correction;
        bool collect_qc_data;
        std::set<std::string> excluded_seqs;
        std::set<std::string> bias_training_seqnames;

        std::vector<Sampler*>& samplers;

        Queue<int>& indexes;
        unsigned int rng_seed;

        boost::thread* thread;
};


// Threads to run sampler iterations
class SamplerTickThread
{
    public:
        SamplerTickThread(std::vector<Sampler*>& samplers,
                          matrix<float>& Q,
                          Queue<int>& tick_queue,
                          Queue<int>& tock_queue)
            : samplers(samplers)
            , Q(Q)
            , tick_queue(tick_queue)
            , tock_queue(tock_queue)
            , thread(NULL)
            , optimize_state(false)
        { }

        void run()
        {
            int index;
            while (true) {
                if ((index = tick_queue.pop()) == -1) break;

                if (optimize_state) {
                    samplers[index]->optimize();
                }
                else {
                    samplers[index]->sample();
                }

                const std::vector<float>& state = samplers[index]->state();

                matrix_row<matrix<float> > row(Q, index);
                std::copy(state.begin(), state.end(), row.begin());

                // notify of completion
                tock_queue.push(1);
            }
        }

        void start()
        {
            if (thread != NULL) return;
            thread = new boost::thread(boost::bind(&SamplerTickThread::run, this));
        }

        void join()
        {
            thread->join();
            delete thread;
            thread = NULL;
        }

        void set_optimize_state(bool state)
        {
            optimize_state = state;
        }


    private:
        std::vector<Sampler*> samplers;
        matrix<float>& Q;
        Queue<int>& tick_queue;
        Queue<int>& tock_queue;
        boost::thread* thread;

        bool optimize_state;

};


void Analyze::setup_samplers()
{
    fms.resize(K);
    qsamplers.resize(K);

    std::vector<SamplerInitThread*> threads(constants::num_threads);
    Queue<int> indexes;
    for (unsigned int i = 0; i < constants::num_threads; ++i) {
        threads[i] = new SamplerInitThread(rng_seed, filenames, genome_filename,
                                           transcripts, fms,
                                           run_seqbias_correction, run_gc_correction,
                                           run_3p_correction, run_frag_correction,
                                           collect_qc_data, excluded_seqs,
                                           bias_training_seqnames,
                                           qsamplers, indexes);
        threads[i]->start();
    }

    for (unsigned int i = 0; i < K; ++i) indexes.push(i);
    for (unsigned int i = 0; i < constants::num_threads; ++i) indexes.push(-1);

    for (unsigned int i = 0; i < constants::num_threads; ++i) {
        threads[i]->join();
        delete threads[i];
    }
}


void Analyze::setup_output(hid_t file_id)
{
    // transcript information
    // ----------------------
    {
        hsize_t dims[1] = { N };
        hid_t dataspace = H5Screate_simple_checked(1, dims, NULL);

        hid_t varstring_type = H5Tcopy(H5T_C_S1);
        if (varstring_type < 0 || H5Tset_size(varstring_type, H5T_VARIABLE) < 0) {
            Logger::abort("HDF5 type creation failed.");
        }

        const char** string_data = new const char* [N];

        // transcript_id table
        hid_t transcript_id_dataset =
            H5Dcreate2_checked(file_id, "/transcript_id", varstring_type,
                               dataspace, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);

        for (TranscriptSet::iterator t = transcripts.begin();
                t != transcripts.end(); ++t) {
            string_data[t->id] = t->transcript_id.get().c_str();
        }

        H5Dwrite_checked(transcript_id_dataset, varstring_type,
                         H5S_ALL, H5S_ALL, H5P_DEFAULT, string_data);
        H5Dclose(transcript_id_dataset);

        // gene_id table
        hid_t gene_id_dataset =
            H5Dcreate2_checked(file_id, "/gene_id", varstring_type,
                               dataspace, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);

        for (TranscriptSet::iterator t = transcripts.begin();
                t != transcripts.end(); ++t) {
            string_data[t->id] = t->gene_id.get().c_str();
        }

        H5Dwrite_checked(gene_id_dataset, varstring_type,
                         H5S_ALL, H5S_ALL, H5P_DEFAULT, string_data);
        H5Dclose(gene_id_dataset);


        // gene_name table
        hid_t gene_name_dataset =
            H5Dcreate2_checked(file_id, "/gene_name", varstring_type,
                               dataspace, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
        for (TranscriptSet::iterator t = transcripts.begin();
                t != transcripts.end(); ++t) {
            string_data[t->id] = t->gene_name.get().c_str();
        }
        H5Dwrite_checked(gene_name_dataset, varstring_type,
                         H5S_ALL, H5S_ALL, H5P_DEFAULT, string_data);
        H5Dclose(gene_name_dataset);

        H5Tclose(varstring_type);

        delete [] string_data;

        // tgroup table
        hid_t tgroup_dataset =
        H5Dcreate2_checked(file_id, "/tgroup", H5T_NATIVE_UINT,
                            dataspace, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);

        unsigned int* tgroup_data = new unsigned int[N];

        for (TranscriptSet::iterator t = transcripts.begin(); t != transcripts.end(); ++t) {
            tgroup_data[t->id] = t->tgroup;
        }

        H5Dwrite_checked(tgroup_dataset, H5T_NATIVE_UINT, H5S_ALL, H5S_ALL,
                         H5S_ALL, tgroup_data);

        H5Dclose(tgroup_dataset);
        delete [] tgroup_data;
    }

    // sample quantification
    // ---------------------
    {
        hsize_t dims[3] = {num_samples, K, N};
        hsize_t chunk_dims[3] = {1, 1, N};

        hid_t dataset_create_property = H5Pcreate(H5P_DATASET_CREATE);
        H5Pset_layout(dataset_create_property, H5D_CHUNKED);
        H5Pset_chunk(dataset_create_property, 3, chunk_dims);
        H5Pset_deflate(dataset_create_property, 7);

        h5_sample_quant_dataspace = H5Screate_simple(3, dims, NULL);

        h5_sample_quant_dataset =
            H5Dcreate2_checked(file_id, "/transcript_quantification", H5T_NATIVE_FLOAT,
                               h5_sample_quant_dataspace, H5P_DEFAULT,
                               dataset_create_property, H5P_DEFAULT);

        H5Pclose(dataset_create_property);

        hsize_t sample_quant_mem_dims[2] = {K, N};
        h5_sample_quant_mem_dataspace =
            H5Screate_simple(2, sample_quant_mem_dims, NULL);

        hsize_t sample_quant_start[2] = {0, 0};
        H5Sselect_hyperslab_checked(h5_sample_quant_dataspace, H5S_SELECT_SET,
                                    sample_quant_start, NULL,
                                    sample_quant_mem_dims, NULL);
    }

    // sample scaling factors
    // ----------------------
    {
        hsize_t dims[2] = {num_samples, K};
        hsize_t chunk_dims[2] = {1, K};

        hid_t dataset_create_property = H5Pcreate(H5P_DATASET_CREATE);
        H5Pset_layout(dataset_create_property, H5D_CHUNKED);
        H5Pset_chunk(dataset_create_property, 2, chunk_dims);
        H5Pset_deflate(dataset_create_property, 7);

        h5_sample_scaling_dataspace = H5Screate_simple(2, dims, NULL);
        h5_sample_scaling_dataset =
            H5Dcreate2_checked(file_id, "/sample_scaling", H5T_NATIVE_FLOAT,
                               h5_sample_scaling_dataspace, H5P_DEFAULT,
                               dataset_create_property, H5P_DEFAULT);

        H5Pclose(dataset_create_property);

        hsize_t sample_scaling_mem_dims[1] = {K};
        h5_sample_scaling_mem_dataspace =
            H5Screate_simple(1, sample_scaling_mem_dims, NULL);

        hsize_t sample_scaling_mem_start[1] = {0};
        H5Sselect_hyperslab_checked(h5_sample_scaling_dataspace, H5S_SELECT_SET,
                                    sample_scaling_mem_start, NULL,
                                    sample_scaling_mem_dims, NULL);
    }

    // experiment parameters
    // ---------------------
    {
        if (H5Gcreate1(file_id, "/experiment", 0) < 0) {
            Logger::abort("HDF5 group creation failed.");
        }

        hsize_t dims[2] = {num_samples, N};
        hsize_t chunk_dims[2] = {1, N};

        hid_t dataset_create_property = H5Pcreate(H5P_DATASET_CREATE);
        H5Pset_layout(dataset_create_property, H5D_CHUNKED);
        H5Pset_chunk(dataset_create_property, 2, chunk_dims);
        H5Pset_deflate(dataset_create_property, 7);

        h5_experiment_mean_dataspace = H5Screate_simple(2, dims, NULL);

        h5_experiment_mean_dataset =
            H5Dcreate2_checked(file_id, "/experiment/mean", H5T_NATIVE_FLOAT,
                               h5_experiment_mean_dataspace, H5P_DEFAULT,
                               dataset_create_property, H5P_DEFAULT);

        // splicing parameters
        chunk_dims[1] = spliced_tgroup_indexes.size();
        if (!spliced_tgroup_indexes.empty()) {
            H5Pset_chunk(dataset_create_property, 2, chunk_dims);
        }

        h5_splice_param_type = H5Tvlen_create(H5T_NATIVE_FLOAT);
        if (h5_splice_param_type < 0) {
            Logger::abort("HDF5 type creation failed.");
        }

        dims[1] = spliced_tgroup_indexes.size();
        h5_experiment_splice_dataspace = H5Screate_simple(2, dims, NULL);
        h5_splicing_mem_dataspace = H5Screate_simple(1, &dims[1], NULL);

        h5_experiment_splice_mu_dataset =
            H5Dcreate2_checked(file_id, "/experiment/splice_mu", h5_splice_param_type,
                               h5_experiment_splice_dataspace, H5P_DEFAULT,
                               dataset_create_property, H5P_DEFAULT);

        h5_experiment_splice_sigma_dataset =
            H5Dcreate2_checked(file_id, "/experiment/splice_sigma", h5_splice_param_type,
                               h5_experiment_splice_dataspace, H5P_DEFAULT,
                               dataset_create_property, H5P_DEFAULT);

        H5Pclose(dataset_create_property);
    }

    // condition parameters
    // --------------------
    {
        if (H5Gcreate1(file_id, "/condition", 0) < 0) {
            Logger::abort("HDF5 group creation failed.");
        }

        hsize_t dims[3] = {num_samples, C, N};
        hsize_t chunk_dims[3] = {1, 1, N};

        hid_t dataset_create_property = H5Pcreate(H5P_DATASET_CREATE);
        H5Pset_layout(dataset_create_property, H5D_CHUNKED);
        H5Pset_chunk(dataset_create_property, 3, chunk_dims);
        H5Pset_deflate(dataset_create_property, 7);

        h5_condition_mean_dataspace = H5Screate_simple(3, dims, NULL);

        h5_condition_mean_dataset =
            H5Dcreate2_checked(file_id, "/condition/mean", H5T_NATIVE_FLOAT,
                               h5_condition_mean_dataspace, H5P_DEFAULT,
                               dataset_create_property, H5P_DEFAULT);

        hsize_t condition_mean_dims[2] = {C, N};
        h5_condition_mean_mem_dataspace = H5Screate_simple(2, condition_mean_dims, NULL);

        hsize_t shape_chunk_dims[2] = {1, N};
        H5Pset_chunk(dataset_create_property, 2, shape_chunk_dims);

        h5_condition_shape_dataset =
            H5Dcreate2_checked(file_id, "/condition/shape", H5T_NATIVE_FLOAT,
                               h5_experiment_mean_dataspace, H5P_DEFAULT,
                               dataset_create_property, H5P_DEFAULT);

        // splicing
        chunk_dims[2] = spliced_tgroup_indexes.size();
        if (!spliced_tgroup_indexes.empty()) {
            H5Pset_chunk(dataset_create_property, 3, chunk_dims);
        }

        dims[2] = spliced_tgroup_indexes.size();
        h5_condition_splice_mu_dataspace = H5Screate_simple(3, dims, NULL);

        h5_condition_splice_mu_dataset =
            H5Dcreate2_checked(file_id, "/condition/splice_mu", h5_splice_param_type,
                               h5_condition_splice_mu_dataspace, H5P_DEFAULT,
                               dataset_create_property, H5P_DEFAULT);

        chunk_dims[1] = spliced_tgroup_indexes.size();
        if (!spliced_tgroup_indexes.empty()) {
            H5Pset_chunk(dataset_create_property, 2, chunk_dims);
        }

        dims[1] = spliced_tgroup_indexes.size();
        h5_condition_splice_sigma_dataspace = H5Screate_simple(2, dims, NULL);

        h5_condition_splice_sigma_dataset =
            H5Dcreate2_checked(file_id, "/condition/splice_sigma", h5_splice_param_type,
                               h5_condition_splice_sigma_dataspace, H5P_DEFAULT,
                               dims[1] > 0 ? dataset_create_property : H5P_DEFAULT,
                               H5P_DEFAULT);

        H5Pclose(dataset_create_property);
    }

    hsize_t dims[1] = { N };
    h5_row_mem_dataspace = H5Screate_simple(1, dims, NULL);

    h5_splice_work = new hvl_t[spliced_tgroup_indexes.size()];
    for (size_t i = 0; i < spliced_tgroup_indexes.size(); ++i) {
        size_t num_tids = tgroup_tids[spliced_tgroup_indexes[i]].size();
        h5_splice_work[i].len = num_tids;
        h5_splice_work[i].p = reinterpret_cast<void*>(new float[num_tids]);
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


void Analyze::qsampler_update_hyperparameters()
{
    for (size_t i = 0; i < K; ++i) {
        qsamplers[i]->hp.scale = scale[i];

        size_t c = condition[i];
        for (size_t j = 0; j < N; ++j) {
            qsamplers[i]->hp.mean[j] = condition_mean(c, j);
            qsamplers[i]->hp.shape[j] = condition_shape[j];
        }

        std::fill(qsamplers[i]->hp.splice_mu.begin(),
                  qsamplers[i]->hp.splice_mu.end(), 0.0);

        std::fill(qsamplers[i]->hp.splice_sigma.begin(),
                  qsamplers[i]->hp.splice_sigma.end(), 0.1);

        for (size_t j = 0; j < spliced_tgroup_indexes.size(); ++j) {
            unsigned int tgroup = spliced_tgroup_indexes[j];
            for (size_t k = 0; k < tgroup_tids[tgroup].size(); ++k) {
                qsamplers[i]->hp.splice_mu[tgroup_tids[tgroup][k]] =
                    condition_splice_mu[c][j][k];
                qsamplers[i]->hp.splice_sigma[tgroup_tids[tgroup][k]] =
                    condition_splice_sigma[j][k];
            }
        }
    }
}


void Analyze::run(hid_t output_file_id, bool dryrun)
{
    C = condition_index.size();
    Q.resize(K, N);
    scale.resize(K, 1.0);
    condition_mean.resize(C, N);
    condition_shape.resize(N);
    experiment_mean.resize(N);

    condition_splice_mu.resize(C);
    for (size_t i = 0; i < C; ++i) {
        condition_splice_mu[i].resize(spliced_tgroup_indexes.size());
        for (size_t j = 0; j < spliced_tgroup_indexes.size(); ++j) {
            condition_splice_mu[i][j].resize(
                tgroup_tids[spliced_tgroup_indexes[j]].size());
            std::fill(condition_splice_mu[i][j].begin(),
                      condition_splice_mu[i][j].end(), 0.0);
        }
    }

    condition_splice_sigma.resize(spliced_tgroup_indexes.size());
    condition_splice_eta.resize(spliced_tgroup_indexes.size());
    size_t flattened_sigma_size = 0;
    for (size_t j = 0; j < spliced_tgroup_indexes.size(); ++j) {
        condition_splice_sigma[j].resize(
            tgroup_tids[spliced_tgroup_indexes[j]].size());
        std::fill(condition_splice_sigma[j].begin(),
                  condition_splice_sigma[j].end(), 0.1);
        condition_splice_eta[j].resize(
            tgroup_tids[spliced_tgroup_indexes[j]].size());
        std::fill(condition_splice_eta[j].begin(),
                  condition_splice_eta[j].end(), 1.0);
        flattened_sigma_size += condition_splice_sigma[j].size();
    }

    condition_splice_sigma_work.resize(flattened_sigma_size);
    experiment_splice_sigma_work.resize(C * flattened_sigma_size);

    experiment_splice_mu.resize(spliced_tgroup_indexes.size());
    for (size_t i = 0; i < spliced_tgroup_indexes.size(); ++i) {
        experiment_splice_mu[i].resize(
                tgroup_tids[spliced_tgroup_indexes[i]].size());
        std::fill(experiment_splice_mu[i].begin(),
                  experiment_splice_mu[i].end(), 0.0);
    }

    choose_initial_values();

    setup_samplers();

    if (dryrun) {
        return;
    }

    setup_output(output_file_id);

    BOOST_FOREACH (Sampler* qsampler, qsamplers) {
        qsampler->start();
    }

    unsigned long total_frag_count = 0;
    BOOST_FOREACH (Sampler* qsampler, qsamplers) {
        total_frag_count += qsampler->num_frags();
    }
    Logger::info("Estimating expression of %lu trancripts in %lu samples with %lu fragments.",
                 N, K, total_frag_count);

    qsampler_threads.resize(constants::num_threads);
    BOOST_FOREACH (SamplerTickThread*& thread, qsampler_threads) {
        thread = new SamplerTickThread(qsamplers, Q, qsampler_tick_queue,
                                       qsampler_notify_queue);
        thread->start();
    }

    meanshape_sampler_threads.resize(constants::num_threads);
    BOOST_FOREACH (ConditionMeanShapeSamplerThread*& thread, meanshape_sampler_threads) {
        thread = new ConditionMeanShapeSamplerThread(
                Q, condition_mean, condition_shape,
                experiment_mean, experiment_shape,
                condition_shape_alpha, condition_shape_beta,
                condition, condition_samples,
                meanshape_sampler_tick_queue,
                meanshape_sampler_notify_queue,
                transcript_rng_pool);
        thread->start();
    }

    experiment_meanshape_sampler_threads.resize(constants::num_threads);
    BOOST_FOREACH (ExperimentMeanShapeSamplerThread*& thread,
                   experiment_meanshape_sampler_threads) {
        thread = new ExperimentMeanShapeSamplerThread(
            experiment_mean, experiment_shape,
            experiment_mean0, experiment_shape0,
            condition_mean, experiment_meanshape_sampler_tick_queue,
            experiment_meanshape_sampler_notify_queue, transcript_rng_pool);

        thread->start();
    }

    splice_mu_sigma_sampler_threads.resize(constants::num_threads);
    BOOST_FOREACH (ConditionSpliceMuSigmaEtaSamplerThread*& thread, splice_mu_sigma_sampler_threads) {
        thread = new ConditionSpliceMuSigmaEtaSamplerThread(
                condition_splice_mu, condition_splice_sigma, condition_splice_eta,
                experiment_splice_mu, experiment_splice_sigma, experiment_splice_nu,
                condition_splice_alpha, condition_splice_beta, Q,
                spliced_tgroup_indexes,
                tgroup_tids,
                condition,
                condition_samples,
                splice_mu_sigma_sampler_tick_queue,
                splice_mu_sigma_sampler_notify_queue,
                splice_rng_pool);
        thread->start();
    }

    experiment_splice_mu_sigma_sampler_threads.resize(constants::num_threads);
    BOOST_FOREACH (ExperimentSpliceMuSigmaSamplerThread*& thread,
                   experiment_splice_mu_sigma_sampler_threads) {
        thread = new ExperimentSpliceMuSigmaSamplerThread(
                experiment_splice_mu,
                experiment_splice_sigma,
                experiment_splice_nu,
                condition_splice_mu,
                spliced_tgroup_indexes,
                tgroup_tids,
                experiment_splice_mu0,
                experiment_splice_sigma0,
                experiment_splice_mu_sigma_sampler_tick_queue,
                experiment_splice_mu_sigma_sampler_notify_queue,
                splice_rng_pool);
        thread->start();
    }

    const char* optimize_task_name = "Optimizing";
    Logger::push_task(optimize_task_name, constants::num_opt_rounds);

    for (size_t i = 0; i < constants::num_opt_rounds; ++i) {
        sample(true);
        Logger::get_task(optimize_task_name).inc();
    }

    if (!nopriors) {
        BOOST_FOREACH (Sampler* sampler, qsamplers) {
            sampler->engage_priors();
        }
    }

    // write the maximum posterior state as sample 0
    write_output(0);
    Logger::pop_task(optimize_task_name);

    BOOST_FOREACH (ConditionSpliceMuSigmaEtaSamplerThread* thread, splice_mu_sigma_sampler_threads) {
        thread->end_burnin();
    }

    BOOST_FOREACH (ExperimentSpliceMuSigmaSamplerThread* thread,
                   experiment_splice_mu_sigma_sampler_threads) {
        thread->end_burnin();
    }

    BOOST_FOREACH (ConditionMeanShapeSamplerThread* thread,
                   meanshape_sampler_threads) {
        thread->end_burnin();
    }

    const char* sample_task_name = "Sampling";
    Logger::push_task(sample_task_name, num_samples + burnin);

    for (size_t i = 0; i < burnin; ++i) {
        sample(false);
        Logger::get_task(sample_task_name).inc();
    }

    for (size_t i = 1; i < num_samples; ++i) {
        sample(false);
        write_output(i);
        Logger::get_task(sample_task_name).inc();
    }

    for (size_t i = 0; i < constants::num_threads; ++i) {
        qsampler_tick_queue.push(-1);
        meanshape_sampler_tick_queue.push(IdxRange(-1, -1));
        experiment_meanshape_sampler_tick_queue.push(IdxRange(-1, -1));
        splice_mu_sigma_sampler_tick_queue.push(IdxRange(-1, -1));
        experiment_splice_mu_sigma_sampler_tick_queue.push(IdxRange(-1, -1));
    }

    for (size_t i = 0; i < constants::num_threads; ++i) {
        qsampler_threads[i]->join();
        meanshape_sampler_threads[i]->join();
        experiment_meanshape_sampler_threads[i]->join();
        splice_mu_sigma_sampler_threads[i]->join();
        experiment_splice_mu_sigma_sampler_threads[i]->join();
    }

    BOOST_FOREACH (Sampler* qsampler, qsamplers) {
        qsampler->stop();
    }

    for (size_t i = 0; i < constants::num_threads; ++i) {
        delete qsampler_threads[i];
        delete meanshape_sampler_threads[i];
        delete experiment_meanshape_sampler_threads[i];
        delete splice_mu_sigma_sampler_threads[i];
        delete experiment_splice_mu_sigma_sampler_threads[i];
    }

    for (size_t i = 0; i < spliced_tgroup_indexes.size(); ++i) {
        delete [] reinterpret_cast<float*>(h5_splice_work[i].p);
    }
    delete [] h5_splice_work;

    H5Dclose(h5_experiment_mean_dataset);
    H5Sclose(h5_experiment_mean_dataspace);
    H5Dclose(h5_condition_mean_dataset);
    H5Sclose(h5_condition_mean_dataspace);
    H5Sclose(h5_condition_mean_mem_dataspace);
    H5Dclose(h5_sample_quant_dataset);
    H5Sclose(h5_sample_quant_dataspace);
    H5Sclose(h5_sample_quant_mem_dataspace);
    H5Dclose(h5_experiment_splice_mu_dataset);
    H5Dclose(h5_condition_splice_mu_dataset);
    H5Dclose(h5_condition_splice_sigma_dataset);
    H5Sclose(h5_row_mem_dataspace);
    H5Sclose(h5_experiment_splice_dataspace);
    H5Sclose(h5_condition_splice_mu_dataspace);
    H5Sclose(h5_condition_splice_sigma_dataspace);
    H5Sclose(h5_splicing_mem_dataspace);
    H5Tclose(h5_splice_param_type);
    H5Dclose(h5_sample_scaling_dataset);
    H5Sclose(h5_sample_scaling_dataspace);
    H5Sclose(h5_sample_scaling_mem_dataspace);

    Logger::pop_task(sample_task_name);
}


void Analyze::sample(bool optimize_state)
{
    qsampler_update_hyperparameters();

    for (size_t i = 0; i < constants::num_threads; ++i) {
        qsampler_threads[i]->set_optimize_state(optimize_state);
    }

    for (size_t i = 0; i < K; ++i) {
        qsampler_tick_queue.push(i);
    }

    // sampling these parameters can't be done in parallel, so we take this
    // oppourtunity

    condition_shape_beta =
        gamma_beta_sampler->sample(
                rng, condition_shape_beta, condition_shape_alpha,
                condition_shape_beta_a, condition_shape_beta_b,
                &condition_shape.at(0), N);
    assert_finite(condition_shape_beta);

    for (size_t i = 0, j = 0; j < condition_splice_sigma.size(); ++j) {
        for (size_t k = 0; k < condition_splice_sigma[j].size(); ++k) {
            condition_splice_sigma_work[i++] = condition_splice_sigma[j][k];
        }
    }

    condition_splice_beta =
        gamma_beta_sampler->sample(
                rng, condition_splice_beta, condition_splice_alpha,
                condition_splice_beta_a, condition_splice_beta_b,
                condition_splice_sigma_work.empty() ? NULL : &condition_splice_sigma_work.at(0),
                condition_splice_sigma_work.size());
    assert_finite(condition_splice_beta);

    for (size_t i = 0, c = 0; c < C; ++c) {
        for (size_t j = 0; j < experiment_splice_mu.size(); ++j) {
            for (size_t k = 0; k < experiment_splice_mu[j].size(); ++k) {
                experiment_splice_sigma_work[i++] =
                    condition_splice_mu[c][j][k] - experiment_splice_mu[j][k];
            }
        }
    }

    experiment_splice_sigma = gamma_normal_sigma_sampler->sample(
            rng, experiment_splice_sigma,
            experiment_splice_sigma_work.empty() ? NULL : &experiment_splice_sigma_work.at(0),
            experiment_splice_sigma_work.size(),
            experiment_splice_sigma_alpha, experiment_splice_sigma_beta);


    // TODO: sampling experiment_tgroup_shape is currently to slow,
    // and not important enough to bother with.

    // for (size_t i = 0, c = 0; c < C; ++c) {
    //     for (size_t j = 0; j < T; ++j) {
    //         experiment_tgroup_shape_work[i++] = experiment_tgroup_mean[j];
    //     }
    // }

    // experiment_tgroup_shape = gamma_shape_sampler->sample(
    //     rng, experiment_tgroup_shape_work.empty() ? NULL : &experiment_tgroup_shape_work.at(0),
    //     experiment_tgroup_shape, &condition_tgroup_mean.data()[0],
    //     condition_tgroup_mean.data().size(),
    //     experiment_tgroup_sigma_alpha, experiment_tgroup_sigma_beta);

    experiment_shape = constants::analyze_experiment_shape;

    for (size_t i = 0; i < K; ++i) {
        qsampler_notify_queue.pop();
    }

    compute_scaling();

    // size of units of work queued for threads
    const size_t block_size = 250;

    // sample condition-level parameters
    for (size_t i = 0; i < N; i += block_size) {
        meanshape_sampler_tick_queue.push(
                IdxRange(i, std::min<int>(N, i + block_size)));
    }

    for (size_t i = 0; i < spliced_tgroup_indexes.size(); i += block_size) {
        splice_mu_sigma_sampler_tick_queue.push(
                IdxRange(i, std::min<int>(spliced_tgroup_indexes.size(), i + block_size)));
    }

    for (size_t i = 0; i < N; i += block_size) {
        meanshape_sampler_notify_queue.pop();
    }

    for (size_t i = 0; i < spliced_tgroup_indexes.size(); i += block_size) {
        splice_mu_sigma_sampler_notify_queue.pop();
    }

    // sample experiment-level parameters
    for (size_t i = 0; i < N; i += block_size) {
        experiment_meanshape_sampler_tick_queue.push(
                IdxRange(i, std::min<int>(N, i + block_size)));
    }

    for (size_t i = 0; i < spliced_tgroup_indexes.size(); i += block_size) {
        experiment_splice_mu_sigma_sampler_tick_queue.push(
                IdxRange(i, std::min<int>(spliced_tgroup_indexes.size(), i + block_size)));
    }

    for (size_t i = 0; i < N; i += block_size) {
        experiment_meanshape_sampler_notify_queue.pop();
    }

    for (size_t i = 0; i < spliced_tgroup_indexes.size(); i += block_size) {
        experiment_splice_mu_sigma_sampler_notify_queue.pop();
    }
}


void Analyze::write_output(size_t sample_num)
{
    hsize_t file_start2[2] = {sample_num, 0};
    hsize_t file_count2[2] = {1, N};

    H5Sselect_hyperslab_checked(h5_experiment_mean_dataspace, H5S_SELECT_SET,
                                file_start2, NULL, file_count2, NULL);
    H5Dwrite_checked(h5_experiment_mean_dataset, H5T_NATIVE_FLOAT,
                     h5_row_mem_dataspace, h5_experiment_mean_dataspace,
                     H5P_DEFAULT, &experiment_mean.at(0));

    hsize_t file_start3[3] = {sample_num, 0, 0};
    hsize_t file_count3[3] = {1, C, N};

    H5Sselect_hyperslab_checked(h5_condition_mean_dataspace, H5S_SELECT_SET,
                                file_start3, NULL, file_count3, NULL);
    H5Dwrite_checked(h5_condition_mean_dataset, H5T_NATIVE_FLOAT,
                     h5_condition_mean_mem_dataspace, h5_condition_mean_dataspace,
                     H5P_DEFAULT, &condition_mean.data()[0]);

    hsize_t sample_quant_start[3] = {sample_num, 0, 0};
    hsize_t sample_quant_count[3] = {1, K, N};

    H5Sselect_hyperslab_checked(h5_sample_quant_dataspace, H5S_SELECT_SET,
                                sample_quant_start, NULL,
                                sample_quant_count, NULL);

    H5Dwrite_checked(h5_sample_quant_dataset, H5T_NATIVE_FLOAT,
                     h5_sample_quant_mem_dataspace, h5_sample_quant_dataspace,
                     H5P_DEFAULT, &Q.data()[0]);

    // write sample scaling factors
    hsize_t sample_scaling_start[2] = {sample_num, 0};
    hsize_t sample_scaling_count[2] = {1, K};
    H5Sselect_hyperslab_checked(h5_sample_scaling_dataspace, H5S_SELECT_SET,
                                sample_scaling_start, NULL,
                                sample_scaling_count, NULL);
    H5Dwrite_checked(h5_sample_scaling_dataset, H5T_NATIVE_FLOAT,
                     h5_sample_scaling_mem_dataspace, h5_sample_scaling_dataspace,
                     H5P_DEFAULT, &scale.at(0));

    // write experiment and condition splicing parameters
    hsize_t experiment_splicing_start[2] = {sample_num, 0};
    hsize_t experiment_splicing_count[2] = {1, spliced_tgroup_indexes.size()};

    H5Sselect_hyperslab_checked(h5_experiment_splice_dataspace,
                                H5S_SELECT_SET,
                                experiment_splicing_start, NULL,
                                experiment_splicing_count, NULL);

    for (size_t i = 0; i < spliced_tgroup_indexes.size(); ++i) {
        float* xs = reinterpret_cast<float*>(h5_splice_work[i].p);
        for (size_t j = 0; j < h5_splice_work[i].len; ++j) {
            xs[j] = experiment_splice_mu[i][j];
        }
    }

    H5Dwrite_checked(h5_experiment_splice_mu_dataset, h5_splice_param_type,
                     h5_splicing_mem_dataspace, h5_experiment_splice_dataspace,
                     H5P_DEFAULT, h5_splice_work);

    hsize_t condition_splice_mu_start[3] = {sample_num, 0, 0};
    hsize_t condition_splice_mu_count[3] = {1, 1, spliced_tgroup_indexes.size()};

    for (size_t i = 0; i < C; ++i) {
        for (size_t j = 0; j < spliced_tgroup_indexes.size(); ++j) {
            float* xs = reinterpret_cast<float*>(h5_splice_work[j].p);
            for (size_t k = 0; k < h5_splice_work[j].len; ++k) {
                xs[k] = condition_splice_mu[i][j][k];
            }
        }

        condition_splice_mu_start[1] = i;
        H5Sselect_hyperslab_checked(h5_condition_splice_mu_dataspace,
                                    H5S_SELECT_SET,
                                    condition_splice_mu_start, NULL,
                                    condition_splice_mu_count, NULL);

        H5Dwrite_checked(h5_condition_splice_mu_dataset, h5_splice_param_type,
                         h5_splicing_mem_dataspace, h5_condition_splice_mu_dataspace,
                         H5P_DEFAULT, h5_splice_work);
    }

    hsize_t condition_splice_sigma_start[2] = {sample_num, 0};
    hsize_t condition_splice_sigma_count[2] = {1, spliced_tgroup_indexes.size()};

    for (size_t j = 0; j < spliced_tgroup_indexes.size(); ++j) {
        float* xs = reinterpret_cast<float*>(h5_splice_work[j].p);
        for (size_t k = 0; k < h5_splice_work[j].len; ++k) {
            xs[k] = condition_splice_sigma[j][k];
        }
    }

    H5Sselect_hyperslab_checked(h5_condition_splice_sigma_dataspace,
                                H5S_SELECT_SET,
                                condition_splice_sigma_start, NULL,
                                condition_splice_sigma_count, NULL);

    H5Dwrite_checked(h5_condition_splice_sigma_dataset, h5_splice_param_type,
                     h5_splicing_mem_dataspace, h5_condition_splice_sigma_dataspace,
                     H5P_DEFAULT, h5_splice_work);
}


void Analyze::compute_scaling()
{
    size_t effective_size =
        std::min<size_t>(N, constants::sample_scaling_truncation);
    size_t normalization_point_idx =
        N - effective_size + constants::sample_scaling_quantile * effective_size;

    for (unsigned int i = 0; i < K; ++i) {
        matrix_row<matrix<float> > row(Q, i);

        // unscale abundance estimates so we can compute a new scale
        // and renormalize. I know this must seem weird.
        BOOST_FOREACH (float& x, row) x /= scale[i];

        // normalize according to an upper quantile
        std::copy(row.begin(), row.end(), scale_work.begin());
        std::sort(scale_work.begin(), scale_work.end());
        scale[i] = scale_work[normalization_point_idx];
    }

    for (int i = (int) K - 1; i >= 0; --i) {
        scale[i] = scale[0] / scale[i];
    }

    for (unsigned int i = 0; i < K; ++i) {
        matrix_row<matrix<float> > row(Q, i);
        BOOST_FOREACH (float& x, row) {
            x *= scale[i];
        }
    }
}


void Analyze::choose_initial_values()
{
    std::fill(experiment_mean.begin(), experiment_mean.end(), constants::zero_eps);
    std::fill(condition_mean.data().begin(), condition_mean.data().end(), constants::zero_eps);
    std::fill(condition_shape.begin(), condition_shape.end(), 1.0);

    experiment_shape = 2.0;
    condition_shape_beta = 1.0;

    experiment_splice_sigma = 0.5;
    condition_splice_beta = 1.0;

    // choose initially flat values for splicing parameters
    for (size_t i = 0; i < C; ++i) {
        for (size_t j = 0; j < spliced_tgroup_indexes.size(); ++j) {
            std::fill(condition_splice_mu[i][j].begin(),
                      condition_splice_mu[i][j].end(), 0.5);
        }
    }

    for (size_t i = 0; i < spliced_tgroup_indexes.size(); ++i) {
        std::fill(condition_splice_sigma[i].begin(),
                  condition_splice_sigma[i].end(), 0.1);
    }

    // initialially flat values for experiment splicing
    for (size_t i = 0; i < spliced_tgroup_indexes.size(); ++i) {
        std::fill(experiment_splice_mu[i].begin(),
                  experiment_splice_mu[i].end(), 0.5);
    }
}


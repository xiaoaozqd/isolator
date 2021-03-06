
#include <cmath>
#include <boost/math/special_functions/digamma.hpp>
#include <boost/math/special_functions/fpclassify.hpp>

#include "constants.hpp"
#include "fastmath.hpp"
#include "logger.hpp"
#include "nlopt/nlopt.h"
#include "shredder.hpp"


double shredder_opt_objective(unsigned int _n, const double* _x,
                              double* _grad, void* data)
{
    UNUSED(_n);

    Shredder* sampler = reinterpret_cast<Shredder*>(data);
    if (_grad) {
        double fx = sampler->f(_x[0], _grad[0]);
        _grad[0] = std::max<double>(std::min<double>(_grad[0], 1e4), -1e4);
        return fx;
    }
    else {
        double d;
        return sampler->f(_x[0], d);
    }
}


static void assert_finite(double x)
{
    if (!boost::math::isfinite(x)) {
        Logger::abort("%f found where finite value expected.", x);
    }
}


Shredder::Shredder(double lower_limit, double upper_limit, double tolerance)
    : lower_limit(lower_limit)
    , upper_limit(upper_limit)
    , tolerance(tolerance)
    , opt(NULL)
{
    opt = nlopt_create(NLOPT_LD_SLSQP, 1);
    nlopt_set_lower_bounds(opt, &lower_limit);
    nlopt_set_upper_bounds(opt, &upper_limit);
    nlopt_set_max_objective(opt, shredder_opt_objective,
            reinterpret_cast<void*>(this));
    nlopt_set_maxeval(opt, 20);

    nlopt_set_ftol_abs(opt, 1e-7);
    nlopt_set_xtol_abs(opt, &tolerance);
}


Shredder::~Shredder()
{
    nlopt_destroy(opt);
}


void Shredder::set_tolerance(double tolerance)
{
    this->tolerance = tolerance;
    nlopt_set_xtol_abs(opt, &this->tolerance);
}


double Shredder::sample(rng_t& rng, double x0)
{
    double d0;
    double lp0 = f(x0, d0);

    assert_finite(lp0);

    double slice_height = fastlog(
            std::max<double>(constants::zero_eps, random_uniform_01(rng))) + lp0;
    assert_finite(slice_height);

    x_min = find_slice_edge(x0, slice_height, lp0, d0, -1);
    x_max = find_slice_edge(x0, slice_height, lp0, d0,  1);

    double x = (x_max + x_min) / 2;
    while (x_max - x_min > tolerance) {
        x = x_min + (x_max - x_min) * random_uniform_01(rng);
        double d;
        double lp = f(x, d);

        if (lp >= slice_height) break;
        else if (x > x0) x_max = x;
        else             x_min = x;
    }

    return x;
}


double Shredder::optimize(double x0)
{
    // bounds may have changed
    nlopt_set_lower_bounds(opt, &lower_limit);
    nlopt_set_upper_bounds(opt, &upper_limit);

    x0 = std::max<double>(std::min<double>(x0, upper_limit), lower_limit);
    //x0 = (lower_limit + upper_limit) / 2;
    double maxf;
    nlopt_result result = nlopt_optimize(opt, &x0, &maxf);

    if (result < 0 && result != NLOPT_ROUNDOFF_LIMITED) {
        Logger::warn("Optimization failed with code %d", (int) result);
    }

    return std::max<double>(std::min<double>(x0, upper_limit), lower_limit);
}


double Shredder::find_slice_edge(double x0, double slice_height,
                                 double lp0, double d0, int direction)
{
    const double lp_eps = 1e-2;
    const double d_eps  = 1e-3;

    // if newton method iterations are not making progress, resort to bisection
    size_t newton_count = 0;

    double lp = lp0 - slice_height;
    double d = d0;
    double x = x0;
    double x_bound_lower, x_bound_upper;

    if (direction < 0) {
        x_bound_lower = lower_limit;
        x_bound_upper = x0;
        double fx = f(lower_limit, d);
        if (boost::math::isfinite(fx) && fx >= slice_height) {
            return lower_limit;
        }
    }
    else {
        x_bound_lower = x0;
        x_bound_upper = upper_limit;
        double fx = f(upper_limit, d);
        if (boost::math::isfinite(fx) && fx >= slice_height) {
            return upper_limit;
        }
    }

    while (fabs(lp) > lp_eps && fabs(x_bound_upper - x_bound_lower) > tolerance) {
        double x1 = x - lp / d;
        if (isnan(d) || d == 0.0 || fabs(d) < d_eps || !boost::math::isfinite(x1)) {
            x1 = (x_bound_lower + x_bound_upper) / 2;
        }

        // if we are very close to the boundry, and this iteration moves us past
        // the boundry, just give up.
        if (direction < 0 && fabs(x - lower_limit) <= tolerance && (x1 < x || lp > 0.0)) break;
        if (direction > 0 && fabs(x - upper_limit) <= tolerance && (x1 > x || lp > 0.0)) break;

        // if we are moving in the wrong direction (i.e. toward the other root),
        // use bisection to correct course.
        if (direction < 0) {
            if (lp > 0) x_bound_upper = x;
            else        x_bound_lower = x;
        }
        else {
            if (lp > 0) x_bound_lower = x;
            else        x_bound_upper = x;
        }

        bool bisect = newton_count >= constants::max_newton_steps ||
            x1 < x_bound_lower + tolerance || x1 > x_bound_upper - tolerance;

        // try using the gradient
        if (!bisect) {
            x = x1;
            lp = f(x, d) - slice_height;
            bisect = !boost::math::isfinite(lp) || !boost::math::isfinite(d);
        }

        // resort to binary search if we seem not to be making progress
        if (bisect) {
            size_t iteration_count = 0;
            while (true) {
                x = (x_bound_lower + x_bound_upper) / 2;
                lp = f(x, d) - slice_height;

                //if (boost::math::isinf(lp) || boost::math::isinf(d)) {
                if (!boost::math::isfinite(lp)) {
                    if (direction < 0) x_bound_lower = x;
                    else               x_bound_upper = x;
                }
                else break;

                if (++iteration_count > 50) {
                    Logger::abort("Slice sampler edge finding is not making progress.");
                }
            }
    }
    else ++newton_count;

        assert_finite(lp);
    }

    assert_finite(x);

    return x;
}


static double sq(double x)
{
    return x * x;
}


static double cb(double x)
{
    return x * x * x;
}

static double lbeta(double x, double y)
{
    return lgamma(x) + lgamma(y) - lgamma(x + y);
}


#if 0
double PoissonLogPdf::f(float lambda, unsigned int k)
{
    return k * fastlog(lambda) - lgammaf(k + 1) - lambda;
}


double PoissonLogPdf::df_dlambda(float lambda, unsigned int k)
{
    return (float) k / lambda - 1;
}
#endif


static const double NEG_LOG_2_PI_DIV_2 = -log(2 * M_PI)/2;

double NormalLogPdf::f(double mu, double sigma, const double* xs, size_t n)
{
    double part1 = n * (NEG_LOG_2_PI_DIV_2 - fastlog(sigma));
    double part2 = 0.0;
    for (size_t i = 0; i < n; ++i) {
        part2 += sq(xs[i] - mu) / (2 * sq(sigma));
    }

    return part1 - part2;
}


float NormalLogPdf::f(float mu, float sigma, const float* xs, size_t n)
{
    float part1 = n * (NEG_LOG_2_PI_DIV_2 - fastlog(sigma));
    float part2 = 0.0;
    for (size_t i = 0; i < n; ++i) {
        part2 += sq(xs[i] - mu) / (2 * sq(sigma));
    }

    return part1 - part2;
}


double NormalLogPdf::f(double mu, double sigma, const double x)
{
    double part1 = NEG_LOG_2_PI_DIV_2 - fastlog(sigma);
    double part2 = sq(x - mu) / (2 * sq(sigma));
    return part1 - part2;
}


double NormalLogPdf::df_dx(double mu, double sigma, const double x)
{
    return (mu - x) / sq(sigma);
}


double NormalLogPdf::df_dx(double mu, double sigma, const double* xs, size_t n)
{
    double part = 0.0;
    for (size_t i = 0; i < n; ++i) {
        part += mu - xs[i];
    }
    return part / sq(sigma);
}


double NormalLogPdf::df_dmu(double mu, double sigma, const double* xs, size_t n)
{
    double part = 0.0;
    for (size_t i = 0; i < n; ++i) {
        part += xs[i] - mu;
    }
    return part / sq(sigma);
}


float NormalLogPdf::df_dmu(float mu, float sigma, const float* xs, size_t n)
{
    float part = 0.0;
    for (size_t i = 0; i < n; ++i) {
        part += xs[i] - mu;
    }
    return part / sq(sigma);
}


double NormalLogPdf::df_dsigma(double mu, double sigma, const double* xs, size_t n)
{
    double part = 0.0;
    for (size_t i = 0; i < n; ++i) {
        part += sq(xs[i] - mu);
    }

    return part / cb(sigma) - n/sigma;
}


float NormalLogPdf::df_dsigma(float mu, float sigma, const float* xs, size_t n)
{
    float part = 0.0;
    for (size_t i = 0; i < n; ++i) {
        part += sq(xs[i] - mu);
    }

    return part / cb(sigma) - n/sigma;
}


double LogNormalLogPdf::f(double mu, double sigma, const double* xs, size_t n)
{
    double part1 = n * (NEG_LOG_2_PI_DIV_2 - fastlog(sigma));
    double part2 = 0.0;
    for (size_t i = 0; i < n; ++i) {
        double logx = fastlog(xs[i]);
        part2 += sq(logx - mu) / (2 * sq(sigma)) + logx;
    }

    return part1 - part2;
}


double LogNormalLogPdf::df_dx(double mu, double sigma, double x)
{
    return (mu - fastlog(x)) / (x * sq(sigma)) - 1.0 / x;
}


double LogNormalLogPdf::df_dmu(double mu, double sigma, const double* xs, size_t n)
{
    double part = 0.0;
    for (size_t i = 0; i < n; ++i) {
        part += fastlog(xs[i]) - mu;
    }
    return part / sq(sigma);
}


double LogNormalLogPdf::df_dsigma(double mu, double sigma, const double* xs, size_t n)
{
    double part = 0.0;
    for (size_t i = 0; i < n; ++i) {
        part += sq(fastlog(xs[i]) - mu);
    }

    return part / cb(sigma) - n/sigma;
}


#if 0
double StudentsTLogPdf::f(double nu, double mu, double sigma, const double* xs, size_t n)
{
    double part1 =
        n * (lgamma((nu + 1) / 2) - lgamma(nu / 2) - fastlog(sqrt(nu * M_PI) * sigma));

    double part2 = 0.0;
    for (size_t i = 0; i < n; ++i) {
        part2 += log1p(sq((xs[i] - mu) / sigma) / nu);
    }

    return part1 - ((nu + 1) / 2) * part2;
}


float StudentsTLogPdf::f(float nu, float mu, float sigma, const float* xs, size_t n)
{
    float part1 =
        n * (lgammaf((nu + 1) / 2) - lgamma(nu / 2) - fastlog(sqrt(nu * M_PI) * sigma));

    float part2 = 0.0;
    for (size_t i = 0; i < n; ++i) {
        part2 += log1p(sq((xs[i] - mu) / sigma) / nu);
    }

    return part1 - ((nu + 1) / 2) * part2;
}


double StudentsTLogPdf::df_dx(double nu, double mu, double sigma, const double* xs, size_t n)
{
    double part = 0.0;
    for (size_t i = 0; i < n; ++i) {
        part += (2 * (xs[i] - mu) / sq(sigma) / nu) / (1 + sq((xs[i] - mu) / sigma) / nu);
    }

    return -((nu + 1) / 2) * part;
}


double StudentsTLogPdf::df_dmu(double nu, double mu, double sigma, const double* xs, size_t n)
{
    double part = 0.0;
    for (size_t i = 0; i < n; ++i) {
        part += (2 * (xs[i] - mu) / sq(sigma) / nu) / (1 + sq((xs[i] - mu) / sigma) / nu);
    }

    return ((nu + 1) / 2) * part;
}


float StudentsTLogPdf::df_dmu(float nu, float mu, float sigma, const float* xs, size_t n)
{
    float part = 0.0;
    for (size_t i = 0; i < n; ++i) {
        part += (2 * (xs[i] - mu) / sq(sigma) / nu) / (1 + sq((xs[i] - mu) / sigma) / nu);
    }

    return ((nu + 1) / 2) * part;
}


double StudentsTLogPdf::df_dsigma(double nu, double mu, double sigma, const double* xs, size_t n)
{
    double part = 0.0;
    for (size_t i = 0; i < n; ++i) {
        part += (2 * sq((xs[i] - mu) / sigma) / (nu * sigma)) /
                    (1 + sq((xs[i] - mu) / sigma) / nu);
    }

    return ((nu + 1) / 2) * part - n / sigma;
}
#endif


#if 0
double GammaLogPdf::f(double alpha, double beta, const double* xs, size_t n)
{
    double part1 = 0.0, part2 = 0.0;
    for (size_t i = 0; i < n; ++i) {
        part1 += fastlog(xs[i]);
        part2 += xs[i];
    }

    return
        n * (alpha * fastlog(beta) - lgamma(alpha)) +
        (alpha - 1) * part1 -
        beta * part2;
}


float GammaLogPdf::f(float alpha, float beta, const float* xs, size_t n)
{
    float part1 = 0.0, part2 = 0.0;
    for (size_t i = 0; i < n; ++i) {
        part1 += fastlog(xs[i]);
        part2 += xs[i];
    }

    return
        n * (alpha * fastlog(beta) - lgammaf(alpha)) +
        (alpha - 1) * part1 -
        beta * part2;
}


double GammaLogPdf::df_dx(double alpha, double beta, const double* xs, size_t n)
{
    double part = 0.0;
    for (size_t i = 0; i < n; ++i) {
        part += (alpha - 1) / xs[i];
    }

    return part - n * beta;
}


double GammaLogPdf::df_dalpha(double alpha, double beta, const double* xs, size_t n)
{
    double part = 0.0;
    for (size_t i = 0; i < n; ++i) {
        part += fastlog(xs[i]);
    }

    return n * (fastlog(beta) - boost::math::digamma(alpha)) + part;
}


double GammaLogPdf::df_dbeta(double alpha, double beta, const double* xs, size_t n)
{
    double part = 0.0;
    for (size_t i = 0; i < n; ++i) {
        part += xs[i];
    }

    return (double) n * (alpha / beta) - part;
}


float GammaLogPdf::df_dbeta(float alpha, float beta, const float* xs, size_t n)
{
    float part = 0.0;
    for (size_t i = 0; i < n; ++i) {
        part += xs[i];
    }

    return (float) n * (alpha / beta) - part;
}
#endif


#if 0
double AltGammaLogPdf::f(double mean, double shape, const double* xs, size_t n)
{
    double scale =  mean / shape;
    double part = 0.0;
    for (size_t i = 0; i < n; ++i) {
        part += (shape - 1.0) * fastlog(xs[i]) - xs[i] / scale;
    }

    double ans = -(double) n * (lgamma(shape) + shape * fastlog(scale)) + part;
    assert_finite(ans);
    return ans;
}


float AltGammaLogPdf::f(float mean, float shape, const float* xs, size_t n)
{
    float scale =  mean / shape;
    float part = 0.0;
    for (size_t i = 0; i < n; ++i) {
        part += (shape - 1.0) * fastlog(xs[i]) - xs[i] / scale;
    }

    float ans = -(float) n * (lgammaf(shape) + shape * fastlog(scale)) + part;
    assert_finite(ans);
    return ans;
}


double AltGammaLogPdf::f(double mean, double shape, double x)
{
    double scale =  mean / shape;
    double part = (shape - 1.0) * fastlog(x) - x / scale;
    return -(lgamma(shape) + shape * fastlog(scale)) + part;
}


float AltGammaLogPdf::f(float mean, float shape, float x)
{
    float scale =  mean / shape;
    float part = (shape - 1.0) * fastlog(x) - x / scale;
    return -(lgammaf(shape) + shape * fastlog(scale)) + part;
}



double AltGammaLogPdf::df_dx(double mean, double shape, const double* xs, size_t n)
{
    double scale = mean / shape;
    double part = 0.0;
    for (size_t i = 0; i < n; ++i) {
        part += (shape - 1.0) / xs[i];
    }

    return part - (double) n / scale;
}


double AltGammaLogPdf::df_dx(double mean, double shape, double x)
{
    double scale = mean / shape;
    double part = (shape - 1.0) / x;
    return part - 1.0 / scale;
}


double AltGammaLogPdf::df_dmean(double mean, double shape, const double* xs, size_t n)
{
    double part = 0.0;
    for (size_t i = 0; i < n; ++i) {
        part += xs[i];
    }
    part *= shape / sq(mean);
    return part - (double) n * shape / mean;
}


float AltGammaLogPdf::df_dmean(float mean, float shape, const float* xs, size_t n)
{
    float part = 0.0;
    for (size_t i = 0; i < n; ++i) {
        part += xs[i];
    }
    part *= shape / sq(mean);
    return part - (float) n * shape / mean;
}


double AltGammaLogPdf::df_dshape(double mean, double shape, const double* xs, size_t n)
{
    double scale = mean / shape;
    double part = 0.0;
    for (size_t i = 0; i < n; ++i) {
        part += fastlog(xs[i]) - xs[i] / mean;
    }

    return (double) n * (-boost::math::digamma(shape) + fastlog(scale) * (mean/sq(shape))) + part;
}


float AltGammaLogPdf::df_dshape(float mean, float shape, const float* xs, size_t n)
{
    float scale = mean / shape;
    float part = 0.0;
    for (size_t i = 0; i < n; ++i) {
        part += fastlog(xs[i]) - xs[i] / mean;
    }

    return (float) n * (-boost::math::digamma(shape) + fastlog(scale) * (mean/sq(shape))) + part;
}


float AltGammaLogPdf::df_dshape(float mean, float shape, float x)
{
    float scale = mean / shape;
    float part = fastlog(x) - x / mean;
    return (-boost::math::digamma(shape) + fastlog(scale) * (mean/sq(shape))) + part;
}
#endif


double InvGammaLogPdf::f(double alpha, double beta, const double* xs, size_t n)
{
    double part = 0.0;
    for (size_t i = 0; i < n; ++i) {
        part += (alpha + 1) * fastlog(xs[i]) + beta / xs[i];
    }

    return n * (alpha * fastlog(beta) - lgamma(alpha)) - part;
}


double InvGammaLogPdf::df_dx(double alpha, double beta, const double* xs, size_t n)
{
    double part = 0.0;
    for (size_t i = 0; i < n; ++i) {
        part += beta / sq(xs[i]) - (alpha + 1) / xs[i];
    }

    return part;
}


double InvGammaLogPdf::df_dalpha(double alpha, double beta, const double* xs, size_t n)
{
    double part = 0.0;
    for (size_t i = 0; i < n; ++i) {
        part += fastlog(xs[i]);
    }

    return n * (fastlog(beta) - boost::math::digamma(alpha)) - part;
}


double InvGammaLogPdf::df_dbeta(double alpha, double beta, const double* xs, size_t n)
{
    double part = 0.0;
    for (size_t i = 0; i < n; ++i) {
        part += 1 / xs[i];
    }

    return n * (alpha / beta) - part;
}


double SqInvGammaLogPdf::f(double alpha, double beta, const double* xs, size_t n)
{
    double part = 0.0;
    for (size_t i = 0; i < n; ++i) {
        double x = xs[i] * xs[i];
        part += (alpha + 1) * fastlog(x) + beta / x;
    }

    return n * (alpha * fastlog(beta) - lgamma(alpha)) - part;
}


double SqInvGammaLogPdf::df_dx(double alpha, double beta, const double* xs, size_t n)
{
    double part = 0.0;
    for (size_t i = 0; i < n; ++i) {
        part += 2 * beta / cb(xs[i]) - (2 * alpha + 2) / xs[i];
    }

    return part;
}


double SqInvGammaLogPdf::df_dalpha(double alpha, double beta, const double* xs, size_t n)
{
    double part = 0.0;
    for (size_t i = 0; i < n; ++i) {
        double x = xs[i] * xs[i];
        part += fastlog(x);
    }

    return n * (fastlog(beta) - boost::math::digamma(alpha)) - part;
}


double SqInvGammaLogPdf::df_dbeta(double alpha, double beta, const double* xs, size_t n)
{
    double part = 0.0;
    for (size_t i = 0; i < n; ++i) {
        double x = xs[i] * xs[i];
        part += 1 / x;
    }

    return n * (alpha / beta) - part;
}


double BetaLogPdf::f(double alpha, double beta, double x)
{
    return (alpha - 1) * fastlog(x) + (beta - 1) * fastlog(1 - x) - lbeta(alpha, beta);
}


double BetaLogPdf::df_dx(double alpha, double beta, double x)
{
    return (alpha - 1) / x - (beta - 1) / (1 - x);
}


double BetaLogPdf::df_dgamma(double gamma, double c, double x)
{
    return c * (fastlog(x / (1 - x)) -
                boost::math::digamma(gamma * c) +
                boost::math::digamma((1 - gamma) * c));
}


double DirichletLogPdf::f(double alpha,
                          const boost::numeric::ublas::matrix<double>* mean,
                          const boost::numeric::ublas::matrix<double>* data,
                          size_t n, size_t m)
{
    double part = 0.0;
    for (size_t i = 0; i < n; ++i) {
        for (size_t j = 0; j < m; ++j) {
            double am = alpha * (*mean)(i, j);
            part += (am - 1) * fastlog((*data)(i, j)) - lgamma(am);
        }
    }

    return n * lgamma(alpha) + part;
}


double DirichletLogPdf::df_dalpha(double alpha,
                                  const boost::numeric::ublas::matrix<double>* mean,
                                  const boost::numeric::ublas::matrix<double>* data,
                                  size_t n, size_t m)
{
    double part = 0.0;
    for (size_t i = 0; i < n; ++i) {
        for (size_t j = 0; j < m; ++j) {
            part += (*mean)(i, j) * (fastlog((*data)(i, j)) -
                                     boost::math::digamma(alpha * (*mean)(i, j)));
        }
    }

    return n * boost::math::digamma(alpha) + part;
}


double LogisticNormalLogPdf::f(double mu, double sigma, double x)
{
    return -fastlog(sigma) - fastlog(sqrt(2*M_PI)) -
           sq(fastlog(x / (1 - x)) - mu) / (2 * sq(sigma)) -
           fastlog(x) - fastlog(1-x);
}


double LogisticNormalLogPdf::df_dx(double mu, double sigma, double x)
{
    double y = fastlog(x / (1 - x));
    return (1/(1-x)) - (1/x) - (mu - y) / (sq(sigma) * (x - 1) * x);
}



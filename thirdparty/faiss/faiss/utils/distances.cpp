/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// -*- c++ -*-

#include <faiss/utils/distances.h>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include "simd/hook.h"

#include <omp.h>

#include <faiss/FaissHook.h>
#include <faiss/impl/AuxIndexStructures.h>
#include <faiss/impl/FaissAssert.h>
#include <faiss/impl/ResultHandler.h>
#include <faiss/utils/utils.h>

#ifndef FINTEGER
#define FINTEGER long
#endif

extern "C" {

/* declare BLAS functions, see http://www.netlib.org/clapack/cblas/ */

int sgemm_(
        const char* transa,
        const char* transb,
        FINTEGER* m,
        FINTEGER* n,
        FINTEGER* k,
        const float* alpha,
        const float* a,
        FINTEGER* lda,
        const float* b,
        FINTEGER* ldb,
        float* beta,
        float* c,
        FINTEGER* ldc);
}

namespace faiss {

/***************************************************************************
 * Matrix/vector ops
 ***************************************************************************/

/* Compute the L2 norm of a set of nx vectors */
void fvec_norms_L2(
        float* __restrict nr,
        const float* __restrict x,
        size_t d,
        size_t nx) {
#pragma omp parallel for
    for (int64_t i = 0; i < nx; i++) {
        nr[i] = sqrtf(fvec_norm_L2sqr(x + i * d, d));
    }
}

void fvec_norms_L2sqr(
        float* __restrict nr,
        const float* __restrict x,
        size_t d,
        size_t nx) {
#pragma omp parallel for
    for (int64_t i = 0; i < nx; i++)
        nr[i] = fvec_norm_L2sqr(x + i * d, d);
}

void fvec_renorm_L2(size_t d, size_t nx, float* __restrict x) {
#pragma omp parallel for
    for (int64_t i = 0; i < nx; i++) {
        float* __restrict xi = x + i * d;

        float nr = fvec_norm_L2sqr(xi, d);

        if (nr > 0) {
            size_t j;
            const float inv_nr = 1.0 / sqrtf(nr);
            for (j = 0; j < d; j++)
                xi[j] *= inv_nr;
        }
    }
}

/***************************************************************************
 * KNN functions
 ***************************************************************************/

namespace {

int parallel_policy_threshold = 65535;

/* Find the nearest neighbors for nx queries in a set of ny vectors */
template <class ResultHandler>
void exhaustive_parallel_on_nx(
        const float* x,
        const float* y,
        size_t d,
        size_t nx,
        size_t ny,
        ResultHandler& res,
        decltype(fvec_inner_product) dis_compute_func,
        const BitsetView bitset) {
    using SingleResultHandler = typename ResultHandler::SingleResultHandler;
#pragma omp parallel
    {
        SingleResultHandler resi(res);
#pragma omp for
        for (int64_t i = 0; i < nx; i++) {
            const float* x_i = x + i * d;
            const float* y_j = y;

            resi.begin(i);
            for (size_t j = 0; j < ny; j++) {
                if (bitset.empty() || !bitset.test(j)) {
                    float ip = dis_compute_func(x_i, y_j, d);
                    resi.add_result(ip, j);
                }
                y_j += d;
            }
            resi.end();
        }
    }
}

template <class ResultHandler>
void exhaustive_parallel_on_ny(
        const float* x,
        const float* y,
        size_t d,
        size_t nx,
        size_t ny,
        ResultHandler& res,
        decltype(fvec_inner_product) dis_compute_func,
        const BitsetView bitset) {
    using SingleResultHandler = typename ResultHandler::SingleResultHandler;
    size_t k = res.k;
    size_t thread_max_num = omp_get_max_threads();

    size_t val = d * sizeof(float) +
            thread_max_num * k * (sizeof(float) + sizeof(int64_t));
    size_t block_x = std::min<size_t>(get_l3_size() / val, nx);
    if (block_x == 0) {
        block_x = 1;
    }

    size_t all_heap_size = block_x * k * thread_max_num;
    ResultHandler* ress = res.clone_n(thread_max_num, block_x);

    for (size_t x_from = 0, x_to; x_from < nx; x_from = x_to) {
        x_to = std::min(nx, x_from + block_x);
        size_t size = x_to - x_from;
        size_t thread_heap_size = size * k;

        // init heap
        for (int t = 0; t < thread_max_num; t++) {
            ress[t].begin_multiple(0, block_x);
        }

#pragma omp parallel for schedule(static)
        for (size_t j = 0; j < ny; j++) {
            int t = omp_get_thread_num();
            if (bitset.empty() || !bitset.test(j)) {
                const float* y_j = y + j * d;
                const float* x_i = x + x_from * d;
                for (size_t i = 0; i < size; i++) {
                    float ip = dis_compute_func(x_i, y_j, d);
                    ress[t].add_single_result(i, ip, j);
                    x_i += d;
                }
            }
        }

        // merge heap
        // for ReservoirResultHander maybe to_results first and then merge?
        for (size_t t = 1; t < thread_max_num; t++) {
            for (size_t i = 0; i < size; ++i) {
                ress[0].merge(i, ress[t]);
            }
        }

        // sort
        ress[0].end_multiple();

        // copy result
        res.copy_from(ress[0], x_from, size);
    }
    delete[] ress;
}

/* Find the nearest neighbors for nx queries in a set of ny vectors */
template <class ResultHandler>
void exhaustive_L2sqr_IP_seq(
        const float* x,
        const float* y,
        size_t d,
        size_t nx,
        size_t ny,
        ResultHandler& res,
        decltype(fvec_inner_product) dis_compute_func,
        const BitsetView bitset) {
    size_t thread_max_num = omp_get_max_threads();
    if (ny > parallel_policy_threshold ||
        (nx < thread_max_num / 2 && ny >= thread_max_num * 32)) {
        exhaustive_parallel_on_ny(
                x, y, d, nx, ny, res, dis_compute_func, bitset);
    } else {
        exhaustive_parallel_on_nx(
                x, y, d, nx, ny, res, dis_compute_func, bitset);
    }
}

/* Find the nearest neighbors for nx queries in a set of ny vectors */
template <class ResultHandler>
void exhaustive_inner_product_seq(
        const float* x,
        const float* y,
        size_t d,
        size_t nx,
        size_t ny,
        ResultHandler& res,
        const BitsetView bitset) {
    using SingleResultHandler = typename ResultHandler::SingleResultHandler;
    int nt = std::min(int(nx), omp_get_max_threads());

#pragma omp parallel num_threads(nt)
    {
        SingleResultHandler resi(res);
#pragma omp for
        for (int64_t i = 0; i < nx; i++) {
            const float* x_i = x + i * d;
            const float* y_j = y;
            resi.begin(i);
            for (size_t j = 0; j < ny; j++) {
                if (bitset.empty() || !bitset.test(j)) {
                    float ip = fvec_inner_product(x_i, y_j, d);
                    resi.add_result(ip, j);
                }
                y_j += d;
            }
            resi.end();
        }
    }
}

template <class ResultHandler>
void exhaustive_L2sqr_seq(
        const float* x,
        const float* y,
        size_t d,
        size_t nx,
        size_t ny,
        ResultHandler& res,
        const BitsetView bitset) {
    using SingleResultHandler = typename ResultHandler::SingleResultHandler;
    int nt = std::min(int(nx), omp_get_max_threads());

#pragma omp parallel num_threads(nt)
    {
        SingleResultHandler resi(res);
#pragma omp for
        for (int64_t i = 0; i < nx; i++) {
            const float* x_i = x + i * d;
            const float* y_j = y;
            resi.begin(i);
            for (size_t j = 0; j < ny; j++) {
                if (bitset.empty() || !bitset.test(j)) {
                    float disij = fvec_L2sqr(x_i, y_j, d);
                    resi.add_result(disij, j);
                }
                y_j += d;
            }
            resi.end();
        }
    }
}

namespace {
float fvec_cosine(const float* x, const float* y, size_t d) {
    return fvec_inner_product(x, y, d) / sqrtf(fvec_norm_L2sqr(y, d));
}
} // namespace

template <class ResultHandler>
void exhaustive_cosine_seq(
        const float* x,
        const float* y,
        size_t d,
        size_t nx,
        size_t ny,
        ResultHandler& res,
        const BitsetView bitset) {
    using SingleResultHandler = typename ResultHandler::SingleResultHandler;
    int nt = std::min(int(nx), omp_get_max_threads());

#pragma omp parallel num_threads(nt)
    {
        SingleResultHandler resi(res);
#pragma omp for
        for (int64_t i = 0; i < nx; i++) {
            const float* x_i = x + i * d;
            const float* y_j = y;
            resi.begin(i);
            for (size_t j = 0; j < ny; j++) {
                if (bitset.empty() || !bitset.test(j)) {
                    float disij = fvec_cosine(x_i, y_j, d);
                    resi.add_result(disij, j);
                }
                y_j += d;
            }
            resi.end();
        }
    }
}

/** Find the nearest neighbors for nx queries in a set of ny vectors */
template <class ResultHandler>
void exhaustive_inner_product_blas(
        const float* x,
        const float* y,
        size_t d,
        size_t nx,
        size_t ny,
        ResultHandler& res,
        const BitsetView bitset) {
    // BLAS does not like empty matrices
    if (nx == 0 || ny == 0)
        return;

    /* block sizes */
    const size_t bs_x = distance_compute_blas_query_bs;
    const size_t bs_y = distance_compute_blas_database_bs;
    std::unique_ptr<float[]> ip_block(new float[bs_x * bs_y]);

    for (size_t i0 = 0; i0 < nx; i0 += bs_x) {
        size_t i1 = i0 + bs_x;
        if (i1 > nx)
            i1 = nx;

        res.begin_multiple(i0, i1);

        for (size_t j0 = 0; j0 < ny; j0 += bs_y) {
            size_t j1 = j0 + bs_y;
            if (j1 > ny)
                j1 = ny;
            /* compute the actual dot products */
            {
                float one = 1, zero = 0;
                FINTEGER nyi = j1 - j0, nxi = i1 - i0, di = d;
                sgemm_("Transpose",
                       "Not transpose",
                       &nyi,
                       &nxi,
                       &di,
                       &one,
                       y + j0 * d,
                       &di,
                       x + i0 * d,
                       &di,
                       &zero,
                       ip_block.get(),
                       &nyi);
            }

            res.add_results(j0, j1, ip_block.get(), bitset);
        }
        res.end_multiple();
        InterruptCallback::check();
    }
}

// distance correction is an operator that can be applied to transform
// the distances
template <class ResultHandler>
void exhaustive_L2sqr_blas(
        const float* x,
        const float* y,
        size_t d,
        size_t nx,
        size_t ny,
        ResultHandler& res,
        const float* y_norms = nullptr,
        const BitsetView bitset = nullptr) {
    // BLAS does not like empty matrices
    if (nx == 0 || ny == 0)
        return;

    /* block sizes */
    const size_t bs_x = distance_compute_blas_query_bs;
    const size_t bs_y = distance_compute_blas_database_bs;
    // const size_t bs_x = 16, bs_y = 16;
    std::unique_ptr<float[]> ip_block(new float[bs_x * bs_y]);
    std::unique_ptr<float[]> x_norms(new float[nx]);
    std::unique_ptr<float[]> del2;

    fvec_norms_L2sqr(x_norms.get(), x, d, nx);

    if (!y_norms) {
        float* y_norms2 = new float[ny];
        del2.reset(y_norms2);
        fvec_norms_L2sqr(y_norms2, y, d, ny);
        y_norms = y_norms2;
    }

    for (size_t i0 = 0; i0 < nx; i0 += bs_x) {
        size_t i1 = i0 + bs_x;
        if (i1 > nx)
            i1 = nx;

        res.begin_multiple(i0, i1);

        for (size_t j0 = 0; j0 < ny; j0 += bs_y) {
            size_t j1 = j0 + bs_y;
            if (j1 > ny)
                j1 = ny;
            /* compute the actual dot products */
            {
                float one = 1, zero = 0;
                FINTEGER nyi = j1 - j0, nxi = i1 - i0, di = d;
                sgemm_("Transpose",
                       "Not transpose",
                       &nyi,
                       &nxi,
                       &di,
                       &one,
                       y + j0 * d,
                       &di,
                       x + i0 * d,
                       &di,
                       &zero,
                       ip_block.get(),
                       &nyi);
            }
#pragma omp parallel for
            for (int64_t i = i0; i < i1; i++) {
                float* ip_line = ip_block.get() + (i - i0) * (j1 - j0);

                for (size_t j = j0; j < j1; j++) {
                    float ip = *ip_line;
                    float dis = x_norms[i] + y_norms[j] - 2 * ip;

                    // negative values can occur for identical vectors
                    // due to roundoff errors
                    if (dis < 0)
                        dis = 0;

                    *ip_line = dis;
                    ip_line++;
                }
            }
            res.add_results(j0, j1, ip_block.get(), bitset);
        }
        res.end_multiple();
        InterruptCallback::check();
    }
}

template <class ResultHandler>
void exhaustive_cosine_blas(
        const float* x,
        const float* y,
        size_t d,
        size_t nx,
        size_t ny,
        ResultHandler& res,
        const BitsetView bitset = nullptr) {
    // BLAS does not like empty matrices
    if (nx == 0 || ny == 0)
        return;

    /* block sizes */
    const size_t bs_x = distance_compute_blas_query_bs;
    const size_t bs_y = distance_compute_blas_database_bs;
    // const size_t bs_x = 16, bs_y = 16;
    std::unique_ptr<float[]> ip_block(new float[bs_x * bs_y]);
    std::unique_ptr<float[]> y_norms(new float[nx]);
    std::unique_ptr<float[]> del2;

    fvec_norms_L2(y_norms.get(), x, d, nx);

    for (size_t i0 = 0; i0 < nx; i0 += bs_x) {
        size_t i1 = i0 + bs_x;
        if (i1 > nx)
            i1 = nx;

        res.begin_multiple(i0, i1);

        for (size_t j0 = 0; j0 < ny; j0 += bs_y) {
            size_t j1 = j0 + bs_y;
            if (j1 > ny)
                j1 = ny;
            /* compute the actual dot products */
            {
                float one = 1, zero = 0;
                FINTEGER nyi = j1 - j0, nxi = i1 - i0, di = d;
                sgemm_("Transpose",
                       "Not transpose",
                       &nyi,
                       &nxi,
                       &di,
                       &one,
                       y + j0 * d,
                       &di,
                       x + i0 * d,
                       &di,
                       &zero,
                       ip_block.get(),
                       &nyi);
            }
#pragma omp parallel for
            for (int64_t i = i0; i < i1; i++) {
                float* ip_line = ip_block.get() + (i - i0) * (j1 - j0);

                for (size_t j = j0; j < j1; j++) {
                    float ip = *ip_line;
                    float dis = ip / y_norms[j];
                    *ip_line = dis;
                    ip_line++;
                }
            }
            res.add_results(j0, j1, ip_block.get(), bitset);
        }
        res.end_multiple();
        InterruptCallback::check();
    }
}

template <class DistanceCorrection, class ResultHandler>
static void knn_jaccard_blas(
        const float* x,
        const float* y,
        size_t d,
        size_t nx,
        size_t ny,
        ResultHandler& res,
        const DistanceCorrection& corr,
        const BitsetView bitset) {
    // BLAS does not like empty matrices
    if (nx == 0 || ny == 0)
        return;

    /* block sizes */
    const size_t bs_x = 4096, bs_y = 1024;
    // const size_t bs_x = 16, bs_y = 16;
    float* ip_block = new float[bs_x * bs_y];
    float* x_norms = new float[nx];
    float* y_norms = new float[ny];
    ScopeDeleter<float> del1(ip_block), del3(x_norms), del2(y_norms);

    fvec_norms_L2sqr(x_norms, x, d, nx);
    fvec_norms_L2sqr(y_norms, y, d, ny);

    for (size_t i0 = 0; i0 < nx; i0 += bs_x) {
        size_t i1 = i0 + bs_x;
        if (i1 > nx)
            i1 = nx;

        res.begin_multiple(i0, i1);

        for (size_t j0 = 0; j0 < ny; j0 += bs_y) {
            size_t j1 = j0 + bs_y;
            if (j1 > ny)
                j1 = ny;
            /* compute the actual dot products */
            {
                float one = 1, zero = 0;
                FINTEGER nyi = j1 - j0, nxi = i1 - i0, di = d;
                sgemm_("Transpose",
                       "Not transpose",
                       &nyi,
                       &nxi,
                       &di,
                       &one,
                       y + j0 * d,
                       &di,
                       x + i0 * d,
                       &di,
                       &zero,
                       ip_block,
                       &nyi);
            }

            /* collect minima */
#pragma omp parallel for
            for (size_t i = i0; i < i1; i++) {
                float* ip_line = ip_block + (i - i0) * (j1 - j0);

                for (size_t j = j0; j < j1; j++) {
                    if (bitset.empty() || !bitset.test(j)) {
                        float ip = *ip_line;
                        float dis = 1.0 - ip / (x_norms[i] + y_norms[j] - ip);

                        // negative values can occur for identical vectors
                        // due to roundoff errors
                        if (dis < 0)
                            dis = 0;

                        dis = corr(dis, i, j);
                        *ip_line = dis;
                    }
                    ip_line++;
                }
            }
            res.add_results(j0, j1, ip_block);
        }
        res.end_multiple();
        InterruptCallback::check();
    }
}

} // anonymous namespace

/*******************************************************
 * KNN driver functions
 *******************************************************/
int distance_compute_blas_threshold = 16384;
int distance_compute_blas_query_bs = 4096;
int distance_compute_blas_database_bs = 1024;
int distance_compute_min_k_reservoir = 100;

void knn_inner_product(
        const float* x,
        const float* y,
        size_t d,
        size_t nx,
        size_t ny,
        float_minheap_array_t* ha,
        const BitsetView bitset) {
    if (ha->k < distance_compute_min_k_reservoir) {
        HeapResultHandler<CMin<float, int64_t>> res(
                ha->nh, ha->val, ha->ids, ha->k);
        if (nx < distance_compute_blas_threshold) {
            exhaustive_L2sqr_IP_seq(
                    x, y, d, nx, ny, res, fvec_inner_product, bitset);
        } else {
            exhaustive_inner_product_blas(x, y, d, nx, ny, res, bitset);
        }
    } else {
        ReservoirResultHandler<CMin<float, int64_t>> res(
                ha->nh, ha->val, ha->ids, ha->k);
        if (nx < distance_compute_blas_threshold) {
            exhaustive_L2sqr_IP_seq(
                    x, y, d, nx, ny, res, fvec_inner_product, bitset);
        } else {
            exhaustive_inner_product_blas(x, y, d, nx, ny, res, bitset);
        }
    }
}

void knn_L2sqr(
        const float* x,
        const float* y,
        size_t d,
        size_t nx,
        size_t ny,
        float_maxheap_array_t* ha,
        const float* y_norm2,
        const BitsetView bitset) {
    if (ha->k < distance_compute_min_k_reservoir) {
        HeapResultHandler<CMax<float, int64_t>> res(
                ha->nh, ha->val, ha->ids, ha->k);

        if (nx < distance_compute_blas_threshold) {
            exhaustive_L2sqr_IP_seq(x, y, d, nx, ny, res, fvec_L2sqr, bitset);
        } else {
            exhaustive_L2sqr_blas(x, y, d, nx, ny, res, y_norm2, bitset);
        }
    } else {
        ReservoirResultHandler<CMax<float, int64_t>> res(
                ha->nh, ha->val, ha->ids, ha->k);
        if (nx < distance_compute_blas_threshold) {
            exhaustive_L2sqr_IP_seq(x, y, d, nx, ny, res, fvec_L2sqr, bitset);
        } else {
            exhaustive_L2sqr_blas(x, y, d, nx, ny, res, y_norm2, bitset);
        }
    }
}

void knn_cosine(
        const float* x,
        const float* y,
        size_t d,
        size_t nx,
        size_t ny,
        float_minheap_array_t* ha,
        const BitsetView bitset) {
    if (ha->k < distance_compute_min_k_reservoir) {
        HeapResultHandler<CMin<float, int64_t>> res(
                ha->nh, ha->val, ha->ids, ha->k);
        if (nx < distance_compute_blas_threshold) {
            exhaustive_L2sqr_IP_seq(x, y, d, nx, ny, res, fvec_cosine, bitset);
        } else {
            exhaustive_cosine_blas(x, y, d, nx, ny, res, bitset);
        }
    } else {
        ReservoirResultHandler<CMin<float, int64_t>> res(
                ha->nh, ha->val, ha->ids, ha->k);
        if (nx < distance_compute_blas_threshold) {
            exhaustive_L2sqr_IP_seq(
                    x, y, d, nx, ny, res, fvec_inner_product, bitset);
        } else {
            exhaustive_cosine_blas(x, y, d, nx, ny, res, bitset);
        }
    }
}

struct NopDistanceCorrection {
    float operator()(float dis, size_t /*qno*/, size_t /*bno*/) const {
        return dis;
    }
};

void knn_jaccard(
        const float* x,
        const float* y,
        size_t d,
        size_t nx,
        size_t ny,
        float_maxheap_array_t* ha,
        const BitsetView bitset) {
    if (d % 4 != 0) {
        // knn_jaccard_sse(x, y, d, nx, ny, res);
        FAISS_ASSERT_MSG(false, "dim is not multiple of 4!");
    } else {
        NopDistanceCorrection nop;
        HeapResultHandler<CMax<float, int64_t>> res(
                ha->nh, ha->val, ha->ids, ha->k);
        knn_jaccard_blas(x, y, d, nx, ny, res, nop, bitset);
    }
}

/***************************************************************************
 * Range search
 ***************************************************************************/

void range_search_L2sqr(
        const float* x,
        const float* y,
        size_t d,
        size_t nx,
        size_t ny,
        float radius,
        RangeSearchResult* res,
        const BitsetView bitset) {
    RangeSearchResultHandler<CMax<float, int64_t>> resh(res, radius);
    if (nx < distance_compute_blas_threshold) {
        exhaustive_L2sqr_seq(x, y, d, nx, ny, resh, bitset);
    } else {
        exhaustive_L2sqr_blas(x, y, d, nx, ny, resh, nullptr, bitset);
    }
}

void range_search_inner_product(
        const float* x,
        const float* y,
        size_t d,
        size_t nx,
        size_t ny,
        float radius,
        RangeSearchResult* res,
        const BitsetView bitset) {
    RangeSearchResultHandler<CMin<float, int64_t>> resh(res, radius);
    if (nx < distance_compute_blas_threshold) {
        exhaustive_inner_product_seq(x, y, d, nx, ny, resh, bitset);
    } else {
        exhaustive_inner_product_blas(x, y, d, nx, ny, resh, bitset);
    }
}

void range_search_cosine(
        const float* x,
        const float* y,
        size_t d,
        size_t nx,
        size_t ny,
        float radius,
        RangeSearchResult* res,
        const BitsetView bitset) {
    RangeSearchResultHandler<CMin<float, int64_t>> resh(res, radius);
    if (nx < distance_compute_blas_threshold) {
        exhaustive_cosine_seq(x, y, d, nx, ny, resh, bitset);
    } else {
        exhaustive_cosine_blas(x, y, d, nx, ny, resh, bitset);
    }
}

/***************************************************************************
 * compute a subset of  distances
 ***************************************************************************/

/* compute the inner product between x and a subset y of ny vectors,
   whose indices are given by idy.  */
void fvec_inner_products_by_idx(
        float* __restrict ip,
        const float* x,
        const float* y,
        const int64_t* __restrict ids, /* for y vecs */
        size_t d,
        size_t nx,
        size_t ny) {
#pragma omp parallel for
    for (int64_t j = 0; j < nx; j++) {
        const int64_t* __restrict idsj = ids + j * ny;
        const float* xj = x + j * d;
        float* __restrict ipj = ip + j * ny;
        for (size_t i = 0; i < ny; i++) {
            if (idsj[i] < 0)
                continue;
            ipj[i] = fvec_inner_product(xj, y + d * idsj[i], d);
        }
    }
}

/* compute the inner product between x and a subset y of ny vectors,
   whose indices are given by idy.  */
void fvec_L2sqr_by_idx(
        float* __restrict dis,
        const float* x,
        const float* y,
        const int64_t* __restrict ids, /* ids of y vecs */
        size_t d,
        size_t nx,
        size_t ny) {
#pragma omp parallel for
    for (int64_t j = 0; j < nx; j++) {
        const int64_t* __restrict idsj = ids + j * ny;
        const float* xj = x + j * d;
        float* __restrict disj = dis + j * ny;
        for (size_t i = 0; i < ny; i++) {
            if (idsj[i] < 0)
                continue;
            disj[i] = fvec_L2sqr(xj, y + d * idsj[i], d);
        }
    }
}

void pairwise_indexed_L2sqr(
        size_t d,
        size_t n,
        const float* x,
        const int64_t* ix,
        const float* y,
        const int64_t* iy,
        float* dis) {
#pragma omp parallel for
    for (int64_t j = 0; j < n; j++) {
        if (ix[j] >= 0 && iy[j] >= 0) {
            dis[j] = fvec_L2sqr(x + d * ix[j], y + d * iy[j], d);
        }
    }
}

void pairwise_indexed_inner_product(
        size_t d,
        size_t n,
        const float* x,
        const int64_t* ix,
        const float* y,
        const int64_t* iy,
        float* dis) {
#pragma omp parallel for
    for (int64_t j = 0; j < n; j++) {
        if (ix[j] >= 0 && iy[j] >= 0) {
            dis[j] = fvec_inner_product(x + d * ix[j], y + d * iy[j], d);
        }
    }
}

/* Find the nearest neighbors for nx queries in a set of ny vectors
   indexed by ids. May be useful for re-ranking a pre-selected vector list */
void knn_inner_products_by_idx(
        const float* x,
        const float* y,
        const int64_t* ids,
        size_t d,
        size_t nx,
        size_t ny,
        float_minheap_array_t* res) {
    size_t k = res->k;

#pragma omp parallel for
    for (int64_t i = 0; i < nx; i++) {
        const float* x_ = x + i * d;
        const int64_t* idsi = ids + i * ny;
        size_t j;
        float* __restrict simi = res->get_val(i);
        int64_t* __restrict idxi = res->get_ids(i);
        minheap_heapify(k, simi, idxi);

        for (j = 0; j < ny; j++) {
            if (idsi[j] < 0)
                break;
            float ip = fvec_inner_product(x_, y + d * idsi[j], d);

            if (ip > simi[0]) {
                minheap_replace_top(k, simi, idxi, ip, idsi[j]);
            }
        }
        minheap_reorder(k, simi, idxi);
    }
}

void knn_L2sqr_by_idx(
        const float* x,
        const float* y,
        const int64_t* __restrict ids,
        size_t d,
        size_t nx,
        size_t ny,
        float_maxheap_array_t* res) {
    size_t k = res->k;

#pragma omp parallel for
    for (int64_t i = 0; i < nx; i++) {
        const float* x_ = x + i * d;
        const int64_t* __restrict idsi = ids + i * ny;
        float* __restrict simi = res->get_val(i);
        int64_t* __restrict idxi = res->get_ids(i);
        maxheap_heapify(res->k, simi, idxi);
        for (size_t j = 0; j < ny; j++) {
            float disij = fvec_L2sqr(x_, y + d * idsi[j], d);

            if (disij < simi[0]) {
                maxheap_replace_top(k, simi, idxi, disij, idsi[j]);
            }
        }
        maxheap_reorder(res->k, simi, idxi);
    }
}

void pairwise_L2sqr(
        int64_t d,
        int64_t nq,
        const float* xq,
        int64_t nb,
        const float* xb,
        float* dis,
        int64_t ldq,
        int64_t ldb,
        int64_t ldd) {
    if (nq == 0 || nb == 0)
        return;
    if (ldq == -1)
        ldq = d;
    if (ldb == -1)
        ldb = d;
    if (ldd == -1)
        ldd = nb;

    // store in beginning of distance matrix to avoid malloc
    float* b_norms = dis;

#pragma omp parallel for
    for (int64_t i = 0; i < nb; i++)
        b_norms[i] = fvec_norm_L2sqr(xb + i * ldb, d);

#pragma omp parallel for
    for (int64_t i = 1; i < nq; i++) {
        float q_norm = fvec_norm_L2sqr(xq + i * ldq, d);
        for (int64_t j = 0; j < nb; j++)
            dis[i * ldd + j] = q_norm + b_norms[j];
    }

    {
        float q_norm = fvec_norm_L2sqr(xq, d);
        for (int64_t j = 0; j < nb; j++)
            dis[j] += q_norm;
    }

    {
        FINTEGER nbi = nb, nqi = nq, di = d, ldqi = ldq, ldbi = ldb, lddi = ldd;
        float one = 1.0, minus_2 = -2.0;

        sgemm_("Transposed",
               "Not transposed",
               &nbi,
               &nqi,
               &di,
               &minus_2,
               xb,
               &ldbi,
               xq,
               &ldqi,
               &one,
               dis,
               &lddi);
    }
}

void inner_product_to_L2sqr(
        float* __restrict dis,
        const float* nr1,
        const float* nr2,
        size_t n1,
        size_t n2) {
#pragma omp parallel for
    for (int64_t j = 0; j < n1; j++) {
        float* disj = dis + j * n2;
        for (size_t i = 0; i < n2; i++)
            disj[i] = nr1[j] + nr2[i] - 2 * disj[i];
    }
}

void elkan_L2_sse(
        const float* x,
        const float* y,
        size_t d,
        size_t nx,
        size_t ny,
        int64_t* ids,
        float* val) {
    if (nx == 0 || ny == 0) {
        return;
    }

    const size_t bs_y = 1024;
    float* data = (float*)malloc((bs_y * (bs_y - 1) / 2) * sizeof(float));

    for (size_t j0 = 0; j0 < ny; j0 += bs_y) {
        size_t j1 = j0 + bs_y;
        if (j1 > ny)
            j1 = ny;

        auto Y = [&](size_t i, size_t j) -> float& {
            assert(i != j);
            i -= j0, j -= j0;
            return (i > j) ? data[j + i * (i - 1) / 2]
                           : data[i + j * (j - 1) / 2];
        };

#pragma omp parallel
        {
            int nt = omp_get_num_threads();
            int rank = omp_get_thread_num();
            for (size_t i = j0 + 1 + rank; i < j1; i += nt) {
                const float* y_i = y + i * d;
                for (size_t j = j0; j < i; j++) {
                    const float* y_j = y + j * d;
                    Y(i, j) = fvec_L2sqr(y_i, y_j, d);
                }
            }
        }

#pragma omp parallel for
        for (size_t i = 0; i < nx; i++) {
            const float* x_i = x + i * d;

            int64_t ids_i = j0;
            float val_i = fvec_L2sqr(x_i, y + j0 * d, d);
            float val_i_time_4 = val_i * 4;
            for (size_t j = j0 + 1; j < j1; j++) {
                if (val_i_time_4 <= Y(ids_i, j)) {
                    continue;
                }
                const float* y_j = y + j * d;
                float disij = fvec_L2sqr(x_i, y_j, d / 2);
                if (disij >= val_i) {
                    continue;
                }
                disij += fvec_L2sqr(x_i + d / 2, y_j + d / 2, d - d / 2);
                if (disij < val_i) {
                    ids_i = j;
                    val_i = disij;
                    val_i_time_4 = val_i * 4;
                }
            }

            if (j0 == 0 || val[i] > val_i) {
                val[i] = val_i;
                ids[i] = ids_i;
            }
        }
    }

    free(data);
}

} // namespace faiss

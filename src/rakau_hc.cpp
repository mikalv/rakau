#include <array>
#include <cstddef>
#include <tuple>
#include <utility>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
#pragma GCC diagnostic ignored "-Wdeprecated-dynamic-exception-spec"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wnon-virtual-dtor"

#include <hc.hpp>
#include <hc_math.hpp>

#pragma GCC diagnostic pop

#include <boost/numeric/conversion/cast.hpp>

#include <rakau/detail/hc_fwd.hpp>
#include <rakau/detail/tree_fwd.hpp>

namespace rakau
{

inline namespace detail
{

// Machinery to transform an array of pointers into a tuple of array views.
template <typename T, std::size_t N, std::size_t... I>
inline auto ap2tv_impl(const std::array<T *, N> &a, int size, std::index_sequence<I...>)
{
    return std::make_tuple(hc::array_view<T, 1>(size, a[I])...);
}

template <typename T, std::size_t N>
inline auto ap2tv(const std::array<T *, N> &a, int size)
{
    return ap2tv_impl(a, size, std::make_index_sequence<N>{});
}

// Helper to turn a tuple of values of the same type into an array.
template <typename Tuple, std::size_t... I>
inline auto t2a_impl(const Tuple &tup, std::index_sequence<I...>)
#if defined(__HCC_ACCELERATOR__)
    [[hc]]
#endif
{
    return std::array<std::tuple_element_t<0, Tuple>, std::tuple_size_v<Tuple>>{std::get<I>(tup)...};
}

template <typename Tuple>
inline auto t2a(const Tuple &tup)
#if defined(__HCC_ACCELERATOR__)
    [[hc]]
#endif
{
    return t2a_impl(tup, std::make_index_sequence<std::tuple_size_v<Tuple>>{});
}

template <unsigned Q, std::size_t NDim, typename F, typename PView, typename ResArray>
inline void tree_self_interactions_hcc(F eps2, int pidx, int tgt_begin, int tgt_end, PView p_view, ResArray &res_array)
    [[hc]]
{
    // Temporary vectors to be used in the loops below.
    std::array<F, NDim> diffs, pos;
    // Load the coords of the current particle.
    for (std::size_t j = 0; j < NDim; ++j) {
        pos[j] = p_view[j][pidx];
    }
    // Load the mass of the current particle.
    const auto m1 = p_view[NDim][pidx];
    // NOTE: setup the loop to avoid computing self interactions on pidx.
    for (auto i = tgt_begin + (tgt_begin == pidx); i < tgt_end; i += 1 + ((i + 1) == pidx)) {
        // Determine dist2, dist and dist3.
        F dist2(eps2);
        for (std::size_t j = 0; j < NDim; ++j) {
            const auto tmp = p_view[j][i] - pos[j];
            diffs[j] = tmp;
            dist2 = fma(tmp, tmp, dist2);
        }
        const auto dist = sqrt(dist2), m2 = p_view[NDim][i];
        if constexpr (Q == 0u || Q == 2u) {
            // Q == 0 or 2: accelerations are requested.
            const auto dist3 = dist2 * dist, m2_dist3 = m2 / dist3, m1_dist3 = m1 / dist3;
            // Accumulate the accelerations.
            for (std::size_t j = 0; j < NDim; ++j) {
                res_array[j] = fma(m2_dist3, diffs[j], res_array[j]);
            }
        }
        if constexpr (Q == 1u || Q == 2u) {
            // Q == 1 or 2: potentials are requested.
            // Establish the index of the potential in the result array:
            // 0 if only the potentials are requested, NDim otherwise.
            constexpr auto pot_idx = static_cast<std::size_t>(Q == 1u ? 0u : NDim);
            // Compute the negated mutual potential.
            const auto mut_pot = m1 / dist * m2;
            // Subtract mut_pot from the accumulator for the current particle.
            res_array[pot_idx] -= mut_pot;
        }
    }
}

template <unsigned Q, std::size_t NDim, typename F, typename SrcNode, typename PView, typename ResArray>
inline void tree_acc_pot_leaf_hcc(const SrcNode &src_node, F eps2, int src_idx, int pidx, PView p_view,
                                  ResArray &res_array) [[hc]]
{
    // Establish the range of the source node.
    const auto src_begin = static_cast<int>(std::get<1>(src_node)[0]),
               src_end = static_cast<int>(std::get<1>(src_node)[1]);
    // Local variables for the scalar computation.
    std::array<F, NDim> pos1, diffs;
    // Load the coordinates of the current particle
    // in the target node.
    for (std::size_t j = 0; j < NDim; ++j) {
        pos1[j] = p_view[j][pidx];
    }
    // Load the target mass, but only if we are interested in the potentials.
    [[maybe_unused]] F m1;
    if constexpr (Q == 1u || Q == 2u) {
        m1 = p_view[NDim][pidx];
    }
    // Iterate over the particles in the src node.
    for (auto i = src_begin; i < src_end; ++i) {
        F dist2(eps2);
        for (std::size_t j = 0; j < NDim; ++j) {
            diffs[j] = p_view[j][i] - pos1[j];
            dist2 = fma(diffs[j], diffs[j], dist2);
        }
        const auto dist = sqrt(dist2), m2 = p_view[NDim][i];
        if constexpr (Q == 0u || Q == 2u) {
            // Q == 0 or 2: accelerations are requested.
            const auto dist3 = dist * dist2, m_dist3 = m2 / dist3;
            for (std::size_t j = 0; j < NDim; ++j) {
                res_array[j] = fma(diffs[j], m_dist3, res_array[j]);
            }
        }
        if constexpr (Q == 1u || Q == 2u) {
            // Q == 1 or 2: potentials are requested.
            // Establish the index of the potential in the result array:
            // 0 if only the potentials are requested, NDim otherwise.
            constexpr auto pot_idx = static_cast<std::size_t>(Q == 1u ? 0u : NDim);
            res_array[pot_idx] = fma(-m1, m2 / dist, res_array[pot_idx]);
        }
    }
}

template <unsigned Q, std::size_t NDim, typename F, typename SrcNode, typename PView, typename ResArray>
inline void tree_acc_pot_bh_com_hcc(const SrcNode &src_node, F eps2, int src_idx, int pidx, PView p_view,
                                    ResArray &res_array, F dist2, const std::array<F, NDim> &dist_vec) [[hc]]
{
    // Add the softening length to dist2.
    dist2 += eps2;
    // Compute the (softened) distance.
    const F dist = sqrt(dist2);
    // Load locally the mass of the source node.
    const auto m_src = std::get<2>(src_node);
    if constexpr (Q == 0u || Q == 2u) {
        // Q == 0 or 2: accelerations are requested.
        const auto m_src_dist3 = m_src / (dist * dist2);
        for (std::size_t j = 0; j < NDim; ++j) {
            res_array[j] = fma(dist_vec[j], m_src_dist3, res_array[j]);
        }
    }
    if constexpr (Q == 1u || Q == 2u) {
        // Q == 1 or 2: potentials are requested.
        // Establish the index of the potential in the result array:
        // 0 if only the potentials are requested, NDim otherwise.
        constexpr auto pot_idx = static_cast<std::size_t>(Q == 1u ? 0u : NDim);
        // Load the target mass and compute the potential.
        res_array[pot_idx] = fma(-p_view[NDim][pidx], m_src / dist, res_array[pot_idx]);
    }
}

template <unsigned Q, std::size_t NDim, typename F, typename SrcNode, typename PView, typename ResArray>
inline int tree_acc_pot_bh_check_hcc(const SrcNode &src_node, int src_idx, F theta2, F eps2, int pidx, PView p_view,
                                     ResArray &res_array) [[hc]]
{
    // Copy locally the number of children of the source node.
    const auto n_children_src = static_cast<int>(std::get<1>(src_node)[2]);
    // Copy locally the COM coords of the source.
    const auto com_pos = std::get<3>(src_node);
    // Copy locally the dim2 of the source node.
    const auto src_dim2 = std::get<5>(src_node);
    // Compute the distance between target particle and source COM.
    F dist2(0);
    std::array<F, NDim> dist_vec;
    for (std::size_t j = 0; j < NDim; ++j) {
        const auto diff = com_pos[j] - p_view[j][pidx];
        dist2 = fma(diff, diff, dist2);
        dist_vec[j] = diff;
    }
    if (src_dim2 < theta2 * dist2) {
        // The source node satisfies the BH criterion for all the particles of the target node. Add the
        // acceleration due to the com of the source node.
        tree_acc_pot_bh_com_hcc<Q, NDim>(src_node, eps2, src_idx, pidx, p_view, res_array, dist2, dist_vec);
        // We can now skip all the children of the source node.
        return src_idx + n_children_src + 1;
    }
    // The source node does not satisfy the BH criterion. We check if it is a leaf
    // node, in which case we need to compute all the pairwise interactions.
    if (!n_children_src) {
        // Leaf node.
        tree_acc_pot_leaf_hcc<Q, NDim>(src_node, eps2, src_idx, pidx, p_view, res_array);
    }
    // In any case, we keep traversing the tree moving to the next node in depth-first order.
    return src_idx + 1;
}

template <unsigned Q, std::size_t NDim, typename F, typename UInt>
void acc_pot_impl_hcc(const std::array<F *, tree_nvecs_res<Q, NDim>> &out, const tree_cnode_t<F, UInt> *cnodes,
                      tree_size_t<F> cnodes_size, const tree_node_t<NDim, F, UInt> *tree, tree_size_t<F> tree_size,
                      const std::array<const F *, NDim + 1u> &p_parts, tree_size_t<F> nparts, F theta2, F G, F eps2,
                      tree_size_t<F> ncrit)
{
    using size_type = tree_size_t<F>;
    hc::array_view<const tree_node_t<NDim, F, UInt>, 1> tree_view(boost::numeric_cast<int>(tree_size), tree);
    hc::array_view<const tree_cnode_t<F, UInt>, 1> cnodes_view(boost::numeric_cast<int>(cnodes_size), cnodes);
    // Turn the input particles data and the result buffers into tuples of views, for use
    // in the parallel_for_each().
    auto pt = ap2tv(p_parts, boost::numeric_cast<int>(nparts));
    auto rt = ap2tv(out, boost::numeric_cast<int>(nparts));
    // TODO overflow checks on ncrit*ncodes.
    // TODO checking that no critical node has actually more than ncrit particles.
    hc::parallel_for_each(
        hc::extent<1>(boost::numeric_cast<int>(ncrit * cnodes_size)).tile(boost::numeric_cast<int>(ncrit)),
        [cnodes_view, tree_view, tsize = static_cast<int>(tree_size), pt, rt, theta2, G,
         eps2](hc::tiled_index<1> thread_id) [[hc]] {
            // Turn the tuples back into arrays.
            auto p_view = t2a(pt);
            auto res_view = t2a(rt);

            // Fetch the info about the critical node to which the current particle belongs.
            const auto tgt_code = std::get<0>(cnodes_view[thread_id.tile]);
            const auto tgt_begin = static_cast<int>(std::get<1>(cnodes_view[thread_id.tile]));
            const auto tgt_end = static_cast<int>(std::get<2>(cnodes_view[thread_id.tile]));
            const auto tgt_level = tree_level<NDim>(tgt_code);

            // The tile size is the maximum number of particles in a critical node.
            // Many critical nodes will have fewer particles than that, so we must ensure
            // that we are not going out of the boundaries of the critical node.
            if (thread_id.local[0] >= (tgt_end - tgt_begin)) {
                return;
            }
            // Compute the global index in the particles arrays of the current particle.
            const int pidx = tgt_begin + thread_id.local[0];

            // Array of results, inited to zeroes.
            std::array<F, tree_nvecs_res<Q, NDim>> res_array{};

            // Start looping over the source data.
            for (auto src_idx = 0; src_idx < tsize;) {
                // Get a reference to the current source node.
                const auto &src_node = tree_view[src_idx];
                // Extract the code of the source node.
                const auto src_code = std::get<0>(src_node);
                // Number of children of the source node.
                const auto n_children_src = static_cast<int>(std::get<1>(src_node)[2]);
                // Extract the level of the source node.
                const auto src_level = std::get<4>(src_node);
                // Compute the shifted target code. This is tgt_code
                // shifted down by the difference between tgt_level
                // and src_level. For instance, in an octree,
                // if the target code is 1 000 000 001 000, then tgt_level
                // is 4, and, if src_level is 2, then the shifted code
                // will be 1 000 000.
                const auto s_tgt_code = static_cast<UInt>(tgt_code >> ((tgt_level - src_level) * NDim));
                if (s_tgt_code == src_code) {
                    // The shifted target code coincides with the source code. This means
                    // that either the source node is an ancestor of the target node, or it is
                    // the target node itself. In the former cases, we just have to continue
                    // the depth-first traversal by setting ++src_idx. In the latter case,
                    // we want to bump up src_idx by n_children_src + 1 in order to skip
                    // the target node and all its children. We will compute later the self
                    // interactions in the target node.
                    const auto tgt_eq_src_mask = static_cast<unsigned>(-(src_code == tgt_code));
                    src_idx += 1 + static_cast<int>(static_cast<unsigned>(n_children_src) & tgt_eq_src_mask);
                } else {
                    // The source node is not an ancestor of the target. We need to run the BH criterion
                    // check. The tree_acc_pot_bh_check_hcc() function will return the index of the next node
                    // in the traversal.
                    src_idx
                        = tree_acc_pot_bh_check_hcc<Q, NDim>(src_node, src_idx, theta2, eps2, pidx, p_view, res_array);
                }
            }

            // Compute the self interactions within the target node.
            tree_self_interactions_hcc<Q, NDim>(eps2, pidx, tgt_begin, tgt_end, p_view, res_array);

            // Handle the G constant and write out the result.
            for (std::size_t j = 0; j < tree_nvecs_res<Q, NDim>; ++j) {
                res_view[j][pidx] = fma(G, res_array[j], res_view[j][pidx]);
            }
        })
        .get();
    for (auto &v : t2a(rt)) {
        v.synchronize();
    }
}

// Explicit instantiations.
#define RAKAU_HC_EXPLICIT_INST(Q, NDim, F, UInt)                                                                       \
    template void acc_pot_impl_hcc<Q, NDim, F, UInt>(                                                                  \
        const std::array<F *, tree_nvecs_res<Q, NDim>> &, const tree_cnode_t<F, UInt> *, tree_size_t<F>,               \
        const tree_node_t<NDim, F, UInt> *, tree_size_t<F>, const std::array<const F *, NDim + 1u> &, tree_size_t<F>,  \
        F, F, F, tree_size_t<F>)

RAKAU_HC_EXPLICIT_INST(0, 3, float, std::size_t);
RAKAU_HC_EXPLICIT_INST(1, 3, float, std::size_t);
RAKAU_HC_EXPLICIT_INST(2, 3, float, std::size_t);

RAKAU_HC_EXPLICIT_INST(0, 3, double, std::size_t);
RAKAU_HC_EXPLICIT_INST(1, 3, double, std::size_t);
RAKAU_HC_EXPLICIT_INST(2, 3, double, std::size_t);

#undef RAKAU_HC_EXPLICIT_INST

} // namespace detail
} // namespace rakau

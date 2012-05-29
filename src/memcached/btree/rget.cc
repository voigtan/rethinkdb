#include "memcached/btree/rget.hpp"

#include "errors.hpp"
#include <boost/bind.hpp>
#include <boost/make_shared.hpp>

#include "arch/runtime/runtime.hpp"
#include "btree/iteration.hpp"
#include "containers/iterators.hpp"
#include "memcached/btree/btree_data_provider.hpp"
#include "memcached/btree/node.hpp"
#include "memcached/btree/value.hpp"

/*
 * Possible rget designs:
 * 1. Depth-first search through the B-tree, then iterating through leaves (and maintaining a stack
 *    with some data to be able to backtrack).
 * 2. Breadth-first search, by maintaining a queue of blocks and releasing the lock on the block
 *    when we extracted the IDs of its children.
 * 3. Hybrid of 1 and 2: maintain a deque and use it as a queue, like in 2, thus releasing the locks
 *    for the top of the B-tree quickly, however when the deque reaches some size, start using it as
 *    a stack in depth-first search (but not quite in a usual way; see the note below).
 *
 * Problems of 1: we have to lock the whole path from the root down to the current node, which works
 * fine with small rgets (when max_results is low), but causes unnecessary amounts of locking (and
 * probably copy-on-writes, once we implement them).
 *
 * Problem of 2: while it doesn't hold unnecessary locks to the top (close to root) levels of the
 * B-tree, it may try to lock too much at once if the rget query effectively spans too many blocks
 * (e.g. when we try to rget the whole database).
 *
 * Hybrid approach seems to be the best choice here, because we hold the locks as low (far from the
 * root) in the tree as possible, while minimizing their number by doing a depth-first search from
 * some level.
 *
 * Note (on hybrid implementation):
 * If the deque approach is used, it is important to note that all the nodes in the current level
 * are in a reversed order when we decide to switch to popping from the stack:
 *
 *      P       Lets assume that we have node P in our deque, P is locked: [P]
 *    /   \     We remove P from the deque, lock its children, and push them back to the deque: [A, B]
 *   A     B    Now we can release the P lock.
 *  /|\   /.\   Next, we remove A, lock its children, and push them back to the deque: [B, c, d, e]
 * c d e .....  We release the A lock.
 * ..... ......
 *              At this point we decide that we need to do a depth-first search (to limit the number
 * of locked nodes), and start to use deque as a stack. However since we want
 * an inorder traversal, not the reversed inorder, we can't pop from the end of
 * the deque, we need to pop node 'c' instead of 'e', then (once we're done
 * with its depth-first search) do 'd', and then do 'e'.
 *
 * There are several possible approaches, one of them is putting markers in the deque in
 * between the nodes of different B-tree levels, another (probably a better one) is maintaining a
 * deque of deques, where the inner deques contain the nodes from the current B-tree level.
 *
 *
 * Currently the DFS design is implemented, since it's the simplest solution, also it is a good
 * fit for small rgets (the most popular use-case).
 *
 *
 * Most of the implementation now resides in btree/iteration.{hpp,cc}.
 * Actual merging of the slice iterators is done in server/key_value_store.cc.
 */

size_t estimate_rget_result_pair_size(const key_with_data_buffer_t &pair) {
    static const size_t rget_approx_per_key_overhead = 8;
    return rget_approx_per_key_overhead + pair.key.size + pair.value_provider->size();
}

rget_result_t memcached_rget_slice(btree_slice_t *slice, const key_range_t &range,
        int maximum, exptime_t effective_time, boost::scoped_ptr<transaction_t>& txn, boost::scoped_ptr<superblock_t> &superblock) {

    boost::shared_ptr<value_sizer_t<memcached_value_t> > sizer = boost::make_shared<memcached_value_sizer_t>(txn->get_cache()->get_block_size());

    rget_result_t result;
    size_t cumulative_size = 0;
    slice_keys_iterator_t<memcached_value_t> iterator(sizer, txn.get(), superblock, slice->home_thread(), range, &slice->stats);
    while (int(result.pairs.size()) < maximum && cumulative_size < rget_max_chunk_size) {
        boost::optional<key_value_pair_t<memcached_value_t> > next = iterator.next();
        if (!next) {
            break;
        }
        const memcached_value_t *value = reinterpret_cast<const memcached_value_t *>(next->value.get());
        if (value->expired(effective_time)) {
            continue;
        }
        intrusive_ptr_t<data_buffer_t> data(value_to_data_buffer(value, txn.get()));
        result.pairs.push_back(key_with_data_buffer_t(next->key, value->mcflags(), data));
        cumulative_size += estimate_rget_result_pair_size(result.pairs.back());
    }
    if (cumulative_size >= rget_max_chunk_size) {
        result.truncated = true;
    } else {
        result.truncated = false;
    }
    return result;
}
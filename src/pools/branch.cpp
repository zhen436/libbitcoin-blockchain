/**
 * Copyright (c) 2011-2017 libbitcoin developers (see AUTHORS)
 *
 * This file is part of libbitcoin.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <bitcoin/blockchain/pools/branch.hpp>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <numeric>
#include <utility>
#include <bitcoin/bitcoin.hpp>
#include <bitcoin/blockchain/define.hpp>

namespace libbitcoin {
namespace blockchain {

using namespace bc::chain;
using namespace bc::config;

branch::branch(size_t height)
  : height_(height),
    blocks_(std::make_shared<block_const_ptr_list>())
{
}

void branch::set_height(size_t height)
{
    height_ = height;
}

// Front is the top of the chain plus one, back is the top of the branch.
bool branch::push_front(block_const_ptr block)
{
    const auto linked = [this](block_const_ptr block)
    {
        const auto& front = blocks_->front()->header();
        return front.previous_block_hash() == block->hash();
    };

    if (empty() || linked(block))
    {
        blocks_->insert(blocks_->begin(), block);
        return true;
    }

    return false;
}

block_const_ptr branch::top() const
{
    return empty() ? nullptr : blocks_->back();
}

size_t branch::top_height() const
{
    return height() + size();
}

block_const_ptr_list_const_ptr branch::blocks() const
{
    return blocks_;
}

bool branch::empty() const
{
    return blocks_->empty();
}

size_t branch::size() const
{
    return blocks_->size();
}

size_t branch::height() const
{
    return height_;
}

hash_digest branch::hash() const
{
    return empty() ? null_hash :
        blocks_->front()->header().previous_block_hash();
}

config::checkpoint branch::fork_point() const
{
    return{ hash(), height() };
}

// private
size_t branch::index_of(size_t height) const
{
    // The member height_ is the height of the fork point, not the first block.
    return safe_subtract(safe_subtract(height, height_), size_t(1));
}

// private
size_t branch::height_at(size_t index) const
{
    // The height of the blockchain branch point plus zero-based index.
    return safe_add(safe_add(index, height_), size_t(1));
}

// The branch work check is both a consensus check and denial of service
// protection. It is necessary here that total claimed work exceeds that of the
// competing chain segment (consensus), and that the work has actually been
// expended (denial of service protection). The latter ensures we don't query
// the chain for total segment work path the branch competetiveness. Once work
// is proven sufficient the blocks are validated, requiring each to have the
// work required by the header accept check. It is possible that a longer chain
// of lower work blocks could meet both above criteria. However this requires
// the same amount of work as a shorter segment, so an attacker gains no
// advantage from that option, and it will be caught in validation.
uint256_t branch::work() const
{
    uint256_t total;

    // Not using accumulator here avoids repeated copying of uint256 object.
    for (auto block: *blocks_)
        total += block->proof();

    return total;
}

// BUGBUG: this does not differentiate between spent and unspent txs.
// Spent transactions could exist in the pool due to other txs in the same or
// later pool blocks. So this is disabled in favor of "allowed collisions".
// Otherwise it could reject a spent duplicate. Given that collisions must be
// rejected at least prior to the BIP34 checkpoint this is technically a
// consensus break which would only apply to a reorg at height less than BIP34.
////void branch::populate_duplicate(const chain::transaction& tx) const
////{
////    const auto outer = [&tx](size_t total, block_const_ptr block)
////    {
////        const auto hashes = [&tx](const transaction& block_tx)
////        {
////            return block_tx.hash() == tx.hash();
////        };
////
////        const auto& txs = block->transactions();
////        return total + std::count_if(txs.begin(), txs.end(), hashes);
////    };
////
////    // Counting all is easier than excluding self and terminating early.
////    const auto count = std::accumulate(blocks_->begin(), blocks_->end(),
////        size_t(0), outer);
////
////    BITCOIN_ASSERT(count > 0);
////    tx.validation.duplicate = count > 1u;
////}

void branch::populate_spent(const output_point& outpoint) const
{
    auto& prevout = outpoint.validation;

    // Assuming (1) block.check() validates against internal double spends
    // and (2) the outpoint is of the top block, there is no need to consider
    // the top block here. Under these assumptions spends in the top block
    // could only be double spent by a spend in a preceding block. Excluding
    // the top block requires that we consider 1 collision spent (vs. > 1).
    if (size() < 2u)
    {
        prevout.spent = false;
        prevout.confirmed = false;
        return;
    }

    // This is inefficient for long branches and will be replaced in v4 by the
    // database storage of weak chain blocks. This will allow use of the hash
    // table index to locate spends. However due to lack of weak chain indexing
    // if spend and position data in the store some inefficiency will remain.
    // This will be a design tradeoff of space against reorg performance.
    const auto blocks = [&outpoint](block_const_ptr block)
    {
        const auto transactions = [&outpoint](const transaction& tx)
        {
            const auto prevout_match = [&outpoint](const input& input)
            {
                return input.previous_output() == outpoint;
            };

            const auto& ins = tx.inputs();
            return std::any_of(ins.begin(), ins.end(), prevout_match);
        };

        const auto& txs = block->transactions();
        BITCOIN_ASSERT_MSG(!txs.empty(), "empty block in branch");
        return std::any_of(txs.begin() + 1, txs.end(), transactions);
    };

    auto spent = std::any_of(blocks_->begin() + 1, blocks_->end(), blocks);
    prevout.spent = spent;
    prevout.confirmed = prevout.spent;
}

void branch::populate_prevout(const output_point& outpoint) const
{
    const auto count = size();
    auto& prevout = outpoint.validation;
    struct result { size_t height; size_t position; output out; };

    const auto get_output = [this, count, &outpoint]() -> result
    {
        const auto& blocks = *blocks_;

        // Reverse search because of BIP30.
        for (size_t forward = 0; forward < count; ++forward)
        {
            const size_t index = count - forward - 1u;
            const auto& txs = blocks[index]->transactions();

            for (size_t position = 0; position < txs.size(); ++position)
            {
                const auto& tx = txs[position];

                if (outpoint.hash() == tx.hash() &&
                    outpoint.index() < tx.outputs().size())
                {
                    return
                    {
                        height_at(index),
                        position,
                        tx.outputs()[outpoint.index()]
                    };
                }
            }
        }

        return{};
    };

    // In case this input is a coinbase or the prevout is spent.
    prevout.cache = chain::output{};

    // The height of the prevout must be set iff the prevout is coinbase.
    prevout.height = output_point::validation_type::not_specified;

    // The input is a coinbase, so there is no prevout to populate.
    if (outpoint.is_null())
        return;

    // We continue even if prevout spent and/or missing.

    // Get the script and value for the prevout.
    const auto finder = get_output();

    if (!finder.out.is_valid())
        return;

    // Found the prevout at or below the indexed block.
    prevout.cache = finder.out;

    // Set height iff the prevout is coinbase (first tx is coinbase).
    if (finder.position == 0)
        prevout.height = finder.height;
}

/// The bits of the block at the given height in the branch.
bool branch::get_bits(uint32_t& out_bits, size_t height) const
{
    if (height <= height_)
        return false;

    const auto block = (*blocks_)[index_of(height)];

    if (!block)
        return false;

    out_bits = block->header().bits();
    return true;
}

// The version of the block at the given height in the branch.
bool branch::get_version(uint32_t& out_version, size_t height) const
{
    if (height <= height_)
        return false;

    const auto block = (*blocks_)[index_of(height)];

    if (!block)
        return false;

    out_version = block->header().version();
    return true;
}

// The timestamp of the block at the given height in the branch.
bool branch::get_timestamp(uint32_t& out_timestamp, size_t height) const
{
    if (height <= height_)
        return false;

    const auto block = (*blocks_)[index_of(height)];

    if (!block)
        return false;

    out_timestamp = block->header().timestamp();
    return true;
}

// The hash of the block at the given height if it exists in the branch.
bool branch::get_block_hash(hash_digest& out_hash, size_t height) const
{
    if (height <= height_)
        return false;

    const auto block = (*blocks_)[index_of(height)];

    if (!block)
        return false;

    out_hash = block->hash();
    return true;
}

} // namespace blockchain
} // namespace libbitcoin

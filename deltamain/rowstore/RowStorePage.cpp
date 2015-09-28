/*
 * (C) Copyright 2015 ETH Zurich Systems Group (http://www.systems.ethz.ch/) and others.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * Contributors:
 *     Markus Pilman <mpilman@inf.ethz.ch>
 *     Simon Loesing <sloesing@inf.ethz.ch>
 *     Thomas Etter <etterth@gmail.com>
 *     Kevin Bocksrocker <kevin.bocksrocker@gmail.com>
 *     Lucas Braun <braunl@inf.ethz.ch>
 */
#include "RowStorePage.hpp"
#include "deltamain/InsertMap.hpp"
#include "deltamain/Record.hpp"

#include <util/CuckooHash.hpp>

namespace tell {
namespace store {
namespace deltamain {

auto RowStorePage::Iterator::operator++() -> Iterator&
{
    CDMRecord rec(current);
    current += rec.size();
    return *this;
}

auto RowStorePage::Iterator::operator++(int) -> Iterator
{
    auto res = *this;
    ++(*this);
    return res;
}

const char* RowStorePage::Iterator::operator*() const {
    return current;
}

bool RowStorePage::Iterator::operator== (const Iterator& other) const {
    return current == other.current;
}

auto RowStorePage::begin() const -> Iterator {
    return Iterator(mData + 8);
}

auto RowStorePage::end() const -> Iterator {
    return Iterator(mData + mSize);
}

char* RowStorePage::gc(uint64_t lowestActiveVersion,
        InsertMap& insertMap,
        bool& done,
        Modifier& hashTable)
{
        // We iterate throgh our page
        uint64_t offset = mStartOffset;
        // in the first iteration we just decide wether we
        // need to collect any garbage here
        bool hasToClean = mStartOffset != 8;
        while (offset < mSize && !hasToClean) {
            CDMRecord rec(mData + offset);
            if (rec.needsCleaning(lowestActiveVersion, insertMap)) {
                hasToClean = true;
                break;
            }
            offset += rec.size();
        }
        if (!hasToClean) {
            // we are done - no cleaning needed for this page
            done = true;
            return mData;
        }

        // At this point we know that we will need to clean the page
        // if its the first gc call to that page, we have to mark it for deletion
        if (mStartOffset == 8)
            markCurrentForDeletion();

        // construct new fill page if needed
        constructFillPage();

        offset = mStartOffset;
        while (offset < mSize) {
            CDMRecord rec(mData + offset);
            bool couldRelocate = false;
            auto pos = mFillPage + mFillOffset;
            mFillOffset += rec.copyAndCompact(lowestActiveVersion,
                    insertMap,
                    pos,
                    TELL_PAGE_SIZE - mFillOffset,
                    couldRelocate);
            if (!couldRelocate) {
                // The current fillPage is full
                // In this case we set its used memory, return it and set mFillPage to null
                *reinterpret_cast<uint64_t*>(mFillPage) = mFillOffset;
                auto res = mFillPage;
                mFillPage = nullptr;
                done = false;
                return res;
            }
            hashTable.insert(rec.key(), pos, true);
            offset += rec.size();
        }

        // we are done, but the fillpage is (most probably) not full yet
        done = true;
        return nullptr;
}

char *RowStorePage::fillWithInserts(uint64_t lowestActiveVersion, InsertMap& insertMap, Modifier& hashTable)
{
    // construct new fill page if needed
    constructFillPage();

    // Create a dummy record to copy the inserts into the main page
    // The dummy record has one version that is marked as delete so only inserts are processed. The newest pointer and
    // the key have to be reset every time we use the dummy record.
    char dummyRecord[40];
    dummyRecord[0] = crossbow::to_underlying(RecordType::MULTI_VERSION_RECORD);
    *reinterpret_cast<uint32_t*>(dummyRecord + 4) = 1; // Number of versions
    *reinterpret_cast<uint64_t*>(dummyRecord + 24) = 0; // Version number
    *reinterpret_cast<uint32_t*>(dummyRecord + 32) = 40; // Offset to first version (start of data region)
    *reinterpret_cast<uint32_t*>(dummyRecord + 36) = 40; // Offset past the last version (end of data region)
    DMRecord dummy(dummyRecord);
    while (!insertMap.empty()) {
        bool couldRelocate;
        auto fst = insertMap.begin();
        uint64_t key = fst->first.key;
        // since we truncate the log only on a page level, it could be that
        // there are still some inserts that got processed in the previous GC phase
        if (hashTable.get(key)) {
            insertMap.erase(fst);
            continue;
        }

        *reinterpret_cast<const char**>(dummyRecord + 16) = nullptr; // Newest pointer
        dummy.writeKey(key);

        auto pos = mFillPage + mFillOffset;
        mFillOffset += dummy.copyAndCompact(lowestActiveVersion,
                insertMap,
                pos,
                TELL_PAGE_SIZE - mFillOffset,
                couldRelocate);
        if (couldRelocate) {
            hashTable.insert(key, pos);
            insertMap.erase(fst);
        } else {
            break;
        }
    }
    *reinterpret_cast<uint64_t*>(mFillPage) = mFillOffset;
    auto res = mFillPage;
    mFillPage = nullptr;
    return res;
}

} // namespace deltamain
} // namespace store
} // namespace tell


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

#pragma once

#include <util/IteratorEntry.hpp>

namespace tell {
namespace store {

class Record;

namespace deltamain {

class RowStoreVersionIterator {
    public:
        using IteratorEntry = tell::store::BaseIteratorEntry;
    private:
        IteratorEntry currEntry;
//        const Record* record; TODO: seems like it wouldn't be used...
        const char* current = nullptr;
        uint32_t idx = 0;
        void initRes();

    public:

        RowStoreVersionIterator(const Record* record, const char* current);

        RowStoreVersionIterator() {}

        bool isValid() const { return current != nullptr; }

        RowStoreVersionIterator& operator++();

        const IteratorEntry& operator*() const {
            return currEntry;
        }

        const IteratorEntry* operator->() const {
            return &currEntry;
        }
    };

} // namespace deltamain
} // namespace store
} // namespace tell

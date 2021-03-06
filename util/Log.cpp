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
#include "Log.hpp"

#include <crossbow/allocator.hpp>

namespace tell {
namespace store {

constexpr uint32_t LogPage::MAX_ENTRY_SIZE;

static_assert(ATOMIC_POINTER_LOCK_FREE, "Atomic pointer operations not supported");
static_assert(sizeof(LogPage*) == sizeof(std::atomic<LogPage*>), "Atomics won't work correctly");
static_assert(sizeof(LogPage) <= LogPage::LOG_HEADER_SIZE, "LOG_HEADER_SIZE must be larger or equal than LogPage");
static_assert(sizeof(LogEntry) <= LogEntry::LOG_ENTRY_SIZE, "LOG_ENTRY_SIZE must be larger or equal than LogEntry");

uint32_t LogEntry::tryAcquire(uint32_t size, uint32_t type) {
    LOG_ASSERT(size != 0x0u, "Size has to be greater than zero");
    LOG_ASSERT((size >> 31) == 0, "MSB has to be zero");

    auto s = ((size << 1) | 0x1u);
    uint32_t exp = 0x0u;
    if (!mSize.compare_exchange_strong(exp, s)) {
        return entrySizeFromSize(exp >> 1);
    }
    mType = type;
    return 0x0u;
}

LogEntry* LogPage::append(uint32_t size, uint32_t type /* = 0x0u */) {
    auto entrySize = LogEntry::entrySizeFromSize(size);
    if (entrySize > LogPage::MAX_ENTRY_SIZE) {
        LOG_ASSERT(false, "Tried to append %d bytes but %d bytes is max", entrySize, LogPage::MAX_ENTRY_SIZE);
        return nullptr;
    }
    return appendEntry(size, entrySize, type);
}

LogEntry* LogPage::appendEntry(uint32_t size, uint32_t entrySize, uint32_t type) {
    auto offset = mOffset.load();

    // Check if page is already sealed
    if ((offset & 0x1u) == 0) {
        return nullptr;
    }
    auto position = (offset >> 1);

    while (true) {
        auto endPosition = position + entrySize;

        // Check if we have enough space in the log page
        if (endPosition > LogPage::MAX_ENTRY_SIZE) {
            return nullptr;
        }

        // Try to acquire the space for the new entry
        auto entry = reinterpret_cast<LogEntry*>(this->data() + position);
        LOG_ASSERT((reinterpret_cast<uintptr_t>(entry) % 16) == 8 , "Position is not 16 byte aligned with offset 8");

        auto res = entry->tryAcquire(size, type);
        if (res != 0) {
            position += res;
            continue;
        }

        // Try to set the new offset until we succeed or another thread set a higher offset
        auto nOffset = ((endPosition << 1) | 0x1u);
        while (offset < nOffset) {
            // Set new offset, if this fails offset will contain the new offset value
            if (mOffset.compare_exchange_strong(offset, nOffset)) {
                break;
            }
            // Check if page was sealed in the meantime
            if ((offset & 0x1u) == 0) {
                // Check if page was sealed after we completely acquired the space for the log entry
                if ((offset >> 1) >= endPosition) {
                    break;
                }

                // Page was sealed before we completely acquired the space for the log entry
                return nullptr;
            }
        }

        return entry;
    }
}

void BaseLogImpl::freeEmptyPageNow(LogPage* page) {
    memset(page, 0, LogPage::LOG_HEADER_SIZE);
    mPageManager.free(page);
}

void BaseLogImpl::freePage(LogPage* begin, LogPage* end) {
    auto& pageManager = mPageManager;
    crossbow::allocator::invoke([begin, end, &pageManager]() {
        auto page = begin;
        while (page != end) {
            auto next = page->next().load();
            pageManager.free(page);
            page = next;
        }
    });
}

UnorderedLogImpl::UnorderedLogImpl(PageManager& pageManager)
        : BaseLogImpl(pageManager),
          mHead(LogHead(acquirePage(), nullptr)),
          mTail(mHead.load().writeHead),
          mPages(1) {
    LOG_ASSERT((reinterpret_cast<uintptr_t>(&mHead) % 16) == 0, "Head is not 16 byte aligned");
    LOG_ASSERT(mHead.is_lock_free(), "LogHead is not lock free");
    LOG_ASSERT(mHead.load().writeHead == mTail.load(), "Head and Tail do not point to the same page");
    if (!mHead.load().writeHead) {
        LOG_ERROR("PageManager ran out of space");
        std::terminate();
    }
}

void UnorderedLogImpl::appendPage(LogPage* begin, LogPage* end) {
    auto oldHead = mHead.load();

    auto pages = 1;
    for (auto page = begin; page != end; page = page->next().load()) {
        ++pages;
    }
    mPages.fetch_add(pages);

    while (true) {
        // Next should point to the last appendHead or the writeHead if there are no pages waiting to be appended
        auto next = (oldHead.appendHead ? oldHead.appendHead : oldHead.writeHead);
        end->next().store(next);

        // Seal the old append head
        if (oldHead.appendHead) {
            oldHead.appendHead->seal();
        }

        // Try to update the head
        LogHead nHead(oldHead.writeHead, begin);
        if (mHead.compare_exchange_strong(oldHead, nHead)) {
            break;
        }
    }
}

void UnorderedLogImpl::erase(LogPage* begin, LogPage* end) {
    LOG_ASSERT(begin != nullptr, "Begin page must not be null");

    if (begin == end) {
        return;
    }

    if (!end) {
        mTail.store(begin);
    }

    auto next = begin->next().exchange(end);
    if (next == end) return;

    auto pages = 0;
    for (auto page = next; page != end; page = page->next().load()) {
        ++pages;
    }
    mPages.fetch_sub(pages);

    freePage(next, end);
}

LogEntry* UnorderedLogImpl::appendEntry(uint32_t size, uint32_t entrySize, uint32_t type) {
    auto head = mHead.load();
    while (head.writeHead) {
        // Try to append a new log entry to the page
        auto entry = head.writeHead->appendEntry(size, entrySize, type);
        if (entry != nullptr) {
            return entry;
        }

        // The page must be full, acquire a new one
        head = createPage(head);
    }

    // This can only happen if the page manager ran out of space
    return nullptr;
}

UnorderedLogImpl::LogHead UnorderedLogImpl::createPage(LogHead oldHead) {
    auto writeHead = oldHead.writeHead;

    // Seal the old write head so no one can append
    writeHead->seal();

    while (true) {
        bool freeHead = false;
        LogHead nHead(oldHead.appendHead, nullptr);

        // If the append head is null we have to allocate a new head page
        if (!oldHead.appendHead) {
            nHead.writeHead = acquirePage();
            if (!nHead.writeHead) {
                LOG_ERROR("PageManager ran out of space");
                return nHead;
            }
            ++mPages;
            nHead.writeHead->next().store(oldHead.writeHead);
            freeHead = true;
        }

        // Try to set the page as new head
        // If this fails then another thread already set a new page and oldHead will point to it
        if (!mHead.compare_exchange_strong(oldHead, nHead)) {
            // We either have a new write or append head so we can free the previously allocated page
            if (freeHead) {
                --mPages;
                freeEmptyPageNow(nHead.writeHead);
            }

            // Check if the write head is still the same (i.e. only append head changed)
            if (oldHead.writeHead == writeHead) {
                continue;
            }

            // Write head changed so retry with new head
            return oldHead;
        }

        return nHead;
    }
}

OrderedLogImpl::OrderedLogImpl(PageManager& pageManager)
        : BaseLogImpl(pageManager),
          mHead(acquirePage()),
          mSealedHead(LogPosition(mHead.load(), 0)),
          mTail(LogPosition(mHead.load(), 0)) {
    LOG_ASSERT(mHead.load() == mSealedHead.load().page, "Head and Sealed head do not point to the same page");
    LOG_ASSERT(mHead.load() == mTail.load().page, "Head and Tail do not point to the same page");
    if (!mHead) {
        LOG_ERROR("PageManager ran out of space");
        std::terminate();
    }
}

void OrderedLogImpl::seal(LogEntry* entry) {
    entry->seal();

    // Check if the sealed head pointer points to another element
    auto sealedHead = mSealedHead.load();
    if (reinterpret_cast<LogEntry*>(sealedHead.page->data() + sealedHead.offset) != entry) {
        return;
    }
    advanceSealedHead(sealedHead);
}

bool OrderedLogImpl::truncateLog(LogIterator oldTail, LogIterator newTail) {
    LogPosition old(oldTail.page(), oldTail.offset());
    if (!mTail.compare_exchange_strong(old, LogPosition(newTail.page(), newTail.offset()))) {
        return false;
    }

    if (oldTail.page() != newTail.page()) {
        freePage(oldTail.page(), newTail.page());
    }

    return true;
}

LogEntry* OrderedLogImpl::appendEntry(uint32_t size, uint32_t entrySize, uint32_t type) {
    auto head = mHead.load();
    while (head) {
        // Try to append a new log entry to the page
        auto entry = head->appendEntry(size, entrySize, type);
        if (entry != nullptr) {
            return entry;
        }

        // The page must be full, acquire a new one
        head = createPage(head);
    }

    // This can only happen if the page manager ran out of space
    return nullptr;
}

LogPage* OrderedLogImpl::createPage(LogPage* oldHead) {
    // Check if the old head already has a next pointer
    auto next = oldHead->next().load();
    if (next) {
        // Try to set the next page as new head
        // If this fails then another thread already set a new head and oldHead will point to it
        if (!mHead.compare_exchange_strong(oldHead, next)) {
            return oldHead;
        }
        return next;
    }

    // Seal the old head so no one can append
    oldHead->seal();

    // Not enough space left in page - acquire new page
    auto nPage = acquirePage();
    if (!nPage) {
        LOG_ERROR("PageManager ran out of space");
        return nullptr;
    }

    // Try to set the new page as next page on the old head
    // If this fails then another thread already set a new page and next will point to it
    if (!oldHead->next().compare_exchange_strong(next, nPage)) {
        freeEmptyPageNow(nPage);
        return next;
    }

    // Set the page as new head
    // We do not care if this succeeds - if it does not, it means another thread updated the head for us
    auto expectedHead = oldHead;
    mHead.compare_exchange_strong(expectedHead, nPage);

    // The sealed end pointer must be moved to the next page in case it points past the last valid element
    auto sealedHead = mSealedHead.load();
    if (sealedHead.page == oldHead && sealedHead.offset == oldHead->offset()) {
        advanceSealedHead(sealedHead);
    }

    return nPage;
}

void OrderedLogImpl::advanceSealedHead(LogPosition oldSealedHead) {
    auto sealedHead = oldSealedHead;
    uint32_t size;
    bool sealed;
    LogEntry* currentEntry;

    // Check if the page has space left for a following log entry otherwise mark as move-to-next-page
    if (sealedHead.offset <= (LogPage::MAX_ENTRY_SIZE - LogEntry::LOG_ENTRY_SIZE)) {
        currentEntry = reinterpret_cast<LogEntry*>(sealedHead.page->data() + sealedHead.offset);
        std::tie(size, sealed) = currentEntry->sizeAndSealed();
    } else {
        size = 0u;
        sealed = true;
        currentEntry = nullptr;
    }

    while (true) {
        while (sealed) {
            if (size == 0u) {
                uint32_t pageOffset;
                bool pageSealed;
                std::tie(pageOffset, pageSealed) = sealedHead.page->offsetAndSealed();

                // If the page is not sealed other threads might still append to this page do not advance in this case
                if (!pageSealed) {
                    break;
                }

                // Check if another thread did an append in the meantime else move to next page
                if (pageOffset > sealedHead.offset) {
                    std::tie(size, sealed) = currentEntry->sizeAndSealed();
                    LOG_ASSERT(size != 0u, "Entry was not acquired despite being in valid page region");
                } else {
                    auto next = sealedHead.page->next().load();
                    // Only advance if the next page is already valid
                    if (next == nullptr) {
                        break;
                    }
                    sealedHead.page = next;
                    sealedHead.offset = 0u;

                    currentEntry = reinterpret_cast<LogEntry*>(sealedHead.page->data() + sealedHead.offset);
                    std::tie(size, sealed) = currentEntry->sizeAndSealed();
                }
            } else {
                sealedHead.offset += LogEntry::entrySizeFromSize(size);

                // Check if the page has space left for a following log entry otherwise mark as move-to-next-page
                if (sealedHead.offset <= (LogPage::MAX_ENTRY_SIZE - LogEntry::LOG_ENTRY_SIZE)) {
                    currentEntry = reinterpret_cast<LogEntry*>(sealedHead.page->data() + sealedHead.offset);
                    std::tie(size, sealed) = currentEntry->sizeAndSealed();
                } else {
                    size = 0u;
                }
            }
        }

        // Set the new unsealed head
        // In the case this fails the sealed head was moved forward by another thread and the algorithm can return.
        if (!mSealedHead.compare_exchange_strong(oldSealedHead, sealedHead)) {
            return;
        }
        oldSealedHead = sealedHead;

        // The oldest unsealed element might have been sealed in the meantime
        // In this case repeat the sealing process
        if (sealedHead.offset <= (LogPage::MAX_ENTRY_SIZE - LogEntry::LOG_ENTRY_SIZE)) {
            std::tie(size, sealed) = currentEntry->sizeAndSealed();
        } else {
            size = 0u;
            sealed = true;
        }

        // There might be a new next page
        if (size == 0u) {
            uint32_t pageOffset;
            bool pageSealed;
            std::tie(pageOffset, pageSealed) = sealedHead.page->offsetAndSealed();

            // If the page is not sealed then there will be no new page
            if (!pageSealed) {
                return;
            }

            // Check if elements are left on the current page otherwise advance to next page
            if (pageOffset > sealedHead.offset) {
                std::tie(size, sealed) = currentEntry->sizeAndSealed();
                LOG_ASSERT(size != 0u, "Entry was not acquired despite being in valid page region");
            } else {
                auto next = sealedHead.page->next().load();
                if (next == nullptr) {
                    return;
                }
                sealedHead.page = next;
                sealedHead.offset = 0u;

                currentEntry = reinterpret_cast<LogEntry*>(sealedHead.page->data() + sealedHead.offset);
                std::tie(size, sealed) = currentEntry->sizeAndSealed();
            }
        }

        if (!sealed) {
            return;
        }
    }
}

template <class Impl>
Log<Impl>::~Log() {
    // Safe Memory Reclamation mechanism has to ensure that the Log class is only deleted when no one references it
    // anymore so we can safely delete all pages here
    auto i = pageBegin();
    auto end = pageEnd();
    while (i != end) {
        auto page = i.operator->();
        ++i;
        Impl::freePageNow(page);
    }
}

template <class Impl>
LogEntry* Log<Impl>::append(uint32_t size, uint32_t type /* = 0x0u */) {
    auto entrySize = LogEntry::entrySizeFromSize(size);
    if (entrySize > LogPage::MAX_ENTRY_SIZE) {
        LOG_ASSERT(false, "Tried to append %d bytes but %d bytes is max", entrySize, LogPage::MAX_ENTRY_SIZE);
        return nullptr;
    }

    return Impl::appendEntry(size, entrySize, type);
}

template class Log<UnorderedLogImpl>;
template class Log<OrderedLogImpl>;

} // namespace store
} // namespace tell

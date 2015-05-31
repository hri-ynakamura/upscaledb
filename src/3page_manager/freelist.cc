/*
 * Copyright (C) 2005-2015 Christoph Rupp (chris@crupp.de).
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
 */

#include "0root/root.h"

// Always verify that a file of level N does not include headers > N!
#include "1base/error.h"
#include "1base/pickle.h"
#include "3page_manager/freelist.h"

#ifndef HAM_ROOT_H
#  error "root.h was not included"
#endif

namespace hamsterdb {

uint64_t
Freelist::store_state(Context *context)
{
  return (0);
}

void
Freelist::decode_state(uint8_t *data)
{
  uint32_t page_size = config.page_size_bytes;

  // get the number of stored elements
  uint32_t counter = *(uint32_t *)data;
  data += 4;

  // now read all pages
  for (uint32_t i = 0; i < counter; i++) {
    // 4 bits page_counter, 4 bits for number of following bytes
    int page_counter = (*data & 0xf0) >> 4;
    int num_bytes = *data & 0x0f;
    ham_assert(page_counter > 0);
    ham_assert(num_bytes <= 8);
    data += 1;

    uint64_t id = Pickle::decode_u64(num_bytes, data);
    data += num_bytes;

    free_pages[id * page_size] = page_counter;
  }
}

uint64_t
Freelist::alloc(size_t num_pages)
{
  uint64_t address = 0;
  uint32_t page_size = config.page_size_bytes;

  for (FreeMap::iterator it = free_pages.begin();
            it != free_pages.end();
            it++) {
    if (it->second == num_pages) {
      address = it->first;
      free_pages.erase(it);
      break;
    }
    if (it->second > num_pages) {
      address = it->first;
      free_pages[it->first + num_pages * page_size] = it->second - num_pages;
      free_pages.erase(it);
      break;
    }
  }

  if (address != 0)
    freelist_hits++;
  else
    freelist_misses++;

  return (address);
}

void
Freelist::put(uint64_t page_id, size_t page_count)
{
  free_pages[page_id] = page_count;
}

bool
Freelist::has(uint64_t page_id) const
{
  return (free_pages.find(page_id) != free_pages.end());
}

uint64_t
Freelist::truncate(uint64_t file_size)
{
  uint32_t page_size = config.page_size_bytes;
  uint64_t lower_bound = file_size;

  for (FreeMap::reverse_iterator it = free_pages.rbegin();
            it != free_pages.rend();
            it++) {
    if (it->first + it->second * page_size == lower_bound)
      lower_bound = it->first;
  }

  return (lower_bound);
}

} // namespace hamsterdb

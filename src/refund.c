/// \copyright SPDX-License-Identifier: Apache-2.0
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2015-2025

#include "internals.h"

#if MDBX_ENABLE_REFUND
static void refund_reclaimed(MDBX_txn *txn) {
  /* Scanning in descend order */
  pgno_t first_unallocated = txn->geo.first_unallocated;
  const pnl_t pnl = txn->wr.repnl;
  tASSERT(txn, pnl_size(pnl) && MDBX_PNL_MOST(pnl) == first_unallocated - 1);
#if MDBX_PNL_ASCENDING
  size_t i = pnl_size(pnl);
  tASSERT(txn, pnl[i] == first_unallocated - 1);
  while (--first_unallocated, --i > 0 && pnl[i] == first_unallocated - 1)
    ;
  pnl_setsize(pnl, i);
#else
  size_t i = 1;
  tASSERT(txn, pnl[i] == first_unallocated - 1);
  size_t len = pnl_size(pnl);
  while (--first_unallocated, ++i <= len && pnl[i] == first_unallocated - 1)
    ;
  pnl_setsize(pnl, len -= i - 1);
  for (size_t move = 0; move < len; ++move)
    pnl[1 + move] = pnl[i + move];
#endif
  VERBOSE("refunded %" PRIaPGNO " pages: %" PRIaPGNO " -> %" PRIaPGNO, txn->geo.first_unallocated - first_unallocated,
          txn->geo.first_unallocated, first_unallocated);
  txn->geo.first_unallocated = first_unallocated;
  tASSERT(txn, pnl_check_allocated(txn->wr.repnl, txn->geo.first_unallocated - 1));
}

static void refund_loose(MDBX_txn *txn) {
  tASSERT(txn, txn->wr.loose_pages != nullptr);
  tASSERT(txn, txn->wr.loose_count > 0);

  dpl_t *const dl = txn->wr.dirtylist;
  if (dl) {
    tASSERT(txn, dl->length >= txn->wr.loose_count);
    tASSERT(txn, (txn->flags & MDBX_WRITEMAP) == 0 || MDBX_AVOID_MSYNC);
  } else {
    tASSERT(txn, (txn->flags & MDBX_WRITEMAP) != 0 && !MDBX_AVOID_MSYNC);
  }

  pgno_t onstack[MDBX_CACHELINE_SIZE * 8 / sizeof(pgno_t)];
  pnl_t suitable = onstack;

  if (!dl || dl->length - dl->sorted > txn->wr.loose_count) {
    /* Dirty list is useless since unsorted. */
    if (pnl_bytes2size(sizeof(onstack)) < txn->wr.loose_count) {
      suitable = pnl_alloc(txn->wr.loose_count);
      if (unlikely(!suitable))
        return /* this is not a reason for transaction fail */;
    }

    /* Collect loose-pages which may be refunded. */
    tASSERT(txn, txn->geo.first_unallocated >= MIN_PAGENO + txn->wr.loose_count);
    pgno_t most = MIN_PAGENO;
    size_t w = 0;
    for (const page_t *lp = txn->wr.loose_pages; lp; lp = page_next(lp)) {
      tASSERT(txn, lp->flags == P_LOOSE);
      tASSERT(txn, txn->geo.first_unallocated > lp->pgno);
      if (likely(txn->geo.first_unallocated - txn->wr.loose_count <= lp->pgno)) {
        tASSERT(txn, w < ((suitable == onstack) ? pnl_bytes2size(sizeof(onstack)) : pnl_alloclen(suitable)));
        suitable[++w] = lp->pgno;
        most = (lp->pgno > most) ? lp->pgno : most;
      }
      MDBX_ASAN_UNPOISON_MEMORY_REGION(&page_next(lp), sizeof(page_t *));
      VALGRIND_MAKE_MEM_DEFINED(&page_next(lp), sizeof(page_t *));
    }

    if (most + 1 == txn->geo.first_unallocated) {
      /* Sort suitable list and refund pages at the tail. */
      pnl_setsize(suitable, w);
      pnl_sort(suitable, MAX_PAGENO + 1);

      /* Scanning in descend order */
      const intptr_t step = MDBX_PNL_ASCENDING ? -1 : 1;
      const intptr_t begin = MDBX_PNL_ASCENDING ? pnl_size(suitable) : 1;
      const intptr_t end = MDBX_PNL_ASCENDING ? 0 : pnl_size(suitable) + 1;
      tASSERT(txn, suitable[begin] >= suitable[end - step]);
      tASSERT(txn, most == suitable[begin]);

      for (intptr_t i = begin + step; i != end; i += step) {
        if (suitable[i] != most - 1)
          break;
        most -= 1;
      }
      const size_t refunded = txn->geo.first_unallocated - most;
      DEBUG("refund-suitable %zu pages %" PRIaPGNO " -> %" PRIaPGNO, refunded, most, txn->geo.first_unallocated);
      txn->geo.first_unallocated = most;
      txn->wr.loose_count -= refunded;
      if (dl) {
        txn->wr.dirtyroom += refunded;
        dl->pages_including_loose -= refunded;
        assert(txn->wr.dirtyroom <= txn->env->options.dp_limit);

        /* Filter-out dirty list */
        size_t r = 0;
        w = 0;
        if (dl->sorted) {
          do {
            if (dl->items[++r].pgno < most) {
              if (++w != r)
                dl->items[w] = dl->items[r];
            }
          } while (r < dl->sorted);
          dl->sorted = w;
        }
        while (r < dl->length) {
          if (dl->items[++r].pgno < most) {
            if (++w != r)
              dl->items[w] = dl->items[r];
          }
        }
        dpl_setlen(dl, w);
        tASSERT(txn, txn->wr.dirtyroom + txn->wr.dirtylist->length ==
                         (txn->parent ? txn->parent->wr.dirtyroom : txn->env->options.dp_limit));
      }
      goto unlink_loose;
    }
  } else {
    /* Dirtylist is mostly sorted, just refund loose pages at the end. */
    dpl_sort(txn);
    tASSERT(txn, dl->length < 2 || dl->items[1].pgno < dl->items[dl->length].pgno);
    tASSERT(txn, dl->sorted == dl->length);

    /* Scan dirtylist tail-forward and cutoff suitable pages. */
    size_t n;
    for (n = dl->length; dl->items[n].pgno == txn->geo.first_unallocated - 1 && dl->items[n].ptr->flags == P_LOOSE;
         --n) {
      tASSERT(txn, n > 0);
      page_t *dp = dl->items[n].ptr;
      DEBUG("refund-sorted page %" PRIaPGNO, dp->pgno);
      tASSERT(txn, dp->pgno == dl->items[n].pgno);
      txn->geo.first_unallocated -= 1;
    }
    dpl_setlen(dl, n);

    if (dl->sorted != dl->length) {
      const size_t refunded = dl->sorted - dl->length;
      dl->sorted = dl->length;
      txn->wr.loose_count -= refunded;
      txn->wr.dirtyroom += refunded;
      dl->pages_including_loose -= refunded;
      tASSERT(txn, txn->wr.dirtyroom + txn->wr.dirtylist->length ==
                       (txn->parent ? txn->parent->wr.dirtyroom : txn->env->options.dp_limit));

      /* Filter-out loose chain & dispose refunded pages. */
    unlink_loose:
      for (page_t *__restrict *__restrict link = &txn->wr.loose_pages; *link;) {
        page_t *dp = *link;
        tASSERT(txn, dp->flags == P_LOOSE);
        MDBX_ASAN_UNPOISON_MEMORY_REGION(&page_next(dp), sizeof(page_t *));
        VALGRIND_MAKE_MEM_DEFINED(&page_next(dp), sizeof(page_t *));
        if (txn->geo.first_unallocated > dp->pgno) {
          link = &page_next(dp);
        } else {
          *link = page_next(dp);
          if ((txn->flags & MDBX_WRITEMAP) == 0)
            page_shadow_release(txn->env, dp, 1);
        }
      }
    }
  }

  tASSERT(txn, dpl_check(txn));
  if (suitable != onstack)
    pnl_free(suitable);
  txn->wr.loose_refund_wl = txn->geo.first_unallocated;
}

bool txn_refund(MDBX_txn *txn) {
  const pgno_t before = txn->geo.first_unallocated;

  if (txn->wr.loose_pages && txn->wr.loose_refund_wl > txn->geo.first_unallocated)
    refund_loose(txn);

  while (true) {
    if (pnl_size(txn->wr.repnl) == 0 || MDBX_PNL_MOST(txn->wr.repnl) != txn->geo.first_unallocated - 1)
      break;

    refund_reclaimed(txn);
    if (!txn->wr.loose_pages || txn->wr.loose_refund_wl <= txn->geo.first_unallocated)
      break;

    const pgno_t memo = txn->geo.first_unallocated;
    refund_loose(txn);
    if (memo == txn->geo.first_unallocated)
      break;
  }

  if (before == txn->geo.first_unallocated)
    return false;

  if (txn->wr.spilled.list)
    /* Squash deleted pagenums if we refunded any */
    spill_purge(txn);

  return true;
}

#else /* MDBX_ENABLE_REFUND */

bool txn_refund(MDBX_txn *txn) {
  (void)txn;
  /* No online auto-compactification. */
  return false;
}

#endif /* MDBX_ENABLE_REFUND */

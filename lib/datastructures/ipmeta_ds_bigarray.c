/*
 * libipmeta
 *
 * Alistair King, CAIDA, UC San Diego
 * corsaro-info@caida.org
 *
 * Copyright (C) 2012 The Regents of the University of California.
 *
 * This file is part of libipmeta.
 *
 * libipmeta is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * libipmeta is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with libipmeta.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "config.h"

#include <arpa/inet.h>
#include <assert.h>

#include "utils.h"

#include "libipmeta_int.h"
#include "ipmeta_ds_bigarray.h"

#define DS_NAME "bigarray"

#define STATE(ds) (IPMETA_DS_STATE(bigarray, ds))

static ipmeta_ds_t ipmeta_ds_bigarray = {
  IPMETA_DS_BIGARRAY, DS_NAME, IPMETA_DS_GENERATE_PTRS(bigarray) NULL};

KHASH_INIT(u32u32, uint32_t, uint32_t, 1, kh_int_hash_func, kh_int_hash_equal)

typedef struct ipmeta_ds_bigarray_state {
  /** Temporary hash to map from record id to lookup id */
  khash_t(u32u32) * record_lookup;

  /** Mapping from a uint32 lookup id to a list of records (one per provider).
   * @note, 0 is a reserved ID (indicates empty)
   */
  ipmeta_record_t ***lookup_table;

  /** Number of records in the lookup table */
  int lookup_table_cnt;

  /** Mapping from IP address to uint32 lookup id (see lookup table) */
  uint32_t *array;
} ipmeta_ds_bigarray_state_t;

ipmeta_ds_t *ipmeta_ds_bigarray_alloc()
{
  return &ipmeta_ds_bigarray;
}

int ipmeta_ds_bigarray_init(ipmeta_ds_t *ds)
{
  /* the ds structure is malloc'd already, we just need to init the state */

  assert(STATE(ds) == NULL);

  if ((ds->state = malloc_zero(sizeof(ipmeta_ds_bigarray_state_t))) == NULL) {
    ipmeta_log(__func__, "could not malloc bigarray state");
    return -1;
  }

  /** NEVER support IPv6 :) */

  if ((STATE(ds)->array = malloc_zero(sizeof(uint32_t) * IPMETA_PROVIDER_MAX *
                                      UINT32_MAX)) == NULL) {
    ipmeta_log(__func__, "could not malloc big array. is this a 64bit OS?");
    return -1;
  }

  if ((STATE(ds)->lookup_table = malloc_zero(sizeof(ipmeta_record_t **))) ==
      NULL) {
    return -1;
  }
  STATE(ds)->lookup_table_cnt = 1;

  STATE(ds)->record_lookup = kh_init(u32u32);

  return 0;
}

void ipmeta_ds_bigarray_free(ipmeta_ds_t *ds)
{
  uint64_t i;
  if (ds == NULL) {
    return;
  }

  if (STATE(ds) != NULL) {
    if (STATE(ds)->lookup_table != NULL) {
      for (i = 0; i < STATE(ds)->lookup_table_cnt; i++) {
        free(STATE(ds)->lookup_table[i]);
      }
      free(STATE(ds)->lookup_table);
      STATE(ds)->lookup_table = NULL;
    }

    if (STATE(ds)->record_lookup != NULL) {
      kh_destroy(u32u32, STATE(ds)->record_lookup);
      STATE(ds)->record_lookup = NULL;
    }
    free(STATE(ds)->array);
    free(STATE(ds));
    ds->state = NULL;
  }

  free(ds);

  return;
}

#define LOOKUPINDEX(addr, prov)                                                \
  (STATE(ds)->array[(addr * IPMETA_PROVIDER_MAX) + (prov - 1)])

int ipmeta_ds_bigarray_add_prefix(ipmeta_ds_t *ds, uint32_t addr, uint8_t mask,
                                  ipmeta_record_t *record)
{
  assert(ds != NULL && STATE(ds) != NULL);
  ipmeta_ds_bigarray_state_t *state = STATE(ds);
  ipmeta_record_t **recarray = NULL;

  uint32_t first_addr = ntohl(addr) & (~0UL << (32 - mask));
  uint64_t i;
  uint32_t lookup_id;
  khiter_t khiter;
  int khret;

  /* check if this record is already in the record_lookup hash */
  if ((khiter = kh_get(u32u32, state->record_lookup, record->id)) ==
      kh_end(state->record_lookup)) {
    /* allocate the next id in the actual lookup table */

    /* check if we have run out of space */
    if (state->lookup_table_cnt == UINT32_MAX - 1) {
      ipmeta_log(__func__,
                 "The Big Array datastructure only supports 2^32 records");
      return -1;
    }

    /* realloc the lookup table for this record */
    if ((state->lookup_table = realloc(
           state->lookup_table, sizeof(ipmeta_record_t **) *
                                  (state->lookup_table_cnt + 1))) == NULL) {
      return -1;
    }

    recarray = calloc(IPMETA_PROVIDER_MAX, sizeof(ipmeta_record_t *));

    lookup_id = state->lookup_table_cnt;
    /* move on to the next lookup id */
    state->lookup_table_cnt++;

    /* store this record in the lookup table */
    state->lookup_table[lookup_id] = recarray;

    /* associate this record id with this lookup id */
    khiter = kh_put(u32u32, state->record_lookup, record->id, &khret);
    kh_value(state->record_lookup, khiter) = lookup_id;

  } else {
    lookup_id = kh_value(state->record_lookup, khiter);
    recarray = state->lookup_table[lookup_id];
  }

  recarray[record->source - 1] = record;
  /* iterate over all ips in this prefix and point them to this index in the
     table */
  for (i = first_addr; i < ((uint64_t)first_addr + (1 << (32 - mask))); i++) {
    LOOKUPINDEX(i, record->source) = lookup_id;
  }

  return 0;
}

int ipmeta_ds_bigarray_lookup_records(ipmeta_ds_t *ds, uint32_t addr,
                                      uint8_t mask, uint32_t providermask,
                                      ipmeta_record_set_t *records)
{
  assert(ds != NULL && ds->state != NULL);

  uint64_t total_ips = 1 << (32 - mask);
  uint64_t i;
  int j;
  ipmeta_record_t **recarray;
  uint64_t lookupind, arrayind;

  /* This has HORRIBLE performance. Never use bigarray for prefixes! */
  for (i = 0; i < total_ips; i++) {
    arrayind = ntohl(addr) + i;
    for (j = 0; j < IPMETA_PROVIDER_MAX; j++) {
      if ((1 << (j)) & providermask) {
        lookupind = LOOKUPINDEX(arrayind, j + 1);
        recarray = (ipmeta_record_t **)(STATE(ds)->lookup_table[lookupind]);
        if (ipmeta_record_set_add_record(records, recarray[j], 1) != 0) {
          return -1;
        }
      }
    }
  }

  return records->n_recs;
}

int ipmeta_ds_bigarray_lookup_record_single(ipmeta_ds_t *ds, uint32_t addr,
                                            uint32_t providermask,
                                            ipmeta_record_set_t *found)
{
  ipmeta_record_t **recarray;
  int i;
  uint64_t lookupind, arrayind;

  arrayind = ntohl(addr);
  for (i = 0; i < IPMETA_PROVIDER_MAX; i++) {
    if (((1 << (i)) & providermask) == 0) {
      continue;
    }
    lookupind = LOOKUPINDEX(arrayind, i + 1);
    if (lookupind == 0) {
      continue;
    }
    recarray = (ipmeta_record_t **)(STATE(ds)->lookup_table[lookupind]);
    if (ipmeta_record_set_add_record(found, recarray[i], 1) != 0) {
      return -1;
    }
  }
  return found->n_recs;
}

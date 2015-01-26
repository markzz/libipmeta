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

#include <assert.h>
#include <arpa/inet.h>

#include "utils.h"

#include "libipmeta_int.h"
#include "ipmeta_ds_bigarray.h"

#define DS_NAME "bigarray"

#define STATE(ds)				\
  (IPMETA_DS_STATE(bigarray, ds))

static ipmeta_ds_t ipmeta_ds_bigarray = {
  IPMETA_DS_BIGARRAY,
  DS_NAME,
  IPMETA_DS_GENERATE_PTRS(bigarray)
  NULL
};

KHASH_INIT(u32u32, uint32_t, uint32_t, 1, kh_int_hash_func, kh_int_hash_equal)

typedef struct ipmeta_ds_bigarray_state
{
  /** Temporary hash to map from record id to lookup id */
  khash_t(u32u32) *record_lookup;

  /** Mapping from a uint32 lookup id to a record.
   * @note, 0 is a reserved ID (indicates empty)
   */
  ipmeta_record_t **lookup_table;

  /** Number of records in the lookup table */
  int lookup_table_cnt;

  /** Mapping from IP address to uint32 lookup id (see lookup table) */
  uint32_t array[UINT32_MAX];
} ipmeta_ds_bigarray_state_t;

ipmeta_ds_t *ipmeta_ds_bigarray_alloc()
{
  return &ipmeta_ds_bigarray;
}

int ipmeta_ds_bigarray_init(ipmeta_ds_t *ds)
{
  /* the ds structure is malloc'd already, we just need to init the state */

  assert(STATE(ds) == NULL);

  if((ds->state = malloc_zero(sizeof(ipmeta_ds_bigarray_state_t)))
     == NULL)
    {
      ipmeta_log(__func__, "could not malloc bigarray state");
      return -1;
    }

  /** NEVER support IPv6 :) */

  if((STATE(ds)->lookup_table = malloc_zero(sizeof(ipmeta_record_t*))) == NULL)
    {
      return -1;
    }
  STATE(ds)->lookup_table_cnt = 1;

  STATE(ds)->record_lookup = kh_init(u32u32);

  return 0;
}

void ipmeta_ds_bigarray_free(ipmeta_ds_t *ds)
{
  if(ds == NULL)
    {
      return;
    }

  if(STATE(ds) != NULL)
    {
      if(STATE(ds)->lookup_table != NULL)
	{
	  free(STATE(ds)->lookup_table);
	  STATE(ds)->lookup_table = NULL;
	}

      if(STATE(ds)->record_lookup != NULL)
	{
	  kh_destroy(u32u32, STATE(ds)->record_lookup);
	  STATE(ds)->record_lookup = NULL;
	}

      free(STATE(ds));
      ds->state = NULL;
    }

  free(ds);

  return;
}

int ipmeta_ds_bigarray_add_prefix(ipmeta_ds_t *ds,
				  uint32_t addr, uint8_t mask,
				  ipmeta_record_t *record)
{
  assert(ds != NULL && STATE(ds) != NULL);
  ipmeta_ds_bigarray_state_t *state = STATE(ds);

  uint32_t first_addr = ntohl(addr) & (~0UL << (32-mask));
  uint64_t i;
  uint32_t lookup_id;
  khiter_t khiter;
  int khret;

  /* check if this record is already in the record_lookup hash */
  if((khiter = kh_get(u32u32, state->record_lookup, record->id)) ==
     kh_end(state->record_lookup))
    {
      /* allocate the next id in the actual lookup table */

      /* check if we have run out of space */
      if(state->lookup_table_cnt == UINT32_MAX-1)
	{
	  ipmeta_log(__func__,
		     "The Big Array datastructure only supports 2^32 records");
	  return -1;
	}

      /* realloc the lookup table for this record */
      if((state->lookup_table =
	  realloc(state->lookup_table,
		  sizeof(ipmeta_record_t*) * (state->lookup_table_cnt+1))
	  ) == NULL)
	{
	  return -1;
	}

      lookup_id = state->lookup_table_cnt;
      /* move on to the next lookup id */
      state->lookup_table_cnt++;

      /* store this record in the lookup table */
      state->lookup_table[lookup_id] = record;

      /* associate this record id with this lookup id */
      khiter = kh_put(u32u32, state->record_lookup, record->id, &khret);
      kh_value(state->record_lookup, khiter) = lookup_id;
    }
  else
    {
      lookup_id = kh_value(state->record_lookup, khiter);
    }

  /* iterate over all ips in this prefix and point them to this index in the
     table */
  for(i=first_addr; i < ((uint64_t)first_addr + (1 << (32 - mask))); i++)
    {
      state->array[i] = lookup_id;
    }

  return 0;
}

int ipmeta_ds_bigarray_lookup_records(ipmeta_ds_t *ds,
                                      uint32_t addr, uint8_t mask,
                                      ipmeta_record_set_t *records)
{
  assert(ds != NULL && ds->state != NULL);

  uint64_t total_ips = 1 << (32-mask);
  uint64_t i;
  ipmeta_record_t *rec;

  ipmeta_record_set_clear_records(records);

  /* Optimization for single IP special case (no hashing required) */
  if(total_ips == 1)
    {
      if((rec = STATE(ds)->lookup_table[STATE(ds)->array[ntohl(addr)]])!=NULL)
        {
          ipmeta_record_set_add_record(records, rec, 1);
          return 1;
        }
      return 0;
    }

  for(i=0; i<total_ips; i++)
    {
      if ((rec = STATE(ds)->lookup_table[STATE(ds)->array[ntohl(addr)+i]])==NULL)
        {
          /* No match */
          continue;
        }

      /* This has HORRIBLE performance. Never use bigarray for prefixes! */
      if(ipmeta_record_set_add_record(records, rec, 1) != 0)
        {
          return -1;
        }
    }

  return records->n_recs;
}

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

#include "ipmeta_ds_intervaltree.h"
#include "ipmeta_ds_bigarray.h"
#include "ipmeta_ds_patricia.h"
#include "utils.h"

#include "ipmeta_ds.h"
#include "ipmeta_provider.h"

/** Convenience typedef for the alloc function type */
typedef ipmeta_ds_t *(*ds_alloc_func_t)();

/** Array of datastructure allocation functions.
 *
 * @note the indexes of these functions must exactly match the ID in
 * ipmeta_ds_id_t. The element at index 0 MUST be NULL.
 */
static const ds_alloc_func_t ds_alloc_functions[] = {
  NULL, ipmeta_ds_patricia_alloc, ipmeta_ds_bigarray_alloc,
  ipmeta_ds_intervaltree_alloc};

int ipmeta_ds_init(struct ipmeta_ds **ds, ipmeta_ds_id_t ds_id)
{
  assert(ARR_CNT(ds_alloc_functions) == IPMETA_DS_MAX + 1);
  assert(ds_id > 0 && ds_id <= IPMETA_DS_MAX);

  /* malloc some room for the datastructure */
  if ((*ds = malloc_zero(sizeof(ipmeta_ds_t))) == NULL) {
    ipmeta_log(__func__, "could not malloc ipmeta_ds_t");
    return -1;
  }

  /* allocate the datastructure */
  memcpy(*ds, ds_alloc_functions[ds_id](), sizeof(ipmeta_ds_t));

  assert(*ds != NULL);

  /** init the ds */
  if ((*ds)->init(*ds) != 0) {
    return -1;
  }

  return 0;
}

int ipmeta_ds_init_by_name(struct ipmeta_ds **ds, const char *name)
{
  int i;
  ipmeta_ds_t *tmp_ds;

  /* call each of the ds alloc functions and look for a name that matches the
     one we were given, then call the regular ds_init function for that ds */

  for (i = 1; i < ARR_CNT(ds_alloc_functions); i++) {
    tmp_ds = ds_alloc_functions[i]();
    assert(tmp_ds != NULL);

    if (strcmp(tmp_ds->name, name) == 0) {
      return ipmeta_ds_init(ds, i);
    }
  }

  /* no matching datastructure */
  return -1;
}

const char **ipmeta_ds_get_all()
{
  const char **names;
  int i;
  ipmeta_ds_t *tmp_ds;

  if ((names = malloc(sizeof(char *) * IPMETA_DS_MAX)) == NULL) {
    return NULL;
  }

  for (i = 1; i < ARR_CNT(ds_alloc_functions); i++) {
    tmp_ds = ds_alloc_functions[i]();
    assert(tmp_ds != NULL);

    names[i - 1] = tmp_ds->name;
  }

  return names;
}

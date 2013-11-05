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


#ifndef __LIBIPMETA_H
#define __LIBIPMETA_H

/** @file
 *
 * @brief Header file that exposes the public interface of libipmeta.
 *
 * @author Alistair King
 *
 */

/**
 * @name Public Opaque Data Structures
 *
 * @{ */

/** Opaque struct holding ipmeta state */
typedef struct ipmeta ipmeta_t;

/** Opaque struct holding state for a metadata provider */
typedef struct ipmeta_provider ipmeta_provider_t;

/** Opaque struct holding state for a metadata ds */
typedef struct ipmeta_ds ipmeta_ds_t;

/** @} */

/**
 * @name Public Data Structures
 *
 * @{ */

/** Structure which contains an IP meta-data record */
/** @todo use some sort of key-value record so that we don't have to extend
    this structure whenever a new provider is added */
typedef struct ipmeta_record
{
  /** A unique ID for this record (used to join the Blocks and Locations Files)
   *
   * This should be considered unique only within a single provider type
   * i.e. id's may not be unique across different ipmeta_provider_t objects
   */
  uint32_t id;

  /** 2 character string which holds the ISO2 country code */
  char country_code[3];

  /** Continent Code */
  int continent_code;

  /** 2 character string which represents the region the city is in */
  char region[3];

  /** String which contains the city name */
  char *city;

  /** String which contains the postal code
   * @note This cannot be an int as some countries (I'm looking at you, Canada)
   * use characters
   */
  char *post_code;

  /** Latitude of the city */
  double latitude;

  /** Longitude of the city */
  double longitude;

  /** Metro code */
  uint32_t metro_code;

  /** Area code */
  uint32_t area_code;

  /** Connection Speed/Type */
  char *conn_speed;

  /** Array of Autonomous System Numbers */
  uint32_t *asn;

  /** Number of ASNs in the asn array */
  int asn_cnt;

  /** Number of IP addresses that this ASN (or ASN group) 'owns' */
  uint32_t asn_ip_cnt;

  /* -- ADD NEW FIELDS ABOVE HERE -- */

  /** The next record in the list */
  struct ipmeta_record *next;

} ipmeta_record_t;

/** @} */

/**
 * @name Public Enums
 *
 * @{ */

/** Should this provider be set to be the default metadata provider */
typedef enum ipmeta_provider_default
  {
    /** This provider should *not* be the default geolocation result */
    IPMETA_PROVIDER_DEFAULT_NO   = 0,

    /** This provider should be the default geolocation result */
    IPMETA_PROVIDER_DEFAULT_YES  = 1,

  } ipmeta_provider_default_t;

/** A unique identifier for each metadata provider that libipmeta supports
 *
 * @note Remember to add the provider name to provider_names in
 * ipmeta_providers.c when you add a new provider ID below
 */
/** @todo move the provider names into an array matched by their ID */
typedef enum ipmeta_provider_id
  {
    /** Geolocation data from Maxmind (Geo or GeoLite) */
    IPMETA_PROVIDER_MAXMIND      =  1,

    /** Geolocation data from Net Acuity Edge */
    IPMETA_PROVIDER_NETACQ_EDGE  =  2,

    /** ASN data from CAIDA pfx2as */
    IPMETA_PROVIDER_PFX2AS       = 3,

    /** Highest numbered metadata provider ID */
    IPMETA_PROVIDER_MAX          = IPMETA_PROVIDER_PFX2AS,

  } ipmeta_provider_id_t;

/** A unique identifier for each metadata ds that libipmeta supports.
 *
 * @note When adding a datastructure to this list, there must also be a
 * corresponding entry added to the ds_alloc_functions array in ipmeta_ds.c
 */
typedef enum ipmeta_ds_id
  {
    /** Patricia Trie */
    IPMETA_DS_PATRICIA      =  1,

    /** @todo add Huge Array implementation */

    /** Highest numbered ds ID */
    IPMETA_DS_MAX          = IPMETA_DS_PATRICIA,

    /** Default Geolocation data-structure */
    IPMETA_DS_DEFAULT      = IPMETA_DS_PATRICIA,

  } ipmeta_ds_id_t;

/** @} */

/** Get the provider name for the given ID
 *
 * @param id            The provider ID to retrieve the name for
 * @return the name of the provider, NULL if an invalid ID was provided
 */
const char *ipmeta_get_provider_name(ipmeta_provider_id_t id);

/** Get an array of provider names
 *
 * @return an array of provider names
 *
 * @note the number of elements in the array will be exactly
 * IPMETA_PROVIDER_MAX+1. The [0] element will be NULL.
 */
const char **ipmeta_get_provider_names();

/** @todo many of these functions will need to be moved into the libipmeta_int
 * header because they will only be called by provider plugins.  At this point
 * we will need to come up with a nice clean interface that Corsaro and tools
 * will use.
 */

/** Allocate a metadata provider object in the packet state
 *
 * @param ipmeta        The ipmeta object to alloc the provider for
 * @param provider_id   The unique ID of the metadata provider
 * @param ds_id         The type of datastructure to use
 * @param set_default   Set this provider as the default
 * @return the provider object created, NULL if an error occurred
 *
 * @todo replace this function with a standard per-provider init pointer similar
 * to how corsaro plugins work so that we can automatically instantiate all
 * available metadata providers.
 *
 * Plugins which implement a metadata provider should call this function
 * inside their init_output function to allocate a provider object
 *
 * @note Default provider status overrides the requests of previous
 * plugins. Thus, the order in which users request the plugins to be run in can
 * have an effect on plugins which make use of the default provider
 * (e.g. corsaro_report).
 */
ipmeta_provider_t *ipmeta_init_provider(ipmeta_t *ipmeta,
					ipmeta_provider_id_t provider_id,
					ipmeta_ds_id_t ds_id,
					ipmeta_provider_default_t set_default);

/** Free the given metadata provider object
 *
 * @param ipmeta          The ipmeta object to remove the provider from
 * @param provider        The metadata provider object to free
 *
 * @note if this provider was the default, there will be *no* default provider set
 * after this function returns
 */
void ipmeta_free_provider(ipmeta_t *ipmeta,
			  ipmeta_provider_t *provider);

/** Allocate an empty metadata record for the given id
 *
 * @param provider      The metadata provider to associate the record with
 * @param id            The id to use to inialize the record
 * @return the new metadata record, NULL if an error occurred
 *
 * @note Most metadata providers will not want to allocate a record on the fly
 * for every lookup, instead they will allocate all needed records at init time,
 * and then use ipmeta_provider_add_record to add the appropriate record to the
 * results structure. These records are stored in the provider, and free'd when
 * ipmeta_free_provider is called. Also *ALL* char pointers in this structure
 * will be free'd.
 */
ipmeta_record_t *ipmeta_init_record(ipmeta_provider_t *provider,
				    uint32_t id);

/** Get the metadata record for the given id
 *
 * @param provider      The metadata provider to retrieve the record from
 * @param id            The id of the record to retrieve
 * @return the corresponding metadata record, NULL if an error occurred
 */
ipmeta_record_t *ipmeta_get_record(ipmeta_provider_t *provider,
				   uint32_t id);

/** Get an array of all the metadata records registered with the given
 *  provider
 *
 * @param provider      The metadata provider to retrieve the records from
 * @param[out] records  Returns an array of metadata records
 * @return the number of records in the array, -1 if an error occurs
 *
 * @note This function allocates and populates the array dynamically, so do not
 * call repeatedly. Also, it is the caller's responsibility to free the array.
 * DO NOT free the records contained in the array.
 */
int ipmeta_get_all_records(ipmeta_provider_t *provider,
			   ipmeta_record_t ***records);


/** Register a new prefix to record mapping for the given provider
 *
 * @param ipmeta        The ipmeta object associated with the provider
 * @param provider      The provider to register the mapping with
 * @param addr          The network byte-ordered component of the prefix
 * @param mask          The mask component of the prefix
 * @param record        The record to associate with the prefix
 * @return 0 if the prefix is successfully associated with the prefix, -1 if an
 * error occurs
 */
int ipmeta_provider_associate_record(ipmeta_provider_t *provider,
				     uint32_t addr,
				     uint8_t mask,
				     ipmeta_record_t *record);

/** Look up the given address in the provider's datastructure
 *
 * @param ipmeta        The ipmeta object associated with the provider
 * @param provider      The provider to perform the lookup with
 * @param addr          The address to retrieve the record for
                        (network byte ordering)
 * @return the record which best matches the address, NULL if no record is found
 */
ipmeta_record_t *ipmeta_provider_lookup_record(ipmeta_provider_t *provider,
					       uint32_t addr);

/** Remove all the existing records from the given metadata provider
 *
 * @param provider        The metadata provider to clear records for
 * @return the number of records cleared, -1 if an error occurs
 *
 * @note Typically this will be called by a metadata provider for each lookup,
 * before it calls ipmeta_provider_add_record to add the appropriate record
 */
int ipmeta_provider_clear(ipmeta_provider_t *provider);

/** Add the given metadata record to the head of the given metadata provider
 * results list
 *
 * @param provider      The metadata provider object to add the record to
 * @param record        The metadata record to add
 *
 * @note This function can be called multiple times to add multiple records to
 * the provider object. For example, there may be multiple ASes which a packet
 * could belong to.
 *
 * @warning with great power comes great responsibility. If you add a single
 * record more than once, it will cause a loop in the record list. Be careful.
 */
void ipmeta_provider_add_record(ipmeta_provider_t *provider,
				ipmeta_record_t *record);

/** Retrieve the provider object for the default metadata provider
 *
 * @param ipmeta       The ipmeta object to retrieve the provider object from
 * @return the provider object for the default provider, NULL if there is no
 * default provider
 */
ipmeta_provider_t *ipmeta_get_default(ipmeta_t *ipmeta);

/** Retrieve the provider object for the given provider ID
 *
 * @param ipmeta        The ipmeta object to retrieve the provider object from
 * @param id            The metadata provider ID to retrieve
 * @return the provider object for the given ID, NULL if there are no matches
 */
ipmeta_provider_t *ipmeta_get_by_id(ipmeta_t *ipmeta,
				    ipmeta_provider_id_t id);

/** Retrieve the provider object for the given provider name
 *
 * @param ipmeta        The ipmeta object to retrieve the provider object from
 * @param name          The metadata provider name to retrieve
 * @return the provider object for the given name, NULL if there are no matches
 */
ipmeta_provider_t *ipmeta_get_by_name(ipmeta_t *ipmeta, const char *name);

/** Retrieve the next metadata provider record in the list
 *
 * @param provider        The metadata provider to get the next record for
 * @param record          The current record
 * @return the record which follows the current record, NULL if the end of the
 * record list has been reached. If record is NULL, the first record will be
 * returned.
 */
ipmeta_record_t *ipmeta_next_record(ipmeta_provider_t *provider,
				    ipmeta_record_t *record);

/** Dump the given metadata record to stdout (for debugging)
 *
 * @param record        The record to dump
 */
void ipmeta_dump_record(ipmeta_record_t *record);

/**
 * @name Provider-specific helper functions
 *
 * These are class functions that can be used to retrieve meta-data about
 * specific providers
 *
 * @todo move into the appropriate provider class
 *
 * @{ */

/** Get the ISO-3166-1 2 character country code for the given maxmind country id
 *
 * @param country_id     The maxmind country id to get the code for
 * @return the 2 character country code
 */
const char *ipmeta_geo_get_maxmind_iso2(int country_id);

/** Get a list of all possible ISO-3166-1 2 character country codes that maxmind
 *  uses
 *
 * @param[out] countries     Returns a pointer to an array of country codes
 * @return the number of countries in the array
 */
int ipmeta_geo_get_maxmind_iso2_list(const char ***countries);

/** Get the ISO-3166-1 3 character country code for the given maxmind country id
 *
 * @param country_id     The maxmind country id to get the code for
 * @return  the 3 character country code
 */
const char *ipmeta_geo_get_maxmind_iso3(int country_id);

/** Get a list of all possible ISO-3166-1 3 character country codes that maxmind
 *  uses
 *
 * @param[out] countries     Returns a pointer to an array of country codes
 * @return the number of countries in the array
 */
int ipmeta_geo_get_maxmind_iso3_list(const char ***countries);

/** Get the country name for the given maxmind country id
 *
 * @param country_id     The maxmind country id to get the name for
 * @return the name of the country
 */
const char *ipmeta_geo_get_maxmind_country_name(int country_id);

/** Get a list of all possible country names that maxmind uses
 *
 * @param[out] countries     Returns a pointer to an array of country codes
 * @return the number of countries in the array
 */
int ipmeta_geo_get_maxmind_country_name_list(const char ***countries);

/** Get the continent code for the given maxmind country id
 *
 * @param country_id     The maxmind country id to get the continent for
 * @return the 2 character name of the continent
 */
const char *ipmeta_geo_get_maxmind_continent(int country_id);

/** Get a mapping of continent codes that maxmind uses
 *
 * @param[out] continents     Returns a pointer to an array of continent codes
 * @return the number of countries in the array
 *
 * @note The returned array should be used to map from the country array to
 * continents
 */
int ipmeta_geo_get_maxmind_country_continent_list(const char ***continents);

/** @todo Add support for Net Acuity countries */

/** @} */

#endif /* __LIBIPMETA_H */

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

#include "libipmeta_int.h"
#include "config.h"

#include <arpa/inet.h>
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "wandio.h"

#include "khash.h"
#include "utils.h"
#include "csv.h"
#include "ip_utils.h"

#include "ipmeta_ds.h"
#include "ipmeta_provider_netacq_edge.h"

#define PROVIDER_NAME "netacq-edge"

#define STATE(provname) (IPMETA_PROVIDER_STATE(netacq_edge, provname))

#define BUFFER_LEN 1024

#define POLYGON_FILE_CNT_MAX 8 /* increase as you like */

/** The basic fields that every instance of this provider have in common */
static ipmeta_provider_t ipmeta_provider_netacq_edge = {
  IPMETA_PROVIDER_NETACQ_EDGE, PROVIDER_NAME,
  IPMETA_PROVIDER_GENERATE_PTRS(netacq_edge)};

/** Maps a single Net Acuity location id to a single Polygon id */
typedef struct na_to_polygon {
  /** Net Acuity location id */
  uint32_t na_loc_id;

  /** Polygon ids (index <==> polygon table id) */
  uint32_t polygon_ids[POLYGON_FILE_CNT_MAX];

} na_to_polygon_t;

/** Holds the state for an instance of this provider */
typedef struct ipmeta_provider_netacq_edge_state {

  /* info extracted from args */
  char *locations_file;
  char *blocks_file;
  char *region_file;
  char *country_file;
  char *polygon_files[POLYGON_FILE_CNT_MAX];
  int polygon_files_cnt;
  char *na_to_polygon_file;

  /* array of region decode info */
  ipmeta_provider_netacq_edge_region_t **regions;
  int regions_cnt;

  /* array of country decode info */
  ipmeta_provider_netacq_edge_country_t **countries;
  int countries_cnt;

  /* array of polygon decode info */
  ipmeta_polygon_table_t **polygon_tables;
  int polygon_tables_cnt;

  /* temp mapping array of netacq2polygon info (one per locid) */
  na_to_polygon_t **na_to_polygons;
  int na_to_polygons_cnt;

  /* State for CSV parser */
  struct csv_parser parser;
  int current_line;
  int current_column;
  ipmeta_record_t tmp_record;
  uint32_t block_id;
  ip_prefix_t block_lower;
  ip_prefix_t block_upper;
  ipmeta_provider_netacq_edge_region_t tmp_region;
  int tmp_region_ignore;
  ipmeta_provider_netacq_edge_country_t tmp_country;
  int tmp_country_ignore;
  ipmeta_polygon_t tmp_polygon; /* to be inserted in the current poly table */
  na_to_polygon_t tmp_na_to_polygon;
  int tmp_na_col_to_tbl[POLYGON_FILE_CNT_MAX];

} ipmeta_provider_netacq_edge_state_t;

/** Provides a mapping from the integer continent code to the 2 character
    strings that we use in libipmeta */
static const char *continent_strings[] = {
  "??", /* 0 */
  "AF", /* 1 */
  "AN", /* 2 */
  "OC", /* 3 */
  "AS", /* 4 */
  "EU", /* 5 */
  "NA", /* 6 */
  "SA", /* 7 */
};

#define CONTINENT_MAX 7

/** The columns in the netacq_edge locations CSV file */
typedef enum locations_cols {
  /** ID */
  LOCATION_COL_ID = 0,
  /** 2 Char Country Code */
  LOCATION_COL_CC = 1,
  /** Region String */
  LOCATION_COL_REGION = 2,
  /** City String */
  LOCATION_COL_CITY = 3,
  /** Postal Code */
  LOCATION_COL_POSTAL = 4,
  /** Latitude */
  LOCATION_COL_LAT = 5,
  /** Longitude */
  LOCATION_COL_LONG = 6,
  /** Metro Code */
  LOCATION_COL_METRO = 7,
  /** Area Codes (plural) */
  LOCATION_COL_AREACODES = 8, /* not used */
  /** 3 Char Country Code */
  LOCATION_COL_CC3 = 9, /* not used */
  /** Country Code */
  LOCATION_COL_CNTRYCODE = 10, /* not used */
  /** Region Code */
  LOCATION_COL_RCODE = 11,
  /** City Code */
  LOCATION_COL_CITYCODE = 12, /* not used */
  /** Continent Code */
  LOCATION_COL_CONTCODE = 13,
  /** Internal Code */
  LOCATION_COL_INTERNAL = 14, /* not used */
  /** Connection Speed String */
  LOCATION_COL_CONN = 15,
  /** Country-Conf ?? */
  LOCATION_COL_CNTRYCONF = 16, /* not used */
  /** Region-Conf ?? */
  LOCATION_COL_REGCONF = 17, /* not used */
  /** City-Conf ?? */
  LOCATION_COL_CITYCONF = 18, /* not used */
  /** Postal-Conf ?? */
  LOCATION_COL_POSTCONF = 19, /* not used */
  /** GMT-Offset */
  LOCATION_COL_GMTOFF = 20, /* not used */
  /** In CST */
  LOCATION_COL_INDST = 21, /* not used */

  /** Total number of columns in the locations table */
  LOCATION_COL_COUNT = 22
} locations_cols_t;

/** The columns in the netacq_edge locations CSV file */
typedef enum blocks_cols {
  /** Range Start IP */
  BLOCKS_COL_STARTIP = 0,
  /** Range End IP */
  BLOCKS_COL_ENDIP = 1,
  /** ID */
  BLOCKS_COL_ID = 2,

  /** Total number of columns in blocks table */
  BLOCKS_COL_COUNT = 3
} blocks_cols_t;

/** The columns in the netacq region decode CSV file */
typedef enum region_cols {
  /** Country */
  REGION_COL_COUNTRY = 0,
  /** Region */
  REGION_COL_REGION = 1,
  /** Description */
  REGION_COL_DESC = 2,
  /** Region Code */
  REGION_COL_CODE = 3,

  /** Total number of columns in region file */
  REGION_COL_COUNT = 4
} region_cols_t;

/** The columns in the netacq country decode CSV file */
typedef enum country_cols {
  /** ISO 3 char */
  COUNTRY_COL_ISO3 = 0,
  /** ISO 2 char */
  COUNTRY_COL_ISO2 = 1,
  /** Name */
  COUNTRY_COL_NAME = 2,
  /** Is region info? */
  COUNTRY_COL_REGIONS = 3,
  /** Continent code */
  COUNTRY_COL_CONTCODE = 4,
  /** Continent name-abbr */
  COUNTRY_COL_CONTNAME = 5,
  /** Country code (internal) */
  COUNTRY_COL_CODE = 6,

  /** Total number of columns in country file */
  COUNTRY_COL_COUNT = 7
} country_cols_t;

/** The columns in the polygon decode CSV file */
typedef enum polygon_cols {
  /** id */
  POLYGON_COL_ID = 0,
  /** FQ-ID */
  POLYGON_COL_FQID = 1,
  /** Name */
  POLYGON_COL_NAME = 2,
  /* User ID */
  POLYGON_COL_USERCODE = 3,

  /** Total number of columns in polygon decode file */
  POLYGON_COL_COUNT = 4
} polygon_cols_t;

/** The columns in the netacq2polygon mapping CSV file */
typedef enum na_to_polygon_cols {
  /** netacq location id */
  NA_TO_POLYGON_COL_NETACQ_LOC_ID = 0,

  /** Plus an arbitrary number of other polygon id columns */
} na_to_polygon_cols_t;

/** The number of header rows in the netacq_edge CSV files */
#define HEADER_ROW_CNT 1

/** Print usage information to stderr */
static void usage(ipmeta_provider_t *provider)
{
  fprintf(stderr,
          "provider usage: %s -l locations -b blocks\n"
          "       -b            blocks file (must be used with -l)\n"
          "       -c            country decode file\n",
          provider->name);

  fprintf(stderr,
          "       -l            locations file (must be used with -b)\n"
          "       -r            region decode file\n"
          "       -p            netacq2polygon mapping file\n"
          "       -t            polygon table file\n"
          "                       (can be used up to %d times to specify "
          "multiple tables)\n",
          POLYGON_FILE_CNT_MAX);
}

/** Parse the arguments given to the provider
 * @todo add option to choose datastructure
 */
static int parse_args(ipmeta_provider_t *provider, int argc, char **argv)
{
  ipmeta_provider_netacq_edge_state_t *state = STATE(provider);
  int opt;

  /* no args */
  if (argc == 0) {
    usage(provider);
    return -1;
  }

  /* NB: remember to reset optind to 1 before using getopt! */
  optind = 1;

  /* remember the argv strings DO NOT belong to us */

  while ((opt = getopt(argc, argv, "b:c:D:l:r:p:t:?")) >= 0) {
    switch (opt) {
    case 'b':
      state->blocks_file = strdup(optarg);
      break;

    case 'c':
      state->country_file = strdup(optarg);
      break;

    case 'D':
      fprintf(
        stderr,
        "WARNING: -D option is no longer supported by individual providers.\n");
      break;

    case 'l':
      state->locations_file = strdup(optarg);
      break;

    case 'r':
      state->region_file = strdup(optarg);
      break;

    case 'p':
      state->na_to_polygon_file = strdup(optarg);
      break;

    case 't':
      state->polygon_files[state->polygon_files_cnt++] = strdup(optarg);
      break;

    case '?':
    case ':':
    default:
      usage(provider);
      return -1;
    }
  }

  if (state->locations_file == NULL || state->blocks_file == NULL) {
    fprintf(stderr, "ERROR: %s requires both '-b' and '-l'\n", provider->name);
    usage(provider);
    return -1;
  }

  return 0;
}

/* Parse a netacq_edge location cell */
static void parse_netacq_edge_location_cell(void *s, size_t i, void *data)
{
  ipmeta_provider_t *provider = (ipmeta_provider_t *)data;
  ipmeta_provider_netacq_edge_state_t *state = STATE(provider);
  ipmeta_record_t *tmp = &(state->tmp_record);
  char *tok = (char *)s;

  uint16_t tmp_continent;

  char *end;

  /* skip the first two lines */
  if (state->current_line < HEADER_ROW_CNT) {
    return;
  }

  /*
    ipmeta_log(__func__, "row: %d, column: %d, tok: %s",
    state->current_line,
    state->current_column,
    tok);
  */

  switch (state->current_column) {
  case LOCATION_COL_ID:
    /* init this record */
    tmp->id = strtoul(tok, &end, 10);
    if (end == tok || *end != '\0' || errno == ERANGE) {
      ipmeta_log(__func__, "Invalid ID Value (%s)", tok);
      state->parser.status = CSV_EUSER;
      return;
    }
    break;

  case LOCATION_COL_CC:
    if (tok == NULL ||
        (strlen(tok) != 2 && (strlen(tok) == 1 && tok[0] != '?'))) {
      ipmeta_log(__func__, "Invalid Country Code (%s)", tok);
      ipmeta_log(__func__, "Invalid Net Acuity Edge Location Column (%d:%d)",
                 state->current_line, state->current_column);
      state->parser.status = CSV_EUSER;
      return;
    }
    /* ugly hax to s/uk/GB/ in country names */
    if (tok[0] == 'u' && tok[1] == 'k') {
      tmp->country_code[0] = 'G';
      tmp->country_code[1] = 'B';
    }
    /* s/ ** /??/ and s/?/??/ */
    else if (tok[0] == '?' || (tok[0] == '*' && tok[1] == '*')) {
      tmp->country_code[0] = '?';
      tmp->country_code[1] = '?';
      /* continent code will be 0 */
    } else {
      tmp->country_code[0] = toupper(tok[0]);
      tmp->country_code[1] = toupper(tok[1]);
    }
    break;

  case LOCATION_COL_REGION:
    if (tok == NULL) {
      ipmeta_log(__func__, "Invalid Region Code (%s)", tok);
      ipmeta_log(__func__, "Invalid Net Acuity Edge Location Column (%d:%d)",
                 state->current_line, state->current_column);
      state->parser.status = CSV_EUSER;
      return;
    }
    /* s/ * /?/g */
    for (i = 0; i < strlen(tok); i++) {
      if (tok[i] == '*') {
        tok[i] = '?';
      }
    }
    if ((tmp->region = strdup(tok)) == NULL) {
      ipmeta_log(__func__, "Region code copy failed (%s)", tok);
      state->parser.status = CSV_EUSER;
      return;
    }
    break;

  case LOCATION_COL_CITY:
    if (tok != NULL) {
      tmp->city = strndup(tok, strlen(tok));
    }
    break;

  case LOCATION_COL_POSTAL:
    tmp->post_code = strndup(tok, strlen(tok));
    break;

  case LOCATION_COL_LAT:
    tmp->latitude = strtof(tok, &end);
    if (end == tok || *end != '\0' || errno == ERANGE) {
      ipmeta_log(__func__, "Invalid Latitude Value (%s)", tok);
      state->parser.status = CSV_EUSER;
      return;
    }
    break;

  case LOCATION_COL_LONG:
    /* longitude */
    tmp->longitude = strtof(tok, &end);
    if (end == tok || *end != '\0' || errno == ERANGE) {
      ipmeta_log(__func__, "Invalid Longitude Value (%s)", tok);
      state->parser.status = CSV_EUSER;
      return;
    }
    break;

  case LOCATION_COL_METRO:
    /* metro code - whatever the heck that is */
    if (tok != NULL) {
      tmp->metro_code = strtol(tok, &end, 10);
      if (end == tok || *end != '\0' || errno == ERANGE) {
        ipmeta_log(__func__, "Invalid Metro Value (%s)", tok);
        state->parser.status = CSV_EUSER;
        return;
      }
    }
    break;

  case LOCATION_COL_AREACODES:
  case LOCATION_COL_CC3:
  case LOCATION_COL_CNTRYCODE:
    break;

  case LOCATION_COL_RCODE:
    tmp->region_code = strtoul(tok, &end, 10);
    if (end == tok || *end != '\0' || errno == ERANGE) {
      ipmeta_log(__func__, "Invalid Region Code (%s)", tok);
      state->parser.status = CSV_EUSER;
      return;
    }
    break;

  case LOCATION_COL_CITYCODE:
    break;

  case LOCATION_COL_CONTCODE:
    if (tok != NULL) {
      tmp_continent = strtoul(tok, &end, 10);
      if (end == tok || *end != '\0' || errno == ERANGE ||
          tmp_continent > CONTINENT_MAX) {
        ipmeta_log(__func__, "Invalid Continent Code Value (%s)", tok);
        state->parser.status = CSV_EUSER;
        return;
      }
      memcpy(tmp->continent_code, continent_strings[tmp_continent], 2);
    }
    break;

  case LOCATION_COL_INTERNAL:
    break;

  case LOCATION_COL_CONN:
    if (tok != NULL) {
      tmp->conn_speed = strndup(tok, strlen(tok));
    }
    break;

  case LOCATION_COL_CNTRYCONF:
  case LOCATION_COL_REGCONF:
  case LOCATION_COL_CITYCONF:
  case LOCATION_COL_POSTCONF:
  case LOCATION_COL_GMTOFF:
  case LOCATION_COL_INDST:
    break;

  default:
    ipmeta_log(__func__, "Invalid Net Acuity Edge Location Column (%d:%d)",
               state->current_line, state->current_column);
    state->parser.status = CSV_EUSER;
    return;
    break;
  }

  /* move on to the next column */
  state->current_column++;
}

/** Handle an end-of-row event from the CSV parser */
static void parse_netacq_edge_location_row(int c, void *data)
{
  ipmeta_provider_t *provider = (ipmeta_provider_t *)data;
  ipmeta_provider_netacq_edge_state_t *state = STATE(provider);
  ipmeta_record_t *record;
  int i;

  /* skip the first two lines */
  if (state->current_line < HEADER_ROW_CNT) {
    state->current_line++;
    return;
  }

  /* at the end of successful row parsing, current_column will be 9 */
  /* make sure we parsed exactly as many columns as we anticipated */
  if (state->current_column != LOCATION_COL_COUNT) {
    ipmeta_log(__func__,
               "ERROR: Expecting %d columns in the locations file, "
               "but actually got %d",
               LOCATION_COL_COUNT, state->current_column);
    state->parser.status = CSV_EUSER;
    return;
  }

  if ((record = ipmeta_provider_init_record(provider, state->tmp_record.id)) ==
      NULL) {
    ipmeta_log(__func__, "ERROR: Could not initialize meta record");
    state->parser.status = CSV_EUSER;
    return;
  }

  state->tmp_record.source = provider->id;
  memcpy(record, &(state->tmp_record), sizeof(ipmeta_record_t));

  /* tag with polygon id, if there is a match in the netacq2polygons table */
  if ((record->id < state->na_to_polygons_cnt) &&
      state->na_to_polygons[record->id] != NULL) {
    if ((record->polygon_ids =
           malloc(sizeof(uint32_t) * state->polygon_tables_cnt)) == NULL) {
      ipmeta_log(__func__, "ERROR: Could not allocate polygon ids array");
      state->parser.status = CSV_EUSER;
      return;
    }

    for (i = 0; i < state->polygon_tables_cnt; i++) {
      record->polygon_ids[i] =
        state->na_to_polygons[record->id]->polygon_ids[i];
    }
    record->polygon_ids_cnt = state->polygon_tables_cnt;
  }

  /* done processing the line */

  /* increment the current line */
  state->current_line++;
  /* reset the current column */
  state->current_column = 0;
  /* reset the temp record */
  memset(&(state->tmp_record), 0, sizeof(ipmeta_record_t));

  return;
}

/** Read a locations file */
static int read_locations(ipmeta_provider_t *provider, io_t *file)
{
  ipmeta_provider_netacq_edge_state_t *state = STATE(provider);

  char buffer[BUFFER_LEN];
  int read = 0;

  /* reset the state variables before we start */
  state->current_column = 0;
  state->current_line = 0;
  memset(&(state->tmp_record), 0, sizeof(ipmeta_record_t));

  /* options for the csv parser */
  int options = CSV_STRICT | CSV_REPALL_NL | CSV_STRICT_FINI | CSV_APPEND_NULL |
                CSV_EMPTY_IS_NULL;

  csv_init(&(state->parser), options);

  while ((read = wandio_read(file, &buffer, BUFFER_LEN)) > 0) {
    if (csv_parse(&(state->parser), buffer, read,
                  parse_netacq_edge_location_cell,
                  parse_netacq_edge_location_row, provider) != read) {
      ipmeta_log(__func__, "Error parsing %s Location file", provider->name);
      ipmeta_log(__func__, "CSV Error: %s",
                 csv_strerror(csv_error(&(state->parser))));
      return -1;
    }
  }

  if (csv_fini(&(state->parser), parse_netacq_edge_location_cell,
               parse_netacq_edge_location_row, provider) != 0) {
    ipmeta_log(__func__, "Error parsing %s Location file", provider->name);
    ipmeta_log(__func__, "CSV Error: %s",
               csv_strerror(csv_error(&(state->parser))));
    return -1;
  }

  csv_free(&(state->parser));

  return 0;
}

/** Parse a blocks cell */
static void parse_blocks_cell(void *s, size_t i, void *data)
{
  ipmeta_provider_t *provider = (ipmeta_provider_t *)data;
  ipmeta_provider_netacq_edge_state_t *state = STATE(provider);
  char *tok = (char *)s;
  char *end;

  /* skip the first lines */
  if (state->current_line < HEADER_ROW_CNT) {
    return;
  }

  switch (state->current_column) {
  case BLOCKS_COL_STARTIP:
    /* start ip */
    state->block_lower.addr = strtoul(tok, &end, 10);
    if (end == tok || *end != '\0' || errno == ERANGE) {
      ipmeta_log(__func__, "Invalid Start IP Value (%s)", tok);
      state->parser.status = CSV_EUSER;
    }
    break;

  case BLOCKS_COL_ENDIP:
    /* end ip */
    state->block_upper.addr = strtoul(tok, &end, 10);
    if (end == tok || *end != '\0' || errno == ERANGE) {
      ipmeta_log(__func__, "Invalid End IP Value (%s)", tok);
      state->parser.status = CSV_EUSER;
    }
    break;

  case BLOCKS_COL_ID:
    /* id */
    state->block_id = strtoul(tok, &end, 10);
    if (end == tok || *end != '\0' || errno == ERANGE) {
      ipmeta_log(__func__, "Invalid ID Value (%s)", tok);
      state->parser.status = CSV_EUSER;
    }
    break;

  default:
    ipmeta_log(__func__, "Invalid Blocks Column (%d:%d)", state->current_line,
               state->current_column);
    state->parser.status = CSV_EUSER;
    break;
  }

  /* move on to the next column */
  state->current_column++;
}

static void parse_blocks_row(int c, void *data)
{
  ipmeta_provider_t *provider = (ipmeta_provider_t *)data;
  ipmeta_provider_netacq_edge_state_t *state = STATE(provider);

  ip_prefix_list_t *pfx_list = NULL;
  ip_prefix_list_t *temp = NULL;
  ipmeta_record_t *record = NULL;

  if (state->current_line < HEADER_ROW_CNT) {
    state->current_line++;
    return;
  }

  /* done processing the line */

  /* make sure we parsed exactly as many columns as we anticipated */
  if (state->current_column != BLOCKS_COL_COUNT) {
    ipmeta_log(__func__,
               "ERROR: Expecting %d columns in the blocks file, "
               "but actually got %d",
               BLOCKS_COL_COUNT, state->current_column);
    state->parser.status = CSV_EUSER;
    return;
  }

  assert(state->block_id > 0);

  /* convert the range to prefixes */
  if (ip_range_to_prefix(state->block_lower, state->block_upper, &pfx_list) !=
      0) {
    ipmeta_log(__func__, "ERROR: Could not convert range to pfxs");
    state->parser.status = CSV_EUSER;
    return;
  }
  assert(pfx_list != NULL);

  /* get the record from the provider */
  if ((record = ipmeta_provider_get_record(provider, state->block_id)) ==
      NULL) {
    ipmeta_log(__func__, "ERROR: Missing record for location %d",
               state->block_id);
    state->parser.status = CSV_EUSER;
    return;
  }

  /* iterate over and add each prefix to the trie */
  while (pfx_list != NULL) {
    if (ipmeta_provider_associate_record(provider, htonl(pfx_list->prefix.addr),
                                         pfx_list->prefix.masklen,
                                         record) != 0) {
      ipmeta_log(__func__, "ERROR: Failed to associate record");
      state->parser.status = CSV_EUSER;
      return;
    }

    /* store this node so we can free it */
    temp = pfx_list;
    /* move on to the next pfx */
    pfx_list = pfx_list->next;
    /* free this node (saves us walking the list twice) */
    free(temp);
  }

  /* increment the current line */
  state->current_line++;
  /* reset the current column */
  state->current_column = 0;
}

/** Read a blocks file  */
static int read_blocks(ipmeta_provider_t *provider, io_t *file)
{
  ipmeta_provider_netacq_edge_state_t *state = STATE(provider);
  char buffer[BUFFER_LEN];
  int read = 0;

  /* reset the state variables before we start */
  state->current_column = 0;
  state->current_line = 0;
  state->block_id = 0;
  state->block_lower.masklen = 32;
  state->block_upper.masklen = 32;

  /* options for the csv parser */
  int options = CSV_STRICT | CSV_REPALL_NL | CSV_STRICT_FINI | CSV_APPEND_NULL |
                CSV_EMPTY_IS_NULL;

  csv_init(&(state->parser), options);

  while ((read = wandio_read(file, &buffer, BUFFER_LEN)) > 0) {
    if (csv_parse(&(state->parser), buffer, read, parse_blocks_cell,
                  parse_blocks_row, provider) != read) {
      ipmeta_log(__func__, "Error parsing Blocks file");
      ipmeta_log(__func__, "CSV Error: %s",
                 csv_strerror(csv_error(&(state->parser))));
      return -1;
    }
  }

  if (csv_fini(&(state->parser), parse_blocks_cell, parse_blocks_row,
               provider) != 0) {
    ipmeta_log(__func__, "Error parsing Netacq_Edge Blocks file");
    ipmeta_log(__func__, "CSV Error: %s",
               csv_strerror(csv_error(&(state->parser))));
    return -1;
  }

  csv_free(&(state->parser));

  return 0;
}

/** Parse a regions cell */
static void parse_regions_cell(void *s, size_t i, void *data)
{
  ipmeta_provider_t *provider = (ipmeta_provider_t *)data;
  ipmeta_provider_netacq_edge_state_t *state = STATE(provider);
  char *tok = (char *)s;
  char *end;

  int j;
  int len;

  /* skip the first lines */
  if (state->current_line < HEADER_ROW_CNT) {
    return;
  }

  switch (state->current_column) {
  case REGION_COL_COUNTRY:
    /* country */
    if (tok == NULL) {
      ipmeta_log(__func__, "Invalid ISO Country Code (%s)", tok);
      ipmeta_log(__func__, "Invalid Net Acuity Region Column (%d:%d)",
                 state->current_line, state->current_column);
      state->parser.status = CSV_EUSER;
      return;
    }
    len = strnlen(tok, 3);
    for (j = 0; j < len; j++) {
      if (tok[j] == '*') {
        tok[j] = '?';
      }
      state->tmp_region.country_iso[j] = toupper(tok[j]);
    }
    state->tmp_region.country_iso[len] = '\0';

    break;

  case REGION_COL_REGION:
    /* region */
    if (tok == NULL) {
      ipmeta_log(__func__, "Invalid ISO Region Code (%s)", tok);
      ipmeta_log(__func__, "Invalid Net Acuity Region Column (%d:%d)",
                 state->current_line, state->current_column);
      state->parser.status = CSV_EUSER;
      return;
    }

    /* remove the ***-? region and the ?-? regions */
    if (tok[0] == '?') {
      state->tmp_region_ignore = 1;
    }

    /* special check for "no region" */
    if (strncmp(tok, "no region", 9) == 0) {
      tok = "???";
    }

    len = strnlen(tok, 3);
    for (j = 0; j < len; j++) {
      if (tok[j] == '*') {
        tok[j] = '?';
      }
      state->tmp_region.region_iso[j] = toupper(tok[j]);
    }
    state->tmp_region.region_iso[len] = '\0';
    break;

  case REGION_COL_DESC:
    /* description */
    if (tok == NULL) {
      ipmeta_log(__func__, "Invalid Description Code (%s)", tok);
      ipmeta_log(__func__, "Invalid Net Acuity Region Column (%d:%d)",
                 state->current_line, state->current_column);
      state->parser.status = CSV_EUSER;
      return;
    }
    state->tmp_region.name = strndup(tok, strlen(tok));
    break;

  case REGION_COL_CODE:
    state->tmp_region.code = strtoul(tok, &end, 10);
    if (end == tok || *end != '\0' || errno == ERANGE) {
      ipmeta_log(__func__, "Invalid Code Value (%s)", tok);
      ipmeta_log(__func__, "Invalid Net Acuity Region Column (%d:%d)",
                 state->current_line, state->current_column);
      state->parser.status = CSV_EUSER;
      return;
    }
    break;

  default:
    ipmeta_log(__func__, "Invalid Regions Column (%d:%d)", state->current_line,
               state->current_column);
    state->parser.status = CSV_EUSER;
    break;
  }

  /* move on to the next column */
  state->current_column++;
}

static void parse_regions_row(int c, void *data)
{
  ipmeta_provider_t *provider = (ipmeta_provider_t *)data;
  ipmeta_provider_netacq_edge_state_t *state = STATE(provider);

  ipmeta_provider_netacq_edge_region_t *region = NULL;

  if (state->current_line < HEADER_ROW_CNT) {
    state->current_line++;
    return;
  }

  /* done processing the line */

  /* make sure we parsed exactly as many columns as we anticipated */
  if (state->current_column != REGION_COL_COUNT) {
    ipmeta_log(__func__,
               "ERROR: Expecting %d columns in the regions file, "
               "but actually got %d",
               REGION_COL_COUNT, state->current_column);
    state->parser.status = CSV_EUSER;
    return;
  }

  if (state->tmp_region_ignore == 0) {
    /* copy the tmp region structure into a new struct */
    if ((region = malloc(sizeof(ipmeta_provider_netacq_edge_region_t))) ==
        NULL) {
      ipmeta_log(__func__, "ERROR: Could not allocate memory for region");
      state->parser.status = CSV_EUSER;
      return;
    }
    memcpy(region, &(state->tmp_region),
           sizeof(ipmeta_provider_netacq_edge_region_t));

    /* make room in the region array for this region */
    if ((state->regions = realloc(
           state->regions, sizeof(ipmeta_provider_netacq_edge_region_t *) *
                             (state->regions_cnt + 1))) == NULL) {
      ipmeta_log(__func__, "ERROR: Could not allocate memory for region array");
      state->parser.status = CSV_EUSER;
      return;
    }
    /* now poke it in */
    state->regions[state->regions_cnt++] = region;
  }

  /* increment the current line */
  state->current_line++;
  /* reset the current column */
  state->current_column = 0;
  /* reset the tmp region info */
  memset(&(state->tmp_region), 0, sizeof(ipmeta_provider_netacq_edge_region_t));
  state->tmp_region_ignore = 0;
}

/** Read a region decode file  */
static int read_regions(ipmeta_provider_t *provider, io_t *file)
{
  ipmeta_provider_netacq_edge_state_t *state = STATE(provider);
  char buffer[BUFFER_LEN];
  int read = 0;

  /* reset the state variables before we start */
  state->current_column = 0;
  state->current_line = 0;
  memset(&(state->tmp_region), 0, sizeof(ipmeta_provider_netacq_edge_region_t));
  state->tmp_region_ignore = 0;

  /* options for the csv parser */
  int options = CSV_STRICT | CSV_REPALL_NL | CSV_STRICT_FINI | CSV_APPEND_NULL |
                CSV_EMPTY_IS_NULL;

  csv_init(&(state->parser), options);

  while ((read = wandio_read(file, &buffer, BUFFER_LEN)) > 0) {
    if (csv_parse(&(state->parser), buffer, read, parse_regions_cell,
                  parse_regions_row, provider) != read) {
      ipmeta_log(__func__, "Error parsing regions file");
      ipmeta_log(__func__, "CSV Error: %s",
                 csv_strerror(csv_error(&(state->parser))));
      return -1;
    }
  }

  if (csv_fini(&(state->parser), parse_regions_cell, parse_regions_row,
               provider) != 0) {
    ipmeta_log(__func__, "Error parsing Netacq_Edge Region file");
    ipmeta_log(__func__, "CSV Error: %s",
               csv_strerror(csv_error(&(state->parser))));
    return -1;
  }

  csv_free(&(state->parser));

  return 0;
}

/** Parse a country cell */
static void parse_country_cell(void *s, size_t i, void *data)
{
  ipmeta_provider_t *provider = (ipmeta_provider_t *)data;
  ipmeta_provider_netacq_edge_state_t *state = STATE(provider);
  char *tok = (char *)s;
  char *end;

  int j;

  /* skip the first lines */
  if (state->current_line < HEADER_ROW_CNT) {
    return;
  }

  switch (state->current_column) {
  case COUNTRY_COL_ISO3:
    /* country 3 char */
    if (tok == NULL) {
      ipmeta_log(__func__, "Invalid ISO-3 Country Code (%s)", tok);
      ipmeta_log(__func__, "Invalid Net Acuity Region Column (%d:%d)",
                 state->current_line, state->current_column);
      state->parser.status = CSV_EUSER;
      return;
    }
    if (tok[0] == '*' && tok[1] == '*' && tok[2] == '*') {
      state->tmp_country.iso3[0] = '?';
      state->tmp_country.iso3[1] = '?';
      state->tmp_country.iso3[2] = '?';
    } else if (tok[0] == '?') {
      /* we have manually deprecated the ? country */
      state->tmp_country_ignore = 1;
    } else {
      assert(strnlen(tok, 3) == 3);
      for (j = 0; j < 3; j++) {
        state->tmp_country.iso3[j] = toupper(tok[j]);
      }
    }
    state->tmp_country.iso3[3] = '\0';
    break;

  case COUNTRY_COL_ISO2:
    /* country 2 char */
    if (tok == NULL) {
      ipmeta_log(__func__, "Invalid ISO-2 Country Code (%s)", tok);
      ipmeta_log(__func__, "Invalid Net Acuity Region Column (%d:%d)",
                 state->current_line, state->current_column);
      state->parser.status = CSV_EUSER;
      return;
    }
    /* ugly hax to s/uk/GB/ in country names */
    if (tok[0] == 'u' && tok[1] == 'k') {
      state->tmp_country.iso2[0] = 'G';
      state->tmp_country.iso2[1] = 'B';
    }
    /* s/ ** / ?? / */
    else if (tok[0] == '*' && tok[1] == '*') {
      state->tmp_country.iso2[0] = '?';
      state->tmp_country.iso2[1] = '?';
    }
    /* ignore ? country (not used, replaced by ??) */
    else if (tok[0] == '?') {
      state->tmp_country_ignore = 1;
    } else {
      state->tmp_country.iso2[0] = toupper(tok[0]);
      state->tmp_country.iso2[1] = toupper(tok[1]);
    }
    state->tmp_country.iso2[2] = '\0';
    break;

  case COUNTRY_COL_NAME:
    /* name */
    if (tok == NULL) {
      ipmeta_log(__func__, "Invalid Country Name (%s)", tok);
      ipmeta_log(__func__, "Invalid Net Acuity Country Column (%d:%d)",
                 state->current_line, state->current_column);
      state->parser.status = CSV_EUSER;
      return;
    }
    state->tmp_country.name = strndup(tok, strlen(tok));
    break;

  case COUNTRY_COL_REGIONS:
    state->tmp_country.regions = strtol(tok, &end, 10);
    if (end == tok || *end != '\0' || errno == ERANGE ||
        (state->tmp_country.regions != 0 && state->tmp_country.regions != 1)) {
      ipmeta_log(__func__, "Invalid Regions Value (%s)", tok);
      ipmeta_log(__func__, "Invalid Net Acuity Country Column (%d:%d)",
                 state->current_line, state->current_column);
      state->parser.status = CSV_EUSER;
      return;
    }
    break;

  case COUNTRY_COL_CONTCODE:
    state->tmp_country.continent_code = strtoul(tok, &end, 10);
    if (end == tok || *end != '\0' || errno == ERANGE) {
      ipmeta_log(__func__, "Invalid Continent Code Value (%s)", tok);
      ipmeta_log(__func__, "Invalid Net Acuity Country Column (%d:%d)",
                 state->current_line, state->current_column);
      state->parser.status = CSV_EUSER;
      return;
    }
    break;

  case COUNTRY_COL_CONTNAME:
    /* continent 2 char*/
    if (tok == NULL || strnlen(tok, 2) != 2) {
      ipmeta_log(__func__, "Invalid 2 char Continent Code (%s)", tok);
      ipmeta_log(__func__, "Invalid Net Acuity Country Column (%d:%d)",
                 state->current_line, state->current_column);
      state->parser.status = CSV_EUSER;
      return;
    }
    /* s/ ** /??/ */
    if (tok[0] == '*' && tok[1] == '*') {
      tok[0] = '?';
      tok[1] = '?';
    }
    /* s/au/oc/ */
    if (tok[0] == 'a' && tok[1] == 'u') {
      tok[0] = 'o';
      tok[1] = 'c';
    }
    state->tmp_country.continent[0] = toupper(tok[0]);
    state->tmp_country.continent[1] = toupper(tok[1]);
    break;

  case COUNTRY_COL_CODE:
    state->tmp_country.code = strtoul(tok, &end, 10);
    if (end == tok || *end != '\0' || errno == ERANGE) {
      ipmeta_log(__func__, "Invalid Code Value (%s)", tok);
      ipmeta_log(__func__, "Invalid Net Acuity Country Column (%d:%d)",
                 state->current_line, state->current_column);
      state->parser.status = CSV_EUSER;
      return;
    }
    break;

  default:
    ipmeta_log(__func__, "Invalid Country Column (%d:%d)", state->current_line,
               state->current_column);
    state->parser.status = CSV_EUSER;
    break;
  }

  /* move on to the next column */
  state->current_column++;
}

static void parse_country_row(int c, void *data)
{
  ipmeta_provider_t *provider = (ipmeta_provider_t *)data;
  ipmeta_provider_netacq_edge_state_t *state = STATE(provider);

  ipmeta_provider_netacq_edge_country_t *country = NULL;

  if (state->current_line < HEADER_ROW_CNT) {
    state->current_line++;
    return;
  }

  /* done processing the line */

  /* make sure we parsed exactly as many columns as we anticipated */
  if (state->current_column != COUNTRY_COL_COUNT) {
    ipmeta_log(__func__,
               "ERROR: Expecting %d columns in the country file, "
               "but actually got %d",
               COUNTRY_COL_COUNT, state->current_column);
    state->parser.status = CSV_EUSER;
    return;
  }

  if (state->tmp_country_ignore == 0) {
    /* copy the tmp country structure into a new struct */
    if ((country = malloc(sizeof(ipmeta_provider_netacq_edge_country_t))) ==
        NULL) {
      ipmeta_log(__func__, "ERROR: Could not allocate memory for country");
      state->parser.status = CSV_EUSER;
      return;
    }
    memcpy(country, &(state->tmp_country),
           sizeof(ipmeta_provider_netacq_edge_country_t));

    /* make room in the country array for this country */
    if ((state->countries = realloc(
           state->countries, sizeof(ipmeta_provider_netacq_edge_country_t *) *
                               (state->countries_cnt + 1))) == NULL) {
      ipmeta_log(__func__,
                 "ERROR: Could not allocate memory for country array");
      state->parser.status = CSV_EUSER;
      return;
    }
    /* now poke it in */
    state->countries[state->countries_cnt++] = country;
  }

  /* increment the current line */
  state->current_line++;
  /* reset the current column */
  state->current_column = 0;
  /* reset the tmp country info */
  memset(&(state->tmp_country), 0,
         sizeof(ipmeta_provider_netacq_edge_country_t));
  state->tmp_country_ignore = 0;
}

/** Read a country decode file  */
static int read_countries(ipmeta_provider_t *provider, io_t *file)
{
  ipmeta_provider_netacq_edge_state_t *state = STATE(provider);
  char buffer[BUFFER_LEN];
  int read = 0;

  /* reset the state variables before we start */
  state->current_column = 0;
  state->current_line = 0;
  memset(&(state->tmp_country), 0,
         sizeof(ipmeta_provider_netacq_edge_country_t));
  state->tmp_country_ignore = 0;

  /* options for the csv parser */
  int options = CSV_STRICT | CSV_REPALL_NL | CSV_STRICT_FINI | CSV_APPEND_NULL |
                CSV_EMPTY_IS_NULL;

  csv_init(&(state->parser), options);

  while ((read = wandio_read(file, &buffer, BUFFER_LEN)) > 0) {
    if (csv_parse(&(state->parser), buffer, read, parse_country_cell,
                  parse_country_row, provider) != read) {
      ipmeta_log(__func__, "Error parsing country file");
      ipmeta_log(__func__, "CSV Error: %s",
                 csv_strerror(csv_error(&(state->parser))));
      return -1;
    }
  }

  if (csv_fini(&(state->parser), parse_country_cell, parse_country_row,
               provider) != 0) {
    ipmeta_log(__func__, "Error parsing Netacq Edge Country file");
    ipmeta_log(__func__, "CSV Error: %s",
               csv_strerror(csv_error(&(state->parser))));
    return -1;
  }

  csv_free(&(state->parser));

  return 0;
}

/* Parse a polygon decode table cell */
static void parse_polygons_cell(void *s, size_t i, void *data)
{
  ipmeta_provider_t *provider = (ipmeta_provider_t *)data;
  ipmeta_provider_netacq_edge_state_t *state = STATE(provider);
  char *tok = (char *)s;
  char *end;
  char *sfx;

  ipmeta_polygon_table_t *new_table;

  /* process the header row, creating polygon table objects */
  if (state->current_line == 0) {
    if (state->current_column == 0) {
      /* create a new polygon table */
      if ((new_table = malloc_zero(sizeof(ipmeta_polygon_table_t))) == NULL) {
        ipmeta_log(__func__, "Cannot allocate polygon table (%s)", tok);
        state->parser.status = CSV_EUSER;
        return;
      }
      /* we extract the table name from this column name */
      new_table->id = state->polygon_tables_cnt;

      /* chop off the -id suffix */
      if ((sfx = strstr(tok, "-id")) != NULL) {
        *sfx = '\0';
      }
      if ((new_table->ascii_id = strdup(tok)) == NULL) {
        ipmeta_log(__func__, "Cannot allocate polygon table name");
        state->parser.status = CSV_EUSER;
        return;
      }

      /* malloc some space in the table array for this table */
      if ((state->polygon_tables =
             realloc(state->polygon_tables,
                     sizeof(ipmeta_polygon_table_t *) *
                       (state->polygon_tables_cnt + 1))) == NULL) {
        ipmeta_log(__func__, "ERROR: Could not allocate polygon table array");
        state->parser.status = CSV_EUSER;
        return;
      }

      state->polygon_tables[state->polygon_tables_cnt++] = new_table;
    }
    /* else, ignore the other column names, we don't care */

    state->current_column++;
    return;
  }

  switch (state->current_column) {
  case POLYGON_COL_ID:
    /* Polygon id */
    state->tmp_polygon.id = strtoul(tok, &end, 10);
    if (end == tok || *end != '\0' || errno == ERANGE) {
      ipmeta_log(__func__, "Invalid Polygon ID Value (%s)", tok);
      state->parser.status = CSV_EUSER;
      return;
    }
    break;
  case POLYGON_COL_NAME:
    /* Polygon name string */
    if ((state->tmp_polygon.name = strdup(tok == NULL ? "" : tok)) == NULL) {
      ipmeta_log(__func__, "Cannot allocate memory for Polygon name");
      state->parser.status = CSV_EUSER;
      return;
    }
    break;
  case POLYGON_COL_FQID:
    /* Fully-Qualified ID */
    if ((state->tmp_polygon.fqid = strdup(tok == NULL ? "" : tok)) == NULL) {
      ipmeta_log(__func__, "Cannot allocate memory for Polygon FQID");
      state->parser.status = CSV_EUSER;
      return;
    }
    break;
  case POLYGON_COL_USERCODE:
    /* User code string */
    if ((state->tmp_polygon.usercode = strdup(tok == NULL ? "" : tok)) ==
        NULL) {
      ipmeta_log(__func__, "Cannot allocate memory for Polygon user code");
      state->parser.status = CSV_EUSER;
      return;
    }
    break;
  default:
    /* Just ignore non-relevant cols */
    break;
  }

  /* move on to the next column */
  state->current_column++;
}

/** Handle an end-of-row event for the polygon decode table*/
static void parse_polygons_row(int c, void *data)
{
  ipmeta_provider_t *provider = (ipmeta_provider_t *)data;
  ipmeta_provider_netacq_edge_state_t *state = STATE(provider);

  ipmeta_polygon_table_t *table = NULL;
  ipmeta_polygon_t *polygon = NULL;

  /* process the header row */
  if (state->current_line == 0) {
    /* all is done by the col parser */
    state->current_column = 0;
    state->current_line++;
    return;
  }

  table = state->polygon_tables[state->polygon_tables_cnt - 1];
  assert(table != NULL);

  /* done processing the line */

  /* make sure we parsed exactly as many columns as we anticipated */
  if (state->current_column != POLYGON_COL_COUNT) {
    ipmeta_log(__func__,
               "ERROR: Expecting %d columns in the polygons file, "
               "but actually got %d",
               POLYGON_COL_COUNT, state->current_column);
    state->parser.status = CSV_EUSER;
    return;
  }

  /* copy the tmp polygon struct into a new one */
  if ((polygon = malloc(sizeof(ipmeta_polygon_t))) == NULL) {
    ipmeta_log(__func__, "ERROR: Could not allocate memory for polygon");
    state->parser.status = CSV_EUSER;
    return;
  }
  memcpy(polygon, &(state->tmp_polygon), sizeof(ipmeta_polygon_t));

  /* make room in the polygons array for this polygon */
  if ((table->polygons =
         realloc(table->polygons, sizeof(ipmeta_polygon_t *) *
                                    (table->polygons_cnt + 1))) == NULL) {
    ipmeta_log(__func__, "ERROR: Could not allocate memory for polygon array");
    state->parser.status = CSV_EUSER;
    return;
  }
  /* now poke it in */
  table->polygons[table->polygons_cnt++] = polygon;

  /* increment the current line */
  state->current_line++;
  /* reset the current column */
  state->current_column = 0;
  /* reset the tmp region info */
  memset(&(state->tmp_polygon), 0, sizeof(ipmeta_polygon_t));
}

/** Read a polygon decode file */
static int read_polygons(ipmeta_provider_t *provider, io_t *file)
{
  ipmeta_provider_netacq_edge_state_t *state = STATE(provider);
  char buffer[BUFFER_LEN];
  int read = 0;

  /* reset the state variables before we start */
  state->current_column = 0;
  state->current_line = 0;
  memset(&(state->tmp_polygon), 0, sizeof(ipmeta_polygon_t));

  /* options for the csv parser */
  int options = CSV_STRICT | CSV_REPALL_NL | CSV_STRICT_FINI | CSV_APPEND_NULL |
                CSV_EMPTY_IS_NULL;

  csv_init(&(state->parser), options);

  while ((read = wandio_read(file, &buffer, BUFFER_LEN)) > 0) {
    if (csv_parse(&(state->parser), buffer, read, parse_polygons_cell,
                  parse_polygons_row, provider) != read) {
      ipmeta_log(__func__, "Error parsing polygon decode file");
      ipmeta_log(__func__, "CSV Error: %s",
                 csv_strerror(csv_error(&(state->parser))));
      return -1;
    }
  }

  if (csv_fini(&(state->parser), parse_polygons_cell, parse_polygons_row,
               provider) != 0) {
    ipmeta_log(__func__, "Error parsing locations to polygons file");
    ipmeta_log(__func__, "CSV Error: %s",
               csv_strerror(csv_error(&(state->parser))));
    return -1;
  }

  csv_free(&(state->parser));

  return 0;
}

/* Parse a netacq2polygon table cell */
static void parse_na_to_polygon_cell(void *s, size_t i, void *data)
{
  ipmeta_provider_t *provider = (ipmeta_provider_t *)data;
  ipmeta_provider_netacq_edge_state_t *state = STATE(provider);
  char *tok = (char *)s;
  char *end;
  char *sfx;
  int found = 0;
  int table_id;

  /* process the first line */
  if (state->current_line == 0) {
    if (state->current_column > 0) {
      /* what table does this refer to? */
      for (i = 0; i < state->polygon_tables_cnt; i++) {
        /* chop off the -id suffix */
        if ((sfx = strstr(tok, "-id")) != NULL) {
          *sfx = '\0';
        }
        if (strcmp(tok, state->polygon_tables[i]->ascii_id) == 0) {
          /* this is it! */
          state->tmp_na_col_to_tbl[state->current_column] = i;
          found = 1;
          break;
        }
      }

      if (found == 0) {
        ipmeta_log(__func__, "Missing Polygon Table for (%s)", tok);
        state->parser.status = CSV_EUSER;
        return;
      }
    }

    state->current_column++;
    return;
  }

  switch (state->current_column) {
  case NA_TO_POLYGON_COL_NETACQ_LOC_ID:
    /* Netacq id */
    if (tok == NULL) {
      ipmeta_log(__func__, "Missing Net Acuity ID Value (%d:%d)",
                 state->current_line, state->current_column);
      state->parser.status = CSV_EUSER;
      return;
    }
    state->tmp_na_to_polygon.na_loc_id = strtoul(tok, &end, 10);
    if (end == tok || *end != '\0' || errno == ERANGE) {
      ipmeta_log(__func__, "Invalid Net Acuity ID Value (%s)", tok);
      state->parser.status = CSV_EUSER;
      return;
    }
    break;
  default:
    table_id = state->tmp_na_col_to_tbl[state->current_column];
    if (tok == NULL) {
      ipmeta_log(__func__, "Missing Polygon ID Value (%d:%d)",
                 state->current_line, state->current_column);
      state->parser.status = CSV_EUSER;
      return;
    }
    state->tmp_na_to_polygon.polygon_ids[table_id] = strtoul(tok, &end, 10);
    if (end == tok || *end != '\0' || errno == ERANGE) {
      ipmeta_log(__func__, "Invalid Polygon ID Value (%s)", tok);
      state->parser.status = CSV_EUSER;
      return;
    }
    break;
  }

  /* move on to the next column */
  state->current_column++;
}

/** Handle an end-of-row event for the netacq2polygon table*/
static void parse_na_to_polygon_row(int c, void *data)
{
  int i;
  ipmeta_provider_t *provider = (ipmeta_provider_t *)data;
  ipmeta_provider_netacq_edge_state_t *state = STATE(provider);

  na_to_polygon_t *n2p = NULL;

  /* process the header row */
  if (state->current_line == 0) {
    /** all work is done in the col parser ? */
    state->current_column = 0;
    state->current_line++;
    return;
  }

  /* done processing the line */

  /* make sure we parsed exactly as many columns as we anticipated */
  if (state->current_column < NA_TO_POLYGON_COL_NETACQ_LOC_ID) {
    ipmeta_log(__func__, "ERROR: Missing Net Acuity location ID column");
    state->parser.status = CSV_EUSER;
    return;
  }

  /* copy the tmp polygon struct into a new one */
  if ((n2p = malloc(sizeof(na_to_polygon_t))) == NULL) {
    ipmeta_log(__func__, "ERROR: Could not allocate memory for polygon");
    state->parser.status = CSV_EUSER;
    return;
  }
  memcpy(n2p, &(state->tmp_na_to_polygon), sizeof(na_to_polygon_t));

  /* if this id would overflow the table, just make the table bigger */
  if ((n2p->na_loc_id + 1) > state->na_to_polygons_cnt) {
    if ((state->na_to_polygons =
           realloc(state->na_to_polygons,
                   sizeof(na_to_polygon_t *) * (n2p->na_loc_id + 1))) == NULL) {
      ipmeta_log(__func__,
                 "ERROR: Could not allocate memory for na2polygon array");
      state->parser.status = CSV_EUSER;
      return;
    }
    /* zero out the newly allocated memory */
    for (i = state->na_to_polygons_cnt; i < n2p->na_loc_id; i++) {
      state->na_to_polygons[i] = NULL;
    }

    state->na_to_polygons_cnt = n2p->na_loc_id + 1;
  } else if (state->na_to_polygons[n2p->na_loc_id] != NULL) {
    /* About to override an already inserted location */
    ipmeta_log(__func__, "ERROR: Duplicate location ID: %d in polygons file",
               n2p->na_loc_id);
    state->parser.status = CSV_EUSER;
    return;
  }

  /* now poke it in */
  state->na_to_polygons[n2p->na_loc_id] = n2p;

  /* increment the current line */
  state->current_line++;
  /* reset the current column */
  state->current_column = 0;
  /* reset the tmp region info */
  memset(&(state->tmp_na_to_polygon), 0, sizeof(na_to_polygon_t));
}

/** Read a netacq2polygon mapping file */
static int read_na_to_polygon(ipmeta_provider_t *provider, io_t *file)
{
  ipmeta_provider_netacq_edge_state_t *state = STATE(provider);
  char buffer[BUFFER_LEN];
  int read = 0;

  /* reset the state variables before we start */
  state->current_column = 0;
  state->current_line = 0;
  memset(&(state->tmp_na_to_polygon), 0, sizeof(na_to_polygon_t));

  /* options for the csv parser */
  int options = CSV_STRICT | CSV_REPALL_NL | CSV_STRICT_FINI | CSV_APPEND_NULL |
                CSV_EMPTY_IS_NULL;

  csv_init(&(state->parser), options);

  while ((read = wandio_read(file, &buffer, BUFFER_LEN)) > 0) {
    if (csv_parse(&(state->parser), buffer, read, parse_na_to_polygon_cell,
                  parse_na_to_polygon_row, provider) != read) {
      ipmeta_log(__func__, "Error parsing netacq2polygon file");
      ipmeta_log(__func__, "CSV Error: %s",
                 csv_strerror(csv_error(&(state->parser))));
      return -1;
    }
  }

  if (csv_fini(&(state->parser), parse_na_to_polygon_cell,
               parse_na_to_polygon_row, provider) != 0) {
    ipmeta_log(__func__, "Error parsing locations to polygons file");
    ipmeta_log(__func__, "CSV Error: %s",
               csv_strerror(csv_error(&(state->parser))));
    return -1;
  }

  csv_free(&(state->parser));

  return 0;
}

static void na_to_polygon_free(ipmeta_provider_netacq_edge_state_t *state)
{
  int i;

  for (i = 0; i < state->na_to_polygons_cnt; i++) {
    free(state->na_to_polygons[i]);
    state->na_to_polygons[i] = NULL;
  }
  free(state->na_to_polygons);
  state->na_to_polygons = NULL;
  state->na_to_polygons_cnt = 0;
}

/* ===== PUBLIC FUNCTIONS BELOW THIS POINT ===== */

ipmeta_provider_t *ipmeta_provider_netacq_edge_alloc()
{
  return &ipmeta_provider_netacq_edge;
}

int ipmeta_provider_netacq_edge_init(ipmeta_provider_t *provider, int argc,
                                     char **argv)
{
  ipmeta_provider_netacq_edge_state_t *state;
  io_t *file = NULL;
  int i;

  /* allocate our state */
  if ((state = malloc_zero(sizeof(ipmeta_provider_netacq_edge_state_t))) ==
      NULL) {
    ipmeta_log(__func__,
               "could not malloc ipmeta_provider_netacq_edge_state_t");
    return -1;
  }
  ipmeta_provider_register_state(provider, state);

  /* parse the command line args */
  if (parse_args(provider, argc, argv) != 0) {
    return -1;
  }

  assert(state->locations_file != NULL && state->blocks_file != NULL);

  /* if provided, open the region decode file and populate the lookup arrays */
  if (state->region_file != NULL) {
    ipmeta_log(__func__, "processing region file (%s)", state->region_file);
    if ((file = wandio_create(state->region_file)) == NULL) {
      ipmeta_log(__func__, "failed to open region decode file '%s'",
                 state->region_file);
      return -1;
    }

    /* populate the arrays! */
    if (read_regions(provider, file) != 0) {
      ipmeta_log(__func__, "failed to parse region decode file");
      goto err;
    }

    /* close it... */
    wandio_destroy(file);
  }

  /* if provided, open the country decode file and populate the lookup arrays */
  if (state->country_file != NULL) {
    ipmeta_log(__func__, "processing country file (%s)", state->country_file);
    if ((file = wandio_create(state->country_file)) == NULL) {
      ipmeta_log(__func__, "failed to open country decode file '%s'",
                 state->country_file);
      return -1;
    }

    /* populate the arrays! */
    if (read_countries(provider, file) != 0) {
      ipmeta_log(__func__, "failed to parse country decode file");
      goto err;
    }

    /* close it... */
    wandio_destroy(file);
  }

  /* open each polygon decode file and populate the lookup arrays */
  for (i = 0; i < state->polygon_files_cnt; i++) {
    assert(state->polygon_files[i] != NULL);
    ipmeta_log(__func__, "processing polygon table (%s)",
               state->polygon_files[i]);
    if ((file = wandio_create(state->polygon_files[i])) == NULL) {
      ipmeta_log(__func__, "failed to open Polygon decode file '%s'",
                 state->polygon_files[i]);
      return -1;
    }

    /* populate the arrays! */
    if (read_polygons(provider, file) != 0) {
      ipmeta_log(__func__, "failed to parse Polygon decode file");
      goto err;
    }

    /* close it... */
    wandio_destroy(file);
  }

  /* if provided, open the netacq2polygon mapping file and populate the
     temporary join table */
  if (state->na_to_polygon_file != NULL) {
    ipmeta_log(__func__, "processing na2poly table (%s)",
               state->na_to_polygon_file);
    if ((file = wandio_create(state->na_to_polygon_file)) == NULL) {
      ipmeta_log(__func__,
                 "failed to open Net Acuity to Polygon mapping file '%s'",
                 state->na_to_polygon_file);
      return -1;
    }

    /* populate the arrays! */
    if (read_na_to_polygon(provider, file) != 0) {
      ipmeta_log(__func__,
                 "failed to parse Net Acuity to Polygon mapping file");
      goto err;
    }

    /* close it... */
    wandio_destroy(file);
  }

  ipmeta_log(__func__, "processing locations file (%s)", state->locations_file);

  /* open the locations file */
  if ((file = wandio_create(state->locations_file)) == NULL) {
    ipmeta_log(__func__, "failed to open location file '%s'",
               state->locations_file);
    return -1;
  }

  /* populate the locations hash */
  if (read_locations(provider, file) != 0) {
    ipmeta_log(__func__, "failed to parse locations file");
    goto err;
  }

  /* close the locations file */
  wandio_destroy(file);
  file = NULL;

  ipmeta_log(__func__, "processing blocks file (%s)", state->blocks_file);

  /* open the blocks file */
  if ((file = wandio_create(state->blocks_file)) == NULL) {
    ipmeta_log(__func__, "failed to open blocks file '%s'", state->blocks_file);
    goto err;
  }

  /* populate the ds (by joining on the id in the hash) */
  if (read_blocks(provider, file) != 0) {
    ipmeta_log(__func__, "failed to parse blocks file");
    goto err;
  }

  /* close the blocks file */
  wandio_destroy(file);

  /* free the netacq 2 polygon temporary mapping table */
  na_to_polygon_free(state);

  /* ready to rock n roll */

  return 0;

err:
  if (file != NULL) {
    wandio_destroy(file);
  }
  usage(provider);
  return -1;
}

void ipmeta_provider_netacq_edge_free(ipmeta_provider_t *provider)
{
  ipmeta_provider_netacq_edge_state_t *state = STATE(provider);
  int i, j;
  ipmeta_polygon_table_t *table;

  if (state != NULL) {
    free(state->locations_file);
    state->locations_file = NULL;

    free(state->blocks_file);
    state->blocks_file = NULL;

    free(state->region_file);
    state->region_file = NULL;

    free(state->country_file);
    state->country_file = NULL;

    for (i = 0; i < state->polygon_files_cnt; i++) {
      free(state->polygon_files[i]);
      state->polygon_files[i] = NULL;
    }
    state->polygon_files_cnt = 0;

    free(state->na_to_polygon_file);
    state->na_to_polygon_file = NULL;

    if (state->regions != NULL) {
      for (i = 0; i < state->regions_cnt; i++) {
        if (state->regions[i]->name != NULL) {
          free(state->regions[i]->name);
          state->regions[i]->name = NULL;
        }
        free(state->regions[i]);
        state->regions[i] = NULL;
      }
      free(state->regions);
      state->regions = NULL;
      state->regions_cnt = 0;
    }

    if (state->countries != NULL) {
      for (i = 0; i < state->countries_cnt; i++) {
        if (state->countries[i]->name != NULL) {
          free(state->countries[i]->name);
          state->countries[i]->name = NULL;
        }
        free(state->countries[i]);
        state->countries[i] = NULL;
      }
      free(state->countries);
      state->countries = NULL;
      state->countries_cnt = 0;
    }

    if (state->polygon_tables != NULL) {
      /* @todo move to a polygon_table_free function */
      for (i = 0; i < state->polygon_tables_cnt; i++) {
        table = state->polygon_tables[i];

        free(table->ascii_id);
        table->ascii_id = NULL;

        /** @todo move a a polygon_free function */
        for (j = 0; j < table->polygons_cnt; j++) {
          free(table->polygons[j]->name);
          table->polygons[j]->name = NULL;

          free(table->polygons[j]->fqid);
          table->polygons[j]->fqid = NULL;

          free(table->polygons[j]->usercode);
          table->polygons[j]->usercode = NULL;

          free(table->polygons[j]);
          table->polygons[j] = NULL;
        }

        free(table->polygons);
        table->polygons = NULL;

        free(table);
        state->polygon_tables[i] = NULL;
      }
      free(state->polygon_tables);
      state->polygon_tables = NULL;
      state->polygon_tables_cnt = 0;
    }

    /* just in case */
    na_to_polygon_free(state);
    csv_free(&(state->parser));

    ipmeta_provider_free_state(provider);
  }
  return;
}

int ipmeta_provider_netacq_edge_lookup(ipmeta_provider_t *provider,
                                       uint32_t addr, uint8_t mask,
                                       ipmeta_record_set_t *records)
{
  /* just call the lookup helper func in provider manager */
  return ipmeta_provider_lookup_records(provider, addr, mask, records);
}

int ipmeta_provider_netacq_edge_lookup_single(ipmeta_provider_t *provider,
                                              uint32_t addr,
                                              ipmeta_record_set_t *found)
{
  /* just call the lookup helper func in provider manager */
  return ipmeta_provider_lookup_record_single(provider, addr, found);
}

int ipmeta_provider_netacq_edge_get_regions(
  ipmeta_provider_t *provider, ipmeta_provider_netacq_edge_region_t ***regions)
{
  assert(provider != NULL && provider->enabled != 0);
  ipmeta_provider_netacq_edge_state_t *state = STATE(provider);
  *regions = state->regions;
  return state->regions_cnt;
}

int ipmeta_provider_netacq_edge_get_countries(
  ipmeta_provider_t *provider,
  ipmeta_provider_netacq_edge_country_t ***countries)
{
  assert(provider != NULL && provider->enabled != 0);
  ipmeta_provider_netacq_edge_state_t *state = STATE(provider);
  *countries = state->countries;
  return state->countries_cnt;
}

int ipmeta_provider_netacq_edge_get_polygon_tables(
  ipmeta_provider_t *provider, ipmeta_polygon_table_t ***tables)
{
  assert(provider != NULL && provider->enabled != 0);
  ipmeta_provider_netacq_edge_state_t *state = STATE(provider);
  *tables = state->polygon_tables;
  return state->polygon_tables_cnt;
}

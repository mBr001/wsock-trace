/*
 * ip2loc.c - Part of Wsock-Trace.
 *
 * This file is an interface for the IP2Location library.
 *   Ref: https://github.com/chrislim2888/IP2Location-C-Library
 *        http://lite.ip2location.com
 *
 * For 'inet_addr' warning.
 */
#ifndef _WINSOCK_DEPRECATED_NO_WARNINGS
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#endif

#include <sys/stat.h>

#include "common.h"
#include "init.h"
#include "geoip.h"

#if defined(USE_IP2LOCATION)
#include <stdint.h>
#include <IP2Location.h>

static IP2Location *handle;
static DWORD        file_size;

/* A static function inside IP2Location.c which gets included below.
 */
static int IP2Location_initialize (IP2Location *loc);

/*
 * Do not call 'IP2Location_open()' because of it's use of 'printf()'.
 * Hence just do what 'IP2Location_open()' does here.
 */
static IP2Location *open_file (const char *file)
{
  struct stat  st;
  IP2Location *loc;
  FILE        *f;
  int          IPvX;

  f = fopen (file, "rb");
  if (!f)
  {
    TRACE (1, "ip2loc: Failed to open \"bin_file\" file %s.\n", file);
    return (NULL);
  }
  loc = calloc (1, sizeof(*loc));
  loc->filehandle = f;

  IP2Location_initialize (loc);
  if (IP2Location_open_mem(loc, IP2LOCATION_SHARED_MEMORY) == -1)
  {
    TRACE (1, "ip2loc: Call to IP2Location_open_mem() failed.\n");
    IP2Location_close (loc);
    return (NULL);
  }

  stat (file, &st);
  file_size = st.st_size;

  /* The IP2Loc database scheme is really strange.
   */
  IPvX = loc->ipversion;
  if (IPvX == IPV4)
     IPvX = 4;
  else if (IPvX == IPV6)
     IPvX = 6;

  TRACE (2, "ip2loc: Success. Database has %s entries. API-version: %s\n"
            "                Date: %02d-%02d-%04d, IPvX: %d, "
            "IP4count: %u, IP6count: %u.\n",
         dword_str(loc->ipv4databasecount), IP2Location_api_version_string(),
         loc->databaseday, loc->databasemonth, 2000+loc->databaseyear,
         IPvX,
         loc->ipv4databasecount, loc->ipv6databasecount);
  return (loc);
}

BOOL ip2loc_init (void)
{
  if (!g_cfg.geoip_enable || !g_cfg.ip2location_bin_file)
     return (FALSE);

  if (!handle)
     handle = open_file (g_cfg.ip2location_bin_file);

  return (handle != NULL);
}

void ip2loc_exit (void)
{
  if (handle)
     IP2Location_close (handle);

  IP2Location_delete_shm();
  handle = NULL;
}

DWORD ip2loc_num_ipv4_entries (void)
{
  if (handle)
     return (handle->ipv4databasecount);
  return (0);
}

DWORD ip2loc_num_ipv6_entries (void)
{
  if (handle)
     return (handle->ipv6databasecount);
  return (0);
}

/*
 * Include the IP2Location sources here to avoid the need to build the library.
 * The Makefile.win on Github is broken anyway.
 *
 * And turn off some warnings:
 */
#if defined(__GNUC__) || defined(__clang__)
  GCC_PRAGMA (GCC diagnostic ignored "-Wunused-function");
  GCC_PRAGMA (GCC diagnostic ignored "-Wunused-variable");
  #if !defined(__clang__)
    GCC_PRAGMA (GCC diagnostic ignored "-Wunused-but-set-variable");
  #endif

#elif defined(_MSC_VER)
  #pragma warning (disable: 4101 4244)
#endif

/*
 * For 'inet_pton()' in below "IP2Location.c"
 */
#include "in_addr.h"

/*
 * Since 'IP2Location_parse_addr()' does a lot of calls to
 * 'IP2Location_ip_is_ipv4()' and 'IP2Location_ip_is_ipv6()', keep
 * the noise-level down ny not calling 'WSASetLastError()' in in_addr.c.
*/
static int ip2loc_inet_pton (int family, const char *addr, void *result)
{
  BOOL save = call_WSASetLastError;
  int  rc;

  call_WSASetLastError = FALSE;
  rc = wsock_trace_inet_pton (family, addr, result);
  call_WSASetLastError = save;
  return (rc);
}

#undef  inet_pton
#define inet_pton(family, addr, result)  ip2loc_inet_pton (family, addr, result)

#if defined(__CYGWIN__) && !defined(_WIN32)
#define _WIN32   /* Checks on '_WIN32' in "IP2Location.c" */
#endif

/*
 * This assumes the IP2Location .c/.h files are in the %INCLUDE% or
 * %C_INCLUDE_PATH% path. Or the '$(IP2LOCATION_ROOT)' is set in
 * respective makefile.
 */
#include "IP2Location.c"
#include "IP2Loc_DBInterface.c"

BOOL ip2loc_get_entry (const char *addr, struct ip2loc_entry *ent)
{
  IP2LocationRecord *r = IP2Location_get_record (handle, (char*)addr,
                                                 COUNTRYSHORT | COUNTRYLONG | REGION | CITY);

  memset (ent, '\0', sizeof(*ent));
  if (!r)
     return (FALSE);

  TRACE (3, "Record for %s; country_short: \"%.2s\"\n", addr, r->country_short);

  if (r->country_short[0] == '-' ||                   /* is "-" for unallocated addr */
      !strncmp(r->country_short,"INVALID",7) ||       /* INVALID_IPV4_ADDRESS/INVALID IPV4 ADDRESS */
      !strncmp(r->country_short,"This parameter",14)) /* NOT_SUPPORTED */
  {
    IP2Location_free_record (r);
    return (FALSE);
  }

  _strlcpy (ent->country_short, r->country_short, sizeof(ent->country_short));
  _strlcpy (ent->country_long, r->country_long, sizeof(ent->country_long));
  _strlcpy (ent->city, r->city, sizeof(ent->city));
  _strlcpy (ent->region, r->region, sizeof(ent->region));
  IP2Location_free_record (r);
  return (TRUE);
}

#else /* USE_IP2LOCATION */

BOOL ip2loc_init (void)
{
  return (FALSE);
}

void ip2loc_exit (void)
{
}

DWORD ip2loc_num_ipv4_entries (void)
{
  return (0);
}

DWORD ip2loc_num_ipv6_entries (void)
{
  return (0);
}

BOOL ip2loc_get_entry (const char *addr, struct ip2loc_entry *ent)
{
  ARGSUSED (addr);
  ARGSUSED (ent);
  return (FALSE);
}
#endif  /* USE_IP2LOCATION */

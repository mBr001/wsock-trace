/**\file    firewall.c
 * \ingroup inet_util
 *
 * \brief
 *  Function for listening for "Windows Filtering Platform (WFP)" events
 *
 *  The `fw_init()` and `fw_monitor_start()` needs Administrator privileges.
 *  Running `firewall_test.exe` as a normal non-elevated user will normally cause an
 *  "The device does not recognize the command" (`ERROR_BAD_COMMAND = 22`).
 *
 * Thanks to dmex for his implementation of similar stuff in his ProcessHacker:
 * \see
 *   + https://github.com/processhacker/plugins-extra/blob/master/FirewallMonitorPlugin/fw.c
 *   + https://github.com/processhacker/plugins-extra/blob/master/FirewallMonitorPlugin/monitor.c
 *
 * Also many thanks to Henry++ for his SimpleWall:
 *  \see
 *    + https://github.com/henrypp/simplewall/blob/master/src/main.cpp
 *
 * A rather messy but rich example is at:
 *  \see
 *   + https://social.msdn.microsoft.com/Forums/sqlserver/en-US/74e3bf1d-3a0b-43ce-a528-2a88bc1fb882/log-packets?forum=wfp
 */

/**
 * For MSVC/clang We need at least a Win-Vista SDK here.
 * But for MinGW (tdm-gcc) we need a Win-7 SDK (0x601).
 */
#if defined(__MINGW32__)
  #define MIN_WINNT 0x601
#else
  #define MIN_WINNT 0x600
#endif

#if defined(__WATCOMC__)
  /*
   * OpenWatcom 2.x is hardly able to compile and use anything here.
   */
  #error "This module if not for Watcom / OpenWatcom."
#endif

#if !defined(_WIN32_WINNT) || (_WIN32_WINNT < MIN_WINNT)
  #undef  _WIN32_WINNT
  #define _WIN32_WINNT MIN_WINNT
#endif

#include "common.h"
#include "smartlist.h"
#include "init.h"
#include "in_addr.h"
#include "dump.h"
#include "cpu.h"
#include "geoip.h"
#include "wsock_trace.h"

typedef LONG NTSTATUS;

#include <sddl.h>   /* For `ConvertSidToStringSid()` */
#include <fwpmu.h>

#if !defined(__MINGW32__) && !defined(__CYGWIN__)
#include <fwpsu.h>
#endif

#include "firewall.h"

/*
 * The code 'ip = _byteswap_ulong (*(DWORD*)&header->localAddrV4);' causes
 * a gcc warning. Ignore it.
 */
GCC_PRAGMA (GCC diagnostic ignored "-Wstrict-aliasing")
GCC_PRAGMA (GCC diagnostic ignored "-Wunused-but-set-variable")
GCC_PRAGMA (GCC diagnostic ignored "-Wunused-function")
GCC_PRAGMA (GCC diagnostic ignored "-Wenum-compare")
GCC_PRAGMA (GCC diagnostic ignored "-Wmissing-braces")

#if defined(__CYGWIN__)
  #include <errno.h>
  #include <wctype.h>

  #define _popen(cmd, mode)  popen (cmd, mode)
  #define _pclose(fil)       pclose (fil)

 /*
  * These are prototyped in '<w32api/intrin.h>', but found nowhere.
  * Besides including <asm/byteorder.h> fails since <winsock2.h>
  * was already included. Sigh!
  */
  #define _byteswap_ulong(x)   swap32(x)
  #define _byteswap_ushort(x)  swap16(x)
#endif

/**
 * \def FW_API_LOW
 *  The lowest API level supported here.
 *
 * \def FW_API_HIGH
 *  The highest API level supported here.
 *
 * \def FW_API_DEFAULT
 *  The default API level used here if not specified using the `fw_api` variable
 *  prior to calling `fw_monitor_start()` or `fw_dump_events()`.
 */
#define FW_API_LOW     0
#define FW_API_HIGH    4
#define FW_API_DEFAULT 3

/**
 * \def FW_FUNC_ERROR
 *  The error-code (1627) to use if a needed function is not found.
 */
#define FW_FUNC_ERROR  ERROR_FUNCTION_FAILED

/**
 * \def TIME_STRING_FMT
 * The `fw_buf_add()` format for a `get_time_string()` result.
 *
 * \def INDENT_SZ
 * The number of spaces to indent a printed line.
 */
#if defined(TEST_FIREWALL)
  #define TIME_STRING_FMT  "\n~1%s: "
  #define INDENT_SZ        2

  /* Used for the reference-timestamp value in `get_time_string (NULL)`.
   */
  func_GetSystemTimePreciseAsFileTime p_GetSystemTimePreciseAsFileTime;
#else

  /* Similar as to wsock_trace.c shows a time-stamp.
   */
  #define TIME_STRING_FMT "\n  ~1* %s: "
  #define INDENT_SZ        (2 + g_cfg.trace_indent)
#endif

DWORD fw_errno;
int   fw_api = FW_API_DEFAULT;

typedef enum FW_STORE_TYPE {
             FW_STORE_TYPE_INVALID,
             FW_STORE_TYPE_GP_RSOP,
             FW_STORE_TYPE_LOCAL,
             FW_STORE_TYPE_NOT_USED_VALUE_3,
             FW_STORE_TYPE_NOT_USED_VALUE_4,
             FW_STORE_TYPE_DYNAMIC,
             FW_STORE_TYPE_GPO,
             FW_STORE_TYPE_DEFAULTS,
             FW_STORE_TYPE_MAX
           } FW_STORE_TYPE;

typedef enum FW_PROFILE_TYPE {
             FW_PROFILE_TYPE_INVALID  = 0,
             FW_PROFILE_TYPE_DOMAIN   = 0x001,
             FW_PROFILE_TYPE_STANDARD = 0x002,
             FW_PROFILE_TYPE_PRIVATE  = FW_PROFILE_TYPE_STANDARD,
             FW_PROFILE_TYPE_PUBLIC   = 0x004,
             FW_PROFILE_TYPE_ALL      = 0x7FFFFFFF,
             FW_PROFILE_TYPE_CURRENT  = 0x80000000,
             FW_PROFILE_TYPE_NONE     = FW_PROFILE_TYPE_CURRENT + 1
           } FW_PROFILE_TYPE;

typedef enum FW_RULE_STATUS {
             FW_RULE_STATUS_OK                                               = 0x00010000,
             FW_RULE_STATUS_PARTIALLY_IGNORED                                = 0x00020000,
             FW_RULE_STATUS_IGNORED                                          = 0x00040000,
             FW_RULE_STATUS_PARSING_ERROR_NAME                               = 0x00080001,
             FW_RULE_STATUS_PARSING_ERROR_DESC                               = 0x00080002,
             FW_RULE_STATUS_PARSING_ERROR_APP                                = 0x00080003,
             FW_RULE_STATUS_PARSING_ERROR_SVC                                = 0x00080004,
             FW_RULE_STATUS_PARSING_ERROR_RMA                                = 0x00080005,
             FW_RULE_STATUS_PARSING_ERROR_RUA                                = 0x00080006,
             FW_RULE_STATUS_PARSING_ERROR_EMBD                               = 0x00080007,
             FW_RULE_STATUS_PARSING_ERROR_RULE_ID                            = 0x00080008,
             FW_RULE_STATUS_PARSING_ERROR_PHASE1_AUTH                        = 0x00080009,
             FW_RULE_STATUS_PARSING_ERROR_PHASE2_CRYPTO                      = 0x0008000A,
             FW_RULE_STATUS_PARSING_ERROR_REMOTE_ENDPOINTS                   = 0x0008000F,
             FW_RULE_STATUS_PARSING_ERROR_REMOTE_ENDPOINT_FQDN               = 0x00080010,
             FW_RULE_STATUS_PARSING_ERROR_KEY_MODULE                         = 0x00080011,
             FW_RULE_STATUS_PARSING_ERROR_PHASE2_AUTH                        = 0x0008000B,
             FW_RULE_STATUS_PARSING_ERROR_RESOLVE_APP                        = 0x0008000C,
             FW_RULE_STATUS_PARSING_ERROR_MAINMODE_ID                        = 0x0008000D,
             FW_RULE_STATUS_PARSING_ERROR_PHASE1_CRYPTO                      = 0x0008000E,
             FW_RULE_STATUS_PARSING_ERROR                                    = 0x00080000,
             FW_RULE_STATUS_SEMANTIC_ERROR_RULE_ID                           = 0x00100010,
             FW_RULE_STATUS_SEMANTIC_ERROR_PORTS                             = 0x00100020,
             FW_RULE_STATUS_SEMANTIC_ERROR_PORT_KEYW                         = 0x00100021,
             FW_RULE_STATUS_SEMANTIC_ERROR_PORT_RANGE                        = 0x00100022,
             FW_RULE_STATUS_SEMANTIC_ERROR_ADDR_V4_SUBNETS                   = 0x00100040,
             FW_RULE_STATUS_SEMANTIC_ERROR_ADDR_V6_SUBNETS                   = 0x00100041,
             FW_RULE_STATUS_SEMANTIC_ERROR_ADDR_V4_RANGES                    = 0x00100042,
             FW_RULE_STATUS_SEMANTIC_ERROR_ADDR_V6_RANGES                    = 0x00100043,
             FW_RULE_STATUS_SEMANTIC_ERROR_ADDR_RANGE                        = 0x00100044,
             FW_RULE_STATUS_SEMANTIC_ERROR_ADDR_MASK                         = 0x00100045,
             FW_RULE_STATUS_SEMANTIC_ERROR_ADDR_PREFIX                       = 0x00100046,
             FW_RULE_STATUS_SEMANTIC_ERROR_ADDR_KEYW                         = 0x00100047,
             FW_RULE_STATUS_SEMANTIC_ERROR_LADDR_PROP                        = 0x00100048,
             FW_RULE_STATUS_SEMANTIC_ERROR_RADDR_PROP                        = 0x00100049,
             FW_RULE_STATUS_SEMANTIC_ERROR_ADDR_V6                           = 0x0010004A,
             FW_RULE_STATUS_SEMANTIC_ERROR_LADDR_INTF                        = 0x0010004B,
             FW_RULE_STATUS_SEMANTIC_ERROR_ADDR_V4                           = 0x0010004C,
             FW_RULE_STATUS_SEMANTIC_ERROR_TUNNEL_ENDPOINT_ADDR              = 0x0010004D,
             FW_RULE_STATUS_SEMANTIC_ERROR_DTE_VER                           = 0x0010004E,
             FW_RULE_STATUS_SEMANTIC_ERROR_DTE_MISMATCH_ADDR                 = 0x0010004F,
             FW_RULE_STATUS_SEMANTIC_ERROR_PROFILE                           = 0x00100050,
             FW_RULE_STATUS_SEMANTIC_ERROR_ICMP                              = 0x00100060,
             FW_RULE_STATUS_SEMANTIC_ERROR_ICMP_CODE                         = 0x00100061,
             FW_RULE_STATUS_SEMANTIC_ERROR_IF_ID                             = 0x00100070,
             FW_RULE_STATUS_SEMANTIC_ERROR_IF_TYPE                           = 0x00100071,
             FW_RULE_STATUS_SEMANTIC_ERROR_ACTION                            = 0x00100080,
             FW_RULE_STATUS_SEMANTIC_ERROR_ALLOW_BYPASS                      = 0x00100081,
             FW_RULE_STATUS_SEMANTIC_ERROR_DO_NOT_SECURE                     = 0x00100082,
             FW_RULE_STATUS_SEMANTIC_ERROR_ACTION_BLOCK_IS_ENCRYPTED_SECURE  = 0x00100083,
             FW_RULE_STATUS_SEMANTIC_ERROR_DIR                               = 0x00100090,
             FW_RULE_STATUS_SEMANTIC_ERROR_PROT                              = 0x001000A0,
             FW_RULE_STATUS_SEMANTIC_ERROR_PROT_PROP                         = 0x001000A1,
             FW_RULE_STATUS_SEMANTIC_ERROR_DEFER_EDGE_PROP                   = 0x001000A2,
             FW_RULE_STATUS_SEMANTIC_ERROR_ALLOW_BYPASS_OUTBOUND             = 0x001000A3,
             FW_RULE_STATUS_SEMANTIC_ERROR_DEFER_USER_INVALID_RULE           = 0x001000A4,
             FW_RULE_STATUS_SEMANTIC_ERROR_FLAGS                             = 0x001000B0,
             FW_RULE_STATUS_SEMANTIC_ERROR_FLAGS_AUTO_AUTH                   = 0x001000B1,
             FW_RULE_STATUS_SEMANTIC_ERROR_FLAGS_AUTO_BLOCK                  = 0x001000B2,
             FW_RULE_STATUS_SEMANTIC_ERROR_FLAGS_AUTO_DYN_RPC                = 0x001000B3,
             FW_RULE_STATUS_SEMANTIC_ERROR_FLAGS_AUTHENTICATE_ENCRYPT        = 0x001000B4,
             FW_RULE_STATUS_SEMANTIC_ERROR_FLAGS_AUTH_WITH_ENC_NEGOTIATE_VER = 0x001000B5,
             FW_RULE_STATUS_SEMANTIC_ERROR_FLAGS_AUTH_WITH_ENC_NEGOTIATE     = 0x001000B6,
             FW_RULE_STATUS_SEMANTIC_ERROR_FLAGS_ESP_NO_ENCAP_VER            = 0x001000B7,
             FW_RULE_STATUS_SEMANTIC_ERROR_FLAGS_ESP_NO_ENCAP                = 0x001000B8,
             FW_RULE_STATUS_SEMANTIC_ERROR_FLAGS_TUNNEL_AUTH_MODES_VER       = 0x001000B9,
             FW_RULE_STATUS_SEMANTIC_ERROR_FLAGS_TUNNEL_AUTH_MODES           = 0x001000BA,
             FW_RULE_STATUS_SEMANTIC_ERROR_FLAGS_IP_TLS_VER                  = 0x001000BB,
             FW_RULE_STATUS_SEMANTIC_ERROR_FLAGS_PORTRANGE_VER               = 0x001000BC,
             FW_RULE_STATUS_SEMANTIC_ERROR_FLAGS_ADDRS_TRAVERSE_DEFER_VER    = 0x001000BD,
             FW_RULE_STATUS_SEMANTIC_ERROR_FLAGS_AUTH_WITH_ENC_NEGOTIATE_OUTBOUND      = 0x001000BE,
             FW_RULE_STATUS_SEMANTIC_ERROR_FLAGS_AUTHENTICATE_WITH_OUTBOUND_BYPASS_VER = 0x001000BF,
             FW_RULE_STATUS_SEMANTIC_ERROR_REMOTE_AUTH_LIST                  = 0x001000C0,
             FW_RULE_STATUS_SEMANTIC_ERROR_REMOTE_USER_LIST                  = 0x001000C1,
             FW_RULE_STATUS_SEMANTIC_ERROR_PLATFORM                          = 0x001000E0,
             FW_RULE_STATUS_SEMANTIC_ERROR_PLATFORM_OP_VER                   = 0x001000E1,
             FW_RULE_STATUS_SEMANTIC_ERROR_PLATFORM_OP                       = 0x001000E2,
             FW_RULE_STATUS_SEMANTIC_ERROR_DTE_NOANY_ADDR                    = 0x001000F0,
             FW_RULE_STATUS_SEMANTIC_TUNNEL_EXEMPT_WITH_GATEWAY              = 0x001000F1,
             FW_RULE_STATUS_SEMANTIC_TUNNEL_EXEMPT_VER                       = 0x001000F2,
             FW_RULE_STATUS_SEMANTIC_ERROR_PHASE1_AUTH_SET_ID                = 0x00100500,
             FW_RULE_STATUS_SEMANTIC_ERROR_PHASE2_CRYPTO_SET_ID              = 0x00100510,
             FW_RULE_STATUS_SEMANTIC_ERROR_PHASE1_CRYPTO_SET_ID              = 0x00100511,
             FW_RULE_STATUS_SEMANTIC_ERROR_SET_ID                            = 0x00101000,
             FW_RULE_STATUS_SEMANTIC_ERROR_IPSEC_PHASE                       = 0x00101010,
             FW_RULE_STATUS_SEMANTIC_ERROR_EMPTY_SUITES                      = 0x00101020,
             FW_RULE_STATUS_SEMANTIC_ERROR_PHASE1_AUTH_METHOD                = 0x00101030,
             FW_RULE_STATUS_SEMANTIC_ERROR_PHASE2_AUTH_METHOD                = 0x00101031,
             FW_RULE_STATUS_SEMANTIC_ERROR_AUTH_METHOD_ANONYMOUS             = 0x00101032,
             FW_RULE_STATUS_SEMANTIC_ERROR_AUTH_METHOD_DUPLICATE             = 0x00101033,
             FW_RULE_STATUS_SEMANTIC_ERROR_AUTH_METHOD_VER                   = 0x00101034,
             FW_RULE_STATUS_SEMANTIC_ERROR_AUTH_SUITE_FLAGS                  = 0x00101040,
             FW_RULE_STATUS_SEMANTIC_ERROR_HEALTH_CERT                       = 0x00101041,
             FW_RULE_STATUS_SEMANTIC_ERROR_AUTH_SIGNCERT_VER                 = 0x00101042,
             FW_RULE_STATUS_SEMANTIC_ERROR_AUTH_INTERMEDIATE_CA_VER          = 0x00101043,
             FW_RULE_STATUS_SEMANTIC_ERROR_MACHINE_SHKEY                     = 0x00101050,
             FW_RULE_STATUS_SEMANTIC_ERROR_CA_NAME                           = 0x00101060,
             FW_RULE_STATUS_SEMANTIC_ERROR_MIXED_CERTS                       = 0x00101061,
             FW_RULE_STATUS_SEMANTIC_ERROR_NON_CONTIGUOUS_CERTS              = 0x00101062,
             FW_RULE_STATUS_SEMANTIC_ERROR_MIXED_CA_TYPE_IN_BLOCK            = 0x00101063,
             FW_RULE_STATUS_SEMANTIC_ERROR_MACHINE_USER_AUTH                 = 0x00101070,
             FW_RULE_STATUS_SEMANTIC_ERROR_PHASE1_CRYPTO_NON_DEFAULT_ID      = 0x00105000,
             FW_RULE_STATUS_SEMANTIC_ERROR_PHASE1_CRYPTO_FLAGS               = 0x00105001,
             FW_RULE_STATUS_SEMANTIC_ERROR_PHASE1_CRYPTO_TIMEOUT_MINUTES     = 0x00105002,
             FW_RULE_STATUS_SEMANTIC_ERROR_PHASE1_CRYPTO_TIMEOUT_SESSIONS    = 0x00105003,
             FW_RULE_STATUS_SEMANTIC_ERROR_PHASE1_CRYPTO_KEY_EXCHANGE        = 0x00105004,
             FW_RULE_STATUS_SEMANTIC_ERROR_PHASE1_CRYPTO_ENCRYPTION          = 0x00105005,
             FW_RULE_STATUS_SEMANTIC_ERROR_PHASE1_CRYPTO_HASH                = 0x00105006,
             FW_RULE_STATUS_SEMANTIC_ERROR_PHASE1_CRYPTO_ENCRYPTION_VER      = 0x00105007,
             FW_RULE_STATUS_SEMANTIC_ERROR_PHASE1_CRYPTO_HASH_VER            = 0x00105008,
             FW_RULE_STATUS_SEMANTIC_ERROR_PHASE2_CRYPTO_PFS                 = 0x00105020,
             FW_RULE_STATUS_SEMANTIC_ERROR_PHASE2_CRYPTO_PROTOCOL            = 0x00105021,
             FW_RULE_STATUS_SEMANTIC_ERROR_PHASE2_CRYPTO_ENCRYPTION          = 0x00105022,
             FW_RULE_STATUS_SEMANTIC_ERROR_PHASE2_CRYPTO_HASH                = 0x00105023,
             FW_RULE_STATUS_SEMANTIC_ERROR_PHASE2_CRYPTO_TIMEOUT_MINUTES     = 0x00105024,
             FW_RULE_STATUS_SEMANTIC_ERROR_PHASE2_CRYPTO_TIMEOUT_KBYTES      = 0x00105025,
             FW_RULE_STATUS_SEMANTIC_ERROR_PHASE2_CRYPTO_ENCRYPTION_VER      = 0x00105026,
             FW_RULE_STATUS_SEMANTIC_ERROR_PHASE2_CRYPTO_HASH_VER            = 0x00105027,
             FW_RULE_STATUS_SEMANTIC_ERROR_QUERY_OR_AND_CONDITIONS           = 0x00106000,
             FW_RULE_STATUS_SEMANTIC_ERROR_QUERY_AND_CONDITIONS              = 0x00106001,
             FW_RULE_STATUS_SEMANTIC_ERROR_QUERY_CONDITION_KEY               = 0x00106002,
             FW_RULE_STATUS_SEMANTIC_ERROR_QUERY_CONDITION_MATCH_TYPE        = 0x00106003,
             FW_RULE_STATUS_SEMANTIC_ERROR_QUERY_CONDITION_DATA_TYPE         = 0x00106004,
             FW_RULE_STATUS_SEMANTIC_ERROR_QUERY_CONDITION_KEY_AND_DATA_TYPE = 0x00106005,
             FW_RULE_STATUS_SEMANTIC_ERROR_QUERY_KEYS_PROTOCOL_PORT          = 0x00106006,
             FW_RULE_STATUS_SEMANTIC_ERROR_QUERY_KEY_PROFILE                 = 0x00106007,
             FW_RULE_STATUS_SEMANTIC_ERROR_QUERY_KEY_STATUS                  = 0x00106008,
             FW_RULE_STATUS_SEMANTIC_ERROR_QUERY_KEY_FILTERID                = 0x00106009,
             FW_RULE_STATUS_SEMANTIC_ERROR_QUERY_KEY_APP_PATH                = 0x00106010,
             FW_RULE_STATUS_SEMANTIC_ERROR_QUERY_KEY_PROTOCOL                = 0x00106011,
             FW_RULE_STATUS_SEMANTIC_ERROR_QUERY_KEY_LOCAL_PORT              = 0x00106012,
             FW_RULE_STATUS_SEMANTIC_ERROR_QUERY_KEY_REMOTE_PORT             = 0x00106013,
             FW_RULE_STATUS_SEMANTIC_ERROR_QUERY_KEY_SVC_NAME                = 0x00106015,
             FW_RULE_STATUS_SEMANTIC_ERROR_REQUIRE_IN_CLEAR_OUT_ON_TRANSPORT = 0x00107000,
             FW_RULE_STATUS_SEMANTIC_ERROR_TUNNEL_BYPASS_TUNNEL_IF_SECURE_ON_TRANSPORT = 0x00107001,
             FW_RULE_STATUS_SEMANTIC_ERROR_AUTH_NOENCAP_ON_TUNNEL            = 0x00107002,
             FW_RULE_STATUS_SEMANTIC_ERROR_AUTH_NOENCAP_ON_PSK               = 0x00107003,
             FW_RULE_STATUS_SEMANTIC_ERROR_CRYPTO_ENCR_HASH                  = 0x00105040,
             FW_RULE_STATUS_SEMANTIC_ERROR_CRYPTO_ENCR_HASH_COMPAT           = 0x00105041,
             FW_RULE_STATUS_SEMANTIC_ERROR_SCHEMA_VERSION                    = 0x00105050,
             FW_RULE_STATUS_SEMANTIC_ERROR                                   = 0x00100000,
             FW_RULE_STATUS_RUNTIME_ERROR_PHASE1_AUTH_NOT_FOUND              = 0x00200001,
             FW_RULE_STATUS_RUNTIME_ERROR_PHASE2_AUTH_NOT_FOUND              = 0x00200002,
             FW_RULE_STATUS_RUNTIME_ERROR_PHASE2_CRYPTO_NOT_FOUND            = 0x00200003,
             FW_RULE_STATUS_RUNTIME_ERROR_AUTH_MCHN_SHKEY_MISMATCH           = 0x00200004,
             FW_RULE_STATUS_RUNTIME_ERROR_PHASE1_CRYPTO_NOT_FOUND            = 0x00200005,
             FW_RULE_STATUS_RUNTIME_ERROR_AUTH_NOENCAP_ON_TUNNEL             = 0x00200006,
             FW_RULE_STATUS_RUNTIME_ERROR_AUTH_NOENCAP_ON_PSK                = 0x00200007,
             FW_RULE_STATUS_RUNTIME_ERROR                                    = 0x00200000,
             FW_RULE_STATUS_ERROR                                            = FW_RULE_STATUS_PARSING_ERROR |
                                                                               FW_RULE_STATUS_SEMANTIC_ERROR |
                                                                               FW_RULE_STATUS_RUNTIME_ERROR,
             FW_RULE_STATUS_ALL                                              = 0xFFFF0000
           } FW_RULE_STATUS;

typedef enum FW_RULE_STATUS_CLASS {
             FW_RULE_STATUS_CLASS_OK                = FW_RULE_STATUS_OK,
             FW_RULE_STATUS_CLASS_PARTIALLY_IGNORED = FW_RULE_STATUS_PARTIALLY_IGNORED,
             FW_RULE_STATUS_CLASS_IGNORED           = FW_RULE_STATUS_IGNORED,
             FW_RULE_STATUS_CLASS_PARSING_ERROR     = FW_RULE_STATUS_PARSING_ERROR,
             FW_RULE_STATUS_CLASS_SEMANTIC_ERROR    = FW_RULE_STATUS_SEMANTIC_ERROR,
             FW_RULE_STATUS_CLASS_RUNTIME_ERROR     = FW_RULE_STATUS_RUNTIME_ERROR,
             FW_RULE_STATUS_CLASS_ERROR             = FW_RULE_STATUS_ERROR,
             FW_RULE_STATUS_CLASS_ALL               = FW_RULE_STATUS_ALL
           } FW_RULE_STATUS_CLASS;

typedef enum FW_POLICY_ACCESS_RIGHT {
             FW_POLICY_ACCESS_RIGHT_INVALID,
             FW_POLICY_ACCESS_RIGHT_READ,
             FW_POLICY_ACCESS_RIGHT_READ_WRITE,
             FW_POLICY_ACCESS_RIGHT_MAX
           } FW_POLICY_ACCESS_RIGHT;

typedef enum FW_POLICY_STORE_FLAGS {
             FW_POLICY_STORE_FLAGS_NONE,
             FW_POLICY_STORE_FLAGS_DELETE_DYNAMIC_RULES_AFTER_CLOSE,
             FW_POLICY_STORE_FLAGS_MAX
           } FW_POLICY_STORE_FLAGS;

typedef enum FW_RULE_ORIGIN_TYPE {
             FW_RULE_ORIGIN_INVALID,
             FW_RULE_ORIGIN_LOCAL,
             FW_RULE_ORIGIN_GP,
             FW_RULE_ORIGIN_DYNAMIC,
             FW_RULE_ORIGIN_AUTOGEN,
             FW_RULE_ORIGIN_HARDCODED,
             FW_RULE_ORIGIN_MAX
           } FW_RULE_ORIGIN_TYPE;

/**
 * \enum FW_ENUM_RULES_FLAGS
 * \see http://msdn.microsoft.com/en-us/library/cc231521.aspx
 */
typedef enum FW_ENUM_RULES_FLAGS {
             /**
              * This value signifies that no specific flag is used.
              * It is defined for IDL definitions and code to add readability, instead of using the number 0.
              */
             FW_ENUM_RULES_FLAG_NONE = 0x0000,

             /** Resolves rule description strings to user-friendly, localizable strings if they are in the following
              *  format: `@file.dll,-<resID>`. resID refers to the resource ID in the indirect string.
              *  Please see [MSDN-SHLoadIndirectString] for further documentation on the string format.
              */
             FW_ENUM_RULES_FLAG_RESOLVE_NAME = 0x0001,

             /** Resolves rule description strings to user-friendly, localizable strings if they are in the following
              *  format: `@file.dll,-<resID>`. resID refers to the resource ID in the indirect string.
              * Please see [MSDN-SHLoadIndirectString] for further documentation on the string format.
              */
             FW_ENUM_RULES_FLAG_RESOLVE_DESCRIPTION = 0x0002,

             /** If this flag is set, the server MUST inspect the wszLocalApplication field of each FW_RULE structure
              *  and replace all environment variables in the string with their corresponding values.
              *  See [MSDN-ExpandEnvironmentStrings] for more details about environment-variable strings.
              */
             FW_ENUM_RULES_FLAG_RESOLVE_APPLICATION = 0x0004,

             /** Resolves keywords in addresses and ports to the actual addresses and
              *  ports (dynamic store only).
              */
             FW_ENUM_RULES_FLAG_RESOLVE_KEYWORD = 0x0008,

             /** Resolves the GPO name for the GP_RSOP rules.
              */
             FW_ENUM_RULES_FLAG_RESOLVE_GPO_NAME = 0x0010,

             /** If this flag is set, the server MUST only return objects where at least one
              *  FW_ENFORCEMENT_STATE entry in the object's metadata is equal to FW_ENFORCEMENT_STATE_FULL.
              *  This flag is available for the dynamic store only.
              */
             FW_ENUM_RULES_FLAG_EFFECTIVE = 0x0020,

             /** Includes the metadata object information, represented by the FW_OBJECT_METADATA structure,
              *  in the enumerated objects.
              */
             FW_ENUM_RULES_FLAG_INCLUDE_METADATA = 0x0040,

             /** This value and greater values are invalid and MUST NOT be used. It is defined for
              *  simplicity in writing IDL definitions and code.
              */
             FW_ENUM_RULES_FLAG_MAX = 0x0080
           } FW_ENUM_RULES_FLAGS;

typedef enum FW_RULE_ACTION {
             FW_RULE_ACTION_INVALID = 0,
             FW_RULE_ACTION_ALLOW_BYPASS,
             FW_RULE_ACTION_BLOCK,
             FW_RULE_ACTION_ALLOW,
             FW_RULE_ACTION_MAX
           } FW_RULE_ACTION;

typedef enum FW_DIRECTION {
             FW_DIR_INVALID = 0,
             FW_DIR_IN      = 1,
             FW_DIR_OUT     = 2,
             FW_DIR_BOTH    = 3 /* MAX */
           } FW_DIRECTION;

typedef enum FW_ENFORCEMENT_STATE {
             FW_ENFORCEMENT_STATE_INVALID,
             FW_ENFORCEMENT_STATE_FULL,
             FW_ENFORCEMENT_STATE_WF_OFF_IN_PROFILE,
             FW_ENFORCEMENT_STATE_CATEGORY_OFF,
             FW_ENFORCEMENT_STATE_DISABLED_OBJECT,
             FW_ENFORCEMENT_STATE_INACTIVE_PROFILE,
             FW_ENFORCEMENT_STATE_LOCAL_ADDRESS_RESOLUTION_EMPTY,
             FW_ENFORCEMENT_STATE_REMOTE_ADDRESS_RESOLUTION_EMPTY,
             FW_ENFORCEMENT_STATE_LOCAL_PORT_RESOLUTION_EMPTY,
             FW_ENFORCEMENT_STATE_REMOTE_PORT_RESOLUTION_EMPTY,
             FW_ENFORCEMENT_STATE_INTERFACE_RESOLUTION_EMPTY,
             FW_ENFORCEMENT_STATE_APPLICATION_RESOLUTION_EMPTY,
             FW_ENFORCEMENT_STATE_REMOTE_MACHINE_EMPTY,
             FW_ENFORCEMENT_STATE_REMOTE_USER_EMPTY,
             FW_ENFORCEMENT_STATE_LOCAL_GLOBAL_OPEN_PORTS_DISALLOWED,
             FW_ENFORCEMENT_STATE_LOCAL_AUTHORIZED_APPLICATIONS_DISALLOWED,
             FW_ENFORCEMENT_STATE_LOCAL_FIREWALL_RULES_DISALLOWED,
             FW_ENFORCEMENT_STATE_LOCAL_CONSEC_RULES_DISALLOWED,
             FW_ENFORCEMENT_STATE_MISMATCHED_PLATFORM,
             FW_ENFORCEMENT_STATE_OPTIMIZED_OUT,
             FW_ENFORCEMENT_STATE_MAX
           } FW_ENFORCEMENT_STATE;

typedef enum _FWPM_NET_EVENT_TYPE {
             _FWPM_NET_EVENT_TYPE_IKEEXT_MM_FAILURE  = 0,
             _FWPM_NET_EVENT_TYPE_IKEEXT_QM_FAILURE  = 1,
             _FWPM_NET_EVENT_TYPE_IKEEXT_EM_FAILURE  = 2,
             _FWPM_NET_EVENT_TYPE_CLASSIFY_DROP      = 3,
             _FWPM_NET_EVENT_TYPE_IPSEC_KERNEL_DROP  = 4,
             _FWPM_NET_EVENT_TYPE_IPSEC_DOSP_DROP    = 5,
             _FWPM_NET_EVENT_TYPE_CLASSIFY_ALLOW     = 6,
             _FWPM_NET_EVENT_TYPE_CAPABILITY_DROP    = 7,
             _FWPM_NET_EVENT_TYPE_CAPABILITY_ALLOW   = 8,
             _FWPM_NET_EVENT_TYPE_CLASSIFY_DROP_MAC  = 9,
             _FWPM_NET_EVENT_TYPE_LPM_PACKET_ARRIVAL = 10,
             _FWPM_NET_EVENT_TYPE_MAX                = 11
           } _FWPM_NET_EVENT_TYPE;

typedef enum FWPM_APPC_NETWORK_CAPABILITY_TYPE {
             FWPM_APPC_NETWORK_CAPABILITY_INTERNET_CLIENT,
             FWPM_APPC_NETWORK_CAPABILITY_INTERNET_CLIENT_SERVER,
             FWPM_APPC_NETWORK_CAPABILITY_INTERNET_PRIVATE_NETWORK
           } FWPM_APPC_NETWORK_CAPABILITY_TYPE;

typedef struct FW_PORT_RANGE {
         USHORT  wBegin;
         USHORT  wEnd;
       } FW_PORT_RANGE;

typedef struct FW_PORT_RANGE_LIST {
        ULONG          dwNumEntries;
        FW_PORT_RANGE *pPorts;
      } FW_PORT_RANGE_LIST;

typedef struct FW_PORTS {
        USHORT             wPortKeywords;
        FW_PORT_RANGE_LIST Ports;
      } FW_PORTS;

typedef struct FW_ICMP_TYPE_CODE {
        UCHAR  bType;
        USHORT wCode;
      } FW_ICMP_TYPE_CODE;

typedef struct FW_ICMP_TYPE_CODE_LIST {
        ULONG              dwNumEntries;
        FW_ICMP_TYPE_CODE *pEntries;
      } FW_ICMP_TYPE_CODE_LIST;

typedef struct FW_IPV4_SUBNET {
        ULONG  dwAddress;
        ULONG  dwSubNetMask;
      } FW_IPV4_SUBNET;

typedef struct FW_IPV4_SUBNET_LIST {
        ULONG           dwNumEntries;
        FW_IPV4_SUBNET *pSubNets;
      } FW_IPV4_SUBNET_LIST;

typedef struct FW_IPV4_ADDRESS_RANGE {
        ULONG  dwBegin;
        ULONG  dwEnd;
    } FW_IPV4_ADDRESS_RANGE;

typedef struct FW_IPV4_RANGE_LIST {
        ULONG                  dwNumEntries;
        FW_IPV4_ADDRESS_RANGE *pRanges;
      } FW_IPV4_RANGE_LIST;

typedef struct FW_IPV6_SUBNET {
        UCHAR  Address [16];
        ULONG  dwNumPrefixBits;
      } FW_IPV6_SUBNET;

typedef struct FW_IPV6_SUBNET_LIST {
        ULONG           dwNumEntries;
        FW_IPV6_SUBNET *pSubNets;
      } FW_IPV6_SUBNET_LIST;

typedef struct FW_IPV6_ADDRESS_RANGE {
        UCHAR  Begin [16];
        UCHAR  End [16];
      } FW_IPV6_ADDRESS_RANGE;

typedef struct FW_IPV6_RANGE_LIST {
        ULONG                  dwNumEntries;
        FW_IPV6_ADDRESS_RANGE *pRanges;
      } FW_IPV6_RANGE_LIST;

typedef struct FW_ADDRESSES {
        ULONG               dwV4AddressKeywords;
        ULONG               dwV6AddressKeywords;
        FW_IPV4_SUBNET_LIST V4SubNets;
        FW_IPV4_RANGE_LIST  V4Ranges;
        FW_IPV6_SUBNET_LIST V6SubNets;
        FW_IPV6_RANGE_LIST  V6Ranges;
      } FW_ADDRESSES;

typedef struct FW_INTERFACE_LUIDS {
        ULONG  dwNumLUIDs;
        GUID  *pLUIDs;
      } FW_INTERFACE_LUIDS;

typedef struct FW_NETWORK_NAMES {
        ULONG     dwNumEntries;
        wchar_t **wszNames;
      } FW_NETWORK_NAMES;

typedef struct FW_OS_PLATFORM {
        UCHAR  bPlatform;
        UCHAR  bMajorVersion;
        UCHAR  bMinorVersion;
        UCHAR  Reserved;
      } FW_OS_PLATFORM;

typedef struct FW_OS_PLATFORM_LIST {
        ULONG           dwNumEntries;
        FW_OS_PLATFORM *pPlatforms;
      } FW_OS_PLATFORM_LIST;

typedef struct FW_RULE2_0 {
        struct FW_RULE2_0 *pNext;
        USHORT             wSchemaVersion;
        wchar_t           *wszRuleId;
        wchar_t           *wszName;
        wchar_t           *wszDescription;
        FW_PROFILE_TYPE    dwProfiles;
        FW_DIRECTION       Direction;
        USHORT             wIpProtocol;
        union {
          struct {
            FW_PORTS LocalPorts;
            FW_PORTS RemotePorts;
          };
          FW_ICMP_TYPE_CODE_LIST V4TypeCodeList;
          FW_ICMP_TYPE_CODE_LIST V6TypeCodeList;
        };
        FW_ADDRESSES         LocalAddresses;
        FW_ADDRESSES         RemoteAddresses;
        FW_INTERFACE_LUIDS   LocalInterfaceIds;
        ULONG                dwLocalInterfaceTypes;
        wchar_t             *wszLocalApplication;
        wchar_t             *wszLocalService;
        FW_RULE_ACTION       Action;
        FW_ENUM_RULES_FLAGS  wFlags;
        wchar_t             *wszRemoteMachineAuthorizationList;
        wchar_t             *wszRemoteUserAuthorizationList;
        wchar_t             *wszEmbeddedContext;
        FW_OS_PLATFORM_LIST  PlatformValidityList;
        FW_RULE_STATUS       Status;
        FW_RULE_ORIGIN_TYPE  Origin;
        wchar_t             *wszGPOName;
        ULONG                Reserved;
      } FW_RULE2_0;

typedef struct FW_OBJECT_METADATA {
         ULONGLONG             qwFilterContextID;
         ULONG                 dwNumEntries;
         FW_ENFORCEMENT_STATE *pEnforcementStates;
       } FW_OBJECT_METADATA;

typedef struct FW_RULE {
        struct FW_RULE     *pNext;
        USHORT              wSchemaVersion;
        wchar_t            *wszRuleId;
        wchar_t            *wszName;
        wchar_t            *wszDescription;
        FW_PROFILE_TYPE     dwProfiles;
        FW_DIRECTION        Direction;
        USHORT              wIpProtocol;
        union {
          struct {
            FW_PORTS LocalPorts;
            FW_PORTS RemotePorts;
          };
          FW_ICMP_TYPE_CODE_LIST V4TypeCodeList;
          FW_ICMP_TYPE_CODE_LIST V6TypeCodeList;
        };
        FW_ADDRESSES        LocalAddresses;
        FW_ADDRESSES        RemoteAddresses;
        FW_INTERFACE_LUIDS  LocalInterfaceIds;
        ULONG               dwLocalInterfaceTypes;
        wchar_t            *wszLocalApplication;
        wchar_t            *wszLocalService;
        FW_RULE_ACTION      Action;
        FW_ENUM_RULES_FLAGS wFlags;
        wchar_t            *wszRemoteMachineAuthorizationList;
        wchar_t            *wszRemoteUserAuthorizationList;
        wchar_t            *wszEmbeddedContext;
        FW_OS_PLATFORM_LIST PlatformValidityList;
        FW_RULE_STATUS      Status;
        FW_RULE_ORIGIN_TYPE Origin;
        wchar_t            *wszGPOName;
        ULONG               Reserved;
        FW_OBJECT_METADATA *pMetaData;
        wchar_t            *wszLocalUserAuthorizationList;
        wchar_t            *wszPackageId;
        wchar_t            *wszLocalUserOwner;
        unsigned long       dwTrustTupleKeywords;
        FW_NETWORK_NAMES    OnNetworkNames;
        wchar_t            *wszSecurityRealmId;
        unsigned short      wFlags2;
        FW_NETWORK_NAMES    RemoteOutServerNames;
        wchar_t            *Fqbn;            /* since RS1 or RS2? */
        ULONG               compartmentId;
      } FW_RULE;

/*
 * http://msdn.microsoft.com/en-us/library/cc231461.aspx
 */
#define FW_VISTA_SCHEMA_VERSION        0x0200
#define FW_SERVER2K8_BINARY_VERSION    0x0201
#define FW_SERVER2K8_SCHEMA_VERSION    0x0201
#define FW_SEVEN_BINARY_VERSION        0x020A
#define FW_SEVEN_SCHEMA_VERSION        0x020A
#define FW_WIN8_1_BINARY_VERSION       0x0214
#define FW_WIN10_BINARY_VERSION        0x0216
#define FW_THRESHOLD_BINARY_VERSION    0x0218
#define FW_THRESHOLD2_BINARY_VERSION   0x0219
#define FW_REDSTONE1_BINARY_VERSION    0x021A
#define FW_REDSTONE2_BINARY_VERSION    0x021B

#define FWP_DIRECTION_IN               0x00003900L
#define FWP_DIRECTION_OUT              0x00003901L
#define FWP_DIRECTION_FORWARD          0x00003902L
#define FWP_DIRECTION_FORWARD2         0x00003903L

#ifndef FWPM_SESSION_FLAG_DYNAMIC
#define FWPM_SESSION_FLAG_DYNAMIC      0x00000001
#endif

typedef struct _FWPM_NET_EVENT_CLASSIFY_DROP0 {
        UINT64  filterId;
        UINT16  layerId;
     } _FWPM_NET_EVENT_CLASSIFY_DROP0;

typedef struct _FWPM_NET_EVENT_CLASSIFY_DROP1 {
        UINT64        filterId;
        UINT16        layerId;
        UINT32        reauthReason;
        UINT32        originalProfile;
        UINT32        currentProfile;
        UINT32        msFwpDirection;
        BOOL          isLoopback;
      } _FWPM_NET_EVENT_CLASSIFY_DROP1;

typedef struct _FWPM_NET_EVENT_CLASSIFY_DROP2 {
        UINT64        filterId;
        UINT16        layerId;
        UINT32        reauthReason;
        UINT32        originalProfile;
        UINT32        currentProfile;
        UINT32        msFwpDirection;
        BOOL          isLoopback;
        FWP_BYTE_BLOB vSwitchId;
        UINT32        vSwitchSourcePort;
        UINT32        vSwitchDestinationPort;
      } _FWPM_NET_EVENT_CLASSIFY_DROP2;

typedef struct _FWPM_NET_EVENT_CLASSIFY_ALLOW0 {
        UINT64        filterId;
        UINT16        layerId;
        UINT32        reauthReason;
        UINT32        originalProfile;
        UINT32        currentProfile;
        UINT32        msFwpDirection;
        BOOL          isLoopback;
      } _FWPM_NET_EVENT_CLASSIFY_ALLOW0;

typedef struct _FWPM_NET_EVENT_CAPABILITY_DROP0 {
        FWPM_APPC_NETWORK_CAPABILITY_TYPE networkCapabilityId;
        UINT64                            filterId;
        BOOL                              isLoopback;
      } _FWPM_NET_EVENT_CAPABILITY_DROP0;

typedef struct _FWPM_NET_EVENT_CAPABILITY_ALLOW0 {
        FWPM_APPC_NETWORK_CAPABILITY_TYPE networkCapabilityId;
        UINT64                            filterId;
        BOOL                              isLoopback;
      } _FWPM_NET_EVENT_CAPABILITY_ALLOW0;

typedef struct _FWPM_NET_EVENT_HEADER0 {
        FILETIME           timeStamp;
        UINT32             flags;
        FWP_IP_VERSION     ipVersion;
        UINT8              ipProtocol;
        union {
          UINT32           localAddrV4;
          FWP_BYTE_ARRAY16 localAddrV6;
        };
        union {
          UINT32           remoteAddrV4;
          FWP_BYTE_ARRAY16 remoteAddrV6;
        };
        UINT16             localPort;
        UINT16             remotePort;
        UINT32             scopeId;
        FWP_BYTE_BLOB      appId;
        SID               *userId;
      } _FWPM_NET_EVENT_HEADER0;

typedef struct _FWPM_NET_EVENT_HEADER1 {
        FILETIME           timeStamp;
        UINT32             flags;
        FWP_IP_VERSION     ipVersion;
        UINT8              ipProtocol;
        union {
          UINT32           localAddrV4;
          FWP_BYTE_ARRAY16 localAddrV6;
        };
        union {
          UINT32           remoteAddrV4;
          FWP_BYTE_ARRAY16 remoteAddrV6;
        };
        UINT16             localPort;
        UINT16             remotePort;
        UINT32             scopeId;
        FWP_BYTE_BLOB      appId;
        SID               *userId;
        union {
          struct {
            FWP_AF reserved1;
            union {
              struct {
                FWP_BYTE_ARRAY6 reserved2;
                FWP_BYTE_ARRAY6 reserved3;
                UINT32          reserved4;
                UINT32          reserved5;
                UINT16          reserved6;
                UINT32          reserved7;
                UINT32          reserved8;
                UINT16          reserved9;
                UINT64          reserved10;
              };
            };
          };
        };
      } _FWPM_NET_EVENT_HEADER1;

typedef struct _FWPM_NET_EVENT_HEADER2 {
        FILETIME           timeStamp;
        UINT32             flags;
        FWP_IP_VERSION     ipVersion;
        UINT8              ipProtocol;
        union {
          UINT32           localAddrV4;
          FWP_BYTE_ARRAY16 localAddrV6;
        };
        union {
          UINT32           remoteAddrV4;
          FWP_BYTE_ARRAY16 remoteAddrV6;
        };
        UINT16             localPort;
        UINT16             remotePort;
        UINT32             scopeId;
        FWP_BYTE_BLOB      appId;
        SID               *userId;
        FWP_AF             addressFamily;
        SID               *packageSid;
      } _FWPM_NET_EVENT_HEADER2;

typedef struct _FWPM_NET_EVENT_HEADER3 {
        FILETIME            timeStamp;
        UINT32              flags;
        FWP_IP_VERSION      ipVersion;
        UINT8               ipProtocol;
        union {
          UINT32            localAddrV4;
          FWP_BYTE_ARRAY16  localAddrV6;
        };
        union {
          UINT32            remoteAddrV4;
          FWP_BYTE_ARRAY16  remoteAddrV6;
        };
        UINT16              localPort;
        UINT16              remotePort;
        UINT32              scopeId;
        FWP_BYTE_BLOB       appId;
        SID                *userId;
        FWP_AF              addressFamily;
        SID                *packageSid;
        wchar_t            *enterpriseId;
        UINT64              policyFlags;
        FWP_BYTE_BLOB       effectiveName;
      } _FWPM_NET_EVENT_HEADER3;

typedef struct _FWPM_FILTER_CONDITION0 {
        GUID                  fieldKey;
        FWP_MATCH_TYPE        matchType;
        FWP_CONDITION_VALUE0  conditionValue;
      } _FWPM_FILTER_CONDITION0;

typedef struct _FWPM_NET_EVENT_ENUM_TEMPLATE0 {
        FILETIME                  startTime;
        FILETIME                  endTime;
        UINT32                    numFilterConditions;
        _FWPM_FILTER_CONDITION0  *filterCondition;
      } _FWPM_NET_EVENT_ENUM_TEMPLATE0;

typedef struct _FWPM_NET_EVENT_SUBSCRIPTION0 {
        _FWPM_NET_EVENT_ENUM_TEMPLATE0 *enumTemplate;
        UINT32                          flags;
        GUID                            sessionKey;
      } _FWPM_NET_EVENT_SUBSCRIPTION0;

#if defined(__MINGW32__) || defined(__CYGWIN__)
  typedef struct FWPM_LAYER_STATISTICS0 {
          GUID    layerId;
          UINT32  classifyPermitCount;
          UINT32  classifyBlockCount;
          UINT32  classifyVetoCount;
          UINT32  numCacheEntries;
        } FWPM_LAYER_STATISTICS0;

  typedef struct FWPM_STATISTICS0 {
          UINT32                  numLayerStatistics;
          FWPM_LAYER_STATISTICS0 *layerStatistics;
          UINT32                  inboundAllowedConnectionsV4;
          UINT32                  inboundBlockedConnectionsV4;
          UINT32                  outboundAllowedConnectionsV4;
          UINT32                  outboundBlockedConnectionsV4;
          UINT32                  inboundAllowedConnectionsV6;
          UINT32                  inboundBlockedConnectionsV6;
          UINT32                  outboundAllowedConnectionsV6;
          UINT32                  outboundBlockedConnectionsV6;
          UINT32                  inboundActiveConnectionsV4;
          UINT32                  outboundActiveConnectionsV4;
          UINT32                  inboundActiveConnectionsV6;
          UINT32                  outboundActiveConnectionsV6;
          UINT64                  reauthDirInbound;
          UINT64                  reauthDirOutbound;
          UINT64                  reauthFamilyV4;
          UINT64                  reauthFamilyV6;
          UINT64                  reauthProtoOther;
          UINT64                  reauthProtoIPv4;
          UINT64                  reauthProtoIPv6;
          UINT64                  reauthProtoICMP;
          UINT64                  reauthProtoICMP6;
          UINT64                  reauthProtoUDP;
          UINT64                  reauthProtoTCP;
          UINT64                  reauthReasonPolicyChange;
          UINT64                  reauthReasonNewArrivalInterface;
          UINT64                  reauthReasonNewNextHopInterface;
          UINT64                  reauthReasonProfileCrossing;
          UINT64                  reauthReasonClassifyCompletion;
          UINT64                  reauthReasonIPSecPropertiesChanged;
          UINT64                  reauthReasonMidStreamInspection;
          UINT64                  reauthReasonSocketPropertyChanged;
          UINT64                  reauthReasonNewInboundMCastBCastPacket;
          UINT64                  reauthReasonEDPPolicyChanged;
          UINT64                  reauthReasonPreclassifyLocalAddrLayerChange;
          UINT64                  reauthReasonPreclassifyRemoteAddrLayerChange;
          UINT64                  reauthReasonPreclassifyLocalPortLayerChange;
          UINT64                  reauthReasonPreclassifyRemotePortLayerChange;
          UINT64                  reauthReasonProxyHandleChanged;
        } FWPM_STATISTICS0;

  #define FWPM_NET_EVENT                           FWPM_NET_EVENT2
  #define FWPM_SESSION                             FWPM_SESSION0
  #define FWPM_STATISTICS                          FWPM_STATISTICS0
  #define FWP_VALUE                                FWP_VALUE0

  #define FWPM_NET_EVENT_KEYWORD_CAPABILITY_DROP   0x00000004
  #define FWPM_NET_EVENT_KEYWORD_CAPABILITY_ALLOW  0x00000008
  #define FWPM_NET_EVENT_KEYWORD_CLASSIFY_ALLOW    0x00000010

  #define FWPM_NET_EVENT_FLAG_IP_PROTOCOL_SET      0x00000001
  #define FWPM_NET_EVENT_FLAG_LOCAL_ADDR_SET       0x00000002
  #define FWPM_NET_EVENT_FLAG_REMOTE_ADDR_SET      0x00000004
  #define FWPM_NET_EVENT_FLAG_LOCAL_PORT_SET       0x00000008
  #define FWPM_NET_EVENT_FLAG_REMOTE_PORT_SET      0x00000010
  #define FWPM_NET_EVENT_FLAG_APP_ID_SET           0x00000020
  #define FWPM_NET_EVENT_FLAG_USER_ID_SET          0x00000040
  #define FWPM_NET_EVENT_FLAG_SCOPE_ID_SET         0x00000080
  #define FWPM_NET_EVENT_FLAG_IP_VERSION_SET       0x00000100
  #define FWPM_NET_EVENT_FLAG_REAUTH_REASON_SET    0x00000200
  #define FWPM_NET_EVENT_FLAG_PACKAGE_ID_SET       0x00000400
  #define FWPM_NET_EVENT_FLAG_ENTERPRISE_ID_SET    0x00000800
  #define FWPM_NET_EVENT_FLAG_POLICY_FLAGS_SET     0x00001000
  #define FWPM_NET_EVENT_FLAG_EFFECTIVE_NAME_SET   0x00002000

  #define FWPM_ENGINE_MONITOR_IPSEC_CONNECTIONS    3
#endif  /* __MINGW32__ || __CYGWIN__ */

/*
 * These are not in any MinGW SDK. So just define them here.
 */
typedef struct _FWPM_NET_EVENT0 {
        _FWPM_NET_EVENT_HEADER0 header;
        FWPM_NET_EVENT_TYPE     type;
        union {
            _FWPM_NET_EVENT_CLASSIFY_DROP0      *classifyDrop;
          /* FWPM_NET_EVENT_IKEEXT_MM_FAILURE0  *ikeMmFailure; Not needed */
          /* FWPM_NET_EVENT_IKEEXT_QM_FAILURE0  *ikeQmFailure; Not needed */
          /* FWPM_NET_EVENT_IKEEXT_EM_FAILURE0  *ikeEmFailure; Not needed */
          /* FWPM_NET_EVENT_IPSEC_KERNEL_DROP0  *ipsecDrop;    Not needed */
          /* FWPM_NET_EVENT_IPSEC_DOSP_DROP0    *idpDrop;      Not needed */
        };
      }  _FWPM_NET_EVENT0;

typedef struct _FWPM_NET_EVENT1 {
        _FWPM_NET_EVENT_HEADER1 header;
        FWPM_NET_EVENT_TYPE     type;
        union {
            _FWPM_NET_EVENT_CLASSIFY_DROP1      *classifyDrop;
          /* FWPM_NET_EVENT_IKEEXT_MM_FAILURE1  *ikeMmFailure;  Not needed */
          /* FWPM_NET_EVENT_IKEEXT_QM_FAILURE0  *ikeQmFailure;  Not needed */
          /* FWPM_NET_EVENT_IKEEXT_EM_FAILURE1  *ikeEmFailure;  Not needed */
          /* FWPM_NET_EVENT_IPSEC_KERNEL_DROP0  *ipsecDrop;     Not needed */
          /* FWPM_NET_EVENT_IPSEC_DOSP_DROP0    *idpDrop;       Not needed */
        };
      } _FWPM_NET_EVENT1;

typedef struct _FWPM_NET_EVENT2 {
        _FWPM_NET_EVENT_HEADER2 header;
        FWPM_NET_EVENT_TYPE     type;
        union {
            _FWPM_NET_EVENT_CLASSIFY_DROP2      *classifyDrop;
            _FWPM_NET_EVENT_CLASSIFY_ALLOW0     *classifyAllow;
            _FWPM_NET_EVENT_CAPABILITY_DROP0    *capabilityDrop;
            _FWPM_NET_EVENT_CAPABILITY_ALLOW0   *capabilityAllow;
          /* FWPM_NET_EVENT_IKEEXT_MM_FAILURE1  *ikeMmFailure;     Not needed */
          /* FWPM_NET_EVENT_IKEEXT_QM_FAILURE0  *ikeQmFailure;     Not needed */
          /* FWPM_NET_EVENT_IKEEXT_EM_FAILURE1  *ikeEmFailure;     Not needed */
          /* FWPM_NET_EVENT_IPSEC_KERNEL_DROP0  *ipsecDrop;        Not needed */
          /* FWPM_NET_EVENT_IPSEC_DOSP_DROP0    *idpDrop;          Not needed */
          /* FWPM_NET_EVENT_CLASSIFY_DROP_MAC0  *classifyDropMac;  Not needed */
        };
      } _FWPM_NET_EVENT2;

typedef struct _FWPM_NET_EVENT3 {
        _FWPM_NET_EVENT_HEADER3 header;
        FWPM_NET_EVENT_TYPE     type;
        union {
            _FWPM_NET_EVENT_CLASSIFY_DROP2      *classifyDrop;
            _FWPM_NET_EVENT_CLASSIFY_ALLOW0     *classifyAllow;
            _FWPM_NET_EVENT_CAPABILITY_DROP0    *capabilityDrop;
            _FWPM_NET_EVENT_CAPABILITY_ALLOW0   *capabilityAllow;
          /* FWPM_NET_EVENT_IKEEXT_MM_FAILURE1  *ikeMmFailure;    Not needed */
          /* FWPM_NET_EVENT_IKEEXT_QM_FAILURE0  *ikeQmFailure;    Not needed */
          /* FWPM_NET_EVENT_IKEEXT_EM_FAILURE1  *ikeEmFailure;    Not needed */
          /* FWPM_NET_EVENT_IPSEC_KERNEL_DROP0  *ipsecDrop;       Not needed */
          /* FWPM_NET_EVENT_IPSEC_DOSP_DROP0    *idpDrop;         Not needed */
          /* FWPM_NET_EVENT_CLASSIFY_DROP_MAC0  *classifyDropMac; Not needed */
        };
      } _FWPM_NET_EVENT3;

typedef struct _FWPM_NET_EVENT4 {
        _FWPM_NET_EVENT_HEADER3 header;
        FWPM_NET_EVENT_TYPE     type;
        union {
            _FWPM_NET_EVENT_CLASSIFY_DROP2      *classifyDrop;
            _FWPM_NET_EVENT_CLASSIFY_ALLOW0     *classifyAllow;
            _FWPM_NET_EVENT_CAPABILITY_DROP0    *capabilityDrop;
            _FWPM_NET_EVENT_CAPABILITY_ALLOW0   *capabilityAllow;
          /* FWPM_NET_EVENT_IKEEXT_MM_FAILURE2  *ikeMmFailure;    Not needed */
          /* FWPM_NET_EVENT_IKEEXT_QM_FAILURE1  *ikeQmFailure;    Not needed */
          /* FWPM_NET_EVENT_IKEEXT_EM_FAILURE1  *ikeEmFailure;    Not needed */
          /* FWPM_NET_EVENT_IPSEC_KERNEL_DROP0  *ipsecDrop;       Not needed */
          /* FWPM_NET_EVENT_IPSEC_DOSP_DROP0    *idpDrop;         Not needed */
          /* FWPM_NET_EVENT_CLASSIFY_DROP_MAC0  *classifyDropMac; Not needed */
        };
      } _FWPM_NET_EVENT4;

typedef struct _FWPM_NET_EVENT5 {
        _FWPM_NET_EVENT_HEADER3 header;
        FWPM_NET_EVENT_TYPE     type;
        union {
            _FWPM_NET_EVENT_CLASSIFY_DROP2       *classifyDrop;
            _FWPM_NET_EVENT_CLASSIFY_ALLOW0      *classifyAllow;
            _FWPM_NET_EVENT_CAPABILITY_DROP0     *capabilityDrop;
            _FWPM_NET_EVENT_CAPABILITY_ALLOW0    *capabilityAllow;
          /* FWPM_NET_EVENT_IKEEXT_MM_FAILURE2   *ikeMmFailure;     Not needed */
          /* FWPM_NET_EVENT_IKEEXT_QM_FAILURE1   *ikeQmFailure;     Not needed */
          /* FWPM_NET_EVENT_IKEEXT_EM_FAILURE1   *ikeEmFailure;     Not needed */
          /* FWPM_NET_EVENT_IPSEC_KERNEL_DROP0   *ipsecDrop;        Not needed */
          /* FWPM_NET_EVENT_IPSEC_DOSP_DROP0     *idpDrop;          Not needed */
          /* FWPM_NET_EVENT_CLASSIFY_DROP_MAC0   *classifyDropMac;  Not needed */
          /* FWPM_NET_EVENT_LPM_PACKET_ARRIVAL0  *lpmPacketArrival; Not needed */
        };
      } _FWPM_NET_EVENT5;

typedef void (CALLBACK *_FWPM_NET_EVENT_CALLBACK0) (void *context,
                                                    const _FWPM_NET_EVENT1 *event);

typedef void (CALLBACK *_FWPM_NET_EVENT_CALLBACK1) (void                   *context,
                                                    const _FWPM_NET_EVENT2 *event);

typedef void (CALLBACK *_FWPM_NET_EVENT_CALLBACK2) (void                   *context,
                                                    const _FWPM_NET_EVENT3 *event);

typedef void (CALLBACK *_FWPM_NET_EVENT_CALLBACK3) (void                   *context,
                                                    const _FWPM_NET_EVENT4 *event);

typedef void (CALLBACK *_FWPM_NET_EVENT_CALLBACK4) (void                   *context,
                                                    const _FWPM_NET_EVENT5 *event);

/**
 * \def DEF_FUNC(ret, f, args)
 *
 * Macro to both define and declare the function-pointer.
 *
 * \param[in] ret The return value of the typdef'ed function-pointer.
 * \param[in] f   The function name.
 * \param[in] f   The function arguments as a list of `(arg1, arg2, ...)`.
 */
#define DEF_FUNC(ret, f, args)  typedef ret (WINAPI *func_##f) args; \
                                static func_##f  p_##f = NULL

/*
 * "FwpUclnt.dll" typedefs and functions pointers:
 */
DEF_FUNC (DWORD, FwpmNetEventSubscribe0, (HANDLE                               engine_handle,
                                          const _FWPM_NET_EVENT_SUBSCRIPTION0 *subscription,
                                          _FWPM_NET_EVENT_CALLBACK0            callback,
                                          void                                *context,
                                          HANDLE                              *events_handle));

DEF_FUNC (DWORD, FwpmNetEventSubscribe1, (HANDLE                               engine_handle,
                                          const _FWPM_NET_EVENT_SUBSCRIPTION0 *subscription,
                                          _FWPM_NET_EVENT_CALLBACK1            callback,
                                          void                                *context,
                                          HANDLE                              *events_handle));

DEF_FUNC (DWORD, FwpmNetEventSubscribe2, (HANDLE                               engine_handle,
                                          const _FWPM_NET_EVENT_SUBSCRIPTION0 *subscription,
                                          _FWPM_NET_EVENT_CALLBACK2            callback,
                                          void                                *context,
                                          HANDLE                              *events_handle));

DEF_FUNC (DWORD, FwpmNetEventSubscribe3, (HANDLE                               engine_handle,
                                          const _FWPM_NET_EVENT_SUBSCRIPTION0 *subscription,
                                          _FWPM_NET_EVENT_CALLBACK3            callback,
                                          void                                *context,
                                          HANDLE                              *events_handle));

DEF_FUNC (DWORD, FwpmNetEventSubscribe4, (HANDLE                               engine_handle,
                                          const _FWPM_NET_EVENT_SUBSCRIPTION0 *subscription,
                                          _FWPM_NET_EVENT_CALLBACK4            callback,
                                          void                                *context,
                                          HANDLE                              *events_handle));

DEF_FUNC (DWORD, FwpmNetEventUnsubscribe0, (HANDLE engine_handle,
                                            HANDLE events_handle));

DEF_FUNC (DWORD, FwpmEngineOpen0, (const wchar_t             *server_name,
                                   UINT32                     authn_service,
                                   SEC_WINNT_AUTH_IDENTITY_W *auth_identity,
                                   const FWPM_SESSION0       *session,
                                   HANDLE                    *engine_handle));

DEF_FUNC (DWORD, FwpmEngineSetOption0, (HANDLE             engine_handle,
                                        FWPM_ENGINE_OPTION option,
                                        const FWP_VALUE0  *new_value));

DEF_FUNC (DWORD, FwpmLayerGetById0, (HANDLE        engine_handle,
                                     UINT16        id,
                                     FWPM_LAYER0 **layer));

DEF_FUNC (DWORD, FwpmFilterGetById0, (HANDLE         engine_handle,
                                      UINT64         id,
                                      FWPM_FILTER0 **filter));

DEF_FUNC (void, FwpmFreeMemory0, (void **p));

DEF_FUNC (DWORD, FwpmEngineClose0, (HANDLE engine_handle));

DEF_FUNC (DWORD, FwpmCalloutCreateEnumHandle0, (HANDLE                             engine_handle,
                                                const FWPM_CALLOUT_ENUM_TEMPLATE0 *enum_template,
                                                HANDLE                            *enum_handle));

DEF_FUNC (DWORD, FwpmCalloutEnum0, (HANDLE           engine_handle,
                                    HANDLE           enum_handle,
                                    UINT32           num_entries_requested,
                                    FWPM_CALLOUT0 ***entries,
                                    UINT32          *num_entries_returned));

DEF_FUNC (DWORD, FwpmCalloutDestroyEnumHandle0, (HANDLE engine_handle,
                                                 HANDLE enum_handle));

/*
 * For fw_dump_events():
 */
DEF_FUNC (DWORD, FwpmNetEventCreateEnumHandle0,
                  (HANDLE                                engine_handle,
                   const _FWPM_NET_EVENT_ENUM_TEMPLATE0 *enum_template,
                   HANDLE                               *enum_handle));

DEF_FUNC (DWORD, FwpmNetEventDestroyEnumHandle0, (HANDLE engine_handle,
                                                  HANDLE enum_handle));

#define DEF_NetEventEnum(N) \
        DEF_FUNC (DWORD, FwpmNetEventEnum##N,                           \
                          (HANDLE                engine_handle,         \
                           HANDLE                enum_handle,           \
                           UINT32                num_entries_requested, \
                           _FWPM_NET_EVENT##N ***entries,               \
                           UINT32               *num_entries_returned))
DEF_NetEventEnum (0);
DEF_NetEventEnum (1);
DEF_NetEventEnum (2);
DEF_NetEventEnum (3);
DEF_NetEventEnum (4);
DEF_NetEventEnum (5);

/*
 * "FirewallAPI.dll" typedefs and functions pointers:
 */
DEF_FUNC (ULONG, FWOpenPolicyStore, (USHORT                   binary_version,
                                     wchar_t                 *machine_or_GPO,
                                     FW_STORE_TYPE           store_type,
                                     FW_POLICY_ACCESS_RIGHT  access_right,
                                     FW_POLICY_STORE_FLAGS   flags,
                                     HANDLE                 *policy));

DEF_FUNC (ULONG, FWEnumFirewallRules, (HANDLE                 policy_store,
                                       FW_RULE_STATUS_CLASS   filtered_by_status,
                                       FW_PROFILE_TYPE        profile_filter,
                                       FW_ENUM_RULES_FLAGS    flags,
                                       ULONG                 *num_rules,
                                       FW_RULE              **rules));

DEF_FUNC (ULONG, FWStatusMessageFromStatusCode, (FW_RULE_STATUS status_code,
                                                 wchar_t       *msg,
                                                 ULONG         *msg_size));

DEF_FUNC (ULONG, FWFreeFirewallRules, (FW_RULE *pFwRules));
DEF_FUNC (ULONG, FWClosePolicyStore, (HANDLE *policy_store));

/**
 * \def ADD_VALUE(dll, func)
 *
 * Add the function-pointer value `p_XXfunc` to the `fw_funcs[]` array.
 *
 * \param[in] dll  The name of the .DLL to use for `LoadLibrary()`.
 * \param[in  func The name of the function to  use for `GetProcAddress()`.
 */
#define ADD_VALUE(dll, func)   { TRUE, NULL, dll, #func, (void**)&p_##func }

static struct LoadTable fw_funcs[] = {
              ADD_VALUE ("FirewallAPI.dll", FWOpenPolicyStore),
              ADD_VALUE ("FirewallAPI.dll", FWClosePolicyStore),
              ADD_VALUE ("FirewallAPI.dll", FWEnumFirewallRules),
              ADD_VALUE ("FirewallAPI.dll", FWFreeFirewallRules),
              ADD_VALUE ("FirewallAPI.dll", FWStatusMessageFromStatusCode),
              ADD_VALUE ("FwpUclnt.dll",    FwpmNetEventSubscribe0),
              ADD_VALUE ("FwpUclnt.dll",    FwpmNetEventSubscribe1),
              ADD_VALUE ("FwpUclnt.dll",    FwpmNetEventSubscribe2),
              ADD_VALUE ("FwpUclnt.dll",    FwpmNetEventSubscribe3),    /* Win10 RS4+ */
              ADD_VALUE ("FwpUclnt.dll",    FwpmNetEventSubscribe4),    /* Win10 RS5+ */
              ADD_VALUE ("FwpUclnt.dll",    FwpmNetEventUnsubscribe0),
              ADD_VALUE ("FwpUclnt.dll",    FwpmFreeMemory0),
              ADD_VALUE ("FwpUclnt.dll",    FwpmEngineClose0),
              ADD_VALUE ("FwpUclnt.dll",    FwpmEngineOpen0),
              ADD_VALUE ("FwpUclnt.dll",    FwpmEngineSetOption0),
              ADD_VALUE ("FwpUclnt.dll",    FwpmLayerGetById0),
              ADD_VALUE ("FwpUclnt.dll",    FwpmFilterGetById0),
              ADD_VALUE ("FwpUclnt.dll",    FwpmCalloutCreateEnumHandle0),
              ADD_VALUE ("FwpUclnt.dll",    FwpmCalloutEnum0),
              ADD_VALUE ("FwpUclnt.dll",    FwpmCalloutDestroyEnumHandle0),
              ADD_VALUE ("FwpUclnt.dll",    FwpmNetEventCreateEnumHandle0),
              ADD_VALUE ("FwpUclnt.dll",    FwpmNetEventDestroyEnumHandle0),
              ADD_VALUE ("FwpUclnt.dll",    FwpmNetEventEnum0),
              ADD_VALUE ("FwpUclnt.dll",    FwpmNetEventEnum1),
              ADD_VALUE ("FwpUclnt.dll",    FwpmNetEventEnum2),
              ADD_VALUE ("FwpUclnt.dll",    FwpmNetEventEnum3),
              ADD_VALUE ("FwpUclnt.dll",    FwpmNetEventEnum4),
              ADD_VALUE ("FwpUclnt.dll",    FwpmNetEventEnum5),
              ADD_VALUE ("kernel32.dll",    GetSystemTimePreciseAsFileTime),
            };

#undef  ADD_VALUE
#define ADD_VALUE(v)  { _FWPM_NET_EVENT_TYPE_##v, "FWPM_NET_EVENT_TYPE_" #v }

static const struct search_list events[] = {
                    ADD_VALUE (CLASSIFY_DROP),
                    ADD_VALUE (CLASSIFY_ALLOW),
                    ADD_VALUE (CAPABILITY_DROP),
                    ADD_VALUE (CAPABILITY_ALLOW),
                    ADD_VALUE (CLASSIFY_DROP_MAC),
                    ADD_VALUE (IKEEXT_MM_FAILURE),
                    ADD_VALUE (IKEEXT_QM_FAILURE),
                    ADD_VALUE (IKEEXT_EM_FAILURE),
                    ADD_VALUE (IPSEC_KERNEL_DROP),
                    ADD_VALUE (IPSEC_DOSP_DROP),
                    ADD_VALUE (LPM_PACKET_ARRIVAL),
                    ADD_VALUE (MAX)
                  };

#undef  ADD_VALUE
#define ADD_VALUE(v)  { FWPM_NET_EVENT_FLAG_##v, "FWPM_NET_EVENT_FLAG_" #v }

static const struct search_list ev_flags[] = {
                    ADD_VALUE (IP_PROTOCOL_SET),
                    ADD_VALUE (LOCAL_ADDR_SET),
                    ADD_VALUE (REMOTE_ADDR_SET),
                    ADD_VALUE (LOCAL_PORT_SET),
                    ADD_VALUE (REMOTE_PORT_SET),
                    ADD_VALUE (APP_ID_SET),
                    ADD_VALUE (USER_ID_SET),
                    ADD_VALUE (SCOPE_ID_SET),
                    ADD_VALUE (IP_VERSION_SET),
                    ADD_VALUE (REAUTH_REASON_SET),
                    ADD_VALUE (PACKAGE_ID_SET),
                    ADD_VALUE (ENTERPRISE_ID_SET),
                    ADD_VALUE (POLICY_FLAGS_SET),
                    ADD_VALUE (EFFECTIVE_NAME_SET)
     };

#undef  ADD_VALUE
#define ADD_VALUE(v)  { FWP_DIRECTION_##v, #v }

static const struct search_list directions[] = {
                    ADD_VALUE (IN),
                    ADD_VALUE (INBOUND),
                    ADD_VALUE (OUT),
                    ADD_VALUE (OUTBOUND),
                    ADD_VALUE (FORWARD),
                    ADD_VALUE (FORWARD2)
                  };

/*
 * Copied from dump.c:
 */
#define _IPPROTO_HOPOPTS               0
#define _IPPROTO_ICMP                  1
#define _IPPROTO_IGMP                  2
#define _IPPROTO_GGP                   3
#define _IPPROTO_IPV4                  4
#define _IPPROTO_ST                    5
#define _IPPROTO_TCP                   6
#define _IPPROTO_CBT                   7
#define _IPPROTO_EGP                   8
#define _IPPROTO_IGP                   9
#define _IPPROTO_PUP                   12
#define _IPPROTO_UDP                   17
#define _IPPROTO_IDP                   22
#define _IPPROTO_RDP                   27
#define _IPPROTO_IPV6                  41
#define _IPPROTO_ROUTING               43
#define _IPPROTO_FRAGMENT              44
#define _IPPROTO_ESP                   50
#define _IPPROTO_AH                    51
#define _IPPROTO_ICMPV6                58
#define _IPPROTO_NONE                  59
#define _IPPROTO_DSTOPTS               60
#define _IPPROTO_ND                    77
#define _IPPROTO_ICLFXBM               78
#define _IPPROTO_PIM                   103
#define _IPPROTO_PGM                   113
#define _IPPROTO_RM                    113
#define _IPPROTO_L2TP                  115
#define _IPPROTO_SCTP                  132
#define _IPPROTO_RAW                   255
#define _IPPROTO_MAX                   256
#define _IPPROTO_RESERVED_RAW          257
#define _IPPROTO_RESERVED_IPSEC        258
#define _IPPROTO_RESERVED_IPSECOFFLOAD 259
#define _IPPROTO_RESERVED_WNV          260
#define _IPPROTO_RESERVED_MAX          261

#undef  ADD_VALUE
#define ADD_VALUE(v)  { _IPPROTO_##v, "IPPROTO_" #v }

static const struct search_list protocols[] = {
                    ADD_VALUE (ICMP),
                    ADD_VALUE (IGMP),
                    ADD_VALUE (TCP),
                    ADD_VALUE (UDP),
                    ADD_VALUE (ICMPV6),
                    ADD_VALUE (RM),
                    ADD_VALUE (RAW),
                    ADD_VALUE (HOPOPTS),
                    ADD_VALUE (GGP),
                    ADD_VALUE (IPV4),
                    ADD_VALUE (IPV6),
                    ADD_VALUE (ST),
                    ADD_VALUE (CBT),
                    ADD_VALUE (EGP),
                    ADD_VALUE (IGP),
                    ADD_VALUE (PUP),
                    ADD_VALUE (IDP),
                    ADD_VALUE (RDP),
                    ADD_VALUE (ROUTING),
                    ADD_VALUE (FRAGMENT),
                    ADD_VALUE (ESP),
                    ADD_VALUE (AH),
                    ADD_VALUE (DSTOPTS),
                    ADD_VALUE (ND),
                    ADD_VALUE (ICLFXBM),
                    ADD_VALUE (PIM),
                    ADD_VALUE (PGM),
                    ADD_VALUE (L2TP),
                    ADD_VALUE (SCTP),
                    ADD_VALUE (NONE),
                    ADD_VALUE (RAW),
                    ADD_VALUE (RESERVED_IPSEC),
                    ADD_VALUE (RESERVED_IPSECOFFLOAD),
                    ADD_VALUE (RESERVED_WNV),
                    ADD_VALUE (RESERVED_RAW),
                    ADD_VALUE (RESERVED_IPSEC),
                    ADD_VALUE (RESERVED_IPSECOFFLOAD),
                    ADD_VALUE (RESERVED_WNV),
                    ADD_VALUE (RESERVED_MAX)
                  };

#ifndef FWP_CALLOUT_FLAG_CONDITIONAL_ON_FLOW
#define FWP_CALLOUT_FLAG_CONDITIONAL_ON_FLOW         0x00000001
#endif

#ifndef FWP_CALLOUT_FLAG_ALLOW_OFFLOAD
#define FWP_CALLOUT_FLAG_ALLOW_OFFLOAD               0x00000002
#endif

#ifndef FWP_CALLOUT_FLAG_ENABLE_COMMIT_ADD_NOTIFY
#define FWP_CALLOUT_FLAG_ENABLE_COMMIT_ADD_NOTIFY    0x00000004
#endif

#ifndef FWP_CALLOUT_FLAG_ALLOW_MID_STREAM_INSPECTION
#define FWP_CALLOUT_FLAG_ALLOW_MID_STREAM_INSPECTION 0x00000008
#endif

#ifndef FWP_CALLOUT_FLAG_ALLOW_RECLASSIFY
#define FWP_CALLOUT_FLAG_ALLOW_RECLASSIFY            0x00000010
#endif

#ifndef FWP_CALLOUT_FLAG_RESERVED1
#define FWP_CALLOUT_FLAG_RESERVED1                   0x00000020
#endif

#ifndef FWP_CALLOUT_FLAG_ALLOW_RSC
#define FWP_CALLOUT_FLAG_ALLOW_RSC                   0x00000040
#endif

#ifndef FWP_CALLOUT_FLAG_ALLOW_L2_BATCH_CLASSIFY
#define FWP_CALLOUT_FLAG_ALLOW_L2_BATCH_CLASSIFY     0x00000080
#endif

#ifndef FWPM_CALLOUT_FLAG_PERSISTENT
#define FWPM_CALLOUT_FLAG_PERSISTENT                 0x00010000
#endif

#ifndef FWPM_CALLOUT_FLAG_USES_PROVIDER_CONTEXT
#define FWPM_CALLOUT_FLAG_USES_PROVIDER_CONTEXT      0x00020000
#endif

#ifndef FWPM_CALLOUT_FLAG_REGISTERED
#define FWPM_CALLOUT_FLAG_REGISTERED                 0x00040000
#endif

#undef  ADD_VALUE
#define ADD_VALUE(v)  { v, #v }

/* Enter flags with highest bit first.
 */
static const struct search_list callout_flags[] = {
                    ADD_VALUE (FWPM_CALLOUT_FLAG_REGISTERED),
                    ADD_VALUE (FWPM_CALLOUT_FLAG_USES_PROVIDER_CONTEXT),
                    ADD_VALUE (FWPM_CALLOUT_FLAG_PERSISTENT),
                    ADD_VALUE (FWP_CALLOUT_FLAG_ALLOW_L2_BATCH_CLASSIFY),
                    ADD_VALUE (FWP_CALLOUT_FLAG_ALLOW_RSC),
                 /* ADD_VALUE (FWP_CALLOUT_FLAG_RESERVED1), */
                    ADD_VALUE (FWP_CALLOUT_FLAG_ALLOW_RECLASSIFY),
                    ADD_VALUE (FWP_CALLOUT_FLAG_ALLOW_MID_STREAM_INSPECTION),
                    ADD_VALUE (FWP_CALLOUT_FLAG_ENABLE_COMMIT_ADD_NOTIFY),
                    ADD_VALUE (FWP_CALLOUT_FLAG_ALLOW_OFFLOAD),
                    ADD_VALUE (FWP_CALLOUT_FLAG_CONDITIONAL_ON_FLOW)
                  };

static const char *get_callout_flag (UINT32 flags)
{
  flags &= ~FWP_CALLOUT_FLAG_RESERVED1;
  return flags_decode (flags, callout_flags, DIM(callout_flags));
}

static FWPM_SESSION fw_session;
static HANDLE       fw_policy_handle  = INVALID_HANDLE_VALUE;
static HANDLE       fw_engine_handle  = INVALID_HANDLE_VALUE;
static HANDLE       fw_event_handle   = INVALID_HANDLE_VALUE;
static DWORD        fw_num_rules      = 0;
static DWORD        fw_num_events     = 0;
static DWORD        fw_num_ignored    = 0;
static DWORD        fw_unknown_layers = 0;
static BOOL         fw_have_ip2loc4   = FALSE;
static BOOL         fw_have_ip2loc6   = FALSE;
static UINT         fw_acp;
static char         fw_module [_MAX_PATH] = { '\0' };


/**
 * \def MAX_DOMAIN_SZ
 *  Maximum size of a `domain` name in a `struct SID_entry`.
 *
 * \def MAX_ACCOUNT_SZ
 *  Maximum size of an `account` name in a `struct SID_entry`.
 */
#define MAX_DOMAIN_SZ    20
#define MAX_ACCOUNT_SZ   30

/**
 * \struct SID_entry
 * A cache of SIDs for `print_user_id()` and `print_package_id()`.
 */
struct SID_entry {
       SID  *sid_copy;
       char *sid_str;
       char  domain [MAX_DOMAIN_SZ];
       char  account[MAX_ACCOUNT_SZ];
     };

static smartlist_t *fw_SID_list;
static char         fw_logged_on_user [100];

/**
 * Stuff for checking if `%n` can be used in `*printf()` functions.
 *
 * Is the `_set_printf_count_output()` available?
 */
#if defined(_MSC_VER) || (defined(__MINGW_MAJOR_VERSION) && __USE_MINGW_ANSI_STDIO == 0)
  #define _SET_PRINTF_COUNT_OUTPUT(x)  _set_printf_count_output (x)
#else
  #define _SET_PRINTF_COUNT_OUTPUT(x)  0
#endif

static BOOL fw_have_n_format = FALSE;

static void fw_check_n_format (BOOL init, BOOL push)
{
  static int n_state = 0;

  if (init)
  {
    char buf [10];
    int  len;

    n_state = _SET_PRINTF_COUNT_OUTPUT (1);
    if (snprintf(buf,sizeof(buf),"12345%n6789",&len) == 5)
       fw_have_n_format = TRUE;

    _SET_PRINTF_COUNT_OUTPUT (n_state);
  }
  else if (push)
  {
    /* Push the state and enable the use of '%n' format in MSVC's `*printf()` functions.
     */
     n_state = _SET_PRINTF_COUNT_OUTPUT (1);
  }
  else
  {
    /* Pop the state.
     */
    _SET_PRINTF_COUNT_OUTPUT (n_state);
  }
}

/**
 * \struct filter_entry
 * A cache of filter-IDs and names for `print_filter_rule()`, `print_filter_rule2()` and `print_layer_item2()`.
 */
struct filter_entry {
       UINT64 value;
       char   name [50];
     };

static smartlist_t *fw_filter_list;

static char  fw_buf [2000];
static char *fw_ptr  = fw_buf;
static int   fw_left = (int)sizeof(fw_buf) - 1;

static int fw_buf_add (const char *fmt, ...)
{
  va_list args;
  int     len;

  if (fw_left < (int)strlen(fmt))
     return (0);

  va_start (args, fmt);
  len = vsnprintf (fw_ptr, fw_left, fmt, args);
  fw_ptr  += len;
  fw_left -= len;
  va_end (args);
  return (len);
}

static int fw_buf_addc (int ch)
{
  if (fw_left <= 1)
     return (0);

  *fw_ptr++ = ch;
  fw_left--;
  return (1);
}

static void fw_buf_reset (void)
{
  fw_ptr  = fw_buf;
  fw_left = (int)sizeof(fw_buf) - 1;
}

static void fw_buf_flush (void)
{
  size_t len = fw_ptr - fw_buf;

  if (len > 0)
  {
    *fw_ptr = '\0';
    trace_puts (fw_buf);
  }
  fw_buf_reset();
}

static void fw_add_long_line (const char *start, size_t indent, int brk_ch)
{
  size_t      left = g_cfg.screen_width - indent;
  const char *c    = start;

  while (*c)
  {
    /* Break a long line only at a break-character or a '-'.
     * Check if room for a long string before we must break the line.
     */
    if (*c == brk_ch || *c == '-')
    {
      const char *p = strchr (c+1, brk_ch);
      size_t      i;
      int         ch;

      if (!p)
         p = strchr (c+1, '\0');

      if (left < 2 || (left <= (size_t)(p - c)))
      {
        if (brk_ch != ' ')
           fw_buf_addc (brk_ch);
        fw_buf_addc ('\n');
        for (i = 0; i < indent; i++)
           fw_buf_addc (' ');
        left  = g_cfg.screen_width - indent;
        start = ++c;
        continue;
      }

      /* Drop multiple break-chars or '-'.
       */
      ch = c[-1];
      if (c > start && (ch == brk_ch || ch == '-'))
      {
        start = ++c;
        continue;
      }
    }
    if (!fw_buf_addc(*c++))
       break;
    left--;
  }
  fw_buf_addc ('\n');
}

/**
 * \def FW_EVENT_CALLBACK
 * The macro for defining the event-callback functions for API-levels 0 - 4.
 *
 * (since C does not support C++-like templates, do it with this hack).
 */
#define FW_EVENT_CALLBACK(event_ver, callback_ver, allow_member1, allow_member2, drop_member1, drop_member2) \
        static void CALLBACK                                                                                 \
        fw_event_callback##event_ver (void *context,                                                         \
                                      const _FWPM_NET_EVENT##callback_ver *event)                            \
        {                                                                                                    \
       /* ENTER_CRIT(); */                                                                                   \
       /* ws_sema_wait(); */                                                                                 \
          if (event)                                                                                         \
          {                                                                                                  \
            if (g_cfg.trace_level >= 3)                                                                      \
               trace_printf ("\n------------------------------------------"                                  \
                             "-----------------------------------------\n"                                   \
                             "%s(): thr-id: %lu.\n",                                                         \
                             __FUNCTION__, DWORD_CAST(GetCurrentThreadId()));                                \
            fw_event_callback (event->type,                                                                  \
                               (const _FWPM_NET_EVENT_HEADER3*) &event->header,                              \
                                                                                                             \
                               event->type == _FWPM_NET_EVENT_TYPE_CLASSIFY_DROP ?                           \
                                 (const _FWPM_NET_EVENT_CLASSIFY_DROP2*) drop_member1 : NULL,                \
                                                                                                             \
                               event->type == _FWPM_NET_EVENT_TYPE_CAPABILITY_DROP ?                         \
                                 (const _FWPM_NET_EVENT_CAPABILITY_DROP0*) drop_member2 : NULL,              \
                                                                                                             \
                               event->type == _FWPM_NET_EVENT_TYPE_CLASSIFY_ALLOW ?                          \
                                 (const _FWPM_NET_EVENT_CLASSIFY_ALLOW0*) allow_member1: NULL,               \
                                                                                                             \
                                event->type == _FWPM_NET_EVENT_TYPE_CAPABILITY_ALLOW ?                       \
                                 (const _FWPM_NET_EVENT_CAPABILITY_ALLOW0*) allow_member2 : NULL);           \
          }                                                                                                  \
          ARGSUSED (context);                                                                                \
       /* LEAVE_CRIT(); */                                                                                   \
        }

static void CALLBACK fw_event_callback (const UINT                               event_type,
                                        const _FWPM_NET_EVENT_HEADER3           *header,
                                        const _FWPM_NET_EVENT_CLASSIFY_DROP2    *drop_event1,
                                        const _FWPM_NET_EVENT_CAPABILITY_DROP0  *drop_event2,
                                        const _FWPM_NET_EVENT_CLASSIFY_ALLOW0   *allow_event1,
                                        const _FWPM_NET_EVENT_CAPABILITY_ALLOW0 *allow_event2);

/**
 * This expands to:
 * \li `static void CALLBACK fw_event_callback0 (...)`
 * \li ...
 * \li `static void CALLBACK fw_event_callback4 (...)`
 */
FW_EVENT_CALLBACK (0, 1, NULL,                 NULL,                   event->classifyDrop, NULL)
FW_EVENT_CALLBACK (1, 2, NULL,                 NULL,                   event->classifyDrop, NULL)
FW_EVENT_CALLBACK (2, 3, event->classifyAllow, event->capabilityAllow, event->classifyDrop, event->capabilityDrop)
FW_EVENT_CALLBACK (3, 4, event->classifyAllow, event->capabilityAllow, event->classifyDrop, event->capabilityDrop)
FW_EVENT_CALLBACK (4, 5, event->classifyAllow, event->capabilityAllow, event->classifyDrop, event->capabilityDrop)

/**
 * Return a time-string for an event.
 *
 * This return a time-string matching `g_cfg.trace_time_format`.
 * Ref. `get_timestamp()`.
 *
 * \note A `diff` can be negative since different layers in the WFP, seems to create
 *       these timestamp them-self. And each event is not sent to this callback in
 *       an ordered fashion.
 */
static const char *get_time_string (const FILETIME *ts)
{
  static char  time_str [30];
  static int64 ref_ts = S64_SUFFIX(0);
  int64  diff;

  /* Init `ref_ts` for a `TS_RELATIVE` or `TS_DELTA` time-format.
   * Called from `fw_init()`.
   */
  if (!ts)
  {
    FILETIME _ts;

    if (p_GetSystemTimePreciseAsFileTime)
        (*p_GetSystemTimePreciseAsFileTime) (&_ts);
    else GetSystemTimeAsFileTime (&_ts);

    ref_ts = FILETIME_to_usec (&_ts);
    return (NULL);
  }

  if (g_cfg.trace_time_format == TS_NONE)
     return ("");

  if (g_cfg.trace_time_format == TS_RELATIVE || g_cfg.trace_time_format == TS_DELTA)
  {
    static int64 last_ts = S64_SUFFIX(0);
    const char *sign = "";
    int64       _ts = FILETIME_to_usec (ts);
    long        sec, msec;

    if (g_cfg.trace_time_format == TS_RELATIVE)
         diff = _ts - ref_ts;
    else if (last_ts == S64_SUFFIX(0))  /* First event when `g_cfg.trace_time_format == TS_DELTA` */
         diff = S64_SUFFIX(0);
    else diff = _ts - last_ts;

    last_ts = _ts;
    sec  = (long) (diff / S64_SUFFIX(1000000));
    msec = (long) ((diff - (1000000 * sec)) % 1000);
    if (sec < 0)
    {
      sec  = -sec;
      sign = "-";
    }
    if (msec < 0)
    {
      msec = -msec;
      sign = "-";
    }
    snprintf (time_str, sizeof(time_str), "%s%ld.%03ld sec", sign, sec, msec);
  }
  else if (g_cfg.trace_time_format == TS_ABSOLUTE)
  {
    SYSTEMTIME sys_time;
    FILETIME   loc_time;

    memset (&sys_time, '\0', sizeof(sys_time));
    memset (&loc_time, '\0', sizeof(loc_time));
    FileTimeToLocalFileTime (ts, &loc_time);
    FileTimeToSystemTime (&loc_time, &sys_time);

    snprintf (time_str, sizeof(time_str), "%02u:%02u:%02u.%03u",
              sys_time.wHour, sys_time.wMinute, sys_time.wSecond, sys_time.wMilliseconds);
  }
  return (time_str);
}

/**
 * Ensure the needed functions are loaded only once.
 *
 * We'll probably manage with only `FwpmNetEventSubscribe0()` and
 * `FwpmNetEventEnum0`. Hence subtract (4 + 5) from the number of
 * functions in `fw_funcs[]`.
 */
static BOOL fw_load_funcs (void)
{
  const struct LoadTable *tab = fw_funcs + 0;
  int   functions_needed = DIM(fw_funcs) - (4 + 5);
  int   i, num;

  for (i = num = 0; i < DIM(fw_funcs); i++, tab++)
  {
    if (*tab->func_addr)
       num++;
  }

  /* Already loaded functions okay; get out.
   */
  if (num >= functions_needed)
     return (TRUE);

  /* Functions never loaded.
   */
  if (num == 0)
     num = load_dynamic_table (fw_funcs, DIM(fw_funcs));

  if (num < functions_needed)
  {
    fw_errno = FW_FUNC_ERROR;
    return (FALSE);
  }
  return (TRUE);
}

/**
 * This should be the first functions called in this module.
 *
 * It should be called after `geoip_init()`. I.e. after `wsock_trace_init()`.
 */
BOOL fw_init (void)
{
  USHORT api_version = FW_REDSTONE2_BINARY_VERSION;
  ULONG  user_len    = sizeof(fw_logged_on_user);

  fw_SID_list    = smartlist_new();
  fw_filter_list = smartlist_new();
  fw_num_rules   = 0;

  fw_have_ip2loc4 = (ip2loc_num_ipv4_entries() > 0);
  fw_have_ip2loc6 = (ip2loc_num_ipv6_entries() > 0);

  if ((g_cfg.trace_stream == stdout || g_cfg.trace_stream == stderr) && isatty(fileno(g_cfg.trace_stream)))
       fw_acp = GetConsoleCP();
  else fw_acp = CP_ACP;

  GetUserName (fw_logged_on_user, &user_len);

  get_time_string (NULL);

  if (!fw_module[0])
     GetModuleFileName (NULL, fw_module, sizeof(fw_module));

  TRACE (2, "fw_module: '%s', fw_logged_on_user: '%s'.\n", fw_module, fw_logged_on_user);

  if (g_cfg.firewall.show_all == 0)
     exclude_list_add (fw_module, EXCL_PROGRAM);

  fw_check_n_format (TRUE, FALSE);

  if (!fw_load_funcs())
     return (FALSE);

  fw_errno = (*p_FWOpenPolicyStore) (api_version, NULL, FW_STORE_TYPE_DEFAULTS, FW_POLICY_ACCESS_RIGHT_READ,
                                     FW_POLICY_STORE_FLAGS_NONE, &fw_policy_handle);
  return (fw_errno == ERROR_SUCCESS);
}

/**
 * `smartlist_wipe()` helper.
 * Free an item in the `fw_SID_list` smartlist.
 */
static void fw_SID_free (void *_e)
{
  struct SID_entry *e = (struct SID_entry*) _e;

  if (e->sid_str)
     LocalFree (e->sid_str);
  free (e);
}

/**
 * This should be the last functions called in this module.
 */
void fw_exit (void)
{
  if (p_FWClosePolicyStore && fw_policy_handle != INVALID_HANDLE_VALUE)
    (*p_FWClosePolicyStore) (fw_policy_handle);

  fw_policy_handle = INVALID_HANDLE_VALUE;

  fw_monitor_stop (FALSE);

  if (fw_SID_list)
     smartlist_wipe (fw_SID_list, fw_SID_free);
  if (fw_filter_list)
     smartlist_wipe (fw_filter_list, free);

  fw_SID_list = fw_filter_list = NULL;

  unload_dynamic_table (fw_funcs, DIM(fw_funcs));
}

/**
 * Create the `fw_engine_handle` if not already done.
 * Initialise the global `fw_session`.
 */
static BOOL fw_create_engine (void)
{
  DWORD rc;

  if (fw_engine_handle != INVALID_HANDLE_VALUE)
     return (TRUE);

  if (!p_FwpmEngineOpen0)
  {
    fw_errno = FW_FUNC_ERROR;
    TRACE (1, "%s() failed: %s\n", __FUNCTION__, win_strerror(fw_errno));
    return (FALSE);
  }

  memset (&fw_session, '\0', sizeof(fw_session));
  fw_session.flags                   = 0;
  fw_session.displayData.name        = L"FirewallMonitoringSession";
  fw_session.displayData.description = L"Non-Dynamic session for wsock_trace";

#if 0
  /* Let the Base Firewall Engine cleanup after us.
   */
  fw_session.flags = FWPM_SESSION_FLAG_DYNAMIC;
#endif

  rc = (*p_FwpmEngineOpen0) (NULL, RPC_C_AUTHN_WINNT, NULL, &fw_session, &fw_engine_handle);
  if (rc != ERROR_SUCCESS)
  {
    fw_errno = rc;
    TRACE (1, "FwpmEngineOpen0() failed: %s\n", win_strerror(fw_errno));
    return (FALSE);
  }
  return (TRUE);
}

static BOOL fw_monitor_init (_FWPM_NET_EVENT_SUBSCRIPTION0 *subscription)
{
  FWP_VALUE value;
  DWORD     rc;

  /* If 'fw_init()' wasn't called or succeeded, return FALSE.
   */
  if (fw_policy_handle == INVALID_HANDLE_VALUE)
     return (FALSE);

  if (!fw_create_engine())
     return (FALSE);

  /* A major error if this is missing.
   */
  if (!p_FwpmEngineSetOption0)
  {
    fw_errno = FW_FUNC_ERROR;
    return (FALSE);
  }

  /* Enable collection of NetEvents
   */
  memset (&value, '\0', sizeof(value));
  value.type   = FWP_UINT32;
  value.uint32 = 1;

  rc = (*p_FwpmEngineSetOption0) (fw_engine_handle, FWPM_ENGINE_COLLECT_NET_EVENTS, &value);
  if (rc != ERROR_SUCCESS)
  {
    fw_errno = rc;
    return (FALSE);
  }

  value.type   = FWP_UINT32;
  value.uint32 = FWPM_NET_EVENT_KEYWORD_CAPABILITY_DROP |
                 FWPM_NET_EVENT_KEYWORD_CAPABILITY_ALLOW;

  if (g_cfg.firewall.show_all)
     value.uint32 += FWPM_NET_EVENT_KEYWORD_CLASSIFY_ALLOW |
                     FWPM_NET_EVENT_KEYWORD_INBOUND_MCAST  |
                     FWPM_NET_EVENT_KEYWORD_INBOUND_BCAST;

  rc = (*p_FwpmEngineSetOption0) (fw_engine_handle, FWPM_ENGINE_NET_EVENT_MATCH_ANY_KEYWORDS, &value);
  if (rc != ERROR_SUCCESS)
  {
    fw_errno = rc;
    return (FALSE);
  }

#if 1
  value.type   = FWP_UINT32;
  value.uint32 = 1;

  rc = (*p_FwpmEngineSetOption0) (fw_engine_handle, FWPM_ENGINE_MONITOR_IPSEC_CONNECTIONS, &value);
  if (rc != ERROR_SUCCESS)
  {
    fw_errno = rc;
    return (FALSE);
  }
#endif

  subscription->sessionKey = fw_session.sessionKey;
  fw_errno = ERROR_SUCCESS;
  return (TRUE);
}

/**
 * Try all available `FwpmNetEventSubscribeX()` functions and return TRUE if one succeedes.
 * Start with the one above or equal the given API-level in `fw_api`.
 */
static BOOL fw_monitor_subscribe (_FWPM_NET_EVENT_SUBSCRIPTION0 *subscription)
{
  #define SET_API_CALLBACK(N)                                                  \
          do {                                                                 \
            if (api_level == N && p_FwpmNetEventSubscribe##N)                  \
            {                                                                  \
              TRACE (2, "Trying FwpmNetEventSubscribe%d().\n", N);             \
              fw_errno = (*p_FwpmNetEventSubscribe##N) (fw_engine_handle,      \
                                                        subscription,          \
                                                        fw_event_callback##N,  \
                                                        fw_engine_handle,      \
                                                        &fw_event_handle);     \
              if (fw_errno == ERROR_SUCCESS)                                   \
              {                                                                \
                TRACE (1, "FwpmNetEventSubscribe%d() succeeded.\n", N);        \
                return (TRUE);                                                 \
              }                                                                \
            }                                                                  \
            if (api_level >= N && !p_FwpmNetEventSubscribe##N)                 \
            {                                                                  \
              fw_errno = ERROR_BAD_COMMAND;                                    \
              TRACE (0, "p_FwpmNetEventSubscribe%d() is not available.\n", N); \
              return (FALSE);                                                  \
            }                                                                  \
          } while (0)

  int api_level = fw_api;

  if (api_level < FW_API_LOW || api_level > FW_API_HIGH)
  {
    fw_errno = ERROR_INVALID_DATA;
    TRACE (1, "FwpmNetEventSubscribe%d() is not a legal API-level.\n", api_level);
    return (FALSE);
  }

  SET_API_CALLBACK (4);
  SET_API_CALLBACK (3);
  SET_API_CALLBACK (2);
  SET_API_CALLBACK (1);
  SET_API_CALLBACK (0);

  TRACE (1, "FwpmNetEventSubscribe%d() failed: %s\n", api_level, win_strerror(fw_errno));
  return (FALSE);
}

static BOOL fw_check_sizes (void)
{
  #define CHK_SIZE(a, cond, b)                                               \
          do {                                                               \
            if (! (sizeof(a) cond sizeof(b)) ) {                             \
               TRACE (0, "Mismatch of '%s' and '%s'. %d versus %d bytes.\n", \
                      #a, #b, (int)sizeof(a), (int)sizeof(b));               \
              return (FALSE);                                                \
            }                                                                \
          } while (0)

  #define OffsetOf(x, item) (unsigned) offsetof (x, item)

  #define CHK_OFS(a, b, item)                                                    \
          do {                                                                   \
            if (offsetof(a, item) != offsetof(b, item)) {                        \
               TRACE (0, "Mismatch of '%s' and '%s'. ofs %d versus %d bytes.\n", \
                      #a, #b, OffsetOf(a,item), OffsetOf(b,item));               \
              return (FALSE);                                                    \
            }                                                                    \
          } while (0)

  fw_errno = FW_FUNC_ERROR;  /* Assume failure */

#if (_WIN32_WINNT >= 0x0A02)
  CHK_SIZE (FWPM_NET_EVENT_HEADER1, ==, _FWPM_NET_EVENT_HEADER1);
  CHK_SIZE (FWPM_NET_EVENT_HEADER2, ==, _FWPM_NET_EVENT_HEADER2);
  CHK_SIZE (FWPM_NET_EVENT_HEADER3, ==, _FWPM_NET_EVENT_HEADER3);

  CHK_SIZE (FWPM_NET_EVENT_CLASSIFY_DROP2, ==, _FWPM_NET_EVENT_CLASSIFY_DROP2);
  CHK_SIZE (FWPM_NET_EVENT_CLASSIFY_ALLOW0, ==, _FWPM_NET_EVENT_CLASSIFY_ALLOW0);

  CHK_SIZE (FWPM_NET_EVENT0, ==, _FWPM_NET_EVENT0);
  CHK_SIZE (FWPM_NET_EVENT1, ==, _FWPM_NET_EVENT1);
  CHK_SIZE (FWPM_NET_EVENT2, ==, _FWPM_NET_EVENT2);
  CHK_SIZE (FWPM_NET_EVENT3, ==, _FWPM_NET_EVENT3);
  CHK_SIZE (FWPM_NET_EVENT4, ==, _FWPM_NET_EVENT4);
  CHK_SIZE (FWPM_NET_EVENT5, ==, _FWPM_NET_EVENT5);
#endif

#if !defined(__CYGWIN__)
  CHK_OFS (FWPM_NET_EVENT_HEADER0,        _FWPM_NET_EVENT_HEADER0, appId);
  CHK_OFS (FWPM_NET_EVENT_HEADER1,        _FWPM_NET_EVENT_HEADER1, appId);
  CHK_OFS (FWPM_NET_EVENT_CLASSIFY_DROP1, _FWPM_NET_EVENT_CLASSIFY_DROP1, msFwpDirection);
#endif

#if (_WIN32_WINNT >= 0x0602)
  CHK_OFS (FWPM_NET_EVENT_HEADER2,        _FWPM_NET_EVENT_HEADER2, appId);
  CHK_OFS (FWPM_NET_EVENT_CLASSIFY_DROP2, _FWPM_NET_EVENT_CLASSIFY_DROP2, msFwpDirection);
#endif

  CHK_SIZE (_FWPM_NET_EVENT_HEADER3, >, _FWPM_NET_EVENT_HEADER0);
  CHK_SIZE (_FWPM_NET_EVENT_HEADER3, <, _FWPM_NET_EVENT_HEADER1); /* Yeah, really */
  CHK_SIZE (_FWPM_NET_EVENT_HEADER3, >, _FWPM_NET_EVENT_HEADER2);

  fw_errno = 0;
  return (TRUE);
}

BOOL fw_monitor_start (void)
{
  _FWPM_NET_EVENT_SUBSCRIPTION0  subscription   = { 0 };
  _FWPM_NET_EVENT_ENUM_TEMPLATE0 event_template = { 0 };

  fw_num_events = fw_num_ignored = 0;

  if (ws_sema_inherited)
  {
    TRACE (1, "Not safe to use 'fw_monitor_start()' in a sub-process.\n");
    return (FALSE);
  }

  if (!fw_check_sizes())
     return (FALSE);

  if (!fw_monitor_init(&subscription))
     return (FALSE);

  /* Get events for all conditions
   */
  event_template.numFilterConditions = 0;

#if 0
  subscription.enumTemplate = &event_template;
#else
  subscription.enumTemplate = NULL; /* Don't really need a template */
#endif

  /* Subscribe to the events.
   * With API level = `fw_api == FW_API_DEFAULT` if not user-defined.
   */
  return fw_monitor_subscribe (&subscription);
}

void fw_monitor_stop (BOOL force)
{
  if (force)
  {
    if (fw_event_handle != INVALID_HANDLE_VALUE)
       CloseHandle (fw_event_handle);
    if (fw_engine_handle != INVALID_HANDLE_VALUE)
       CloseHandle (fw_engine_handle);
  }
  else
  {
    if (fw_engine_handle != INVALID_HANDLE_VALUE &&
        fw_event_handle  != INVALID_HANDLE_VALUE && p_FwpmNetEventUnsubscribe0)
       (*p_FwpmNetEventUnsubscribe0) (fw_engine_handle, fw_event_handle);

    if (fw_engine_handle != INVALID_HANDLE_VALUE && p_FwpmEngineClose0)
       (*p_FwpmEngineClose0) (fw_engine_handle);
  }
  fw_event_handle = fw_engine_handle = INVALID_HANDLE_VALUE;
}

/**
 * The `rule->wszName` wide-string may contain some strange characters that cause the
 * console output to become messed up. Hence convert to MultiByte before printing it.
 */
static void fw_dump_rule (const FW_RULE *rule)
{
  const char *dir = (rule->Direction == FW_DIR_INVALID) ? "INV" :
                    (rule->Direction == FW_DIR_IN)      ? "IN"  :
                    (rule->Direction == FW_DIR_OUT)     ? "OUT" :
                    (rule->Direction == FW_DIR_BOTH)    ? "BOTH": "?";
  char ascii [300];
  int  indent = 6;

  fw_buf_reset();

  if (WideCharToMultiByte (fw_acp, 0, rule->wszDescription, -1, ascii, (int)sizeof(ascii)-1, NULL, NULL) == 0)
     strcpy (ascii, "?");

  if (fw_have_n_format)
       fw_buf_add ("~4%3lu: ~3%s:~0%*s%n", DWORD_CAST(++fw_num_rules), dir, 8-strlen(dir), "", &indent);
  else indent = fw_buf_add ("~4%3lu: ~3%s:~0%*s", DWORD_CAST(++fw_num_rules), dir, 8-strlen(dir), "");

  fw_add_long_line (ascii, indent-6, ' ');
  fw_buf_flush();

//fw_buf_add ("     ~2status:~0  0x%08X, 0x%08X, 0x%p\n", rule->Status, rule->wFlags, rule->pMetaData);

  if (rule->wszName)
  {
    if (WideCharToMultiByte (fw_acp, 0, rule->wszName, -1, ascii, (int)sizeof(ascii)-1, NULL, NULL) == 0)
       strcpy (ascii, "?");
    fw_buf_add ("     ~2name:~0    %s\n", ascii);
  }

  if (rule->wszLocalApplication)
     fw_buf_add ("     ~2prog:~0    %S\n", rule->wszLocalApplication);

  if (rule->wszEmbeddedContext)
     fw_buf_add ("     ~2context:~0 %S\n", rule->wszEmbeddedContext);

  fw_buf_addc ('\n');
  fw_buf_flush();
}

int fw_enumerate_rules (void)
{
  int      num;
  FW_RULE *rule, *rules = NULL;
  ULONG    rule_count = 0;
  ULONG    rc;
  ULONG    flags = FW_ENUM_RULES_FLAG_RESOLVE_NAME |
                   FW_ENUM_RULES_FLAG_RESOLVE_DESCRIPTION |
                   FW_ENUM_RULES_FLAG_RESOLVE_APPLICATION |
                   FW_ENUM_RULES_FLAG_RESOLVE_KEYWORD;

  FW_PROFILE_TYPE  profile = (g_cfg.firewall.show_all ? FW_PROFILE_TYPE_ALL : FW_PROFILE_TYPE_CURRENT);

  rc = (*p_FWEnumFirewallRules) (fw_policy_handle, FW_RULE_STATUS_CLASS_ALL, profile,
                                 (FW_ENUM_RULES_FLAGS)flags, &rule_count, &rules);
  if (rc != ERROR_SUCCESS)
  {
    fw_errno = rc;
    TRACE (1, "FWEnumFirewallRules() failed: %s.\n", win_strerror(fw_errno));
    return (-1);
  }

  TRACE (1, "Got rule_count: %lu.\n", DWORD_CAST(rule_count));

  fw_check_n_format (FALSE, TRUE);

  for (num = 0, rule = rules; rule && num < (int)rule_count; rule = rule->pNext, num++)
      fw_dump_rule (rule);

  fw_check_n_format (FALSE, FALSE);

  if (p_FWFreeFirewallRules && rules)
    (*p_FWFreeFirewallRules) (rules);

  if (num != (int)rule_count)
     TRACE (1, "num: %d, rule_count: %lu.\n", num, DWORD_CAST(rule_count));
  return (num);
}

/**
 * Get the `FWPM_LAYER_xx` name from <fwpmu.h> for this layer.
 *
 * \eg{.}:
 * ```
 *  // c86fd1bf-21cd-497e-a0bb-17425c885c58
 *  _DEFINE_GUID (FWPM_LAYER_INBOUND_IPPACKET_V4,
 *                ...)
 * ```
 * In this case, print the `layer` as `C86FD1BF-21CD-497E-A0BB-17425C885C58 = FWPM_LAYER_INBOUND_IPPACKET_V4`.
 *
 * \see `get_callout_layer_name()` and how it is used in `fw_enumerate_callouts()`.
 */
struct GUID_search_list2 {
       const GUID *guid;
       const char *name;
     };

#define _DEFINE_GUID(name, dw, w1, w2, b1, b2, b3, b4, b5, b6, b7, b8) \
         static const GUID _##name = {                                 \
                      dw, w1, w2, { b1, b2, b3, b4, b5, b6, b7, b8 }   \
                    };
/*
 * Generated by `gen-fwpm-guid.bat` and hand-edited:
 */
_DEFINE_GUID (FWPM_LAYER_INBOUND_IPPACKET_V4,
              0xC86FD1BF,
              0x21CD,
              0x497E,
              0xA0, 0xBB, 0x17, 0x42, 0x5C, 0x88, 0x5C, 0x58);

_DEFINE_GUID (FWPM_LAYER_INBOUND_IPPACKET_V4_DISCARD,
              0xB5A230D0,
              0xA8C0,
              0x44F2,
              0x91, 0x6E, 0x99, 0x1B, 0x53, 0xDE, 0xD1, 0xF7);

_DEFINE_GUID (FWPM_LAYER_INBOUND_IPPACKET_V6,
              0xF52032CB,
              0x991C,
              0x46E7,
              0x97, 0x1D, 0x26, 0x01, 0x45, 0x9A, 0x91, 0xCA);

_DEFINE_GUID (FWPM_LAYER_INBOUND_IPPACKET_V6_DISCARD,
              0xBB24C279,
              0x93B4,
              0x47A2,
              0x83, 0xAD, 0xAE, 0x16, 0x98, 0xB5, 0x08, 0x85);

_DEFINE_GUID (FWPM_LAYER_OUTBOUND_IPPACKET_V4,
              0x1E5C9FAE,
              0x8A84,
              0x4135,
              0xA3, 0x31, 0x95, 0x0B, 0x54, 0x22, 0x9E, 0xCD);

_DEFINE_GUID (FWPM_LAYER_OUTBOUND_IPPACKET_V4_DISCARD,
              0x08E4BCB5,
              0xB647,
              0x48F3,
              0x95, 0x3C, 0xE5, 0xDD, 0xBD, 0x03, 0x93, 0x7E);

_DEFINE_GUID (FWPM_LAYER_OUTBOUND_IPPACKET_V6,
              0xA3B3AB6B,
              0x3564,
              0x488C,
              0x91, 0x17, 0xF3, 0x4E, 0x82, 0x14, 0x27, 0x63);

_DEFINE_GUID (FWPM_LAYER_OUTBOUND_IPPACKET_V6_DISCARD,
              0x9513D7C4,
              0xA934,
              0x49DC,
              0x91, 0xA7, 0x6C, 0xCB, 0x80, 0xCC, 0x02, 0xE3);

_DEFINE_GUID (FWPM_LAYER_IPFORWARD_V4,
              0xA82ACC24,
              0x4EE1,
              0x4EE1,
              0xB4, 0x65, 0xFD, 0x1D, 0x25, 0xCB, 0x10, 0xA4);

_DEFINE_GUID (FWPM_LAYER_IPFORWARD_V4_DISCARD,
              0x9E9EA773,
              0x2FAE,
              0x4210,
              0x8F, 0x17, 0x34, 0x12, 0x9E, 0xF3, 0x69, 0xEB);

_DEFINE_GUID (FWPM_LAYER_IPFORWARD_V6,
              0x7B964818,
              0x19C7,
              0x493A,
              0xB7, 0x1F, 0x83, 0x2C, 0x36, 0x84, 0xD2, 0x8C);

_DEFINE_GUID (FWPM_LAYER_IPFORWARD_V6_DISCARD,
              0x31524A5D,
              0x1DFE,
              0x472F,
              0xBB, 0x93, 0x51, 0x8E, 0xE9, 0x45, 0xD8, 0xA2);

_DEFINE_GUID (FWPM_LAYER_INBOUND_TRANSPORT_V4,
              0x5926DFC8,
              0xE3CF,
              0x4426,
              0xA2, 0x83, 0xDC, 0x39, 0x3F, 0x5D, 0x0F, 0x9D);

_DEFINE_GUID (FWPM_LAYER_INBOUND_TRANSPORT_V4_DISCARD,
              0xAC4A9833,
              0xF69D,
              0x4648,
              0xB2, 0x61, 0x6D, 0xC8, 0x48, 0x35, 0xEF, 0x39);

_DEFINE_GUID (FWPM_LAYER_INBOUND_TRANSPORT_V6,
              0x634A869F,
              0xFC23,
              0x4B90,
              0xB0, 0xC1, 0xBF, 0x62, 0x0A, 0x36, 0xAE, 0x6F);

_DEFINE_GUID (FWPM_LAYER_INBOUND_TRANSPORT_V6_DISCARD,
              0x2A6FF955,
              0x3B2B,
              0x49D2,
              0x98, 0x48, 0xAD, 0x9D, 0x72, 0xDC, 0xAA, 0xB7);

_DEFINE_GUID (FWPM_LAYER_OUTBOUND_TRANSPORT_V4,
              0x09E61AEA,
              0xD214,
              0x46E2,
              0x9B, 0x21, 0xB2, 0x6B, 0x0B, 0x2F, 0x28, 0xC8);

_DEFINE_GUID (FWPM_LAYER_OUTBOUND_TRANSPORT_V4_DISCARD,
              0xC5F10551,
              0xBDB0,
              0x43D7,
              0xA3, 0x13, 0x50, 0xE2, 0x11, 0xF4, 0xD6, 0x8A);

_DEFINE_GUID (FWPM_LAYER_OUTBOUND_TRANSPORT_V6,
              0xE1735BDE,
              0x013F,
              0x4655,
              0xB3, 0x51, 0xA4, 0x9E, 0x15, 0x76, 0x2D, 0xF0);

_DEFINE_GUID (FWPM_LAYER_OUTBOUND_TRANSPORT_V6_DISCARD,
              0xF433DF69,
              0xCCBD,
              0x482E,
              0xB9, 0xB2, 0x57, 0x16, 0x56, 0x58, 0xC3, 0xB3);

_DEFINE_GUID (FWPM_LAYER_STREAM_V4,
              0x3B89653C,
              0xC170,
              0x49E4,
              0xB1, 0xCD, 0xE0, 0xEE, 0xEE, 0xE1, 0x9A, 0x3E);

_DEFINE_GUID (FWPM_LAYER_STREAM_V4_DISCARD,
              0x25C4C2C2,
              0x25FF,
              0x4352,
              0x82, 0xF9, 0xC5, 0x4A, 0x4A, 0x47, 0x26, 0xDC);

_DEFINE_GUID (FWPM_LAYER_STREAM_V6,
              0x47C9137A,
              0x7EC4,
              0x46B3,
              0xB6, 0xE4, 0x48, 0xE9, 0x26, 0xB1, 0xED, 0xA4);

_DEFINE_GUID (FWPM_LAYER_STREAM_V6_DISCARD,
              0x10A59FC7,
              0xB628,
              0x4C41,
              0x9E, 0xB8, 0xCF, 0x37, 0xD5, 0x51, 0x03, 0xCF);

_DEFINE_GUID (FWPM_LAYER_DATAGRAM_DATA_V4,
              0x3D08BF4E,
              0x45F6,
              0x4930,
              0xA9, 0x22, 0x41, 0x70, 0x98, 0xE2, 0x00, 0x27);

_DEFINE_GUID (FWPM_LAYER_DATAGRAM_DATA_V4_DISCARD,
              0x18E330C6,
              0x7248,
              0x4E52,
              0xAA, 0xAB, 0x47, 0x2E, 0xD6, 0x77, 0x04, 0xFD);

_DEFINE_GUID (FWPM_LAYER_DATAGRAM_DATA_V6,
              0xFA45FE2F,
              0x3CBA,
              0x4427,
              0x87, 0xFC, 0x57, 0xB9, 0xA4, 0xB1, 0x0D, 0x00);

_DEFINE_GUID (FWPM_LAYER_DATAGRAM_DATA_V6_DISCARD,
              0x09D1DFE1,
              0x9B86,
              0x4A42,
              0xBE, 0x9D, 0x8C, 0x31, 0x5B, 0x92, 0xA5, 0xD0);

_DEFINE_GUID (FWPM_LAYER_INBOUND_ICMP_ERROR_V4,
              0x61499990,
              0x3CB6,
              0x4E84,
              0xB9, 0x50, 0x53, 0xB9, 0x4B, 0x69, 0x64, 0xF3);

_DEFINE_GUID (FWPM_LAYER_INBOUND_ICMP_ERROR_V4_DISCARD,
              0xA6B17075,
              0xEBAF,
              0x4053,
              0xA4, 0xE7, 0x21, 0x3C, 0x81, 0x21, 0xED, 0xE5);

_DEFINE_GUID (FWPM_LAYER_INBOUND_ICMP_ERROR_V6,
              0x65F9BDFF,
              0x3B2D,
              0x4E5D,
              0xB8, 0xC6, 0xC7, 0x20, 0x65, 0x1F, 0xE8, 0x98);

_DEFINE_GUID (FWPM_LAYER_INBOUND_ICMP_ERROR_V6_DISCARD,
              0xA6E7CCC0,
              0x08FB,
              0x468D,
              0xA4, 0x72, 0x97, 0x71, 0xD5, 0x59, 0x5E, 0x09);

_DEFINE_GUID (FWPM_LAYER_OUTBOUND_ICMP_ERROR_V4,
              0x41390100,
              0x564C,
              0x4B32,
              0xBC, 0x1D, 0x71, 0x80, 0x48, 0x35, 0x4D, 0x7C);

_DEFINE_GUID (FWPM_LAYER_OUTBOUND_ICMP_ERROR_V4_DISCARD,
              0xB3598D36,
              0x0561,
              0x4588,
              0xA6, 0xBF, 0xE9, 0x55, 0xE3, 0xF6, 0x26, 0x4B);

_DEFINE_GUID (FWPM_LAYER_OUTBOUND_ICMP_ERROR_V6,
              0x7FB03B60,
              0x7B8D,
              0x4DFA,
              0xBA, 0xDD, 0x98, 0x01, 0x76, 0xFC, 0x4E, 0x12);

_DEFINE_GUID (FWPM_LAYER_OUTBOUND_ICMP_ERROR_V6_DISCARD,
              0x65F2E647,
              0x8D0C,
              0x4F47,
              0xB1, 0x9B, 0x33, 0xA4, 0xD3, 0xF1, 0x35, 0x7C);

_DEFINE_GUID (FWPM_LAYER_ALE_RESOURCE_ASSIGNMENT_V4,
              0x1247D66D,
              0x0B60,
              0x4A15,
              0x8D, 0x44, 0x71, 0x55, 0xD0, 0xF5, 0x3A, 0x0C);

_DEFINE_GUID (FWPM_LAYER_ALE_RESOURCE_ASSIGNMENT_V4_DISCARD,
              0x0B5812A2,
              0xC3FF,
              0x4ECA,
              0xB8, 0x8D, 0xC7, 0x9E, 0x20, 0xAC, 0x63, 0x22);

_DEFINE_GUID (FWPM_LAYER_ALE_RESOURCE_ASSIGNMENT_V6,
              0x55A650E1,
              0x5F0A,
              0x4ECA,
              0xA6, 0x53, 0x88, 0xF5, 0x3B, 0x26, 0xAA, 0x8C);

_DEFINE_GUID (FWPM_LAYER_ALE_RESOURCE_ASSIGNMENT_V6_DISCARD,
              0xCBC998BB,
              0xC51F,
              0x4C1A,
              0xBB, 0x4F, 0x97, 0x75, 0xFC, 0xAC, 0xAB, 0x2F);

_DEFINE_GUID (FWPM_LAYER_ALE_AUTH_LISTEN_V4,
              0x88BB5DAD,
              0x76D7,
              0x4227,
              0x9C, 0x71, 0xDF, 0x0A, 0x3E, 0xD7, 0xBE, 0x7E);

_DEFINE_GUID (FWPM_LAYER_ALE_AUTH_LISTEN_V4_DISCARD,
              0x371DFADA,
              0x9F26,
              0x45FD,
              0xB4, 0xEB, 0xC2, 0x9E, 0xB2, 0x12, 0x89, 0x3F);

_DEFINE_GUID (FWPM_LAYER_ALE_AUTH_LISTEN_V6,
              0x7AC9DE24,
              0x17DD,
              0x4814,
              0xB4, 0xBD, 0xA9, 0xFB, 0xC9, 0x5A, 0x32, 0x1B);

_DEFINE_GUID (FWPM_LAYER_ALE_AUTH_LISTEN_V6_DISCARD,
              0x60703B07,
              0x63C8,
              0x48E9,
              0xAD, 0xA3, 0x12, 0xB1, 0xAF, 0x40, 0xA6, 0x17);

_DEFINE_GUID (FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V4,
              0xE1CD9FE7,
              0xF4B5,
              0x4273,
              0x96, 0xC0, 0x59, 0x2E, 0x48, 0x7B, 0x86, 0x50);

_DEFINE_GUID (FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V4_DISCARD,
              0x9EEAA99B,
              0xBD22,
              0x4227,
              0x91, 0x9F, 0x00, 0x73, 0xC6, 0x33, 0x57, 0xB1);

_DEFINE_GUID (FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V6,
              0xA3B42C97,
              0x9F04,
              0x4672,
              0xB8, 0x7E, 0xCE, 0xE9, 0xC4, 0x83, 0x25, 0x7F);

_DEFINE_GUID (FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V6_DISCARD,
              0x89455B97,
              0xDBE1,
              0x453F,
              0xA2, 0x24, 0x13, 0xDA, 0x89, 0x5A, 0xF3, 0x96);

_DEFINE_GUID (FWPM_LAYER_ALE_AUTH_CONNECT_V4,
              0xC38D57D1,
              0x05A7,
              0x4C33,
              0x90, 0x4F, 0x7F, 0xBC, 0xEE, 0xE6, 0x0E, 0x82);

_DEFINE_GUID (FWPM_LAYER_ALE_AUTH_CONNECT_V4_DISCARD,
              0xD632A801,
              0xF5BA,
              0x4AD6,
              0x96, 0xE3, 0x60, 0x70, 0x17, 0xD9, 0x83, 0x6A);

_DEFINE_GUID (FWPM_LAYER_ALE_AUTH_CONNECT_V6,
              0x4A72393B,
              0x319F,
              0x44BC,
              0x84, 0xC3, 0xBA, 0x54, 0xDC, 0xB3, 0xB6, 0xB4);

_DEFINE_GUID (FWPM_LAYER_ALE_AUTH_CONNECT_V6_DISCARD,
              0xC97BC3B8,
              0xC9A3,
              0x4E33,
              0x86, 0x95, 0x8E, 0x17, 0xAA, 0xD4, 0xDE, 0x09);

_DEFINE_GUID (FWPM_LAYER_ALE_FLOW_ESTABLISHED_V4,
              0xAF80470A,
              0x5596,
              0x4C13,
              0x99, 0x92, 0x53, 0x9E, 0x6F, 0xE5, 0x79, 0x67);

_DEFINE_GUID (FWPM_LAYER_ALE_FLOW_ESTABLISHED_V4_DISCARD,
              0x146AE4A9,
              0xA1D2,
              0x4D43,
              0xA3, 0x1A, 0x4C, 0x42, 0x68, 0x2B, 0x8E, 0x4F);

_DEFINE_GUID (FWPM_LAYER_ALE_FLOW_ESTABLISHED_V6,
              0x7021D2B3,
              0xDFA4,
              0x406E,
              0xAF, 0xEB, 0x6A, 0xFA, 0xF7, 0xE7, 0x0E, 0xFD);

_DEFINE_GUID (FWPM_LAYER_ALE_FLOW_ESTABLISHED_V6_DISCARD,
              0x46928636,
              0xBBCA,
              0x4B76,
              0x94, 0x1D, 0x0F, 0xA7, 0xF5, 0xD7, 0xD3, 0x72);

_DEFINE_GUID (FWPM_LAYER_INBOUND_MAC_FRAME_ETHERNET,
              0xEFFB7EDB,
              0x0055,
              0x4F9A,
              0xA2, 0x31, 0x4F, 0xF8, 0x13, 0x1A, 0xD1, 0x91);

_DEFINE_GUID (FWPM_LAYER_OUTBOUND_MAC_FRAME_ETHERNET,
              0x694673BC,
              0xD6DB,
              0x4870,
              0xAD, 0xEE, 0x0A, 0xCD, 0xBD, 0xB7, 0xF4, 0xB2);

_DEFINE_GUID (FWPM_LAYER_INBOUND_MAC_FRAME_NATIVE,
              0xD4220BD3,
              0x62CE,
              0x4F08,
              0xAE, 0x88, 0xB5, 0x6E, 0x85, 0x26, 0xDF, 0x50);

_DEFINE_GUID (FWPM_LAYER_OUTBOUND_MAC_FRAME_NATIVE,
              0x94C44912,
              0x9D6F,
              0x4EBF,
              0xB9, 0x95, 0x05, 0xAB, 0x8A, 0x08, 0x8D, 0x1B);

_DEFINE_GUID (FWPM_LAYER_INGRESS_VSWITCH_ETHERNET,
              0x7D98577A,
              0x9A87,
              0x41EC,
              0x97, 0x18, 0x7C, 0xF5, 0x89, 0xC9, 0xF3, 0x2D);

_DEFINE_GUID (FWPM_LAYER_EGRESS_VSWITCH_ETHERNET,
              0x86C872B0,
              0x76FA,
              0x4B79,
              0x93, 0xA4, 0x07, 0x50, 0x53, 0x0A, 0xE2, 0x92);

_DEFINE_GUID (FWPM_LAYER_INGRESS_VSWITCH_TRANSPORT_V4,
              0xB2696FF6,
              0x774F,
              0x4554,
              0x9F, 0x7D, 0x3D, 0xA3, 0x94, 0x5F, 0x8E, 0x85);

_DEFINE_GUID (FWPM_LAYER_INGRESS_VSWITCH_TRANSPORT_V6,
              0x5EE314FC,
              0x7D8A,
              0x47F4,
              0xB7, 0xE3, 0x29, 0x1A, 0x36, 0xDA, 0x4E, 0x12);

_DEFINE_GUID (FWPM_LAYER_EGRESS_VSWITCH_TRANSPORT_V4,
              0xB92350B6,
              0x91F0,
              0x46B6,
              0xBD, 0xC4, 0x87, 0x1D, 0xFD, 0x4A, 0x7C, 0x98);

_DEFINE_GUID (FWPM_LAYER_EGRESS_VSWITCH_TRANSPORT_V6,
              0x1B2DEF23,
              0x1881,
              0x40BD,
              0x82, 0xF4, 0x42, 0x54, 0xE6, 0x31, 0x41, 0xCB);

_DEFINE_GUID (FWPM_LAYER_INBOUND_TRANSPORT_FAST,
              0xE41D2719,
              0x05C7,
              0x40F0,
              0x89, 0x83, 0xEA, 0x8D, 0x17, 0xBB, 0xC2, 0xF6);

_DEFINE_GUID (FWPM_LAYER_OUTBOUND_TRANSPORT_FAST,
              0x13ED4388,
              0xA070,
              0x4815,
              0x99,0x35,0x7A,0x9B,0xE6,0x40,0x8B,0x78);

_DEFINE_GUID (FWPM_LAYER_INBOUND_MAC_FRAME_NATIVE_FAST,
              0x853AAA8E,
              0x2B78,
              0x4D24,
              0xA8,0x04,0x36,0xDB,0x08,0xB2,0x97,0x11);

_DEFINE_GUID (FWPM_LAYER_OUTBOUND_MAC_FRAME_NATIVE_FAST,
              0x470DF946,
              0xC962,
              0x486F,
              0x94,0x46,0x82,0x93,0xCB,0xC7,0x5E,0xB8);

_DEFINE_GUID (FWPM_LAYER_IPSEC_KM_DEMUX_V4,
              0xF02B1526,
              0xA459,
              0x4A51,
              0xB9, 0xE3, 0x75, 0x9D, 0xE5, 0x2B, 0x9D, 0x2C);

_DEFINE_GUID (FWPM_LAYER_IPSEC_KM_DEMUX_V6,
              0x2F755CF6,
              0x2FD4,
              0x4E88,
              0xB3, 0xE4, 0xA9, 0x1B, 0xCA, 0x49, 0x52, 0x35);

_DEFINE_GUID (FWPM_LAYER_IPSEC_V4,
              0xEDA65C74,
              0x610D,
              0x4BC5,
              0x94, 0x8F, 0x3C, 0x4F, 0x89, 0x55, 0x68, 0x67);

_DEFINE_GUID (FWPM_LAYER_IPSEC_V6,
              0x13C48442,
              0x8D87,
              0x4261,
              0x9A, 0x29, 0x59, 0xD2, 0xAB, 0xC3, 0x48, 0xB4);

_DEFINE_GUID (FWPM_LAYER_IKEEXT_V4,
              0xB14B7BDB,
              0xDBBD,
              0x473E,
              0xBE, 0xD4, 0x8B, 0x47, 0x08, 0xD4, 0xF2, 0x70);

_DEFINE_GUID (FWPM_LAYER_IKEEXT_V6,
              0xB64786B3,
              0xF687,
              0x4EB9,
              0x89, 0xD2, 0x8E, 0xF3, 0x2A, 0xCD, 0xAB, 0xE2);

_DEFINE_GUID (FWPM_LAYER_RPC_UM,
              0x75A89DDA,
              0x95E4,
              0x40F3,
              0xAD, 0xC7, 0x76, 0x88, 0xA9, 0xC8, 0x47, 0xE1);

_DEFINE_GUID (FWPM_LAYER_RPC_EPMAP,
              0x9247BC61,
              0xEB07,
              0x47EE,
              0x87, 0x2C, 0xBF, 0xD7, 0x8B, 0xFD, 0x16, 0x16);

_DEFINE_GUID (FWPM_LAYER_RPC_EP_ADD,
              0x618DFFC7,
              0xC450,
              0x4943,
              0x95, 0xDB, 0x99, 0xB4, 0xC1, 0x6A, 0x55, 0xD4);

_DEFINE_GUID (FWPM_LAYER_RPC_PROXY_CONN,
              0x94A4B50B,
              0xBA5C,
              0x4F27,
              0x90, 0x7A, 0x22, 0x9F, 0xAC, 0x0C, 0x2A, 0x7A);

_DEFINE_GUID (FWPM_LAYER_RPC_PROXY_IF,
              0xF8A38615,
              0xE12C,
              0x41AC,
              0x98, 0xDF, 0x12, 0x1A, 0xD9, 0x81, 0xAA, 0xDE);

_DEFINE_GUID (FWPM_LAYER_KM_AUTHORIZATION,
              0x4AA226E9,
              0x9020,
              0x45FB,
              0x95,0x6A, 0xC0, 0x24, 0x9D, 0x84, 0x11, 0x95);

_DEFINE_GUID (FWPM_LAYER_NAME_RESOLUTION_CACHE_V4,
              0x0C2AA681,
              0x905B,
              0x4CCD,
              0xA4, 0x67, 0x4D, 0xD8, 0x11, 0xD0, 0x7B, 0x7B);

_DEFINE_GUID (FWPM_LAYER_NAME_RESOLUTION_CACHE_V6,
              0x92D592FA,
              0x6B01,
              0x434A,
              0x9D, 0xEA, 0xD1, 0xE9, 0x6E, 0xA9, 0x7D, 0xA9);

_DEFINE_GUID (FWPM_LAYER_ALE_RESOURCE_RELEASE_V4,
              0x74365CCE,
              0xCCB0,
              0x401A,
              0xBF, 0xC1, 0xB8, 0x99, 0x34, 0xAD, 0x7E, 0x15);

_DEFINE_GUID (FWPM_LAYER_ALE_RESOURCE_RELEASE_V6,
              0xF4E5CE80,
              0xEDCC,
              0x4E13,
              0x8A, 0x2F, 0xB9, 0x14, 0x54, 0xBB, 0x05, 0x7B);

_DEFINE_GUID (FWPM_LAYER_ALE_ENDPOINT_CLOSURE_V4,
              0xB4766427,
              0xE2A2,
              0x467A,
              0xBD, 0x7E, 0xDB, 0xCD, 0x1B, 0xD8, 0x5A, 0x09);

_DEFINE_GUID (FWPM_LAYER_ALE_ENDPOINT_CLOSURE_V6,
              0xBB536CCD,
              0x4755,
              0x4BA9,
              0x9F, 0xF7, 0xF9, 0xED, 0xF8, 0x69, 0x9C, 0x7B);

_DEFINE_GUID (FWPM_LAYER_ALE_CONNECT_REDIRECT_V4,
              0xC6E63C8C,
              0xB784,
              0x4562,
              0xAA, 0x7D, 0x0A, 0x67, 0xCF, 0xCA, 0xF9, 0xA3);

_DEFINE_GUID (FWPM_LAYER_ALE_CONNECT_REDIRECT_V6,
              0x587E54A7,
              0x8046,
              0x42BA,
              0xA0, 0xAA, 0xB7, 0x16, 0x25, 0x0F, 0xC7, 0xFD);

_DEFINE_GUID (FWPM_LAYER_ALE_BIND_REDIRECT_V4,
              0x66978CAD,
              0xC704,
              0x42AC,
              0x86, 0xAC, 0x7C, 0x1A, 0x23, 0x1B, 0xD2, 0x53);

_DEFINE_GUID (FWPM_LAYER_ALE_BIND_REDIRECT_V6,
              0xBEF02C9C,
              0x606B,
              0x4536,
              0x8C, 0x26, 0x1C, 0x2F, 0xC7, 0xB6, 0x31, 0xD4);

_DEFINE_GUID (FWPM_LAYER_STREAM_PACKET_V4,
              0xAF52D8EC,
              0xCB2D,
              0x44E5,
              0xAD, 0x92, 0xF8, 0xDC, 0x38, 0xD2, 0xEB, 0x29);

_DEFINE_GUID (FWPM_LAYER_STREAM_PACKET_V6,
              0x779A8CA3,
              0xF099,
              0x468F,
              0xB5, 0xD4, 0x83, 0x53, 0x5C, 0x46, 0x1C, 0x02);

_DEFINE_GUID (FWPM_LAYER_INBOUND_RESERVED2,
              0xF4FB8D55,
              0xC076,
              0x46D8,
              0xA2, 0xC7, 0x6A, 0x4C, 0x72, 0x2C, 0xA4, 0xED);

#undef  ADD_VALUE
#define ADD_VALUE(v)  { &_FWPM_LAYER_##v, "FWPM_LAYER_" #v }

static const struct GUID_search_list2 fwpm_GUIDs[] = {
                    ADD_VALUE (INBOUND_IPPACKET_V4),
                    ADD_VALUE (INBOUND_IPPACKET_V4_DISCARD),
                    ADD_VALUE (INBOUND_IPPACKET_V6),
                    ADD_VALUE (INBOUND_IPPACKET_V6_DISCARD),
                    ADD_VALUE (INBOUND_TRANSPORT_V4),
                    ADD_VALUE (INBOUND_TRANSPORT_V4_DISCARD),
                    ADD_VALUE (INBOUND_TRANSPORT_V6),
                    ADD_VALUE (INBOUND_TRANSPORT_V6_DISCARD),
                    ADD_VALUE (INBOUND_TRANSPORT_FAST),
                    ADD_VALUE (INBOUND_ICMP_ERROR_V4),
                    ADD_VALUE (INBOUND_ICMP_ERROR_V4_DISCARD),
                    ADD_VALUE (INBOUND_ICMP_ERROR_V6),
                    ADD_VALUE (INBOUND_ICMP_ERROR_V6_DISCARD),
                    ADD_VALUE (INBOUND_MAC_FRAME_ETHERNET),
                    ADD_VALUE (INBOUND_MAC_FRAME_NATIVE),
                    ADD_VALUE (INBOUND_MAC_FRAME_NATIVE_FAST),
                    ADD_VALUE (INBOUND_RESERVED2),

                    ADD_VALUE (OUTBOUND_IPPACKET_V4),
                    ADD_VALUE (OUTBOUND_IPPACKET_V4_DISCARD),
                    ADD_VALUE (OUTBOUND_IPPACKET_V6),
                    ADD_VALUE (OUTBOUND_IPPACKET_V6_DISCARD),
                    ADD_VALUE (OUTBOUND_TRANSPORT_V4),
                    ADD_VALUE (OUTBOUND_TRANSPORT_V4_DISCARD),
                    ADD_VALUE (OUTBOUND_TRANSPORT_V6),
                    ADD_VALUE (OUTBOUND_TRANSPORT_V6_DISCARD),
                    ADD_VALUE (OUTBOUND_ICMP_ERROR_V4),
                    ADD_VALUE (OUTBOUND_ICMP_ERROR_V4_DISCARD),
                    ADD_VALUE (OUTBOUND_ICMP_ERROR_V6),
                    ADD_VALUE (OUTBOUND_ICMP_ERROR_V6_DISCARD),
                    ADD_VALUE (OUTBOUND_MAC_FRAME_ETHERNET),
                    ADD_VALUE (OUTBOUND_MAC_FRAME_NATIVE),
                    ADD_VALUE (OUTBOUND_TRANSPORT_FAST),
                    ADD_VALUE (OUTBOUND_MAC_FRAME_NATIVE_FAST),

                    ADD_VALUE (IPFORWARD_V4),
                    ADD_VALUE (IPFORWARD_V4_DISCARD),
                    ADD_VALUE (IPFORWARD_V6),
                    ADD_VALUE (IPFORWARD_V6_DISCARD),

                    ADD_VALUE (STREAM_V4),
                    ADD_VALUE (STREAM_V4_DISCARD),
                    ADD_VALUE (STREAM_V6),
                    ADD_VALUE (STREAM_V6_DISCARD),
                    ADD_VALUE (STREAM_PACKET_V4),
                    ADD_VALUE (STREAM_PACKET_V6),

                    ADD_VALUE (DATAGRAM_DATA_V4),
                    ADD_VALUE (DATAGRAM_DATA_V4_DISCARD),
                    ADD_VALUE (DATAGRAM_DATA_V6),
                    ADD_VALUE (DATAGRAM_DATA_V6_DISCARD),

                    ADD_VALUE (ALE_AUTH_LISTEN_V4),
                    ADD_VALUE (ALE_AUTH_LISTEN_V4_DISCARD),
                    ADD_VALUE (ALE_AUTH_LISTEN_V6),
                    ADD_VALUE (ALE_AUTH_LISTEN_V6_DISCARD),
                    ADD_VALUE (ALE_AUTH_RECV_ACCEPT_V4),
                    ADD_VALUE (ALE_AUTH_RECV_ACCEPT_V4_DISCARD),
                    ADD_VALUE (ALE_AUTH_RECV_ACCEPT_V6),
                    ADD_VALUE (ALE_AUTH_RECV_ACCEPT_V6_DISCARD),
                    ADD_VALUE (ALE_AUTH_CONNECT_V4),
                    ADD_VALUE (ALE_AUTH_CONNECT_V4_DISCARD),
                    ADD_VALUE (ALE_AUTH_CONNECT_V6),
                    ADD_VALUE (ALE_AUTH_CONNECT_V6_DISCARD),
                    ADD_VALUE (ALE_FLOW_ESTABLISHED_V4),
                    ADD_VALUE (ALE_FLOW_ESTABLISHED_V4_DISCARD),
                    ADD_VALUE (ALE_FLOW_ESTABLISHED_V6),
                    ADD_VALUE (ALE_FLOW_ESTABLISHED_V6_DISCARD),
                    ADD_VALUE (ALE_ENDPOINT_CLOSURE_V4),
                    ADD_VALUE (ALE_ENDPOINT_CLOSURE_V6),
                    ADD_VALUE (ALE_CONNECT_REDIRECT_V4),
                    ADD_VALUE (ALE_CONNECT_REDIRECT_V6),
                    ADD_VALUE (ALE_BIND_REDIRECT_V4),
                    ADD_VALUE (ALE_BIND_REDIRECT_V6),
                    ADD_VALUE (ALE_RESOURCE_ASSIGNMENT_V4),
                    ADD_VALUE (ALE_RESOURCE_ASSIGNMENT_V4_DISCARD),
                    ADD_VALUE (ALE_RESOURCE_ASSIGNMENT_V6),
                    ADD_VALUE (ALE_RESOURCE_ASSIGNMENT_V6_DISCARD),
                    ADD_VALUE (ALE_RESOURCE_RELEASE_V4),
                    ADD_VALUE (ALE_RESOURCE_RELEASE_V6),

                    ADD_VALUE (INGRESS_VSWITCH_ETHERNET),
                    ADD_VALUE (INGRESS_VSWITCH_TRANSPORT_V4),
                    ADD_VALUE (INGRESS_VSWITCH_TRANSPORT_V6),

                    ADD_VALUE (EGRESS_VSWITCH_ETHERNET),
                    ADD_VALUE (EGRESS_VSWITCH_TRANSPORT_V4),
                    ADD_VALUE (EGRESS_VSWITCH_TRANSPORT_V6),

                    ADD_VALUE (IPSEC_KM_DEMUX_V4),
                    ADD_VALUE (IPSEC_KM_DEMUX_V6),
                    ADD_VALUE (IPSEC_V4),
                    ADD_VALUE (IPSEC_V6),
                    ADD_VALUE (IKEEXT_V4),
                    ADD_VALUE (IKEEXT_V6),

                    ADD_VALUE (RPC_UM),
                    ADD_VALUE (RPC_EPMAP),
                    ADD_VALUE (RPC_EP_ADD),
                    ADD_VALUE (RPC_PROXY_CONN),
                    ADD_VALUE (RPC_PROXY_IF),

                    ADD_VALUE (KM_AUTHORIZATION),
                    ADD_VALUE (NAME_RESOLUTION_CACHE_V4),
                    ADD_VALUE (NAME_RESOLUTION_CACHE_V6)
                  };

static const char *get_callout_layer_name (const GUID *layer)
{
  const struct GUID_search_list2 *list = fwpm_GUIDs + 0;
  const GUID *guid = list->guid;
  int   i;

  for (i = 0; i < DIM(fwpm_GUIDs); i++)
  {
    if (!memcmp(layer,guid,sizeof(*guid)))
       return (list->name);
    list++;
    guid = list->guid;
  }
  fw_unknown_layers++;
  return ("?");
}

/**
 * "Callouts": a set of functions exposed by a driver and used for specialized filtering.
 * \see
 *  + https://docs.microsoft.com/en-gb/windows/desktop/FWP/about-windows-filtering-platform
 *  + https://github.com/Microsoft/Windows-driver-samples/blob/master/network/trans/WFPSampler/lib/HelperFunctions_FwpmCallout.cpp
 */
BOOL fw_enumerate_callouts (void)
{
  FWPM_CALLOUT0 **entries = NULL;
  HANDLE          fw_callout_handle = INVALID_HANDLE_VALUE;
  UINT32          i, num_in, num_out;
  DWORD           rc;

  if (!p_FwpmCalloutCreateEnumHandle0  ||
      !p_FwpmCalloutDestroyEnumHandle0 ||
      !p_FwpmCalloutEnum0              ||
      !p_FwpmFreeMemory0)
  {
    rc = FW_FUNC_ERROR;
    TRACE (1, "%s() failed: %s.\n", __FUNCTION__, win_strerror(fw_errno));
    return (FALSE);
  }

  if (!fw_create_engine())
     return (FALSE);

  rc = (*p_FwpmCalloutCreateEnumHandle0) (fw_engine_handle, NULL, &fw_callout_handle);
  if (rc != ERROR_SUCCESS)
  {
    fw_errno = rc;
    TRACE (1, "FwpmCalloutCreateEnumHandle0() failed: %s.\n", win_strerror(fw_errno));
    goto fail;
  }

  num_in  = 200;  /* should be plenty */
  num_out = 0;

  rc = (*p_FwpmCalloutEnum0) (fw_engine_handle, fw_callout_handle, num_in, &entries, &num_out);

  if (rc == FWP_E_CALLOUT_NOT_FOUND || rc == FWP_E_NOT_FOUND)
  {
    fw_errno = rc;
    TRACE (1, "FwpmCalloutEnum0() returned no callouts: %s.\n", win_strerror(fw_errno));
    goto fail;
  }
  if (rc != ERROR_SUCCESS)
  {
    fw_errno = rc;
    TRACE (1, "FwpmCalloutEnum0() failed: %s.\n", win_strerror(fw_errno));
    goto fail;
  }

  TRACE (1, "FwpmCalloutEnum0() returned %u entries.\n", num_out);
  for (i = 0; i < num_out; i++)
  {
    const FWPM_CALLOUT0  *entry = entries[i];
    int   indent;
    char  descr [200];

    if (entry->displayData.description)
         WideCharToMultiByte (fw_acp, 0, entry->displayData.description, -1, descr, (int)sizeof(descr), NULL, NULL);
    else strcpy (descr, "<None>");

    fw_buf_add ("~4%2u~0: calloutId: ~3%u:~0\n", i, entry->calloutId);
    fw_buf_add ("    ~4name~0:            %S\n", entry->displayData.name);

    indent = fw_buf_add ("    ~4descr:~0           ") - 4;
    fw_add_long_line (descr, indent, ' ');

    fw_buf_add ("    ~4flags:~2           ");
    fw_add_long_line (get_callout_flag(entry->flags), indent, '|');

    fw_buf_add ("    ~4calloutKey:~0      %s\n", get_guid_string(&entry->calloutKey));
    fw_buf_add ("    ~4providerKey:~0     %s\n", entry->providerKey ? get_guid_string(entry->providerKey) : "<None>");
    fw_buf_add ("    ~4applicableLayer:~0 %s\n%*s= ~2%s~0\n", get_guid_string(&entry->applicableLayer), indent, "",
                                                              get_callout_layer_name(&entry->applicableLayer));

#if 0  /* Never anything here */
   fw_buf_add ("    ~4providerData:~0    ");
   if (entry->providerData.data && entry->providerData.size > 0)
         fw_buf_add ("%.*S\n", entry->providerData.size, entry->providerData.data);
    else fw_buf_add ("<None>\n");
#endif
    fw_buf_addc ('\n');
    fw_buf_flush();
  }

  if (fw_unknown_layers)
     fw_buf_add ("Found %lu unknown callout layer GUIDs.\n", DWORD_CAST(fw_unknown_layers));

  fw_buf_flush();
  fw_unknown_layers = 0;
  fw_errno = ERROR_SUCCESS;

fail:
  if (entries)
    (*p_FwpmFreeMemory0) ((void**)&entries);

  if (fw_callout_handle != INVALID_HANDLE_VALUE)
    (*p_FwpmCalloutDestroyEnumHandle0) (fw_engine_handle, fw_callout_handle);

  return (fw_errno == ERROR_SUCCESS);
}

/**
 * Check for the more interesting DROP events.
 */
static BOOL fw_check_ignore (_FWPM_NET_EVENT_TYPE type)
{
  if (g_cfg.firewall.show_all ||
      type == _FWPM_NET_EVENT_TYPE_CLASSIFY_DROP || type == _FWPM_NET_EVENT_TYPE_CAPABILITY_DROP)
     return (FALSE);
  fw_num_ignored++;
  return (TRUE);
}

/**
 * Dumps recent FW-events from 0-time until now.
 */
static BOOL fw_dump_events (void)
{
  HANDLE  fw_enum_handle = INVALID_HANDLE_VALUE;
  UINT32  i, num_in, num_out;
  DWORD   rc;
  int     api_level = fw_api;
  void   *entries = NULL;

  _FWPM_FILTER_CONDITION0        filter_conditions[5] = { 0 };
  _FWPM_NET_EVENT_ENUM_TEMPLATE0 event_template = { 0 };

  if (api_level < FW_API_LOW || api_level > FW_API_HIGH)
  {
    fw_errno = ERROR_INVALID_DATA;
    TRACE (1, "FwpmNetEventEnum%d() is not a legal API-level.\n", api_level);
    return (FALSE);
  }

  if (!p_FwpmNetEventCreateEnumHandle0 || !p_FwpmNetEventDestroyEnumHandle0 || !p_FwpmFreeMemory0)
  {
    fw_errno = FW_FUNC_ERROR;
    TRACE (1, "%s() failed: %s.\n", __FUNCTION__, win_strerror(fw_errno));
    return (FALSE);
  }

  if (!fw_create_engine())
     return (FALSE);

  fw_num_events = fw_num_ignored = 0UL;

  event_template.numFilterConditions      = 0;
  event_template.filterCondition          = filter_conditions;
  event_template.startTime.dwLowDateTime  = 0UL;
  event_template.startTime.dwHighDateTime = 0UL;
  GetSystemTimeAsFileTime (&event_template.endTime);

  rc = (*p_FwpmNetEventCreateEnumHandle0) (fw_engine_handle, &event_template, &fw_enum_handle);
  if (rc != ERROR_SUCCESS)
  {
    TRACE (1, "FwpmNetEventCreateEnumHandle0() failed: %s.\n", win_strerror(rc));
    goto quit;
  }

  num_in  = INFINITE; /* == 0xFFFFFFFF */
  num_out = 0;

  #define DO_ENUM_LOOP(N, allow_member1, allow_member2, drop_member1, drop_member2)                    \
          do {                                                                                         \
            TRACE (1, "FwpmNetEventEnum%d() returned %u entries.\n", N, num_out);                      \
            for (i = 0; i < num_out; i++)                                                              \
            {                                                                                          \
              const _FWPM_NET_EVENT##N      *entry = entries##N [i];                                   \
              const _FWPM_NET_EVENT_HEADER3 *header = (const _FWPM_NET_EVENT_HEADER3*) &entry->header; \
                                                                                                       \
              if (fw_check_ignore((_FWPM_NET_EVENT_TYPE)entry->type))                                  \
                 continue;                                                                             \
                                                                                                       \
              switch ((_FWPM_NET_EVENT_TYPE)entry->type)  /* Cast to shut-up MinGW/CygWin */           \
              {                                                                                        \
                case _FWPM_NET_EVENT_TYPE_CLASSIFY_DROP:                                               \
                     fw_event_callback (entry->type, header,                                           \
                                        (const _FWPM_NET_EVENT_CLASSIFY_DROP2*) drop_member1,          \
                                        NULL, NULL, NULL);                                             \
                     break;                                                                            \
                case _FWPM_NET_EVENT_TYPE_CLASSIFY_ALLOW:                                              \
                     fw_event_callback (entry->type, header,                                           \
                                        NULL, NULL,                                                    \
                                        (const _FWPM_NET_EVENT_CLASSIFY_ALLOW0*) allow_member1,        \
                                        NULL);                                                         \
                     break;                                                                            \
                case _FWPM_NET_EVENT_TYPE_CAPABILITY_DROP:                                             \
                     fw_event_callback (entry->type, header,                                           \
                                        NULL, drop_member2,                                            \
                                        NULL, NULL);                                                   \
                     break;                                                                            \
                case _FWPM_NET_EVENT_TYPE_CAPABILITY_ALLOW:                                            \
                     fw_event_callback (entry->type, header,                                           \
                                        NULL, NULL,                                                    \
                                        NULL, allow_member2);                                          \
                     break;                                                                            \
                                                                                                       \
                case _FWPM_NET_EVENT_TYPE_IKEEXT_MM_FAILURE:                                           \
                case _FWPM_NET_EVENT_TYPE_IKEEXT_QM_FAILURE:                                           \
                case _FWPM_NET_EVENT_TYPE_IKEEXT_EM_FAILURE:                                           \
                case _FWPM_NET_EVENT_TYPE_IPSEC_KERNEL_DROP:                                           \
                case _FWPM_NET_EVENT_TYPE_IPSEC_DOSP_DROP:                                             \
                case _FWPM_NET_EVENT_TYPE_CLASSIFY_DROP_MAC:                                           \
                case _FWPM_NET_EVENT_TYPE_LPM_PACKET_ARRIVAL:                                          \
                case _FWPM_NET_EVENT_TYPE_MAX:                                                         \
                     TRACE (1, "Ignoring entry->type: %s\n",                                           \
                            list_lookup_name(entry->type, events, DIM(events)));                       \
              }                                                                                        \
            }                                                                                          \
          } while (0)


 /*
  * Ref:
  *   https://github.com/Microsoft/Windows-classic-samples/blob/master/Samples/Win7Samples/netds/wfp/diagevents/diagevents.c#L188
  * for an example use.
  */
  #define GET_ENUM_ENTRIES(N, allow_member1, allow_member2, drop_member1, drop_member2)     \
          do {                                                                              \
            if (api_level == N && p_FwpmNetEventEnum##N)                                    \
            {                                                                               \
              _FWPM_NET_EVENT##N **entries##N = NULL;                                       \
                                                                                            \
              TRACE (2, "Trying FwpmNetEventEnum%d().\n", N);                               \
              rc = (*p_FwpmNetEventEnum##N) (fw_engine_handle, fw_enum_handle,              \
                                             num_in, &entries##N, &num_out);                \
              if (rc != ERROR_SUCCESS)                                                      \
              {                                                                             \
                fw_errno = rc;                                                              \
                TRACE (1, "FwpmNetEventEnum%d() failed: %s\n", N, win_strerror(fw_errno));  \
              }                                                                             \
              else                                                                          \
              {                                                                             \
                entries = (void**) entries##N;                                              \
                DO_ENUM_LOOP (N, allow_member1, allow_member2, drop_member1, drop_member2); \
              }                                                                             \
              goto quit;                                                                    \
            }                                                                               \
          } while (0)

  GET_ENUM_ENTRIES (4, entry->classifyAllow, entry->capabilityAllow, entry->classifyDrop, entry->capabilityDrop);
  GET_ENUM_ENTRIES (3, entry->classifyAllow, entry->capabilityAllow, entry->classifyDrop, entry->capabilityDrop);
  GET_ENUM_ENTRIES (2, entry->classifyAllow, entry->capabilityAllow, entry->classifyDrop, entry->capabilityDrop);
  GET_ENUM_ENTRIES (1, NULL,                 NULL,                   entry->classifyDrop, NULL);
  GET_ENUM_ENTRIES (0, NULL,                 NULL,                   entry->classifyDrop, NULL);

quit:
  fw_errno = rc;

  if (entries)
    (*p_FwpmFreeMemory0) (&entries);

  if (fw_event_handle != INVALID_HANDLE_VALUE)
    (*p_FwpmNetEventDestroyEnumHandle0) (fw_engine_handle, fw_enum_handle);

  ARGSUSED (filter_conditions);
  return (fw_errno == ERROR_SUCCESS);
}

#undef  ADD_VALUE
#define ADD_VALUE(v)  { FWPM_APPC_NETWORK_CAPABILITY_##v , "FWPM_APPC_NETWORK_CAPABILITY_" #v }

static const struct search_list network_capabilities[] = {
                    ADD_VALUE (INTERNET_CLIENT),
                    ADD_VALUE (INTERNET_CLIENT_SERVER),
                    ADD_VALUE (INTERNET_PRIVATE_NETWORK)
                  };

static const char *get_network_capability_id (int id)
{
  return list_lookup_name (id, network_capabilities, DIM(network_capabilities));
}

/**
 * Lookup the entry for a `filter` value in the `fw_filter_list` cache.
 * If not found, add an entry for it.
 *
 * \note a `filter == 0` is never valid.
 */
static const struct filter_entry *lookup_or_add_filter (UINT64 filter)
{
  static struct filter_entry null_filter = { 0, "NULL" };
  struct filter_entry *fe;
  FWPM_FILTER0        *filter_item;
  int                  i, max;

  if (filter == 0UL)
     return (&null_filter);

  max = smartlist_len (fw_filter_list);
  for (i = 0; i < max; i++)
  {
    fe = smartlist_get (fw_filter_list, i);
    if (filter == fe->value)
       return (fe);
  }

  fe = calloc (sizeof(*fe), 1);
  fe->value = filter;
  strcpy (fe->name, "?");

  if ((*p_FwpmFilterGetById0)(fw_engine_handle, filter, &filter_item) == ERROR_SUCCESS)
  {
    WideCharToMultiByte (fw_acp, 0, filter_item->displayData.name, -1, fe->name, (int)sizeof(fe->name), NULL, NULL);
    (*p_FwpmFreeMemory0) ((void**)&filter_item);
  }
  smartlist_add (fw_filter_list, fe);
  return (fe);
}

static BOOL print_layer_item (const _FWPM_NET_EVENT_CLASSIFY_DROP2  *drop_event,
                              const _FWPM_NET_EVENT_CLASSIFY_ALLOW0 *allow_event)
{
  FWPM_LAYER0 *layer_item = NULL;
  UINT16       id = 0;

  if (drop_event)
     id = drop_event->layerId;
  else if (allow_event)
     id = allow_event->layerId;

  if (id && (*p_FwpmLayerGetById0)(fw_engine_handle, id, &layer_item) == ERROR_SUCCESS)
  {
    fw_buf_add ("%-*slayer:   (%u) %S\n", INDENT_SZ, "", id, layer_item->displayData.name);
    (*p_FwpmFreeMemory0) ((void**)&layer_item);
  }
  return (id != 0);
}

static BOOL print_layer_item2 (const _FWPM_NET_EVENT_CAPABILITY_DROP0  *drop_event,
                               const _FWPM_NET_EVENT_CAPABILITY_ALLOW0 *allow_event)
{
  int    capability_id = allow_event ? allow_event->networkCapabilityId : drop_event->networkCapabilityId;
  int    is_loopback   = allow_event ? allow_event->isLoopback          : drop_event->isLoopback;
  UINT64 filter_id     = allow_event ? allow_event->filterId            : drop_event->filterId;

  fw_buf_add ("%-*slayer2:  ", INDENT_SZ, "");
  if (filter_id)
  {
    const struct filter_entry *fe = lookup_or_add_filter (filter_id);

    fw_buf_add ("(%" U64_FMT ") %s, ", fe->value, fe->name);
  }
  fw_buf_add ("%s, isLoopback: %d\n", get_network_capability_id(capability_id), is_loopback);
  return (filter_id != 0);
}

static BOOL print_filter_rule (const _FWPM_NET_EVENT_CLASSIFY_DROP2  *drop_event,
                               const _FWPM_NET_EVENT_CLASSIFY_ALLOW0 *allow_event)
{
  UINT64 filter_id = 0UL;

  if (drop_event)
     filter_id = drop_event->filterId;
  else if (allow_event)
     filter_id = allow_event->filterId;

  if (filter_id)
  {
    const struct filter_entry *fe = lookup_or_add_filter (filter_id);

    fw_buf_add ("%-*sfilter:  (%" U64_FMT ") %s\n", INDENT_SZ, "", fe->value, fe->name);
    return (TRUE);
  }
  return (FALSE);
}

static BOOL print_filter_rule2 (const _FWPM_NET_EVENT_CAPABILITY_DROP0  *drop_event,
                                const _FWPM_NET_EVENT_CAPABILITY_ALLOW0 *allow_event)
{
  const struct filter_entry *fe = NULL;

  if (drop_event)
     fe = lookup_or_add_filter (drop_event->filterId);
  else if (allow_event)
     fe = lookup_or_add_filter (allow_event->filterId);

  if (fe)
  {
    fw_buf_add ("%-*sfilter:  (%" U64_FMT ") %s\n", INDENT_SZ, "", fe->value, fe->name);
    return (TRUE);
  }
  return (FALSE);
}

static void print_country_location (const struct in_addr *ia4, const struct in6_addr *ia6)
{
  const char *country, *location;
  BOOL  have_location;

  have_location = ia4 ? fw_have_ip2loc4                : fw_have_ip2loc6;
  country       = ia4 ? geoip_get_country_by_ipv4(ia4) : geoip_get_country_by_ipv6(ia6);

  if (!country || country[0] == '-')
     return;

  /* Get the long country name; "US" -> "United States"
   */
  country = geoip_get_long_name_by_A2 (country);
  if (have_location)
  {
    /* Location is known. Print as "country, city/region".
     */
    location = ia4 ? geoip_get_location_by_ipv4(ia4) : geoip_get_location_by_ipv6(ia6);
    fw_buf_add ("%-*scountry: %s, %s\n", INDENT_SZ, "", country, location ? location : "?");
  }
  else
  {
    /* Location is unknown. Just print the country.
     */
    fw_buf_add ("%-*scountry: %s\n", INDENT_SZ, "", country);
  }
}

#define PORT_STR_SIZE  80
#define PORTS_FMT      ", ports: %s / %s"

static void get_port (const _FWPM_NET_EVENT_HEADER3 *header, WORD port, char *port_str)
{
  struct servent *se = NULL;

  /* If called when wsock_trace.dll is active, we might get "late events".
   * Hence we cannot call `getservbyport()` after a `WSACleanup()`.
   * Just return the port-number as a string.
   */
#if !defined(TEST_FIREWALL)
  if (cleaned_up)
  {
    _itoa (port, port_str, 10);
    return;
  }
#endif

  /* Do not use 'WSTRACE()' on 'getservbyport()' here.
   */
  trace_level_save_restore (0);

  if (header->ipProtocol == IPPROTO_TCP)
     se = getservbyport (_byteswap_ushort(port), "tcp");
  else if (header->ipProtocol == IPPROTO_UDP)
     se = getservbyport (_byteswap_ushort(port), "udp");

  if (se && se->s_name)
       snprintf (port_str, PORT_STR_SIZE, "%d (%s)", port, se->s_name);
  else _itoa (port, port_str, 10);

  trace_level_save_restore (1);
}

static const char *get_ports (const _FWPM_NET_EVENT_HEADER3 *header)
{
  static char ret [sizeof(PORTS_FMT) + 2*PORT_STR_SIZE];
  char local_port [PORT_STR_SIZE];
  char remote_port[PORT_STR_SIZE];

  if (header->ipProtocol != IPPROTO_UDP && header->ipProtocol != IPPROTO_TCP)
     return ("");

  if (header->flags & FWPM_NET_EVENT_FLAG_LOCAL_PORT_SET)
       get_port (header, header->localPort, local_port);
  else strcpy (local_port, "-");

  if (header->flags & FWPM_NET_EVENT_FLAG_REMOTE_PORT_SET)
       get_port (header, header->remotePort, remote_port);
  else strcpy (remote_port, "-");

  snprintf (ret, sizeof(ret), PORTS_FMT, local_port, remote_port);
  return (ret);
}

/**
 * If it's an IPv4 `ALLOW` / `DROP` event, print the local / remote addresses for it.
 */
static BOOL print_addresses_ipv4 (const _FWPM_NET_EVENT_HEADER3 *header, BOOL direction_in)
{
  struct in_addr ia4;
  const char    *ports;
  char           local_addr [INET_ADDRSTRLEN];
  char           remote_addr [INET_ADDRSTRLEN];
  DWORD          ip;

  if (header->ipVersion != FWP_IP_VERSION_V4)
     return (FALSE);

  if ((header->flags & FWPM_NET_EVENT_FLAG_IP_VERSION_SET) == 0 ||
      (header->flags & (FWPM_NET_EVENT_FLAG_LOCAL_ADDR_SET | FWPM_NET_EVENT_FLAG_REMOTE_ADDR_SET)) == 0)
     return (FALSE);

  if (header->flags & FWPM_NET_EVENT_FLAG_LOCAL_ADDR_SET)
  {
    ip = _byteswap_ulong (*(DWORD*)&header->localAddrV4);
    inet_ntop (AF_INET, (INET_NTOP_ADDR)&ip, local_addr, sizeof(local_addr));
  }
  else
    strcpy (local_addr, "-");

  if (header->flags & FWPM_NET_EVENT_FLAG_REMOTE_ADDR_SET)
  {
    ip = _byteswap_ulong (*(DWORD*)&header->remoteAddrV4);
    inet_ntop (AF_INET, (INET_NTOP_ADDR)&ip, remote_addr, sizeof(remote_addr));
  }
  else
    strcpy (remote_addr, "-");

  if (*local_addr != '-' && exclude_list_get(local_addr,EXCL_ADDRESS))
  {
    TRACE (2, "Ignoring event for local_addr: %s.\n", local_addr);
    return (FALSE);
  }
  if (*remote_addr != '-' && exclude_list_get(remote_addr,EXCL_ADDRESS))
  {
    TRACE (2, "Ignoring event for remote_addr: %s.\n", remote_addr);
    return (FALSE);
  }

  fw_buf_add ("%-*s", INDENT_SZ, "");

  ports = get_ports (header);

  if (direction_in)
       fw_buf_add ("addr:    %s -> %s%s\n", remote_addr, local_addr, ports);
  else fw_buf_add ("addr:    %s -> %s%s\n", local_addr, remote_addr, ports);

  if (header->flags & FWPM_NET_EVENT_FLAG_REMOTE_ADDR_SET)
  {
    ia4.s_addr = _byteswap_ulong (*(DWORD*)&header->remoteAddrV4);
    print_country_location (&ia4, NULL);
  }

  return (TRUE);
}

/**
 * If it's an IPv6 `ALLOW` / `DROP` event, print the local / remote addresses for it.
 */
static BOOL print_addresses_ipv6 (const _FWPM_NET_EVENT_HEADER3 *header, BOOL direction_in)
{
  const char *ports;
  char        local_addr [INET6_ADDRSTRLEN];
  char        remote_addr [INET6_ADDRSTRLEN];
  char        scope [20];

  if (header->ipVersion != FWP_IP_VERSION_V6)
     return (FALSE);

  if ((header->flags & FWPM_NET_EVENT_FLAG_IP_VERSION_SET) == 0 ||
      (header->flags & (FWPM_NET_EVENT_FLAG_LOCAL_ADDR_SET | FWPM_NET_EVENT_FLAG_REMOTE_ADDR_SET)) == 0)
     return (FALSE);

  if (header->flags & FWPM_NET_EVENT_FLAG_LOCAL_ADDR_SET)
       inet_ntop (AF_INET6, (INET_NTOP_ADDR)&header->localAddrV6, local_addr, sizeof(local_addr));
  else strcpy (local_addr, "-");

  if (header->flags & FWPM_NET_EVENT_FLAG_REMOTE_ADDR_SET)
       inet_ntop (AF_INET6, (INET_NTOP_ADDR)&header->remoteAddrV6, remote_addr, sizeof(remote_addr));
  else strcpy (remote_addr, "-");

  if (*local_addr != '-' && exclude_list_get(local_addr,EXCL_ADDRESS))
  {
    TRACE (2, "Ignoring event for local_addr: %s.\n", local_addr);
    return (FALSE);
  }
  if (*remote_addr != '-' && exclude_list_get(remote_addr,EXCL_ADDRESS))
  {
    TRACE (2, "Ignoring event for remote_addr: %s.\n", remote_addr);
    return (FALSE);
  }

  fw_buf_add ("%-*s", INDENT_SZ, "");

  ports = get_ports (header);

  if (header->flags & FWPM_NET_EVENT_FLAG_SCOPE_ID_SET)
  {
    scope[0] = '%';
    _itoa (header->scopeId, scope+1, 10);
  }
  else
    scope[0] = '\0';

  if (direction_in)
       fw_buf_add ("addr:   %s -> %s%s%s\n", remote_addr, local_addr, scope, ports);
  else fw_buf_add ("addr:   %s%s -> %s%s\n", local_addr, scope, remote_addr, ports);

  if (header->flags & FWPM_NET_EVENT_FLAG_REMOTE_ADDR_SET)
     print_country_location (NULL, (const struct in6_addr*)&header->remoteAddrV6);

  return (TRUE);
}

/**
 * Map a "\\device\\harddiskvolume[0-9]\\" string to a drive letter the easy way.
 * Somewhat related:
 *   https://stackoverflow.com/questions/18509633/how-do-i-map-the-device-details-such-as-device-harddisk1-dr1-in-the-event-log-t
 */
static const char *volume_to_path (const char *volume)
{
  #define VOLUME "\\Device\\HarddiskVolume"
  const  char *p;
  static char  path [_MAX_PATH];

  if (!strnicmp(volume, VOLUME, sizeof(VOLUME)-1))
  {
    p = volume + sizeof(VOLUME) - 1;
    if (isdigit(*p) && p[1] == '\\')
    {
      path[0] = 'a' - '0' + *p;
      path[1] = ':';
      _strlcpy (path+2, p+1, sizeof(path));
      return (path);
    }
  }
  return (volume);
}

/**
 * Process the `header->appId` field.
 */
static BOOL print_app_id (const _FWPM_NET_EVENT_HEADER3 *header)
{
  char    a_name [_MAX_PATH];
  char   *a_base;
  LPCWSTR w_name;
  int     w_len;

  if ((header->flags & FWPM_NET_EVENT_FLAG_APP_ID_SET) == 0 ||
      !header->appId.data || header->appId.size == 0)
     return (TRUE);    /* Can't exclude a `appId` based on this */

  w_name = (LPCWSTR) header->appId.data;
  w_len  = header->appId.size;

  if (WideCharToMultiByte(fw_acp, 0, w_name, w_len, a_name, (int)sizeof(a_name), NULL, NULL) == 0)
       _strlcpy (a_name, "?", sizeof(a_name));
  else _strlcpy (a_name, volume_to_path(a_name), sizeof(a_name));

  a_base = basename (a_name);

  if (g_cfg.firewall.show_all == 0)
  {
    if (!stricmp(fw_module, a_name) || !stricmp(fw_module, a_base))
    {
      TRACE (1, "Got event for fw_module: '%s' matching '%s'.\n", fw_module, a_name);
      return (TRUE);
    }
    return (FALSE);
  }

  if (exclude_list_get(a_base,EXCL_PROGRAM) ||  /* short file-name */
      exclude_list_get(a_name,EXCL_PROGRAM))    /* full file-name */
  {
    TRACE (2, "Ignoring event for '%s'.\n", a_name);
    return (FALSE);
  }
  fw_buf_add ("%-*sapp:     %s\n", INDENT_SZ, "", a_name);
  return (TRUE);
}

/**
 * Process the `header->effectiveName` field.
 */
static void print_eff_name_id (const _FWPM_NET_EVENT_HEADER3 *header)
{
  LPCWSTR w_name;
  int     w_len;

  if ((header->flags & FWPM_NET_EVENT_FLAG_EFFECTIVE_NAME_SET) == 0 ||
      !header->effectiveName.data || header->effectiveName.size == 0)
     return;

  w_name = (LPCWSTR) header->effectiveName.data;
  w_len  = header->effectiveName.size;
  fw_buf_add ("\n%-*seff:      %.*S\n", INDENT_SZ, "", w_len, w_name);
}

/**
 * Lookup the account and domain for a `sid` to get
 * a more sensible account and domain-name.
 */
static BOOL lookup_account_SID (const SID *sid, const char *sid_str, char *account, char *domain)
{
  SID_NAME_USE sid_use = SidTypeUnknown;
  DWORD        account_sz = 0;
  DWORD        domain_sz  = 0;
  DWORD        err, rc;

  /* First call to LookupAccountSid() to get the sizes of account/domain names.
   */
  rc = LookupAccountSid (NULL, (PSID)sid,
                         NULL, (DWORD*)&account_sz,
                         NULL, (DWORD*)&domain_sz,
                         &sid_use);

  if (domain_sz > MAX_ACCOUNT_SZ)
      domain_sz = MAX_DOMAIN_SZ;
  if (account_sz > MAX_ACCOUNT_SZ)
      account_sz = MAX_ACCOUNT_SZ;

  /* If no mapping between SID and account-name, just return the
   * account-name as a SID-string. And no domain-name.
   */
  if (!rc && GetLastError() == ERROR_NONE_MAPPED && sid_use == SidTypeUnknown)
  {
    TRACE (2, "No account mapping for SID: %s.\n", sid_str);
    _strlcpy (account, sid_str, MAX_ACCOUNT_SZ);
    return (TRUE);
  }

  rc = LookupAccountSid (NULL,                  /* NULL -> name of local computer */
                         (PSID)sid,             /* security identifier */
                         account,               /* account name buffer */
                         (DWORD*)&account_sz,   /* size of account name buffer */
                         domain,                /* domain name */
                         (DWORD*)&domain_sz,    /* size of domain name buffer */
                         &sid_use);             /* SID type */
  if (!rc)
  {
    err = GetLastError();
    if (err == ERROR_NONE_MAPPED)
         TRACE (1, "Account owner not found for specified SID.\n");
    else TRACE (1, "Error in LookupAccountSid(): %s.\n", win_strerror(err));
    return (FALSE);
  }
  return (TRUE);
}

/**
 * Lookup the entry for the `sid` in the `fw_SID_list` cache.
 * If not found, add an entry for it.
 */
static struct SID_entry *lookup_or_add_SID (SID *sid)
{
  struct SID_entry *se;
  DWORD  len;
  int    i, max = smartlist_len (fw_SID_list);

  for (i = 0; i < max; i++)
  {
    se = smartlist_get (fw_SID_list, i);
    if (EqualSid(sid, se->sid_copy))
       return (se);
  }

  len = GetLengthSid (sid);
  se  = calloc (sizeof(*se) + len, 1);
  se->sid_copy = (SID*) (se + 1);
  CopySid (len, se->sid_copy, sid);
  ConvertSidToStringSid (sid, &se->sid_str);

  lookup_account_SID (sid, se->sid_str, se->account, se->domain);
  smartlist_add (fw_SID_list, se);
  return (se);
}

/**
 * Process the `header->userId` field.
 */
static BOOL print_user_id (const _FWPM_NET_EVENT_HEADER3 *header)
{
  const struct SID_entry *se;

  if (!(header->flags & FWPM_NET_EVENT_FLAG_USER_ID_SET) || !header->userId)
     return (TRUE);

  se = lookup_or_add_SID (header->userId);

  /* Show activity for logged-on user only
   */
  if (g_cfg.firewall.show_user && !stricmp(se->account, fw_logged_on_user))
     return (FALSE);

  fw_buf_add ("%-*suser:    %s\\%s\n",
              INDENT_SZ, "", se->domain[0] ? se->domain : "?", se->account[0] ? se->account : "?");
  return (TRUE);
}

/**
 * Process the `header->packageSid` field.
 */
static BOOL print_package_id (const _FWPM_NET_EVENT_HEADER3 *header)
{
  #define NULL_SID "S-1-0-0"
  const struct SID_entry *se;

  if (!(header->flags & FWPM_NET_EVENT_FLAG_PACKAGE_ID_SET) || !header->packageSid)
     return (TRUE);

  se = lookup_or_add_SID (header->packageSid);

  if (se->sid_str && (g_cfg.firewall.show_all || strcmp(NULL_SID, se->sid_str)))
  {
    fw_buf_add ("%-*spackage: %s\n", INDENT_SZ, "", se->sid_str);
    return (TRUE);
  }
  return (FALSE);
}

static void print_reauth_reason (const _FWPM_NET_EVENT_HEADER3         *header,
                                 const _FWPM_NET_EVENT_CLASSIFY_DROP2  *drop_event,
                                 const _FWPM_NET_EVENT_CLASSIFY_ALLOW0 *allow_event)
{
  if (!(header->flags & FWPM_NET_EVENT_FLAG_REAUTH_REASON_SET))
     return;

  fw_buf_add ("%-*sreauth:  ", INDENT_SZ, "");
  if (drop_event)
       fw_buf_add ("%lu\n", DWORD_CAST(drop_event->reauthReason));
  else fw_buf_add ("%lu\n", DWORD_CAST(allow_event->reauthReason));
}

static const char *get_protocol (UINT8 proto)
{
  return list_lookup_name (proto, protocols, DIM(protocols));
}

static void CALLBACK
  fw_event_callback (const UINT                               event_type,
                     const _FWPM_NET_EVENT_HEADER3           *header,
                     const _FWPM_NET_EVENT_CLASSIFY_DROP2    *drop_event1,
                     const _FWPM_NET_EVENT_CAPABILITY_DROP0  *drop_event2,
                     const _FWPM_NET_EVENT_CLASSIFY_ALLOW0   *allow_event1,
                     const _FWPM_NET_EVENT_CAPABILITY_ALLOW0 *allow_event2)
{
  BOOL        direction_in  = FALSE;
  BOOL        direction_out = FALSE;
  BOOL        address_printed, program_printed, user_printed, pkg_printed;
  DWORD       unhandled_flags;
  const char *event_name;

  fw_buf_reset();

  if (header->flags & FWPM_NET_EVENT_FLAG_IP_VERSION_SET)
  {
    if ((header->ipVersion == FWP_IP_VERSION_V4 && !g_cfg.firewall.show_ipv4) ||
        (header->ipVersion == FWP_IP_VERSION_V6 && !g_cfg.firewall.show_ipv6))
    {
      fw_num_ignored++;
      TRACE (2, "Ignoring IPv%d event.\n", header->ipVersion == FWP_IP_VERSION_V4 ? 4 : 6);
      return;
    }
  }

  /**
   * The `address__printed` variable is used to examine all the pieces of an event and the return value
   * of `exclude_list_get (address_str, EXCL_ADDRESS)` before desiding to print anything.
   * The same goes for `exclude_list_get (appId, EXCL_PROGRAM)`.
   *
   * If both `X_printed` are `FALSE`, `fw_buf_reset()` is called and nothing gets printed to `trace_puts()`.
   */
  event_name = list_lookup_name(event_type, events, DIM(events));

  fw_buf_add (TIME_STRING_FMT "~4%s~0",
              get_time_string(&header->timeStamp), event_name);

  if (event_type == _FWPM_NET_EVENT_TYPE_CLASSIFY_DROP)
  {
    if (drop_event1->msFwpDirection == FWP_DIRECTION_IN ||
        drop_event1->msFwpDirection == FWP_DIRECTION_INBOUND)
       direction_in = TRUE;
    else
    if (drop_event1->msFwpDirection == FWP_DIRECTION_OUT ||
        drop_event1->msFwpDirection == FWP_DIRECTION_OUTBOUND)
       direction_out = TRUE;

    /* API 0-2 doesn't set the `header->msFwpDirection` correctly.
     */
    if (!direction_in && !direction_out)
       direction_in = TRUE;

    if (direction_in || direction_out)
       fw_buf_add (", ~3%s~0", list_lookup_name(drop_event1->msFwpDirection, directions, DIM(directions)));

    if (header->flags & FWPM_NET_EVENT_FLAG_IP_PROTOCOL_SET)
         fw_buf_add (", %s\n", get_protocol(header->ipProtocol));
    else fw_buf_addc ('\n');

    print_layer_item (drop_event1, NULL);
    print_filter_rule (drop_event1, NULL);
  }
  else if (event_type == _FWPM_NET_EVENT_TYPE_CLASSIFY_ALLOW)
  {
    if (allow_event1->msFwpDirection == FWP_DIRECTION_IN ||
        allow_event1->msFwpDirection == FWP_DIRECTION_INBOUND)
       direction_in = TRUE;
    else
    if (allow_event1->msFwpDirection == FWP_DIRECTION_OUT ||
        allow_event1->msFwpDirection == FWP_DIRECTION_OUTBOUND)
       direction_out = TRUE;

    /* API 0-2 doesn't set the `header->msFwpDirection` correctly.
     */
    if (!direction_in && !direction_out)
       direction_in = TRUE;

    if (direction_in || direction_out)
       fw_buf_add (", ~3%s~0", list_lookup_name(allow_event1->msFwpDirection, directions, DIM(directions)));

    if (header->flags & FWPM_NET_EVENT_FLAG_IP_PROTOCOL_SET)
         fw_buf_add (", %s\n", get_protocol(header->ipProtocol));
    else fw_buf_addc ('\n');

    print_layer_item (NULL, allow_event1);
    print_filter_rule (NULL, allow_event1);
  }
  else if (event_type == _FWPM_NET_EVENT_TYPE_CAPABILITY_ALLOW)
  {
    direction_in = TRUE;

    if (header->flags & FWPM_NET_EVENT_FLAG_IP_PROTOCOL_SET)
       fw_buf_add (", %s\n", get_protocol(header->ipProtocol));

    print_layer_item2 (NULL, allow_event2);
    print_filter_rule2 (NULL, allow_event2);
  }
  else if (event_type == _FWPM_NET_EVENT_TYPE_CAPABILITY_DROP)
  {
    direction_in = TRUE;

    if (header->flags & FWPM_NET_EVENT_FLAG_IP_PROTOCOL_SET)
       fw_buf_add (", %s\n", get_protocol(header->ipProtocol));

    print_layer_item2 (drop_event2, NULL);
    print_filter_rule2 (drop_event2, NULL);
  }
  else
    return;  /* Impossible */

  /* Print the local / remote addresses and ports for IPv4 / IPv6.
   * A single event can only match IPv4 or IPv6 (or something else).
   */
  address_printed = print_addresses_ipv4 (header, direction_in);

  if (!address_printed)
     address_printed = print_addresses_ipv6 (header, direction_in);

  program_printed = print_app_id (header);
  user_printed    = print_user_id (header);
  pkg_printed     = print_package_id (header);

  print_eff_name_id (header);

  if (event_type == _FWPM_NET_EVENT_TYPE_CLASSIFY_ALLOW ||
      event_type == _FWPM_NET_EVENT_TYPE_CLASSIFY_DROP)
     print_reauth_reason (header, drop_event1, allow_event1);

  /* We filter on addresses, programs, logged-on user and packages.
   */
  if (!user_printed)
     address_printed = FALSE;

  if (!program_printed)
     address_printed = FALSE;

  if (address_printed || program_printed || user_printed || pkg_printed)
  {
    fw_buf_flush();
    fw_num_events++;
  }
  else
  {
    fw_buf_reset();
    fw_num_ignored++;
  }

  unhandled_flags = header->flags & (FWPM_NET_EVENT_FLAG_ENTERPRISE_ID_SET |
                                     FWPM_NET_EVENT_FLAG_POLICY_FLAGS_SET  |
                                     FWPM_NET_EVENT_FLAG_EFFECTIVE_NAME_SET);
  if (unhandled_flags)
     TRACE (1, "Unhandled %s header->flags: %s\n",
            event_name, flags_decode(unhandled_flags, ev_flags, DIM(ev_flags)));
}

void fw_print_statistics (FWPM_STATISTICS *stats)
{
  if (fw_num_events > 0UL || fw_num_ignored > 0UL)
  {
     trace_printf ("Got %lu events, %lu ignored.\n",
                   DWORD_CAST(fw_num_events), DWORD_CAST(fw_num_ignored));

    if (g_cfg.geoip_enable)
    {
      DWORD num_ip4, num_ip6;

      geoip_num_unique_countries (&num_ip4, &num_ip6, NULL, NULL);

      if (g_cfg.firewall.show_ipv4)
         trace_printf ("Unique IPv4 countries: %3lu.\n", DWORD_CAST(num_ip4));
      if (g_cfg.firewall.show_ipv6)
         trace_printf ("Unique IPv6 countries: %3lu.\n", DWORD_CAST(num_ip6));
    }
  }
  ARGSUSED (stats);
}

/*
 * Return text of FWP_E_* error codes in range 0x80320001 - 0x80320039 and
 * RPC_* error codes in range 0x80010001 - 0x80010122.
 *
 * win_strerror() works for these ranges too.
 */
const char *fw_strerror (DWORD err)
{
  return win_strerror (err);
}

#if defined(TEST_FIREWALL)

#include <signal.h>
#include "getopt.h"

/**
 * For getopt.c.
 */
const char *program_name = "firewall_test.exe";
static int  quit;

/**
 * Return a `malloc()`ed string of the program (with arguments) to pass to `_popen()`.
 */
static char *set_net_program (int argc, char **argv)
{
  char   *prog = NULL;
  size_t len, i;

  for (i = len = 0; i < (size_t)argc; i++)
      len += strlen (argv[i]) + 2;

  if (len > 0)
  {
    prog = malloc (len+1);
    *prog = '\0';
    for (i = 0; i < (size_t)argc; i++)
    {
      strcat (prog, argv[i]);
      if (i < (size_t)(argc-1))
         strcat (prog, " ");  /* Add a space between arguments (but not after last) */
    }
  }
  return (prog);
}

static int show_help (const char *my_name)
{
  printf ("Simple Windows ICF Firewall monitor test program.\n"
          "  Usage: %s [options] [program]\n"
          "  options:\n"
          "    -a:  the API-level to use (%d-%d, default: %d).\n"
          "    -c:  only dump the callout rules.\n"
          "    -e:  only dump recent event; does not work with \"-a0\" or \"-a1\".\n"
          "    -l:  print to \"log-file\" only.\n"
          "    -p:  print events for the below program only (implies your \"user-activity\" only).\n"
          "    -r:  only dump the firewall rules.\n"
          "    -v:  sets \"g_cfg.firewall.show_all = 1\".\n"
          "\n"
          "  program: the program (and arguments) to test Firewall activity with.\n"
          "    Does not work with GUI programs. Event may come in late. So an extra \"sleep\" is handy.\n"
          "    Examples:\n"
          "      pause\n"
          "      ping -n 10 www.google.com\n"
          "      \"wget -d -o- -O NUL www.google.com & sleep 3\"\n",
          my_name, FW_API_LOW, FW_API_HIGH, FW_API_DEFAULT);
  return (0);
}

static void sig_handler (int sig)
{
  quit = 1;
  trace_puts ("~1Quitting.~0\n");
  (void) sig;
}

/*
 * This mysterious SID was found in FirewallApi.DLL.
 * Figure out what account it has.
 */
static int test_SID (void)
{
  const char *sid_str = "S-1-15-3-4214768333-1334025770-122408079-3919188833";
  char        account[MAX_ACCOUNT_SZ];
  char        domain [MAX_DOMAIN_SZ];
  PSID        sid = NULL;

  g_cfg.trace_level = 2;
  if (!ConvertStringSidToSid(sid_str, &sid))
  {
    printf ("ConvertStringSidToSid() failed.\n");
    return (1);
  }
  account[0] = domain[0] = '\0';
  lookup_account_SID (sid, sid_str, account, domain);
  printf ("SID: %s -> %s\\%s\n", sid_str, domain[0] ? domain : "?", account[0] ? account : "?");
  LocalFree (sid);
  return (0);
}

static int run_program (const char *program)
{
  FILE *p;
  char  p_buf [1000];
  const char *what;

  what = g_cfg.firewall.show_ipv4 &&  g_cfg.firewall.show_ipv6 ? "IPv4/6 " :
         g_cfg.firewall.show_ipv4 && !g_cfg.firewall.show_ipv6 ? "IPv4 "   :
        !g_cfg.firewall.show_ipv4 &&  g_cfg.firewall.show_ipv6 ? "IPv6 "   : "non-IPv4/IPv6 ";

  trace_printf ("Executing ~1%s~0 while listening for %sFilter events.\n",
                program ? program : "no program", what);

  if (!program)
     return (1);

  p = _popen (program, "rb");
  if (!p)
  {
    TRACE (0, "_popen() failed, errno %d\n", errno);
    return (1);
  }

  while (fgets(p_buf,sizeof(p_buf)-1,p) && !quit)
  {
    trace_puts ("~1program: ");
    trace_puts_raw (p_buf);
    trace_puts ("~0");
    trace_flush();
  }
  _pclose (p);
  return (0);
}

int main (int argc, char **argv)
{
  int     ch, rc = 1;
  int     dump_rules = 0;
  int     dump_callouts = 0;
  int     dump_events = 0;
  int     program_only = 0;  /* Capture 'appId' matching program or 'userId' matching logged-on user only. */
  char   *program;
  char   *log_file = NULL;
  FILE   *log_f    = NULL;
  WSADATA wsa;
  WORD    ver = MAKEWORD(2,2);

  wsock_trace_init();

  g_cfg.trace_use_ods = g_cfg.DNSBL.test = FALSE;
  g_cfg.trace_indent  = 0;
  g_cfg.trace_report  = 1;

  while ((ch = getopt(argc, argv, "a:h?cel:prtv")) != EOF)
    switch (ch)
    {
      case 'a':
           fw_api = atoi (optarg);
           break;
      case 'c':
           dump_callouts = 1;
           break;
      case 'e':
           dump_events = 1;
           break;
      case 'l':
           log_file = strdup (optarg);
           break;
      case 'p':
           program_only = 1;
           break;
      case 'r':
           dump_rules = 1;
           break;
      case 't':
           return test_SID();
      case 'v':
           g_cfg.firewall.show_all = 1;
           break;
      case '?':
      case 'h':
           return show_help (argv[0]);
    }

  program = set_net_program (argc-optind, argv+optind);

  /* Because we use `getservbyport()` above, we need to call `WSAStartup()` first.
   */
  if (WSAStartup(ver, &wsa) != 0 || wsa.wVersion < ver)
  {
    TRACE (0, "Winsock init failed: %s\n", win_strerror(GetLastError()));
    goto quit;
  }

  if (log_file)
  {
    log_f  = fopen (log_file, "wb+");
    g_cfg.trace_stream = log_f;
    if (!g_cfg.trace_stream)
    {
      TRACE (0, "Failed to create log-file %s: %s.\n", log_file, strerror(errno));
      goto quit;
    }
  }

  if (program_only)
  {
    if (program)
    {
      char *space;

      _strlcpy (fw_module, program, sizeof(fw_module));
      space = strchr (fw_module, ' ');
      if (space)
         *space = '\0';
      exclude_list_add (fw_module, EXCL_PROGRAM);
    }
    g_cfg.firewall.show_all  = 0;
    g_cfg.firewall.show_user = 1;
    TRACE (1, "fw_module: '%s'. Exists: %d\n", fw_module, file_exists(fw_module));
  }

  if (!fw_init())
  {
    TRACE (0, "fw_init() failed: %s\n", win_strerror(fw_errno));
    goto quit;
  }

  if (dump_rules || dump_callouts || dump_events)
  {
    if (dump_rules)
       fw_enumerate_rules();

    if (dump_events)
       fw_dump_events();

    if (dump_callouts)
       fw_enumerate_callouts();
  }
  else if (fw_monitor_start())
  {
    signal (SIGINT, sig_handler);
    rc = run_program (program);
  }
  else
    TRACE (0, "fw_monitor_start() failed: %s\n", win_strerror(fw_errno));

quit:
  fw_print_statistics (NULL);
  free (program);
  free (log_file);
  wsock_trace_exit();

  if (log_f)
     fclose (log_f);

  return (rc);
}
#endif  /* TEST_FIREWALL */

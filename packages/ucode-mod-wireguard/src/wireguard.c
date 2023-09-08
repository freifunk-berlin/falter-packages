
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <limits.h>
#include <math.h>
#include <assert.h>
#include <fcntl.h>
#include <poll.h>
#include <ctype.h>

#include <net/if.h>
#include <netinet/ether.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netlink/msg.h>
#include <netlink/attr.h>
#include <netlink/socket.h>
#include <netlink/genl/genl.h>
#include <netlink/genl/family.h>
#include <netlink/genl/ctrl.h>

#include <linux/wireguard.h>
#include <libubox/utils.h>

#include "ucode/module.h"

#define DIV_ROUND_UP(n, d)      (((n) + (d) - 1) / (d))

#define err_return(code, ...) do { set_error(code, __VA_ARGS__); return NULL; } while(0)

static struct {
  int code;
  char *msg;
} last_error;

__attribute__((format(printf, 2, 3))) static void
set_error(int errcode, const char *fmt, ...) {
  va_list ap;

  free(last_error.msg);

  last_error.code = errcode;
  last_error.msg = NULL;

  if (fmt) {
    va_start(ap, fmt);
    xvasprintf(&last_error.msg, fmt, ap);
    va_end(ap);
  }
}

static bool
uc_wg_parse_u32(uc_value_t *val, uint32_t *n)
{
  uint64_t u;

  u = ucv_to_unsigned(val);

  if (errno != 0 || u > UINT32_MAX)
    return false;

  *n = (uint32_t)u;

  return true;
}

static bool
uc_wg_parse_s32(uc_value_t *val, uint32_t *n)
{
  int64_t i;

  i = ucv_to_integer(val);

  if (errno != 0 || i < INT32_MIN || i > INT32_MAX)
    return false;

  *n = (uint32_t)i;

  return true;
}

static bool
uc_wg_parse_u64(uc_value_t *val, uint64_t *n)
{
  *n = ucv_to_unsigned(val);

  return (errno == 0);
}

typedef struct {
  socklen_t len;
  union {
    struct sockaddr sa;
    struct sockaddr_in sin;
    struct sockaddr_in6 sin6;
  } u;
} uc_wg_addr_t;

// static unsigned int uc_wg_default_port = 51820;

static inline bool
uc_wg_parse_endpoint(uc_wg_addr_t *endpoint, const char *value)
{
  char *mutable = strdup(value);
  char *begin, *end;
  int ret, retries = 15;
  struct addrinfo *resolved;
  struct addrinfo hints = {
    .ai_family = AF_UNSPEC,
    .ai_socktype = SOCK_DGRAM,
    .ai_protocol = IPPROTO_UDP
  };
  if (!mutable) {
    set_error(-1, "uc_wg_parse_endpoint: strdup");
    return false;
  }
  if (!strlen(value)) {
    free(mutable);
    set_error(-1, "uc_wg_parse_endpoint: unable to parse empty endpoint");
    return false;
  }
  if (mutable[0] == '[') {
    begin = &mutable[1];
    end = strchr(mutable, ']');
    if (!end) {
      free(mutable);
      set_error(-1, "Unable to find matching brace of endpoint: `%s'", value);
      return false;
    }
    *end++ = '\0';
    if (*end++ != ':' || !*end) {
      free(mutable);
      set_error(-1, "Unable to find port of endpoint: `%s'", value);
      return false;
    }
  } else {
    begin = mutable;
    end = strrchr(mutable, ':');
    if (!end || !*(end + 1)) {
      free(mutable);
      set_error(-1, "Unable to find port of endpoint: `%s'", value);
      return false;
    }
    *end++ = '\0';
  }

  #define min(a, b) ((a) < (b) ? (a) : (b))
  for (unsigned int timeout = 1000000;; timeout = min(20000000, timeout * 6 / 5)) {
    ret = getaddrinfo(begin, end, &hints, &resolved);
    if (!ret)
      break;
    /* The set of return codes that are "permanent failures". All other possibilities are potentially transient.
     *
     * This is according to https://sourceware.org/glibc/wiki/NameResolver which states:
     *  "From the perspective of the application that calls getaddrinfo() it perhaps
     *   doesn't matter that much since EAI_FAIL, EAI_NONAME and EAI_NODATA are all
     *   permanent failure codes and the causes are all permanent failures in the
     *   sense that there is no point in retrying later."
     *
     * So this is what we do, except FreeBSD removed EAI_NODATA some time ago, so that's conditional.
     */
    if (ret == EAI_NONAME || ret == EAI_FAIL ||
      #ifdef EAI_NODATA
        ret == EAI_NODATA ||
      #endif
        (retries >= 0 && !retries--)) {
      free(mutable);
      fprintf(stderr, "%s: `%s'\n", ret == EAI_SYSTEM ? strerror(errno) : gai_strerror(ret), value);
      return false;
    }
    set_error(-1, "%s: `%s'. Trying again in %.2f seconds...", ret == EAI_SYSTEM ? strerror(errno) : gai_strerror(ret), value, timeout / 1000000.0);
    usleep(timeout);
  }

  if ((resolved->ai_family == AF_INET && resolved->ai_addrlen == sizeof(struct sockaddr_in)) ||
      (resolved->ai_family == AF_INET6 && resolved->ai_addrlen == sizeof(struct sockaddr_in6))) {
    memcpy(&endpoint->u, resolved->ai_addr, resolved->ai_addrlen);
    endpoint->len = resolved->ai_addrlen;
  } else {
    freeaddrinfo(resolved);
    free(mutable);
    set_error(-1, "Neither IPv4 nor IPv6 address found: `%s'", value);
    return false;
  }
  freeaddrinfo(resolved);
  free(mutable);
  return true;
}

static char *
uc_wg_convert_endpoint(const struct sockaddr *addr)
{
  char host[4096 + 1];
  char service[512 + 1];
  static char buf[sizeof(host) + sizeof(service) + 4];
  int ret;
  socklen_t addr_len = 0;

  memset(buf, 0, sizeof(buf));
  if (addr->sa_family == AF_INET)
    addr_len = sizeof(struct sockaddr_in);
  else if (addr->sa_family == AF_INET6)
    addr_len = sizeof(struct sockaddr_in6);

  ret = getnameinfo(addr, addr_len, host, sizeof(host), service, sizeof(service), NI_DGRAM | NI_NUMERICSERV | NI_NUMERICHOST);
  if (ret) {
    strncpy(buf, gai_strerror(ret), sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
  } else
    snprintf(buf, sizeof(buf), (addr->sa_family == AF_INET6 && strchr(host, ':')) ? "[%s]:%s" : "%s:%s", host, service);
  return buf;
}

typedef struct {
  uint8_t family;
  uint8_t mask;
  uint8_t alen;
  uint8_t bitlen;
  union {
    struct in_addr in;
    struct in6_addr in6;
  } addr;
} uc_wg_cidr_t;

static bool
uc_wg_parse_cidr(uc_vm_t *vm, uc_value_t *val, uc_wg_cidr_t *p)
{
  char *s = ucv_to_string(vm, val);
  struct in6_addr mask6 = { 0 };
  struct in_addr mask = { 0 };
  bool valid = true;
  char *m, *e;
  long n = 0;
  // size_t i;

  if (!s)
    return false;

  m = strchr(s, '/');

  if (m)
    *m++ = '\0';

  if (inet_pton(AF_INET6, s, &p->addr.in6) == 1) {
    if (m) {
      if (inet_pton(AF_INET6, m, &mask6) == 1) {
        while (n < 128 && (mask6.s6_addr[n / 8] << (n % 8)) & 128)
          n++;
      }
      else {
        n = strtol(m, &e, 10);

        if (e == m || *e || n < 0 || n > 128)
          valid = false;
      }

      p->mask = (uint8_t)n;
    }
    else {
      p->mask = 128;
    }

    p->family = AF_INET6;
    p->alen = sizeof(mask6);
    p->bitlen = p->alen * 8;
  }
  else if (strchr(s, '.') && inet_pton(AF_INET, s, &p->addr.in) == 1) {
    if (m) {
      if (inet_pton(AF_INET, m, &mask) == 1) {
        mask.s_addr = ntohl(mask.s_addr);

        while (n < 32 && (mask.s_addr << n) & 0x80000000)
          n++;
      }
      else {
        n = strtol(m, &e, 10);

        if (e == m || *e || n < 0 || n > 32)
          valid = false;
      }

      p->mask = (uint8_t)n;
    }
    else {
      p->mask = 32;
    }

    p->family = AF_INET;
    p->alen = sizeof(mask);
    p->bitlen = p->alen * 8;
  }
  else {
    if (m)
      m[-1] = '/';

    // if (mpls_pton(AF_MPLS, s, &p->addr.mpls, sizeof(p->addr.mpls)) == 1) {
    //   p->family = AF_MPLS;
    //   p->alen = 0;

    //   for (i = 0; i < ARRAY_SIZE(p->addr.mpls); i++) {
    //     p->alen += sizeof(struct mpls_label);

    //     if (ntohl(p->addr.mpls[i].entry) & MPLS_LS_S_MASK)
    //       break;
    //   }

    //   p->bitlen = p->alen * 8;
    //   p->mask = p->bitlen;
    // }
    // else {
      valid = false;
    // }
  }

  free(s);

  return valid;
}

// TODO: which types do wireguard commands need?
typedef enum {
  DT_FLAG,
  DT_BOOL,
  DT_U8,
  DT_S8,
  DT_U16,
  DT_U32,
  DT_S32,
  DT_U64,
  DT_MSECS,
  DT_STRING,
  DT_KEY,
  DT_ENDPOINT,
  DT_ANYADDR,
  DT_NETDEV,
  DT_INADDR,
  DT_LLADDR,
  DT_NESTED,
} uc_wg_attr_datatype_t;

// TODO: which type flags are needed?
enum {
  DF_NO_SET = (1 << 0),
  DF_MULTIPLE = (1 << 1),
  DF_AUTOIDX = (1 << 2),
  DF_TYPEIDX = (1 << 3),
  DF_OFFSET1 = (1 << 4),
  DF_ARRAY = (1 << 5),
  DF_BINARY = (1 << 6),
  DF_STORE_MASK = (1 << 7),
  DF_FAMILY_HINT = (1 << 8)
};

typedef struct uc_wg_attr_spec {
  size_t attr;
  const char *key;
  uc_wg_attr_datatype_t type;
  uint32_t flags;
  const void *auxdata;
} uc_wg_attr_spec_t;

typedef struct uc_wg_nested_spec {
  size_t headsize;
  size_t nattrs;
  const uc_wg_attr_spec_t attrs[];
} uc_wg_nested_spec_t;

#define SIZE(type) (void *)(uintptr_t)sizeof(struct type)
#define MEMBER(type, field) (void *)(uintptr_t)offsetof(struct type, field)

static const uc_wg_nested_spec_t wg_msg_allowedip = {
  .headsize = 0,
  .nattrs = 3, // don't forget to update
  .attrs = {
    { WGALLOWEDIP_A_FAMILY, "family", DT_U16, 0, NULL },
    { WGALLOWEDIP_A_IPADDR, "ipaddr", DT_ANYADDR, 0, NULL },
    { WGALLOWEDIP_A_CIDR_MASK, "cidrMask", DT_U8, 0, NULL },
  }
};

static const uc_wg_nested_spec_t wg_msg_peer = {
  .headsize = 0,
  .nattrs = 10, // don't forget to update
  .attrs = {
    { WGPEER_A_PUBLIC_KEY, "publicKey", DT_KEY, 0, NULL },
    { WGPEER_A_FLAGS, "flags", DT_U32, 0, NULL },
    { WGPEER_A_PRESHARED_KEY, "presharedKey", DT_KEY, 0, NULL },
    { WGPEER_A_ENDPOINT, "endpoint", DT_ENDPOINT, 0, NULL },
    { WGPEER_A_PERSISTENT_KEEPALIVE_INTERVAL, "persistentKeepaliveInterval", DT_U16, 0, NULL },
    { WGPEER_A_LAST_HANDSHAKE_TIME, "lastHandshakeTime", DT_U64, 0, NULL },
    { WGPEER_A_RX_BYTES, "rxBytes", DT_U64, 0, NULL },
    { WGPEER_A_TX_BYTES, "txBytes", DT_U64, 0, NULL },
    { WGPEER_A_ALLOWEDIPS, "allowedips", DT_NESTED, DF_MULTIPLE|DF_AUTOIDX, &wg_msg_allowedip },
    { WGPEER_A_PROTOCOL_VERSION, "protocolVersion", DT_U32, 0, NULL },
  }
};

static const uc_wg_nested_spec_t wg_msg = {
  .headsize = 0,
  .nattrs = 8, // don't forget to update
  .attrs = {
    { WGDEVICE_A_IFINDEX, "ifindex", DT_U32, 0, NULL },
    { WGDEVICE_A_IFNAME, "ifname", DT_STRING, 0, NULL },
    { WGDEVICE_A_FLAGS, "flags", DT_U32, 0, NULL },
    { WGDEVICE_A_PRIVATE_KEY, "privateKey", DT_KEY, 0, NULL },
    { WGDEVICE_A_PUBLIC_KEY, "publicKey", DT_KEY, 0, NULL },
    { WGDEVICE_A_LISTEN_PORT, "listenPort", DT_U16, 0, NULL },
    { WGDEVICE_A_FWMARK, "fwmark", DT_U32, 0, NULL },
    { WGDEVICE_A_PEERS, "peers", DT_NESTED, DF_MULTIPLE|DF_AUTOIDX, &wg_msg_peer },
  }
};

static bool
nla_check_len(struct nlattr *nla, size_t sz)
{
  return (nla && nla_len(nla) >= (ssize_t)sz);
}

static bool
nla_parse_error(const uc_wg_attr_spec_t *spec, uc_vm_t *vm, uc_value_t *v, const char *msg)
{
  char *s;

  s = ucv_to_string(vm, v);

  set_error(NLE_INVAL, "%s `%s` has invalid value `%s`: %s",
    spec->attr ? "attribute" : "field",
    spec->key,
    s,
    msg);

  free(s);

  return false;
}

static void
uc_wg_put_struct_member(char *base, const void *offset, size_t datalen, void *data)
{
  memcpy(base + (uintptr_t)offset, data, datalen);
}

static void
uc_wg_put_struct_member_u8(char *base, const void *offset, uint8_t u8)
{
  base[(uintptr_t)offset] = u8;
}

static uint8_t
uc_wg_get_struct_member_u8(char *base, const void *offset)
{
  return (uint8_t)base[(uintptr_t)offset];
}

static void
uc_wg_nla_parse(struct nlattr *tb[], int maxtype, struct nlattr *head, int len)
{
  struct nlattr *nla;
  int rem;

  memset(tb, 0, sizeof(struct nlattr *) * (maxtype + 1));

  nla_for_each_attr(nla, head, len, rem) {
    int type = nla_type(nla);

    if (type <= maxtype)
      tb[type] = nla;
  }

  if (rem > 0)
    fprintf(stderr, "netlink: %d bytes leftover after parsing attributes.\n", rem);
}


static bool
uc_wg_parse_attr(const uc_wg_attr_spec_t *spec, struct nl_msg *msg, char *base, uc_vm_t *vm, uc_value_t *val, size_t idx);

static uc_value_t *
uc_wg_convert_attr(const uc_wg_attr_spec_t *spec, struct nl_msg *msg, char *base, struct nlattr **tb, uc_vm_t *vm);

static bool
uc_wg_convert_attrs(struct nl_msg *msg, void *buf, size_t buflen, size_t headsize, const uc_wg_attr_spec_t *attrs, size_t nattrs, uc_vm_t *vm, uc_value_t *obj)
{
  struct nlattr **tb, *nla, *nla_nest;
  size_t i, type, maxattr = 0;
  uc_value_t *v, *arr;
  int rem;

  for (i = 0; i < nattrs; i++)
    if (attrs[i].attr > maxattr)
      maxattr = attrs[i].attr;

  tb = calloc(maxattr + 1, sizeof(struct nlattr *));

  if (!tb)
    return false;

  uc_wg_nla_parse(tb, maxattr, buf + headsize, buflen - headsize);

  nla_for_each_attr(nla, buf + headsize, buflen - headsize, rem) {
    type = nla_type(nla);

    if (type <= maxattr)
      tb[type] = nla;
  }

  for (i = 0; i < nattrs; i++) {
    if (attrs[i].attr != 0 && !tb[attrs[i].attr])
      continue;

    if (attrs[i].flags & DF_MULTIPLE) {
      arr = ucv_array_new(vm);
      nla_nest = tb[attrs[i].attr];

      nla_for_each_attr(nla, nla_data(nla_nest), nla_len(nla_nest), rem) {
        if (!(attrs[i].flags & (DF_AUTOIDX|DF_TYPEIDX)) &&
            attrs[i].auxdata && nla_type(nla) != (intptr_t)attrs[i].auxdata)
          continue;

        tb[attrs[i].attr] = nla;

        v = uc_wg_convert_attr(&attrs[i], msg, (char *)buf, tb, vm);

        if (!v)
          continue;

        if (attrs[i].flags & DF_TYPEIDX)
          ucv_array_set(arr, nla_type(nla) - !!(attrs[i].flags & DF_OFFSET1), v);
        else
          ucv_array_push(arr, v);
      }

      if (!ucv_array_length(arr)) {
        ucv_put(arr);

        continue;
      }

      v = arr;
    }
    else {
      v = uc_wg_convert_attr(&attrs[i], msg, (char *)buf, tb, vm);

      if (!v)
        continue;
    }

    ucv_object_add(obj, attrs[i].key, v);
  }

  free(tb);

  return true;
}

static bool
uc_wg_parse_attrs(struct nl_msg *msg, char *base, const uc_wg_attr_spec_t *attrs, size_t nattrs, uc_vm_t *vm, uc_value_t *obj)
{
  struct nlattr *nla_nest = NULL;
  uc_value_t *v, *item;
  size_t i, j, idx;
  bool exists;

  for (i = 0; i < nattrs; i++) {
    // if (attrs[i].attr == NL80211_ATTR_NOT_IMPLEMENTED)
    //   continue;

    v = ucv_object_get(obj, attrs[i].key, &exists);

    if (!exists)
      continue;

    if (attrs[i].flags & DF_MULTIPLE) {
      nla_nest = nla_nest_start(msg, attrs[i].attr);

      if (ucv_type(v) == UC_ARRAY) {
        for (j = 0; j < ucv_array_length(v); j++) {
          item = ucv_array_get(v, j);

          if (!item && (attrs[i].flags & DF_TYPEIDX))
            continue;

          if (!attrs[i].auxdata || (attrs[i].flags & (DF_AUTOIDX|DF_TYPEIDX)))
            idx = j + !!(attrs[i].flags & DF_OFFSET1);
          else
            idx = (uintptr_t)attrs[i].auxdata;

          if (!uc_wg_parse_attr(&attrs[i], msg, base, vm, item, idx))
            return false;
        }
      }
      else {
        if (!attrs[i].auxdata || (attrs[i].flags & (DF_AUTOIDX|DF_TYPEIDX)))
          idx = !!(attrs[i].flags & DF_OFFSET1);
        else
          idx = (uintptr_t)attrs[i].auxdata;

        if (!uc_wg_parse_attr(&attrs[i], msg, base, vm, v, idx))
          return false;
      }

      nla_nest_end(msg, nla_nest);
    }
    else if (!uc_wg_parse_attr(&attrs[i], msg, base, vm, v, 0)) {
      return false;
    }
  }

  return true;
}

static bool
uc_wg_parse_rta_nested(const uc_wg_attr_spec_t *spec, struct nl_msg *msg, char *base, uc_vm_t *vm, uc_value_t *val)
{
  const uc_wg_nested_spec_t *nest = spec->auxdata;
  struct nlattr *nested_nla;

  if (!nest)
    return false;

  nested_nla = nla_reserve(msg, spec->attr|NLA_F_NESTED, nest->headsize);

  if (!uc_wg_parse_attrs(msg, nla_data(nested_nla), nest->attrs, nest->nattrs, vm, val))
    return false;

  nla_nest_end(msg, nested_nla);

  return true;
}

static uc_value_t *
uc_wg_convert_rta_nested(const uc_wg_attr_spec_t *spec, struct nl_msg *msg, struct nlattr **tb, uc_vm_t *vm)
{
  const uc_wg_nested_spec_t *nest = spec->auxdata;
  uc_value_t *nested_obj;
  bool rv;

  if (!nest)
    return NULL;

  if (!nla_check_len(tb[spec->attr], nest->headsize))
    return NULL;

  nested_obj = ucv_object_new(vm);

  rv = uc_wg_convert_attrs(msg,
    nla_data(tb[spec->attr]), nla_len(tb[spec->attr]), nest->headsize,
    nest->attrs, nest->nattrs,
    vm, nested_obj);

  if (!rv) {
    ucv_put(nested_obj);

    return NULL;
  }

  return nested_obj;
}

static bool
uc_wg_parse_numval(const uc_wg_attr_spec_t *spec, struct nl_msg *msg, char *base, uc_vm_t *vm, uc_value_t *val, void *dst)
{
  uint64_t u64;
  uint32_t u32;
  uint16_t u16;
  uint8_t u8;

  switch (spec->type) {
  case DT_U8:
    if (!uc_wg_parse_u32(val, &u32) || u32 > 255)
      return nla_parse_error(spec, vm, val, "not an integer or out of range 0-255");

    u8 = (uint8_t)u32;

    memcpy(dst, &u8, sizeof(u8));
    break;

  case DT_U16:
    if (!uc_wg_parse_u32(val, &u32) || u32 > 65535)
      return nla_parse_error(spec, vm, val, "not an integer or out of range 0-65535");

    u16 = (uint16_t)u32;

    memcpy(dst, &u16, sizeof(u16));
    break;

  case DT_S32:
  case DT_U32:
    if (spec->type == DT_S32 && !uc_wg_parse_s32(val, &u32))
      return nla_parse_error(spec, vm, val, "not an integer or out of range -2147483648-2147483647");
    else if (spec->type == DT_U32 && !uc_wg_parse_u32(val, &u32))
      return nla_parse_error(spec, vm, val, "not an integer or out of range 0-4294967295");

    memcpy(dst, &u32, sizeof(u32));
    break;

  case DT_U64:
    if (!uc_wg_parse_u64(val, &u64))
      return nla_parse_error(spec, vm, val, "not an integer or negative");

    memcpy(dst, &u64, sizeof(u64));
    break;

  default:
    return false;
  }

  return true;
}

static const uint8_t dt_sizes[] = {
  [DT_U8] = sizeof(uint8_t),
  [DT_S8] = sizeof(int8_t),
  [DT_U16] = sizeof(uint16_t),
  [DT_U32] = sizeof(uint32_t),
  [DT_S32] = sizeof(int32_t),
  [DT_U64] = sizeof(uint64_t),
};

static bool
uc_wg_parse_attr(const uc_wg_attr_spec_t *spec, struct nl_msg *msg, char *base, uc_vm_t *vm, uc_value_t *val, size_t idx)
{
  char buf[sizeof(uint64_t)];
  struct nlattr *nla;
  uc_value_t *item;
  size_t attr, i;
  uint32_t u32;
  char *s;
  uc_wg_cidr_t cidr = { 0 };

  if (spec->flags & DF_MULTIPLE)
    attr = idx;
  else
    attr = spec->attr;

  switch (spec->type) {
  case DT_U8:
  case DT_U16:
  case DT_U32:
  case DT_S32:
  case DT_U64:
    if (spec->flags & DF_ARRAY) {
      assert(spec->attr != 0);

      if (ucv_type(val) != UC_ARRAY)
        return nla_parse_error(spec, vm, val, "not an array");

      nla = nla_reserve(msg, spec->attr, ucv_array_length(val) * dt_sizes[spec->type]);
      s = nla_data(nla);

      for (i = 0; i < ucv_array_length(val); i++) {
        item = ucv_array_get(val, i);

        if (!uc_wg_parse_numval(spec, msg, base, vm, item, buf))
          return false;

        memcpy(s, buf, dt_sizes[spec->type]);

        s += dt_sizes[spec->type];
      }
    }
    else {
      if (!uc_wg_parse_numval(spec, msg, base, vm, val, buf))
        return false;

      if (spec->attr == 0)
        uc_wg_put_struct_member(base, spec->auxdata, dt_sizes[spec->type], buf);
      else
        nla_put(msg, attr, dt_sizes[spec->type], buf);
    }

    break;

  case DT_BOOL:
    u32 = (uint32_t)ucv_is_truish(val);

    if (spec->attr == 0)
      uc_wg_put_struct_member_u8(base, spec->auxdata, u32);
    else
      nla_put_u8(msg, attr, u32);

    break;

  case DT_FLAG:
    u32 = (uint32_t)ucv_is_truish(val);

    if (spec->attr == 0)
      uc_wg_put_struct_member_u8(base, spec->auxdata, u32);
    else if (u32 == 1)
      nla_put_flag(msg, attr);

    break;

  case DT_KEY:
    assert(spec->attr != 0);

    char buf64[WG_KEY_LEN];
    s = ucv_to_string(vm, val);

    if (!s) {
      free(s);
      return nla_parse_error(spec, vm, val, "out of memory");
    }

    if (strlen(s) != (B64_ENCODE_LEN(WG_KEY_LEN) - 1)) {
      free(s);
      return nla_parse_error(spec, vm, val, "invalid wireguard key");
    }

    if (!b64_decode(s, &buf64, WG_KEY_LEN)) {
      free(s);
      return nla_parse_error(spec, vm, val, "invalid wireguard key base64 encoding");
    }

    nla_put(msg, attr, WG_KEY_LEN, buf64);
    free(s);

    break;

  case DT_ENDPOINT:
    assert(spec->attr != 0);

    s = ucv_to_string(vm, val);

    if (!s)
      return nla_parse_error(spec, vm, val, "out of memory");

    uc_wg_addr_t a = { };
    if (!uc_wg_parse_endpoint(&a, s)) {
      free(s);
      return nla_parse_error(spec, vm, val, last_error.msg);
    }
    free(s);

    if (nla_put(msg, attr, a.len, &a.u.sin) != 0) {
      return nla_parse_error(spec, vm, val, "nla_put failed");
    }

    break;

  // case DT_INADDR:
  // case DT_IN6ADDR:
  // case DT_MPLSADDR:
  case DT_ANYADDR:
    assert(spec->attr != 0);

    if (!uc_wg_parse_cidr(vm, val, &cidr))
      return nla_parse_error(spec, vm, val, "invalid IP address");

    // if ((spec->type == DT_INADDR && cidr.family != AF_INET) ||
    //     (spec->type == DT_IN6ADDR && cidr.family != AF_INET6) ||
    //     (spec->type == DT_MPLSADDR && cidr.family != AF_MPLS))
    //     return nla_parse_error(spec, vm, val, "wrong address family");

    if (spec->flags & DF_STORE_MASK)
      uc_wg_put_struct_member_u8(base, spec->auxdata, cidr.mask);
    else if (cidr.mask != cidr.bitlen)
      return nla_parse_error(spec, vm, val, "address range given but single address expected");

    nla_put(msg, attr, cidr.alen, &cidr.addr.in6);

    break;

  case DT_STRING:
    assert(spec->attr != 0);

    s = ucv_to_string(vm, val);

    if (!s)
      return nla_parse_error(spec, vm, val, "out of memory");

    nla_put_string(msg, attr, s);
    free(s);

    break;

  case DT_NESTED:
    if (!uc_wg_parse_rta_nested(spec, msg, base, vm, val))
      return false;

    break;

  default:
    assert(0);
  }

  return true;
}

static uc_value_t *
uc_wg_convert_numval(const uc_wg_attr_spec_t *spec, char *base)
{
  union { uint8_t *u8; uint16_t *u16; uint32_t *u32; uint64_t *u64; char *base; } t = { .base = base };

  switch (spec->type) {
  case DT_U8:
    return ucv_uint64_new(t.u8[0]);

  case DT_S8:
    return ucv_int64_new((int8_t)t.u8[0]);

  case DT_U16:
    return ucv_uint64_new(t.u16[0]);

  case DT_U32:
    return ucv_uint64_new(t.u32[0]);

  case DT_S32:
    return ucv_int64_new((int32_t)t.u32[0]);

  case DT_U64:
    return ucv_uint64_new(t.u64[0]);

  default:
    return NULL;
  }
}

static uc_value_t *
uc_wg_convert_attr(const uc_wg_attr_spec_t *spec, struct nl_msg *msg, char *base, struct nlattr **tb, uc_vm_t *vm)
{
  union { uint8_t u8; uint16_t u16; uint32_t u32; uint64_t u64; size_t sz; } t = { 0 };
  uc_value_t *v;
  int i;
  char buf[sizeof(struct sockaddr_in6) + 8];

  switch (spec->type) {
  case DT_U8:
  case DT_S8:
  case DT_U16:
  case DT_U32:
  case DT_S32:
  case DT_U64:
    if (spec->flags & DF_ARRAY) {
      assert(spec->attr != 0);
      assert((nla_len(tb[spec->attr]) % dt_sizes[spec->type]) == 0);

      v = ucv_array_new_length(vm, nla_len(tb[spec->attr]) / dt_sizes[spec->type]);

      for (i = 0; i < nla_len(tb[spec->attr]); i += dt_sizes[spec->type])
        ucv_array_push(v, uc_wg_convert_numval(spec, nla_data(tb[spec->attr]) + i));

      return v;
    }
    else if (nla_check_len(tb[spec->attr], dt_sizes[spec->type])) {
      return uc_wg_convert_numval(spec, nla_data(tb[spec->attr]));
    }

    return NULL;

  case DT_BOOL:
    if (spec->attr == 0)
      t.u8 = uc_wg_get_struct_member_u8(base, spec->auxdata);
    else if (nla_check_len(tb[spec->attr], sizeof(t.u8)))
      t.u8 = nla_get_u8(tb[spec->attr]);

    return ucv_boolean_new(t.u8 != 0);

  case DT_FLAG:
    if (spec->attr == 0)
      t.u8 = uc_wg_get_struct_member_u8(base, spec->auxdata);
    else if (tb[spec->attr] != NULL)
      t.u8 = 1;

    return ucv_boolean_new(t.u8 != 0);

  case DT_KEY:
    assert(spec->attr != 0);

    char buf64[B64_ENCODE_LEN(WG_KEY_LEN) - 1];

    if (!nla_check_len(tb[spec->attr], WG_KEY_LEN))
      return NULL;

    size_t b64len = B64_ENCODE_LEN(WG_KEY_LEN) - 1;
    if (!b64_encode(nla_data(tb[spec->attr]), WG_KEY_LEN, &buf64, b64len))
      return NULL;

    return ucv_string_new_length(buf64, b64len);

  case DT_ENDPOINT:
    assert(spec->attr != 0);

    if (!nla_check_len(tb[spec->attr], 1))
      return NULL;

    char * addrport = uc_wg_convert_endpoint(nla_data(tb[spec->attr]));
    return ucv_string_new_length(addrport, strlen(addrport));

  case DT_ANYADDR:
    assert(spec->attr != 0);

    t.sz = (size_t)nla_len(tb[spec->attr]);

    if (t.sz == sizeof(struct in6_addr) &&
        !inet_ntop(AF_INET6, nla_data(tb[spec->attr]), buf, sizeof(buf)))
      return NULL;

    if (t.sz == sizeof(struct in_addr) &&
        !inet_ntop(AF_INET, nla_data(tb[spec->attr]), buf, sizeof(buf)))
      return NULL;

    return ucv_string_new(buf);

  case DT_STRING:
    assert(spec->attr != 0);

    if (!nla_check_len(tb[spec->attr], 1))
      return NULL;

    t.sz = nla_len(tb[spec->attr]);

    if (!(spec->flags & DF_BINARY))
      t.sz -= 1;

    return ucv_string_new_length(nla_data(tb[spec->attr]), t.sz);

  case DT_NESTED:
    return uc_wg_convert_rta_nested(spec, msg, tb, vm);

  default:
    assert(0);
  }

  return NULL;
}

static struct {
  struct nl_sock *sock;
  struct nl_sock *evsock;
  struct nl_cache *cache;
  struct genl_family *nlwg;
  struct genl_family *nlctrl;
  struct nl_cb *evsock_cb;
} wg_conn;

typedef enum {
  STATE_UNREPLIED,
  STATE_CONTINUE,
  STATE_REPLIED,
  STATE_ERROR
} reply_state_t;

typedef struct {
  reply_state_t state;
  uc_vm_t *vm;
  uc_value_t *res;
  bool merge;
} request_state_t;


static uc_value_t *
uc_wg_error(uc_vm_t *vm, size_t nargs)
{
  uc_stringbuf_t *buf;
  const char *s;

  if (last_error.code == 0)
    return NULL;

  buf = ucv_stringbuf_new();

  if (last_error.code == NLE_FAILURE && last_error.msg) {
    ucv_stringbuf_addstr(buf, last_error.msg, strlen(last_error.msg));
  }
  else {
    s = nl_geterror(last_error.code);

    ucv_stringbuf_addstr(buf, s, strlen(s));

    if (last_error.msg)
      ucv_stringbuf_printf(buf, ": %s", last_error.msg);
  }

  set_error(0, NULL);

  return ucv_stringbuf_finish(buf);
}

static int
cb_done(struct nl_msg *msg, void *arg)
{
  request_state_t *s = arg;

  s->state = STATE_REPLIED;

  return NL_STOP;
}

static void
deep_merge_array(uc_value_t *dest, uc_value_t *src);

static void
deep_merge_object(uc_value_t *dest, uc_value_t *src);

static void
deep_merge_array(uc_value_t *dest, uc_value_t *src)
{
  uc_value_t *e, *v;
  size_t i;

  if (ucv_type(dest) == UC_ARRAY && ucv_type(src) == UC_ARRAY) {
    for (i = 0; i < ucv_array_length(src); i++) {
      e = ucv_array_get(dest, i);
      v = ucv_array_get(src, i);

      if (!e)
        ucv_array_set(dest, i, ucv_get(v));
      else if (ucv_type(v) == UC_ARRAY)
        deep_merge_array(e, v);
      else if (ucv_type(v) == UC_OBJECT)
        deep_merge_object(e, v);
    }
  }
}

static void
deep_merge_object(uc_value_t *dest, uc_value_t *src)
{
  uc_value_t *e;
  bool exists;

  if (ucv_type(dest) == UC_OBJECT && ucv_type(src) == UC_OBJECT) {
    ucv_object_foreach(src, k, v) {
      e = ucv_object_get(dest, k, &exists);

      if (!exists)
        ucv_object_add(dest, k, ucv_get(v));
      else if (ucv_type(v) == UC_ARRAY)
        deep_merge_array(e, v);
      else if (ucv_type(v) == UC_OBJECT)
        deep_merge_object(e, v);
    }
  }
}

static int
cb_reply(struct nl_msg *msg, void *arg)
{
  struct nlmsghdr *hdr = nlmsg_hdr(msg);
  struct genlmsghdr *gnlh = nlmsg_data(hdr);
  request_state_t *s = arg;
  uc_value_t *o;
  bool rv;

  o = ucv_object_new(s->vm);

  rv = uc_wg_convert_attrs(msg,
    genlmsg_attrdata(gnlh, 0), genlmsg_attrlen(gnlh, 0),
    0, wg_msg.attrs, wg_msg.nattrs, s->vm, o);

  if (rv) {
    if (hdr->nlmsg_flags & NLM_F_MULTI) {
      if (!s->res)
        s->res = ucv_array_new(s->vm);

      ucv_array_push(s->res, o);
    }
    else {
      s->res = o;
    }
  }
  else {
    ucv_put(o);
  }

  s->state = STATE_CONTINUE;

  return NL_SKIP;
}

static bool
uc_wg_connect_sock(struct nl_sock **sk, bool nonblocking)
{
  int err, fd;

  if (*sk)
    return true;

  *sk = nl_socket_alloc();

  if (!*sk) {
    set_error(NLE_NOMEM, NULL);
    goto err;
  }

  err = genl_connect(*sk);

  if (err != 0) {
    set_error(err, NULL);
    goto err;
  }

  fd = nl_socket_get_fd(*sk);

  if (fcntl(fd, F_SETFD, fcntl(fd, F_GETFD) | FD_CLOEXEC) < 0) {
    set_error(NLE_FAILURE, "unable to set FD_CLOEXEC flag on socket: %s", strerror(errno));
    goto err;
  }

  if (nonblocking) {
    err = nl_socket_set_nonblocking(*sk);

    if (err != 0) {
      set_error(err, NULL);
      goto err;
    }
  }

  return true;

err:
  if (*sk) {
    nl_socket_free(*sk);
    *sk = NULL;
  }

  return false;
}

static int
uc_wg_find_family_id(const char *name)
{
  struct genl_family *fam;

  if (!wg_conn.cache && genl_ctrl_alloc_cache(wg_conn.sock, &wg_conn.cache))
    return -NLE_NOMEM;

  fam = genl_ctrl_search_by_name(wg_conn.cache, name);

  if (!fam)
    return -NLE_OBJ_NOTFOUND;

  return genl_family_get_id(fam);
}

static int
cb_errno(struct sockaddr_nl *nla, struct nlmsgerr *err, void *arg)
{
  int *ret = arg;

  *ret = err->error;

  return NL_STOP;
}

static uc_value_t *
uc_wg_request(uc_vm_t *vm, size_t nargs)
{
  request_state_t st = { .vm = vm };
  uc_value_t *cmd = uc_fn_arg(0);
  uc_value_t *flags = uc_fn_arg(1);
  uc_value_t *payload = uc_fn_arg(2);
  uint16_t flagval = 0;
  struct nl_msg *msg;
  struct nl_cb *cb;
  int ret, id;

  if (ucv_type(cmd) != UC_INTEGER || ucv_int64_get(cmd) < 0 ||
      (flags != NULL && ucv_type(flags) != UC_INTEGER) ||
      (payload != NULL && ucv_type(payload) != UC_OBJECT))
    err_return(NLE_INVAL, NULL);

  if (flags) {
    if (ucv_int64_get(flags) < 0 || ucv_int64_get(flags) > 0xffff)
      err_return(NLE_INVAL, NULL);
    else
      flagval = (uint16_t)ucv_int64_get(flags);
  }

  if (!uc_wg_connect_sock(&wg_conn.sock, false))
    return NULL;

  msg = nlmsg_alloc();

  if (!msg)
    err_return(NLE_NOMEM, NULL);

  id = uc_wg_find_family_id(WG_GENL_NAME);

  if (id < 0)
    err_return(-id, NULL);

  genlmsg_put(msg, 0, 0, id, 0, flagval, ucv_int64_get(cmd), WG_GENL_VERSION);

  if (!uc_wg_parse_attrs(msg, nlmsg_data(nlmsg_hdr(msg)), wg_msg.attrs, wg_msg.nattrs, vm, payload)) {
    nlmsg_free(msg);

    return NULL;
  }

  cb = nl_cb_alloc(NL_CB_DEFAULT);

  if (!cb) {
    nlmsg_free(msg);
    err_return(NLE_NOMEM, NULL);
  }

  ret = 1;

  nl_cb_set(cb, NL_CB_VALID, NL_CB_CUSTOM, cb_reply, &st);
  nl_cb_set(cb, NL_CB_FINISH, NL_CB_CUSTOM, cb_done, &st);
  nl_cb_set(cb, NL_CB_ACK, NL_CB_CUSTOM, cb_done, &st);
  nl_cb_err(cb, NL_CB_CUSTOM, cb_errno, &ret);

  nl_send_auto_complete(wg_conn.sock, msg);

  while (ret > 0 && st.state < STATE_REPLIED)
    nl_recvmsgs(wg_conn.sock, cb);

  nlmsg_free(msg);
  nl_cb_put(cb);

  if (ret < 0)
    err_return(nl_syserr2nlerr(ret), NULL);

  switch (st.state) {
  case STATE_REPLIED:
    return st.res;

  case STATE_UNREPLIED:
    return ucv_boolean_new(true);

  default:
    set_error(NLE_FAILURE, "Interrupted reply");

    return ucv_boolean_new(false);
  }
}

static void
register_constants(uc_vm_t *vm, uc_value_t *scope)
{
  uc_value_t *c = ucv_object_new(vm);

  ucv_object_add(c, "WG_GENL_NAME", ucv_string_new_length("wireguard", 9));
  ucv_object_add(c, "WG_GENL_VERSION", ucv_uint64_new(1));

#define ADD_CONST(x) ucv_object_add(c, #x, ucv_int64_new(x))

  ADD_CONST(NLM_F_DUMP);
  ADD_CONST(NLM_F_REQUEST);
  ADD_CONST(NLM_F_ACK);

  ADD_CONST(NLA_F_NESTED);

  ADD_CONST(WG_KEY_LEN);
  ADD_CONST(WG_CMD_GET_DEVICE);
  ADD_CONST(WG_CMD_SET_DEVICE);

  ADD_CONST(WGDEVICE_F_REPLACE_PEERS);
  ADD_CONST(WGDEVICE_A_UNSPEC);
  ADD_CONST(WGDEVICE_A_IFINDEX);
  ADD_CONST(WGDEVICE_A_IFNAME);
  ADD_CONST(WGDEVICE_A_PRIVATE_KEY);
  ADD_CONST(WGDEVICE_A_PUBLIC_KEY);
  ADD_CONST(WGDEVICE_A_FLAGS);
  ADD_CONST(WGDEVICE_A_LISTEN_PORT);
  ADD_CONST(WGDEVICE_A_FWMARK);
  ADD_CONST(WGDEVICE_A_PEERS);

  ADD_CONST(WGPEER_F_REMOVE_ME);
  ADD_CONST(WGPEER_F_REPLACE_ALLOWEDIPS);
  ADD_CONST(WGPEER_F_UPDATE_ONLY);
  ADD_CONST(WGPEER_A_UNSPEC);
  ADD_CONST(WGPEER_A_PUBLIC_KEY);
  ADD_CONST(WGPEER_A_PRESHARED_KEY);
  ADD_CONST(WGPEER_A_FLAGS);
  ADD_CONST(WGPEER_A_ENDPOINT);
  ADD_CONST(WGPEER_A_PERSISTENT_KEEPALIVE_INTERVAL);
  ADD_CONST(WGPEER_A_LAST_HANDSHAKE_TIME);
  ADD_CONST(WGPEER_A_RX_BYTES);
  ADD_CONST(WGPEER_A_TX_BYTES);
  ADD_CONST(WGPEER_A_ALLOWEDIPS);
  ADD_CONST(WGPEER_A_PROTOCOL_VERSION);

  ADD_CONST(WGALLOWEDIP_A_UNSPEC);
  ADD_CONST(WGALLOWEDIP_A_FAMILY);
  ADD_CONST(WGALLOWEDIP_A_IPADDR);
  ADD_CONST(WGALLOWEDIP_A_CIDR_MASK);

  ucv_object_add(scope, "const", c);
};

static const uc_function_list_t global_fns[] = {
  { "error",    uc_wg_error },
  { "request",  uc_wg_request },
};


void uc_module_init(uc_vm_t *vm, uc_value_t *scope)
{
  uc_function_list_register(scope, global_fns);

  register_constants(vm, scope);
}

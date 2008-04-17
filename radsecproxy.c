/*
 * Copyright (C) 2006-2008 Stig Venaas <venaas@uninett.no>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 */

/* For UDP there is one server instance consisting of udpserverrd and udpserverth
 *              rd is responsible for init and launching wr
 * For TLS there is a server instance that launches tlsserverrd for each TLS peer
 *          each tlsserverrd launches tlsserverwr
 * For each UDP/TLS peer there is clientrd and clientwr, clientwr is responsible
 *          for init and launching rd
 *
 * serverrd will receive a request, processes it and puts it in the requestq of
 *          the appropriate clientwr
 * clientwr monitors its requestq and sends requests
 * clientrd looks for responses, processes them and puts them in the replyq of
 *          the peer the request came from
 * serverwr monitors its reply and sends replies
 *
 * In addition to the main thread, we have:
 * If UDP peers are configured, there will be 2 + 2 * #peers UDP threads
 * If TLS peers are configured, there will initially be 2 * #peers TLS threads
 * For each TLS peer connecting to us there will be 2 more TLS threads
 *       This is only for connected peers
 * Example: With 3 UDP peer and 30 TLS peers, there will be a max of
 *          1 + (2 + 2 * 3) + (2 * 30) + (2 * 30) = 129 threads
*/

#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <string.h>
#include <unistd.h>
#ifdef SYS_SOLARIS9
#include <fcntl.h>
#endif
#include <sys/time.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include <regex.h>
#include <libgen.h>
#include <pthread.h>
#include <openssl/ssl.h>
#include <openssl/rand.h>
#include <openssl/err.h>
#include <openssl/md5.h>
#include <openssl/hmac.h>
#include <openssl/x509v3.h>
#include "debug.h"
#include "list.h"
#include "util.h"
#include "gconfig.h"
#include "radsecproxy.h"

static struct options options;
struct list *clconfs, *srvconfs, *realms, *tlsconfs, *rewriteconfs;

static int client_udp_count = 0;
static int client_tls_count = 0;
static int server_udp_count = 0;
static int server_tls_count = 0;

static struct clsrvconf *srcudpres = NULL;
static struct clsrvconf *srctcpres = NULL;

static struct replyq *udp_server_replyq = NULL;
static int udp_server_sock = -1;
static int udp_accserver_sock = -1;
static int udp_client4_sock = -1;
static int udp_client6_sock = -1;
static pthread_mutex_t *ssl_locks;
static long *ssl_lock_count;
extern int optind;
extern char *optarg;

/* callbacks for making OpenSSL thread safe */
unsigned long ssl_thread_id() {
        return (unsigned long)pthread_self();
}

void ssl_locking_callback(int mode, int type, const char *file, int line) {
    if (mode & CRYPTO_LOCK) {
	pthread_mutex_lock(&ssl_locks[type]);
	ssl_lock_count[type]++;
    } else
	pthread_mutex_unlock(&ssl_locks[type]);
}

static int pem_passwd_cb(char *buf, int size, int rwflag, void *userdata) {
    int pwdlen = strlen(userdata);
    if (rwflag != 0 || pwdlen > size) /* not for decryption or too large */
	return 0;
    memcpy(buf, userdata, pwdlen);
    return pwdlen;
}

static int verify_cb(int ok, X509_STORE_CTX *ctx) {
  char buf[256];
  X509 *err_cert;
  int err, depth;

  err_cert = X509_STORE_CTX_get_current_cert(ctx);
  err = X509_STORE_CTX_get_error(ctx);
  depth = X509_STORE_CTX_get_error_depth(ctx);

  if (depth > MAX_CERT_DEPTH) {
      ok = 0;
      err = X509_V_ERR_CERT_CHAIN_TOO_LONG;
      X509_STORE_CTX_set_error(ctx, err);
  }

  if (!ok) {
      X509_NAME_oneline(X509_get_subject_name(err_cert), buf, 256);
      debug(DBG_WARN, "verify error: num=%d:%s:depth=%d:%s", err, X509_verify_cert_error_string(err), depth, buf);

      switch (err) {
      case X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT:
	  X509_NAME_oneline(X509_get_issuer_name(ctx->current_cert), buf, 256);
	  debug(DBG_WARN, "\tIssuer=%s", buf);
	  break;
      case X509_V_ERR_CERT_NOT_YET_VALID:
      case X509_V_ERR_ERROR_IN_CERT_NOT_BEFORE_FIELD:
	  debug(DBG_WARN, "\tCertificate not yet valid");
	  break;
      case X509_V_ERR_CERT_HAS_EXPIRED:
	  debug(DBG_WARN, "Certificate has expired");
	  break;
      case X509_V_ERR_ERROR_IN_CERT_NOT_AFTER_FIELD:
	  debug(DBG_WARN, "Certificate no longer valid (after notAfter)");
	  break;
      }
  }
#ifdef DEBUG  
  printf("certificate verify returns %d\n", ok);
#endif  
  return ok;
}

int resolvepeer(struct clsrvconf *conf, int ai_flags) {
    struct addrinfo hints, *addrinfo, *res;
    char *slash, *s;
    int plen;

    slash = conf->host ? strchr(conf->host, '/') : NULL;
    if (slash) {
	s = slash + 1;
	if (!*s) {
	    debug(DBG_WARN, "resolvepeer: prefix length must be specified after the / in %s", conf->host);
	    return 0;
	}
	for (; *s; s++)
	    if (*s < '0' || *s > '9') {
		debug(DBG_WARN, "resolvepeer: %s in %s is not a valid prefix length", slash + 1, conf->host);
		return 0;
	    }
	plen = atoi(slash + 1);
	if (plen < 0 || plen > 128) {
	    debug(DBG_WARN, "resolvepeer: %s in %s is not a valid prefix length", slash + 1, conf->host);
	    return 0;
	}
	*slash = '\0';
    }
    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = (conf->type == 'T' ? SOCK_STREAM : SOCK_DGRAM);
    hints.ai_family = AF_UNSPEC;
    hints.ai_flags = ai_flags;
    if (!conf->host && !conf->port) {
	/* getaddrinfo() doesn't like host and port to be NULL */
	if (getaddrinfo(conf->host, DEFAULT_UDP_PORT, &hints, &addrinfo)) {
	    debug(DBG_WARN, "resolvepeer: can't resolve (null) port (null)");
	    return 0;
	}
	for (res = addrinfo; res; res = res->ai_next) {
	    switch (res->ai_family) {
	    case AF_INET:
		((struct sockaddr_in *)res->ai_addr)->sin_port = 0;
		break;
	    case AF_INET6:
		((struct sockaddr_in6 *)res->ai_addr)->sin6_port = 0;
		break;
	    }
	}
    } else {
	if (slash)
	    hints.ai_flags |= AI_NUMERICHOST;
	if (getaddrinfo(conf->host, conf->port, &hints, &addrinfo)) {
	    debug(DBG_WARN, "resolvepeer: can't resolve %s port %s", conf->host ? conf->host : "(null)", conf->port ? conf->port : "(null)");
	    return 0;
	}
	if (slash) {
	    *slash = '/';
	    switch (addrinfo->ai_family) {
	    case AF_INET:
		if (plen > 32) {
		    debug(DBG_WARN, "resolvepeer: prefix length must be <= 32 in %s", conf->host);
		    freeaddrinfo(addrinfo);
		    return 0;
		}
		break;
	    case AF_INET6:
		break;
	    default:
		debug(DBG_WARN, "resolvepeer: prefix must be IPv4 or IPv6 in %s", conf->host);
		freeaddrinfo(addrinfo);
		return 0;
	    }
	    conf->prefixlen = plen;
	} else
	    conf->prefixlen = 255;
    }
    if (conf->addrinfo)
	freeaddrinfo(conf->addrinfo);
    conf->addrinfo = addrinfo;
    return 1;
}	  

int bindtoaddr(struct addrinfo *addrinfo, int family, int reuse, int v6only) {
    int s, on = 1;
    struct addrinfo *res;
    
    for (res = addrinfo; res; res = res->ai_next) {
	if (family != AF_UNSPEC && family != res->ai_family)
	    continue;
        s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (s < 0) {
            debug(DBG_WARN, "bindtoaddr: socket failed");
            continue;
        }
	if (reuse)
	    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
#ifdef IPV6_V6ONLY
	if (v6only)
	    setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, &on, sizeof(on));
#endif		

	if (!bind(s, res->ai_addr, res->ai_addrlen))
	    return s;
	debug(DBG_WARN, "bindtoaddr: bind failed");
        close(s);
    }
    return -1;
}	  

char *parsehostport(char *s, struct clsrvconf *conf, char *default_port) {
    char *p, *field;
    int ipv6 = 0;

    p = s;
    /* allow literal addresses and port, e.g. [2001:db8::1]:1812 */
    if (*p == '[') {
	p++;
	field = p;
	for (; *p && *p != ']' && *p != ' ' && *p != '\t' && *p != '\n'; p++);
	if (*p != ']')
	    debugx(1, DBG_ERR, "no ] matching initial [");
	ipv6 = 1;
    } else {
	field = p;
	for (; *p && *p != ':' && *p != ' ' && *p != '\t' && *p != '\n'; p++);
    }
    if (field == p)
	debugx(1, DBG_ERR, "missing host/address");

    conf->host = stringcopy(field, p - field);
    if (ipv6) {
	p++;
	if (*p && *p != ':' && *p != ' ' && *p != '\t' && *p != '\n')
	    debugx(1, DBG_ERR, "unexpected character after ]");
    }
    if (*p == ':') {
	    /* port number or service name is specified */;
	    field = ++p;
	    for (; *p && *p != ' ' && *p != '\t' && *p != '\n'; p++);
	    if (field == p)
		debugx(1, DBG_ERR, "syntax error, : but no following port");
	    conf->port = stringcopy(field, p - field);
    } else
	conf->port = default_port ? stringcopy(default_port, 0) : NULL;
    return p;
}

struct clsrvconf *resolve_hostport(char type, char *lconf, char *default_port) {
    struct clsrvconf *conf;

    conf = malloc(sizeof(struct clsrvconf));
    if (!conf)
	debugx(1, DBG_ERR, "malloc failed");
    memset(conf, 0, sizeof(struct clsrvconf));
    conf->type = type;
    if (lconf) {
	parsehostport(lconf, conf, default_port);
	if (!strcmp(conf->host, "*")) {
	    free(conf->host);
	    conf->host = NULL;
	}
    } else
	conf->port = default_port ? stringcopy(default_port, 0) : NULL;
    if (!resolvepeer(conf, AI_PASSIVE))
	debugx(1, DBG_ERR, "failed to resolve host %s port %s, exiting", conf->host ? conf->host : "(null)", conf->port ? conf->port : "(null)");
    return conf;
}

int connecttcp(struct addrinfo *addrinfo) {
    int s;
    struct addrinfo *res;

    s = -1;
    for (res = addrinfo; res; res = res->ai_next) {
	s = bindtoaddr(srctcpres->addrinfo, res->ai_family, 1, 1);
        if (s < 0) {
            debug(DBG_WARN, "connecttoserver: socket failed");
            continue;
        }
        if (connect(s, res->ai_addr, res->ai_addrlen) == 0)
            break;
        debug(DBG_WARN, "connecttoserver: connect failed");
        close(s);
        s = -1;
    }
    return s;
}	  

/* returns 1 if the len first bits are equal, else 0 */
int prefixmatch(void *a1, void *a2, uint8_t len) {
    static uint8_t mask[] = { 0, 0x80, 0xc0, 0xe0, 0xf0, 0xf8, 0xfc, 0xfe };
    int r, l = len / 8;
    if (l && memcmp(a1, a2, l))
	return 0;
    r = len % 8;
    if (!r)
	return 1;
    return (((uint8_t *)a1)[l] & mask[r]) == (((uint8_t *)a2)[l] & mask[r]);
}

/* check if conf has matching address */
struct clsrvconf *checkconfaddr(char type, struct sockaddr *addr, struct clsrvconf *conf) {
    struct sockaddr_in6 *sa6 = NULL;
    struct in_addr *a4 = NULL;
    struct addrinfo *res;
    
    if (addr->sa_family == AF_INET6) {
        sa6 = (struct sockaddr_in6 *)addr;
        if (IN6_IS_ADDR_V4MAPPED(&sa6->sin6_addr)) {
            a4 = (struct in_addr *)&sa6->sin6_addr.s6_addr[12];
	    sa6 = NULL;
	}
    } else
	a4 = &((struct sockaddr_in *)addr)->sin_addr;

    if (conf->type == type) {
	if (conf->prefixlen == 255) {
	    for (res = conf->addrinfo; res; res = res->ai_next)
		if ((a4 && res->ai_family == AF_INET &&
		     !memcmp(a4, &((struct sockaddr_in *)res->ai_addr)->sin_addr, 4)) ||
		    (sa6 && res->ai_family == AF_INET6 &&
		     !memcmp(&sa6->sin6_addr, &((struct sockaddr_in6 *)res->ai_addr)->sin6_addr, 16)))
		    return conf;
	} else {
	    res = conf->addrinfo;
	    if (res &&
		((a4 && res->ai_family == AF_INET &&
		  prefixmatch(a4, &((struct sockaddr_in *)res->ai_addr)->sin_addr, conf->prefixlen)) ||
		 (sa6 && res->ai_family == AF_INET6 &&
		  prefixmatch(&sa6->sin6_addr, &((struct sockaddr_in6 *)res->ai_addr)->sin6_addr, conf->prefixlen))))
		return conf;
	}
    }
    return NULL;
}

/* returns next config with matching address, or NULL */
struct clsrvconf *find_conf(char type, struct sockaddr *addr, struct list *confs, struct list_node **cur) {
    struct sockaddr_in6 *sa6 = NULL;
    struct in_addr *a4 = NULL;
    struct addrinfo *res;
    struct list_node *entry;
    struct clsrvconf *conf;
    
    if (addr->sa_family == AF_INET6) {
        sa6 = (struct sockaddr_in6 *)addr;
        if (IN6_IS_ADDR_V4MAPPED(&sa6->sin6_addr)) {
            a4 = (struct in_addr *)&sa6->sin6_addr.s6_addr[12];
	    sa6 = NULL;
	}
    } else
	a4 = &((struct sockaddr_in *)addr)->sin_addr;

    for (entry = (cur ? list_next(*cur) : list_first(confs)); entry; entry = list_next(entry)) {
	conf = (struct clsrvconf *)entry->data;
	if (conf->type == type) {
	    if (conf->prefixlen == 255) {
		for (res = conf->addrinfo; res; res = res->ai_next)
		    if ((a4 && res->ai_family == AF_INET &&
			 !memcmp(a4, &((struct sockaddr_in *)res->ai_addr)->sin_addr, 4)) ||
			(sa6 && res->ai_family == AF_INET6 &&
			 !memcmp(&sa6->sin6_addr, &((struct sockaddr_in6 *)res->ai_addr)->sin6_addr, 16))) {
			if (cur)
			    *cur = entry;
			return conf;
		    }
	    } else {
		res = conf->addrinfo;
		if (res &&
		    ((a4 && res->ai_family == AF_INET &&
		      prefixmatch(a4, &((struct sockaddr_in *)res->ai_addr)->sin_addr, conf->prefixlen)) ||
		     (sa6 && res->ai_family == AF_INET6 &&
		      prefixmatch(&sa6->sin6_addr, &((struct sockaddr_in6 *)res->ai_addr)->sin6_addr, conf->prefixlen)))) {
		    if (cur)
			*cur = entry;
		    return conf;
		}
	    }
	}
    }    
    return NULL;
}

struct replyq *newreplyq() {
    struct replyq *replyq;
    
    replyq = malloc(sizeof(struct replyq));
    if (!replyq)
	debugx(1, DBG_ERR, "malloc failed");
    replyq->replies = list_create();
    if (!replyq->replies)
	debugx(1, DBG_ERR, "malloc failed");
    pthread_mutex_init(&replyq->mutex, NULL);
    pthread_cond_init(&replyq->cond, NULL);
    return replyq;
}

struct client *addclient(struct clsrvconf *conf) {
    struct client *new = malloc(sizeof(struct client));
    
    if (!new) {
	debug(DBG_ERR, "malloc failed");
	return NULL;
    }
    if (!conf->clients) {
	conf->clients = list_create();
	if (!conf->clients) {
	    debug(DBG_ERR, "malloc failed");
	    return NULL;
	}
    }
    
    memset(new, 0, sizeof(struct client));
    new->conf = conf;
    new->replyq = conf->type == 'T' ? newreplyq() : udp_server_replyq;

    list_push(conf->clients, new);
    return new;
}

void removeclient(struct client *client) {
    struct list_node *entry;
    
    if (!client || !client->conf->clients)
	return;

    pthread_mutex_lock(&client->replyq->mutex);
    for (entry = list_first(client->replyq->replies); entry; entry = list_next(entry))
	free(((struct reply *)entry)->buf);
    list_destroy(client->replyq->replies);
    pthread_mutex_unlock(&client->replyq->mutex);
    list_removedata(client->conf->clients, client);
    free(client);
}

void removeclientrqs(struct client *client) {
    struct list_node *entry;
    struct server *server;
    struct request *rq;
    int i;
    
    for (entry = list_first(srvconfs); entry; entry = list_next(entry)) {
	server = ((struct clsrvconf *)entry->data)->servers;
	pthread_mutex_lock(&server->newrq_mutex);
	for (i = 0; i < MAX_REQUESTS; i++) {
	    rq = server->requests + i;
	    if (rq->from == client)
		rq->from = NULL;
	}
	pthread_mutex_unlock(&server->newrq_mutex);
    }
}
		     
void addserver(struct clsrvconf *conf) {
    if (conf->servers)
	debugx(1, DBG_ERR, "addserver: currently works with just one server per conf");
    
    conf->servers = malloc(sizeof(struct server));
    if (!conf->servers)
	debugx(1, DBG_ERR, "malloc failed");
    memset(conf->servers, 0, sizeof(struct server));
    conf->servers->conf = conf;

    if (conf->type == 'U') {
	if (!srcudpres)
	    srcudpres = resolve_hostport('U', options.sourceudp, NULL);
	switch (conf->addrinfo->ai_family) {
	case AF_INET:
	    if (udp_client4_sock < 0) {
		udp_client4_sock = bindtoaddr(srcudpres->addrinfo, AF_INET, 0, 1);
		if (udp_client4_sock < 0)
		    debugx(1, DBG_ERR, "addserver: failed to create client socket for server %s", conf->host);
	    }
	    conf->servers->sock = udp_client4_sock;
	    break;
	case AF_INET6:
	    if (udp_client6_sock < 0) {
		udp_client6_sock = bindtoaddr(srcudpres->addrinfo, AF_INET6, 0, 1);
		if (udp_client6_sock < 0)
		    debugx(1, DBG_ERR, "addserver: failed to create client socket for server %s", conf->host);
	    }
	    conf->servers->sock = udp_client6_sock;
	    break;
	default:
	    debugx(1, DBG_ERR, "addserver: unsupported address family");
	}
	
    } else {
	if (!srctcpres)
	    srctcpres = resolve_hostport('T', options.sourcetcp, NULL);
	conf->servers->sock = -1;
    }
    
    pthread_mutex_init(&conf->servers->lock, NULL);
    conf->servers->requests = calloc(MAX_REQUESTS, sizeof(struct request));
    if (!conf->servers->requests)
	debugx(1, DBG_ERR, "malloc failed");
    conf->servers->newrq = 0;
    pthread_mutex_init(&conf->servers->newrq_mutex, NULL);
    pthread_cond_init(&conf->servers->newrq_cond, NULL);
}

/* exactly one of client and server must be non-NULL */
/* should probably take peer list (client(s) or server(s)) as argument instead */
/* if *peer == NULL we return who we received from, else require it to be from peer */
/* return from in sa if not NULL */
unsigned char *radudpget(int s, struct client **client, struct server **server, struct sockaddr_storage *sa) {
    int cnt, len;
    unsigned char buf[65536], *rad;
    struct sockaddr_storage from;
    socklen_t fromlen = sizeof(from);
    struct clsrvconf *p;
    struct list_node *node;
    
    for (;;) {
	cnt = recvfrom(s, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fromlen);
	if (cnt == -1) {
	    debug(DBG_WARN, "radudpget: recv failed");
	    continue;
	}
	debug(DBG_DBG, "radudpget: got %d bytes from %s", cnt, addr2string((struct sockaddr *)&from, fromlen));

	if (cnt < 20) {
	    debug(DBG_WARN, "radudpget: packet too small");
	    continue;
	}
    
	len = RADLEN(buf);
	if (len < 20) {
	    debug(DBG_WARN, "radudpget: length too small");
	    continue;
	}

	if (cnt < len) {
	    debug(DBG_WARN, "radudpget: packet smaller than length field in radius header");
	    continue;
	}
	if (cnt > len)
	    debug(DBG_DBG, "radudpget: packet was padded with %d bytes", cnt - len);

	if (client)
	    if (*client)
		p = checkconfaddr('U', (struct sockaddr *)&from, (*client)->conf);
	    else
		p = find_conf('U', (struct sockaddr *)&from, clconfs, NULL);
	else
	    if (*server)
		p = checkconfaddr('U', (struct sockaddr *)&from, (*server)->conf);
	    else
		p = find_conf('U', (struct sockaddr *)&from, srvconfs, NULL);

	if (!p) {
	    debug(DBG_WARN, "radudpget: got packet from wrong or unknown UDP peer, ignoring");
	    continue;
	}
	
	rad = malloc(len);
	if (!rad) {
	    debug(DBG_ERR, "radudpget: malloc failed");
	    continue;
	}
	
	if (client && !*client) {
	    node = list_first(p->clients);
	    *client = node ? (struct client *)node->data : addclient(p);
	    if (!*client) {
		free(rad);
		continue;
	    }
	} else if (server && !*server)
	    *server = p->servers;
	
	break;
    }
    memcpy(rad, buf, len);
    if (sa)
	*sa = from;
    return rad;
}

int subjectaltnameaddr(X509 *cert, int family, struct in6_addr *addr) {
    int loc, i, l, n, r = 0;
    char *v;
    X509_EXTENSION *ex;
    STACK_OF(GENERAL_NAME) *alt;
    GENERAL_NAME *gn;
    
    debug(DBG_DBG, "subjectaltnameaddr");
    
    loc = X509_get_ext_by_NID(cert, NID_subject_alt_name, -1);
    if (loc < 0)
	return r;
    
    ex = X509_get_ext(cert, loc);
    alt = X509V3_EXT_d2i(ex);
    if (!alt)
	return r;
    
    n = sk_GENERAL_NAME_num(alt);
    for (i = 0; i < n; i++) {
	gn = sk_GENERAL_NAME_value(alt, i);
	if (gn->type != GEN_IPADD)
	    continue;
	r = -1;
	v = (char *)ASN1_STRING_data(gn->d.ia5);
	l = ASN1_STRING_length(gn->d.ia5);
	if (((family == AF_INET && l == sizeof(struct in_addr)) || (family == AF_INET6 && l == sizeof(struct in6_addr)))
	    && !memcmp(v, &addr, l)) {
	    r = 1;
	    break;
	}
    }
    GENERAL_NAMES_free(alt);
    return r;
}

int cnregexp(X509 *cert, char *exact, regex_t *regex) {
    int loc, l;
    char *v, *s;
    X509_NAME *nm;
    X509_NAME_ENTRY *e;
    ASN1_STRING *t;

    nm = X509_get_subject_name(cert);
    loc = -1;
    for (;;) {
	loc = X509_NAME_get_index_by_NID(nm, NID_commonName, loc);
	if (loc == -1)
	    break;
	e = X509_NAME_get_entry(nm, loc);
	t = X509_NAME_ENTRY_get_data(e);
	v = (char *) ASN1_STRING_data(t);
	l = ASN1_STRING_length(t);
	if (l < 0)
	    continue;
	if (exact) {
	    if (l == strlen(exact) && !strncasecmp(exact, v, l))
		return 1;
	} else {
	    s = stringcopy((char *)v, l);
	    if (!s) {
		debug(DBG_ERR, "malloc failed");
		continue;
	    }
	    if (regexec(regex, s, 0, NULL, 0)) {
		free(s);
		continue;
	    }
	    free(s);
	    return 1;
	}
    }
    return 0;
}

int subjectaltnameregexp(X509 *cert, int type, char *exact,  regex_t *regex) {
    int loc, i, l, n, r = 0;
    char *s, *v;
    X509_EXTENSION *ex;
    STACK_OF(GENERAL_NAME) *alt;
    GENERAL_NAME *gn;
    
    debug(DBG_DBG, "subjectaltnameregexp");
    
    loc = X509_get_ext_by_NID(cert, NID_subject_alt_name, -1);
    if (loc < 0)
	return r;
    
    ex = X509_get_ext(cert, loc);
    alt = X509V3_EXT_d2i(ex);
    if (!alt)
	return r;
    
    n = sk_GENERAL_NAME_num(alt);
    for (i = 0; i < n; i++) {
	gn = sk_GENERAL_NAME_value(alt, i);
	if (gn->type != type)
	    continue;
	r = -1;
	v = (char *)ASN1_STRING_data(gn->d.ia5);
	l = ASN1_STRING_length(gn->d.ia5);
	if (l <= 0)
	    continue;
#ifdef DEBUG
	printfchars(NULL, gn->type == GEN_DNS ? "dns" : "uri", NULL, v, l);
#endif	
	if (exact) {
	    if (memcmp(v, exact, l))
		continue;
	} else {
	    s = stringcopy((char *)v, l);
	    if (!s) {
		debug(DBG_ERR, "malloc failed");
		continue;
	    }
	    if (regexec(regex, s, 0, NULL, 0)) {
		free(s);
		continue;
	    }
	    free(s);
	}
	r = 1;
	break;
    }
    GENERAL_NAMES_free(alt);
    return r;
}

X509 *verifytlscert(SSL *ssl) {
    X509 *cert;
    unsigned long error;
    
    if (SSL_get_verify_result(ssl) != X509_V_OK) {
	debug(DBG_ERR, "verifytlscert: basic validation failed");
	while ((error = ERR_get_error()))
	    debug(DBG_ERR, "verifytlscert: TLS: %s", ERR_error_string(error, NULL));
	return NULL;
    }

    cert = SSL_get_peer_certificate(ssl);
    if (!cert)
	debug(DBG_ERR, "verifytlscert: failed to obtain certificate");
    return cert;
}
    
int verifyconfcert(X509 *cert, struct clsrvconf *conf) {
    int r;
    uint8_t type = 0; /* 0 for DNS, AF_INET for IPv4, AF_INET6 for IPv6 */
    struct in6_addr addr;
    
    if (conf->prefixlen == 255) {
	if (inet_pton(AF_INET, conf->host, &addr))
	    type = AF_INET;
	else if (inet_pton(AF_INET6, conf->host, &addr))
	    type = AF_INET6;

	r = type ? subjectaltnameaddr(cert, type, &addr) : subjectaltnameregexp(cert, GEN_DNS, conf->host, NULL);
	if (r) {
	    if (r < 0) {
		debug(DBG_DBG, "verifyconfcert: No subjectaltname matching %s %s", type ? "address" : "host", conf->host);
		return 0;
	    }
	    debug(DBG_DBG, "verifyconfcert: Found subjectaltname matching %s %s", type ? "address" : "host", conf->host);
	} else {
	    if (!cnregexp(cert, conf->host, NULL)) {
		debug(DBG_ERR, "verifyconfcert: cn not matching host %s", conf->host);
		return 0;
	    }		
	    debug(DBG_DBG, "verifyconfcert: Found cn matching host %s", conf->host);
	}
    }
    if (conf->certcnregex) {
	if (cnregexp(cert, NULL, conf->certcnregex) < 1) {
	    debug(DBG_DBG, "verifyconfcert: CN not matching regex");
	    return 0;
	}
	debug(DBG_DBG, "verifyconfcert: CN matching regex");
    }
    if (conf->certuriregex) {
	if (subjectaltnameregexp(cert, GEN_URI, NULL, conf->certuriregex) < 1) {
	    debug(DBG_DBG, "verifyconfcert: subjectaltname URI not matching regex");
	    return 0;
	}
	debug(DBG_DBG, "verifyconfcert: subjectaltname URI matching regex");
    }
    return 1;
}

void tlsconnect(struct server *server, struct timeval *when, char *text) {
    struct timeval now;
    time_t elapsed;
    X509 *cert;
    
    debug(DBG_DBG, "tlsconnect called from %s", text);
    pthread_mutex_lock(&server->lock);
    if (when && memcmp(&server->lastconnecttry, when, sizeof(struct timeval))) {
	/* already reconnected, nothing to do */
	debug(DBG_DBG, "tlsconnect(%s): seems already reconnected", text);
	pthread_mutex_unlock(&server->lock);
	return;
    }

    debug(DBG_DBG, "tlsconnect %s", text);

    for (;;) {
	gettimeofday(&now, NULL);
	elapsed = now.tv_sec - server->lastconnecttry.tv_sec;
	if (server->connectionok) {
	    server->connectionok = 0;
	    sleep(2);
	} else if (elapsed < 1)
	    sleep(2);
	else if (elapsed < 60) {
	    debug(DBG_INFO, "tlsconnect: sleeping %lds", elapsed);
	    sleep(elapsed);
	} else if (elapsed < 100000) {
	    debug(DBG_INFO, "tlsconnect: sleeping %ds", 60);
	    sleep(60);
	} else
	    server->lastconnecttry.tv_sec = now.tv_sec;  /* no sleep at startup */
	debug(DBG_WARN, "tlsconnect: trying to open TLS connection to %s port %s", server->conf->host, server->conf->port);
	if (server->sock >= 0)
	    close(server->sock);
	if ((server->sock = connecttcp(server->conf->addrinfo)) < 0) {
	    debug(DBG_ERR, "tlsconnect: connecttcp failed");
	    continue;
	}
	
	SSL_free(server->ssl);
	server->ssl = SSL_new(server->conf->ssl_ctx);
	SSL_set_fd(server->ssl, server->sock);
	if (SSL_connect(server->ssl) <= 0)
	    continue;
	cert = verifytlscert(server->ssl);
	if (!cert)
	    continue;
	if (verifyconfcert(cert, server->conf)) {
	    X509_free(cert);
	    break;
	}
	X509_free(cert);
    }
    debug(DBG_WARN, "tlsconnect: TLS connection to %s port %s up", server->conf->host, server->conf->port);
    gettimeofday(&server->lastconnecttry, NULL);
    pthread_mutex_unlock(&server->lock);
}

unsigned char *radtlsget(SSL *ssl) {
    int cnt, total, len;
    unsigned char buf[4], *rad;

    for (;;) {
	for (total = 0; total < 4; total += cnt) {
	    cnt = SSL_read(ssl, buf + total, 4 - total);
	    if (cnt <= 0) {
		debug(DBG_ERR, "radtlsget: connection lost");
		if (SSL_get_error(ssl, cnt) == SSL_ERROR_ZERO_RETURN) {
		    /* remote end sent close_notify, send one back */
		    SSL_shutdown(ssl);
		}
		return NULL;
	    }
	}

	len = RADLEN(buf);
	rad = malloc(len);
	if (!rad) {
	    debug(DBG_ERR, "radtlsget: malloc failed");
	    continue;
	}
	memcpy(rad, buf, 4);

	for (; total < len; total += cnt) {
	    cnt = SSL_read(ssl, rad + total, len - total);
	    if (cnt <= 0) {
		debug(DBG_ERR, "radtlsget: connection lost");
		if (SSL_get_error(ssl, cnt) == SSL_ERROR_ZERO_RETURN) {
		    /* remote end sent close_notify, send one back */
		    SSL_shutdown(ssl);
		}
		free(rad);
		return NULL;
	    }
	}
    
	if (total >= 20)
	    break;
	
	free(rad);
	debug(DBG_WARN, "radtlsget: packet smaller than minimum radius size");
    }
    
    debug(DBG_DBG, "radtlsget: got %d bytes", total);
    return rad;
}

int clientradput(struct server *server, unsigned char *rad) {
    int cnt;
    size_t len;
    unsigned long error;
    struct timeval lastconnecttry;
    struct clsrvconf *conf = server->conf;
    
    len = RADLEN(rad);
    if (conf->type == 'U') {
	if (sendto(server->sock, rad, len, 0, conf->addrinfo->ai_addr, conf->addrinfo->ai_addrlen) >= 0) {
	    debug(DBG_DBG, "clienradput: sent UDP of length %d to %s port %s", len, conf->host, conf->port);
	    return 1;
	}
	debug(DBG_WARN, "clientradput: send failed");
	return 0;
    }

    lastconnecttry = server->lastconnecttry;
    while ((cnt = SSL_write(server->ssl, rad, len)) <= 0) {
	while ((error = ERR_get_error()))
	    debug(DBG_ERR, "clientradput: TLS: %s", ERR_error_string(error, NULL));
	tlsconnect(server, &lastconnecttry, "clientradput");
	lastconnecttry = server->lastconnecttry;
    }

    server->connectionok = 1;
    debug(DBG_DBG, "clientradput: Sent %d bytes, Radius packet of length %d to TLS peer %s", cnt, len, conf->host);
    return 1;
}

int radsign(unsigned char *rad, unsigned char *sec) {
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    static unsigned char first = 1;
    static EVP_MD_CTX mdctx;
    unsigned int md_len;
    int result;
    
    pthread_mutex_lock(&lock);
    if (first) {
	EVP_MD_CTX_init(&mdctx);
	first = 0;
    }

    result = (EVP_DigestInit_ex(&mdctx, EVP_md5(), NULL) &&
	EVP_DigestUpdate(&mdctx, rad, RADLEN(rad)) &&
	EVP_DigestUpdate(&mdctx, sec, strlen((char *)sec)) &&
	EVP_DigestFinal_ex(&mdctx, rad + 4, &md_len) &&
	md_len == 16);
    pthread_mutex_unlock(&lock);
    return result;
}

int validauth(unsigned char *rad, unsigned char *reqauth, unsigned char *sec) {
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    static unsigned char first = 1;
    static EVP_MD_CTX mdctx;
    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int len;
    int result;
    
    pthread_mutex_lock(&lock);
    if (first) {
	EVP_MD_CTX_init(&mdctx);
	first = 0;
    }

    len = RADLEN(rad);
    
    result = (EVP_DigestInit_ex(&mdctx, EVP_md5(), NULL) &&
	      EVP_DigestUpdate(&mdctx, rad, 4) &&
	      EVP_DigestUpdate(&mdctx, reqauth, 16) &&
	      (len <= 20 || EVP_DigestUpdate(&mdctx, rad + 20, len - 20)) &&
	      EVP_DigestUpdate(&mdctx, sec, strlen((char *)sec)) &&
	      EVP_DigestFinal_ex(&mdctx, hash, &len) &&
	      len == 16 &&
	      !memcmp(hash, rad + 4, 16));
    pthread_mutex_unlock(&lock);
    return result;
}
	      
int checkmessageauth(unsigned char *rad, uint8_t *authattr, char *secret) {
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    static unsigned char first = 1;
    static HMAC_CTX hmacctx;
    unsigned int md_len;
    uint8_t auth[16], hash[EVP_MAX_MD_SIZE];
    
    pthread_mutex_lock(&lock);
    if (first) {
	HMAC_CTX_init(&hmacctx);
	first = 0;
    }

    memcpy(auth, authattr, 16);
    memset(authattr, 0, 16);
    md_len = 0;
    HMAC_Init_ex(&hmacctx, secret, strlen(secret), EVP_md5(), NULL);
    HMAC_Update(&hmacctx, rad, RADLEN(rad));
    HMAC_Final(&hmacctx, hash, &md_len);
    memcpy(authattr, auth, 16);
    if (md_len != 16) {
	debug(DBG_WARN, "message auth computation failed");
	pthread_mutex_unlock(&lock);
	return 0;
    }

    if (memcmp(auth, hash, 16)) {
	debug(DBG_WARN, "message authenticator, wrong value");
	pthread_mutex_unlock(&lock);
	return 0;
    }	
	
    pthread_mutex_unlock(&lock);
    return 1;
}

int createmessageauth(unsigned char *rad, unsigned char *authattrval, char *secret) {
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    static unsigned char first = 1;
    static HMAC_CTX hmacctx;
    unsigned int md_len;

    if (!authattrval)
	return 1;
    
    pthread_mutex_lock(&lock);
    if (first) {
	HMAC_CTX_init(&hmacctx);
	first = 0;
    }

    memset(authattrval, 0, 16);
    md_len = 0;
    HMAC_Init_ex(&hmacctx, secret, strlen(secret), EVP_md5(), NULL);
    HMAC_Update(&hmacctx, rad, RADLEN(rad));
    HMAC_Final(&hmacctx, authattrval, &md_len);
    if (md_len != 16) {
	debug(DBG_WARN, "message auth computation failed");
	pthread_mutex_unlock(&lock);
	return 0;
    }

    pthread_mutex_unlock(&lock);
    return 1;
}

unsigned char *attrget(unsigned char *attrs, int length, uint8_t type) {
    while (length > 1) {
	if (ATTRTYPE(attrs) == type)
	    return attrs;
	length -= ATTRLEN(attrs);
	attrs += ATTRLEN(attrs);
    }
    return NULL;
}

void freerqdata(struct request *rq) {
    if (rq->origusername)
	free(rq->origusername);
    if (rq->buf)
	free(rq->buf);
}

void sendrq(struct server *to, struct request *rq) {
    int i;
    uint8_t *attr;

    pthread_mutex_lock(&to->newrq_mutex);
    /* might simplify if only try nextid, might be ok */
    for (i = to->nextid; i < MAX_REQUESTS; i++)
	if (!to->requests[i].buf)
	    break;
    if (i == MAX_REQUESTS) {
	for (i = 0; i < to->nextid; i++)
	    if (!to->requests[i].buf)
		break;
	if (i == to->nextid) {
	    debug(DBG_WARN, "No room in queue, dropping request");
	    freerqdata(rq);
	    pthread_mutex_unlock(&to->newrq_mutex);
	    return;
	}
    }
    
    rq->buf[1] = (char)i;

    attr = attrget(rq->buf + 20, RADLEN(rq->buf) - 20, RAD_Attr_Message_Authenticator);
    if (attr && !createmessageauth(rq->buf, ATTRVAL(attr), to->conf->secret)) {
	freerqdata(rq);
	pthread_mutex_unlock(&to->newrq_mutex);
	return;
    }

    debug(DBG_DBG, "sendrq: inserting packet with id %d in queue for %s", i, to->conf->host);
    to->requests[i] = *rq;
    to->nextid = i + 1;

    if (!to->newrq) {
	to->newrq = 1;
	debug(DBG_DBG, "signalling client writer");
	pthread_cond_signal(&to->newrq_cond);
    }
    pthread_mutex_unlock(&to->newrq_mutex);
}

void sendreply(struct client *to, unsigned char *buf, struct sockaddr_storage *tosa) {
    struct reply *reply;
    uint8_t first;
    
    if (!radsign(buf, (unsigned char *)to->conf->secret)) {
	free(buf);
	debug(DBG_WARN, "sendreply: failed to sign message");
	return;
    }

    reply = malloc(sizeof(struct reply));
    if (!reply) {
	free(buf);
	debug(DBG_ERR, "sendreply: malloc failed");
	return;
    }
    memset(reply, 0, sizeof(struct reply));
    reply->buf = buf;
    if (tosa)
	reply->tosa = *tosa;
    
    pthread_mutex_lock(&to->replyq->mutex);

    first = list_first(to->replyq->replies) == NULL;
    
    if (!list_push(to->replyq->replies, reply)) {
	pthread_mutex_unlock(&to->replyq->mutex);
	free(reply);
	free(buf);
	debug(DBG_ERR, "sendreply: malloc failed");
	return;
    }
    
    if (first) {
	debug(DBG_DBG, "signalling server writer");
	pthread_cond_signal(&to->replyq->cond);
    }
    pthread_mutex_unlock(&to->replyq->mutex);
}

int pwdencrypt(uint8_t *in, uint8_t len, char *shared, uint8_t sharedlen, uint8_t *auth) {
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    static unsigned char first = 1;
    static EVP_MD_CTX mdctx;
    unsigned char hash[EVP_MAX_MD_SIZE], *input;
    unsigned int md_len;
    uint8_t i, offset = 0, out[128];
    
    pthread_mutex_lock(&lock);
    if (first) {
	EVP_MD_CTX_init(&mdctx);
	first = 0;
    }

    input = auth;
    for (;;) {
	if (!EVP_DigestInit_ex(&mdctx, EVP_md5(), NULL) ||
	    !EVP_DigestUpdate(&mdctx, (uint8_t *)shared, sharedlen) ||
	    !EVP_DigestUpdate(&mdctx, input, 16) ||
	    !EVP_DigestFinal_ex(&mdctx, hash, &md_len) ||
	    md_len != 16) {
	    pthread_mutex_unlock(&lock);
	    return 0;
	}
	for (i = 0; i < 16; i++)
	    out[offset + i] = hash[i] ^ in[offset + i];
	input = out + offset - 16;
	offset += 16;
	if (offset == len)
	    break;
    }
    memcpy(in, out, len);
    pthread_mutex_unlock(&lock);
    return 1;
}

int pwddecrypt(uint8_t *in, uint8_t len, char *shared, uint8_t sharedlen, uint8_t *auth) {
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    static unsigned char first = 1;
    static EVP_MD_CTX mdctx;
    unsigned char hash[EVP_MAX_MD_SIZE], *input;
    unsigned int md_len;
    uint8_t i, offset = 0, out[128];
    
    pthread_mutex_lock(&lock);
    if (first) {
	EVP_MD_CTX_init(&mdctx);
	first = 0;
    }

    input = auth;
    for (;;) {
	if (!EVP_DigestInit_ex(&mdctx, EVP_md5(), NULL) ||
	    !EVP_DigestUpdate(&mdctx, (uint8_t *)shared, sharedlen) ||
	    !EVP_DigestUpdate(&mdctx, input, 16) ||
	    !EVP_DigestFinal_ex(&mdctx, hash, &md_len) ||
	    md_len != 16) {
	    pthread_mutex_unlock(&lock);
	    return 0;
	}
	for (i = 0; i < 16; i++)
	    out[offset + i] = hash[i] ^ in[offset + i];
	input = in + offset;
	offset += 16;
	if (offset == len)
	    break;
    }
    memcpy(in, out, len);
    pthread_mutex_unlock(&lock);
    return 1;
}

int msmppencrypt(uint8_t *text, uint8_t len, uint8_t *shared, uint8_t sharedlen, uint8_t *auth, uint8_t *salt) {
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    static unsigned char first = 1;
    static EVP_MD_CTX mdctx;
    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int md_len;
    uint8_t i, offset;
    
    pthread_mutex_lock(&lock);
    if (first) {
	EVP_MD_CTX_init(&mdctx);
	first = 0;
    }

#if 0
    printfchars(NULL, "msppencrypt auth in", "%02x ", auth, 16);
    printfchars(NULL, "msppencrypt salt in", "%02x ", salt, 2);
    printfchars(NULL, "msppencrypt in", "%02x ", text, len);
#endif
    
    if (!EVP_DigestInit_ex(&mdctx, EVP_md5(), NULL) ||
	!EVP_DigestUpdate(&mdctx, shared, sharedlen) ||
	!EVP_DigestUpdate(&mdctx, auth, 16) ||
	!EVP_DigestUpdate(&mdctx, salt, 2) ||
	!EVP_DigestFinal_ex(&mdctx, hash, &md_len)) {
	pthread_mutex_unlock(&lock);
	return 0;
    }

#if 0    
    printfchars(NULL, "msppencrypt hash", "%02x ", hash, 16);
#endif
    
    for (i = 0; i < 16; i++)
	text[i] ^= hash[i];
    
    for (offset = 16; offset < len; offset += 16) {
#if 0	
	printf("text + offset - 16 c(%d): ", offset / 16);
	printfchars(NULL, NULL, "%02x ", text + offset - 16, 16);
#endif
	if (!EVP_DigestInit_ex(&mdctx, EVP_md5(), NULL) ||
	    !EVP_DigestUpdate(&mdctx, shared, sharedlen) ||
	    !EVP_DigestUpdate(&mdctx, text + offset - 16, 16) ||
	    !EVP_DigestFinal_ex(&mdctx, hash, &md_len) ||
	    md_len != 16) {
	    pthread_mutex_unlock(&lock);
	    return 0;
	}
#if 0
	printfchars(NULL, "msppencrypt hash", "%02x ", hash, 16);
#endif    
	
	for (i = 0; i < 16; i++)
	    text[offset + i] ^= hash[i];
    }
    
#if 0
    printfchars(NULL, "msppencrypt out", "%02x ", text, len);
#endif

    pthread_mutex_unlock(&lock);
    return 1;
}

int msmppdecrypt(uint8_t *text, uint8_t len, uint8_t *shared, uint8_t sharedlen, uint8_t *auth, uint8_t *salt) {
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    static unsigned char first = 1;
    static EVP_MD_CTX mdctx;
    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int md_len;
    uint8_t i, offset;
    char plain[255];
    
    pthread_mutex_lock(&lock);
    if (first) {
	EVP_MD_CTX_init(&mdctx);
	first = 0;
    }

#if 0
    printfchars(NULL, "msppdecrypt auth in", "%02x ", auth, 16);
    printfchars(NULL, "msppdecrypt salt in", "%02x ", salt, 2);
    printfchars(NULL, "msppdecrypt in", "%02x ", text, len);
#endif
    
    if (!EVP_DigestInit_ex(&mdctx, EVP_md5(), NULL) ||
	!EVP_DigestUpdate(&mdctx, shared, sharedlen) ||
	!EVP_DigestUpdate(&mdctx, auth, 16) ||
	!EVP_DigestUpdate(&mdctx, salt, 2) ||
	!EVP_DigestFinal_ex(&mdctx, hash, &md_len)) {
	pthread_mutex_unlock(&lock);
	return 0;
    }

#if 0    
    printfchars(NULL, "msppdecrypt hash", "%02x ", hash, 16);
#endif
    
    for (i = 0; i < 16; i++)
	plain[i] = text[i] ^ hash[i];
    
    for (offset = 16; offset < len; offset += 16) {
#if 0 	
	printf("text + offset - 16 c(%d): ", offset / 16);
	printfchars(NULL, NULL, "%02x ", text + offset - 16, 16);
#endif
	if (!EVP_DigestInit_ex(&mdctx, EVP_md5(), NULL) ||
	    !EVP_DigestUpdate(&mdctx, shared, sharedlen) ||
	    !EVP_DigestUpdate(&mdctx, text + offset - 16, 16) ||
	    !EVP_DigestFinal_ex(&mdctx, hash, &md_len) ||
	    md_len != 16) {
	    pthread_mutex_unlock(&lock);
	    return 0;
	}
#if 0
	printfchars(NULL, "msppdecrypt hash", "%02x ", hash, 16);
#endif    

	for (i = 0; i < 16; i++)
	    plain[offset + i] = text[offset + i] ^ hash[i];
    }

    memcpy(text, plain, len);
#if 0
    printfchars(NULL, "msppdecrypt out", "%02x ", text, len);
#endif

    pthread_mutex_unlock(&lock);
    return 1;
}

struct realm *id2realm(char *id, uint8_t len) {
    struct list_node *entry;
    struct realm *realm;
    
    for (entry = list_first(realms); entry; entry = list_next(entry)) {
	realm = (struct realm *)entry->data;
	if (!regexec(&realm->regex, id, 0, NULL, 0)) {
	    debug(DBG_DBG, "found matching realm: %s", realm->name);
	    return realm;
	}
    }
    return NULL;
}

int rqinqueue(struct server *to, struct client *from, uint8_t id) {
    int i;
    
    pthread_mutex_lock(&to->newrq_mutex);
    for (i = 0; i < MAX_REQUESTS; i++)
	if (to->requests[i].buf && !to->requests[i].received && to->requests[i].origid == id && to->requests[i].from == from)
	    break;
    pthread_mutex_unlock(&to->newrq_mutex);
    
    return i < MAX_REQUESTS;
}

int attrvalidate(unsigned char *attrs, int length) {
    while (length > 1) {
	if (ATTRLEN(attrs) < 2) {
	    debug(DBG_WARN, "attrvalidate: invalid attribute length %d", ATTRLEN(attrs));
	    return 0;
	}
	length -= ATTRLEN(attrs);
	if (length < 0) {
	    debug(DBG_WARN, "attrvalidate: attribute length %d exceeds packet length", ATTRLEN(attrs));
	    return 0;
	}
	attrs += ATTRLEN(attrs);
    }
    if (length)
	debug(DBG_WARN, "attrvalidate: malformed packet? remaining byte after last attribute");
    return 1;
}

int pwdrecrypt(uint8_t *pwd, uint8_t len, char *oldsecret, char *newsecret, uint8_t *oldauth, uint8_t *newauth) {
    if (len < 16 || len > 128 || len % 16) {
	debug(DBG_WARN, "pwdrecrypt: invalid password length");
	return 0;
    }
	
    if (!pwddecrypt(pwd, len, oldsecret, strlen(oldsecret), oldauth)) {
	debug(DBG_WARN, "pwdrecrypt: cannot decrypt password");
	return 0;
    }
#ifdef DEBUG
    printfchars(NULL, "pwdrecrypt: password", "%02x ", pwd, len);
#endif	
    if (!pwdencrypt(pwd, len, newsecret, strlen(newsecret), newauth)) {
	debug(DBG_WARN, "pwdrecrypt: cannot encrypt password");
	return 0;
    }
    return 1;
}

int msmpprecrypt(uint8_t *msmpp, uint8_t len, char *oldsecret, char *newsecret, unsigned char *oldauth, char *newauth) {
    if (len < 18)
	return 0;
    if (!msmppdecrypt(msmpp + 2, len - 2, (unsigned char *)oldsecret, strlen(oldsecret), oldauth, msmpp)) {
	debug(DBG_WARN, "msmpprecrypt: failed to decrypt msppe key");
	return 0;
    }
    if (!msmppencrypt(msmpp + 2, len - 2, (unsigned char *)newsecret, strlen(newsecret), (unsigned char *)newauth, msmpp)) {
	debug(DBG_WARN, "msmpprecrypt: failed to encrypt msppe key");
	return 0;
    }
    return 1;
}

int msmppe(unsigned char *attrs, int length, uint8_t type, char *attrtxt, struct request *rq,
	   char *oldsecret, char *newsecret) {
    unsigned char *attr;
    
    for (attr = attrs; (attr = attrget(attr, length - (attr - attrs), type)); attr += ATTRLEN(attr)) {
	debug(DBG_DBG, "msmppe: Got %s", attrtxt);
	if (!msmpprecrypt(ATTRVAL(attr), ATTRVALLEN(attr), oldsecret, newsecret, rq->buf + 4, rq->origauth))
	    return 0;
    }
    return 1;
}

int findvendorsubattr(uint32_t *attrs, uint32_t vendor, uint8_t subattr) {
    if (!attrs)
	return 0;
    
    for (; attrs[0]; attrs += 2)
	if (attrs[0] == vendor && attrs[1] == subattr)
	    return 1;
    return 0;
}

int dovendorrewrite(uint8_t *attrs, uint16_t length, uint32_t *removevendorattrs) {
    uint8_t alen, sublen, rmlen = 0;
    uint32_t vendor = *(uint32_t *)ATTRVAL(attrs);
    uint8_t *subattrs;
    
    if (!removevendorattrs)
	return 0;

    while (*removevendorattrs && *removevendorattrs != vendor)
	removevendorattrs += 2;
    if (!*removevendorattrs)
	return 0;
    
    alen = ATTRLEN(attrs);

    if (findvendorsubattr(removevendorattrs, vendor, -1)) {
	/* remove entire vendor attribute */
	memmove(attrs, attrs + alen, length - alen);
	return alen;
    }

    sublen = alen - 4;
    subattrs = ATTRVAL(attrs) + 4;
    
    if (!attrvalidate(subattrs, sublen)) {
	debug(DBG_WARN, "dovendorrewrite: vendor attribute validation failed, no rewrite");
	return 0;
    }

    length -= 6;
    while (sublen > 1) {
	alen = ATTRLEN(subattrs);
	sublen -= alen;
	length -= alen;
	if (findvendorsubattr(removevendorattrs, vendor, ATTRTYPE(subattrs))) {
	    memmove(subattrs, subattrs + alen, length);
	    rmlen += alen;
	} else
	    subattrs += alen;
    }

    ATTRLEN(attrs) -= rmlen;
    return rmlen;
}

void dorewrite(uint8_t *buf, struct rewrite *rewrite) {
    uint8_t *attrs, alen;
    uint16_t len, rmlen = 0;
    
    if (!rewrite || (!rewrite->removeattrs && !rewrite->removevendorattrs))
	return;

    len = RADLEN(buf) - 20;
    attrs = buf + 20;
    while (len > 1) {
	alen = ATTRLEN(attrs);
	len -= alen;
	if (rewrite->removeattrs && strchr((char *)rewrite->removeattrs, ATTRTYPE(attrs))) {
	    memmove(attrs, attrs + alen, len);
	    rmlen += alen;
	} else if (ATTRTYPE(attrs) == RAD_Attr_Vendor_Specific && rewrite->removevendorattrs)
	    rmlen += dovendorrewrite(attrs, len, rewrite->removevendorattrs);
	else
	    attrs += alen;
    }
    if (rmlen)
	((uint16_t *)buf)[1] = htons(RADLEN(buf) - rmlen);
}

/* returns a pointer to the resized attribute value */
uint8_t *resizeattr(uint8_t **buf, uint8_t newvallen, uint8_t type) {
    uint8_t *attrs, *attr, vallen;
    uint16_t len;
    unsigned char *new;
    
    len = RADLEN(*buf) - 20;
    attrs = *buf + 20;

    attr = attrget(attrs, len, type);
    if (!attr)
	return NULL;
    
    vallen = ATTRVALLEN(attr);
    if (vallen == newvallen)
	return attr + 2;

    len += newvallen - vallen;
    if (newvallen > vallen) {
	new = realloc(*buf, len + 20);
	if (!new) {
	    debug(DBG_ERR, "resizeattr: malloc failed");
	    return NULL;
	}
	if (new != *buf) {
	    attr += new - *buf;
	    attrs = new + 20;
	    *buf = new;
	}
    }
    memmove(attr + 2 + newvallen, attr + 2 + vallen, len - (attr - attrs + newvallen));
    attr[1] = newvallen + 2;
    ((uint16_t *)*buf)[1] = htons(len + 20);
    return attr + 2;
}
		
int rewriteusername(struct request *rq, char *in) {
    size_t nmatch = 10, reslen = 0, start = 0;
    regmatch_t pmatch[10], *pfield;
    int i;
    unsigned char *result;
    char *out = rq->from->conf->rewriteattrreplacement;
    
    if (regexec(rq->from->conf->rewriteattrregex, in, nmatch, pmatch, 0)) {
	debug(DBG_DBG, "rewriteattr: username not matching, no rewrite");
	return 1;
    }
    
    rq->origusername = stringcopy(in, 0);
    if (!rq->origusername)
	return 0;
    
    for (i = start; out[i]; i++) {
	if (out[i] == '\\' && out[i + 1] >= '1' && out[i + 1] <= '9') {
	    pfield = &pmatch[out[i + 1] - '0'];
	    if (pfield->rm_so >= 0) {
		reslen += i - start + pfield->rm_eo - pfield->rm_so;
		start = i + 2;
	    }
	    i++;
	}
    }
    reslen += i - start;

    result = resizeattr(&rq->buf, reslen, RAD_Attr_User_Name);
    if (!result)
	return 0;
    
    start = 0;
    reslen = 0;
    for (i = start; out[i]; i++) {
	if (out[i] == '\\' && out[i + 1] >= '1' && out[i + 1] <= '9') {
	    pfield = &pmatch[out[i + 1] - '0'];
	    if (pfield->rm_so >= 0) {
		memcpy(result + reslen, out + start, i - start);
		reslen += i - start;
		memcpy(result + reslen, in + pfield->rm_so, pfield->rm_eo - pfield->rm_so);
		reslen += pfield->rm_eo - pfield->rm_so;
		start = i + 2;
	    }
	    i++;
	}
    }

    memcpy(result + reslen, out + start, i - start);
    reslen += i - start;
    memcpy(in, result, reslen);
    in[reslen] = '\0';
    return 1;
}

void acclog(unsigned char *attrs, int length, char *host) {
    unsigned char *attr;
    char username[256];
    
    attr = attrget(attrs, length, RAD_Attr_User_Name);
    if (!attr) {
	debug(DBG_INFO, "acclog: accounting-request from %s without username attribute", host);
	return;
    }
    memcpy(username, ATTRVAL(attr), ATTRVALLEN(attr));
    username[ATTRVALLEN(attr)] = '\0';
    debug(DBG_INFO, "acclog: accounting-request from %s with username: %s", host, username);
}
	
void respondaccounting(struct request *rq) {
    unsigned char *resp;

    resp = malloc(20);
    if (!resp) {
	debug(DBG_ERR, "respondstatusserver: malloc failed");
	return;
    }
    memcpy(resp, rq->buf, 20);
    resp[0] = RAD_Accounting_Response;
    resp[2] = 0;
    resp[3] = 20;
    debug(DBG_DBG, "respondaccounting: responding to %s", rq->from->conf->host);
    sendreply(rq->from, resp, rq->from->conf->type == 'U' ? &rq->fromsa : NULL);
}

void respondstatusserver(struct request *rq) {
    unsigned char *resp;

    resp = malloc(20);
    if (!resp) {
	debug(DBG_ERR, "respondstatusserver: malloc failed");
	return;
    }
    memcpy(resp, rq->buf, 20);
    resp[0] = RAD_Access_Accept;
    resp[2] = 0;
    resp[3] = 20;
    debug(DBG_DBG, "respondstatusserver: responding to %s", rq->from->conf->host);
    sendreply(rq->from, resp, rq->from->conf->type == 'U' ? &rq->fromsa : NULL);
}

void respondreject(struct request *rq, char *message) {
    unsigned char *resp;
    int len = 20;

    if (message && *message)
	len += 2 + strlen(message);
    
    resp = malloc(len);
    if (!resp) {
	debug(DBG_ERR, "respondreject: malloc failed");
	return;
    }
    memcpy(resp, rq->buf, 20);
    resp[0] = RAD_Access_Reject;
    *(uint16_t *)(resp + 2) = htons(len);
    if (message && *message) {
	resp[20] = RAD_Attr_Reply_Message;
	resp[21] = len - 20;
	memcpy(resp + 22, message, len - 22);
    }
    sendreply(rq->from, resp, rq->from->conf->type == 'U' ? &rq->fromsa : NULL);
}

struct server *realm2server(struct realm *realm) {
    struct list_node *entry;
    struct server *server, *best = NULL, *first = NULL;
    
    for (entry = list_first(realm->srvconfs); entry; entry = list_next(entry)) {
	server = ((struct clsrvconf *)entry->data)->servers;
	if (!first)
	    first = server;
	if (!server->connectionok)
	    continue;
	if (!server->loststatsrv)
	    return server;
	if (!best) {
	    best = server;
	    continue;
	}
	if (server->loststatsrv < best->loststatsrv)
	    best = server;
    }
    return best ? best : first;
}

void radsrv(struct request *rq) {
    uint8_t code, id, *auth, *attrs, *attr;
    uint16_t len;
    struct server *to = NULL;
    char username[256];
    unsigned char newauth[16];
    struct realm *realm = NULL;
    
    code = *(uint8_t *)rq->buf;
    id = *(uint8_t *)(rq->buf + 1);
    len = RADLEN(rq->buf);
    auth = (uint8_t *)(rq->buf + 4);

    debug(DBG_DBG, "radsrv: code %d, id %d, length %d", code, id, len);
    
    if (code != RAD_Access_Request && code != RAD_Status_Server && code != RAD_Accounting_Request) {
	debug(DBG_INFO, "radsrv: server currently accepts only access-requests, accounting-requests and status-server, ignoring");
	freerqdata(rq);
	return;
    }

    len -= 20;
    attrs = rq->buf + 20;

    if (!attrvalidate(attrs, len)) {
	debug(DBG_WARN, "radsrv: attribute validation failed, ignoring packet");
	freerqdata(rq);
	return;
    }

    attr = attrget(attrs, len, RAD_Attr_Message_Authenticator);
    if (attr && (ATTRVALLEN(attr) != 16 || !checkmessageauth(rq->buf, ATTRVAL(attr), rq->from->conf->secret))) {
	debug(DBG_WARN, "radsrv: message authentication failed");
	freerqdata(rq);
	return;
    }

    if (code != RAD_Access_Request) {
	switch (code) {
	case RAD_Accounting_Request:
	    /* when forwarding accounting, also filter attributes, call removeattrs(rq) */
	    acclog(attrs, len, rq->from->conf->host);
	    respondaccounting(rq);
	    break;
	case RAD_Status_Server:
	    respondstatusserver(rq);
	    break;
	}
	freerqdata(rq);
	return;
    }

    /* code == RAD_Access_Request */
    if (rq->from->conf->rewrite) {
	dorewrite(rq->buf, rq->from->conf->rewrite);
	len = RADLEN(rq->buf) - 20;
    }
    
    attr = attrget(attrs, len, RAD_Attr_User_Name);
    if (!attr) {
	debug(DBG_WARN, "radsrv: ignoring request, no username attribute");
	freerqdata(rq);
	return;
    }
    memcpy(username, ATTRVAL(attr), ATTRVALLEN(attr));
    username[ATTRVALLEN(attr)] = '\0';

    if (rq->from->conf->rewriteattrregex) {
	if (!rewriteusername(rq, username)) {
	    debug(DBG_WARN, "radsrv: username malloc failed, ignoring request");
	    freerqdata(rq);
	    return;
	}
	len = RADLEN(rq->buf) - 20;
	auth = (uint8_t *)(rq->buf + 4);
	attrs = rq->buf + 20;
    }

    if (rq->origusername)
	debug(DBG_DBG, "Access Request with username: %s (originally %s)", username, rq->origusername);
    else
	debug(DBG_DBG, "Access Request with username: %s", username);
	
    realm = id2realm(username, strlen(username));
    if (!realm) {
	debug(DBG_INFO, "radsrv: ignoring request, don't know where to send it");
	freerqdata(rq);
	return;
    }
	
    to = realm2server(realm);
    if (to && rqinqueue(to, rq->from, id)) {
	debug(DBG_INFO, "radsrv: already got request from host %s with id %d, ignoring", rq->from->conf->host, id);
	freerqdata(rq);
	return;
    }

    if (!to) {
	if (realm->message) {
	    debug(DBG_INFO, "radsrv: sending reject to %s for %s", rq->from->conf->host, username);
	    respondreject(rq, realm->message);
	}
	freerqdata(rq);
	return;
    }
    
    if (!RAND_bytes(newauth, 16)) {
	debug(DBG_WARN, "radsrv: failed to generate random auth");
	freerqdata(rq);
	return;
    }

#ifdef DEBUG
    printfchars(NULL, "auth", "%02x ", auth, 16);
#endif

    attr = attrget(attrs, len, RAD_Attr_User_Password);
    if (attr) {
	debug(DBG_DBG, "radsrv: found userpwdattr with value length %d", ATTRVALLEN(attr));
	if (!pwdrecrypt(ATTRVAL(attr), ATTRVALLEN(attr), rq->from->conf->secret, to->conf->secret, auth, newauth)) {
	    freerqdata(rq);
	    return;
	}
    }
    
    attr = attrget(attrs, len, RAD_Attr_Tunnel_Password);
    if (attr) {
	debug(DBG_DBG, "radsrv: found tunnelpwdattr with value length %d", ATTRVALLEN(attr));
	if (!pwdrecrypt(ATTRVAL(attr), ATTRVALLEN(attr), rq->from->conf->secret, to->conf->secret, auth, newauth)) {
	    freerqdata(rq);
	    return;
	}
    }

    rq->origid = id;
    memcpy(rq->origauth, auth, 16);
    memcpy(auth, newauth, 16);
    sendrq(to, rq);
}

int replyh(struct server *server, unsigned char *buf) {
    struct client *from;
    struct request *rq;
    int i, len, sublen;
    unsigned char *messageauth, *subattrs, *attrs, *attr, *username;
    struct sockaddr_storage fromsa;
    char tmp[256];
    
    server->connectionok = 1;
    server->loststatsrv = 0;
	
    i = buf[1]; /* i is the id */

    switch (*buf) {
    case RAD_Access_Accept:
	debug(DBG_DBG, "got Access Accept with id %d", i);
	break;
    case RAD_Access_Reject:
	debug(DBG_DBG, "got Access Reject with id %d", i);
	break;
    case RAD_Access_Challenge:
	debug(DBG_DBG, "got Access Challenge with id %d", i);
	break;
    default:
	debug(DBG_INFO, "replyh: discarding, only accept access accept, access reject and access challenge messages");
	return 0;
    }

    rq = server->requests + i;

    pthread_mutex_lock(&server->newrq_mutex);
    if (!rq->buf || !rq->tries) {
	pthread_mutex_unlock(&server->newrq_mutex);
	debug(DBG_INFO, "replyh: no matching request sent with this id, ignoring reply");
	return 0;
    }

    if (rq->received) {
	pthread_mutex_unlock(&server->newrq_mutex);
	debug(DBG_INFO, "replyh: already received, ignoring reply");
	return 0;
    }
	
    if (!validauth(buf, rq->buf + 4, (unsigned char *)server->conf->secret)) {
	pthread_mutex_unlock(&server->newrq_mutex);
	debug(DBG_WARN, "replyh: invalid auth, ignoring reply");
	return 0;
    }
	
    len = RADLEN(buf) - 20;
    attrs = buf + 20;

    if (!attrvalidate(attrs, len)) {
	pthread_mutex_unlock(&server->newrq_mutex);
	debug(DBG_WARN, "replyh: attribute validation failed, ignoring reply");
	return 0;
    }
	
    /* Message Authenticator */
    messageauth = attrget(attrs, len, RAD_Attr_Message_Authenticator);
    if (messageauth) {
	if (ATTRVALLEN(messageauth) != 16) {
	    pthread_mutex_unlock(&server->newrq_mutex);
	    debug(DBG_WARN, "replyh: illegal message auth attribute length, ignoring reply");
	    return 0;
	}
	memcpy(tmp, buf + 4, 16);
	memcpy(buf + 4, rq->buf + 4, 16);
	if (!checkmessageauth(buf, ATTRVAL(messageauth), server->conf->secret)) {
	    pthread_mutex_unlock(&server->newrq_mutex);
	    debug(DBG_WARN, "replyh: message authentication failed, ignoring reply");
	    return 0;
	}
	memcpy(buf + 4, tmp, 16);
	debug(DBG_DBG, "replyh: message auth ok");
    }
	
    if (*rq->buf == RAD_Status_Server) {
	rq->received = 1;
	pthread_mutex_unlock(&server->newrq_mutex);
	debug(DBG_INFO, "replyh: got status server response from %s", server->conf->host);
	return 0;
    }

    from = rq->from;
    if (!from) {
	pthread_mutex_unlock(&server->newrq_mutex);
	debug(DBG_INFO, "replyh: client gone, ignoring reply");
	return 0;
    }
	
    if (server->conf->rewrite) {
	dorewrite(buf, server->conf->rewrite);
	len = RADLEN(buf) - 20;
    }
    
    /* MS MPPE */
    for (attr = attrs; (attr = attrget(attr, len - (attr - attrs), RAD_Attr_Vendor_Specific)); attr += ATTRLEN(attr)) {
	if (ATTRVALLEN(attr) <= 4)
	    break;
	    
	if (attr[2] != 0 || attr[3] != 0 || attr[4] != 1 || attr[5] != 55)  /* 311 == MS */
	    continue;
	    
	sublen = ATTRVALLEN(attr) - 4;
	subattrs = ATTRVAL(attr) + 4;  
	if (!attrvalidate(subattrs, sublen) ||
	    !msmppe(subattrs, sublen, RAD_VS_ATTR_MS_MPPE_Send_Key, "MS MPPE Send Key",
		    rq, server->conf->secret, from->conf->secret) ||
	    !msmppe(subattrs, sublen, RAD_VS_ATTR_MS_MPPE_Recv_Key, "MS MPPE Recv Key",
		    rq, server->conf->secret, from->conf->secret))
	    break;
    }
    if (attr) {
	pthread_mutex_unlock(&server->newrq_mutex);
	debug(DBG_WARN, "replyh: MS attribute handling failed, ignoring reply");
	return 0;
    }
	
    if (*buf == RAD_Access_Accept || *buf == RAD_Access_Reject) {
	attr = attrget(rq->buf + 20, RADLEN(rq->buf) - 20, RAD_Attr_User_Name);
	/* we know the attribute exists */
	memcpy(tmp, ATTRVAL(attr), ATTRVALLEN(attr));
	tmp[ATTRVALLEN(attr)] = '\0';
	switch (*buf) {
	case RAD_Access_Accept:
	    if (rq->origusername)
		debug(DBG_INFO, "Access Accept for %s (originally %s) from %s", tmp, rq->origusername, server->conf->host);
	    else
		debug(DBG_INFO, "Access Accept for %s from %s", tmp, server->conf->host);
	    break;
	case RAD_Access_Reject:
	    if (rq->origusername)
		debug(DBG_INFO, "Access Reject for %s (originally %s) from %s", tmp, rq->origusername, server->conf->host);
	    else
		debug(DBG_INFO, "Access Reject for %s from %s", tmp, server->conf->host);
	    break;
	}
    }
	
    buf[1] = (char)rq->origid;
    memcpy(buf + 4, rq->origauth, 16);
#ifdef DEBUG	
    printfchars(NULL, "origauth/buf+4", "%02x ", buf + 4, 16);
#endif

    if (rq->origusername) {
	username = resizeattr(&buf, strlen(rq->origusername), RAD_Attr_User_Name);
	if (!username) {
	    pthread_mutex_unlock(&server->newrq_mutex);
	    debug(DBG_WARN, "replyh: malloc failed, ignoring reply");
	    return 0;
	}
	memcpy(username, rq->origusername, strlen(rq->origusername));
	len = RADLEN(buf) - 20;
	attrs = buf + 20;
	if (messageauth)
	    messageauth = attrget(attrs, len, RAD_Attr_Message_Authenticator);
    }
	
    if (messageauth) {
	if (!createmessageauth(buf, ATTRVAL(messageauth), from->conf->secret)) {
	    pthread_mutex_unlock(&server->newrq_mutex);
	    debug(DBG_WARN, "replyh: failed to create authenticator, malloc failed?, ignoring reply");
	    return 0;
	}
	debug(DBG_DBG, "replyh: computed messageauthattr");
    }

    if (from->conf->type == 'U')
	fromsa = rq->fromsa;
    /* once we set received = 1, rq may be reused */
    rq->received = 1;

    debug(DBG_DBG, "replyh: giving packet back to where it came from");
    sendreply(from, buf, from->conf->type == 'U' ? &fromsa : NULL);
    pthread_mutex_unlock(&server->newrq_mutex);
    return 1;
}

void *udpclientrd(void *arg) {
    struct server *server;
    unsigned char *buf;
    int *s = (int *)arg;
    
    for (;;) {
	server = NULL;
	buf = radudpget(*s, NULL, &server, NULL);
	if (!replyh(server, buf))
	    free(buf);
    }
}

void *tlsclientrd(void *arg) {
    struct server *server = (struct server *)arg;
    unsigned char *buf;
    struct timeval lastconnecttry;
    
    for (;;) {
	/* yes, lastconnecttry is really necessary */
	lastconnecttry = server->lastconnecttry;
	buf = radtlsget(server->ssl);
	if (!buf) {
	    tlsconnect(server, &lastconnecttry, "clientrd");
	    continue;
	}

	if (!replyh(server, buf))
	    free(buf);
    }
}

void *clientwr(void *arg) {
    struct server *server = (struct server *)arg;
    struct request *rq;
    pthread_t tlsclientrdth;
    int i;
    uint8_t rnd;
    struct timeval now, lastsend;
    struct timespec timeout;
    struct request statsrvrq;
    unsigned char statsrvbuf[38];

    memset(&timeout, 0, sizeof(struct timespec));
    
    if (server->conf->statusserver) {
	memset(&statsrvrq, 0, sizeof(struct request));
	memset(statsrvbuf, 0, sizeof(statsrvbuf));
	statsrvbuf[0] = RAD_Status_Server;
	statsrvbuf[3] = 38;
	statsrvbuf[20] = RAD_Attr_Message_Authenticator;
	statsrvbuf[21] = 18;
	gettimeofday(&lastsend, NULL);
    }
    
    if (server->conf->type == 'U') {
	server->connectionok = 1;
    } else {
	tlsconnect(server, NULL, "new client");
	server->connectionok = 1;
	if (pthread_create(&tlsclientrdth, NULL, tlsclientrd, (void *)server))
	    debugx(1, DBG_ERR, "clientwr: pthread_create failed");
    }
    
    for (;;) {
	pthread_mutex_lock(&server->newrq_mutex);
	if (!server->newrq) {
	    gettimeofday(&now, NULL);
	    if (server->conf->statusserver) {
		/* random 0-7 seconds */
		RAND_bytes(&rnd, 1);
		rnd /= 32;
		if (!timeout.tv_sec || timeout.tv_sec > lastsend.tv_sec + STATUS_SERVER_PERIOD + rnd)
		    timeout.tv_sec = lastsend.tv_sec + STATUS_SERVER_PERIOD + rnd;
	    }   
	    if (timeout.tv_sec) {
		debug(DBG_DBG, "clientwr: waiting up to %ld secs for new request", timeout.tv_sec - now.tv_sec);
		pthread_cond_timedwait(&server->newrq_cond, &server->newrq_mutex, &timeout);
		timeout.tv_sec = 0;
	    } else {
		debug(DBG_DBG, "clientwr: waiting for new request");
		pthread_cond_wait(&server->newrq_cond, &server->newrq_mutex);
	    }
	}
	if (server->newrq) {
	    debug(DBG_DBG, "clientwr: got new request");
	    server->newrq = 0;
	} else
	    debug(DBG_DBG, "clientwr: request timer expired, processing request queue");
	pthread_mutex_unlock(&server->newrq_mutex);

	for (i = 0; i < MAX_REQUESTS; i++) {
	    pthread_mutex_lock(&server->newrq_mutex);
	    while (i < MAX_REQUESTS && !server->requests[i].buf)
		i++;
	    if (i == MAX_REQUESTS) {
		pthread_mutex_unlock(&server->newrq_mutex);
		break;
	    }
	    rq = server->requests + i;

            if (rq->received) {
		debug(DBG_DBG, "clientwr: packet %d in queue is marked as received", i);
		if (rq->buf) {
		    debug(DBG_DBG, "clientwr: freeing received packet %d from queue", i);
		    freerqdata(rq);
		    /* setting this to NULL means that it can be reused */
		    rq->buf = NULL;
		}
                pthread_mutex_unlock(&server->newrq_mutex);
                continue;
            }
	    
	    gettimeofday(&now, NULL);
            if (now.tv_sec < rq->expiry.tv_sec) {
		if (!timeout.tv_sec || rq->expiry.tv_sec < timeout.tv_sec)
		    timeout.tv_sec = rq->expiry.tv_sec;
		pthread_mutex_unlock(&server->newrq_mutex);
		continue;
	    }

	    if (rq->tries == (*rq->buf == RAD_Status_Server || server->conf->type == 'T'
			      ? 1 : REQUEST_RETRIES)) {
		debug(DBG_DBG, "clientwr: removing expired packet from queue");
		if (*rq->buf == RAD_Status_Server) {
		    debug(DBG_WARN, "clientwr: no status server response, %s dead?", server->conf->host);
		    if (server->loststatsrv < 255)
			server->loststatsrv++;
		}
		freerqdata(rq);
		/* setting this to NULL means that it can be reused */
		rq->buf = NULL;
		pthread_mutex_unlock(&server->newrq_mutex);
		continue;
	    }
            pthread_mutex_unlock(&server->newrq_mutex);

	    rq->expiry.tv_sec = now.tv_sec +
		(*rq->buf == RAD_Status_Server || server->conf->type == 'T'
		 ? REQUEST_EXPIRY : REQUEST_EXPIRY / REQUEST_RETRIES);
	    if (!timeout.tv_sec || rq->expiry.tv_sec < timeout.tv_sec)
		timeout.tv_sec = rq->expiry.tv_sec;
	    rq->tries++;
	    clientradput(server, server->requests[i].buf);
	    gettimeofday(&lastsend, NULL);
	}
	if (server->conf->statusserver) {
	    gettimeofday(&now, NULL);
	    if (now.tv_sec - lastsend.tv_sec >= STATUS_SERVER_PERIOD) {
		if (!RAND_bytes(statsrvbuf + 4, 16)) {
		    debug(DBG_WARN, "clientwr: failed to generate random auth");
		    continue;
		}
		statsrvrq.buf = malloc(sizeof(statsrvbuf));
		if (!statsrvrq.buf) {
		    debug(DBG_ERR, "clientwr: malloc failed");
		    continue;
		}
		memcpy(statsrvrq.buf, statsrvbuf, sizeof(statsrvbuf));
		debug(DBG_DBG, "clientwr: sending status server to %s", server->conf->host);
		lastsend.tv_sec = now.tv_sec;
		sendrq(server, &statsrvrq);
	    }
	}
    }
}

void *udpserverwr(void *arg) {
    struct replyq *replyq = udp_server_replyq;
    struct reply *reply;
    
    for (;;) {
	pthread_mutex_lock(&replyq->mutex);
	while (!(reply = (struct reply *)list_shift(replyq->replies))) {
	    debug(DBG_DBG, "udp server writer, waiting for signal");
	    pthread_cond_wait(&replyq->cond, &replyq->mutex);
	    debug(DBG_DBG, "udp server writer, got signal");
	}
	pthread_mutex_unlock(&replyq->mutex);

	if (sendto(udp_server_sock, reply->buf, RADLEN(reply->buf), 0,
		   (struct sockaddr *)&reply->tosa, SOCKADDR_SIZE(reply->tosa)) < 0)
	    debug(DBG_WARN, "sendudp: send failed");
	free(reply->buf);
	free(reply);
    }
}

void *udpserverrd(void *arg) {
    struct request rq;
    pthread_t udpserverwrth;
    struct clsrvconf *listenres;

    listenres = resolve_hostport('U', options.listenudp, DEFAULT_UDP_PORT);
    if ((udp_server_sock = bindtoaddr(listenres->addrinfo, AF_UNSPEC, 1, 0)) < 0)
	debugx(1, DBG_ERR, "udpserverrd: socket/bind failed");

    debug(DBG_WARN, "udpserverrd: listening for UDP on %s:%s",
	  listenres->host ? listenres->host : "*", listenres->port);
    free(listenres);
    
    if (pthread_create(&udpserverwrth, NULL, udpserverwr, NULL))
	debugx(1, DBG_ERR, "pthread_create failed");
    
    for (;;) {
	memset(&rq, 0, sizeof(struct request));
	rq.buf = radudpget(udp_server_sock, &rq.from, NULL, &rq.fromsa);
	radsrv(&rq);
    }
}

void *udpaccserverrd(void *arg) {
    struct request rq;
    struct clsrvconf *listenres;
    
    listenres = resolve_hostport('U', options.listenaccudp, DEFAULT_UDP_PORT);
    if ((udp_accserver_sock = bindtoaddr(listenres->addrinfo, AF_UNSPEC, 1, 0)) < 0)
	debugx(1, DBG_ERR, "udpserverrd: socket/bind failed");

    debug(DBG_WARN, "udpaccserverrd: listening for UDP on %s:%s",
	  listenres->host ? listenres->host : "*", listenres->port);
    free(listenres);
    
    for (;;) {
	memset(&rq, 0, sizeof(struct request));
	rq.buf = radudpget(udp_accserver_sock, &rq.from, NULL, &rq.fromsa);
	if (*(uint8_t *)rq.buf == RAD_Accounting_Request) {
	    radsrv(&rq);
	    continue;
	}
	debug(DBG_INFO, "udpaccserverrd: got something other than accounting-request, ignoring");
	freerqdata(&rq);
    }
}

void *tlsserverwr(void *arg) {
    int cnt;
    unsigned long error;
    struct client *client = (struct client *)arg;
    struct replyq *replyq;
    struct reply *reply;
    
    debug(DBG_DBG, "tlsserverwr starting for %s", client->conf->host);
    replyq = client->replyq;
    for (;;) {
	pthread_mutex_lock(&replyq->mutex);
	while (!list_first(replyq->replies)) {
	    if (client->ssl) {	    
		debug(DBG_DBG, "tls server writer, waiting for signal");
		pthread_cond_wait(&replyq->cond, &replyq->mutex);
		debug(DBG_DBG, "tls server writer, got signal");
	    }
	    if (!client->ssl) {
		/* ssl might have changed while waiting */
		pthread_mutex_unlock(&replyq->mutex);
		debug(DBG_DBG, "tlsserverwr: exiting as requested");
		pthread_exit(NULL);
	    }
	}
	reply = (struct reply *)list_shift(replyq->replies);
	pthread_mutex_unlock(&replyq->mutex);
	cnt = SSL_write(client->ssl, reply->buf, RADLEN(reply->buf));
	if (cnt > 0)
	    debug(DBG_DBG, "tlsserverwr: Sent %d bytes, Radius packet of length %d",
		  cnt, RADLEN(reply->buf));
	else
	    while ((error = ERR_get_error()))
		debug(DBG_ERR, "tlsserverwr: SSL: %s", ERR_error_string(error, NULL));
	free(reply->buf);
	free(reply);
    }
}

void tlsserverrd(struct client *client) {
    struct request rq;
    pthread_t tlsserverwrth;
    
    debug(DBG_DBG, "tlsserverrd starting for %s", client->conf->host);
    
    if (pthread_create(&tlsserverwrth, NULL, tlsserverwr, (void *)client)) {
	debug(DBG_ERR, "tlsserverrd: pthread_create failed");
	return;
    }

    for (;;) {
	memset(&rq, 0, sizeof(struct request));
	rq.buf = radtlsget(client->ssl);
	if (!rq.buf)
	    break;
	debug(DBG_DBG, "tlsserverrd: got Radius message from %s", client->conf->host);
	rq.from = client;
	radsrv(&rq);
    }
    
    debug(DBG_ERR, "tlsserverrd: connection lost");
    /* stop writer by setting ssl to NULL and give signal in case waiting for data */
    client->ssl = NULL;
    pthread_mutex_lock(&client->replyq->mutex);
    pthread_cond_signal(&client->replyq->cond);
    pthread_mutex_unlock(&client->replyq->mutex);
    debug(DBG_DBG, "tlsserverrd: waiting for writer to end");
    pthread_join(tlsserverwrth, NULL);
    removeclientrqs(client);
    debug(DBG_DBG, "tlsserverrd for %s exiting", client->conf->host);
}

void *tlsservernew(void *arg) {
    unsigned long error;
    int s;
    struct client *client = (struct client *)arg;
    SSL *ssl;
    X509 *cert;
    
    debug(DBG_DBG, "tlsservernew starting for %s", client->conf->host);
    ssl = client->ssl;

    if (SSL_accept(ssl) <= 0) {
        while ((error = ERR_get_error()))
            debug(DBG_ERR, "tlsservernew: SSL: %s", ERR_error_string(error, NULL));
        debug(DBG_ERR, "SSL_accept failed");
	goto exit;
    }

    cert = verifytlscert(ssl);
    if (!cert)
	goto exit;
    if (!verifyconfcert(cert, client->conf)) {
	X509_free(cert);
	goto exit;
    }
    X509_free(cert);
    tlsserverrd(client);
    
 exit:
    s = SSL_get_fd(ssl);
    SSL_free(ssl);
    shutdown(s, SHUT_RDWR);
    close(s);
    removeclient(client);
    debug(DBG_DBG, "tlsservernew thread for %s exiting", client->conf->host);
    pthread_exit(NULL);
}

/***********************************************
 *  new tls server code
 ***********************************************/
void *tlsservernew2(void *arg) {
    int s;
    struct sockaddr_storage from;
    size_t fromlen = sizeof(from);
    struct clsrvconf *conf;
    struct list_node *cur = NULL;
    SSL *ssl = NULL;
    X509 *cert = NULL;
    unsigned long error;
    struct client *client;

    s = *(int *)arg;
    if (getpeername(s, (struct sockaddr *)&from, &fromlen)) {
	debug(DBG_DBG, "tlsserverrd: getpeername failed, exiting");
	goto exit;
    }
    debug(DBG_WARN, "incoming TLS connection from %s", addr2string((struct sockaddr *)&from, fromlen));

    conf = find_conf('T', (struct sockaddr *)&from, clconfs, &cur);
    if (conf) {
	ssl = SSL_new(conf->ssl_ctx);
	SSL_set_fd(ssl, s);

	if (SSL_accept(ssl) <= 0) {
	    while ((error = ERR_get_error()))
		debug(DBG_ERR, "tlsserverrd: SSL: %s", ERR_error_string(error, NULL));
	    debug(DBG_ERR, "SSL_accept failed");
	    goto exit;
	}
	cert = verifytlscert(ssl);
	if (!cert)
	    goto exit;
    }
    
    while (conf) {
	if (verifyconfcert(cert, conf)) {
	    X509_free(cert);
	    client = addclient(conf);
	    if (client) {
		client->ssl = ssl;
		tlsserverrd(client);
		removeclient(client);
	    } else
		debug(DBG_WARN, "Failed to create new client instance");
	    goto exit;
	}
	conf = find_conf('T', (struct sockaddr *)&from, clconfs, &cur);
    }
    debug(DBG_WARN, "ignoring request, no matching TLS client");
    if (cert)
	X509_free(cert);

 exit:
    SSL_free(ssl);
    shutdown(s, SHUT_RDWR);
    close(s);
    pthread_exit(NULL);
}

int tlslistener2() {
    pthread_t tlsserverth;
    int s, snew;
    struct sockaddr_storage from;
    size_t fromlen = sizeof(from);
    struct clsrvconf *listenres;

    listenres = resolve_hostport('T', options.listentcp, DEFAULT_TLS_PORT);
    if ((s = bindtoaddr(listenres->addrinfo, AF_UNSPEC, 1, 0)) < 0)
	debugx(1, DBG_ERR, "tlslistener: socket/bind failed");

    listen(s, 0);
    debug(DBG_WARN, "listening for incoming TCP on %s:%s", listenres->host ? listenres->host : "*", listenres->port);
    free(listenres);

    for (;;) {
	snew = accept(s, (struct sockaddr *)&from, &fromlen);
	if (snew < 0) {
	    debug(DBG_WARN, "accept failed");
	    continue;
	}
	if (pthread_create(&tlsserverth, NULL, tlsservernew, (void *)&snew)) {
	    debug(DBG_ERR, "tlslistener: pthread_create failed");
	    shutdown(snew, SHUT_RDWR);
	    close(snew);
	    continue;
	}
	pthread_detach(tlsserverth);
    }
    return 0;
}

/***********************************************
 *  end of new tls listening code
 ***********************************************/

int tlslistener() {
    pthread_t tlsserverth;
    int s, snew;
    struct sockaddr_storage from;
    size_t fromlen = sizeof(from);
    struct clsrvconf *conf;
    struct client *client;
    struct clsrvconf *listenres;
    
    listenres = resolve_hostport('T', options.listentcp, DEFAULT_TLS_PORT);
    if ((s = bindtoaddr(listenres->addrinfo, AF_UNSPEC, 1, 0)) < 0)
        debugx(1, DBG_ERR, "tlslistener: socket/bind failed");
    
    listen(s, 0);
    debug(DBG_WARN, "listening for incoming TCP on %s:%s", listenres->host ? listenres->host : "*", listenres->port);
    free(listenres);
    
    for (;;) {
	snew = accept(s, (struct sockaddr *)&from, &fromlen);
	if (snew < 0) {
	    debug(DBG_WARN, "accept failed");
	    continue;
	}
	debug(DBG_WARN, "incoming TLS connection from %s", addr2string((struct sockaddr *)&from, fromlen));

	conf = find_conf('T', (struct sockaddr *)&from, clconfs, NULL);
	if (!conf) {
	    debug(DBG_WARN, "ignoring request, not a known TLS client");
	    shutdown(snew, SHUT_RDWR);
	    close(snew);
	    continue;
	}

	client = addclient(conf);
	if (!client) {
	    debug(DBG_WARN, "Failed to create new client instance");
	    shutdown(snew, SHUT_RDWR);
	    close(snew);
	    continue;
	}
	
	client->ssl = SSL_new(client->conf->ssl_ctx);
	SSL_set_fd(client->ssl, snew);
	if (pthread_create(&tlsserverth, NULL, tlsservernew, (void *)client)) {
	    debug(DBG_ERR, "tlslistener: pthread_create failed");
	    SSL_free(client->ssl);
	    removeclient(client);
	    shutdown(snew, SHUT_RDWR);
	    close(snew);
	    continue;
	}
	pthread_detach(tlsserverth);
    }
    return 0;
}

void tlsadd(char *value, char *cacertfile, char *cacertpath, char *certfile, char *certkeyfile, char *certkeypwd) {
    struct tls *new;
    SSL_CTX *ctx;
    STACK_OF(X509_NAME) *calist;
    int i;
    unsigned long error;
    
    if (!certfile || !certkeyfile)
	debugx(1, DBG_ERR, "TLSCertificateFile and TLSCertificateKeyFile must be specified in TLS context %s", value);

    if (!cacertfile && !cacertpath)
	debugx(1, DBG_ERR, "CA Certificate file or path need to be specified in TLS context %s", value);

    if (!ssl_locks) {
	ssl_locks = malloc(CRYPTO_num_locks() * sizeof(pthread_mutex_t));
	ssl_lock_count = OPENSSL_malloc(CRYPTO_num_locks() * sizeof(long));
	for (i = 0; i < CRYPTO_num_locks(); i++) {
	    ssl_lock_count[i] = 0;
	    pthread_mutex_init(&ssl_locks[i], NULL);
	}
	CRYPTO_set_id_callback(ssl_thread_id);
	CRYPTO_set_locking_callback(ssl_locking_callback);

	SSL_load_error_strings();
	SSL_library_init();

	while (!RAND_status()) {
	    time_t t = time(NULL);
	    pid_t pid = getpid();
	    RAND_seed((unsigned char *)&t, sizeof(time_t));
	    RAND_seed((unsigned char *)&pid, sizeof(pid));
	}
    }
    ctx = SSL_CTX_new(TLSv1_method());
    if (certkeypwd) {
	SSL_CTX_set_default_passwd_cb_userdata(ctx, certkeypwd);
	SSL_CTX_set_default_passwd_cb(ctx, pem_passwd_cb);
    }
    if (!SSL_CTX_use_certificate_chain_file(ctx, certfile) ||
	!SSL_CTX_use_PrivateKey_file(ctx, certkeyfile, SSL_FILETYPE_PEM) ||
	!SSL_CTX_check_private_key(ctx) ||
	!SSL_CTX_load_verify_locations(ctx, cacertfile, cacertpath)) {
	while ((error = ERR_get_error()))
	    debug(DBG_ERR, "SSL: %s", ERR_error_string(error, NULL));
	debugx(1, DBG_ERR, "Error initialising SSL/TLS in TLS context %s", value);
    }

    calist = cacertfile ? SSL_load_client_CA_file(cacertfile) : NULL;
    if (!cacertfile || calist) {
	if (cacertpath) {
	    if (!calist)
		calist = sk_X509_NAME_new_null();
	    if (!SSL_add_dir_cert_subjects_to_stack(calist, cacertpath)) {
		sk_X509_NAME_free(calist);
		calist = NULL;
	    }
	}
    }
    if (!calist) {
	while ((error = ERR_get_error()))
	    debug(DBG_ERR, "SSL: %s", ERR_error_string(error, NULL));
	debugx(1, DBG_ERR, "Error adding CA subjects in TLS context %s", value);
    }
    SSL_CTX_set_client_CA_list(ctx, calist);
    
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, verify_cb);
    SSL_CTX_set_verify_depth(ctx, MAX_CERT_DEPTH + 1);

    new = malloc(sizeof(struct tls));
    if (!new || !list_push(tlsconfs, new))
	debugx(1, DBG_ERR, "malloc failed");

    memset(new, 0, sizeof(struct tls));
    new->name = stringcopy(value, 0);
    if (!new->name)
	debugx(1, DBG_ERR, "malloc failed");
    new->ctx = ctx;
    new->count = 0;
    debug(DBG_DBG, "tlsadd: added TLS context %s", value);
}

void tlsfree() {
    struct list_node *entry;
    struct tls *t;
    
    for (entry = list_first(tlsconfs); entry; entry = list_next(entry)) {
	t = (struct tls *)entry->data;
	if (t->name)
	    free(t->name);
	if (!t->count)
	    SSL_CTX_free(t->ctx);
    }
    list_destroy(tlsconfs);
    tlsconfs = NULL;
}

SSL_CTX *tlsgetctx(char *alt1, char *alt2) {
    struct list_node *entry;
    struct tls *t, *t1 = NULL, *t2 = NULL;
    
    for (entry = list_first(tlsconfs); entry; entry = list_next(entry)) {
	t = (struct tls *)entry->data;
	if (!strcasecmp(t->name, alt1)) {
	    t1 = t;
	    break;
	}
	if (!t2 && alt2 && !strcasecmp(t->name, alt2))
	    t2 = t;
    }

    t = (t1 ? t1 : t2);
    if (!t)
	return NULL;
    t->count++;
    return t->ctx;
}

void addrealm(char *value, char **servers, char *message) {
    int n;
    struct realm *realm;
    char *s, *regex = NULL;
    struct list_node *entry;
    struct clsrvconf *conf;
    
    if (*value == '/') {
	/* regexp, remove optional trailing / if present */
	if (value[strlen(value) - 1] == '/')
	    value[strlen(value) - 1] = '\0';
    } else {
	/* not a regexp, let us make it one */
	if (*value == '*' && !value[1])
	    regex = stringcopy(".*", 0);
	else {
	    for (n = 0, s = value; *s;)
		if (*s++ == '.')
		    n++;
	    regex = malloc(strlen(value) + n + 3);
	    if (regex) {
		regex[0] = '@';
		for (n = 1, s = value; *s; s++) {
		    if (*s == '.')
			regex[n++] = '\\';
		    regex[n++] = *s;
		}
		regex[n++] = '$';
		regex[n] = '\0';
	    }
	}
	if (!regex)
	    debugx(1, DBG_ERR, "malloc failed");
	debug(DBG_DBG, "addrealm: constructed regexp %s from %s", regex, value);
    }

    realm = malloc(sizeof(struct realm));
    if (!realm)
	debugx(1, DBG_ERR, "malloc failed");
    
    memset(realm, 0, sizeof(struct realm));
    realm->name = stringcopy(value, 0);
    if (!realm->name)
	debugx(1, DBG_ERR, "malloc failed");
    if (message && strlen(message) > 253)
	debugx(1, DBG_ERR, "ReplyMessage can be at most 253 bytes");
    realm->message = message;
    
    if (regcomp(&realm->regex, regex ? regex : value + 1, REG_ICASE | REG_NOSUB))
	debugx(1, DBG_ERR, "addrealm: failed to compile regular expression %s", regex ? regex : value + 1);
    if (regex)
	free(regex);
    
    if (servers && *servers) {
	realm->srvconfs = list_create();
	if (!realm->srvconfs)
	    debugx(1, DBG_ERR, "malloc failed");
	for (n = 0; servers[n]; n++) {
	    for (entry = list_first(srvconfs); entry; entry = list_next(entry)) {
		conf = (struct clsrvconf *)entry->data;
		if (!strcasecmp(servers[n], conf->name))
		    break;
	    }
	    if (!entry)
		debugx(1, DBG_ERR, "addrealm failed, no server %s", servers[n]);
	    if (!list_push(realm->srvconfs, conf))
		debugx(1, DBG_ERR, "malloc failed");
	    debug(DBG_DBG, "addrealm: added server %s for realm %s", conf->name, value);
	}
    } else
	realm->srvconfs = NULL;
    
    if (!list_push(realms, realm))
	debugx(1, DBG_ERR, "malloc failed");
    debug(DBG_DBG, "addrealm: added realm %s", value);
}

struct gconffile *openconfigfile(const char *file) {
    struct gconffile *cf = NULL;

    if (!pushgconffile(&cf, file))
	debugx(1, DBG_ERR, "could not read config file %s\n%s", file, strerror(errno));
    debug(DBG_DBG, "reading config file %s", file);
    return cf;
}

int addmatchcertattr(struct clsrvconf *conf, char *matchcertattr) {
    char *v;
    regex_t **r;
    
    if (strncasecmp(matchcertattr, "CN:/", 4)) {
	r = &conf->certcnregex;
	v = matchcertattr + 4;
    } else if (strncasecmp(matchcertattr, "SubjectAltName:URI:/", 20)) {
	r = &conf->certuriregex;
	v = matchcertattr + 20;
    } else
	return 0;
    if (!*v)
	return 0;
    /* regexp, remove optional trailing / if present */
    if (v[strlen(v) - 1] == '/')
	v[strlen(v) - 1] = '\0';
    if (!*v)
	return 0;

    *r = malloc(sizeof(regex_t));
    if (!*r) {
	debug(DBG_ERR, "malloc failed");
	return 0;
    }
    if (regcomp(*r, v, REG_ICASE | REG_NOSUB)) {
	free(*r);
	*r = NULL;
	debug(DBG_ERR, "failed to compile regular expression %s", v);
	return 0;
    }
    return 1;
}

int addrewriteattr(struct clsrvconf *conf, char *rewriteattr) {
    char *v, *w;
    
    v = rewriteattr + 11;
    if (strncasecmp(rewriteattr, "User-Name:/", 11) || !*v)
	return 0;
    /* regexp, remove optional trailing / if present */
    if (v[strlen(v) - 1] == '/')
	v[strlen(v) - 1] = '\0';

    w = strchr(v, '/');
    if (!*w)
	return 0;
    *w = '\0';
    w++;
    
    conf->rewriteattrregex = malloc(sizeof(regex_t));
    if (!conf->rewriteattrregex) {
	debug(DBG_ERR, "malloc failed");
	return 0;
    }

    conf->rewriteattrreplacement = stringcopy(w, 0);
    if (!conf->rewriteattrreplacement) {
	free(conf->rewriteattrregex);
	conf->rewriteattrregex = NULL;
	return 0;
    }
    
    if (regcomp(conf->rewriteattrregex, v, REG_ICASE | REG_EXTENDED)) {
	free(conf->rewriteattrregex);
	conf->rewriteattrregex = NULL;
	free(conf->rewriteattrreplacement);
	conf->rewriteattrreplacement = NULL;
	debug(DBG_ERR, "failed to compile regular expression %s", v);
	return 0;
    }

    return 1;
}

/* should accept both names and numeric values, only numeric right now */
uint8_t attrname2val(char *attrname) {
    int val = 0;
    
    val = atoi(attrname);
    return val > 0 && val < 256 ? val : 0;
}

/* should accept both names and numeric values, only numeric right now */
int vattrname2val(char *attrname, uint32_t *vendor, uint32_t *type) {
    char *s;
    
    *vendor = atoi(attrname);
    s = strchr(attrname, ':');
    if (!s) {
	*type = -1;
	return 1;
    }
    *type = atoi(s + 1);
    return *type >= 0 && *type < 256;
}

void rewritefree() {
    struct list_node *entry;
    struct rewriteconf *r;
    
    for (entry = list_first(rewriteconfs); entry; entry = list_next(entry)) {
	r = (struct rewriteconf *)entry->data;
	if (r->name)
	    free(r->name);
	if (!r->count)
	    free(r->rewrite);
    }
    list_destroy(rewriteconfs);
    rewriteconfs = NULL;
}

struct rewrite *getrewrite(char *alt1, char *alt2) {
    struct list_node *entry;
    struct rewriteconf *r, *r1 = NULL, *r2 = NULL;
    
    for (entry = list_first(rewriteconfs); entry; entry = list_next(entry)) {
	r = (struct rewriteconf *)entry->data;
	if (!strcasecmp(r->name, alt1)) {
	    r1 = r;
	    break;
	}
	if (!r2 && alt2 && !strcasecmp(r->name, alt2))
	    r2 = r;
    }

    r = (r1 ? r1 : r2);
    if (!r)
	return NULL;
    r->count++;
    return r->rewrite;
}

void addrewrite(char *value, char **attrs, char **vattrs) {
    struct rewriteconf *new;
    struct rewrite *rewrite = NULL;
    int i, n;
    uint8_t *a = NULL;
    uint32_t *p, *va = NULL;

    if (attrs) {
	n = 0;
	for (; attrs[n]; n++);
	a = malloc((n + 1) * sizeof(uint8_t));
	if (!a)
	    debugx(1, DBG_ERR, "malloc failed");
    
	for (i = 0; i < n; i++)
	    if (!(a[i] = attrname2val(attrs[i])))
		debugx(1, DBG_ERR, "addrewrite: invalid attribute %s", attrs[i]);
	a[i] = 0;
    }
    
    if (vattrs) {
	n = 0;
	for (; vattrs[n]; n++);
	va = malloc((2 * n + 1) * sizeof(uint32_t));
	if (!va)
	    debugx(1, DBG_ERR, "malloc failed");
    
	for (p = va, i = 0; i < n; i++, p += 2)
	    if (!vattrname2val(vattrs[i], p, p + 1))
		debugx(1, DBG_ERR, "addrewrite: invalid vendor attribute %s", vattrs[i]);
	*p = 0;
    }
    
    if (a || va) {
	rewrite = malloc(sizeof(struct rewrite));
	if (!rewrite)
	    debugx(1, DBG_ERR, "malloc failed");
	rewrite->removeattrs = a;
	rewrite->removevendorattrs = va;
    }
    
    new = malloc(sizeof(struct rewriteconf));
    if (!new || !list_push(rewriteconfs, new))
	debugx(1, DBG_ERR, "malloc failed");

    memset(new, 0, sizeof(struct rewriteconf));
    new->name = stringcopy(value, 0);
    if (!new->name)
	debugx(1, DBG_ERR, "malloc failed");
	
    new->rewrite = rewrite;
    debug(DBG_DBG, "addrewrite: added rewrite block %s", value);
}

void confclient_cb(struct gconffile **cf, char *block, char *opt, char *val) {
    char *type = NULL, *tls = NULL, *matchcertattr = NULL, *rewrite = NULL, *rewriteattr = NULL;
    struct clsrvconf *conf;
    
    debug(DBG_DBG, "confclient_cb called for %s", block);

    conf = malloc(sizeof(struct clsrvconf));
    if (!conf || !list_push(clconfs, conf))
	debugx(1, DBG_ERR, "malloc failed");
    memset(conf, 0, sizeof(struct clsrvconf));
    
    getgenericconfig(cf, block,
		     "type", CONF_STR, &type,
		     "host", CONF_STR, &conf->host,
		     "secret", CONF_STR, &conf->secret,
		     "tls", CONF_STR, &tls,
		     "matchcertificateattribute", CONF_STR, &matchcertattr,
		     "rewrite", CONF_STR, &rewrite,
		     "rewriteattribute", CONF_STR, &rewriteattr,
		     NULL
		     );

    /* leave conf->name to be NULL for clients */
    if (!conf->host)
	conf->host = stringcopy(val, 0);
    
    if (type && !strcasecmp(type, "udp")) {
	conf->type = 'U';
	client_udp_count++;
    } else if (type && !strcasecmp(type, "tls")) {
	conf->ssl_ctx = tls ? tlsgetctx(tls, NULL) : tlsgetctx("defaultclient", "default");
	if (!conf->ssl_ctx)
	    debugx(1, DBG_ERR, "error in block %s, no tls context defined", block);
	if (matchcertattr && !addmatchcertattr(conf, matchcertattr))
	    debugx(1, DBG_ERR, "error in block %s, invalid MatchCertificateAttributeValue", block);
	conf->type = 'T';
	client_tls_count++;
    } else
	debugx(1, DBG_ERR, "error in block %s, type must be set to UDP or TLS", block);
    free(type);
    if (tls)
	free(tls);
    if (matchcertattr)
	free(matchcertattr);
    
    conf->rewrite = rewrite ? getrewrite(rewrite, NULL) : getrewrite("defaultclient", "default");
    
    if (rewriteattr) {
	if (!addrewriteattr(conf, rewriteattr))
	    debugx(1, DBG_ERR, "error in block %s, invalid RewriteAttributeValue", block);
	free(rewriteattr);
    }
    
    if (!resolvepeer(conf, 0))
	debugx(1, DBG_ERR, "failed to resolve host %s port %s, exiting", conf->host ? conf->host : "(null)", conf->port ? conf->port : "(null)");
    
    if (!conf->secret) {
	if (conf->type == 'U')
	    debugx(1, DBG_ERR, "error in block %s, secret must be specified for UDP", block);
	conf->secret = stringcopy(DEFAULT_TLS_SECRET, 0);
    }
}

void confserver_cb(struct gconffile **cf, char *block, char *opt, char *val) {
    char *type = NULL, *tls = NULL, *matchcertattr = NULL, *statusserver = NULL, *rewrite = NULL;
    struct clsrvconf *conf;
    
    debug(DBG_DBG, "confserver_cb called for %s", block);

    conf = malloc(sizeof(struct clsrvconf));
    if (!conf || !list_push(srvconfs, conf))
	debugx(1, DBG_ERR, "malloc failed");
    memset(conf, 0, sizeof(struct clsrvconf));
    
    getgenericconfig(cf, block,
		     "type", CONF_STR, &type,
		     "host", CONF_STR, &conf->host,
		     "port", CONF_STR, &conf->port,
		     "secret", CONF_STR, &conf->secret,
		     "tls", CONF_STR, &tls,
		     "matchcertificateattribute", CONF_STR, &matchcertattr,
		     "rewrite", CONF_STR, &rewrite,
		     "StatusServer", CONF_STR, &statusserver,
		     NULL
		     );

    conf->name = stringcopy(val, 0);
    if (!conf->host)
	conf->host = stringcopy(val, 0);
    
    if (type && !strcasecmp(type, "udp")) {
	conf->type = 'U';
	server_udp_count++;
	if (!conf->port)
	    conf->port = stringcopy(DEFAULT_UDP_PORT, 0);
    } else if (type && !strcasecmp(type, "tls")) {
	conf->ssl_ctx = tls ? tlsgetctx(tls, NULL) : tlsgetctx("defaultserver", "default");
	if (!conf->ssl_ctx)
	    debugx(1, DBG_ERR, "error in block %s, no tls context defined", block);
	if (matchcertattr && !addmatchcertattr(conf, matchcertattr))
	    debugx(1, DBG_ERR, "error in block %s, invalid MatchCertificateAttributeValue", block);
	if (!conf->port)
	    conf->port = stringcopy(DEFAULT_TLS_PORT, 0);
	conf->type = 'T';
	server_tls_count++;
    } else
	debugx(1, DBG_ERR, "error in block %s, type must be set to UDP or TLS", block);
    free(type);
    if (tls)
	free(tls);
    if (matchcertattr)
	free(matchcertattr);
    
    conf->rewrite = rewrite ? getrewrite(rewrite, NULL) : getrewrite("defaultserver", "default");
    
    if (!resolvepeer(conf, 0))
	debugx(1, DBG_ERR, "failed to resolve host %s port %s, exiting", conf->host ? conf->host : "(null)", conf->port ? conf->port : "(null)");
    
    if (!conf->secret) {
	if (conf->type == 'U')
	    debugx(1, DBG_ERR, "error in block %s, secret must be specified for UDP", block);
	conf->secret = stringcopy(DEFAULT_TLS_SECRET, 0);
    }
    
    if (statusserver) {
	if (!strcasecmp(statusserver, "on"))
	    conf->statusserver = 1;
	else if (strcasecmp(statusserver, "off"))
	    debugx(1, DBG_ERR, "error in block %s, StatusServer is %s, must be on or off", block, statusserver);
	free(statusserver);
    }
}

void confrealm_cb(struct gconffile **cf, char *block, char *opt, char *val) {
    char **servers = NULL, *msg = NULL;
    
    debug(DBG_DBG, "confrealm_cb called for %s", block);
    
    getgenericconfig(cf, block,
		     "server", CONF_MSTR, &servers,
		     "ReplyMessage", CONF_STR, &msg,
		     NULL
		     );

    addrealm(val, servers, msg);
    free(servers);
}

void conftls_cb(struct gconffile **cf, char *block, char *opt, char *val) {
    char *cacertfile = NULL, *cacertpath = NULL, *certfile = NULL, *certkeyfile = NULL, *certkeypwd = NULL;
    
    debug(DBG_DBG, "conftls_cb called for %s", block);
    
    getgenericconfig(cf, block,
		     "CACertificateFile", CONF_STR, &cacertfile,
		     "CACertificatePath", CONF_STR, &cacertpath,
		     "CertificateFile", CONF_STR, &certfile,
		     "CertificateKeyFile", CONF_STR, &certkeyfile,
		     "CertificateKeyPassword", CONF_STR, &certkeypwd,
		     NULL
		     );
    
    tlsadd(val, cacertfile, cacertpath, certfile, certkeyfile, certkeypwd);
    free(cacertfile);
    free(cacertpath);
    free(certfile);
    free(certkeyfile);
    free(certkeypwd);
}

void confrewrite_cb(struct gconffile **cf, char *block, char *opt, char *val) {
    char **attrs = NULL, **vattrs = NULL;
    
    debug(DBG_DBG, "confrewrite_cb called for %s", block);
    
    getgenericconfig(cf, block,
		     "removeAttribute", CONF_MSTR, &attrs,
		     "removeVendorAttribute", CONF_MSTR, &vattrs,
		     NULL
		     );
    addrewrite(val, attrs, vattrs);
    free(attrs);
    free(vattrs);
}

void getmainconfig(const char *configfile) {
    char *loglevel = NULL;
    struct gconffile *cfs;

    cfs = openconfigfile(configfile);
    memset(&options, 0, sizeof(options));
    
    clconfs = list_create();
    if (!clconfs)
	debugx(1, DBG_ERR, "malloc failed");
    
    srvconfs = list_create();
    if (!srvconfs)
	debugx(1, DBG_ERR, "malloc failed");
    
    realms = list_create();
    if (!realms)
	debugx(1, DBG_ERR, "malloc failed");    
 
    tlsconfs = list_create();
    if (!tlsconfs)
	debugx(1, DBG_ERR, "malloc failed");
    
    rewriteconfs = list_create();
    if (!rewriteconfs)
	debugx(1, DBG_ERR, "malloc failed");    
 
    getgenericconfig(&cfs, NULL,
		     "ListenUDP", CONF_STR, &options.listenudp,
		     "ListenTCP", CONF_STR, &options.listentcp,
		     "ListenAccountingUDP", CONF_STR, &options.listenaccudp,
		     "SourceUDP", CONF_STR, &options.sourceudp,
		     "SourceTCP", CONF_STR, &options.sourcetcp,
		     "LogLevel", CONF_STR, &loglevel,
		     "LogDestination", CONF_STR, &options.logdestination,
		     "Client", CONF_CBK, confclient_cb,
		     "Server", CONF_CBK, confserver_cb,
		     "Realm", CONF_CBK, confrealm_cb,
		     "TLS", CONF_CBK, conftls_cb,
		     "Rewrite", CONF_CBK, confrewrite_cb,
		     NULL
		     );
    tlsfree();
    rewritefree();
    
    if (loglevel) {
	if (strlen(loglevel) != 1 || *loglevel < '1' || *loglevel > '4')
	    debugx(1, DBG_ERR, "error in %s, value of option LogLevel is %s, must be 1, 2, 3 or 4", configfile, loglevel);
	options.loglevel = *loglevel - '0';
	free(loglevel);
    }
}

void getargs(int argc, char **argv, uint8_t *foreground, uint8_t *pretend, uint8_t *loglevel, char **configfile) {
    int c;

    while ((c = getopt(argc, argv, "c:d:fpv")) != -1) {
	switch (c) {
	case 'c':
	    *configfile = optarg;
	    break;
	case 'd':
	    if (strlen(optarg) != 1 || *optarg < '1' || *optarg > '4')
		debugx(1, DBG_ERR, "Debug level must be 1, 2, 3 or 4, not %s", optarg);
	    *loglevel = *optarg - '0';
	    break;
	case 'f':
	    *foreground = 1;
	    break;
	case 'p':
	    *pretend = 1;
	    break;
	case 'v':
		debugx(0, DBG_ERR, "radsecproxy revision $Rev$");
	default:
	    goto usage;
	}
    }
    if (!(argc - optind))
	return;

 usage:
    debug(DBG_ERR, "Usage:\n%s [ -c configfile ] [ -d debuglevel ] [ -f ] [ -p ] [ -v ]", argv[0]);
    exit(1);
}

#ifdef SYS_SOLARIS9
int daemon(int a, int b) {
    int i;

    if (fork())
	exit(0);

    setsid();

    for (i = 0; i < 3; i++) {
	close(i);
	open("/dev/null", O_RDWR);
    }
    return 1;
}
#endif

void *sighandler(void *arg) {
    sigset_t sigset;
    int sig;

    for(;;) {
	sigemptyset(&sigset);
	sigaddset(&sigset, SIGPIPE);
	sigwait(&sigset, &sig);
	/* only get SIGPIPE right now, so could simplify below code */
        switch (sig) {
        case 0:
            /* completely ignoring this */
            break;
        case SIGPIPE:
            debug(DBG_WARN, "sighandler: got SIGPIPE, TLS write error?");
            break;
        default:
            debug(DBG_WARN, "sighandler: ignoring signal %d", sig);
        }
    }
}

int main(int argc, char **argv) {
    pthread_t sigth, udpserverth, udpaccserverth, udpclient4rdth, udpclient6rdth;
    sigset_t sigset;
    struct list_node *entry;
    uint8_t foreground = 0, pretend = 0, loglevel = 0;
    char *configfile = NULL;
    
    debug_init("radsecproxy");
    debug_set_level(DEBUG_LEVEL);
    getargs(argc, argv, &foreground, &pretend, &loglevel, &configfile);
    if (loglevel)
	debug_set_level(loglevel);
    getmainconfig(configfile ? configfile : CONFIG_MAIN);
    if (loglevel)
	options.loglevel = loglevel;
    else if (options.loglevel)
	debug_set_level(options.loglevel);
    if (foreground)
	options.logdestination = NULL;
    else {
	if (!options.logdestination)
	    options.logdestination = "x-syslog:///";
	debug_set_destination(options.logdestination);
    }

    if (pretend)
	debugx(0, DBG_ERR, "All OK so far; exiting since only pretending");
    
    if (!list_first(clconfs))
	debugx(1, DBG_ERR, "No clients configured, nothing to do, exiting");
    if (!list_first(srvconfs))
	debugx(1, DBG_ERR, "No servers configured, nothing to do, exiting");
    if (!list_first(realms))
	debugx(1, DBG_ERR, "No realms configured, nothing to do, exiting");

    if (!foreground && (daemon(0, 0) < 0))
	debugx(1, DBG_ERR, "daemon() failed: %s", strerror(errno));
    
    debug(DBG_INFO, "radsecproxy revision $Rev$ starting");

    sigemptyset(&sigset);
    /* exit on all but SIGPIPE, ignore more? */
    sigaddset(&sigset, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &sigset, NULL);
    pthread_create(&sigth, NULL, sighandler, NULL);
    
    if (client_udp_count) {
	udp_server_replyq = newreplyq();
	if (pthread_create(&udpserverth, NULL, udpserverrd, NULL))
	    debugx(1, DBG_ERR, "pthread_create failed");
	if (options.listenaccudp)
	    if (pthread_create(&udpaccserverth, NULL, udpaccserverrd, NULL))
		debugx(1, DBG_ERR, "pthread_create failed");
    }
    
    for (entry = list_first(srvconfs); entry; entry = list_next(entry)) {
	addserver((struct clsrvconf *)entry->data);
	if (pthread_create(&((struct clsrvconf *)entry->data)->servers->clientth, NULL, clientwr,
			   (void *)((struct clsrvconf *)entry->data)->servers))
	    debugx(1, DBG_ERR, "pthread_create failed");
    }
    /* srcudpres no longer needed, while srctcpres is needed later */
    if (srcudpres) {
	free(srcudpres);
	srcudpres = NULL;
    }
    if (udp_client4_sock >= 0)
	if (pthread_create(&udpclient4rdth, NULL, udpclientrd, (void *)&udp_client4_sock))
	    debugx(1, DBG_ERR, "clientwr: pthread_create failed");
    if (udp_client6_sock >= 0)
	if (pthread_create(&udpclient6rdth, NULL, udpclientrd, (void *)&udp_client6_sock))
	    debugx(1, DBG_ERR, "clientwr: pthread_create failed");
    
    if (client_tls_count)
	return tlslistener();
    
    /* just hang around doing nothing, anything to do here? */
    for (;;)
	sleep(1000);
}

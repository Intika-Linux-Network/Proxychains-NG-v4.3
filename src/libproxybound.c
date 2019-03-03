/***************************************************************************
                           libproxybound.c
                           ---------------

     copyright: intika      (C) 2019 intika@librefox.org
     copyright: rofl0r      (C) 2012 https://github.com/rofl0r
     copyright: haad        (C) 2012 https://github.com/haad
     copyright: netcreature (C) 2002 netcreature@users.sourceforge.net
    
 ***************************************************************************
 *   GPL                                                                   *
 ***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#undef _GNU_SOURCE
#define _GNU_SOURCE

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <netdb.h>

#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <dlfcn.h>

#include "core.h"
#include "common.h"

#define     satosin(x)      ((struct sockaddr_in *) &(x))
#define     SOCKADDR(x)     (satosin(x)->sin_addr.s_addr)
#define     SOCKADDR_2(x)     (satosin(x)->sin_addr)
#define     SOCKPORT(x)     (satosin(x)->sin_port)
#define     SOCKFAMILY(x)     (satosin(x)->sin_family)
#define     MAX_CHAIN 512

connect_t true_connect;
gethostbyname_t true_gethostbyname;
getaddrinfo_t true_getaddrinfo;
freeaddrinfo_t true_freeaddrinfo;
getnameinfo_t true_getnameinfo;
gethostbyaddr_t true_gethostbyaddr;

send_t true_send;
sendto_t true_sendto;
sendmsg_t true_sendmsg;

int tcp_read_time_out;
int tcp_connect_time_out;
chain_type proxybound_ct;
proxy_data proxybound_pd[MAX_CHAIN];
unsigned int proxybound_proxy_count = 0;
int proxybound_got_chain_data = 0;
unsigned int proxybound_max_chain = 1;
int proxybound_quiet_mode = 0;
int proxybound_allow_leak = 0;
int proxybound_resolver = 0;
localaddr_arg localnet_addr[MAX_LOCALNET];
size_t num_localnet_addr = 0;
unsigned int remote_dns_subnet = 224;
#ifdef THREAD_SAFE
pthread_once_t init_once = PTHREAD_ONCE_INIT;
#endif

static int init_l = 0;

static void init_additional_settings(chain_type *ct);

static inline void get_chain_data(proxy_data * pd, unsigned int *proxy_count, chain_type * ct);

static void manual_socks5_env(proxy_data * pd, unsigned int *proxy_count, chain_type * ct);

static void* load_sym(char* symname, void* proxyfunc) {

	void *funcptr = dlsym(RTLD_NEXT, symname);
	
	if(!funcptr) {
		fprintf(stderr, "Cannot load symbol '%s' %s\n", symname, dlerror());
		exit(1);
	} else {
		PDEBUG("loaded symbol '%s'" " real addr %p  wrapped addr %p\n", symname, funcptr, proxyfunc);
	}
	if(funcptr == proxyfunc) {
		PDEBUG("circular reference detected, aborting!\n");
		abort();
	}
	return funcptr;
}

#define INIT() init_lib_wrapper(__FUNCTION__)

#define SETUP_SYM(X) do { true_ ## X = load_sym( # X, X ); } while(0)

static void do_init(void) {
	MUTEX_INIT(&internal_ips_lock, NULL);
	MUTEX_INIT(&hostdb_lock, NULL);
    
    /* check for simple SOCKS5 proxy setup */
	manual_socks5_env(proxybound_pd, &proxybound_proxy_count, &proxybound_ct);
    
	/* read the config file */
	get_chain_data(proxybound_pd, &proxybound_proxy_count, &proxybound_ct);

	proxybound_write_log(LOG_PREFIX "DLL init\n");
	
	SETUP_SYM(connect);
	SETUP_SYM(gethostbyname);
	SETUP_SYM(getaddrinfo);
	SETUP_SYM(freeaddrinfo);
	SETUP_SYM(gethostbyaddr);
	SETUP_SYM(getnameinfo);
    
	//SETUP_SYM(send);
	//SETUP_SYM(sendto);
	//SETUP_SYM(sendmsg);
	
	init_l = 1;
}

static void init_lib_wrapper(const char* caller) {
#ifndef DEBUG
	(void) caller;
#endif
#ifndef THREAD_SAFE
	if(init_l) return;
	PDEBUG("%s called from %s\n", __FUNCTION__,  caller);
	do_init();
#else
	if(!init_l) PDEBUG("%s called from %s\n", __FUNCTION__,  caller);
	pthread_once(&init_once, do_init);
#endif
}

/* if we use gcc >= 3, we can instruct the dynamic loader 
 * to call init_lib at link time. otherwise it gets loaded
 * lazily, which has the disadvantage that there's a potential
 * race condition if 2 threads call it before init_l is set 
 * and PTHREAD support was disabled */
#if __GNUC__ > 2
__attribute__((constructor))
static void gcc_init(void) {
	INIT();
}
#endif

static void init_additional_settings(chain_type *ct) {
	char *env;

	tcp_read_time_out = 4 * 1000;
	tcp_connect_time_out = 10 * 1000;
	*ct = DYNAMIC_TYPE;
    
	env = getenv(PROXYBOUND_ALLOW_LEAKS_ENV_VAR);
	if(env && *env == '1')
		proxybound_allow_leak = 1;

	env = getenv(PROXYBOUND_QUIET_MODE_ENV_VAR);
	if(env && *env == '1')
		proxybound_quiet_mode = 1;
}

/* get configuration from config file */
static void get_chain_data(proxy_data * pd, unsigned int *proxy_count, chain_type * ct) {
	int count = 0, port_n = 0, list = 0;
	char buff[1024], type[1024], host[1024], user[1024];
	char *env;
	char local_in_addr_port[32];
	char local_in_addr[32], local_in_port[32], local_netmask[32];
	FILE *file = NULL;
    
    //printf("ssssssssss\n");
    //dprintf("sssszeezesssssssss\n");

	if(proxybound_got_chain_data)
		return;

	//Some defaults
    init_additional_settings(ct);
	
	env = get_config_path(getenv(PROXYBOUND_CONF_FILE_ENV_VAR), buff, sizeof(buff));
	file = fopen(env, "r");

	while(fgets(buff, sizeof(buff), file)) {
		if(buff[0] != '\n' && buff[strspn(buff, " ")] != '#') {
			/* proxylist has to come last */
			if(list) {
				if(count >= MAX_CHAIN)
					break;
				
				memset(&pd[count], 0, sizeof(proxy_data));

				pd[count].ps = PLAY_STATE;
				port_n = 0;

				sscanf(buff, "%s %s %d %s %s", type, host, &port_n, pd[count].user, pd[count].pass);

				pd[count].ip.as_int = (uint32_t) inet_addr(host);
				pd[count].port = htons((unsigned short) port_n);

				if(!strcmp(type, "http")) {
					pd[count].pt = HTTP_TYPE;
				} else if(!strcmp(type, "socks4")) {
					pd[count].pt = SOCKS4_TYPE;
				} else if(!strcmp(type, "socks5")) {
					pd[count].pt = SOCKS5_TYPE;
				} else
					continue;

				if(pd[count].ip.as_int && port_n && pd[count].ip.as_int != (uint32_t) - 1)
					count++;
			} else {
				if(strstr(buff, "[ProxyList]")) {
					list = 1;
				} else if(strstr(buff, "random_chain")) {
					*ct = RANDOM_TYPE;
				} else if(strstr(buff, "strict_chain")) {
					*ct = STRICT_TYPE;
				} else if(strstr(buff, "dynamic_chain")) {
					*ct = DYNAMIC_TYPE;
				} else if(strstr(buff, "tcp_read_time_out")) {
					sscanf(buff, "%s %d", user, &tcp_read_time_out);
				} else if(strstr(buff, "tcp_connect_time_out")) {
					sscanf(buff, "%s %d", user, &tcp_connect_time_out);
				} else if(strstr(buff, "remote_dns_subnet")) {
					sscanf(buff, "%s %d", user, &remote_dns_subnet);
					if(remote_dns_subnet >= 256) {
						fprintf(stderr,
							"remote_dns_subnet: invalid value. requires a number between 0 and 255.\n");
						exit(1);
					}
				} else if(strstr(buff, "localnet")) {
					if(sscanf(buff, "%s %21[^/]/%15s", user, local_in_addr_port, local_netmask) < 3) {
						fprintf(stderr, "localnet format error");
						exit(1);
					}
					/* clean previously used buffer */
					memset(local_in_port, 0, sizeof(local_in_port) / sizeof(local_in_port[0]));

					if(sscanf(local_in_addr_port, "%15[^:]:%5s", local_in_addr, local_in_port) < 2) {
						PDEBUG("added localnet: netaddr=%s, netmask=%s\n",
						       local_in_addr, local_netmask);
					} else {
						PDEBUG("added localnet: netaddr=%s, port=%s, netmask=%s\n",
						       local_in_addr, local_in_port, local_netmask);
					}
					if(num_localnet_addr < MAX_LOCALNET) {
						int error;
						error =
						    inet_pton(AF_INET, local_in_addr,
							      &localnet_addr[num_localnet_addr].in_addr);
						if(error <= 0) {
							fprintf(stderr, "localnet address error\n");
							exit(1);
						}
						error =
						    inet_pton(AF_INET, local_netmask,
							      &localnet_addr[num_localnet_addr].netmask);
						if(error <= 0) {
							fprintf(stderr, "localnet netmask error\n");
							exit(1);
						}
						if(local_in_port[0]) {
							localnet_addr[num_localnet_addr].port =
							    (short) atoi(local_in_port);
						} else {
							localnet_addr[num_localnet_addr].port = 0;
						}
						++num_localnet_addr;
					} else {
						fprintf(stderr, "# of localnet exceed %d.\n", MAX_LOCALNET);
					}
				} else if(strstr(buff, "chain_len")) {
					char *pc;
					int len;
					pc = strchr(buff, '=');
					len = atoi(++pc);
					proxybound_max_chain = (len ? len : 1);
				} else if(strstr(buff, "quiet_mode")) {
					proxybound_quiet_mode = 1;
				} else if(strstr(buff, "proxy_dns")) {
					proxybound_resolver = 1;
				}
			}
		}
	}
	fclose(file);
	*proxy_count = count;
	proxybound_got_chain_data = 1;
}

static void manual_socks5_env(proxy_data *pd, unsigned int *proxy_count, chain_type *ct) {
	char *port_string;
    char *host_string;

	if(proxybound_got_chain_data)
		return;

	init_additional_settings(ct);

    port_string = getenv(PROXYBOUND_SOCKS5_PORT_ENV_VAR);
	if(!port_string)
		return;
    
    host_string = getenv(PROXYBOUND_SOCKS5_HOST_ENV_VAR);
    if(!host_string)
        host_string = "127.0.0.1";

	memset(pd, 0, sizeof(proxy_data));

	pd[0].ps = PLAY_STATE;
	pd[0].ip.as_int = (uint32_t) inet_addr(host_string);
	pd[0].port = htons((unsigned short) strtol(port_string, NULL, 0));
	pd[0].pt = SOCKS5_TYPE;
	proxybound_max_chain = 1;

	if(getenv(PROXYBOUND_FORCE_DNS_ENV_VAR) && (*getenv(PROXYBOUND_FORCE_DNS_ENV_VAR) == '1'))
		proxybound_resolver = 1;

	*proxy_count = 1;
	proxybound_got_chain_data = 1;
}

/**************************************************************************************************************************************************************/
/*******  HOOK FUNCTIONS  *************************************************************************************************************************************/

int connect(int sock, const struct sockaddr *addr, socklen_t len) {
    
    PDEBUG("\n\n\n\n\n\n\n\n...INJECT... \n\n\n\n\n\n\n\n");

    int socktype = 0, flags = 0, ret = 0;
    socklen_t optlen = 0;
    ip_type dest_ip;
    char ip[256];
    struct in_addr *p_addr_in;
    unsigned short port;
    size_t i;
    int remote_dns_connect = 0;
    INIT();
    optlen = sizeof(socktype);
    getsockopt(sock, SOL_SOCKET, SO_TYPE, &socktype, &optlen);

    // Sock family list (not complete) 
    // AF_UNIX_CCSID    /*     - Unix domain sockets 		*/
    // AF_UNIX          /*  1  - Unix domain sockets        */
    // AF_INET          /*  2  - Internet IP Protocol 	    */
    // AF_INET6         /*  10 - IPv6                       */
    // AF_UNSPEC	    /*  0                               */
    // AF_AX25			/*  3  - Amateur Radio AX.25 		*/
    // AF_IPX		    /*  4  - Novell IPX 			    */
    // AF_APPLETALK		/*  5  - Appletalk DDP 	           	*/
    // AF_NETROM		/*  6  - Amateur radio NetROM 	    */
    // AF_BRIDGE		/*  7  - Multiprotocol bridge 	    */
    // AF_AAL5			/*  8  - Reserved for Werner's ATM 	*/
    // AF_X25			/*  9  - Reserved for X.25 project 	*/
    // AF_MAX			/*  12 - For now..                  */
    // Etc. 

    //Allow direct unix
    if (SOCKFAMILY(*addr) == AF_UNIX) {
        PDEBUG("allowing direct unix connect()\n\n");
        return true_connect(sock, addr, len);
    }

    p_addr_in = &((struct sockaddr_in *) addr)->sin_addr;
    port = ntohs(((struct sockaddr_in *) addr)->sin_port);
    inet_ntop(AF_INET, p_addr_in, ip, sizeof(ip));

    #ifdef DEBUG
    //PDEBUG("localnet: %s; ", inet_ntop(AF_INET, &in_addr_localnet, ip, sizeof(ip)));
    //PDEBUG("netmask: %s; " , inet_ntop(AF_INET, &in_addr_netmask, ip, sizeof(ip)));
    PDEBUG("target: %s\n\n", ip);
    PDEBUG("port: %d\n\n", port);
    #endif

    //Allow direct local 127.x.x.x
    if ((ip[0] == '1') && (ip[1] == '2') && (ip[2] == '7') && (ip[3] == '.')) {
        PDEBUG("Local ip detected... ignoring\n\n");
        return true_connect(sock, addr, len);
    }

    //Allow empty ip
    /*if (!ip[0]) {
        PDEBUG("Noip... ignoring\n\n");
        return true_connect(sock, addr, len);
    }*/

    //Block other sock 
    if (SOCKFAMILY(*addr) != AF_INET) {
        if (proxybound_allow_leak) {
            PDEBUG("allowing direct non tcp connect()\n\n");
            return true_connect(sock, addr, len);
        } else {
            PDEBUG("blocking direct non tcp connect() \n\n");
            return -1;
        }
    }

    //Block udp 
    if (socktype != SOCK_STREAM) {
        if (proxybound_allow_leak) {
            PDEBUG("allowing direct udp connect()\n\n");
            return true_connect(sock, addr, len);
        } else {
            PDEBUG("blocking direct udp connect() \n\n");
            return -1;
        }
    }

    //Block udp 
    if (socktype == SOCK_DGRAM){
        if (proxybound_allow_leak) {
            PDEBUG("allowing direct udp connect()\n\n");
            return true_connect(sock, addr, len);
        } else {
            PDEBUG("blocking direct udp connect() \n\n");
            return -1;
        }
    }

	// Check if connect called from proxydns
    remote_dns_connect = (ntohl(p_addr_in->s_addr) >> 24 == remote_dns_subnet);

	for(i = 0; i < num_localnet_addr && !remote_dns_connect; i++) {
		if((localnet_addr[i].in_addr.s_addr & localnet_addr[i].netmask.s_addr) == (p_addr_in->s_addr & localnet_addr[i].netmask.s_addr)) {
			if(!localnet_addr[i].port || localnet_addr[i].port == port) {
				PDEBUG("accessing localnet using true_connect\n\n");
				return true_connect(sock, addr, len);
			}
		}
	}

	flags = fcntl(sock, F_GETFL, 0);
	if(flags & O_NONBLOCK)
		fcntl(sock, F_SETFL, !O_NONBLOCK);

	dest_ip.as_int = SOCKADDR(*addr);

	ret = connect_proxy_chain(sock, dest_ip, SOCKPORT(*addr), proxybound_pd, proxybound_proxy_count, proxybound_ct, proxybound_max_chain);

	fcntl(sock, F_SETFL, flags);
	if(ret != SUCCESS)
		errno = ECONNREFUSED;
	return ret;
}
/*
ssize_t sendto(int sockfd, const void *buf, size_t len, int flags, const struct sockaddr *dest_addr, socklen_t addrlen) {
    return true_sendto(sockfd, buf, len, flags, *dest_addr, addrlen);
    //return 0;
}

ssize_t send(int sockfd, const void *buf, size_t len, int flags) {
    return true_send(sockfd, buf, len, flags);
    //return 0;  
}


ssize_t sendmsg(int sockfd, const struct msghdr *msg, int flags) {
    return true_sendmsg(sockfd, msg, flags);
    //return 0;  
}*/

//ssize_t send(int sockfd, const void *buf, size_t len, int flags) {}

//TODO: DNS LEAK: OTHER RESOLVER FUNCTION
//=======================================
//realresinit = dlsym(lib, "res_init");
//realresquery = dlsym(lib, "res_query");
//realressend = dlsym(lib, "res_send");
//realresquerydomain = dlsym(lib, "res_querydomain");
//realressearch = dlsym(lib, "res_search");
//realgethostbyaddr = dlsym(lib, "gethostbyaddr"); //Needs rewrite
//realgetipnodebyname = dlsym(lib, "getipnodebyname");

//UDP & DNS LEAK
//==============
//realsendto = dlsym(lib, "sendto");
//realsendmsg = dlsym(lib, "sendmsg");

static struct gethostbyname_data ghbndata;

struct hostent *gethostbyname(const char *name) {
	INIT();

	PDEBUG("gethostbyname: %s\n", name);

	if(proxybound_resolver)
		return proxy_gethostbyname(name, &ghbndata);
	else
		return true_gethostbyname(name);

	return NULL;
}

int getaddrinfo(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **res) {
	int ret = 0;

	INIT();

	PDEBUG("getaddrinfo: %s %s\n", node, service);

	if(proxybound_resolver)
		ret = proxy_getaddrinfo(node, service, hints, res);
	else
		ret = true_getaddrinfo(node, service, hints, res);

	return ret;
}

void freeaddrinfo(struct addrinfo *res) {
	INIT();

	PDEBUG("freeaddrinfo %p \n", res);

	if(!proxybound_resolver)
		true_freeaddrinfo(res);
	else
		proxy_freeaddrinfo(res);
	return;
}

int getnameinfo(const struct sockaddr *sa, socklen_t salen, char *host, socklen_t hostlen, char *serv, socklen_t servlen, int flags) {
	char ip_buf[16];
	int ret = 0;

	INIT();
	
	PDEBUG("getnameinfo: %s %s\n", host, serv);

	if(!proxybound_resolver) {
		ret = true_getnameinfo(sa, salen, host, hostlen, serv, servlen, flags);
	} else {
		if(hostlen) {
			pc_stringfromipv4((unsigned char*) &(SOCKADDR_2(*sa)), ip_buf);
			strncpy(host, ip_buf, hostlen);
		}
		if(servlen)
			snprintf(serv, servlen, "%d", ntohs(SOCKPORT(*sa)));
	}
	return ret;
}

struct hostent *gethostbyaddr(const void *addr, socklen_t len, int type) {
	static char buf[16];
	static char ipv4[4];
	static char *list[2];
	static char *aliases[1];
	static struct hostent he;

	INIT();

	PDEBUG("TODO: proper gethostbyaddr hook\n");

	if(!proxybound_resolver)
		return true_gethostbyaddr(addr, len, type);
	else {

		PDEBUG("len %u\n", len);
		if(len != 4)
			return NULL;
		he.h_name = buf;
		memcpy(ipv4, addr, 4);
		list[0] = ipv4;
		list[1] = NULL;
		he.h_addr_list = list;
		he.h_addrtype = AF_INET;
		aliases[0] = NULL;
		he.h_aliases = aliases;
		he.h_length = 4;
		pc_stringfromipv4((unsigned char *) addr, buf);
		return &he;
	}
	return NULL;
}

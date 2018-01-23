/*
 * Linux /proc/net/dev metrics cluster
 *
 * Copyright (c) 2013,2018 Red Hat.
 * Copyright (c) 1995,2005 Silicon Graphics, Inc.  All Rights Reserved.
 * 
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 * 
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 */

typedef struct {
    uint32_t	mtu;
    uint32_t	speed;
    uint32_t	type;
    uint8_t	duplex;
    uint8_t	linkup;
    uint8_t	running;
    uint8_t	wireless;
} net_dev_t;

#define HWADDRSTRLEN 64

typedef struct {
    int		has_inet : 1;
    int		has_ipv6 : 1;
    int		has_hw   : 1;
    int		padding : 13;
    uint16_t	ipv6scope;
    char	inet[INET_ADDRSTRLEN];
    char	ipv6[INET6_ADDRSTRLEN+16];	/* extra for /plen */
    char	hw_addr[HWADDRSTRLEN];
} net_addr_t;

#define PROC_DEV_COUNTERS_PER_LINE   16

typedef struct {
    uint64_t	counters[PROC_DEV_COUNTERS_PER_LINE];
    net_dev_t	ioc;
} net_interface_t;

#ifndef ETHTOOL_GSET
#define ETHTOOL_GSET	0x1
#endif

#ifndef SIOCGIFCONF
#define SIOCGIFCONF	0x8912
#endif

#ifndef SIOCGIFFLAGS
#define SIOCGIFFLAGS	0x8913
#endif

#ifndef SIOCGIFADDR
#define SIOCGIFADDR	0x8915
#endif

#ifndef SIOCGIFMTU
#define SIOCGIFMTU	0x8921
#endif

#ifndef SIOCETHTOOL
#define SIOCETHTOOL	0x8946
#endif

#ifndef SIOCGIWRATE
#define SIOCGIWRATE	0x8B21
#endif

/* ioctl(SIOCIFETHTOOL) GSET ("get settings") structure */
struct ethtool_cmd {
    uint32_t	cmd;
    uint32_t	supported;      /* Features this interface supports */
    uint32_t	advertising;    /* Features this interface advertises */
    uint16_t	speed;          /* The forced speed, 10Mb, 100Mb, gigabit */
    uint8_t	duplex;         /* Duplex, half or full */
    uint8_t	port;           /* Which connector port */
    uint8_t	phy_address;
    uint8_t	transceiver;    /* Which tranceiver to use */
    uint8_t	autoneg;        /* Enable or disable autonegotiation */
    uint8_t	mdio_support;
    uint32_t	maxtxpkt;       /* Tx pkts before generating tx int */
    uint32_t	maxrxpkt;       /* Rx pkts before generating rx int */
    uint16_t	speed_hi;	/* High bits of the speed */
    uint8_t	eth_tp_mdix;
    uint8_t	eth_tp_mdix_ctrl;
    uint32_t	lp_advertising;
    uint32_t	reserved[2];
};

/* ioctl(SIOCGIWRATE) GET ("get wireless rate") structure */
struct iw_param {
    int32_t	value;
    uint8_t	fixed;
    uint8_t	disabled;
    uint16_t	flags;
};

union iwreq_data {
    char		padding[256];	/* sufficient for any kernel copyout */
    struct iw_param	bitrate;	/* default bit rate */
};

struct iwreq {
        union {
		char	ifrn_name[16];
        } ifr_ifrn;
        union iwreq_data u;		/* payload, defined above */
};

#define IPV6_ADDR_ANY           0x0000U
#define IPV6_ADDR_UNICAST       0x0001U
#define IPV6_ADDR_MULTICAST     0x0002U
#define IPV6_ADDR_ANYCAST       0x0004U
#define IPV6_ADDR_LOOPBACK      0x0010U
#define IPV6_ADDR_LINKLOCAL     0x0020U
#define IPV6_ADDR_SITELOCAL     0x0040U
#define IPV6_ADDR_COMPATv4      0x0080U

struct linux_container;
extern int refresh_proc_net_dev(pmInDom, struct linux_container *);
extern void refresh_net_addr_ioctl(pmInDom, struct linux_container *, int *);
extern int refresh_net_ioctl(pmInDom, struct linux_container *, int *);
extern void refresh_net_addr_sysfs(pmInDom, int *);
extern int refresh_net_sysfs(pmInDom, int *);

extern void refresh_net_addr_sysfs(pmInDom, int *);
extern void refresh_net_addr_ioctl(pmInDom, struct linux_container *, int *);

extern void clear_net_addr_indom(pmInDom);
extern void store_net_addr_indom(pmInDom, struct linux_container *);

extern char *lookup_ipv6_scope(int);

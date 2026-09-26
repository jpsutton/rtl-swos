/**
 * \addtogroup uipopt
 * @{
 */

/**
 * \name Project-specific configuration options
 * @{
 *
 * uIP has a number of configuration options that can be overridden
 * for each project. These are kept in a project-specific uip-conf.h
 * file and all configuration names have the prefix UIP_CONF.
 */

/*
 * Copyright (c) 2006, Swedish Institute of Computer Science.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the Institute nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE INSTITUTE AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE INSTITUTE OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * This file is part of the uIP TCP/IP stack
 *
 * $Id: uip-conf.h,v 1.6 2006/06/12 08:00:31 adam Exp $
 */

/**
 * \file
 *         An example uIP configuration file
 * \author
 *         Adam Dunkels <adam@sics.se>
 */

#ifndef __UIP_CONF_H__
#define __UIP_CONF_H__

#include <stdint.h>

#define UIP_CONF_EXTERNAL_BUFFER
#define UIP_ARCH_CHKSUM 1
#define UIP_ARCH_IPCHKSUM 1

/**
 * 8 bit datatype
 *
 * This typedef defines the 8-bit type used throughout uIP.
 *
 * \hideinitializer
 */
typedef uint8_t u8_t;

/**
 * 16 bit datatype
 *
 * This typedef defines the 16-bit type used throughout uIP.
 *
 * \hideinitializer
 */
typedef uint16_t u16_t;

/**
 * Statistics datatype
 *
 * This typedef defines the dataype used for keeping statistics in
 * uIP.
 *
 * \hideinitializer
 */
typedef unsigned short uip_stats_t;

/**
 * Maximum number of TCP connections. TODO: Increase this, but also make the socket state/buffer per-connection.
 *
 * Two slots so a lingering half-closed session (FIN_WAIT etc.) does not
 * lock out a fresh telnet connection while it drains. The packet buffer
 * stays shared (UIP_CONF_EXTERNAL_BUFFER), so the extra slot costs only
 * one more uip_conn in xdata.
 *
 * \hideinitializer
 */
#define UIP_CONF_MAX_CONNECTIONS 2

/**
 * Maximum number of UDP connections.
 *
 * One each for DHCP, DNS, NTP, syslog and TFTP. uip_udp_new() returns
 * 0 when no slot is free.
 *
 * \hideinitializer
 */
#define UIP_CONF_UDP_CONNS 5

/**
 * How many times uip_periodic() runs per second: handle_tx() runs the
 * TCP timers at this rate and polls in between.
 *
 * \hideinitializer
 */
#define UIP_TCP_HZ 10

/**
 * Age out ESTABLISHED connections that stay idle: a peer that died or
 * never sent anything would otherwise hold a connection slot until the
 * next power cycle.
 *
 * uip_periodic() runs UIP_TCP_HZ times per second, so the timeout is
 * in seconds.
 *
 * \hideinitializer
 */
#define UIP_CONF_IDLE_PERIODS UIP_TCP_HZ
#define UIP_CONF_IDLE_TIMEOUT 30

/*
 * Exempt the telnet port from the idle reaper above: telnet is a
 * long-lived interactive session and the telnet server enforces its
 * own, longer idle timeout instead.
 */
#define UIP_IDLE_EXEMPT_LPORT 23

/**
 * Maximum number of listening TCP ports.
 *
 * The telnet server (port 23) is the only TCP listener; one spare slot
 * for a future service. uip_listen() fails silently when no listen slot
 * is free.
 *
 * \hideinitializer
 */
#define UIP_CONF_MAX_LISTENPORTS 2

/**
 * uIP buffer size.
 *
 * Sized to the largest frame the CPU port accepts on ingress: anything above
 * that the NIC drops in hardware, so a larger buffer only costs XDATA. Measured
 * with ICMP, which bypasses MSS and so probes the hardware directly: a 1502-byte
 * payload is answered and 1503 is not, which puts the frame at 1556 bytes of
 * uip_buf. The limit is the NIC's rather than a port's, so how the frame was
 * tagged on the wire does not change it.
 *
 * \hideinitializer
 */
#define UIP_CONF_BUFFER_SIZE     1556

/**
 * Bytes of the buffer kept out of the advertised MSS.
 *
 * \hideinitializer
 */
#define UIP_CONF_BUFFER_EXTRA    30

/**
 * CPU byte order.
 *
 * \hideinitializer
 */
#define UIP_CONF_BYTE_ORDER      LITTLE_ENDIAN

/**
 * Logging on or off
 *
 * \hideinitializer
 */
#define UIP_CONF_LOGGING         1

/**
 * UDP support on or off
 *
 * \hideinitializer
 */
#define UIP_CONF_UDP             1

/**
 * UDP checksums on or off
 * The RTL8372/3 are able to calculate and verify all L2 and L3 checksums
 * in hardware. The TX checksums are configured in the CPU-tag of outgoing
 * packets, while the RTL837X_NIC_RX_CTRL register configures the verification
 * of the checksum of inbound packets and subsequent possible drop.
 *
 * \hideinitializer
 */
#define UIP_CONF_UDP_CHECKSUMS   0

/**
 * uIP statistics on or off
 *
 * \hideinitializer
 */
#define UIP_CONF_STATISTICS      0

/* Here we include the header file for the application(s) we use in
   our project. */
/*#include "smtp.h"*/
#include "tcp_app.h"
#include "udp_apps.h"
/*#include "telnetd.h"*/
/*#include "webserver.h" */
/*#include "dhcpc.h"*/
/*#include "resolv.h"*/
/*#include "webclient.h"*/

#endif /* __UIP_CONF_H__ */

/** @} */
/** @} */

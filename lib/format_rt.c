/*
 * This file is part of libtrace
 *
 * Copyright (c) 2004 The University of Waikato, Hamilton, New Zealand.
 * Authors: Daniel Lawson
 *          Perry Lorier
 *	    Shane Alcock
 *
 * All rights reserved.
 *
 * This code has been developed by the University of Waikato WAND
 * research group. For further information please see http://www.wand.net.nz/
 *
 * libtrace is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * libtrace is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with libtrace; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * $Id$
 *
 */

#define _GNU_SOURCE

#include "config.h"
#include "common.h"
#include "libtrace.h"
#include "libtrace_int.h"
#include "format_helper.h"
#include "parse_cmd.h"
#include "rt_protocol.h"

#ifdef HAVE_INTTYPES_H
#  include <inttypes.h>
#else
#  error "Can't find inttypes.h - this needs to be fixed"
#endif

#ifdef HAVE_STDDEF_H
#  include <stddef.h>
#else
# error "Can't find stddef.h - do you define ptrdiff_t elsewhere?"
#endif
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <assert.h>
#include <errno.h>
#include <netdb.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define RT_INFO ((struct rt_format_data_t*)libtrace->format_data)

int reliability = 0;

char *rt_deny_reason(uint8_t reason) {
	char *string = 0;

	switch(reason) {
		case RT_DENY_WRAPPER:
			string = "Rejected by TCP Wrappers";
			break;
		case RT_DENY_FULL:
			string = "Max connections reached on server";
			break;
		case RT_DENY_AUTH:
			string = "Authentication failed";
			break;
		default:
			string = "Unknown reason";
	}

	return string;
}


struct rt_format_data_t {
	char *hostname;
	int port;
	int input_fd;
	int reliable;
	char *pkt_buffer;
	char *buf_current;
	int buf_left;

	
	struct libtrace_t *dummy_erf;
	struct libtrace_t *dummy_pcap;
	struct libtrace_t *dummy_wag;
};

static struct libtrace_format_t rt;

static int rt_connect(struct libtrace_t *libtrace) {
        struct hostent *he;
        struct sockaddr_in remote;
	rt_header_t connect_msg;
	rt_deny_conn_t deny_hdr;	
	rt_hello_t hello_opts;
	uint8_t reason;
	int oldflags;

	
	if ((he=gethostbyname(RT_INFO->hostname)) == NULL) {
                perror("gethostbyname");
                return -1;
        }
        if ((RT_INFO->input_fd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
                perror("socket");
                return -1;
        }

        remote.sin_family = AF_INET;
        remote.sin_port = htons(RT_INFO->port);
        remote.sin_addr = *((struct in_addr *)he->h_addr);
        bzero(&(remote.sin_zero), 8);

        if (connect(RT_INFO->input_fd, (struct sockaddr *)&remote,
                                sizeof(struct sockaddr)) == -1) {
                trace_set_err(libtrace, TRACE_ERR_INIT_FAILED,
				"Could not connect to host %s on port %d",
				RT_INFO->hostname, RT_INFO->port);
		return -1;
        }

	
#if 0
	oldflags = fcntl(RT_INFO->input_fd, F_GETFL, 0);
	if (oldflags == -1) {
		trace_set_err(libtrace, errno,
				"Could not get fd flags from fd %d\n",
				RT_INFO->input_fd);
		return -1;
	}
	oldflags |= O_NONBLOCK;
	if (fcntl(RT_INFO->input_fd, F_SETFL, oldflags) == -1) {
		trace_set_err(libtrace, errno,
				"Could not set fd flags for fd %d\n",
				RT_INFO->input_fd);
		return -1;
	}
#endif
	
	
	/* We are connected, now receive message from server */
	
	if (recv(RT_INFO->input_fd, &connect_msg, sizeof(rt_header_t), 0) != sizeof(rt_header_t) ) {
		trace_set_err(libtrace, TRACE_ERR_INIT_FAILED,
				"Could not receive connection message from %s",
				RT_INFO->hostname);
		return -1;
	}
	
	switch (connect_msg.type) {
		case RT_DENY_CONN:
			
			if (recv(RT_INFO->input_fd, &deny_hdr, 
						sizeof(rt_deny_conn_t),
						0) != sizeof(rt_deny_conn_t)) {
				reason = 0;
			}	
			reason = deny_hdr.reason;
			trace_set_err(libtrace, TRACE_ERR_INIT_FAILED,
				"Connection attempt is denied: %s",
				rt_deny_reason(reason));	
			return -1;
		case RT_HELLO:
			/* do something with options */
			if (recv(RT_INFO->input_fd, &hello_opts, 
						sizeof(rt_hello_t), 0)
					!= sizeof(rt_hello_t)) {
				trace_set_err(libtrace, TRACE_ERR_INIT_FAILED,
					"Failed to receive RT_HELLO options");
				return -1;
			}
			reliability = hello_opts.reliable;
			
			return 0;
		default:
			trace_set_err(libtrace, TRACE_ERR_INIT_FAILED,
					"Unknown message type received: %d",
					connect_msg.type);
			return -1;
	}
	trace_set_err(libtrace, TRACE_ERR_INIT_FAILED,
			"Somehow you managed to reach this unreachable code");
        return -1;
}


static int rt_init_input(struct libtrace_t *libtrace) {
        char *scan;
        char *uridata = libtrace->uridata;
        libtrace->format_data = malloc(sizeof(struct rt_format_data_t));

	RT_INFO->dummy_erf = NULL;
	RT_INFO->dummy_pcap = NULL;
	RT_INFO->dummy_wag = NULL;
	RT_INFO->pkt_buffer = NULL;
	RT_INFO->buf_current = NULL;
	RT_INFO->buf_left = 0;
	
        if (strlen(uridata) == 0) {
                RT_INFO->hostname =
                        strdup("localhost");
                RT_INFO->port =
                        COLLECTOR_PORT;
        } else {
                if ((scan = strchr(uridata,':')) == NULL) {
                        RT_INFO->hostname =
                                strdup(uridata);
                        RT_INFO->port =
                                COLLECTOR_PORT;
                } else {
                        RT_INFO->hostname =
                                (char *)strndup(uridata,
                                                (scan - uridata));
                        RT_INFO->port =
                                atoi(++scan);
                }
        }

	return rt_connect(libtrace);
}
	
static int rt_start_input(struct libtrace_t *libtrace) {
	rt_header_t start_msg;

	start_msg.type = RT_START;
	start_msg.length = sizeof(rt_start_t);

	
	/* Need to send start message to server */
	if (send(RT_INFO->input_fd, &start_msg, sizeof(rt_header_t) +
				start_msg.length, 0) != sizeof(rt_header_t)) {
		printf("Failed to send start message to server\n");
		return -1;
	}

	return 0;
}

static int rt_fin_input(struct libtrace_t *libtrace) {
        rt_header_t close_msg;

	close_msg.type = RT_CLOSE;
	close_msg.length = sizeof(rt_close_t);
	
	/* Send a close message to the server */
	if (send(RT_INFO->input_fd, &close_msg, sizeof(rt_header_t) + 
				close_msg.length, 0) != sizeof(rt_header_t)
				+ close_msg.length) {
		printf("Failed to send close message to server\n");
	
	}
	if (RT_INFO->dummy_erf) 
		trace_destroy_dead(RT_INFO->dummy_erf);
		
	if (RT_INFO->dummy_pcap)
		trace_destroy_dead(RT_INFO->dummy_pcap);

	if (RT_INFO->dummy_wag)
		trace_destroy_dead(RT_INFO->dummy_wag);
	close(RT_INFO->input_fd);
	free(libtrace->format_data);
        return 0;
}

#define RT_BUF_SIZE 4000

static int rt_read(struct libtrace_t *libtrace, void **buffer, size_t len, int block) {
        int numbytes;
	int i;
	char *buf_ptr;

	assert(len <= RT_BUF_SIZE);
	
	if (!RT_INFO->pkt_buffer) {
		RT_INFO->pkt_buffer = malloc(RT_BUF_SIZE);
		RT_INFO->buf_current = RT_INFO->pkt_buffer;
		RT_INFO->buf_left = 0;
	}

	if (block)
		block=0;
	else
		block=MSG_DONTWAIT;

	
	if (len > RT_INFO->buf_left) {
		memcpy(RT_INFO->pkt_buffer, RT_INFO->buf_current, 
				RT_INFO->buf_left);
		RT_INFO->buf_current = RT_INFO->pkt_buffer;

#ifndef MSG_NOSIGNAL
#  define MSG_NOSIGNAL 0
#endif
		while (len > RT_INFO->buf_left) {
                	if ((numbytes = recv(RT_INFO->input_fd,
                                                RT_INFO->pkt_buffer + 
						RT_INFO->buf_left,
                                                RT_BUF_SIZE-RT_INFO->buf_left,
                                                MSG_NOSIGNAL|block)) <= 0) {
				if (numbytes == 0) {
					trace_set_err(libtrace, TRACE_ERR_BAD_PACKET, 
							"No data received");
					return -1;
				}
				
                	        if (errno == EINTR) {
                	                /* ignore EINTR in case
                	                 * a caller is using signals
					 */
                	                continue;
                	        }
				if (errno == EAGAIN) {
					trace_set_err(libtrace,
							EAGAIN,
							"EAGAIN");
					return -1;
				}
				
                        	perror("recv");
				trace_set_err(libtrace, TRACE_ERR_RECV_FAILED,
						"Failed to read data into rt recv buffer");
                        	return -1;
                	}
			/*
			buf_ptr = RT_INFO->pkt_buffer;
			for (i = 0; i < RT_BUF_SIZE ; i++) {
					
				printf("%02x", (unsigned char)*buf_ptr);
				buf_ptr ++;
			}
			printf("\n");
			*/
			RT_INFO->buf_left+=numbytes;
		}

        }
	*buffer = RT_INFO->buf_current;
	RT_INFO->buf_current += len;
	RT_INFO->buf_left -= len;
	assert(RT_INFO->buf_left >= 0);
        return len;
}


static int rt_set_format(libtrace_t *libtrace, libtrace_packet_t *packet) 
{
	
	if (packet->type >= RT_DATA_PCAP) {
		if (!RT_INFO->dummy_pcap) {
			RT_INFO->dummy_pcap = trace_create_dead("pcap:-");
		}
		packet->trace = RT_INFO->dummy_pcap;
		return 0;	
	}

	switch (packet->type) {
		case RT_DATA_ERF:
			if (!RT_INFO->dummy_erf) {
				RT_INFO->dummy_erf = trace_create_dead("erf:-");
			}
			packet->trace = RT_INFO->dummy_erf;
			break;
		case RT_DATA_WAG:
			if (!RT_INFO->dummy_wag) {
				RT_INFO->dummy_wag = trace_create_dead("wtf:-");
			}
			packet->trace = RT_INFO->dummy_wag;
			break;
		case RT_DATA_LEGACY_ETH:
		case RT_DATA_LEGACY_ATM:
		case RT_DATA_LEGACY_POS:
			printf("Sending legacy over RT is currently not supported\n");
			trace_set_err(libtrace, TRACE_ERR_BAD_PACKET, "Legacy packet cannot be sent over rt");
			return -1;
		default:
			printf("Unrecognised format: %d\n", packet->type);
			trace_set_err(libtrace, TRACE_ERR_BAD_PACKET, "Unrecognised packet format");
			return -1;
	}
	return 0; /* success */
}		

static void rt_set_payload(struct libtrace_packet_t *packet) {
	dag_record_t *erfptr;
	
	switch (packet->type) {
		case RT_DATA_ERF:
			erfptr = (dag_record_t *)packet->header;
			
			if (erfptr->flags.rxerror == 1) {
				packet->payload = NULL;
				break;
			}
			/* else drop into the default case */
		default:
			packet->payload = (char *)packet->buffer +
				trace_get_framing_length(packet);
			break;
	}
}

static int rt_send_ack(struct libtrace_t *libtrace, 
		uint32_t seqno)  {
	
	static char *ack_buffer = 0;
	char *buf_ptr;
	int numbytes = 0;
	int to_write = 0;
	rt_header_t *hdr;
	rt_ack_t *ack_hdr;
	
	if (!ack_buffer) {
		ack_buffer = malloc(sizeof(rt_header_t) + sizeof(rt_ack_t));
	}
	
	hdr = (rt_header_t *) ack_buffer;
	ack_hdr = (rt_ack_t *) (ack_buffer + sizeof(rt_header_t));
	
	hdr->type = RT_ACK;
	hdr->length = sizeof(rt_ack_t);

	ack_hdr->sequence = seqno;
	
	to_write = hdr->length + sizeof(rt_header_t);
	buf_ptr = ack_buffer;

	
	while (to_write > 0) {
		numbytes = send(RT_INFO->input_fd, buf_ptr, to_write, 0); 
		if (numbytes == -1) {
			if (errno == EINTR || errno == EAGAIN) {
				continue;
			}
			else {
				printf("Error sending ack\n");
				trace_set_err(libtrace, TRACE_ERR_BAD_PACKET, 
						"Error sending ack");
				return -1;
			}
		}
		to_write = to_write - numbytes;
		buf_ptr = buf_ptr + to_write;
		
	}

	return 1;
}

	
static int rt_read_packet_versatile(libtrace_t *libtrace,
		libtrace_packet_t *packet,int blocking) {
	rt_header_t rt_hdr;
	rt_header_t *pkt_hdr = &rt_hdr;
	int pkt_size = 0;
	
	
        if (packet->buf_control == TRACE_CTRL_EXTERNAL || !packet->buffer) {
                packet->buf_control = TRACE_CTRL_PACKET;
                packet->buffer = malloc(LIBTRACE_PACKET_BUFSIZE);
        } 


	/* FIXME: Better error handling required */
	if (rt_read(libtrace, (void **)&pkt_hdr, sizeof(rt_header_t),blocking) !=
			sizeof(rt_header_t)) {
		return -1;
	}

	packet->type = pkt_hdr->type;
	pkt_size = pkt_hdr->length;
	packet->size = pkt_hdr->length;

	if (packet->type >= RT_DATA_SIMPLE) {
		if (rt_read(libtrace, &packet->buffer, pkt_size,1) != pkt_size) {
			printf("Error receiving packet\n");
			return -1;
		}
        	packet->header = packet->buffer;
		
		if (rt_set_format(libtrace, packet) < 0) {
                       	return -1;
                }
               	rt_set_payload(packet);

                if (reliability > 0) {
                        
			if (rt_send_ack(libtrace, pkt_hdr->sequence) 
					== -1)
			{
                               	return -1;
                        }
		}
	} else {
		switch(packet->type) {
			case RT_STATUS:
			case RT_DUCK:
				if (rt_read(libtrace, &packet->buffer, 
							pkt_size,1) !=
						pkt_size) {
					printf("Error receiving status packet\n");
					return -1;
				}
				packet->header = 0;
				packet->payload = packet->buffer;
				break;
			case RT_END_DATA:
				return 0;
			case RT_PAUSE_ACK:
				/* FIXME: Do something useful */
				break;
			case RT_OPTION:
				/* FIXME: Do something useful here as well */
				break;
			case RT_KEYCHANGE:
				break;
			default:
				printf("Bad rt type for client receipt: %d\n",
					pkt_hdr->type);
		}
	}
	/* Return the number of bytes read from the stream */
	return packet->size; 
}

static int rt_read_packet(libtrace_t *libtrace,
		libtrace_packet_t *packet) {
	rt_read_packet_versatile(libtrace,packet,1);
}


static int rt_get_capture_length(const struct libtrace_packet_t *packet) {
	switch (packet->type) {
		case RT_DUCK:
			return sizeof(rt_duck_t);
		case RT_STATUS:
			return sizeof(rt_status_t);
		case RT_HELLO:
			return sizeof(rt_hello_t);
		case RT_START:
			return sizeof(rt_start_t);
		case RT_ACK:
			return sizeof(rt_ack_t);
		case RT_END_DATA:
			return sizeof(rt_end_data_t);
		case RT_CLOSE:
			return sizeof(rt_close_t);
		case RT_DENY_CONN:
			return sizeof(rt_deny_conn_t);
		case RT_PAUSE:
			return sizeof(rt_pause_t);
		case RT_PAUSE_ACK:
			return sizeof(rt_pause_ack_t);
		case RT_OPTION:
			return sizeof(rt_option_t);
		case RT_KEYCHANGE:
			return sizeof(rt_keychange_t);
	}
	printf("Unknown type: %d\n", packet->type);
	return 0;
}

static int rt_get_wire_length(const libtrace_packet_t *packet) {
	return 0;
}
			
static int rt_get_framing_length(const libtrace_packet_t *packet) {
	return 0;
}

static int rt_get_fd(const libtrace_t *trace) {
        return ((struct rt_format_data_t *)trace->format_data)->input_fd;
}

struct libtrace_eventobj_t trace_event_rt(struct libtrace_t *trace, struct libtrace_packet_t *packet) {
	struct libtrace_eventobj_t event = {0,0,0.0,0};
	libtrace_err_t read_err;
	int data;

	assert(trace);
	assert(packet);
	
	if (trace->format->get_fd) {
		event.fd = trace->format->get_fd(trace);
	} else {
		event.fd = 0;
	}

	event.size = rt_read_packet_versatile(trace, packet, 0);
	if (event.size == -1) {
		read_err = trace_get_err(trace);
		if (read_err.err_num == EAGAIN) {
			event.type = TRACE_EVENT_IOWAIT;
		}
		else {
			printf("packet error\n");
			event.type = TRACE_EVENT_PACKET;
		}
	} else if (event.size == 0) {
		event.type = TRACE_EVENT_TERMINATE;
		
	}	
	else {
		event.type = TRACE_EVENT_PACKET;
	}
	
	return event;
}

static void rt_help() {
        printf("rt format module\n");
        printf("Supported input URIs:\n");
        printf("\trt:hostname:port\n");
        printf("\trt:hostname (connects on default port)\n");
        printf("\n");
        printf("\te.g.: rt:localhost\n");
        printf("\te.g.: rt:localhost:32500\n");
        printf("\n");

}


static struct libtrace_format_t rt = {
        "rt",
        "$Id$",
        TRACE_FORMAT_RT,
        rt_init_input,            	/* init_input */
        NULL,                           /* config_input */
        rt_start_input,           	/* start_input */
        NULL,                           /* init_output */
        NULL,                           /* config_output */
        NULL,                           /* start_output */
	NULL,				/* pause_output */
        rt_fin_input,             	/* fin_input */
        NULL,                           /* fin_output */
        rt_read_packet,           	/* read_packet */
	NULL,				/* fin_packet */
        NULL,                           /* write_packet */
        NULL,		                /* get_link_type */
        NULL,  		            	/* get_direction */
        NULL,              		/* set_direction */
        NULL,          			/* get_erf_timestamp */
        NULL,                           /* get_timeval */
        NULL,                           /* get_seconds */
	NULL,				/* seek_erf */
	NULL,				/* seek_timeval */
	NULL,				/* seek_seconds */
        rt_get_capture_length,        	/* get_capture_length */
        rt_get_wire_length,            		/* get_wire_length */
        rt_get_framing_length, 		/* get_framing_length */
        NULL,         			/* set_capture_length */
        rt_get_fd,                	/* get_fd */
        trace_event_rt,             /* trace_event */
        rt_help,			/* help */
	NULL				/* next pointer */
};

void __attribute__((constructor)) rt_constructor() {
	register_format(&rt);
}
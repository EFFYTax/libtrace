/*
 * This file is part of libtrace
 *
 * Copyright (c) 2004 The University of Waikato, Hamilton, New Zealand.
 * Authors: Daniel Lawson 
 *          Perry Lorier 
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

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifdef HAVE_DAG
#include <sys/mman.h>
#endif

#ifdef WIN32
#  include <io.h>
#  include <share.h>
#  define PATH_MAX _MAX_PATH
#  define snprintf sprintf_s
#else
#  include <netdb.h>
#  ifndef PATH_MAX
#	define PATH_MAX 4096
#  endif
#  include <sys/ioctl.h>
#endif


#define COLLECTOR_PORT 3435

static struct libtrace_format_t erf;
#ifdef HAVE_DAG
static struct libtrace_format_t dag;
#endif 

#define DATA(x) ((struct erf_format_data_t *)x->format_data)
#define DATAOUT(x) ((struct erf_format_data_out_t *)x->format_data)

#define CONNINFO DATA(libtrace)->conn_info
#define INPUT DATA(libtrace)->input
#define OUTPUT DATAOUT(libtrace)->output
#ifdef HAVE_DAG
#define DAG DATA(libtrace)->dag
#define DUCK DATA(libtrace)->duck
#endif
#define OPTIONS DATAOUT(libtrace)->options
struct erf_format_data_t {
	union {
                struct {
                        char *hostname;
                        short port;
                } rt;
        } conn_info;
        
	union {
                int fd;
		libtrace_io_t *file;
        } input;

	struct {
		enum { INDEX_UNKNOWN=0, INDEX_NONE, INDEX_EXISTS } exists;
		libtrace_io_t *index;
		off_t index_len;
	} seek;

#ifdef HAVE_DAG
	struct {
		uint32_t last_duck;	
		uint32_t duck_freq;
		uint32_t last_pkt;
		libtrace_t *dummy_duck;
	} duck;
	
	struct {
		void *buf; 
		unsigned bottom;
		unsigned top;
		unsigned diff;
		unsigned curr;
		unsigned offset;
		unsigned int dagstream;
	} dag;
#endif
};

struct erf_format_data_out_t {
        union {
                struct {
                        char *hostname;
                        short port;
                } rt;
                char *path;
        } conn_info;

	union {
		struct {
			int level;
			int fileflag;
		} erf;
		
	} options;
	
        union {
                int fd;
                struct rtserver_t * rtserver;
		libtrace_io_t *file;
        } output;
};

/** Structure holding status information for a packet */
typedef struct libtrace_packet_status {
	uint8_t type;
	uint8_t reserved;
	uint16_t message;
} libtrace_packet_status_t;

typedef struct erf_index_t {
	uint64_t timestamp;
	uint64_t offset; 
} erf_index_t;

#ifdef HAVE_DAG
static int dag_init_input(libtrace_t *libtrace) {
	struct stat buf;

	libtrace->format_data = (struct erf_format_data_t *)
		malloc(sizeof(struct erf_format_data_t));
	if (stat(libtrace->uridata, &buf) == -1) {
		trace_set_err(libtrace,errno,"stat(%s)",libtrace->uridata);
		return -1;
	} 

	DAG.dagstream = 0;
	
	if (S_ISCHR(buf.st_mode)) {
		/* DEVICE */
		if((INPUT.fd = dag_open(libtrace->uridata)) < 0) {
			trace_set_err(libtrace,errno,"Cannot open DAG %s",
					libtrace->uridata);
			return -1;
		}
		if((DAG.buf = (void *)dag_mmap(INPUT.fd)) == MAP_FAILED) {
			trace_set_err(libtrace,errno,"Cannot mmap DAG %s",
					libtrace->uridata);
			return -1;
		}
	} else {
		trace_set_err(libtrace,errno,"Not a valid dag device: %s",
				libtrace->uridata);
		return -1;
	}

	DUCK.last_duck = 0;
	DUCK.duck_freq = 0;  
	DUCK.last_pkt = 0;
	DUCK.dummy_duck = NULL;
	
	return 0;
}

static int dag_config_input(libtrace_t *libtrace, trace_option_t option,
				void *data) {
	switch(option) {
		case TRACE_META_FREQ:
			DUCK.duck_freq = *(int *)data;
			return 0;
		case TRACE_OPTION_SNAPLEN:
			/* Surely we can set this?? Fall through for now*/
			return -1;
		case TRACE_OPTION_PROMISC:
			/* DAG already operates in a promisc fashion */
			return -1;
		case TRACE_OPTION_FILTER:
			return -1;
		default:
			trace_set_err(libtrace, TRACE_ERR_UNKNOWN_OPTION,
					"Unknown or unsupported option: %i",
					option);
			return -1;
	}
	assert (0);
}
#endif

/* Dag erf ether packets have a 2 byte padding before the packet
 * so that the ip header is aligned on a 32 bit boundary.
 */
static int erf_get_padding(const libtrace_packet_t *packet)
{
	if (packet->trace->format->type==TRACE_FORMAT_ERF) {
		dag_record_t *erfptr = (dag_record_t *)packet->header;
		switch(erfptr->type) {
			case TYPE_ETH: 		return 2;
			default: 		return 0;
		}
	}
	else {
		switch(trace_get_link_type(packet)) {
			case TYPE_ETH:		return 2;
			default:		return 0;
		}
	}
}

static int erf_get_framing_length(const libtrace_packet_t *packet)
{
	return dag_record_size + erf_get_padding(packet);
}


static int erf_init_input(libtrace_t *libtrace) 
{
	libtrace->format_data = malloc(sizeof(struct erf_format_data_t));
	
	INPUT.file = 0;

	return 0; /* success */
}

static int erf_start_input(libtrace_t *libtrace)
{
	if (INPUT.file)
		return 0; /* success */

	INPUT.file = trace_open_file(libtrace);

	if (!INPUT.file)
		return -1;

	return 0; /* success */
}

/* Binary search through the index to find the closest point before
 * the packet.  Consider in future having a btree index perhaps?
 */
static int erf_fast_seek_start(libtrace_t *libtrace,uint64_t erfts)
{
	size_t max_off = DATA(libtrace)->seek.index_len/sizeof(erf_index_t);
	size_t min_off = 0;
	off_t current;
	erf_index_t record;
	do {
		current=(max_off+min_off)>>2;

		libtrace_io_seek(DATA(libtrace)->seek.index,
				current*sizeof(record),
				SEEK_SET);
		libtrace_io_read(DATA(libtrace)->seek.index,
				&record,sizeof(record));
		if (record.timestamp < erfts) {
			min_off=current;
		}
		if (record.timestamp > erfts) {
			max_off=current;
		}
		if (record.timestamp == erfts)
			break;
	} while(min_off<max_off);

	/* If we've passed it, seek backwards.  This loop shouldn't
	 * execute more than twice.
	 */
	do {
		libtrace_io_seek(DATA(libtrace)->seek.index,
				current*sizeof(record),SEEK_SET);
		libtrace_io_read(DATA(libtrace)->seek.index,
				&record,sizeof(record));
		current--;
	} while(record.timestamp>erfts);

	/* We've found our location in the trace, now use it. */
	libtrace_io_seek(INPUT.file,record.offset,SEEK_SET);

	return 0; /* success */
}

/* There is no index.  Seek through the entire trace from the start, nice
 * and slowly.
 */
static int erf_slow_seek_start(libtrace_t *libtrace,uint64_t erfts)
{
	if (INPUT.file) {
		libtrace_io_close(INPUT.file);
	}
	INPUT.file = trace_open_file(libtrace);
	if (!INPUT.file)
		return -1;
	return 0;
}

static int erf_seek_erf(libtrace_t *libtrace,uint64_t erfts)
{
	libtrace_packet_t *packet;
	off_t off = 0;

	if (DATA(libtrace)->seek.exists==INDEX_UNKNOWN) {
		char buffer[PATH_MAX];
		snprintf(buffer,sizeof(buffer),"%s.idx",libtrace->uridata);
		DATA(libtrace)->seek.index=libtrace_io_open(buffer,"rb");
		if (DATA(libtrace)->seek.index) {
			DATA(libtrace)->seek.exists=INDEX_EXISTS;
		}
		else {
			DATA(libtrace)->seek.exists=INDEX_NONE;
		}
	}

	/* If theres an index, use it to find the nearest packet that isn't
	 * after the time we're looking for.  If there is no index we need
	 * to seek slowly through the trace from the beginning.  Sigh.
	 */
	switch(DATA(libtrace)->seek.exists) {
		case INDEX_EXISTS:
			erf_fast_seek_start(libtrace,erfts);
			break;
		case INDEX_NONE:
			erf_slow_seek_start(libtrace,erfts);
			break;
		case INDEX_UNKNOWN:
			assert(0);
			break;
	}

	/* Now seek forward looking for the correct timestamp */
	packet=trace_create_packet();
	do {
		trace_read_packet(libtrace,packet);
		if (trace_get_erf_timestamp(packet)==erfts)
			break;
		off=libtrace_io_tell(INPUT.file);
	} while(trace_get_erf_timestamp(packet)<erfts);

	libtrace_io_seek(INPUT.file,off,SEEK_SET);

	return 0;
}

static int erf_init_output(libtrace_out_t *libtrace) {
	libtrace->format_data = malloc(sizeof(struct erf_format_data_out_t));

	OPTIONS.erf.level = 0;
	OPTIONS.erf.fileflag = O_CREAT | O_WRONLY;
	OUTPUT.file = 0;

	return 0;
}

static int erf_config_output(libtrace_out_t *libtrace, trace_option_output_t option,
		void *value) {

	switch (option) {
		case TRACE_OPTION_OUTPUT_COMPRESS:
			OPTIONS.erf.level = *(int*)value;
			return 0;
		case TRACE_OPTION_OUTPUT_FILEFLAGS:
			OPTIONS.erf.fileflag = *(int*)value;
			return 0;
		default:
			/* Unknown option */
			trace_set_err_out(libtrace,TRACE_ERR_UNKNOWN_OPTION,
					"Unknown option");
			return -1;
	}
}


#ifdef HAVE_DAG
static int dag_pause_input(libtrace_t *libtrace) {
#ifdef DAG_VERSION_2_4
	dag_stop(INPUT.fd);
#else
	if (dag_stop_stream(INPUT.fd, DAG.dagstream) < 0) {
		trace_set_err(libtrace, errno, "Could not stop DAG stream");
		return -1;
	}
	if (dag_detach_stream(INPUT.fd, DAG.dagstream) < 0) {
		trace_set_err(libtrace, errno, "Could not detach DAG stream");
		return -1;
	}
#endif
	return 0; /* success */
}

static int dag_fin_input(libtrace_t *libtrace) {
	/* dag pause input implicitly called to cleanup before this */
	
	dag_close(INPUT.fd);
	if (DUCK.dummy_duck)
		trace_destroy_dead(DUCK.dummy_duck);
	free(libtrace->format_data);
	return 0; /* success */
}
#endif

static int erf_fin_input(libtrace_t *libtrace) {
	if (INPUT.file)
		libtrace_io_close(INPUT.file);
	free(libtrace->format_data);
	return 0;
}

static int erf_fin_output(libtrace_out_t *libtrace) {
	libtrace_io_close(OUTPUT.file);
	free(libtrace->format_data);
	return 0;
}
 
#ifdef HAVE_DAG
#ifdef DAG_VERSION_2_4
static int dag_get_duckinfo(libtrace_t *libtrace, 
				libtrace_packet_t *packet) {
	dag_inf lt_dag_inf;
	
	if (packet->buf_control == TRACE_CTRL_EXTERNAL || 
			!packet->buffer) {
		packet->buffer = malloc(LIBTRACE_PACKET_BUFSIZE);
		packet->buf_control = TRACE_CTRL_PACKET;
		if (!packet->buffer) {
			trace_set_err(libtrace, errno,
					"Cannot allocate packet buffer");
			return -1;
		}
	}
	
	packet->header = 0;
	packet->payload = packet->buffer;
	
	if ((ioctl(INPUT.fd, DAG_IOINF, &lt_dag_inf) < 0)) {
		trace_set_err(libtrace, errno,
				"Error using DAG_IOINF");
		return -1;
	}
	if (!IsDUCK(&lt_dag_inf)) {
		printf("WARNING: %s does not have modern clock support - No DUCK information will be gathered\n", libtrace->uridata);
		return 0;
	}

	if ((ioctl(INPUT.fd, DAG_IOGETDUCK, (duck_inf *)packet->payload) 
				< 0)) {
		trace_set_err(libtrace, errno, "Error using DAG_IOGETDUCK");
		return -1;
	}

	packet->type = TRACE_RT_DUCK_2_4;
	if (!DUCK.dummy_duck) 
		DUCK.dummy_duck = trace_create_dead("duck:dummy");
	packet->trace = DUCK.dummy_duck;
	return sizeof(duck_inf);
}	
#else
static int dag_get_duckinfo(libtrace_t *libtrace, 
				libtrace_packet_t *packet) {
	daginf_t lt_dag_inf;
	
	if (packet->buf_control == TRACE_CTRL_EXTERNAL || 
			!packet->buffer) {
		packet->buffer = malloc(LIBTRACE_PACKET_BUFSIZE);
		packet->buf_control = TRACE_CTRL_PACKET;
		if (!packet->buffer) {
			trace_set_err(libtrace, errno,
					"Cannot allocate packet buffer");
			return -1;
		}
	}
	
	packet->header = 0;
	packet->payload = packet->buffer;
	
	/* No need to check if we can get DUCK or not - we're modern
	 * enough */
	if ((ioctl(INPUT.fd, DAGIOCDUCK, (duckinf_t *)packet->payload) 
				< 0)) {
		trace_set_err(libtrace, errno, "Error using DAGIOCDUCK");
		return -1;
	}

	packet->type = TRACE_RT_DUCK_2_5;
	if (!DUCK.dummy_duck) 
		DUCK.dummy_duck = trace_create_dead("rt:localhost:3434");
	packet->trace = DUCK.dummy_duck;	
	return sizeof(duckinf_t);
}	
#endif

static int dag_read(libtrace_t *libtrace, int block_flag) {

	if (DAG.diff != 0) 
		return DAG.diff;

	DAG.bottom = DAG.top;

	DAG.top = dag_offset(
			INPUT.fd,
			&(DAG.bottom),
			block_flag);

	DAG.diff = DAG.top - DAG.bottom;

	DAG.offset = 0;
	return DAG.diff;
}

/* FIXME: dag_read_packet shouldn't update the pointers, dag_fin_packet
 * should do that.
 */
static int dag_read_packet(libtrace_t *libtrace, libtrace_packet_t *packet) {
	int numbytes;
	int size;
	struct timeval tv;
	dag_record_t *erfptr;

	if (DUCK.last_pkt - DUCK.last_duck > DUCK.duck_freq && 
			DUCK.duck_freq != 0) {
		size = dag_get_duckinfo(libtrace, packet);
		DUCK.last_duck = DUCK.last_pkt;
		if (size != 0) {
			return size;
		}
		/* No DUCK support, so don't waste our time anymore */
		DUCK.duck_freq = 0;
	}
	
	if (packet->buf_control == TRACE_CTRL_PACKET) {
		packet->buf_control = TRACE_CTRL_EXTERNAL;
		free(packet->buffer);
		packet->buffer = 0;
	}
 	
	packet->type = TRACE_RT_DATA_ERF;
	
	if ((numbytes = dag_read(libtrace,0)) < 0) 
		return numbytes;
	assert(numbytes>0);

	/*DAG always gives us whole packets */
	erfptr = (dag_record_t *) ((char *)DAG.buf + 
			(DAG.bottom + DAG.offset));
	size = ntohs(erfptr->rlen);

	assert( size >= dag_record_size );
	assert( size < LIBTRACE_PACKET_BUFSIZE);
	
	packet->buffer = erfptr;
	packet->header = erfptr;
	if (((dag_record_t *)packet->buffer)->flags.rxerror == 1) {
		/* rxerror means the payload is corrupt - drop it
		 * by tweaking rlen */
		packet->payload = NULL;
		erfptr->rlen = htons(erf_get_framing_length(packet));
	} else {
		packet->payload = (char*)packet->buffer 
			+ erf_get_framing_length(packet);
	}

	DAG.offset += size;
	DAG.diff -= size;

	tv = trace_get_timeval(packet);
	DUCK.last_pkt = tv.tv_sec;
	
	return packet->payload ? size : erf_get_framing_length(packet);
}

static int dag_start_input(libtrace_t *libtrace) {
#ifdef DAG_VERSION_2_4
	if(dag_start(INPUT.fd) < 0) {
		trace_set_err(libtrace,errno,"Cannot start DAG %s",
				libtrace->uridata);
		return -1;
	}
#else
	if (dag_attach_stream(INPUT.fd, DAG.dagstream, 0, 0) < 0) {
		trace_set_err(libtrace, errno, "Cannot attach DAG stream");
		return -1;
	}
	if (dag_start_stream(INPUT.fd, DAG.dagstream) < 0) {
		trace_set_err(libtrace, errno, "Cannot start DAG stream");
		return -1;
	}
#endif
	/* dags appear to have a bug where if you call dag_start after
	 * calling dag_stop, and at least one packet has arrived, bad things
	 * happen.  flush the memory hole 
	 */
	while(dag_read(libtrace,1)!=0)
		DAG.diff=0;
	return 0;
}
#endif 

static int erf_read_packet(libtrace_t *libtrace, libtrace_packet_t *packet) {
	int numbytes;
	unsigned int size;
	void *buffer2 = packet->buffer;
	unsigned int rlen;

	if (!packet->buffer || packet->buf_control == TRACE_CTRL_EXTERNAL) {
		packet->buffer = malloc(LIBTRACE_PACKET_BUFSIZE);
		packet->buf_control = TRACE_CTRL_PACKET;
		if (!packet->buffer) {
			trace_set_err(libtrace, errno, 
					"Cannot allocate memory");
			return -1;
		}
	}

	
	
	packet->header = packet->buffer;
	packet->type = TRACE_RT_DATA_ERF;

	if ((numbytes=libtrace_io_read(INPUT.file,
					packet->buffer,
					dag_record_size)) == -1) {
		trace_set_err(libtrace,errno,"read(%s)",
				libtrace->uridata);
		return -1;
	}
	/* EOF */
	if (numbytes == 0) {
		return 0;
	}

	rlen = ntohs(((dag_record_t *)packet->buffer)->rlen);
	buffer2 = (char*)packet->buffer + dag_record_size;
	size = rlen - dag_record_size;

	assert(size < LIBTRACE_PACKET_BUFSIZE);

	/* Unknown/corrupt */
	assert(((dag_record_t *)packet->buffer)->type < 10);
	
	/* read in the rest of the packet */
	if ((numbytes=libtrace_io_read(INPUT.file,
					buffer2,
					size)) != (int)size) {
		if (numbytes==-1) {
			trace_set_err(libtrace,errno, "read(%s)", libtrace->uridata);
			return -1;
		}
		trace_set_err(libtrace,EIO,"Truncated packet (wanted %d, got %d)", size, numbytes);
		/* Failed to read the full packet?  must be EOF */
		return -1;
	}
	if (((dag_record_t *)packet->buffer)->flags.rxerror == 1) {
		packet->payload = NULL;
	} else {
		packet->payload = (char*)packet->buffer + erf_get_framing_length(packet);
	}
	return rlen;
}

static int erf_dump_packet(libtrace_out_t *libtrace,
		dag_record_t *erfptr, unsigned int pad, void *buffer) {
	int numbytes = 0;
	int size;

	if ((numbytes = 
		libtrace_io_write(OUTPUT.file, erfptr, dag_record_size + pad)) 
			!= (int)dag_record_size+pad) {
		trace_set_err_out(libtrace,errno,
				"write(%s)",libtrace->uridata);
		return -1;
	}

	size=ntohs(erfptr->rlen)-(dag_record_size+pad);
	numbytes=libtrace_io_write(OUTPUT.file, buffer, size);
	if (numbytes != size) {
		trace_set_err_out(libtrace,errno,
				"write(%s)",libtrace->uridata);
		return -1;
	}
	return numbytes + pad + dag_record_size;
}

static int erf_start_output(libtrace_out_t *libtrace)
{
	OUTPUT.file = trace_open_file_out(libtrace,
			OPTIONS.erf.level,
			OPTIONS.erf.fileflag);
	if (!OUTPUT.file) {
		return -1;
	}
	return 0;
}

static bool find_compatible_linktype(libtrace_out_t *libtrace,
				libtrace_packet_t *packet)
{
	/* Keep trying to simplify the packet until we can find 
	 * something we can do with it */
	do {
		char type=libtrace_to_erf_type(trace_get_link_type(packet));

		/* Success */
		if (type != (char)-1)
			return true;

		if (!demote_packet(packet)) {
			trace_set_err_out(libtrace,
					TRACE_ERR_NO_CONVERSION,
					"No erf type for packet (%i)",
					trace_get_link_type(packet));
			return false;
		}

	} while(1);

	return true;
}
		
static int erf_write_packet(libtrace_out_t *libtrace, 
		libtrace_packet_t *packet) 
{
	int numbytes = 0;
	int pad = 0;
	dag_record_t *dag_hdr = (dag_record_t *)packet->header;
	void *payload = packet->payload;

	assert(OUTPUT.file);

	if (!packet->header) {
		/*trace_set_err_output(libtrace, TRACE_ERR_BAD_PACKET,
				"Packet has no header - probably an RT packet");
		*/
		return -1;
	}
	
	pad = erf_get_padding(packet);

	/* If we've had an rxerror, we have no payload to write - fix
	 * rlen to be the correct length 
	 */
	/* I Think this is bogus, we should somehow figure out
	 * a way to write out the payload even if it is gibberish -- Perry */
	if (payload == NULL) {
		dag_hdr->rlen = htons(dag_record_size + pad);
		
	} 
	
	if (packet->trace->format == &erf  
#ifdef HAVE_DAG
			|| packet->trace->format == &dag 
#endif
			) {
		numbytes = erf_dump_packet(libtrace,
				(dag_record_t *)packet->header,
				pad,
				payload
				);
	} else {
		dag_record_t erfhdr;
		/* convert format - build up a new erf header */
		/* Timestamp */
		erfhdr.ts = bswap_host_to_le64(trace_get_erf_timestamp(packet));

		/* Flags. Can't do this */
		memset(&erfhdr.flags,1,sizeof(erfhdr.flags));
		if (trace_get_direction(packet)!=~0U)
			erfhdr.flags.iface = trace_get_direction(packet);

		if (!find_compatible_linktype(libtrace,packet))
			return -1;

		payload=packet->payload;
		pad = erf_get_padding(packet);

		erfhdr.type = libtrace_to_erf_type(trace_get_link_type(packet));

		/* Packet length (rlen includes format overhead) */
		assert(trace_get_capture_length(packet)>0 
				&& trace_get_capture_length(packet)<=65536);
		assert(erf_get_framing_length(packet)>0 
				&& trace_get_framing_length(packet)<=65536);
		assert(
			trace_get_capture_length(packet)+erf_get_framing_length(packet)>0
		      &&trace_get_capture_length(packet)+erf_get_framing_length(packet)<=65536);
		erfhdr.rlen = htons(trace_get_capture_length(packet) 
			+ erf_get_framing_length(packet));
		/* loss counter. Can't do this */
		erfhdr.lctr = 0;
		/* Wire length, does not include padding! */
		erfhdr.wlen = htons(trace_get_wire_length(packet));

		/* Write it out */
		numbytes = erf_dump_packet(libtrace,
				&erfhdr,
				pad,
				payload);
	}
	return numbytes;
}

static libtrace_linktype_t erf_get_link_type(const libtrace_packet_t *packet) {
	dag_record_t *erfptr = 0;
	erfptr = (dag_record_t *)packet->header;
	return erf_type_to_libtrace(erfptr->type);
}

static libtrace_direction_t erf_get_direction(const libtrace_packet_t *packet) {
	dag_record_t *erfptr = 0;
	erfptr = (dag_record_t *)packet->header;
	return erfptr->flags.iface;
}

static libtrace_direction_t erf_set_direction(libtrace_packet_t *packet, libtrace_direction_t direction) {
	dag_record_t *erfptr = 0;
	erfptr = (dag_record_t *)packet->header;
	erfptr->flags.iface = direction;
	return erfptr->flags.iface;
}

static uint64_t erf_get_erf_timestamp(const libtrace_packet_t *packet) {
	dag_record_t *erfptr = 0;
	erfptr = (dag_record_t *)packet->header;
	return bswap_le_to_host64(erfptr->ts);
}

static int erf_get_capture_length(const libtrace_packet_t *packet) {
	dag_record_t *erfptr = 0;
	int caplen;
	if (packet->payload == NULL)
		return 0; 
	
	erfptr = (dag_record_t *)packet->header;
	caplen = ntohs(erfptr->rlen) - erf_get_framing_length(packet);
	if (ntohs(erfptr->wlen) < caplen)
		return ntohs(erfptr->wlen);

	return (ntohs(erfptr->rlen) - erf_get_framing_length(packet));
}

static int erf_get_wire_length(const libtrace_packet_t *packet) {
	dag_record_t *erfptr = 0;
	erfptr = (dag_record_t *)packet->header;
	return ntohs(erfptr->wlen);
}

static size_t erf_set_capture_length(libtrace_packet_t *packet, size_t size) {
	dag_record_t *erfptr = 0;
	assert(packet);
	if(size  > trace_get_capture_length(packet)) {
		/* can't make a packet larger */
		return trace_get_capture_length(packet);
	}
	erfptr = (dag_record_t *)packet->header;
	erfptr->rlen = htons(size + erf_get_framing_length(packet));
	return trace_get_capture_length(packet);
}

#ifdef HAVE_DAG
static libtrace_eventobj_t trace_event_dag(libtrace_t *trace, 
					libtrace_packet_t *packet) {
        libtrace_eventobj_t event = {0,0,0.0,0};
        int dag_fd;
        int data;

        if (trace->format->get_fd) {
                dag_fd = trace->format->get_fd(trace);
        } else {
                dag_fd = 0;
        }
	
	data = dag_read(trace, DAGF_NONBLOCK);

        if (data > 0) {
                event.size = dag_read_packet(trace,packet);
                if (trace->filter) {
			if (trace_apply_filter(trace->filter, packet)) {
				event.type = TRACE_EVENT_PACKET;
			} else {
        			event.type = TRACE_EVENT_SLEEP;
        			event.seconds = 0.000001;
				return event;
			}
		}	
		if (trace->snaplen > 0) {
			trace_set_capture_length(packet, trace->snaplen);
		}

		return event;
        }
        event.type = TRACE_EVENT_SLEEP;
        event.seconds = 0.0001;
        return event;
}
#endif

#ifdef HAVE_DAG
static void dag_help(void) {
	printf("dag format module: $Revision$\n");
	printf("Supported input URIs:\n");
	printf("\tdag:/dev/dagn\n");
	printf("\n");
	printf("\te.g.: dag:/dev/dag0\n");
	printf("\n");
	printf("Supported output URIs:\n");
	printf("\tnone\n");
	printf("\n");
}
#endif

static void erf_help(void) {
	printf("erf format module: $Revision$\n");
	printf("Supported input URIs:\n");
	printf("\terf:/path/to/file\t(uncompressed)\n");
	printf("\terf:/path/to/file.gz\t(gzip-compressed)\n");
	printf("\terf:-\t(stdin, either compressed or not)\n");
	printf("\terf:/path/to/socket\n");
	printf("\n");
	printf("\te.g.: erf:/tmp/trace\n");
	printf("\n");
	printf("Supported output URIs:\n");
	printf("\terf:path/to/file\t(uncompressed)\n");
	printf("\terf:/path/to/file.gz\t(gzip-compressed)\n");
	printf("\terf:-\t(stdout, either compressed or not)\n");
	printf("\n");
	printf("\te.g.: erf:/tmp/trace\n");
	printf("\n");
	printf("Supported output options:\n");
	printf("\t-z\tSpecify the gzip compression, ranging from 0 (uncompressed) to 9 - defaults to 1\n");
	printf("\n");

	
}

static struct libtrace_format_t erf = {
	"erf",
	"$Id$",
	TRACE_FORMAT_ERF,
	erf_init_input,			/* init_input */	
	NULL,				/* config_input */
	erf_start_input,		/* start_input */
	NULL,				/* pause_input */
	erf_init_output,		/* init_output */
	erf_config_output,		/* config_output */
	erf_start_output,		/* start_output */
	erf_fin_input,			/* fin_input */
	erf_fin_output,			/* fin_output */
	erf_read_packet,		/* read_packet */
	NULL,				/* fin_packet */
	erf_write_packet,		/* write_packet */
	erf_get_link_type,		/* get_link_type */
	erf_get_direction,		/* get_direction */
	erf_set_direction,		/* set_direction */
	erf_get_erf_timestamp,		/* get_erf_timestamp */
	NULL,				/* get_timeval */
	NULL,				/* get_seconds */
	erf_seek_erf,			/* seek_erf */
	NULL,				/* seek_timeval */
	NULL,				/* seek_seconds */
	erf_get_capture_length,		/* get_capture_length */
	erf_get_wire_length,		/* get_wire_length */
	erf_get_framing_length,		/* get_framing_length */
	erf_set_capture_length,		/* set_capture_length */
	NULL,				/* get_fd */
	trace_event_trace,		/* trace_event */
	erf_help,			/* help */
	NULL				/* next pointer */
};

#ifdef HAVE_DAG
static struct libtrace_format_t dag = {
	"dag",
	"$Id$",
	TRACE_FORMAT_ERF,
	dag_init_input,			/* init_input */	
	dag_config_input,		/* config_input */
	dag_start_input,		/* start_input */
	dag_pause_input,		/* pause_input */
	NULL,				/* init_output */
	NULL,				/* config_output */
	NULL,				/* start_output */
	dag_fin_input,			/* fin_input */
	NULL,				/* fin_output */
	dag_read_packet,		/* read_packet */
	NULL,				/* fin_packet */
	NULL,				/* write_packet */
	erf_get_link_type,		/* get_link_type */
	erf_get_direction,		/* get_direction */
	erf_set_direction,		/* set_direction */
	erf_get_erf_timestamp,		/* get_erf_timestamp */
	NULL,				/* get_timeval */
	NULL,				/* get_seconds */
	NULL,				/* seek_erf */
	NULL, 				/* seek_timeval */
	NULL, 				/* seek_seconds */
	erf_get_capture_length,		/* get_capture_length */
	erf_get_wire_length,		/* get_wire_length */
	erf_get_framing_length,		/* get_framing_length */
	erf_set_capture_length,		/* set_capture_length */
	NULL,				/* get_fd */
	trace_event_dag,		/* trace_event */
	dag_help,			/* help */
	NULL				/* next pointer */
};
#endif

void erf_constructor(void) {
	register_format(&erf);
#ifdef HAVE_DAG
	register_format(&dag);
#endif
}

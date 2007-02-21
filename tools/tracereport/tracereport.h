#ifndef TRACEREPORT_H
#define TRACEREPORT_H

typedef struct {
	uint64_t count;
	uint64_t bytes;
} stat_t;

typedef enum {
	REPORT_TYPE_ERROR = 1,
	REPORT_TYPE_FLOW = 1 << 1,
	REPORT_TYPE_TOS = 1 << 2,
	REPORT_TYPE_PROTO = 1 << 3,
	REPORT_TYPE_PORT = 1 << 4,
	REPORT_TYPE_TTL = 1 << 5,
	REPORT_TYPE_TCPOPT = 1 << 6,
	REPORT_TYPE_NLP = 1 << 7,
	REPORT_TYPE_DIR = 1 << 8,
	REPORT_TYPE_ECN = 1 << 9,
	REPORT_TYPE_TCPSEG = 1 << 10
} report_type_t;

#endif

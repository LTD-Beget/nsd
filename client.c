/*
 * client.c -- contains all client (nsdc/nsd-xfer) code
 *
 * Copyright (c) 2001-2005, NLnet Labs. All rights reserved.
 *
 * See LICENSE for the license.
 *
 */

#include <config.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <assert.h>
#include <errno.h>
#include <netdb.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include "dname.h"
#include "dns.h"
#include "packet.h"
#include "query.h"
#include "rdata.h"
#include "region-allocator.h"
#include "tsig.h"
#include "tsig-openssl.h"
#include "util.h"
#include "zonec.h"
#include "client.h"

void
error(int exitcode, const char *format, ...)
{
        va_list args;
        va_start(args, format);
        log_vmsg(LOG_ERR, format, args);
        va_end(args);
        exit(exitcode);
}

void
warning(const char *format, ...)
{
        va_list args;
        va_start(args, format);
        log_vmsg(LOG_WARNING, format, args);
        va_end(args);
}

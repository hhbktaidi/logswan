/*****************************************************************************/
/*                                                                           */
/* Logswan 1.05                                                              */
/* Copyright (c) 2015-2016, Frederic Cambus                                  */
/* http://www.logswan.org                                                    */
/*                                                                           */
/* Created:      2015-05-31                                                  */
/* Last Updated: 2016-02-25                                                  */
/*                                                                           */
/* Logswan is released under the BSD 3-Clause license.                       */
/* See LICENSE file for details.                                             */
/*                                                                           */
/*****************************************************************************/

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <err.h>
#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#ifndef HAVE_PLEDGE
#include "pledge.h"
#endif

#ifndef HAVE_STRTONUM
#include "strtonum.h"
#endif

#include "hll.h"

#include <GeoIP.h>

#include "config.h"
#include "output.h"
#include "parse.h"

void displayUsage() {
	printf("USAGE : logswan [options] inputfile\n\n" \
	       "Options are :\n\n" \
	       "	-h Display usage\n" \
	       "	-v Display version\n\n");
}

int main (int argc, char *argv[]) {
	GeoIP *geoip, *geoipv6;

	clock_t begin, end;

	char lineBuffer[LINE_MAX_LENGTH];

	Results results;
	struct date parsedDate;
	struct logLine parsedLine;
	struct request parsedRequest;

	struct sockaddr_in ipv4;
	struct sockaddr_in6 ipv6;
	uint32_t isIPv4 = 0, isIPv6 = 0;

	uint64_t bandwidth;
	uint32_t statusCode;
	uint32_t hour;
	uint32_t countryId = 0;

	struct stat logFileSize;
	FILE *logFile;

	const char *errstr;

	int8_t getoptFlag;

	struct HLL uniqueIPv4, uniqueIPv6;
	char *intputFile;

	if (pledge("stdio rpath", NULL) == -1) {
		err(EXIT_FAILURE, "pledge");
	}

	hll_init(&uniqueIPv4, HLL_BITS);
	hll_init(&uniqueIPv6, HLL_BITS);

	while ((getoptFlag = getopt(argc, argv, "hv")) != -1) {
		switch(getoptFlag) {
		case 'h':
			displayUsage();
			return EXIT_SUCCESS;

		case 'v':
			printf("%s\n\n", VERSION);
			return EXIT_SUCCESS;
		}
	}

	if (optind < argc) {
		intputFile = argv[optind];
	} else {
		displayUsage();
		return EXIT_SUCCESS;
	}

	argc -= optind;
	argv += optind;

	/* Starting timer */
	begin = clock();

	/* Initializing GeoIP */
	geoip = GeoIP_open(GEOIPDIR "GeoIP.dat", GEOIP_MEMORY_CACHE);
	geoipv6 = GeoIP_open(GEOIPDIR "GeoIPv6.dat", GEOIP_MEMORY_CACHE);

	/* Get log file size */
	stat(intputFile, &logFileSize);
	results.fileName = intputFile;
	results.fileSize = (uint64_t)logFileSize.st_size;

	/* Open log file */
	if (!strcmp(intputFile, "-")) {
		/* Read from standard input */
		logFile = stdin;
	} else {
		/* Attempt to read from file */
		if (!(logFile = fopen(intputFile, "r"))) {
			perror("Can't open log file");
			return EXIT_FAILURE;
		}
	}

	while (fgets(lineBuffer, LINE_MAX_LENGTH, logFile)) {
		/* Parse and tokenize line */
		parseLine(&parsedLine, lineBuffer);

		/* Detect if remote host is IPv4 or IPv6 */
		if (parsedLine.remoteHost) { /* Do not feed NULL tokens to inet_pton */
			isIPv4 = inet_pton(AF_INET, parsedLine.remoteHost, &(ipv4.sin_addr));
			isIPv6 = inet_pton(AF_INET6, parsedLine.remoteHost, &(ipv6.sin6_addr));
		}

		if (isIPv4 || isIPv6) {
			if (isIPv4) {
				/* Increment hits counter */
				results.hitsIPv4++;

				/* Unique visitors */
				hll_add(&uniqueIPv4, parsedLine.remoteHost, strlen(parsedLine.remoteHost));

				if (geoip) {
					countryId = GeoIP_id_by_addr(geoip, parsedLine.remoteHost);
				}
			}

			if (isIPv6) {
				/* Increment hits counter */
				results.hitsIPv6++;

				/* Unique visitors */
				hll_add(&uniqueIPv6, parsedLine.remoteHost, strlen(parsedLine.remoteHost));

				if (geoipv6) {
					countryId = GeoIP_id_by_addr_v6(geoipv6, parsedLine.remoteHost);
				}
			}

			/* Increment countries array */
			results.countries[countryId]++;

			/* Increment continents array */
			for (size_t loop = 0; loop<CONTINENTS; loop++) {
				if (!strcmp(continentsId[loop], GeoIP_continent_by_id(countryId))) {
					results.continents[loop] ++;
					break;
				}
			}

			/* Hourly distribution */
			if (parsedLine.date) {
				parseDate(&parsedDate, parsedLine.date);

				if (parsedDate.hour) {
					hour = strtonum(parsedDate.hour, 0, 23, &errstr);

					if (!errstr) {
						results.hours[hour] ++;
					}
				}
			}

			/* Parse request */
			if (parsedLine.request) {
				parseRequest(&parsedRequest, parsedLine.request);

				if (parsedRequest.method) {
					for (size_t loop = 0; loop<METHODS; loop++) {
						if (!strcmp(methods[loop], parsedRequest.method)) {
							results.methods[loop] ++;
							break;
						}
					}
				}

				if (parsedRequest.protocol) {
					for (size_t loop = 0; loop<PROTOCOLS; loop++) {
						if (!strcmp(protocols[loop], parsedRequest.protocol)) {
							results.protocols[loop] ++;
							break;
						}
					}
				}
			}

			/* Count HTTP status codes occurences */
			if (parsedLine.statusCode) {
				statusCode = strtonum(parsedLine.statusCode, 0, STATUS_CODE_MAX-1, &errstr);

				if (!errstr) {
					results.status[statusCode] ++;
				}
			}

			/* Increment bandwidth usage */
			if (parsedLine.objectSize) {
				bandwidth = strtonum(parsedLine.objectSize, 0, INT64_MAX, &errstr);

				if (!errstr) {
					results.bandwidth += bandwidth;
				}
			}
		} else {
			/* Invalid line */
			results.invalidLines++;
		}
	}

	/* Counting hits and processed lines */
	results.hits = results.hitsIPv4 + results.hitsIPv6;
	results.processedLines = results.hits + results.invalidLines;

	/* Counting unique visitors */
	results.visitsIPv4 = hll_count(&uniqueIPv4);
	results.visitsIPv6 = hll_count(&uniqueIPv6);
	results.visits = results.visitsIPv4 + results.visitsIPv6;

	/* Stopping timer */
	end = clock();
	results.runtime = (double)(end - begin) / CLOCKS_PER_SEC;

	/* Generate timestamp */
	time_t now = time(NULL);
	strftime(results.timeStamp, 20, "%Y-%m-%d %H:%M:%S", localtime(&now));

	/* Printing results */
	fprintf(stderr, "Processed %" PRIu64 " lines in %f seconds\n", results.processedLines, results.runtime);
	fclose(logFile);

	fputs(output(results), stdout);

	GeoIP_delete(geoip);
	GeoIP_delete(geoipv6);

	hll_destroy(&uniqueIPv4);
	hll_destroy(&uniqueIPv6);

	return EXIT_SUCCESS;
}

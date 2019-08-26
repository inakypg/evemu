/*****************************************************************************
 *
 * evemu - Kernel device emulation
 *
 * Copyright (C) 2011 Red Hat, Inc.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 ****************************************************************************/
#define _GNU_SOURCE

#include "evemu.h"
#include <getopt.h>
#include <limits.h>
#include <stdio.h>
#include <fcntl.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <stdlib.h>
#include <linux/input.h>
#include <libevdev/libevdev.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include "argz.h"

static struct option opts[] = {
	{ "type", required_argument, 0, 't'},
	{ "code", required_argument, 0, 'c'},
	{ "value", required_argument, 0, 'v'},
	{ "sync", no_argument, 0, 's'},
	{ "device", required_argument, 0, 'd'},
	{ "fifo", optional_argument, 0, 'f'}
};

static const int MAX_DEV_PATH_LENGTH = 100;

static int parse_arg(const char *arg, long int *value)
{
	char *endp;

	*value = strtol(arg, &endp, 0);
	if (*arg == '\0' || *endp != '\0')
		return 1;
	return 0;
}

static int parse_type(const char *arg, long int *value)
{
	*value = libevdev_event_type_from_name(arg);
	if (*value != -1)
		return 0;

	return parse_arg(arg, value);
}

static int parse_code(long int type, const char *arg, long int *value)
{
	*value = libevdev_event_code_from_name(type, arg);
	if (*value != -1)
		return 0;

	return parse_arg(arg, value);
}

// TODO: Add --fifo usage.
static void usage(void)
{
	fprintf(stderr, "Usage: %s [--sync] <device> --type <type> --code <code> --value <value>\n\n", program_invocation_short_name);
	fprintf(stderr, "Program also support create a fifo, then write your event to that fifo file.\n"
					"Usage: %s --fifo=<file_name>\n", program_invocation_short_name);
}

int ev_from_args(const char* path, long type, long code, long value, int sync)
{
	int rc = -1;
	int fd = -1;
	struct input_event ev;

	fd = open(path, O_WRONLY);
	if (fd < 0) {
		fprintf(stderr, "error: could not open device (%m)\n");
		goto out;
	}
    
	if (evemu_create_event(&ev, type, code, value)) {
		fprintf(stderr, "error: failed to create event\n");
		goto out;
	}

	if (evemu_play_one(fd, &ev)) {
		fprintf(stderr, "error: could not play event\n");
		goto out;
	}

	if (sync) {
		if (evemu_create_event(&ev, EV_SYN, SYN_REPORT, 0)) {
			fprintf(stderr, "error: could not create SYN event\n");
			goto out;
		}
		if (evemu_play_one(fd, &ev)) {
			fprintf(stderr, "error: could not play SYN event\n");
			goto out;
		}
	}

	rc = 0;
out:
	if (fd > -1)
		close(fd);
	return rc;
}


/*
 * Read lines from the fifo @path
 *
 *
 * Each line is in the format:
 *
 * - <DEVICE> <TYPE> <CODE> <VALUE> [SYNC]
 * - WAIT <MILLISECS>
 * - empty (ignored)
 *
 * DEVICE: an absolute name for an input device; if relative,
 *   /dev/input will be prefixed
 *
 * TYPE: type of event (EV_xyz)
 * 
 * CODE: code of the event type (eg: SYN_xyz, KEY_xyz, BTN_xyz,
 *   REL_xyz, ABS_xyz, MSC_xyz, SND_xyz, SW_xyz, LED_xyz, REP_xyz,
 *   FF_xyz...
 *
 * VALUE: numeric value to set (0 to release, 1 to press, etc)
 * 
 * SYNC: the event shall carry the sync flag
 */ 
int read_fifo(const char* path)
{
	int rc = -1;
	FILE * fp;
	char * line = NULL;
	size_t len = 0;
	ssize_t read;

	mkfifo(path, 0666);

	fp = fopen(path, "r");
	if (fp == NULL)
		goto out;

	while (1) {
		read = getline(&line, &len, fp);		
		if (read == -1) {
			/* closed (calling process exited, let's
			 * reopen and chug along for the next client */
			fp = fopen(path, "r");
			continue;
		}
			
		char* result;
		size_t argz_len;
		int sync = 0;
		
		// Reset rc
		rc = -1;

		// Replace the last character to '0' before proceed.
		line[strlen(line)-1] = '\0';

		// Extract the arguments
		argz_create_sep(line, ' ', &result, &argz_len);
		int argc = argz_count(result, argz_len);
		char *argv[argc + 1];
 		argz_extract(result, argz_len, argv);

		if (argc == 0)
			continue;
		if (strcmp(argv[0], "WAIT") == 0) {
			float wait_time = atof(argv[1]);
			struct timespec ts;
			ts.tv_sec = (int) wait_time;
			ts.tv_nsec = (wait_time - ts.tv_sec) * 1000000;
			nanosleep(&ts, NULL);
			continue;
		}
		if (argc != 4 && argc != 5)
			continue;
		
		// DEVICE TYPE CODE VALUE
		long int type, code, value = LONG_MAX;

		// if the device is just a file name, prepend
		// /dev/input to it, otherwise it is a path name use
		// as is
		char *dev_path;
		if (strcmp(basename(argv[0]), argv[0]))
			dev_path = argv[0];
		else {
			dev_path = alloca(strlen("/dev/input/")
					  + strlen(argv[0]) + 1);
			strcpy(dev_path, "/dev/input/");
			strcpy(dev_path + strlen("/dev/input/"), argv[0]);
		}

		if (parse_type(argv[1], &type)) {	// Parse type
			fprintf(stderr, "error: invalid type argument '%s'\n", argv[1]);
			continue;
		}
	
		if (parse_code(type, argv[2], &code)) {	// Parse code
			fprintf(stderr, "error: invalid code argument '%s'\n", argv[2]);
			continue;
		}

		// Parse value
		if (parse_arg(argv[3], &value) || value < INT_MIN || value > INT_MAX) {
			fprintf(stderr, "error: invalid value argument '%s'\n", argv[3]);
			continue;
		}

		if(argc == 5 && strcmp(argv[4], "SYNC") == 0)
			sync = 1;
		rc = ev_from_args(dev_path, type, code, value, sync);
		fprintf(stderr, "sent: %d\n", rc);
	}
out:
	return rc;
}

int main(int argc, char *argv[])
{
	int rc = -1;
	int fd = -1;
	long int type, code, value = LONG_MAX;
	struct input_event ev;
	int sync = 0;
	const char *path = NULL;
	const char *fifo_path = NULL;
	const char *code_arg = NULL, *type_arg = NULL;

	while(1) {
		int option_index = 0;
		int c;

		c = getopt_long(argc, argv, "", opts, &option_index);
		if (c == -1) /* we only do long options */
			break;

		switch(c) {
			case 't': /* type */
				type_arg = optarg;
				break;
			case 'c': /* code */
				code_arg = optarg;
				break;
			case 'v': /* value */
				if (parse_arg(optarg, &value) || value < INT_MIN || value > INT_MAX) {
					fprintf(stderr, "error: invalid value argument '%s'\n", optarg);
					goto out;
				}
				break;
			case 'd': /* device */
				path = optarg;
				break;
			case 's': /* sync */
				sync = 1;
				break;
			case 'f': /* fifo */
				fifo_path = optarg;
				break;
			default:
				usage();
				goto out;
		}
	}

	/* If fifo is provided, then discard the rest of the arguments. */
	if(fifo_path != NULL) {
		rc = read_fifo(fifo_path);
		goto out;
	}

	if (argc < 5) {
		usage();
		goto out;
	}

	if (!type_arg || !code_arg || value == LONG_MAX) {
		usage();
		goto out;
	}

	if (parse_type(type_arg, &type)) {
		fprintf(stderr, "error: invalid type argument '%s'\n", type_arg);
		goto out;
	}

	if (parse_code(type, code_arg, &code)) {
		fprintf(stderr, "error: invalid code argument '%s'\n", code_arg);
		goto out;
	}

	/* if device wasn't specified as option, take the remaining arg */
	if (optind < argc) {
		if (argc - optind != 1 || path) {
			usage();
			goto out;
		}
		path = argv[optind];
	}

	if (!path) {
		fprintf(stderr, "error: missing device path\n");
		usage();
		goto out;
	}

	rc = ev_from_args(path, type, code, value, sync);
out:
	if (fd > -1)
		close(fd);
	return rc;
}

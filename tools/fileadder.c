/*
 * Places a data file at a given address of a flash image, optionally
 * resizing the image first. The build uses it to put the startup
 * configuration into the image:
 *
 *   fileadder -a ADDRESS -s SIZE -d FILE IMAGE
 *
 * The image is overwritten unless -o names another output file. The data
 * is followed by a NUL byte.
 */
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <argp.h>
#include <stdbool.h>

// Use a 4MB buffer, the same as the largest flash rom
#define BUFFER_SIZE 0x400000
static char buffer[BUFFER_SIZE];

const char *argp_program_version = "fileadder 0.2";
static char doc[] = "Adds a file into a flash image";
static char args_doc[] = "INPUT_IMAGE";
static struct argp_option options[] = {
	{ "size", 's', "SIZE", 0, "Resize image" },
	{ "output", 'o', "FILE", 0, "Output image file name instead of overwriting input image" },
	{ "data", 'd', "FILE", 0, "File to add to the image" },
	{ "address", 'a', "ADDRESS", 0, "Address where the data is placed" },
	{ 0 }
};

struct arguments {
	long size, address;
	char *output_file;
	char *data_file;
};

static error_t parse_opt(int key, char *arg, struct argp_state *state)
{
	struct arguments *a = state->input;

	switch (key) {
	case 'a':
		a->address = strtol(arg, NULL, 0);
		break;
	case 's':
		a->size = strtol(arg, NULL, 0);
		break;
	case 'o':
		a->output_file = arg;
		break;
	case 'd':
		a->data_file = arg;
		break;
	default:
		return ARGP_ERR_UNKNOWN;
	}
	return 0;
}

static struct argp argp = { options, parse_opt, args_doc, doc, 0, 0, 0 };

int main(int argc, char **argv)
{
	struct arguments a = { 0, 0, NULL, NULL };
	char tmpfilename[] = "image_XXXXXX";
	int arg_index, fd;
	long filesize, n;
	FILE *f;

	argp_parse(&argp, argc, argv, 0, &arg_index, &a);
	if (arg_index >= argc || !a.data_file) {
		fprintf(stderr, "usage: fileadder -a ADDRESS [-s SIZE] -d FILE IMAGE\n");
		return 5;
	}

	f = fopen(argv[arg_index], "rb");
	if (!f) {
		perror(argv[arg_index]);
		return 5;
	}
	filesize = fread(buffer, 1, sizeof(buffer), f);
	fclose(f);
	if (a.size)
		filesize = a.size;
	if (filesize > BUFFER_SIZE || a.address < 0 || a.address >= BUFFER_SIZE - 1) {
		fprintf(stderr, "image or address too large\n");
		return 5;
	}

	fd = open(a.data_file, O_RDONLY);
	if (fd < 0) {
		perror(a.data_file);
		return 5;
	}
	n = read(fd, &buffer[a.address], BUFFER_SIZE - 1 - a.address);
	close(fd);
	if (n < 0) {
		perror(a.data_file);
		return 5;
	}
	buffer[a.address + n] = '\0';
	printf("Data inserted at 0x%lx, size: %ld\n", a.address, n + 1);

	fd = a.output_file ? creat(a.output_file, 0660) : mkstemp(tmpfilename);
	if (fd < 0 || write(fd, buffer, filesize) != filesize) {
		perror("writing the image");
		return 5;
	}
	close(fd);
	if (!a.output_file)
		rename(tmpfilename, argv[arg_index]);
	return 0;
}

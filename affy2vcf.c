/* The MIT License

   Copyright (c) 2018-2020 Giulio Genovese

   Author: Giulio Genovese <giulio.genovese@gmail.com>

   Permission is hereby granted, free of charge, to any person obtaining a copy
   of this software and associated documentation files (the "Software"), to deal
   in the Software without restriction, including without limitation the rights
   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
   copies of the Software, and to permit persons to whom the Software is
   furnished to do so, subject to the following conditions:

   The above copyright notice and this permission notice shall be included in
   all copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
   THE SOFTWARE.

 */

#include <getopt.h>
#include <errno.h>
#include <wchar.h>
#include <sys/resource.h>
#include <arpa/inet.h>
#include <htslib/hfile.h>
#include <htslib/faidx.h>
#include <htslib/vcf.h>
#include <htslib/kseq.h>
#include <htslib/sam.h>
#include "bcftools.h"
#include "htslib/khash_str2int.h"
#include "gtc2vcf.h"

#define AFFY2VCF_VERSION "2020-05-26"

#define GT_NC -1
#define GT_AA 0
#define GT_AB 1
#define GT_BB 2

#define VERBOSE (1 << 0)
#define LOAD_CEL (1 << 1)
#define CALLS_LOADED (1 << 2)
#define CONFIDENCES_LOADED (1 << 3)
#define SUMMARY_LOADED (1 << 4)
#define MODELS_LOADED (1 << 5)
#define ADJUST_CLUSTERS (1 << 6)

/****************************************
 * READING ROUTINES                     *
 ****************************************/


// tests the end-of-file indicator for an hFILE
static inline int heof(hFILE *fp)
{
	if (hgetc(fp) == EOF)
		return 1;
	fp->begin--;
	return 0;
}

// read or skip a fixed number of bytes
static inline void read_bytes(hFILE *fp, void *buffer, size_t nbytes)
{
	if (buffer) {
		if (hread(fp, buffer, nbytes) < nbytes) {
			error("Failed to read %ld bytes from stream\n", nbytes);
		}
	} else {
		int c = 0;
		for (int i = 0; i < nbytes; i++)
			c = hgetc(fp);
		if (c == EOF)
			error("Failed to reposition stream forward %ld bytes\n", nbytes);
	}
}

static inline int is_gzip(hFILE *fp)
{
	uint8_t buffer[2];
	if (hpeek(fp, (void *)buffer, 2) < 2) {
		error("Failed to read 2 bytes from stream\n");
	}
	return (buffer[0] == 0x1f && buffer[1] == 0x8b);
}

static inline uint32_t read_long(hFILE *fp)
{
	uint32_t value;
	read_bytes(fp, (void *)&value, sizeof(uint32_t));
	value = ntohl(value);
	return value;
}

static inline float read_float(hFILE *fp)
{
	union {
		uint32_t u;
		float f;
	} convert;
	read_bytes(fp, (void *)&convert.u, sizeof(uint32_t));
	convert.u = ntohl(convert.u);
	return convert.f;
}

static inline int32_t read_string8(hFILE *fp, char **buffer)
{
	int32_t len = (int32_t)read_long(fp);
	if (len) {
		*buffer = (char *)malloc((1 + len) * sizeof(char));
		read_bytes(fp, (void *)*buffer, len * sizeof(char));
		(*buffer)[len] = '\0';
	} else {
		*buffer = NULL;
	}
	return len;
}

static inline int32_t read_string16(hFILE *fp, wchar_t **buffer)
{
	int32_t len = (int32_t)read_long(fp);
	if (len) {
		*buffer = (wchar_t *)malloc((1 + len) * sizeof(wchar_t));
		for (int i = 0; i < len; i++) {
			uint16_t cvalue;
			read_bytes(fp, (void *)&cvalue, sizeof(unsigned short));
			(*buffer)[i] = (wchar_t)ntohs(cvalue);
		}
		(*buffer)[len] = L'\0';
	} else {
		*buffer = NULL;
	}
	return len;
}

/****************************************
 * CEL FILE IMPLEMENTATION              *
 ****************************************/

// http://www.affymetrix.com/support/developer/powertools/changelog/gcos-agcc/index.html

typedef struct {
	float mean __attribute__((packed));
	float dev __attribute__((packed));
	int16_t N;
} Cell;

typedef struct {
	int16_t x;
	int16_t y;
} Entry;

typedef struct {
	int32_t row;
	int32_t col;
	float upper_left_x;
	float upper_left_y;
	float upper_right_x;
	float upper_right_y;
	float lower_left_x;
	float lower_left_y;
	float lower_right_x;
	float lower_right_y;
	int32_t left_cell;
	int32_t top_cell;
	int32_t right_cell;
	int32_t bottom_cell;
} SubGrid;

typedef struct {
	char *fn;
	hFILE *fp;
	int32_t version;
	int32_t num_rows;
	int32_t num_cols;
	int32_t num_cells;
	int32_t n_header;
	char *header;
	int32_t n_algorithm;
	char *algorithm;
	int32_t n_parameters;
	char *parameters;
	int32_t cell_margin;
	uint32_t num_outlier_cells;
	uint32_t num_masked_cells;
	int32_t num_sub_grids;
	Cell *cells;
	Entry *masked_entries;
	Entry *outlier_entries;
	SubGrid *sub_grids;
} xda_cel_t;

static xda_cel_t *xda_cel_init(const char *fn, hFILE *fp, int flags)
{
	xda_cel_t *xda_cel = (xda_cel_t *)calloc(1, sizeof(xda_cel_t));
	xda_cel->fn = strdup(fn);
	xda_cel->fp = fp;

	int32_t magic;
	read_bytes(xda_cel->fp, (void *)&magic, sizeof(int32_t));
	if (magic != 64)
		error("XDA CEL file %s magic number is %d while it should be 64\n", xda_cel->fn,
		      magic);

	read_bytes(xda_cel->fp, (void *)&xda_cel->version, sizeof(int32_t));
	if (xda_cel->version != 4)
		error("Cannot read XDA CEL file %s. Unsupported XDA CEL file format version: %d\n",
		      xda_cel->fn, xda_cel->version);

	read_bytes(xda_cel->fp, (void *)&xda_cel->num_rows, sizeof(int32_t));
	read_bytes(xda_cel->fp, (void *)&xda_cel->num_cols, sizeof(int32_t));
	read_bytes(xda_cel->fp, (void *)&xda_cel->num_cells, sizeof(int32_t));

	read_bytes(xda_cel->fp, (void *)&xda_cel->n_header, sizeof(int32_t));
	xda_cel->header = (char *)malloc((1 + xda_cel->n_header) * sizeof(char));
	read_bytes(xda_cel->fp, (void *)xda_cel->header, xda_cel->n_header * sizeof(char));
	xda_cel->header[xda_cel->n_header] = '\0';

	read_bytes(xda_cel->fp, (void *)&xda_cel->n_algorithm, sizeof(int32_t));
	xda_cel->algorithm = (char *)malloc((1 + xda_cel->n_algorithm) * sizeof(char));
	read_bytes(xda_cel->fp, (void *)xda_cel->algorithm,
		   xda_cel->n_algorithm * sizeof(char));
	xda_cel->algorithm[xda_cel->n_algorithm] = '\0';

	read_bytes(xda_cel->fp, (void *)&xda_cel->n_parameters, sizeof(int32_t));
	xda_cel->parameters = (char *)malloc((1 + xda_cel->n_parameters) * sizeof(char));
	read_bytes(xda_cel->fp, (void *)xda_cel->parameters,
		   xda_cel->n_parameters * sizeof(char));
	xda_cel->parameters[xda_cel->n_parameters] = '\0';

	read_bytes(xda_cel->fp, (void *)&xda_cel->cell_margin, sizeof(int32_t));
	read_bytes(xda_cel->fp, (void *)&xda_cel->num_outlier_cells, sizeof(uint32_t));
	read_bytes(xda_cel->fp, (void *)&xda_cel->num_masked_cells, sizeof(uint32_t));
	read_bytes(xda_cel->fp, (void *)&xda_cel->num_sub_grids, sizeof(int32_t));

	if (flags)
		return xda_cel;

	xda_cel->cells = (Cell *)malloc(xda_cel->num_cells * sizeof(Cell));
	read_bytes(xda_cel->fp, (void *)xda_cel->cells, xda_cel->num_cells * sizeof(Cell));

	xda_cel->masked_entries = (Entry *)malloc(xda_cel->num_masked_cells * sizeof(Entry));
	read_bytes(xda_cel->fp, (void *)xda_cel->masked_entries,
		   xda_cel->num_masked_cells * sizeof(Entry));

	xda_cel->outlier_entries = (Entry *)malloc(xda_cel->num_outlier_cells * sizeof(Entry));
	read_bytes(xda_cel->fp, (void *)xda_cel->outlier_entries,
		   xda_cel->num_outlier_cells * sizeof(Entry));

	xda_cel->sub_grids = (SubGrid *)malloc(xda_cel->num_sub_grids * sizeof(SubGrid));
	read_bytes(xda_cel->fp, (void *)xda_cel->sub_grids,
		   xda_cel->num_sub_grids * sizeof(SubGrid));

	if (!heof(xda_cel->fp))
		error("XDA CEL reader did not reach the end of file %s at position %ld\n",
		      xda_cel->fn, htell(xda_cel->fp));

	return xda_cel;
}

static void xda_cel_destroy(xda_cel_t *xda_cel)
{
	if (!xda_cel)
		return;
	free(xda_cel->fn);
	if (hclose(xda_cel->fp) < 0)
		error("Error closing XDA CEL file\n");
	free(xda_cel->header);
	free(xda_cel->algorithm);
	free(xda_cel->parameters);
	free(xda_cel->cells);
	free(xda_cel->masked_entries);
	free(xda_cel->outlier_entries);
	free(xda_cel->sub_grids);
	free(xda_cel);
}

static void xda_cel_print(const xda_cel_t *xda_cel, FILE *stream, int verbose)
{
	fprintf(stream, "[CEL]\n");
	fprintf(stream, "Version=3\n");
	fprintf(stream, "\n[HEADER]\n");
	fprintf(stream, "%s", xda_cel->header);
	fprintf(stream, "\n[INTENSITY]\n");
	fprintf(stream, "NumberCells=%d\n", xda_cel->num_cells);
	fprintf(stream, "CellHeader=X\tY\tMEAN\tSTDV\tNPIXELS\n");
	if (!verbose)
		fprintf(stream, "... use --verbose to visualize Cell Entries ...\n");
	else
		for (int i = 0; i < xda_cel->num_cells; i++)
			fprintf(stream, "%3d\t%3d\t%.1f\t%.1f\t%3d\n", i % xda_cel->num_cols,
				i / xda_cel->num_cols, xda_cel->cells[i].mean,
				xda_cel->cells[i].dev, xda_cel->cells[i].N);
	fprintf(stream, "\n[MASKS]\n");
	fprintf(stream, "NumberCells=%d\n", xda_cel->num_masked_cells);
	fprintf(stream, "CellHeader=X\tY\n");
	if (!verbose)
		fprintf(stream, "... use --verbose to visualize Masked Entries ...\n");
	else
		for (int i = 0; i < xda_cel->num_masked_cells; i++)
			fprintf(stream, "%d\t%d\n", xda_cel->masked_entries[i].x,
				xda_cel->masked_entries[i].y);
	fprintf(stream, "\n[OUTLIERS]\n");
	fprintf(stream, "NumberCells=%d\n", xda_cel->num_outlier_cells);
	fprintf(stream, "CellHeader=X\tY\n");
	if (!verbose)
		fprintf(stream, "... use --verbose to visualize Outlier Entries ...\n");
	else
		for (int i = 0; i < xda_cel->num_outlier_cells; i++)
			fprintf(stream, "%d\t%d\n", xda_cel->outlier_entries[i].x,
				xda_cel->outlier_entries[i].y);
	fprintf(stream, "\n[MODIFIED]\n");
	fprintf(stream, "NumberCells=0\n");
	fprintf(stream, "CellHeader=X\tY\tORIGMEAN\n");
}

/****************************************
 * CHP FILE IMPLEMENTATION              *
 ****************************************/

// http://www.affymetrix.com/support/developer/powertools/changelog/gcos-agcc/index.html

#define BYTE 0
#define UBYTE 1
#define SHORT 2
#define USHORT 3
#define INT 4
#define UINT 5
#define FLOAT 6
#define STRING 7
#define WSTRING 8

typedef struct {
	wchar_t *name;
	char *value;
	wchar_t *mime_type;
	int32_t n_value;
	int8_t type;
} Parameter;

typedef struct DataHeader DataHeader;

struct DataHeader {
	char *data_type_identifier;
	char *guid;
	wchar_t *datetime;
	wchar_t *locale;
	int32_t n_parameters;
	Parameter *parameters;
	int32_t n_parents;
	DataHeader *parents;
};

typedef struct {
	wchar_t *name;
	int8_t type;
	int32_t size;
} ColHeader;

typedef struct {
	uint32_t pos_first_element;
	uint32_t pos_next_data_set;
	wchar_t *name;
	int32_t n_parameters;
	Parameter *parameters;
	uint32_t n_cols;
	ColHeader *col_headers;
	uint32_t n_rows;
	hFILE *fp; // this should not be destroyed
	uint32_t n_buffer;
	uint32_t *col_offsets;
	char *buffer;
} DataSet;

typedef struct {
	uint32_t pos_next_data_group;
	uint32_t pos_first_data_set;
	int32_t num_data_sets;
	wchar_t *name;
	DataSet *data_sets;
} DataGroup;

typedef struct {
	wchar_t *name;
	int8_t type;
	int32_t size;
} ColumnHeader;

typedef struct {
	char *fn;
	hFILE *fp;
	uint8_t magic;
	uint8_t version;
	int32_t num_data_groups;
	uint32_t pos_first_data_group;
	DataHeader data_header;
	DataGroup *data_groups;
	off_t size;
	char *display_name;
} agcc_t;

static void agcc_read_parameters(Parameter *parameter, hFILE *fp, int flags)
{
	read_string16(fp, &parameter->name);
	parameter->n_value = read_string8(fp, &parameter->value);
	read_string16(fp, &parameter->mime_type);
	if (wcscmp(parameter->mime_type, L"text/x-calvin-integer-8") == 0)
		parameter->type = BYTE;
	else if (wcscmp(parameter->mime_type, L"text/x-calvin-unsigned-integer-8") == 0)
		parameter->type = UBYTE;
	else if (wcscmp(parameter->mime_type, L"text/x-calvin-integer-16") == 0)
		parameter->type = SHORT;
	else if (wcscmp(parameter->mime_type, L"text/x-calvin-unsigned-integer-16") == 0)
		parameter->type = USHORT;
	else if (wcscmp(parameter->mime_type, L"text/x-calvin-integer-32") == 0)
		parameter->type = INT;
	else if (wcscmp(parameter->mime_type, L"text/x-calvin-unsigned-integer-32") == 0)
		parameter->type = UINT;
	else if (wcscmp(parameter->mime_type, L"text/x-calvin-float") == 0)
		parameter->type = FLOAT;
	else if (wcscmp(parameter->mime_type, L"text/ascii") == 0)
		parameter->type = STRING;
	else if (wcscmp(parameter->mime_type, L"text/plain") == 0)
		parameter->type = WSTRING;
	else
		error("MIME type %ls not allowed\n", parameter->mime_type);

	// drop parameters that can increase the size of the header dramatically
	if (flags
	    && wcsncmp(parameter->name, L"affymetrix-algorithm-param-apt-opt-cel", 38) == 0) {
		free(parameter->name);
		parameter->name = NULL;
		parameter->n_value = 0;
		free(parameter->value);
		parameter->value = NULL;
		free(parameter->mime_type);
		parameter->mime_type = NULL;
	}
}

static void agcc_read_data_header(DataHeader *data_header, hFILE *fp, int flags)
{
	read_string8(fp, &data_header->data_type_identifier);
	read_string8(fp, &data_header->guid);
	read_string16(fp, &data_header->datetime);
	read_string16(fp, &data_header->locale);

	data_header->n_parameters = (int32_t)read_long(fp);
	data_header->parameters =
		(Parameter *)malloc(data_header->n_parameters * sizeof(Parameter));
	for (int i = 0; i < data_header->n_parameters; i++)
		agcc_read_parameters(&data_header->parameters[i], fp, flags);

	data_header->n_parents = (int32_t)read_long(fp);
	data_header->parents =
		(DataHeader *)malloc(data_header->n_parents * sizeof(DataHeader));
	for (int i = 0; i < data_header->n_parents; i++)
		agcc_read_data_header(&data_header->parents[i], fp, flags);
}

static void agcc_read_data_set(DataSet *data_set, hFILE *fp, int flags)
{
	data_set->pos_first_element = read_long(fp);
	data_set->pos_next_data_set = read_long(fp);
	read_string16(fp, &data_set->name);

	data_set->n_parameters = (int32_t)read_long(fp);
	data_set->parameters = (Parameter *)malloc(data_set->n_parameters * sizeof(Parameter));
	for (int i = 0; i < data_set->n_parameters; i++)
		agcc_read_parameters(&data_set->parameters[i], fp, flags);

	data_set->n_cols = read_long(fp);
	data_set->col_headers = (ColHeader *)malloc(data_set->n_cols * sizeof(ColHeader));
	for (int i = 0; i < data_set->n_cols; i++) {
		read_string16(fp, &data_set->col_headers[i].name);
		read_bytes(fp, (void *)&data_set->col_headers[i].type, sizeof(int8_t));
		data_set->col_headers[i].size = read_long(fp);
	}
	data_set->n_rows = read_long(fp);

	data_set->fp = fp;
	data_set->col_offsets = (uint32_t *)malloc(data_set->n_cols * sizeof(uint32_t *));
	data_set->n_buffer = 0;
	for (int i = 0; i < data_set->n_cols; i++) {
		data_set->col_offsets[i] = data_set->n_buffer;
		data_set->n_buffer += data_set->col_headers[i].size;
	}
	data_set->buffer = (char *)malloc(data_set->n_buffer * sizeof(char));

	if (data_set->pos_next_data_set)
		if (hseek(fp, data_set->pos_next_data_set, SEEK_SET) < 0)
			error("Fail to seek to position %d in AGCC CHP file\n",
			      data_set->pos_next_data_set);
}

static void agcc_read_data_group(DataGroup *data_group, hFILE *fp, int flags)
{
	data_group->pos_next_data_group = read_long(fp);
	data_group->pos_first_data_set = read_long(fp);
	data_group->num_data_sets = read_long(fp);
	read_string16(fp, &data_group->name);
	if (hseek(fp, data_group->pos_first_data_set, SEEK_SET) < 0)
		error("Fail to seek to position %d in AGCC CHP file\n",
		      data_group->pos_first_data_set);
	data_group->data_sets = (DataSet *)malloc(data_group->num_data_sets * sizeof(DataSet));
	for (int i = 0; i < data_group->num_data_sets; i++)
		agcc_read_data_set(&data_group->data_sets[i], fp, flags);
	if (data_group->pos_next_data_group)
		if (hseek(fp, data_group->pos_next_data_group, SEEK_SET) < 0)
			error("Fail to seek to position %d in AGCC CHP file\n",
			      data_group->pos_next_data_group);
}

static agcc_t *agcc_init(const char *fn, hFILE *fp, int flags)
{
	agcc_t *agcc = (agcc_t *)calloc(1, sizeof(agcc_t));
	agcc->fn = strdup(fn);
	agcc->fp = fp;

	// read File Header
	read_bytes(agcc->fp, (void *)&agcc->magic, sizeof(uint8_t));
	if (agcc->magic != 59)
		error("AGCC CHP file %s magic number is %d while it should be 59\n", agcc->fn,
		      agcc->magic);
	read_bytes(agcc->fp, (void *)&agcc->version, sizeof(uint8_t));
	if (agcc->version != 1)
		error("Cannot read AGCC CHP file %s. Unsupported AGCC CHP file format version: %d\n",
		      agcc->fn, agcc->version);
	agcc->num_data_groups = (int32_t)read_long(agcc->fp);
	agcc->pos_first_data_group = read_long(agcc->fp);

	// read Generic Data Header
	agcc_read_data_header(&agcc->data_header, agcc->fp, flags);

	// read Data Groups
	if (hseek(agcc->fp, agcc->pos_first_data_group, SEEK_SET) < 0)
		error("Fail to seek to position %d in AGCC CHP %s file\n",
		      agcc->pos_first_data_group, agcc->fn);
	agcc->data_groups = (DataGroup *)malloc(agcc->num_data_groups * sizeof(DataGroup));
	for (int i = 0; i < agcc->num_data_groups; i++)
		agcc_read_data_group(&agcc->data_groups[i], agcc->fp, flags);

	if (!heof(agcc->fp))
		error("AGCC CHP reader did not reach the end of file %s at position %ld\n",
		      agcc->fn, htell(agcc->fp));

	if (hseek(agcc->fp, 0L, SEEK_END) < 0)
		error("Fail to seek to end of AGCC CHP %s file\n", agcc->fn);
	agcc->size = htell(agcc->fp);

	char *ptr = strrchr(agcc->fn, '/') ? strrchr(agcc->fn, '/') + 1 : agcc->fn;
	agcc->display_name = strdup(ptr);
	ptr = strrchr(agcc->display_name, '.');
	if (ptr && strcmp(ptr + 1, "chp") == 0) {
		*ptr = '\0';
		ptr = strrchr(agcc->display_name, '.');
		if (ptr
		    && (strcmp(ptr + 1, "AxiomGT1") == 0 || strcmp(ptr + 1, "birdseed-v2") == 0
			|| strcmp(ptr + 1, "brlmm-p") == 0))
			*ptr = '\0';
	}

	return agcc;
}

static void agcc_destroy_parameters(Parameter *parameters, int32_t n_parameters)
{
	for (int i = 0; i < n_parameters; i++) {
		free(parameters[i].name);
		free(parameters[i].value);
		free(parameters[i].mime_type);
	}
	free(parameters);
}

static void agcc_destroy_data_header(DataHeader *data_header)
{
	free(data_header->data_type_identifier);
	free(data_header->guid);
	free(data_header->datetime);
	free(data_header->locale);
	agcc_destroy_parameters(data_header->parameters, data_header->n_parameters);
	for (int i = 0; i < data_header->n_parents; i++)
		agcc_destroy_data_header(&data_header->parents[i]);
	free(data_header->parents);
}

static void agcc_destroy_data_set(DataSet *data_set)
{
	free(data_set->name);
	agcc_destroy_parameters(data_set->parameters, data_set->n_parameters);
	for (int i = 0; i < data_set->n_cols; i++)
		free(data_set->col_headers[i].name);
	free(data_set->col_headers);
	free(data_set->col_offsets);
	free(data_set->buffer);
}

static void agcc_destroy_data_group(DataGroup *data_group)
{
	free(data_group->name);
	for (int i = 0; i < data_group->num_data_sets; i++)
		agcc_destroy_data_set(&data_group->data_sets[i]);
	free(data_group->data_sets);
}

static void agcc_destroy(agcc_t *agcc)
{
	if (!agcc)
		return;
	free(agcc->fn);
	if (hclose(agcc->fp) < 0)
		error("Error closing AGCC CHP file\n");
	agcc_destroy_data_header(&agcc->data_header);
	for (int i = 0; i < agcc->num_data_groups; i++)
		agcc_destroy_data_group(&agcc->data_groups[i]);
	free(agcc->data_groups);
	free(agcc->display_name);
	free(agcc);
}

static void agcc_print_parameters(const Parameter *parameters, int32_t n_parameters,
				  FILE *stream)
{
	union {
		uint32_t u;
		float f;
	} convert;
	wchar_t *buffer = NULL;
	size_t m_buffer = 0;
	for (int i = 0; i < n_parameters; i++) {
		fprintf(stream, "#%%%ls=", parameters[i].name ? parameters[i].name : L"");
		switch (parameters[i].type) {
		case BYTE:
			fprintf(stream, "%d\n",
				(int8_t)ntohl(*(uint32_t *)parameters[i].value));
			break;
		case UBYTE:
			fprintf(stream, "%u\n",
				(uint8_t)ntohl(*(uint32_t *)parameters[i].value));
			break;
		case SHORT:
			fprintf(stream, "%d\n",
				(int16_t)ntohl(*(uint32_t *)parameters[i].value));
			break;
		case USHORT:
			fprintf(stream, "%u\n",
				(uint16_t)ntohl(*(uint32_t *)parameters[i].value));
			break;
		case INT:
			fprintf(stream, "%d\n",
				(int32_t)ntohl(*(uint32_t *)parameters[i].value));
			break;
		case UINT:
			fprintf(stream, "%u\n", ntohl(*(uint32_t *)parameters[i].value));
			break;
		case FLOAT:
			convert.u = ntohl(*(uint32_t *)parameters[i].value);
			fprintf(stream, "%f\n", convert.f);
			break;
		case STRING:
			fprintf(stream, "%s\n", parameters[i].value);
			break;
		case WSTRING:
			hts_expand0(wchar_t, parameters[i].n_value / 2 + 1, m_buffer, buffer);
			for (int j = 0; j < parameters[i].n_value / 2; j++)
				buffer[j] =
					(wchar_t)ntohs(((uint16_t *)parameters[i].value)[j]);
			buffer[parameters[i].n_value / 2] = L'\0';
			fprintf(stream, "%ls\n", buffer);
			break;
		default:
			break;
		}
	}
	free(buffer);
}

static void agcc_print_data_header(const DataHeader *data_header, FILE *stream)
{
	if (data_header->guid)
		fprintf(stream, "#%%FileIdentifier=%s\n", data_header->guid);
	fprintf(stream, "#%%FileTypeIdentifier=%s\n", data_header->data_type_identifier);
	fprintf(stream, "#%%FileLocale=%ls\n", data_header->locale);
	agcc_print_parameters(data_header->parameters, data_header->n_parameters, stream);
	for (int i = 0; i < data_header->n_parents; i++)
		agcc_print_data_header(&data_header->parents[i], stream);
}

typedef void (*col_print_t)(const char *, FILE *stream);

void agcc_print_probe_set_name(const char *s, FILE *stream)
{
	uint32_t size = ntohl(*(uint32_t *)s);
	fwrite(s + 4, 1, size, stream);
}

void agcc_print_call(const char *s, FILE *stream)
{
	static const char a[16] = "......ABA..N....";
	static const char b[16] = "......ABB..C....";
	int c = s[0] & 0x0F;
	fputc(a[c], stream);
	fputc(b[c], stream);
}

void agcc_print_float(const char *s, FILE *stream)
{
	union {
		uint32_t u;
		float f;
	} convert;
	convert.u = ntohl(*(uint32_t *)s);
	fprintf(stream, "%g", convert.f);
}

static void agcc_print_data_set(const DataSet *data_set, FILE *stream, int verbose)
{
	fprintf(stream, "#%%SetName=%ls\n", data_set->name);
	fprintf(stream, "#%%Columns=%d\n", data_set->n_cols);
	fprintf(stream, "#%%Rows=%d\n", data_set->n_rows);
	agcc_print_parameters(data_set->parameters, data_set->n_parameters, stream);
	for (int i = 0; i < data_set->n_cols; i++)
		fprintf(stream, "%ls%c", data_set->col_headers[i].name,
			i + 1 < data_set->n_cols ? '\t' : '\n');
	if (data_set->n_rows == 0)
		return;

	if (!verbose) {
		fprintf(stream, "... use --verbose to visualize Data Set ...\n");
		return;
	}
	if (wcscmp(data_set->name, L"Genotype") != 0) {
		fprintf(stream, "... can only visualize Genotype Data Set ...\n");
		return;
	}

	char *col_ends = (char *)malloc(data_set->n_cols * sizeof(char *));
	col_print_t *col_prints =
		(col_print_t *)malloc(data_set->n_cols * sizeof(col_print_t *));
	for (int i = 0; i < data_set->n_cols; i++) {
		col_ends[i] = i + 1 < data_set->n_cols ? '\t' : '\n';
		if (wcscmp(data_set->col_headers[i].name, L"ProbeSetName") == 0)
			col_prints[i] = agcc_print_probe_set_name;
		else if (wcscmp(data_set->col_headers[i].name, L"Call") == 0)
			col_prints[i] = agcc_print_call;
		else if (wcscmp(data_set->col_headers[i].name, L"Confidence") == 0)
			col_prints[i] = agcc_print_float;
		else if (wcscmp(data_set->col_headers[i].name, L"Log Ratio") == 0)
			col_prints[i] = agcc_print_float;
		else if (wcscmp(data_set->col_headers[i].name, L"Strength") == 0)
			col_prints[i] = agcc_print_float;
		else if (wcscmp(data_set->col_headers[i].name, L"Signal A") == 0)
			col_prints[i] = agcc_print_float;
		else if (wcscmp(data_set->col_headers[i].name, L"Signal B") == 0)
			col_prints[i] = agcc_print_float;
		else if (wcscmp(data_set->col_headers[i].name, L"Forced Call") == 0)
			col_prints[i] = agcc_print_call;
		else
			error("Unknown column type %ls in AGCC CHP file with type %d\n",
			      data_set->col_headers[i].name, data_set->col_headers[i].type);
	}
	if (hseek(data_set->fp, data_set->pos_first_element, SEEK_SET) < 0)
		error("Fail to seek to position %d in AGCC CHP file\n",
		      data_set->pos_first_element);
	for (int i = 0; i < data_set->n_rows; i++) {
		read_bytes(data_set->fp, (void *)data_set->buffer, data_set->n_buffer);
		for (int j = 0; j < data_set->n_cols; j++) {
			col_prints[j](data_set->buffer + data_set->col_offsets[j], stream);
			fputc(col_ends[j], stream);
		}
	}
	free(col_ends);
	free(col_prints);
}

static void agcc_print_data_group(const DataGroup *data_group, FILE *stream, int verbose)
{
	fprintf(stream, "#%%GroupName=%ls\n", data_group->name);
	for (int i = 0; i < data_group->num_data_sets; i++)
		agcc_print_data_set(&data_group->data_sets[i], stream, verbose);
}

static void agcc_print(const agcc_t *agcc, FILE *stream, int verbose)
{
	fprintf(stream, "#%%File=%s\n", agcc->fn);
	fprintf(stream, "#%%FileSize=%ld\n", agcc->size);
	fprintf(stream, "#%%Magic=%d\n", agcc->magic);
	fprintf(stream, "#%%Version=%d\n", agcc->version);
	agcc_print_data_header(&agcc->data_header, stream);
	for (int i = 0; i < agcc->num_data_groups; i++)
		agcc_print_data_group(&agcc->data_groups[i], stream, verbose);
}

static void agccs_to_tsv(agcc_t **agcc, int n, FILE *stream)
{
	static const wchar_t *chipsummary[] = {L"computed_gender",
					       L"call_rate",
					       L"total_call_rate",
					       L"het_rate",
					       L"total_het_rate",
					       L"hom_rate",
					       L"total_hom_rate",
					       L"cluster_distance_mean",
					       L"cluster_distance_stdev",
					       L"allele_summarization_mean",
					       L"allele_summarization_stdev",
					       L"allele_deviation_mean",
					       L"allele_deviation_stdev",
					       L"allele_mad_residuals_mean",
					       L"allele_mad_residuals_stdev",
					       L"cn-probe-chrXY-ratio_gender_meanX",
					       L"cn-probe-chrXY-ratio_gender_meanY",
					       L"cn-probe-chrXY-ratio_gender_ratio",
					       L"cn-probe-chrXY-ratio_gender",
					       L"pm_mean"};
	fputs("chp_files", stream);
	for (int j = 0; j < 20; j++)
		fprintf(stream, "\t%ls", chipsummary[j]);
	fputc('\n', stream);
	for (int i = 0; i < n; i++) {
		fputs(strrchr(agcc[i]->fn, '/') ? strrchr(agcc[i]->fn, '/') + 1 : agcc[i]->fn,
		      stream);
		DataHeader *data_header = &agcc[i]->data_header;
		for (int j = 0, k = 0; j < 20; j++) {
			fputc('\t', stream);
			while (wcsncmp(data_header->parameters[k].name,
				       L"affymetrix-chipsummary-", 23)
				       != 0
			       || wcscmp(&data_header->parameters[k].name[23], chipsummary[j])
					  != 0) {
				k++;
				k %= data_header->n_parameters;
			}
			union {
				uint32_t u;
				float f;
			} convert;
			switch (data_header->parameters[k].type) {
			case FLOAT:
				convert.u =
					ntohl(*(uint32_t *)data_header->parameters[k].value);
				fprintf(stream, "%.5f", convert.f);
				break;
			case STRING:
				fputs(data_header->parameters[k].value, stream);
				break;
			default:
				error("Unable to print parameter of type %d from %s AGCC CHP file\n",
				      data_header->parameters[k].type, agcc[i]->fn);
				break;
			}
		}
		fputc('\n', stream);
	}
}

/****************************************
 * PRINT CEL SUMMARY                    *
 ****************************************/

static void parse_dat_header(char *dat_header, char *str[12], int n_str[12])
{
	char *ss = dat_header + 2;
	char *se = strchr(dat_header, '\0');
	if (!se)
		goto fail;

	se = strchr(ss, ':');
	if (!se)
		goto fail;
	str[0] = ss;
	n_str[0] = se - ss;

	ss = se + 5;
	for (se = ss + 4; isspace(*se) && se >= ss; se--)
		;
	str[1] = ss;
	n_str[1] = se - ss + 1;

	ss = ss + 9;
	for (se = ss + 4; isspace(*se) && se >= ss; se--)
		;
	str[2] = ss;
	n_str[2] = se - ss + 1;

	ss = ss + 9;
	for (se = ss + 2; isspace(*se) && se >= ss; se--)
		;
	str[3] = ss;
	n_str[3] = se - ss + 1;

	ss = ss + 7;
	for (se = ss + 2; isspace(*se) && se >= ss; se--)
		;
	str[4] = ss;
	n_str[4] = se - ss + 1;

	ss = ss + 6;
	for (se = ss + 2; isspace(*se) && se >= ss; se--)
		;
	str[5] = ss;
	n_str[5] = se - ss + 1;

	ss = ss + 3;
	for (se = ss + 6; isspace(*se) && se >= ss; se--)
		;
	str[6] = ss;
	n_str[6] = se - ss + 1;

	ss = ss + 7;
	for (se = ss + 3; isspace(*se) && se >= ss; se--)
		;
	str[7] = ss;
	n_str[7] = se - ss + 1;

	ss = ss + 4;
	for (se = ss + 17; isspace(*se) && se >= ss; se--)
		;
	str[8] = ss;
	n_str[8] = se - ss + 1;

	ss = ss + 18;
	se = strchr(ss, ' ');
	if (!se)
		goto fail;
	str[9] = ss;
	n_str[9] = se - ss;

	ss = se + 2;
	se = strstr(ss, "\x14 ");
	if (!se)
		goto fail;
	for (se--; isspace(*se) && se >= ss; se--)
		;
	str[10] = ss;
	n_str[10] = se - ss + 1;

	se = strstr(ss, "\x14 ");
	if (!se)
		goto fail;
	ss = se + 2;
	se = strstr(ss, "\x14 ");
	if (!se)
		goto fail;
	ss = se + 2;
	se = strstr(ss, ".1sq");
	if (!se)
		goto fail;
	str[11] = ss;
	n_str[11] = se - ss;

	return;

fail:
	error("DAT header malformed\n");
}

// https://github.com/HenrikBengtsson/affxparser/blob/master/R/parseDatHeaderString.R
static void cels_to_tsv(uint8_t *magic, void **files, int n, FILE *stream)
{
	char *str[12];
	int n_str[12];
	char *buffer = NULL;
	size_t m_buffer = 0;
	fprintf(stream,
		"cel_files\tDAT Name\tCLS\tRWS\tXIN\tYIN\tVE\tTemp\tPower\tDate\tScanner\tNum\tChipType\n");
	for (int i = 0; i < n; i++) {
		int j;
		char *ss, *se;
		agcc_t *agcc = (agcc_t *)files[i];
		xda_cel_t *xda_cel = (xda_cel_t *)files[i];
		switch (magic[i]) {
		case 59:
			if (strcmp(agcc->data_header.data_type_identifier,
				   "affymetrix-calvin-intensity")
			    != 0)
				error("AGCC CEL file %s does not contain calvin intensities\n",
				      agcc->fn);
			if (agcc->data_header.n_parents == 0
			    || strcmp(agcc->data_header.parents[0].data_type_identifier,
				      "affymetrix-calvin-scan-acquisition")
				       != 0)
				error("AGCC CEL file %s is missing scan acquisition information\n",
				      agcc->fn);
			DataHeader *data_header = &agcc->data_header.parents[0];
			for (j = 0; j < data_header->n_parameters; j++)
				if (wcscmp(data_header->parameters[j].name,
					   L"affymetrix-partial-dat-header")
				    == 0)
					break;
			if (j == data_header->n_parameters)
				error("AGCC CEL file %s is missing DAT header\n", agcc->fn);
			hts_expand0(char, data_header->parameters[j].n_value / 2 + 1, m_buffer,
				    buffer);
			for (int k = 0; k < data_header->parameters[j].n_value / 2; k++)
				buffer[k] = (char)ntohs(
					((uint16_t *)data_header->parameters[j].value)[k]);
			buffer[data_header->parameters[j].n_value / 2] = '\0';
			parse_dat_header(buffer, str, n_str);
			fputs(strrchr(agcc->fn, '/') ? strrchr(agcc->fn, '/') + 1 : agcc->fn,
			      stream);
			break;
		case 64:
			ss = strstr(xda_cel->header, "\nDatHeader=[");
			if (!ss)
				error("XDA CEL file %s is missing DAT header\n", xda_cel->fn);
			ss = strchr(ss + 12, ']');
			if (!ss)
				error("XDA CEL file %s is missing DAT header\n", xda_cel->fn);
			ss++;
			se = strchr(ss, '\n');
			if (!se)
				error("XDA CEL file %s is missing DAT header\n", xda_cel->fn);
			*se = '\0';
			parse_dat_header(ss, str, n_str);
			*se = '\n';
			fputs(strrchr(xda_cel->fn, '/') ? strrchr(xda_cel->fn, '/') + 1
							: xda_cel->fn,
			      stream);
			break;
		default:
			break;
		}
		for (j = 0; j < 12; j++) {
			fputc('\t', stream);
			fwrite(str[j], 1, n_str[j], stream);
		}
		fputc('\n', stream);
	}
	free(buffer);
}

/****************************************
 * htsFILE READING FUNCTIONS            *
 ****************************************/

static htsFile *unheader(const char *fn, kstring_t *str)
{
	htsFile *fp = hts_open(fn, "r");
	if (fp == NULL)
		error("Could not open %s: %s\n", fn, strerror(errno));

	if (hts_getline(fp, KS_SEP_LINE, str) <= 0)
		error("Empty file: %s\n", fn);

	// skip header
	while (str->s[0] == '#')
		hts_getline(fp, KS_SEP_LINE, str);

	return fp;
}

/****************************************
 * CLUSTER MODELS FILE IMPLEMENTATION   *
 ****************************************/

// http://www.affymetrix.com/support/developer/powertools/changelog/SnpModelConverter_8cpp_source.html

typedef struct {
	float xm;   // delta mean of cluster
	float xss;  // delta variance of cluster
	float k;    // strength of mean (pseudo-observations)
	float v;    // strength of variance (pseudo-observations)
	float ym;   // size mean of cluster in other dimension
	float yss;  // size variance of cluster in other dimension
	float xyss; // covariance of cluster in both directions
} cluster_t;

typedef struct {
	char *probe_set_id;
	int copynumber;
	cluster_t aa;
	cluster_t ab;
	cluster_t bb;
} snp_t;

typedef struct {
	int is_birdseed;
	void *probe_set_id[2];
	snp_t *snps[2];
	int n_snps[2];
	int m_snps[2];
} models_t;

static inline void brlmmp_cluster_init(const char *s, const int *off, cluster_t *cluster)
{
	cluster->xm = strtof(&s[off[0]], NULL);
	cluster->xss = strtof(&s[off[1]], NULL);
	cluster->k = strtof(&s[off[2]], NULL);
	cluster->v = strtof(&s[off[3]], NULL);
	cluster->ym = strtof(&s[off[4]], NULL);
	cluster->yss = strtof(&s[off[5]], NULL);
	cluster->xyss = strtof(&s[off[6]], NULL);
}

static inline void birdseed_cluster_init(const char *s, const int *off, cluster_t *cluster)
{
	cluster->xm = strtof(&s[off[0]], NULL);
	cluster->ym = strtof(&s[off[1]], NULL);
	cluster->xss = strtof(&s[off[2]], NULL);
	cluster->xyss = strtof(&s[off[3]], NULL);
	cluster->yss = strtof(&s[off[4]], NULL);
	cluster->k = strtof(&s[off[5]], NULL);
	cluster->v = strtof(&s[off[5]], NULL);
}

static models_t *models_init(const char *fn)
{
	models_t *models = (models_t *)calloc(1, sizeof(models_t));
	for (int i = 0; i < 2; i++) {
		models->probe_set_id[i] = khash_str2int_init();
	}

	kstring_t str = {0, 0, NULL};
	htsFile *fp = unheader(fn, &str);

	int sep1, sep2, sep3, exp_cols;
	if (strcmp(str.s, "id\tBB\tAB\tAA\tCV") == 0) {
		if (hts_getline(fp, KS_SEP_LINE, &str) <= 0)
			error("Missing information in SNP models file: %s\n", fn);
		sep1 = '\t';
		sep2 = ',';
		sep3 = ':';
		exp_cols = 7;
	} else if (!strchr(str.s, '\t')) {
		models->is_birdseed = 1;
		sep1 = ';';
		sep2 = ' ';
		sep3 = '-';
		exp_cols = 6;
	} else {
		error("Malformed SNP model file: %s\n", fn);
	}

	snp_t *snp;
	int moff1 = 0, *off1 = NULL, ncols1;
	int moff2 = 0, *off2 = NULL, ncols2;
	do {
		ncols1 = ksplit_core(str.s, sep1, &moff1, &off1);
		char *col_str = &str.s[off1[0]];

		int len = strlen(col_str);
		int copynumber;
		if (col_str[len - 2] == sep3) {
			copynumber = strtol(&col_str[len - 1], NULL, 10);
			len -= 2;
			col_str[len] = '\0';
		} else {
			copynumber = 2;
		}

		int idx = copynumber == 2;
		hts_expand(snp_t, models->n_snps[idx] + 1, models->m_snps[idx],
			   models->snps[idx]);
		snp = &models->snps[idx][models->n_snps[idx]];
		snp->probe_set_id = strdup(&str.s[off1[0]]);
		snp->copynumber = copynumber;
		khash_str2int_inc(models->probe_set_id[idx], snp->probe_set_id);

		if (ncols1 < 4 - (2 - copynumber) * models->is_birdseed)
			error("Missing information for probeset %s in SNP posterior models file: %s\n",
			      str.s, fn);
		col_str = &str.s[off1[1]];
		ncols2 = ksplit_core(col_str, sep2, &moff2, &off2);

		if (ncols2 < exp_cols)
			error("Missing information for probeset %s in SNP posterior models file: %s\n",
			      str.s, fn);
		if (models->is_birdseed)
			birdseed_cluster_init(col_str, off2, &snp->aa);
		else
			brlmmp_cluster_init(col_str, off2, &snp->bb);

		col_str = &str.s[off1[2]];
		if (models->is_birdseed && copynumber == 1) {
			snp->ab.xm = NAN;
			snp->ab.xss = NAN;
			snp->ab.k = NAN;
			snp->ab.v = NAN;
			snp->ab.ym = NAN;
			snp->ab.yss = NAN;
			snp->ab.xyss = NAN;
		} else {
			ncols2 = ksplit_core(col_str, sep2, &moff2, &off2);
			if (ncols2 < exp_cols)
				error("Missing information for probeset %s in SNP posterior models file: %s\n",
				      str.s, fn);
			if (models->is_birdseed)
				birdseed_cluster_init(col_str, off2, &snp->ab);
			else
				brlmmp_cluster_init(col_str, off2, &snp->ab);
			col_str = &str.s[off1[3]];
		}

		ncols2 = ksplit_core(col_str, sep2, &moff2, &off2);
		if (ncols2 < exp_cols)
			error("Missing information for probeset %s in SNP posterior models file: %s\n",
			      str.s, fn);
		if (models->is_birdseed)
			birdseed_cluster_init(col_str, off2, &snp->bb);
		else
			brlmmp_cluster_init(col_str, off2, &snp->aa);

		models->n_snps[idx]++;
	} while (hts_getline(fp, KS_SEP_LINE, &str) > 0);

	free(off2);
	free(off1);
	free(str.s);
	hts_close(fp);
	return models;
}

static void models_destroy(models_t *models)
{
	for (int i = 0; i < 2; i++) {
		khash_str2int_destroy(models->probe_set_id[i]);
		for (int j = 0; j < models->n_snps[i]; j++)
			free(models->snps[i][j].probe_set_id);
		free(models->snps[i]);
	}
	free(models);
}

/****************************************
 * ANNOT.CSV FILE IMPLEMENTATION        *
 ****************************************/

typedef struct {
	char *probe_set_id;
	char *affy_snp_id;
	char *dbsnp_rs_id;
	char *chromosome;
	int position;
	int strand;
	char *flank;
} record_t;

typedef struct {
	void *probe_set_id;
	record_t *records;
	int n_records, m_records;
} annot_t;

static inline char *unquote(char *str)
{
	if (strcmp(str, "\"---\"") == 0)
		return NULL;
	char *ptr = strrchr(str, '"');
	if (ptr)
		*ptr = '\0';
	return str + 1;
}

static annot_t *annot_init(const char *fn, const char *sam_fn, const char *out_fn, int flags)
{
	annot_t *annot = NULL;
	FILE *out_txt = get_file_handle(out_fn);
	htsFile *hts = NULL;
	sam_hdr_t *sam_hdr = NULL;
	bam1_t *b = NULL;
	if (sam_fn) {
		hts = hts_open(sam_fn, "r");
		if (hts == NULL || hts_get_format(hts)->category != sequence_data)
			error("File %s does not contain sequence data\n", sam_fn);
		sam_hdr = sam_hdr_read(hts);
		if (sam_hdr == NULL)
			error("Reading header from \"%s\" failed", sam_fn);
		b = bam_init1();
		if (b == NULL)
			error("Cannot create SAM record\n");
	}
	kstring_t str = {0, 0, NULL};

	htsFile *fp = hts_open(fn, "r");
	if (!fp)
		error("Could not read: %s\n", fn);
	if (hts_getline(fp, KS_SEP_LINE, &str) <= 0)
		error("Empty file: %s\n", fn);
	const char *null_strand = "---";
	while (str.s[0] == '#') {
		if (strcmp(str.s, "#%netaffx-annotation-tabular-format-version=1.0") == 0)
			null_strand = "---";
		if (strcmp(str.s, "#%netaffx-annotation-tabular-format-version=1.5") == 0)
			null_strand = "+";
		if (hts && out_txt)
			fprintf(out_txt, "%s\n", str.s);
		hts_getline(fp, KS_SEP_LINE, &str);
	}

	if (hts && out_txt)
		fprintf(out_txt, "%s\n", str.s);

	int probe_set_id_idx = -1;
	int affy_snp_id_idx = -1;
	int dbsnp_rs_id_idx = -1;
	int chromosome_idx = -1;
	int position_idx = -1;
	int position_end_idx = -1;
	int strand_idx = -1;
	int flank_idx = -1;
	int allele_a_idx = -1;
	int allele_b_idx = -1;

	int moff = 0, *off = NULL;
	int ncols = ksplit_core(str.s, ',', &moff, &off);
	for (int i = 0; i < ncols; i++) {
		if (strcmp(&str.s[off[i]], "\"Probe Set ID\"") == 0)
			probe_set_id_idx = i;
		else if (strcmp(&str.s[off[i]], "\"Affy SNP ID\"") == 0)
			affy_snp_id_idx = i;
		else if (strcmp(&str.s[off[i]], "\"dbSNP RS ID\"") == 0)
			dbsnp_rs_id_idx = i;
		else if (strcmp(&str.s[off[i]], "\"Chromosome\"") == 0)
			chromosome_idx = i;
		else if (strcmp(&str.s[off[i]], "\"Physical Position\"") == 0)
			position_idx = i;
		else if (strcmp(&str.s[off[i]], "\"Position End\"") == 0)
			position_end_idx = i;
		else if (strcmp(&str.s[off[i]], "\"Strand\"") == 0)
			strand_idx = i;
		else if (strcmp(&str.s[off[i]], "\"Flank\"") == 0)
			flank_idx = i;
		else if (strcmp(&str.s[off[i]], "\"Allele A\"") == 0)
			allele_a_idx = i;
		else if (strcmp(&str.s[off[i]], "\"Allele B\"") == 0)
			allele_b_idx = i;
	}
	if (probe_set_id_idx != 0)
		error("Probe Set ID not the first column in file: %s\n", fn);
	if (flank_idx == -1)
		error("Flank missing from file: %s\n", fn);
	if (allele_a_idx == -1)
		error("Allele A missing from file: %s\n", fn);
	if (allele_b_idx == -1)
		error("Allele B missing from file: %s\n", fn);
	const char *probe_set_id, *flank, *allele_a, *allele_b;

	if (!hts && out_txt) {

		while (hts_getline(fp, KS_SEP_LINE, &str) > 0) {
			ncols = ksplit_core(str.s, ',', &moff, &off);
			probe_set_id = unquote(&str.s[off[probe_set_id_idx]]);
			flank = unquote(&str.s[off[flank_idx]]);
			if (flank)
				flank2fasta(probe_set_id, flank, out_txt);
		}
	} else {
		if (dbsnp_rs_id_idx == -1)
			error("dbSNP RS ID missing from file: %s\n", fn);
		if (chromosome_idx == -1)
			error("Chromosome missing from file: %s\n", fn);
		if (position_idx == -1)
			error("Physical Position missing from file: %s\n", fn);
		if (strand_idx == -1)
			error("Strand missing from file: %s\n", fn);

		if (!out_txt) {
			annot = (annot_t *)calloc(1, sizeof(annot_t));
			annot->probe_set_id = khash_str2int_init();
		}

		char *tmp;
		int n_total = 0, n_unmapped = 0;
		while (hts_getline(fp, KS_SEP_LINE, &str) > 0) {
			ncols = ksplit_core(str.s, ',', &moff, &off);
			probe_set_id = unquote(&str.s[off[probe_set_id_idx]]);
			flank = unquote(&str.s[off[flank_idx]]);
			allele_a = unquote(&str.s[off[allele_a_idx]]);
			allele_b = unquote(&str.s[off[allele_b_idx]]);
			const char *chromosome = NULL;
			int strand = -1, position = 0, idx = -1;
			if (hts) {
				if (!flank) {
					if (flags & VERBOSE)
						fprintf(stderr,
							"Missing flank sequence for marker %s\n",
							probe_set_id);
					n_unmapped++;
				} else {
					idx = get_position(hts, sam_hdr, b, probe_set_id, flank,
							   0, &chromosome, &position, &strand);
					if (idx < 0)
						error("Reading from %s failed", sam_fn);
					else if (idx == 0) {
						if (flags & VERBOSE)
							fprintf(stderr,
								"Unable to determine position for marker %s\n",
								probe_set_id);
						n_unmapped++;
					}
				}
				n_total++;
			} else {
				chromosome = unquote(&str.s[off[chromosome_idx]]);
				const char *ptr = unquote(&str.s[off[position_idx]]);
				position = ptr ? strtol(ptr, &tmp, 10) : 0;
				ptr = unquote(&str.s[off[strand_idx]]);
				if (!ptr)
					strand = -1;
				else if (strcmp(ptr, "+") == 0)
					strand = 0;
				else if (strcmp(ptr, "-") == 0)
					strand = 1;
				else
					strand = -1;
			}

			if (out_txt) {
				// "Ref Allele" and "Alt Allele" will not be updated
				fprintf(out_txt, "\"%s\"", probe_set_id);
				for (int i = 1; i < ncols; i++) {
					if (i == flank_idx)
						fprintf(out_txt, ",\"%s\"", flank);
					if (i == allele_a_idx)
						fprintf(out_txt, ",\"%s\"", allele_a);
					if (i == allele_b_idx) {
						fprintf(out_txt, ",\"%s\"", allele_b);
					} else if (i == chromosome_idx) {
						if (chromosome)
							fprintf(out_txt, ",\"%s\"", chromosome);
						else
							fprintf(out_txt, ",\"---\"");
					} else if (i == position_idx) {
						if (position)
							fprintf(out_txt, ",\"%d\"", position);
						else
							fprintf(out_txt, ",\"---\"");
					} else if (i == position_end_idx) {
						if (flank && position && idx > 0) {
							const char *left = strchr(flank, '[');
							const char *middle = strchr(flank, '/');
							const char *right = strchr(flank, ']');
							if (!left || !middle || !right)
								error("Flank sequence is malformed: %s\n",
								      flank);

							fprintf(out_txt, ",\"%d\"",
								position
									+ (int)(idx > 1 ? right - middle
											: middle - left
												  + (*(left
												       + 1)
												     == '-'))
									- 2);
						} else {
							fprintf(out_txt, ",\"---\"");
						}
					} else if (i == strand_idx) {
						fprintf(out_txt, ",\"%s\"",
							strand == 0
								? "+"
								: (strand == 1 ? "-"
									       : null_strand));
					} else {
						fprintf(out_txt, ",%s", &str.s[off[i]]);
					}
				}
				fprintf(out_txt, "\n");
			} else {
				hts_expand0(record_t, annot->n_records + 1, annot->m_records,
					    annot->records);
				annot->records[annot->n_records].probe_set_id =
					strdup(probe_set_id);
				khash_str2int_inc(
					annot->probe_set_id,
					annot->records[annot->n_records].probe_set_id);
				const char *dbsnp_rs_id = unquote(&str.s[off[dbsnp_rs_id_idx]]);
				if (dbsnp_rs_id)
					annot->records[annot->n_records].dbsnp_rs_id =
						strdup(dbsnp_rs_id);
				if (affy_snp_id_idx >= 0) {
					const char *affy_snp_id =
						unquote(&str.s[off[affy_snp_id_idx]]);
					if (affy_snp_id)
						annot->records[annot->n_records].affy_snp_id =
							strdup(affy_snp_id);
				}
				if (chromosome)
					annot->records[annot->n_records].chromosome =
						strdup(chromosome);
				annot->records[annot->n_records].position = position;
				if (flank) {
					annot->records[annot->n_records].flank = strdup(flank);
					// check whether alleles A and B need to be flipped in
					// the flank sequence (happens with T/C and T/G SNPs
					// only)
					char *left = strchr(
						annot->records[annot->n_records].flank, '[');
					char *middle = strchr(
						annot->records[annot->n_records].flank, '/');
					char *right = strchr(
						annot->records[annot->n_records].flank, ']');
					if (strncmp(left + 1, allele_b, middle - left - 1) == 0
					    && strncmp(middle + 1, allele_a, right - middle - 1)
						       == 0) {
						memcpy(left + 1, allele_a, right - middle - 1);
						*(left + (right - middle)) = '/';
						memcpy(left + (right - middle) + 1, allele_b,
						       middle - left - 1);
					}
				}
				annot->records[annot->n_records].strand = strand;
				annot->n_records++;
			}
		}
		if (hts)
			fprintf(stderr, "Lines   total/unmapped:\t%d/%d\n", n_total,
				n_unmapped);

		bam_destroy1(b);
		sam_hdr_destroy(sam_hdr);
		if (hts && hts_close(hts) < 0)
			error("closing \"%s\" failed", fn);
	}

	free(off);
	free(str.s);
	hts_close(fp);

	if (out_txt && out_txt != stdout && out_txt != stderr)
		fclose(out_txt);
	return annot;
}

static void annot_destroy(annot_t *annot)
{
	khash_str2int_destroy(annot->probe_set_id);
	for (int i = 0; i < annot->n_records; i++) {
		free(annot->records[i].probe_set_id);
		free(annot->records[i].affy_snp_id);
		free(annot->records[i].dbsnp_rs_id);
		free(annot->records[i].chromosome);
		free(annot->records[i].flank);
	}
	free(annot->records);
	free(annot);
}

/****************************************
 * REPORT.TXT FILE IMPLEMENTATION       *
 ****************************************/

typedef struct {
	char **cel_files;
	int8_t *genders;
	int n_samples, m_cel_file, m_genders;
} report_t;

static report_t *report_init(const char *fn)
{
	kstring_t str = {0, 0, NULL};
	htsFile *fp = unheader(fn, &str);
	int moff = 0, *off = NULL, ncols;
	ncols = ksplit_core(str.s, '\t', &moff, &off);
	if (ncols < 2)
		error("Missing information in report file: %s\n", fn);
	if (strcmp(&str.s[off[1]], "computed_gender"))
		error("Second column not genders in file: %s\n", fn);

	report_t *report = (report_t *)calloc(1, sizeof(report_t));
	while (hts_getline(fp, KS_SEP_LINE, &str) > 0) {
		ncols = ksplit_core(str.s, '\t', &moff, &off);
		if (ncols < 2)
			error("Missing information in report file: %s\n", fn);
		hts_expand(report->cel_files, report->n_samples + 1, report->m_cel_file,
			   report->cel_files);
		hts_expand0(int8_t, report->n_samples + 1, report->m_genders, report->genders);
		report->cel_files[report->n_samples] = strdup(&str.s[off[0]]);
		if (strcmp(&str.s[off[1]], "male") == 0)
			report->genders[report->n_samples] = 1;
		else if (strcmp(&str.s[off[1]], "female") == 0)
			report->genders[report->n_samples] = 2;
		report->n_samples++;
	}

	free(off);
	free(str.s);
	hts_close(fp);
	return report;
}

static void report_destroy(report_t *report)
{
	for (int i = 0; i < report->n_samples; i++)
		free(report->cel_files[i]);
	free(report->cel_files);
	free(report->genders);
	free(report);
}

/****************************************
 * READER ITERATORS                     *
 ****************************************/

#define MAX_LENGTH_PROBE_SET_ID 17
typedef struct {
	int nsmpl;
	int nrow;

	DataSet **data_sets;
	int *is_axiom;
	htsFile *calls_fp;
	htsFile *confidences_fp;
	htsFile *summary_fp;
	char probe_set_id[MAX_LENGTH_PROBE_SET_ID + 1];

	int *gts;
	float *conf_arr;
	float *norm_x_arr;
	float *norm_y_arr;
	float *delta_arr;
	float *size_arr;
	float *baf_arr;
	float *lrr_arr;
} varitr_t;

static void varitr_init_common(varitr_t *varitr)
{
	varitr->gts = (int *)malloc(varitr->nsmpl * sizeof(int));
	varitr->conf_arr = (float *)malloc(varitr->nsmpl * sizeof(float));
	varitr->norm_x_arr = (float *)malloc(varitr->nsmpl * sizeof(float));
	varitr->norm_y_arr = (float *)malloc(varitr->nsmpl * sizeof(float));
	varitr->delta_arr = (float *)malloc(varitr->nsmpl * sizeof(float));
	varitr->size_arr = (float *)malloc(varitr->nsmpl * sizeof(float));
	varitr->baf_arr = (float *)malloc(varitr->nsmpl * sizeof(float));
	varitr->lrr_arr = (float *)malloc(varitr->nsmpl * sizeof(float));
}

static varitr_t *varitr_init_cc(bcf_hdr_t *hdr, agcc_t **agcc, int n)
{
	varitr_t *varitr = (varitr_t *)calloc(1, sizeof(varitr_t));
	varitr->nsmpl = n;
	varitr->data_sets = (DataSet **)malloc(n * sizeof(DataSet *));
	varitr->is_axiom = (int *)malloc(n * sizeof(int));
	for (int i = 0; i < n; i++) {
		if (strcmp(agcc[i]->data_header.data_type_identifier,
			   "affymetrix-multi-data-type-analysis")
		    != 0)
			error("AGCC CHP file %s does not contain multi data type analysis\n",
			      agcc[i]->fn);
		if (agcc[i]->num_data_groups == 0
		    || wcscmp(agcc[i]->data_groups[0].name, L"MultiData") != 0)
			error("AGCC CHP file %s does not contain multi data\n", agcc[i]->fn);
		if (agcc[i]->data_groups[0].num_data_sets == 0
		    || wcscmp(agcc[i]->data_groups[0].data_sets[0].name, L"Genotype") != 0)
			error("AGCC CHP file %s does not contain genotype data\n", agcc[i]->fn);
		DataSet *data_set = &agcc[i]->data_groups[0].data_sets[0];
		if (wcscmp(data_set->col_headers[0].name, L"ProbeSetName") != 0
		    || wcscmp(data_set->col_headers[1].name, L"Call") != 0
		    || wcscmp(data_set->col_headers[2].name, L"Confidence") != 0
		    || wcscmp(data_set->col_headers[5].name, L"Forced Call") != 0)
			error("AGCC CHP file %s does not contain genotype data in the expected format\n",
			      agcc[i]->fn);
		if (wcscmp(data_set->col_headers[3].name, L"Log Ratio") == 0
		    || wcscmp(data_set->col_headers[4].name, L"Strength") == 0)
			varitr->is_axiom[i] = 1; // ProbeSetName / Call / Confidence / Log Ratio
						 // / Strength / Forced Call
		else if (wcscmp(data_set->col_headers[3].name, L"Signal A") == 0
			 || wcscmp(data_set->col_headers[4].name, L"Signal B") == 0)
			varitr->is_axiom[i] = 0; // ProbeSetName / Call / Confidence / Signal A
						 // / Signal B / Forced Call
		else
			error("AGCC CHP file %s does not contain intensities data in the expected format\n",
			      agcc[i]->fn);
		if (hseek(data_set->fp, data_set->pos_first_element, SEEK_SET) < 0)
			error("Fail to seek to position %d in AGCC CHP file\n",
			      data_set->pos_first_element);
		bcf_hdr_add_sample(hdr, agcc[i]->display_name);
		varitr->data_sets[i] = data_set;
	}
	varitr_init_common(varitr);
	return varitr;
}

static varitr_t *varitr_init_txt(bcf_hdr_t *hdr, const char *calls_fn,
				 const char *confidences_fn, const char *summary_fn)
{
	varitr_t *varitr = (varitr_t *)calloc(1, sizeof(varitr_t));

	kstring_t str = {0, 0, NULL};
	int moff = 0, *off = NULL, ncols;

	if (calls_fn) {
		varitr->calls_fp = unheader(calls_fn, &str);
		ncols = ksplit_core(str.s, '\t', &moff, &off);
		if (strcmp(&str.s[off[0]], "probeset_id"))
			error("Malformed first line from calls file: %s\n%s\n", calls_fn,
			      str.s);
		varitr->nsmpl = ncols - 1;
		for (int i = 1; i < ncols; i++) {
			char *ptr = strrchr(&str.s[off[i]], '.');
			if (ptr && strcmp(ptr + 1, "CEL") == 0)
				*ptr = '\0';
			bcf_hdr_add_sample(hdr, &str.s[off[i]]);
		}
	}

	if (confidences_fn) {
		varitr->confidences_fp = unheader(confidences_fn, &str);
		ncols = ksplit_core(str.s, '\t', &moff, &off);
		if (strcmp(&str.s[off[0]], "probeset_id"))
			error("Malformed first line from confidences file: %s\n%s\n",
			      confidences_fn, str.s);
		if (!varitr->calls_fp) {
			varitr->nsmpl = ncols - 1;
			for (int i = 1; i < ncols; i++) {
				char *ptr = strrchr(&str.s[off[i]], '.');
				if (ptr && strcmp(ptr + 1, "CEL") == 0)
					*ptr = '\0';
				bcf_hdr_add_sample(hdr, &str.s[off[i]]);
			}
		}
	}

	if (summary_fn) {
		varitr->summary_fp = unheader(summary_fn, &str);
		ncols = ksplit_core(str.s, '\t', &moff, &off);
		if (strcmp(&str.s[off[0]], "probeset_id"))
			error("Malformed first line from summary file: %s\n%s\n", summary_fn,
			      str.s);
		if (!varitr->calls_fp && !varitr->confidences_fp) {
			varitr->nsmpl = ncols - 1;
			for (int i = 1; i < ncols; i++) {
				char *ptr = strrchr(&str.s[off[i]], '.');
				if (ptr && strcmp(ptr + 1, "CEL") == 0)
					*ptr = '\0';
				bcf_hdr_add_sample(hdr, &str.s[off[i]]);
			}
		}
	}

	free(str.s);
	free(off);

	varitr_init_common(varitr);
	return varitr;
}

static inline void check_n_probe_set_id(char *dest, const char *src, uint32_t n)
{
	if (dest[0] == '\0') {
		if (n > MAX_LENGTH_PROBE_SET_ID)
			error("Probe Set Name %.*s is too long\n", n, src);
		strncpy(dest, src, n);
		dest[n] = '\0';
	} else {
		if (strncmp(dest, src, n) != 0)
			error("Probe Set Name mismatch: %s %.*s\n", dest, n, src);
	}
}

static inline void check_probe_set_id(char *dest, const char *src)
{
	if (dest[0] == '\0') {
		if (strlen(src) > MAX_LENGTH_PROBE_SET_ID)
			error("Probe Set Name %s is too long\n", src);
		strcpy(dest, src);
	} else {
		if (strcmp(dest, src) != 0)
			error("Probe Set Name mismatch: %s %s\n", dest, src);
	}
}

static int varitr_loop(varitr_t *varitr)
{
	varitr->probe_set_id[0] = '\0';
	if (varitr->data_sets) {
		varitr->nrow++;
		// check whether you have arrived at the last element
		static const int gt[16] = {-1,	  -1, -1, -1,	 -1, -1, GT_AA, GT_BB,
					   GT_AB, -1, -1, GT_NC, -1, -1, -1,	-1};
		for (int i = 0; i < varitr->nsmpl; i++) {
			DataSet *data_set = varitr->data_sets[i];
			if (varitr->nrow > data_set->n_rows)
				return -1;
			read_bytes(data_set->fp, (void *)data_set->buffer, data_set->n_buffer);
			uint32_t n =
				ntohl(*(uint32_t *)&data_set->buffer[data_set->col_offsets[0]]);
			check_n_probe_set_id(varitr->probe_set_id,
					     &data_set->buffer[data_set->col_offsets[0] + 4],
					     (size_t)n);
			varitr->gts[i] = gt[data_set->buffer[data_set->col_offsets[1]] & 0x0F];
			union {
				uint32_t u;
				float f;
			} convert;
			convert.u =
				ntohl(*(uint32_t *)&data_set->buffer[data_set->col_offsets[2]]);
			varitr->conf_arr[i] = convert.f;
			if (varitr->is_axiom[i]) {
				convert.u = ntohl(*(uint32_t *)&data_set
							   ->buffer[data_set->col_offsets[3]]);
				varitr->delta_arr[i] = convert.f;
				convert.u = ntohl(*(uint32_t *)&data_set
							   ->buffer[data_set->col_offsets[4]]);
				varitr->size_arr[i] = convert.f;
				varitr->norm_x_arr[i] =
					expf((varitr->size_arr[i] + varitr->delta_arr[i] * 0.5f)
					     * (float)M_LN2);
				varitr->norm_y_arr[i] =
					expf((varitr->size_arr[i] - varitr->delta_arr[i] * 0.5f)
					     * (float)M_LN2);
			} else {
				convert.u = ntohl(*(uint32_t *)&data_set
							   ->buffer[data_set->col_offsets[3]]);
				varitr->norm_x_arr[i] = convert.f;
				convert.u = ntohl(*(uint32_t *)&data_set
							   ->buffer[data_set->col_offsets[4]]);
				varitr->norm_y_arr[i] = convert.f;
				float log2x = logf(varitr->norm_x_arr[i]) * (float)M_LOG2E;
				float log2y = logf(varitr->norm_y_arr[i]) * (float)M_LOG2E;
				varitr->delta_arr[i] = log2x - log2y;
				varitr->size_arr[i] = (log2x + log2y) * 0.5f;
			}
		}
	} else {
		kstring_t str = {0, 0, NULL};
		int moff = 0, *off = NULL, ncols;
		char *tmp, buf[MAX_LENGTH_PROBE_SET_ID];

		// read genotypes
		if (varitr->calls_fp) {
			if (hts_getline(varitr->calls_fp, KS_SEP_LINE, &str) < 0)
				return -1;
			ncols = ksplit_core(str.s, '\t', &moff, &off);
			if (ncols != 1 + varitr->nsmpl)
				error("Expected %d columns but %d columns found in the calls file\n",
				      1 + varitr->nsmpl, ncols);
			for (int i = 1; i < 1 + varitr->nsmpl; i++)
				varitr->gts[i - 1] = strtol(&str.s[off[i]], &tmp, 10);
			check_probe_set_id(varitr->probe_set_id, &str.s[off[0]]);
		}

		// read confidences
		if (varitr->confidences_fp) {
			if (hts_getline(varitr->confidences_fp, KS_SEP_LINE, &str) < 0)
				return -1;
			ncols = ksplit_core(str.s, '\t', &moff, &off);
			if (ncols != 1 + varitr->nsmpl)
				error("Expected %d columns but %d columns found in the confidences file\n",
				      1 + varitr->nsmpl, ncols);
			for (int i = 1; i < 1 + varitr->nsmpl; i++)
				varitr->conf_arr[i - 1] = strtof(&str.s[off[i]], &tmp);
			check_probe_set_id(varitr->probe_set_id, &str.s[off[0]]);
		}

		// read intensities
		if (varitr->summary_fp) {
			int ret, len;
			do {
				if ((ret = hts_getline(varitr->summary_fp, KS_SEP_LINE, &str))
				    < 0)
					return -1;
				ncols = ksplit_core(str.s, '\t', &moff, &off);
				if (ncols != 1 + varitr->nsmpl)
					error("Expected %d columns but %d columns found in the summary file\n",
					      1 + varitr->nsmpl, ncols);
				len = strlen(&str.s[off[0]]);
				if (str.s[off[0] + len - 2] != '-'
				    || str.s[off[0] + len - 1] != 'A')
					error("Found Probe Set ID %s while a -A was expected\n",
					      &str.s[off[0]]);
				str.s[off[0] + len - 2] = '\0';
				// check whether the next line contains the expected -B
				// probeset_id
				if (len - 2 > MAX_LENGTH_PROBE_SET_ID)
					error("Cannot read Probe Set %s intensities\n",
					      &str.s[off[0]]);
				ret = hpeek(varitr->summary_fp->fp.hfile, buf, len);
			} while (ret < len || strncmp(&str.s[off[0]], buf, len - 2) != 0
				 || buf[len - 2] != '-' || buf[len - 1] != 'B');
			if (ret < 0)
				return -1;

			for (int i = 1; i < 1 + varitr->nsmpl; i++)
				varitr->norm_x_arr[i - 1] = strtof(&str.s[off[i]], &tmp);
			if (hts_getline(varitr->summary_fp, KS_SEP_LINE, &str) <= 0)
				error("Summary file ended prematurely\n");
			ncols = ksplit_core(str.s, '\t', &moff, &off);
			if (ncols != 1 + varitr->nsmpl)
				error("Expected %d columns but %d columns found in the summary file\n",
				      1 + varitr->nsmpl, ncols);
			len = strlen(&str.s[off[0]]);
			str.s[off[0] + len - 2] = '\0';
			for (int i = 1; i < 1 + varitr->nsmpl; i++) {
				varitr->norm_y_arr[i - 1] = strtof(&str.s[off[i]], &tmp);
				float log2x = logf(varitr->norm_x_arr[i - 1]) * (float)M_LOG2E;
				float log2y = logf(varitr->norm_y_arr[i - 1]) * (float)M_LOG2E;
				varitr->delta_arr[i - 1] = log2x - log2y;
				varitr->size_arr[i - 1] = (log2x + log2y) * 0.5f;
			}
			check_probe_set_id(varitr->probe_set_id, &str.s[off[0]]);
		}

		free(str.s);
		free(off);
	}
	return 0;
}

static void varitr_destroy(varitr_t *varitr)
{
	free(varitr->is_axiom);
	if (varitr->calls_fp) {
		if (hgetc(varitr->calls_fp->fp.hfile) != EOF)
			fprintf(stderr, "Warning: End of calls file was not reached\n");
		hts_close(varitr->calls_fp);
	}
	if (varitr->confidences_fp) {
		if (hgetc(varitr->confidences_fp->fp.hfile) != EOF)
			fprintf(stderr, "Warning: End of confidences file was not reached\n");
		hts_close(varitr->confidences_fp);
	}
	if (varitr->summary_fp) {
		if (hgetc(varitr->summary_fp->fp.hfile) != EOF)
			fprintf(stderr, "Warning: End of summary file was not reached\n");
		hts_close(varitr->summary_fp);
	}

	free(varitr->gts);
	free(varitr->conf_arr);
	free(varitr->norm_x_arr);
	free(varitr->norm_y_arr);
	free(varitr->delta_arr);
	free(varitr->size_arr);
	free(varitr->baf_arr);
	free(varitr->lrr_arr);
	free(varitr);
}

/****************************************
 * OUTPUT FUNCTIONS                     *
 ****************************************/

static bcf_hdr_t *hdr_init(const faidx_t *fai, int flags)
{
	bcf_hdr_t *hdr = bcf_hdr_init("w");
	int n = faidx_nseq(fai);
	for (int i = 0; i < n; i++) {
		const char *seq = faidx_iseq(fai, i);
		int len = faidx_seq_len(fai, seq);
		bcf_hdr_printf(hdr, "##contig=<ID=%s,length=%d>", seq, len);
	}
	bcf_hdr_append(hdr,
		       "##INFO=<ID=ALLELE_A,Number=1,Type=Integer,Description=\"A allele\">");
	bcf_hdr_append(hdr,
		       "##INFO=<ID=ALLELE_B,Number=1,Type=Integer,Description=\"B allele\">");
	bcf_hdr_append(
		hdr,
		"##INFO=<ID=DBSNP_RS_ID,Number=1,Type=String,Description=\"dbSNP RS ID\">");
	bcf_hdr_append(
		hdr,
		"##INFO=<ID=AFFY_SNP_ID,Number=1,Type=String,Description=\"Affymetrix SNP ID\">");
	if (flags & MODELS_LOADED) {
		bcf_hdr_append(
			hdr,
			"##INFO=<ID=meanX_AA,Number=1,Type=Float,Description=\"Mean of normalized DELTA for AA diploid cluster\">");
		bcf_hdr_append(
			hdr,
			"##INFO=<ID=meanX_AB,Number=1,Type=Float,Description=\"Mean of normalized DELTA for AB diploid cluster\">");
		bcf_hdr_append(
			hdr,
			"##INFO=<ID=meanX_BB,Number=1,Type=Float,Description=\"Mean of normalized DELTA for BB diploid cluster\">");
		bcf_hdr_append(
			hdr,
			"##INFO=<ID=varX_AA,Number=1,Type=Float,Description=\"Variance of normalized DELTA for AA diploid cluster\">");
		bcf_hdr_append(
			hdr,
			"##INFO=<ID=varX_AB,Number=1,Type=Float,Description=\"Variance of normalized DELTA for AB diploid cluster\">");
		bcf_hdr_append(
			hdr,
			"##INFO=<ID=varX_BB,Number=1,Type=Float,Description=\"Variance of normalized DELTA for BB diploid cluster\">");
		bcf_hdr_append(
			hdr,
			"##INFO=<ID=nObsMean_AA,Number=1,Type=Float,Description=\"Number of AA calls in training set for diploid mean\">");
		bcf_hdr_append(
			hdr,
			"##INFO=<ID=nObsMean_AB,Number=1,Type=Float,Description=\"Number of AB calls in training set for diploid mean\">");
		bcf_hdr_append(
			hdr,
			"##INFO=<ID=nObsMean_BB,Number=1,Type=Float,Description=\"Number of BB calls in training set for diploid mean\">");
		bcf_hdr_append(
			hdr,
			"##INFO=<ID=nObsVar_AA,Number=1,Type=Float,Description=\"Number of AA calls in training set for diploid variance\">");
		bcf_hdr_append(
			hdr,
			"##INFO=<ID=nObsVar_AB,Number=1,Type=Float,Description=\"Number of AB calls in training set for diploid variance\">");
		bcf_hdr_append(
			hdr,
			"##INFO=<ID=nObsVar_BB,Number=1,Type=Float,Description=\"Number of BB calls in training set for diploid variance\">");
		bcf_hdr_append(
			hdr,
			"##INFO=<ID=meanY_AA,Number=1,Type=Float,Description=\"Mean of normalized SIZE for AA diploid cluster\">");
		bcf_hdr_append(
			hdr,
			"##INFO=<ID=meanY_AB,Number=1,Type=Float,Description=\"Mean of normalized SIZE for AB diploid cluster\">");
		bcf_hdr_append(
			hdr,
			"##INFO=<ID=meanY_BB,Number=1,Type=Float,Description=\"Mean of normalized SIZE for BB diploid cluster\">");
		bcf_hdr_append(
			hdr,
			"##INFO=<ID=varY_AA,Number=1,Type=Float,Description=\"Variance of normalized SIZE for AA diploid cluster\">");
		bcf_hdr_append(
			hdr,
			"##INFO=<ID=varY_AB,Number=1,Type=Float,Description=\"Variance of normalized SIZE for AB diploid cluster\">");
		bcf_hdr_append(
			hdr,
			"##INFO=<ID=varY_BB,Number=1,Type=Float,Description=\"Variance of normalized SIZE for BB diploid cluster\">");
		bcf_hdr_append(
			hdr,
			"##INFO=<ID=covarXY_AA,Number=1,Type=Float,Description=\"Covariance for AA diploid cluster\">");
		bcf_hdr_append(
			hdr,
			"##INFO=<ID=covarXY_AB,Number=1,Type=Float,Description=\"Covariance for AB diploid cluster\">");
		bcf_hdr_append(
			hdr,
			"##INFO=<ID=covarXY_BB,Number=1,Type=Float,Description=\"Covariance for BB diploid cluster\">");
		bcf_hdr_append(
			hdr,
			"##INFO=<ID=meanX_AA.1,Number=1,Type=Float,Description=\"Mean of normalized DELTA for AA haploid cluster\">");
		bcf_hdr_append(
			hdr,
			"##INFO=<ID=meanX_AB.1,Number=1,Type=Float,Description=\"Mean of normalized DELTA for AB haploid cluster\">");
		bcf_hdr_append(
			hdr,
			"##INFO=<ID=meanX_BB.1,Number=1,Type=Float,Description=\"Mean of normalized DELTA for BB haploid cluster\">");
		bcf_hdr_append(
			hdr,
			"##INFO=<ID=varX_AA.1,Number=1,Type=Float,Description=\"Variance of normalized DELTA for AA haploid cluster\">");
		bcf_hdr_append(
			hdr,
			"##INFO=<ID=varX_AB.1,Number=1,Type=Float,Description=\"Variance of normalized DELTA for AB haploid cluster\">");
		bcf_hdr_append(
			hdr,
			"##INFO=<ID=varX_BB.1,Number=1,Type=Float,Description=\"Variance of normalized DELTA for BB haploid cluster\">");
		bcf_hdr_append(
			hdr,
			"##INFO=<ID=nObsMean_AA.1,Number=1,Type=Float,Description=\"Number of AA calls in training set for haploid mean\">");
		bcf_hdr_append(
			hdr,
			"##INFO=<ID=nObsMean_AB.1,Number=1,Type=Float,Description=\"Number of AB calls in training set for haploid mean\">");
		bcf_hdr_append(
			hdr,
			"##INFO=<ID=nObsMean_BB.1,Number=1,Type=Float,Description=\"Number of BB calls in training set for haploid mean\">");
		bcf_hdr_append(
			hdr,
			"##INFO=<ID=nObsVar_AA.1,Number=1,Type=Float,Description=\"Number of AA calls in training set for haploid variance\">");
		bcf_hdr_append(
			hdr,
			"##INFO=<ID=nObsVar_AB.1,Number=1,Type=Float,Description=\"Number of AB calls in training set for haploid variance\">");
		bcf_hdr_append(
			hdr,
			"##INFO=<ID=nObsVar_BB.1,Number=1,Type=Float,Description=\"Number of BB calls in training set for haploid variance\">");
		bcf_hdr_append(
			hdr,
			"##INFO=<ID=meanY_AA.1,Number=1,Type=Float,Description=\"Mean of normalized SIZE for AA haploid cluster\">");
		bcf_hdr_append(
			hdr,
			"##INFO=<ID=meanY_AB.1,Number=1,Type=Float,Description=\"Mean of normalized SIZE for AB haploid cluster\">");
		bcf_hdr_append(
			hdr,
			"##INFO=<ID=meanY_BB.1,Number=1,Type=Float,Description=\"Mean of normalized SIZE for BB haploid cluster\">");
		bcf_hdr_append(
			hdr,
			"##INFO=<ID=varY_AA.1,Number=1,Type=Float,Description=\"Variance of normalized SIZE for AA haploid cluster\">");
		bcf_hdr_append(
			hdr,
			"##INFO=<ID=varY_AB.1,Number=1,Type=Float,Description=\"Variance of normalized SIZE for AB haploid cluster\">");
		bcf_hdr_append(
			hdr,
			"##INFO=<ID=varY_BB.1,Number=1,Type=Float,Description=\"Variance of normalized SIZE for BB haploid cluster\">");
		bcf_hdr_append(
			hdr,
			"##INFO=<ID=covarXY_AA.1,Number=1,Type=Float,Description=\"Covariance for AA haploid cluster\">");
		bcf_hdr_append(
			hdr,
			"##INFO=<ID=covarXY_AB.1,Number=1,Type=Float,Description=\"Covariance for AB haploid cluster\">");
		bcf_hdr_append(
			hdr,
			"##INFO=<ID=covarXY_BB.1,Number=1,Type=Float,Description=\"Covariance for BB haploid cluster\">");
	}
	if (flags & CALLS_LOADED)
		bcf_hdr_append(
			hdr, "##FORMAT=<ID=GT,Number=1,Type=String,Description=\"Genotype\">");
	if (flags & CONFIDENCES_LOADED)
		bcf_hdr_append(
			hdr,
			"##FORMAT=<ID=CONF,Number=1,Type=Float,Description=\"Genotype confidences\">");
	if (flags & SUMMARY_LOADED) {
		bcf_hdr_append(
			hdr,
			"##FORMAT=<ID=NORMX,Number=1,Type=Float,Description=\"Normalized X intensity\">");
		bcf_hdr_append(
			hdr,
			"##FORMAT=<ID=NORMY,Number=1,Type=Float,Description=\"Normalized Y intensity\">");
		bcf_hdr_append(
			hdr,
			"##FORMAT=<ID=DELTA,Number=1,Type=Float,Description=\"Normalized contrast value\">");
		bcf_hdr_append(
			hdr,
			"##FORMAT=<ID=SIZE,Number=1,Type=Float,Description=\"Normalized size value\">");
	}
	if ((flags & SUMMARY_LOADED) && (flags & MODELS_LOADED)) {
		bcf_hdr_append(
			hdr,
			"##FORMAT=<ID=BAF,Number=1,Type=Float,Description=\"B Allele Frequency\">");
		bcf_hdr_append(
			hdr,
			"##FORMAT=<ID=LRR,Number=1,Type=Float,Description=\"Log R Ratio\">");
	}
	return hdr;
}

// adjust cluster centers (using apt-probeset-genotype posteriors as priors)
// similar to
// http://github.com/WGLab/PennCNV/blob/master/affy/bin/generate_affy_geno_cluster.pl
static void adjust_clusters(const int *gts, const float *x, const float *y, int n, snp_t *snp)
{
	snp->aa.xm *= 0.2f;
	snp->ab.xm *= 0.2f;
	snp->bb.xm *= 0.2f;
	snp->aa.ym *= 0.2f;
	snp->ab.ym *= 0.2f;
	snp->bb.ym *= 0.2f;
	snp->aa.k = 0.2f;
	snp->ab.k = 0.2f;
	snp->bb.k = 0.2f;

	for (int i = 0; i < n; i++) {
		switch (gts[i]) {
		case GT_AA:
			snp->aa.k++;
			snp->aa.xm += x[i];
			snp->aa.ym += y[i];
			break;
		case GT_AB:
			snp->ab.k++;
			snp->ab.xm += x[i];
			snp->ab.ym += y[i];
			break;
		case GT_BB:
			snp->bb.k++;
			snp->bb.xm += x[i];
			snp->bb.ym += y[i];
			break;
		default:
			break;
		}
	}

	snp->aa.xm /= snp->aa.k;
	snp->ab.xm /= snp->ab.k;
	snp->bb.xm /= snp->bb.k;
	snp->aa.ym /= snp->aa.k;
	snp->ab.ym /= snp->ab.k;
	snp->bb.ym /= snp->bb.k;
}

static void update_info_cluster(const bcf_hdr_t *hdr, bcf1_t *rec, const char **info_str,
				const snp_t *snp)
{
	bcf_update_info_float(hdr, rec, info_str[0], &snp->aa.xm, 1);
	bcf_update_info_float(hdr, rec, info_str[1], &snp->ab.xm, 1);
	bcf_update_info_float(hdr, rec, info_str[2], &snp->bb.xm, 1);
	bcf_update_info_float(hdr, rec, info_str[3], &snp->aa.xss, 1);
	bcf_update_info_float(hdr, rec, info_str[4], &snp->ab.xss, 1);
	bcf_update_info_float(hdr, rec, info_str[5], &snp->bb.xss, 1);
	bcf_update_info_float(hdr, rec, info_str[6], &snp->aa.k, 1);
	bcf_update_info_float(hdr, rec, info_str[7], &snp->ab.k, 1);
	bcf_update_info_float(hdr, rec, info_str[8], &snp->bb.k, 1);
	bcf_update_info_float(hdr, rec, info_str[9], &snp->aa.v, 1);
	bcf_update_info_float(hdr, rec, info_str[10], &snp->ab.v, 1);
	bcf_update_info_float(hdr, rec, info_str[11], &snp->bb.v, 1);
	bcf_update_info_float(hdr, rec, info_str[12], &snp->aa.ym, 1);
	bcf_update_info_float(hdr, rec, info_str[13], &snp->ab.ym, 1);
	bcf_update_info_float(hdr, rec, info_str[14], &snp->bb.ym, 1);
	bcf_update_info_float(hdr, rec, info_str[15], &snp->aa.yss, 1);
	bcf_update_info_float(hdr, rec, info_str[16], &snp->ab.yss, 1);
	bcf_update_info_float(hdr, rec, info_str[17], &snp->bb.yss, 1);
	bcf_update_info_float(hdr, rec, info_str[18], &snp->aa.xyss, 1);
	bcf_update_info_float(hdr, rec, info_str[19], &snp->ab.xyss, 1);
	bcf_update_info_float(hdr, rec, info_str[20], &snp->bb.xyss, 1);
}

// compute LRR and BAF
// similar to
// http://github.com/WGLab/PennCNV/blob/master/affy/bin/normalize_affy_geno_cluster.pl
static void compute_baf_lrr(const float *norm_x, const float *norm_y, int n, const snp_t *snp,
			    int is_birdseed, float *baf, float *lrr)
{
	float aa_theta, ab_theta, bb_theta, aa_r, ab_r, bb_r;

	if (is_birdseed) {
		aa_theta = atanf(snp->aa.ym / snp->aa.xm) * (float)M_2_PI;
		ab_theta = atanf(snp->ab.ym / snp->ab.xm) * (float)M_2_PI;
		bb_theta = atanf(snp->bb.ym / snp->bb.xm) * (float)M_2_PI;
		aa_r = snp->aa.xm + snp->aa.ym;
		ab_r = snp->ab.xm + snp->ab.ym;
		bb_r = snp->bb.xm + snp->bb.ym;
	} else {
		aa_theta = atanf(expf(-snp->aa.xm * (float)M_LN2)) * (float)M_2_PI;
		ab_theta = atanf(expf(-snp->ab.xm * (float)M_LN2)) * (float)M_2_PI;
		bb_theta = atanf(expf(-snp->bb.xm * (float)M_LN2)) * (float)M_2_PI;
		aa_r = expf(snp->aa.ym * (float)M_LN2) * 2.0f
		       * coshf(snp->aa.xm * 0.5f * (float)M_LN2);
		ab_r = expf(snp->ab.ym * (float)M_LN2) * 2.0f
		       * coshf(snp->ab.xm * 0.5f * (float)M_LN2);
		bb_r = expf(snp->bb.ym * (float)M_LN2) * 2.0f
		       * coshf(snp->bb.xm * 0.5f * (float)M_LN2);
	}

	// handles chromosome Y SNPs
	if (snp->copynumber == 1) {
		ab_theta = (aa_theta + bb_theta) * 0.5f;
		ab_r = (aa_r + bb_r) * 0.5f;
	}

	for (int i = 0; i < n; i++) {
		float ilmn_theta = atanf(norm_y[i] / norm_x[i]) * (float)M_2_PI;
		float ilmn_r = norm_x[i] + norm_y[i];
		get_baf_lrr(ilmn_theta, ilmn_r, aa_theta, ab_theta, bb_theta, aa_r, ab_r, bb_r,
			    &baf[i], &lrr[i]);
	}
}

static void process(faidx_t *fai, const annot_t *annot, models_t *models, varitr_t *varitr,
		    htsFile *out_fh, bcf_hdr_t *hdr, int flags)
{
	if (bcf_hdr_write(out_fh, hdr) < 0)
		error("Unable to write to output VCF file\n");
	if (bcf_hdr_sync(hdr) < 0)
		error_errno("[%s] Failed to update header",
			    __func__); // updates the number of samples
	int nsmpl = bcf_hdr_nsamples(hdr);
	if ((flags & ADJUST_CLUSTERS) && (nsmpl < 100))
		fprintf(stderr,
			"Warning: adjusting clusters with %d sample(s) is not recommended\n",
			nsmpl);

	bcf1_t *rec = bcf_init();
	char ref_base[] = {'\0', '\0'};
	kstring_t allele_a = {0, 0, NULL};
	kstring_t allele_b = {0, 0, NULL};
	kstring_t flank = {0, 0, NULL};

	int32_t *gt_arr = (int32_t *)malloc(nsmpl * 2 * sizeof(int32_t));
	float *baf_arr = (float *)malloc(nsmpl * sizeof(float));
	float *lrr_arr = (float *)malloc(nsmpl * sizeof(float));

	int i = 0, n_missing = 0, n_no_models = 0, n_skipped = 0;
	for (i = 0; i < annot->n_records; i++) {
		// identify variants to use for next VCF record
		int idx;
		if (varitr) {
			if (varitr_loop(varitr) < 0)
				break;
			int ret = khash_str2int_get(annot->probe_set_id, varitr->probe_set_id,
						    &idx);
			if (ret < 0)
				error("Probe Set %s not found in manifest file\n",
				      varitr->probe_set_id);
		} else {
			idx = i;
		}
		record_t *record = &annot->records[idx];

		bcf_clear(rec);
		rec->n_sample = nsmpl;
		rec->rid = bcf_hdr_name2id_flexible(hdr, record->chromosome);
		rec->pos = record->position - 1;
		if (rec->rid < 0 || rec->pos < 0 || record->strand < 0 || !record->flank) {
			if (flags & VERBOSE)
				fprintf(stderr, "Skipping unlocalized marker %s\n",
					record->probe_set_id);
			n_skipped++;
			continue;
		}
		bcf_update_id(hdr, rec, record->probe_set_id);

		flank.l = 0;
		kputs(record->flank, &flank);
		strupper(flank.s);
		if (record->strand)
			flank_reverse_complement(flank.s);

		int32_t allele_b_idx;
		allele_a.l = allele_b.l = 0;
		if (strchr(flank.s, '-')) {
			int ref_is_del =
				get_indel_alleles(flank.s, fai, bcf_seqname(hdr, rec), rec->pos,
						  0, ref_base, &allele_a, &allele_b);
			if (ref_is_del < 0) {
				if (flags & VERBOSE)
					fprintf(stderr,
						"Unable to determine alleles for indel %s\n",
						record->probe_set_id);
				n_missing++;
			}
			if (ref_is_del == 0)
				rec->pos--;
			allele_b_idx = ref_is_del < 0 ? 1 : ref_is_del;
		} else {
			const char *left = strchr(flank.s, '[');
			const char *middle = strchr(flank.s, '/');
			const char *right = strchr(flank.s, ']');
			if (!left || !middle || !right)
				error("Flank sequence is malformed: %s\n", flank.s);

			kputsn(left + 1, middle - left - 1, &allele_a);
			kputsn(middle + 1, right - middle - 1, &allele_b);
			ref_base[0] = get_ref_base(fai, hdr, rec);
			allele_b_idx = get_allele_b_idx(ref_base[0], allele_a.s, allele_b.s);
		}
		int32_t allele_a_idx = get_allele_a_idx(allele_b_idx);
		const char *alleles[3];
		int nals = alleles_ab_to_vcf(alleles, ref_base, allele_a.s, allele_b.s,
					     allele_b_idx);
		if (nals < 0)
			error("Unable to process Probe Set %s\n", record->probe_set_id);
		bcf_update_alleles(hdr, rec, alleles, nals);
		bcf_update_info_int32(hdr, rec, "ALLELE_A", &allele_a_idx, 1);
		bcf_update_info_int32(hdr, rec, "ALLELE_B", &allele_b_idx, 1);
		if (record->dbsnp_rs_id)
			bcf_update_info_string(hdr, rec, "DBSNP_RS_ID", record->dbsnp_rs_id);
		if (record->affy_snp_id)
			bcf_update_info_string(hdr, rec, "AFFY_SNP_ID", record->affy_snp_id);

		if (varitr) {
			if (varitr->data_sets || varitr->calls_fp) {
				for (int i = 0; i < nsmpl; i++) {
					switch (varitr->gts[i]) {
					case GT_NC:
						gt_arr[2 * i] = bcf_gt_missing;
						gt_arr[2 * i + 1] = bcf_gt_missing;
						break;
					case GT_AA:
						gt_arr[2 * i] = bcf_gt_unphased(allele_a_idx);
						gt_arr[2 * i + 1] =
							bcf_gt_unphased(allele_a_idx);
						break;
					case GT_AB:
						gt_arr[2 * i] = bcf_gt_unphased(
							min(allele_a_idx, allele_b_idx));
						gt_arr[2 * i + 1] = bcf_gt_unphased(
							max(allele_a_idx, allele_b_idx));
						break;
					case GT_BB:
						gt_arr[2 * i] = bcf_gt_unphased(allele_b_idx);
						gt_arr[2 * i + 1] =
							bcf_gt_unphased(allele_b_idx);
						break;
					default:
						error("Genotype for Probe Set ID %s is malformed: %d\n",
						      record->probe_set_id, varitr->gts[i]);
						break;
					}
				}
				bcf_update_genotypes(hdr, rec, gt_arr, nsmpl * 2);
			}

			if (varitr->data_sets || varitr->confidences_fp)
				bcf_update_format_float(hdr, rec, "CONF", varitr->conf_arr,
							nsmpl);

			if (varitr->data_sets || varitr->summary_fp) {
				bcf_update_format_float(hdr, rec, "NORMX", varitr->norm_x_arr,
							nsmpl);
				bcf_update_format_float(hdr, rec, "NORMY", varitr->norm_y_arr,
							nsmpl);
				bcf_update_format_float(hdr, rec, "DELTA", varitr->delta_arr,
							nsmpl);
				bcf_update_format_float(hdr, rec, "SIZE", varitr->size_arr,
							nsmpl);
			}
		}

		if (models) {
			int rets[2], idxs[2];
			for (int i = 0; i < 2; i++) {
				rets[i] = khash_str2int_get(models->probe_set_id[i],
							    record->probe_set_id, &idxs[i]);
			}
			static const char *hap_info_str[] = {
				"meanX_AA.1",	 "meanX_AB.1",	  "meanX_BB.1",
				"varX_AA.1",	 "varX_AB.1",	  "varX_BB.1",
				"nObsMean_AA.1", "nObsMean_AB.1", "nObsMean_BB.1",
				"nObsVar_AA.1",	 "nObsVar_AB.1",  "nObsVar_BB.1",
				"meanY_AA.1",	 "meanY_AB.1",	  "meanY_BB.1",
				"varY_AA.1",	 "varY_AB.1",	  "varY_BB.1",
				"covarXY_AA.1",	 "covarXY_AB.1",  "covarXY_BB.1"};
			static const char *dip_info_str[] = {
				"meanX_AA",    "meanX_AB",   "meanX_BB",    "varX_AA",
				"varX_AB",     "varX_BB",    "nObsMean_AA", "nObsMean_AB",
				"nObsMean_BB", "nObsVar_AA", "nObsVar_AB",  "nObsVar_BB",
				"meanY_AA",    "meanY_AB",   "meanY_BB",    "varY_AA",
				"varY_AB",     "varY_BB",    "covarXY_AA",  "covarXY_AB",
				"covarXY_BB"};
			if (rets[0] >= 0)
				update_info_cluster(hdr, rec, hap_info_str,
						    &models->snps[0][idxs[0]]);
			if (rets[1] >= 0)
				update_info_cluster(hdr, rec, dip_info_str,
						    &models->snps[1][idxs[1]]);
			snp_t *snp = rets[1] >= 0 ? &models->snps[1][idxs[1]]
						  : (rets[0] >= 0 ? &models->snps[0][idxs[0]]
								  : NULL);
			if (!snp) {
				n_no_models++;
				if (flags & VERBOSE)
					fprintf(stderr,
						"Warning: SNP model for Probe Set ID %s was not found\n",
						record->probe_set_id);
			} else {
				if (flags & ADJUST_CLUSTERS)
					adjust_clusters(varitr->gts,
							models->is_birdseed ? varitr->norm_x_arr
									    : varitr->delta_arr,
							models->is_birdseed ? varitr->norm_y_arr
									    : varitr->size_arr,
							nsmpl, snp);
				if (flags & SUMMARY_LOADED) {
					compute_baf_lrr(varitr->norm_x_arr, varitr->norm_y_arr,
							nsmpl, snp, models->is_birdseed,
							baf_arr, lrr_arr);
					bcf_update_format_float(hdr, rec, "BAF", baf_arr,
								nsmpl);
					bcf_update_format_float(hdr, rec, "LRR", lrr_arr,
								nsmpl);
				}
			}
		}

		if (bcf_write(out_fh, hdr, rec) < 0)
			error("Unable to write to output VCF file\n");
	}
	if (models)
		fprintf(stderr,
			"Lines   total/missing-reference/missing-models/skipped:\t%d/%d/%d/%d\n",
			i, n_missing, n_no_models, n_skipped);
	else
		fprintf(stderr, "Lines   total/missing-reference/skipped:\t%d/%d/%d\n", i,
			n_missing, n_skipped);

	free(gt_arr);
	free(baf_arr);
	free(lrr_arr);

	free(allele_a.s);
	free(allele_b.s);
	free(flank.s);

	bcf_destroy(rec);
	return;
}

/****************************************
 * PLUGIN                               *
 ****************************************/

const char *about(void)
{
	return "convert Affymetrix files to VCF.\n";
}

static const char *usage_text(void)
{
	return "\n"
	       "About: convert Affymetrix apt-probeset-genotype output files to VCF. (version " AFFY2VCF_VERSION
	       " https://github.com/freeseek/gtc2vcf)\n"
	       "Usage: bcftools +affy2vcf [options] --csv <file> --fasta-ref <file> [<A.chp> ...]\n"
	       "\n"
	       "Plugin options:\n"
	       "    -c, --csv <file>              CSV manifest file\n"
	       "    -f, --fasta-ref <file>        reference sequence in fasta format\n"
	       "        --set-cache-size <int>    select fasta cache size in bytes\n"
	       "        --calls <file>            apt-probeset-genotype calls output\n"
	       "        --confidences <file>      apt-probeset-genotype confidences output\n"
	       "        --summary <file>          apt-probeset-genotype summary output\n"
	       "        --models <file>           apt-probeset-genotype SNP models output\n"
	       "        --report <file>           apt-probeset-genotype report output\n"
	       "        --chps <dir|file>         input CHP files rather than tab delimited files\n"
	       "        --cel <file>              input CEL files rather CHP files\n"
	       "        --adjust-clusters         adjust cluster centers in (Contrast, Size) space (requires --models)\n"
	       "    -x, --sex <file>              output apt-probeset-genotype gender estimate into file (requires --report)\n"
	       "        --no-version              do not append version and command line to the header\n"
	       "    -o, --output <file>           write output to a file [standard output]\n"
	       "    -O, --output-type <b|u|z|v>   b: compressed BCF, u: uncompressed BCF, z: compressed VCF, v: uncompressed VCF [v]\n"
	       "        --threads <int>           number of extra output compression threads [0]\n"
	       "    -v, --verbose                 print verbose information\n"
	       "\n"
	       "Manifest options:\n"
	       "        --fasta-flank             output flank sequence in FASTA format (requires --csv)\n"
	       "    -s, --sam-flank <file>        input source sequence alignment in SAM/BAM format (requires --csv)\n"
	       "\n"
	       "Examples:\n"
	       "    bcftools +affy2vcf \\\n"
	       "        --csv GenomeWideSNP_6.na35.annot.csv \\\n"
	       "        --fasta-ref human_g1k_v37.fasta \\\n"
	       "        --chps cc-chp/ \\\n"
	       "        --models AxiomGT1.snp-posteriors.txt \\\n"
	       "        --output AxiomGT1.vcf\n"
	       "    bcftools +affy2vcf \\\n"
	       "        --csv GenomeWideSNP_6.na35.annot.csv \\\n"
	       "        --fasta-ref human_g1k_v37.fasta \\\n"
	       "        --calls AxiomGT1.calls.txt \\\n"
	       "        --confidences AxiomGT1.confidences.txt \\\n"
	       "        --summary AxiomGT1.summary.txt \\\n"
	       "        --models AxiomGT1.snp-posteriors.txt \\\n"
	       "        --output AxiomGT1.vcf\n"
	       "\n"
	       "Examples of manifest file options:\n"
	       "    bcftools +affy2vcf -c GenomeWideSNP_6.na35.annot.csv --fasta-flank -o GenomeWideSNP_6.fasta\n"
	       "    bwa mem -M GCA_000001405.15_GRCh38_no_alt_analysis_set.fna GenomeWideSNP_6.fasta -o GenomeWideSNP_6.sam\n"
	       "    bcftools +affy2vcf -c GenomeWideSNP_6.na35.annot.csv -s GenomeWideSNP_6.sam -o GenomeWideSNP_6.na35.annot.GRCh38.csv\n"
	       "\n";
}

int run(int argc, char *argv[])
{
	const char *ref_fname = NULL;
	const char *sex_fname = NULL;
	const char *csv_fname = NULL;
	const char *calls_fname = NULL;
	const char *confidences_fname = NULL;
	const char *summary_fname = NULL;
	const char *models_fname = NULL;
	const char *report_fname = NULL;
	const char *pathname = NULL;
	const char *output_fname = "-";
	const char *sam_fname = NULL;
	int flags = 0;
	int output_type = FT_VCF;
	int cache_size = 0;
	int n_threads = 0;
	int record_cmd_line = 1;
	int fasta_flank = 0;
	faidx_t *fai = NULL;

	static struct option loptions[] = {{"csv", required_argument, NULL, 'c'},
					   {"fasta-ref", required_argument, NULL, 'f'},
					   {"set-cache-size", required_argument, NULL, 1},
					   {"calls", required_argument, NULL, 2},
					   {"confidences", required_argument, NULL, 3},
					   {"summary", required_argument, NULL, 4},
					   {"models", required_argument, NULL, 5},
					   {"report", required_argument, NULL, 6},
					   {"chps", required_argument, NULL, 7},
					   {"cel", no_argument, NULL, 10},
					   {"adjust-clusters", no_argument, NULL, 11},
					   {"sex", required_argument, NULL, 'x'},
					   {"no-version", no_argument, NULL, 8},
					   {"output", required_argument, NULL, 'o'},
					   {"output-type", required_argument, NULL, 'O'},
					   {"threads", required_argument, NULL, 9},
					   {"verbose", no_argument, NULL, 'v'},
					   {"fasta-flank", no_argument, NULL, 12},
					   {"sam-flank", required_argument, NULL, 's'},
					   {NULL, 0, NULL, 0}};
	int c;
	while ((c = getopt_long(argc, argv, "h?c:f:x:o:O:vs:", loptions, NULL)) >= 0) {
		switch (c) {
		case 'c':
			csv_fname = optarg;
			break;
		case 'f':
			ref_fname = optarg;
			break;
		case 1:
			cache_size = strtol(optarg, NULL, 0);
			break;
		case 2:
			calls_fname = optarg;
			flags |= CALLS_LOADED;
			break;
		case 3:
			confidences_fname = optarg;
			flags |= CONFIDENCES_LOADED;
			break;
		case 4:
			summary_fname = optarg;
			flags |= SUMMARY_LOADED;
			break;
		case 5:
			models_fname = optarg;
			flags |= MODELS_LOADED;
			break;
		case 6:
			report_fname = optarg;
			break;
		case 7:
			pathname = optarg;
			break;
		case 10:
			flags |= LOAD_CEL;
			break;
		case 11:
			flags |= ADJUST_CLUSTERS;
			break;
		case 'x':
			sex_fname = optarg;
			break;
		case 8:
			record_cmd_line = 0;
			break;
		case 'o':
			output_fname = optarg;
			break;
		case 'O':
			switch (optarg[0]) {
			case 'b':
				output_type = FT_BCF_GZ;
				break;
			case 'u':
				output_type = FT_BCF;
				break;
			case 'z':
				output_type = FT_VCF_GZ;
				break;
			case 'v':
				output_type = FT_VCF;
				break;
			default:
				error("The output type \"%s\" not recognised\n", optarg);
			}
			break;
		case 9:
			n_threads = strtol(optarg, NULL, 0);
			break;
		case 'v':
			flags |= VERBOSE;
			break;
		case 12:
			fasta_flank = 1;
			break;
		case 's':
			sam_fname = optarg;
			break;
		case 'h':
		case '?':
		default:
			error("%s", usage_text());
		}
	}

	int nfiles = 0;
	char **filenames = NULL;
	if (pathname) {
		filenames = get_file_list(pathname, flags & LOAD_CEL ? "CEL" : "chp", &nfiles);
	} else {
		nfiles = argc - optind;
		filenames = argv + optind;
	}
	uint8_t *magic = (uint8_t *)malloc(nfiles * sizeof(uint8_t *));
	void **files = (void **)malloc(nfiles * sizeof(void *));

	if (csv_fname) {
		if (fasta_flank && sam_fname)
			error("Only one of --fasta-flank or --sam-flank options can be used at once\n%s",
			      usage_text());
		if (!fasta_flank && !sam_fname && !ref_fname)
			error("Expected one of --fasta-flank or --sam-flank or --fasta-ref options\n%s",
			      usage_text());
		if ((flags & ADJUST_CLUSTERS) && (!summary_fname || !models_fname))
			error("Expected --summary and --models options with --adjust-clusters option\n%s",
			      usage_text());
		if (sex_fname && !report_fname)
			error("Expected --report option with --sex option\n%s", usage_text());
		if (nfiles > 0 && (calls_fname || confidences_fname || summary_fname))
			error("Cannot load tables --calls, --confidences, --summary if CHP files provided instead\n%s",
			      usage_text());
	} else if (nfiles == 0) {
		error("%s", usage_text());
	}

	// beginning of plugin run
	fprintf(stderr, "affy2vcf " AFFY2VCF_VERSION " https://github.com/freeseek/gtc2vcf\n");

	if (nfiles > 0 && !(flags & LOAD_CEL))
		flags |= CALLS_LOADED | CONFIDENCES_LOADED | SUMMARY_LOADED;

	// make sure the process is allowed to open enough files
	struct rlimit lim;
	getrlimit(RLIMIT_NOFILE, &lim);
	if (nfiles + 7 > lim.rlim_max)
		error("On this system you cannot open more than %ld files at once while %d is required\n",
		      lim.rlim_max, nfiles + 7);
	if (nfiles + 7 > lim.rlim_cur) {
		lim.rlim_cur = nfiles + 7;
		setrlimit(RLIMIT_NOFILE, &lim);
	}

	if (sex_fname) {
		fprintf(stderr, "Reading report file %s\n", report_fname);
		report_t *report = report_init(report_fname);
		FILE *sex_fh = fopen(sex_fname, "w");
		if (!sex_fh)
			error("Failed to open %s: %s\n", sex_fname, strerror(errno));
		for (int i = 0; i < report->n_samples; i++) {
			char *ptr = strrchr(report->cel_files[i], '.');
			if (ptr && strcmp(ptr + 1, "CEL") == 0)
				*ptr = '\0';
			fprintf(sex_fh, "%s\t%d\n", report->cel_files[i], report->genders[i]);
		}
		fclose(sex_fh);
		report_destroy(report);
	}

	annot_t *annot = NULL;
	if (csv_fname) {
		fprintf(stderr, "Reading CSV file %s\n", csv_fname);
		if (sam_fname)
			fprintf(stderr, "Reading SAM file %s\n", sam_fname);
		annot = annot_init(csv_fname, sam_fname,
				   ((sam_fname && !ref_fname) || fasta_flank) ? output_fname
									      : NULL,
				   flags);
	}

	for (int i = 0; i < nfiles; i++) {
		hFILE *fp = hopen(filenames[i], "rb");
		if (fp == NULL)
			error("Could not open %s: %s\n", filenames[i], strerror(errno));
		if (hpeek(fp, (void *)&magic[i], 1) < 1) {
			error("Failed to read from file %s\n", filenames[i]);
		}
		switch (magic[i]) {
		case 59:
			fprintf(stderr, "Reading AGCC file %s\n", filenames[i]);
			files[i] = (void *)agcc_init(filenames[i], fp, nfiles > 1);
			break;
		case 64:
			fprintf(stderr, "Reading XDA CEL file %s\n", filenames[i]);
			files[i] = (void *)xda_cel_init(filenames[i], fp, nfiles > 1);
			break;
		case 65:
			error("Currently unable to read XDA CHP format for file %s\n",
			      filenames[i]);
		default:
			error("Expected magic numbers 59, 64 or 65 but found %d in file %s\n",
			      magic[i], filenames[i]);
		}
	}

	if (annot) {
		fai = fai_load(ref_fname);
		if (!fai)
			error("Could not load the reference %s\n", ref_fname);
		if (cache_size)
			fai_set_cache_size(fai, cache_size);
		if (models_fname)
			fprintf(stderr, "Reading SNP file %s\n", models_fname);
		models_t *models = models_fname ? models_init(models_fname) : NULL;
		fprintf(stderr, "Writing VCF file\n");
		bcf_hdr_t *hdr = hdr_init(fai, flags);
		bcf_hdr_printf(hdr, "##CSV=%s",
			       strrchr(csv_fname, '/') ? strrchr(csv_fname, '/') + 1
						       : csv_fname);
		if (sam_fname)
			bcf_hdr_printf(hdr, "##SAM=%s",
				       strrchr(sam_fname, '/') ? strrchr(sam_fname, '/') + 1
							       : sam_fname);
		if (models_fname)
			bcf_hdr_printf(hdr, "##SNP=%s",
				       strrchr(models_fname, '/')
					       ? strrchr(models_fname, '/') + 1
					       : models_fname);
		if (record_cmd_line)
			bcf_hdr_append_version(hdr, argc, argv, "bcftools_+affy2vcf");
		htsFile *out_fh = hts_open(output_fname, hts_bcf_wmode(output_type));
		if (out_fh == NULL)
			error("Can't write to \"%s\": %s\n", output_fname, strerror(errno));
		if (n_threads)
			hts_set_threads(out_fh, n_threads);
		varitr_t *varitr = NULL;
		if (nfiles > 0)
			varitr = varitr_init_cc(hdr, (agcc_t **)files, nfiles);
		else if (calls_fname || confidences_fname || summary_fname)
			varitr = varitr_init_txt(hdr, calls_fname, confidences_fname,
						 summary_fname);
		process(fai, annot, models, varitr, out_fh, hdr, flags);
		if (varitr)
			varitr_destroy(varitr);
		if (models)
			models_destroy(models);
		fai_destroy(fai);
		bcf_hdr_destroy(hdr);
		hts_close(out_fh);
		annot_destroy(annot);
	}

	if (!ref_fname && nfiles > 0) {
		FILE *out_txt = get_file_handle(output_fname);
		if (nfiles == 1) {
			switch (magic[0]) {
			case 59:
				agcc_print((agcc_t *)files[0], out_txt, flags & VERBOSE);
				break;
			case 64:
				xda_cel_print((xda_cel_t *)files[0], out_txt, flags & VERBOSE);
				break;
			default:
				error("Expected magic numbers 59 or 64 but found %d in file %s\n",
				      magic[0], filenames[0]);
			}
		} else if (flags & LOAD_CEL) {
			cels_to_tsv(magic, files, nfiles, out_txt);
		} else {
			agccs_to_tsv((agcc_t **)files, nfiles, out_txt);
		}
		if (out_txt && out_txt != stdout && out_txt != stderr)
			fclose(out_txt);
	}

	if (pathname) {
		for (int i = 0; i < nfiles; i++)
			free(filenames[i]);
		free(filenames);
	}
	for (int i = 0; i < nfiles; i++) {
		switch (magic[i]) {
		case 59:
			agcc_destroy((agcc_t *)files[i]);
			break;
		case 64:
			xda_cel_destroy((xda_cel_t *)files[i]);
			break;
		default:
			error("Expected magic numbers 59 or 64 but found %d in file %s\n",
			      magic[i], filenames[i]);
		}
	}
	free(magic);
	free(files);
	return 0;
}

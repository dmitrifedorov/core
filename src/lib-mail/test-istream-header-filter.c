/* Copyright (c) 2007-2010 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "istream.h"
#include "message-header-parser.h"
#include "istream-header-filter.h"
#include "test-common.h"

static void filter_callback(struct message_header_line *hdr,
			    bool *matched, void *context ATTR_UNUSED)
{
	if (hdr != NULL && hdr->name_offset == 0) {
		/* drop first header */
		*matched = TRUE;
	}
}

static void test_istream_filter(void)
{
	static const char *exclude_headers[] = { "To", NULL };
	const char *input = "From: foo\nFrom: abc\nTo: bar\n\nhello world\n";
	const char *output = "From: abc\n\nhello world\n";
	struct istream *istream, *filter, *filter2;
	unsigned int i, input_len = strlen(input);
	unsigned int output_len = strlen(output);
	const unsigned char *data;
	size_t size;

	test_begin("i_stream_create_header_filter()");
	istream = test_istream_create(input);
	filter = i_stream_create_header_filter(istream,
					       HEADER_FILTER_EXCLUDE |
					       HEADER_FILTER_NO_CR,
					       exclude_headers, 1,
					       filter_callback, NULL);
	filter2 = i_stream_create_header_filter(filter,
						HEADER_FILTER_EXCLUDE |
						HEADER_FILTER_NO_CR,
						exclude_headers, 1,
						null_header_filter_callback, NULL);
	i_stream_unref(&filter);
	filter = filter2;

	for (i = 1; i < input_len; i++) {
		test_istream_set_size(istream, i);
		test_assert(i_stream_read(filter) >= 0);
	}
	test_istream_set_size(istream, input_len);
	test_assert(i_stream_read(filter) > 0);
	test_assert(i_stream_read(filter) == -1);

	data = i_stream_get_data(filter, &size);
	test_assert(size == output_len && memcmp(data, output, size) == 0);

	i_stream_skip(filter, size);
	i_stream_seek(filter, 0);
	while (i_stream_read(filter) > 0) ;
	data = i_stream_get_data(filter, &size);
	test_assert(size == output_len && memcmp(data, output, size) == 0);

	i_stream_unref(&filter);
	i_stream_unref(&istream);

	test_end();
}

int main(void)
{
	static void (*test_functions[])(void) = {
		test_istream_filter,
		NULL
	};
	return test_run(test_functions);
}

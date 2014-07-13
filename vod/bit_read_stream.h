#ifndef __BIT_READ_STREAM_H__
#define __BIT_READ_STREAM_H__

// includes
#include "read_stream.h"

// typedefs
typedef struct {
	simple_read_stream_t stream;
	u_char cur_byte;
	char cur_bit;
} bit_reader_state_t;

// functions
static vod_inline void 
init_bits_reader(bit_reader_state_t* state, const u_char* buffer, int size)
{
	state->stream.cur_pos = buffer;
	state->stream.end_pos = buffer + size;
	state->stream.eof_reached = FALSE;
	state->cur_byte = 0;
	state->cur_bit = -1;
}

static vod_inline int 
get_bits(bit_reader_state_t* state, int count)
{
	int result = 0;
	
	for (; count; count--)
	{
		if (state->cur_bit < 0)
		{
			state->cur_byte = stream_get8(&state->stream);
			state->cur_bit = 7;
		}
	
		result = (result << 1) | ((state->cur_byte >> state->cur_bit) & 1);
		state->cur_bit--;
	}
	return result;
}

#endif // __BIT_READ_STREAM_H__
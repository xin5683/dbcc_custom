/**@file 2c.c
 * @brief Convert the Abstract Syntax Tree generated by mpc for the DBC file
 * into some C code which can encode/decode signals.
 * @copyright Richard James Howe (2018)
 * @license MIT
 *
 * @todo Validation functions with configurable behavior (range checks, etc)
 * @todo Tidy up this module, make things configurable
 * @todo A data driven version would be better, data should be centralized
 * and the pack/unpack functions should use data structures instead of
 * big functions with switch statements.
 * @todo Signal status; signal should be set to unknown first, or when there
 * is a timeout. A timestamp should also be processed
 * @todo MISRA C Compliance
 * @todo More intelligence code generation (use 'double' less)
 * @todo Group functionality together, eg: function declarations, variable
 * instantiations, ...
 * @todo Optional variable instantiation
 *
 * This file is quite a mess, but that is not going to change, it is also
 * quite short and seems to do the job. A better solution would be to make a
 * template tool, or a macro processor, suited for the task of generating C
 * code. The entire program really should be written in a language like Perl or
 * Python, but I wanted to use the MPC library for something, so here we are. */

#include "2c.h"
#include "util.h"
#include <assert.h>
#include <ctype.h>
#include <inttypes.h>
#include <string.h>
#include <time.h>

#define MAX_NAME_LENGTH (512u)

/* The float packing and unpacking is stolen and modified from 
 * <https://beej.us/guide/bgnet/examples/pack2b.c>! 
 * (It's public domain code as far as I know, from Beej's guide to network 
 * programming).
 *
 * The following link provides a calculator you can use to see what
 * bits correspond to a floating point number:
 * <https://www.h-schmidt.net/FloatConverter/IEEE754.html>
 *
 * Special cases:
 *
 * Zero and sign bit set -> Negative Zero
 *
 * All Exponent Bits Set
 * - Mantissa is zero and sign bit is zero ->  Infinity
 * - Mantissa is zero and sign bit is on   -> -Infinity
 * - Mantissa is non-zero -> NaN */

static char *float_pack = "\
/* pack754() -- pack a floating point number into IEEE-754 format */ \n\
static uint64_t pack754(double f, unsigned bits, unsigned expbits)\n\
{\n\
	if (f == 0.0) /* get this special case out of the way */\n\
		return signbit(f) ? (1uLL << (bits - 1)) :  0;\n\
	if (f != f) /* NaN, encoded as Exponent == all-bits-set, Mantissa != 0, Signbit == Do not care */\n\
		return (1uLL << (bits - 1)) - 1uLL;\n\
	if (f == INFINITY) /* +INFINITY encoded as Mantissa == 0, Exponent == all-bits-set */\n\
		return ((1uLL << expbits) - 1uLL) << (bits - expbits - 1);\n\
	if (f == -INFINITY) /* -INFINITY encoded as Mantissa == 0, Exponent == all-bits-set, Signbit == 1 */\n\
		return (1uLL << (bits - 1)) | ((1uLL << expbits) - 1uLL) << (bits - expbits - 1);\n\
\n\
	long long sign = 0;\n\
	double fnorm = f;\n\
	/* check sign and begin normalization */\n\
	if (f < 0) { sign = 1; fnorm = -f; }\n\
\n\
	/* get the normalized form of f and track the exponent */\n\
	int shift = 0;\n\
	while (fnorm >= 2.0) { fnorm /= 2.0; shift++; }\n\
	while (fnorm < 1.0)  { fnorm *= 2.0; shift--; }\n\
	fnorm = fnorm - 1.0;\n\
\n\
	const unsigned significandbits = bits - expbits - 1; // -1 for sign bit\n\
\n\
	/* calculate the binary form (non-float) of the significand data */\n\
	const long long significand = fnorm * (( 1LL << significandbits) + 0.5f);\n\
\n\
	/* get the biased exponent */\n\
	const long long exp = shift + ((1LL << (expbits - 1)) - 1); // shift + bias\n\
\n\
	/* return the final answer */\n\
	return (sign << (bits - 1)) | (exp << (bits - expbits - 1)) | significand;\n\
}\n\
\n\
static inline uint32_t   pack754_32(float  f)   { return   pack754(f, 32, 8); }\n\
static inline uint64_t   pack754_64(double f)   { return   pack754(f, 64, 11); }\n\
\n\n";

static char *float_unpack = "\
/* unpack754() -- unpack a floating point number from IEEE-754 format */ \n\
static double unpack754(uint64_t i, unsigned bits, unsigned expbits)\n\
{\n\
	if (i == 0) return 0.0;\n\
\n\
	const uint64_t expset = ((1uLL << expbits) - 1uLL) << (bits - expbits - 1);\n\
	if ((i & expset) == expset) { /* NaN or +/-Infinity */\n\
		if (i & ((1uLL << (bits - expbits - 1)) - 1uLL)) /* Non zero Mantissa means NaN */\n\
			return NAN;\n\
		return i & (1uLL << (bits - 1)) ? -INFINITY : INFINITY;\n\
	}\n\
\n\
	/* pull the significand */\n\
	const unsigned significandbits = bits - expbits - 1; /* - 1 for sign bit */\n\
	double result = (i & ((1LL << significandbits) - 1)); /* mask */\n\
	result /= (1LL << significandbits);  /* convert back to float */\n\
	result += 1.0f;                        /* add the one back on */\n\
\n\
	/* deal with the exponent */\n\
	const unsigned bias = (1 << (expbits - 1)) - 1;\n\
	long long shift = ((i >> significandbits) & ((1LL << expbits) - 1)) - bias;\n\
	while (shift > 0) { result *= 2.0; shift--; }\n\
	while (shift < 0) { result /= 2.0; shift++; }\n\
	\n\
	return (i >> (bits - 1)) & 1? -result: result; /* sign it, and return */\n\
}\n\
\n\
static inline float    unpack754_32(uint32_t i) { return unpack754(i, 32, 8); }\n\
static inline double   unpack754_64(uint64_t i) { return unpack754(i, 64, 11); }\n\
\n\n";



static const bool swap_motorola = true;

static unsigned fix_start_bit(bool motorola, unsigned start, unsigned siglen)
{
	if(motorola)
		start = (8 * (7 - (start / 8))) + (start % 8) - (siglen - 1);
	return start;
}

static const char *determine_unsigned_type(unsigned length)
{
	const char *type = "uint64_t";
	if(length <= 32)
		type = "uint32_t";
	if(length <= 16)
		type = "uint16_t";
	if(length <= 8)
		type = "uint8_t";
	return type;
}

static const char *determine_signed_type(unsigned length)
{
	const char *type = "int64_t";
	if(length <= 32)
		type = "int32_t";
	if(length <= 16)
		type = "int16_t";
	if(length <= 8)
		type = "int8_t";
	return type;
}

static const char *determine_type(unsigned length, bool is_signed)
{
	return is_signed ?
		determine_signed_type(length) :
		determine_unsigned_type(length);
}

static int comment(signal_t *sig, FILE *o)
{
	assert(sig);
	assert(o);
	return fprintf(o, "\t/* %s: start-bit %u, length %u, endianess %s, scaling %f, offset %f */\n",
			sig->name,
			sig->start_bit,
			sig->bit_length,
			sig->endianess == endianess_motorola_e ? "motorola" : "intel",
	      		sig->scaling,
			sig->offset);
}

static int signal2deserializer(signal_t *sig, FILE *o)
{
	assert(sig);
	assert(o);
	const bool motorola   = (sig->endianess == endianess_motorola_e);
	const unsigned start  = fix_start_bit(motorola, sig->start_bit, sig->bit_length);
	const unsigned length = sig->bit_length;
	const uint64_t mask = length == 64 ?
		0xFFFFFFFFFFFFFFFFuLL :
		(1uLL << length) - 1uLL;

	if(comment(sig, o) < 0)
		return -1;

	if(start)
		fprintf(o, "\tx = (%c >> %d) & 0x%"PRIx64";\n", motorola ? 'm' : 'i', start, mask);
	else
		fprintf(o, "\tx = %c & 0x%"PRIx64";\n", motorola ? 'm' : 'i',  mask);

	if(sig->is_floating) {
		assert(length == 32 || length == 64);
		if(fprintf(o, "\tunpack->%s = unpack754_%d(x);\n", sig->name, length) < 0)
			return -1;
		/**@todo Make type-punned version optional */
		//if(fprintf(o, "\tunpack->%s = *((%s*)&x);\n", sig->name, length == 32 ? "float" : "double") < 0)
		//	return -1;
		return 0;
	}

	if(sig->is_signed) {
		const uint64_t top = (1uL << (length - 1));
		uint64_t negative = ~mask;
		if(length <= 32)
			negative &= 0xFFFFFFFF;
		if(length <= 16)
			negative &= 0xFFFF;
		if(length <= 8)
			negative &= 0xFF;
		if(negative)
			fprintf(o, "\tx = x & 0x%"PRIx64" ? x | 0x%"PRIx64" : x; \n", top, negative);
	}

	fprintf(o, "\tunpack->%s = x;\n", sig->name);
	return 0;
}

static int signal2serializer(signal_t *sig, FILE *o)
{
	assert(sig);
	assert(o);
	bool motorola = (sig->endianess == endianess_motorola_e);
	int start = fix_start_bit(motorola, sig->start_bit, sig->bit_length);

	uint64_t mask = sig->bit_length == 64 ?
		0xFFFFFFFFFFFFFFFFuLL :
		(1uLL << sig->bit_length) - 1uLL;

	if(comment(sig, o) < 0)
		return -1;

	if (sig->is_floating) {
		assert(sig->bit_length == 32 || sig->bit_length == 64);
		/**@todo Add option for type punning version */
		fprintf(o, "\tx = pack754_%u(pack->%s) & 0x%"PRIx64";\n", sig->bit_length, sig->name, mask);
	} else {
		fprintf(o, "\tx = (*(%s*)(&pack->%s)) & 0x%"PRIx64";\n", determine_unsigned_type(sig->bit_length), sig->name, mask);
	}
	if(start)
		fprintf(o, "\tx <<= %u; \n", start);
	fprintf(o, "\t%c |= x;\n", motorola ? 'm' : 'i');
	return 0;
}

static int signal2print(signal_t *sig, unsigned id, FILE *o)
{
	/*super lazy*/

	if(sig->is_floating)
		return fprintf(o, "\tr = fprintf(data, \"%s = (wire: %%f)\\n\", (double)(print->%s));\n", sig->name, sig->name);
	return fprintf(o, "\tr = fprintf(data, \"%s = (wire: %%.0f)\\n\", (double)(print->%s));\n", sig->name, sig->name);

	/* NEVER REACHED */

	/* @todo TODO Fix this, it should print out the encoded values as well */
	fprintf(o, "\tscaled = decode_can_0x%03x_%s(print);\n", id, sig->name);
	if(sig->is_floating)
		return fprintf(o, "\tr = fprintf(data, \"%s = %%.3f (wire: %%f)\\n\", scaled, (double)(print->%s));\n",
				sig->name, sig->name);
	return fprintf(o, "\tr = fprintf(data, \"%s = %%.3f (wire: %%.0f)\\n\", scaled, (double)(print->%s));\n",
			sig->name, sig->name);
	/*fprintf(o, "\tif(r < 0)\n\t\treturn r;");*/
}

static int signal2type(signal_t *sig, FILE *o)
{
	assert(sig);
	assert(o);
	const unsigned length = sig->bit_length;
	const char *type = determine_type(length, sig->is_signed);

	if(length == 0) {
		warning("signal %s has bit length of 0 (fix the dbc file)");
		return -1;
	}

	if(sig->is_floating) {
		if(length != 32 && length != 64) {
			warning("signal %s is floating point number but has length %u (fix the dbc file)", sig->name, length);
			return -1;
		}
		type = length == 64 ? "double" : "float";
	}

	return fprintf(o, "\t%s %s; /*scaling %.1f, offset %.1f, units %s %s*/\n",
			type, sig->name, sig->scaling, sig->offset, sig->units[0] ? sig->units : "none",
			sig->is_floating ? ", floating" : "");
}

static bool signal_are_min_max_valid(signal_t *sig) 
{
	assert(sig);
	return sig->minimum != sig->maximum;
}

/**@todo more advanced type conversion could be done here */
/**@todo better range generation code, for example the range checks can be
 * eliminated for encoding values where minimum and maximum values are the
 * same as the integral type used for a given signal. Not only that, but
 * integers should be used instead of floats where possible */
static int signal2scaling_encode(const char *msgname, unsigned id, signal_t *sig, FILE *o, bool header) 
{
	assert(msgname);
	assert(sig);
	assert(o);
	const char *type = determine_type(sig->bit_length, sig->is_signed);
	if(sig->scaling != 1.0 || sig->offset != 0.0)
		type = "double";
	fprintf(o, "bool encode_can_0x%03x_%s(%s_t *record, %s in)", id, sig->name, msgname, type);
	if(header)
		return fputs(";\n", o);
	fputs("\n{\n", o);
	fprintf(o, "\trecord->%s = 0;\n", sig->name); // cast!
	if(signal_are_min_max_valid(sig)) {
		fprintf(o, "\tif((in < %f) || (in > %f))\n\t\treturn false;\n", sig->minimum, sig->maximum);
	}

	if(sig->scaling == 0.0)
		error("invalid scaling factor (fix your DBC file)");
	if(sig->offset != 0.0)
		fprintf(o, "\tin += %f;\n", -1.0 * sig->offset);
	if(sig->scaling != 1.0)
		fprintf(o, "\tin *= %f;\n", 1.0 / sig->scaling);
	fprintf(o, "\trecord->%s = in;\n", sig->name); // cast!
	return fputs("\treturn true;\n}\n\n", o);
}

static int signal2scaling_decode(const char *msgname, unsigned id, signal_t *sig, FILE *o, bool header) 
{
	assert(msgname);
	assert(sig);
	assert(o);
	const char *type = determine_type(sig->bit_length, sig->is_signed);
	if(sig->scaling != 1.0 || sig->offset != 0.0)
		type = "double";
	fprintf(o, "bool decode_can_0x%03x_%s(%s_t *record, %s *out)", id, sig->name, msgname, type);
	if(header)
		return fputs(";\n", o);
	fputs("\n{\n", o);
	fprintf(o, "\t%s rval = (%s)(record->%s);\n", type, type, sig->name);
	if(sig->scaling == 0.0)
		error("invalid scaling factor (fix your DBC file)");
	if(sig->scaling != 1.0)
		fprintf(o, "\trval *= %f;\n", sig->scaling);
	if(sig->offset != 0.0)
		fprintf(o, "\trval += %f;\n", sig->offset);
	if(signal_are_min_max_valid(sig)) {
		fprintf(o, "\tif((rval >= %f) && (rval <= %f)) {\n", sig->minimum, sig->maximum);
		fputs("\t\t*out = rval;\n", o);
		fputs("\t\treturn true;\n", o);
		fputs("\t} else {\n", o);
		fprintf(o, "\t\t*out = (%s)0;\n", type);
		fputs("\t\treturn false;\n", o);
		fputs("\t}\n", o);
	} else {
		fputs("\t*out = rval;\n", o);
		fputs("\treturn true;\n", o);
	}
	return fputs("}\n\n", o);
}

static int signal2scaling(const char *msgname, unsigned id, signal_t *sig, FILE *o, bool decode, bool header)
{
	if(decode)
		return signal2scaling_decode(msgname, id, sig, o, header);
	return signal2scaling_encode(msgname, id, sig, o, header);
}

static int print_function_name(FILE *out, const char *prefix, const char *name, const char *postfix, bool in, char *datatype, bool dlc)
{
	assert(out);
	assert(prefix); 
	assert(name); 
	assert(postfix);
	return fprintf(out, "int %s_%s(%s_t *%s, %s %sdata%s)%s",
			prefix, name, name, prefix, datatype,
			in ? "" : "*",
			dlc ? ", uint8_t dlc" : "",
			postfix);
}

static void make_name(char *newname, size_t maxlen, const char *name, unsigned id)
{
	assert(newname);
	assert(name);
	snprintf(newname, maxlen-1, "can_0x%03x_%s", id, name);
}

static signal_t *process_signals_and_find_multiplexer(can_msg_t *msg, FILE *c, const char *name, bool serialize)
{
	assert(msg);
	assert(c);
	assert(name);
	signal_t *multiplexor = NULL;

	for(size_t i = 0; i < msg->signal_count; i++) {
		signal_t *sig = msg->signals[i];
		if(sig->is_multiplexor) {
			if(multiplexor)
				error("multiple multiplexor values detected (only one per CAN msg is allowed) for %s", name);
			multiplexor = sig;
		}
		if(sig->is_multiplexed)
			continue;
		if((serialize ? signal2serializer(sig, c) : signal2deserializer(sig, c)) < 0)
			error("%s failed", serialize ? "serialization" : "deserialization");
	}
	return multiplexor;
}

static int cmp_signal(const void *lhs, const void *rhs) 
{
	assert(lhs);
	assert(rhs);
	int ret = 0;
	if((*(signal_t**)lhs)->switchval < ((*(signal_t**)rhs)->switchval))
		ret = -1;
	else if((*(signal_t**)lhs)->switchval > (*(signal_t**)rhs)->switchval)
		ret = 1;
	return ret;
}
static int multiplexor_switch(can_msg_t *msg, signal_t *multiplexor, FILE *c, bool serialize)
{
	assert(msg);
	assert(multiplexor);
	assert(c);
	fprintf(c, "\tswitch(%s->%s) {\n", serialize ? "pack" : "unpack", multiplexor->name);
	qsort(msg->signals, msg->signal_count, sizeof(*msg->signals), cmp_signal);
	for(size_t i = 0; i < msg->signal_count; i++) {
		signal_t *sig = msg->signals[i];
		if(!(sig->is_multiplexed))
			continue;
		fprintf(c, "\tcase %u:\n", sig->switchval);
		size_t j = i;
		for(; j < msg->signal_count && msg->signals[i]->switchval == msg->signals[j]->switchval; j++) {
			assert(j < msg->signal_count);
			signal_t* sig = msg->signals[j];
			if((serialize ? signal2serializer(sig, c) : signal2deserializer(sig, c)) < 0)
				return -1;
		}
		i = j - 1;
		assert(i < msg->signal_count);
		fprintf(c, "\tbreak;\n");
	}
	fprintf(c, "\tdefault:\n\t\treturn -1;\n}");
	return 0;
}

static int msg_data_type(FILE *c, const char *name)
{
	assert(c);
	assert(name);
	return fprintf(c, "%s_t %s_data;\n\n", name, name);
}

static int msg_pack(can_msg_t *msg, FILE *c, const char *name, bool motorola_used, bool intel_used)
{
	assert(msg);
	assert(c);
	assert(name);
	const bool message_has_signals = motorola_used || intel_used;
	print_function_name(c, "pack", name, "\n{\n", false, "uint64_t", false);
	if(message_has_signals)
		fprintf(c, "\tregister uint64_t x;\n");
	if(motorola_used)
		fprintf(c, "\tregister uint64_t m = 0;\n");
	if(intel_used)
		fprintf(c, "\tregister uint64_t i = 0;\n");
	if(!message_has_signals)
		fprintf(c, "\tUNUSED(data);\n\tUNUSED(pack);\n");
	signal_t *multiplexor = process_signals_and_find_multiplexer(msg, c, name, true);

	if(multiplexor)
		if(multiplexor_switch(msg, multiplexor, c, true) < 0)
			return -1;

	if(message_has_signals) {
		fprintf(c, "\t*data = %s%s%s%s%s;\n",
			swap_motorola && motorola_used ? "reverse_byte_order" : "",
			motorola_used ? "(m)" : "",
			motorola_used && intel_used ? "|" : "",
			(!swap_motorola && intel_used) ? "reverse_byte_order" : "",
			intel_used ? "(i)" : "");
	}
	fprintf(c, "\treturn 0;\n}\n\n");
	return 0;
}

static int msg_unpack(can_msg_t *msg, FILE *c, const char *name, bool motorola_used, bool intel_used)
{
	assert(msg);
	assert(c);
	assert(name);
	const bool message_has_signals = motorola_used || intel_used;
	print_function_name(c, "unpack", name, "\n{\n", true, "uint64_t", true);
	if(message_has_signals)
		fprintf(c, "\tregister uint64_t x;\n");
	if(motorola_used)
		fprintf(c, "\tregister uint64_t m = %s(data);\n", swap_motorola ? "reverse_byte_order" : "");
	if(intel_used)
		fprintf(c, "\tregister uint64_t i = %s(data);\n", swap_motorola ? "" : "reverse_byte_order");
	if(!message_has_signals)
		fprintf(c, "\tUNUSED(data);\n\tUNUSED(unpack);\n");

	/**@note This generated check might be best to be made optional, as nodes have
	 * could be sending the wrong DLC out, decoding should be attempted
	 * regardless (but an error logged, or something). */
	if(msg->dlc)
		fprintf(c, "\tif(dlc < %u)\n\t\treturn -1;\n", msg->dlc);
	else
		fprintf(c, "\tUNUSED(dlc);\n");

	signal_t *multiplexor = process_signals_and_find_multiplexer(msg, c, name, false);
	if(multiplexor)
		if(multiplexor_switch(msg, multiplexor, c, false) < 0)
			return -1;
	fprintf(c, "\treturn 0;\n}\n\n");
	return 0;
}

static int msg_print(can_msg_t *msg, FILE *c, const char *name)
{
	assert(msg);
	assert(c);
	assert(name);
	print_function_name(c, "print", name, "\n{\n", false, "FILE", false);
	if(msg->signal_count)
		fprintf(c, "\tint r = 0;\n"); //fprintf(c, "\tdouble scaled;\n\tint r = 0;\n");
	else
		fprintf(c, "\tUNUSED(data);\n\tUNUSED(print);\n");
	for(size_t i = 0; i < msg->signal_count; i++) {
		if(signal2print(msg->signals[i], msg->id, c) < 0)
			return -1;
	}
	if(msg->signal_count)
		fprintf(c, "\treturn r;\n}\n\n");
	else
		fprintf(c, "\treturn 0;\n}\n\n");
	return 0;
}

static int msg2c(can_msg_t *msg, FILE *c, bool generate_print, bool generate_pack, bool generate_unpack)
{
	assert(msg);
	assert(c);
	char name[MAX_NAME_LENGTH] = {0};
	make_name(name, MAX_NAME_LENGTH, msg->name, msg->id);
	bool motorola_used = false;
	bool intel_used = false;

	for(size_t i = 0; i < msg->signal_count; i++)
		if(msg->signals[i]->endianess == endianess_motorola_e)
			motorola_used = true;
		else
			intel_used = true;

	if(generate_pack || generate_unpack || generate_print) {
		if (msg_data_type(c, name) < 0)
			return -1;
	}

	if(generate_pack && msg_pack(msg, c, name, motorola_used, intel_used) < 0)
		return -1;

	if(generate_unpack && msg_unpack(msg, c, name, motorola_used, intel_used) < 0)
		return -1;

	for(size_t i = 0; i < msg->signal_count; i++) {
		if(generate_unpack)
			if(signal2scaling(name, msg->id, msg->signals[i], c, true, false) < 0)
				return -1;
		if(generate_pack)
			if(signal2scaling(name, msg->id, msg->signals[i], c, false, false) < 0)
				return -1;
	}

	if(generate_print && msg_print(msg, c, name) < 0)
		return -1;

	return 0;
}

static int msg2h(can_msg_t *msg, FILE *h, bool generate_print, bool generate_pack, bool generate_unpack)
{
	assert(msg);
	assert(h);
	char name[MAX_NAME_LENGTH] = {0};
	make_name(name, MAX_NAME_LENGTH, msg->name, msg->id);

	/**@todo add time stamp information to struct */
	fprintf(h, "typedef struct {\n" );

	for(size_t i = 0; i < msg->signal_count; i++)
		if(signal2type(msg->signals[i], h) < 0)
			return -1;

	fprintf(h, "} %s_t;\n\n", name);
	fprintf(h, "extern %s_t %s_data;\n", name, name);

	if (generate_pack)
		print_function_name(h, "pack", name, ";\n", false, "uint64_t", false);

	if (generate_unpack)
		print_function_name(h, "unpack", name, ";\n\n", true, "uint64_t", true);

	for(size_t i = 0; i < msg->signal_count; i++) {
		if(generate_unpack)
			if(signal2scaling(name, msg->id, msg->signals[i], h, true, true) < 0)
				return -1;
		if(generate_pack)
			if(signal2scaling(name, msg->id, msg->signals[i], h, false, true) < 0)
				return -1;
	}

	if (generate_print)
		print_function_name(h, "print", name, ";\n", false, "FILE", false);

	fputs("\n\n", h);

	return 0;
}

static const char *cfunctions =
"static inline uint64_t reverse_byte_order(uint64_t x)\n"
"{\n"
"\tx = (x & 0x00000000FFFFFFFF) << 32 | (x & 0xFFFFFFFF00000000) >> 32;\n"
"\tx = (x & 0x0000FFFF0000FFFF) << 16 | (x & 0xFFFF0000FFFF0000) >> 16;\n"
"\tx = (x & 0x00FF00FF00FF00FF) << 8  | (x & 0xFF00FF00FF00FF00) >> 8;\n"
"\treturn x;\n"
"}\n\n";

static int message_compare_function(const void *a, const void *b)
{
	assert(a);
	assert(b);
	can_msg_t *ap = *((can_msg_t**)a);
	can_msg_t *bp = *((can_msg_t**)b);
	if(ap->id <  bp->id) return -1;
	if(ap->id == bp->id) return  0;
	if(ap->id >  bp->id) return  1;
	return 0;
}

static int signal_compare_function(const void *a, const void *b)
{
	assert(a);
	assert(b);
	signal_t *ap = *((signal_t**)a);
	signal_t *bp = *((signal_t**)b);
	if(ap->bit_length <  bp->bit_length) return  1;
	if(ap->bit_length == bp->bit_length) return  0;
	if(ap->bit_length >  bp->bit_length) return -1;
	return 0;
}

static int switch_function(FILE *c, dbc_t *dbc, char *function, bool unpack, bool prototype, char *datatype, bool dlc)
{
	assert(c);
	assert(dbc);
	assert(function);
	fprintf(c, "int %s_message(unsigned id, %s %sdata%s)",
			function, datatype, unpack ? "" : "*",
			dlc ? ", uint8_t dlc" : "");
	if(prototype)
		return fprintf(c, ";\n");
	fprintf(c, "\n{\n");
	fprintf(c, "\tswitch(id) {\n");
	for(int i = 0; i < dbc->message_count; i++) {
		can_msg_t *msg = dbc->messages[i];
		char name[MAX_NAME_LENGTH] = {0};
		make_name(name, MAX_NAME_LENGTH, msg->name, msg->id);
		fprintf(c, "\tcase 0x%03x: return %s_%s(&%s_data, data%s);\n",
				msg->id,
				function,
				name,
				name,
				dlc ? ", dlc" : "");
	}
	fprintf(c, "\tdefault: break; \n\t}\n");
	return fprintf(c, "\treturn -1; \n}\n\n");
}

int dbc2c(dbc_t *dbc, FILE *c, FILE *h, const char *name, bool use_time_stamps,
		  bool generate_print, bool generate_pack, bool generate_unpack)
{
	/**@todo print out ECU node information */
	assert(dbc);
	assert(c);
	assert(h);
	assert(name);
	time_t rawtime;
	struct tm * timeinfo;
	time(&rawtime);
	timeinfo = localtime(&rawtime);
	char *file_guard = duplicate(name);
	size_t file_guard_len = strlen(file_guard);

	/* make file guard all upper case alphanumeric only, first character
	 * alpha only*/
	if(!isalpha(file_guard[0]))
		file_guard[0] = '_';
	for(size_t i = 0; i < file_guard_len; i++)
		file_guard[i] = (isalnum(file_guard[i])) ?
			toupper(file_guard[i]) : '_';

	/* sort signals by id */
	qsort(dbc->messages, dbc->message_count, sizeof(dbc->messages[0]), message_compare_function);

	/* sort by size for better struct packing */
	for(int i = 0; i < dbc->message_count; i++) {
		can_msg_t *msg = dbc->messages[i];
		qsort(msg->signals, msg->signal_count, sizeof(msg->signals[0]), signal_compare_function);
	}

	/* header file (begin) */
	fprintf(h, "/** @brief CAN message encoder/decoder: automatically generated - do not edit\n");
	if(use_time_stamps)
		fprintf(h, "  * @note  Generated on %s", asctime(timeinfo));
	fprintf(h,
		"  * @note  Generated by dbcc: See https://github.com/howerj/dbcc\n"
		"  */\n\n"
		"#ifndef %s\n"
		"#define %s\n\n"
		"#include <stdint.h>\n"
		"#include <stdbool.h>\n"
		"#include <stdio.h>\n\n"
		"#ifdef __cplusplus\n"
		"extern \"C\" { \n"
		"#endif\n\n",
		file_guard, 
		/*generate_print ? "#include <stdio.h>" : "", */
		file_guard);

	if (generate_unpack)
		switch_function(h, dbc, "unpack", true, true, "uint64_t", true);

	if (generate_pack)
		switch_function(h, dbc, "pack", false, true, "uint64_t", false);

	if (generate_print)
		switch_function(h, dbc, "print", true, true, "FILE*", false);

	fputs("\n", h);

	for(int i = 0; i < dbc->message_count; i++)
		if(msg2h(dbc->messages[i], h,  generate_print,  generate_pack,  generate_unpack) < 0)
			return -1;
	fputs(
		"#ifdef __cplusplus\n"
		"} \n"
		"#endif\n\n"
		"#endif\n",
		h);
	/* header file (end) */

	/* C FILE */
	fprintf(c, "#include \"%s\"\n", name);
	fprintf(c, "#include <inttypes.h>\n");
	if (dbc->use_float)
		fprintf(c, "#include <math.h> /* uses macros NAN, INFINITY, signbit, no need for -lm */\n");
	fputc('\n', c);
	fprintf(c, "#define UNUSED(X) ((void)(X))\n\n");
	fputs(cfunctions, c);

	if (generate_unpack) {
		if (dbc->use_float)
			fputs(float_unpack, c);
		switch_function(c, dbc, "unpack", true, false, "uint64_t", true);
	}

	if (generate_pack) {
		if (dbc->use_float)
			fputs(float_pack, c);
		switch_function(c, dbc, "pack", false, false, "uint64_t", false);
	}

	if (generate_print)
		switch_function(c, dbc, "print", true, false, "FILE*", false);

	for(int i = 0; i < dbc->message_count; i++)
		if(msg2c(dbc->messages[i], c, generate_print, generate_pack, generate_unpack) < 0)
			return -1;

	free(file_guard);
	return 0;
}


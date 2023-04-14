/* Copyright (C) 1996-2018 Free Software Foundation, Inc.

   This file is part of GCC.

   GCC is free software; you can redistribute it and/or modify it under
   the terms of the GNU General Public License as published by the Free
   Software Foundation; either version 3, or (at your option) any later
   version.

   GCC is distributed in the hope that it will be useful, but WITHOUT ANY
   WARRANTY; without even the implied warranty of MERCHANTABILITY or
   FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
   for more details.

   Under Section 7 of GPL version 3, you are granted additional
   permissions described in the GCC Runtime Library Exception, version
   3.1, as published by the Free Software Foundation.

   You should have received a copy of the GNU General Public License and
   a copy of the GCC Runtime Library Exception along with this program;
   see the files COPYING3 and COPYING.RUNTIME respectively.  If not, see
   <http://www.gnu.org/licenses/>.  */

/**
 * This file has excerpts from gcc libgcc/libgcov.h and gcc/gcov-io.h.
 * Which is governed by section 7 of additional permissions.
 */


#ifndef _COVERAGE_H_
#define _COVERAGE_H_

#if (__GNUC__ >= 10)
#define GCOV_COUNTERS 8U
#elif (__GNUC__ >= 8)
#define GCOV_COUNTERS 9U
#else
#define GCOV_COUNTERS 10U
#endif

typedef uint64_t gcov_type;

#define GCOV_TAG_FUNCTION_LENGTH  3
#define GCOV_DATA_MAGIC   (0x67636461)
#define GCOV_TAG_FUNCTION (0x01000000)
#define GCOV_TAG_COUNTER_BASE (0x01a10000)
#define GCOV_TAG_FOR_COUNTER(count)         \
(GCOV_TAG_COUNTER_BASE + ((uint32_t) (count) << 17))

#define FILE_START_INDICATOR	'*'
#define GCOV_DUMP_SEPARATOR	'<'

/**Information about counters for a single function
 *
 * This data is generated by gcc during compilation and doesn't change
 *  at run-time with the exception of the values array.
 */
struct gcov_ctr_info {
	unsigned int num;    /* number of counter values for this type */
	gcov_type *values;   /* array of counter values for this type */
};

/**
 * Profiling meta data per function
 *
 * This data is generated by gcc during compilation and doesn't change
 * at run-time.
 *
 * Information about a single function.  This uses the trailing array
 * idiom. The number of counters is determined from the merge pointer
 * array in gcov_info.  The key is used to detect which of a set of
 * comdat functions was selected -- it points to the gcov_info object
 * of the object file containing the selected comdat function.
 */
struct gcov_fn_info {
	const struct gcov_info *key;     /* comdat key */
	unsigned int ident;              /* unique ident of function */
	unsigned int lineno_checksum;    /* function lineno_checksum */
	unsigned int cfg_checksum;       /* function cfg checksum */
	struct gcov_ctr_info ctrs[0];    /* instrumented counters */
};

/** Profiling data per object file
 *
 * This data is generated by gcc during compilation and doesn't change
 * at run-time with the exception of the next pointer.
 */
struct gcov_info {
	unsigned int version;        /* Gcov version (same as GCC version) */
	struct gcov_info *next;      /* List head for a singly-linked list */
	unsigned int stamp;          /* Uniquifying time stamp */
	const char *filename;        /* Name of the associated gcda data file */
	/* merge functions, null for unused*/
	void (*merge[GCOV_COUNTERS])(gcov_type *, unsigned int);
	unsigned int n_functions;         /* number of instrumented functions */
	struct gcov_fn_info **functions;  /* function information */

};

#endif /* _COVERAGE_H_ */

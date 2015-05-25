/*
 * Copyright (C) 2008-2014  bind-dyndb-ldap authors; see COPYING for license
 */

#ifndef _LD_STR_H_
#define _LD_STR_H_

#include "util.h"
#include <isc/formatcheck.h>
#include <isc/mem.h>

#define LD_MAX_SPLITS	256

#if ISC_MEM_TRACKLINES
#define _STR_MEM_FILELINE	, __FILE__, __LINE__
#define _STR_MEM_FLARG		, const char *file, int line
#define _STR_MEM_FLARG_PASS	, file, line
#else
#define _STR_MEM_FILELINE
#define _STR_MEM_FLAG
#define _STR_MEM_FLARG_PASS
#endif

typedef struct ld_string	ld_string_t;

/*
 * Public functions.
 */

#define str_new(m, s)	str__new((m), (s) _STR_MEM_FILELINE)
#define str_destroy(s)	str__destroy((s) _STR_MEM_FILELINE)

size_t str_len(const ld_string_t *str) ATTR_NONNULLS ATTR_CHECKRESULT;
const char * str_buf(const ld_string_t *src) ATTR_NONNULLS ATTR_CHECKRESULT;
void str_clear(ld_string_t *dest) ATTR_NONNULLS;
isc_result_t str_init_char(ld_string_t *dest, const char *src) ATTR_NONNULLS ATTR_CHECKRESULT;
isc_result_t str_cat_char(ld_string_t *dest, const char *src) ATTR_NONNULLS ATTR_CHECKRESULT;
isc_result_t str_cat_char_len(ld_string_t *dest, const char *src, size_t len) ATTR_NONNULLS ATTR_CHECKRESULT;
isc_result_t str_sprintf(ld_string_t *dest, const char *format, ...) ISC_FORMAT_PRINTF(2, 3) ATTR_NONNULLS ATTR_CHECKRESULT;
isc_result_t str_vsprintf(ld_string_t *dest, const char *format, va_list ap) ATTR_NONNULLS ATTR_CHECKRESULT;

/* These are pseudo-private functions and shouldn't be called directly. */
isc_result_t str__new(isc_mem_t *mctx, ld_string_t **new_str _STR_MEM_FLARG) ATTR_NONNULLS ATTR_CHECKRESULT;
void str__destroy(ld_string_t **str _STR_MEM_FLARG) ATTR_NONNULLS;

#endif /* !_LD_STR_H_ */

/*-
 * Public Domain 2014-2016 MongoDB, Inc.
 * Public Domain 2008-2014 WiredTiger, Inc.
 *
 * This is free and unencumbered software released into the public domain.
 *
 * Anyone is free to copy, modify, publish, use, compile, sell, or
 * distribute this software, either in source code form or as a compiled
 * binary, for any purpose, commercial or non-commercial, and by any
 * means.
 *
 * In jurisdictions that recognize copyright laws, the author or authors
 * of this software dedicate any and all copyright interest in the
 * software to the public domain. We make this dedication for the benefit
 * of the public at large and to the detriment of our heirs and
 * successors. We intend this dedication to be an overt act of
 * relinquishment in perpetuity of all present and future rights to this
 * software under copyright law.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include <zstd.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <wiredtiger_config.h>
#include <wiredtiger.h>
#include <wiredtiger_ext.h>

/* Local compressor structure. */
typedef struct {
	WT_COMPRESSOR compressor;		/* Must come first */

	WT_EXTENSION_API *wt_api;		/* Extension API */

	int	 compression_level;		/* compression level */
} ZSTD_COMPRESSOR;

#ifdef WORDS_BIGENDIAN
/*
 * zstd_bswap64 --
 *	64-bit unsigned little-endian to/from big-endian value.
 */
static inline uint64_t
zstd_bswap64(uint64_t v)
{
	return (
	    ((v << 56) & 0xff00000000000000UL) |
	    ((v << 40) & 0x00ff000000000000UL) |
	    ((v << 24) & 0x0000ff0000000000UL) |
	    ((v <<  8) & 0x000000ff00000000UL) |
	    ((v >>  8) & 0x00000000ff000000UL) |
	    ((v >> 24) & 0x0000000000ff0000UL) |
	    ((v >> 40) & 0x000000000000ff00UL) |
	    ((v >> 56) & 0x00000000000000ffUL)
	);
}
#endif

/*
 * zstd_error --
 *	Output an error message, and return a standard error code.
 */
static int
zstd_error(WT_COMPRESSOR *compressor,
    WT_SESSION *session, const char *call, size_t error)
{
	WT_EXTENSION_API *wt_api;

	wt_api = ((ZSTD_COMPRESSOR *)compressor)->wt_api;

	(void)wt_api->err_printf(wt_api, session,
	    "zstd error: %s: %s", call, ZSTD_getErrorName(error));
	return (WT_ERROR);
}

/*
 *  zstd_compress --
 *	WiredTiger Zstd compression.
 */
static int
zstd_compress(WT_COMPRESSOR *compressor, WT_SESSION *session,
    uint8_t *src, size_t src_len,
    uint8_t *dst, size_t dst_len,
    size_t *result_lenp, int *compression_failed)
{
	ZSTD_COMPRESSOR *zcompressor;
	size_t zstd_ret;

	zcompressor = (ZSTD_COMPRESSOR *)compressor;

	/* Compress, starting past the prefix bytes. */
	zstd_ret = ZSTD_compress(
	    dst + sizeof(size_t), dst_len - sizeof(size_t),
	    src, src_len, zcompressor->compression_level);

	/*
	 * If compression succeeded and the compressed length is smaller than
	 * the original size, return success.
	 */
	if (!ZSTD_isError(zstd_ret) && zstd_ret + sizeof(size_t) < src_len) {
		*result_lenp = zstd_ret + sizeof(size_t);
		*compression_failed = 0;

		/*
		 * On decompression, Zstd requires an exact compressed byte
		 * count (the current value of zstd_ret). WiredTiger does not
		 * preserve that value, so save zstd_ret at the beginning of
		 * the destination buffer.
		 *
		 * Store the value in little-endian format.
		 */
#ifdef WORDS_BIGENDIAN
		zstd_ret = zstd_bswap64(zstd_ret);
#endif
		*(size_t *)dst = zstd_ret;
		return (0);
	}

	*compression_failed = 1;
	return (ZSTD_isError(zstd_ret) ?
	    zstd_error(compressor, session, "ZSTD_compress", zstd_ret) : 0);
}

/*
 * zstd_decompress --
 *	WiredTiger Zstd decompression.
 */
static int
zstd_decompress(WT_COMPRESSOR *compressor, WT_SESSION *session,
    uint8_t *src, size_t src_len,
    uint8_t *dst, size_t dst_len,
    size_t *result_lenp)
{
	size_t zstd_ret;

	/*
	 * Retrieve the saved length, handling little- to big-endian conversion
	 * as necessary.
	 */
	src_len = *(size_t *)src;
#ifdef WORDS_BIGENDIAN
	src_len = zstd_bswap64(src_len);
#endif

	zstd_ret = ZSTD_decompress(dst, dst_len, src + sizeof(size_t), src_len);

	if (!ZSTD_isError(zstd_ret)) {
		*result_lenp = zstd_ret;
		return (0);
	}
	return (zstd_error(compressor, session, "ZSTD_decompress", zstd_ret));
}

/*
 * zstd_pre_size --
 *	WiredTiger Zstd destination buffer sizing for compression.
 */
static int
zstd_pre_size(WT_COMPRESSOR *compressor, WT_SESSION *session,
    uint8_t *src, size_t src_len, size_t *result_lenp)
{
	(void)compressor;				/* Unused parameters */
	(void)session;
	(void)src;

	/*
	 * Zstd compression runs faster if the destination buffer is sized at
	 * the upper-bound of the buffer size needed by the compression. Use
	 * the library calculation of that overhead (plus our overhead).
	 */
	*result_lenp = ZSTD_compressBound(src_len) + sizeof(size_t);
	return (0);
}

/*
 * zstd_terminate --
 *	WiredTiger Zstd compression termination.
 */
static int
zstd_terminate(WT_COMPRESSOR *compressor, WT_SESSION *session)
{
	(void)session;					/* Unused parameters */

	free(compressor);
	return (0);
}

int zstd_extension_init(WT_CONNECTION *, WT_CONFIG_ARG *);

/*
 * zstd_extension_init --
 *	WiredTiger Zstd compression extension - called directly when Zstd
 * support is built in, or via wiredtiger_extension_init when Zstd support
 * is included via extension loading.
 */
int
zstd_extension_init(WT_CONNECTION *connection, WT_CONFIG_ARG *config)
{
	WT_CONFIG_ITEM k, v;
	WT_CONFIG_PARSER *config_parser;
	WT_EXTENSION_API *wtext;
	ZSTD_COMPRESSOR *zstd_compressor;
	int compression_level, ret;

	/*
	 * Zstd's sweet-spot is better compression than zlib at significantly
	 * faster compression/decompression speeds. LZ4 and snappy are faster
	 * than zstd, but have worse compression ratios. Applications wanting
	 * faster compression/decompression with worse compression will select
	 * LZ4 or snappy, so we configure zstd for better compression.
	 *
	 * From the zstd github site, default measurements of the compression
	 * engines we support, listing compression ratios with compression and
	 * decompression speeds:
	 *
	 *	Name	Ratio	C.speed	D.speed
	 *			MB/s	MB/s
	 *	zstd	2.877	330	940
	 *	zlib	2.730	95	360
	 *	LZ4	2.101	620	3100
	 *	snappy	2.091	480	1600
	 *
	 * Set the zstd compression level to 3: according to the zstd web site,
	 * that reduces zstd's compression speed to around 200 MB/s, increasing
	 * the compression ratio to 3.100 (close to zlib's best compression
	 * ratio). In other words, position zstd as a zlib replacement, having
	 * similar compression at much higher compression/decompression speeds.
	 */
	compression_level = 3;

	/*
	 * Zstd compression engine allows applications to specify a compression
	 * level; review the configuration.
	 */
	wtext = connection->get_extension_api(connection);
	if ((ret = wtext->config_get(wtext, NULL, config, "config", &v)) != 0) {
		(void)wtext->err_printf(wtext, NULL,
		    "WT_EXTENSION_API.config_get: zstd configure: %s",
		    wtext->strerror(wtext, NULL, ret));
		return (ret);
	}
	if ((ret = wtext->config_parser_open(
	    wtext, NULL, v.str, v.len, &config_parser)) != 0) {
		(void)wtext->err_printf(wtext, NULL,
		    "WT_EXTENSION_API.config_parser_open: zstd configure: %s",
		    wtext->strerror(wtext, NULL, ret));
		return (ret);
	}
	while ((ret = config_parser->next(config_parser, &k, &v)) == 0)
		if (strlen("compression_level") == k.len &&
		    strncmp("compression_level", k.str, k.len) == 0) {
			compression_level = (int)v.val;
			continue;
		}
	if (ret != WT_NOTFOUND) {
		(void)wtext->err_printf(wtext, NULL,
		    "WT_CONFIG_PARSER.next: zstd configure: %s",
		    wtext->strerror(wtext, NULL, ret));
		return (ret);
	}
	if ((ret = config_parser->close(config_parser)) != 0) {
		(void)wtext->err_printf(wtext, NULL,
		    "WT_CONFIG_PARSER.close: zstd configure: %s",
		    wtext->strerror(wtext, NULL, ret));
		return (ret);
	}

	if ((zstd_compressor = calloc(1, sizeof(ZSTD_COMPRESSOR))) == NULL)
		return (errno);

	zstd_compressor->compressor.compress = zstd_compress;
	zstd_compressor->compressor.compress_raw = NULL;
	zstd_compressor->compressor.decompress = zstd_decompress;
	zstd_compressor->compressor.pre_size = zstd_pre_size;
	zstd_compressor->compressor.terminate = zstd_terminate;

	zstd_compressor->wt_api = connection->get_extension_api(connection);

	zstd_compressor->compression_level = compression_level;

	/* Load the compressor */
	return (connection->add_compressor(
	    connection, "zstd", (WT_COMPRESSOR *)zstd_compressor, NULL));
}

/*
 * We have to remove this symbol when building as a builtin extension otherwise
 * it will conflict with other builtin libraries.
 */
#ifndef	HAVE_BUILTIN_EXTENSION_ZSTD
/*
 * wiredtiger_extension_init --
 *	WiredTiger Zstd compression extension.
 */
int
wiredtiger_extension_init(WT_CONNECTION *connection, WT_CONFIG_ARG *config)
{
	return (zstd_extension_init(connection, config));
}
#endif

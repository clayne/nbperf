/*	$NetBSD: nbperf-bdz.c,v 1.10 2021/01/07 16:03:08 joerg Exp $	*/
/*-
 * Copyright (c) 2009, 2012 The NetBSD Foundation, Inc.
 * Copyright (c) 2022 Reini Urban
 * All rights reserved.
 *
 * This code is derived from software contributed to The NetBSD Foundation
 * by Joerg Sonnenberger.
 * Integer keys and more hashes were added by Reini Urban.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE
 * COPYRIGHT HOLDERS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#if HAVE_NBTOOL_CONFIG_H
#include "nbtool_config.h"
#endif

//#include <sys/cdefs.h>
//__RCSID("$NetBSD: nbperf-bdz.c,v 1.10 2021/01/07 16:03:08 joerg Exp $")

#include <err.h>
#include <inttypes.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "nbperf.h"

/*
 * A full description of the algorithm can be found in:
 * "Simple and Space-Efficient Minimal Perfect Hash Functions"
 * by Botelho, Pagh and Ziviani, proceeedings of WADS 2007.
 */

/*
 * The algorithm is based on random, acyclic 3-graphs.
 *
 * Each edge in the represents a key.  The vertices are the reminder of
 * the hash function mod n.  n = cm with c > 1.23.  This ensures that
 * an acyclic graph can be found with a very high probality.
 *
 * An acyclic graph has an edge order, where at least one vertex of
 * each edge hasn't been seen before.   It is declares the first unvisited
 * vertex as authoritive for the edge and assigns a 2bit value to unvisited
 * vertices, so that the sum of all vertices of the edge modulo 4 is
 * the index of the authoritive vertex.
 */

#define GRAPH_SIZE 3
#include "graph2.h"

struct state {
	struct SIZED(graph) graph;
	uint32_t *visited;
	uint32_t *holes64k;
	uint16_t *holes64;
	uint8_t *g;
	uint32_t *result_map;
};

static void
assign_nodes(struct state *state)
{
	struct SIZED(edge) *e;
	size_t i, j;
	uint32_t t, r, holes;

	for (i = 0; i < state->graph.v; ++i)
		state->g[i] = 3;

	for (i = 0; i < state->graph.e; ++i) {
		j = state->graph.output_order[i];
		e = &state->graph.edges[j];
		if (!state->visited[e->vertices[0]]) {
			r = 0;
			t = e->vertices[0];
		} else if (!state->visited[e->vertices[1]]) {
			r = 1;
			t = e->vertices[1];
		} else {
			if (state->visited[e->vertices[2]])
				abort();
			r = 2;
			t = e->vertices[2];
		}

		state->visited[t] = 2 + j;
		if (state->visited[e->vertices[0]] == 0)
			state->visited[e->vertices[0]] = 1;
		if (state->visited[e->vertices[1]] == 0)
			state->visited[e->vertices[1]] = 1;
		if (state->visited[e->vertices[2]] == 0)
			state->visited[e->vertices[2]] = 1;

		state->g[t] = (9 + r - state->g[e->vertices[0]] - state->g[e->vertices[1]]
		    - state->g[e->vertices[2]]) % 3;
	}

	holes = 0;
	for (i = 0; i < state->graph.v; ++i) {
		if (i % 65536 == 0)
			state->holes64k[i >> 16] = holes;

		if (i % 64 == 0)
			state->holes64[i >> 6] = holes - state->holes64k[i >> 16];

		if (state->visited[i] > 1) {
			j = state->visited[i] - 2;
			state->result_map[j] = i - holes;
		}

		if (state->g[i] == 3)
			++holes;
	}
}

static void
print_hash(struct nbperf *nbperf, struct state *state)
{
	uint64_t sum;
	size_t i;

	print_coda(nbperf);
	fprintf(nbperf->output, "#include <strings.h>\n");
        fprintf(nbperf->output, "#ifdef __GNUC__\n"); // since gcc 4.5
        fprintf(nbperf->output, "#define popcount64 __builtin_popcountll\n");
        fprintf(nbperf->output, "#endif\n\n");

	if (nbperf->intkeys) {
                inthash4_addprint(nbperf);
	}

	fprintf(nbperf->output, "%suint32_t\n",
	    nbperf->static_hash ? "static " : "");
	if (!nbperf->intkeys)
		fprintf(nbperf->output,
		    "%s(const void * __restrict key, size_t keylen)\n",
		    nbperf->hash_name);
	else
		fprintf(nbperf->output,	"%s(const int32_t key)\n", nbperf->hash_name);
	fprintf(nbperf->output, "{\n");

	fprintf(nbperf->output,
	    "\tstatic const uint64_t g1[%" PRId32 "] = {\n",
	    (state->graph.v + 63) / 64);
	sum = 0;
	for (i = 0; i < state->graph.v; ++i) {
		sum |= ((uint64_t)state->g[i] & 1) << (i & 63);
		if (i % 64 == 63) {
			fprintf(nbperf->output, "%sUINT64_C(0x%016" PRIx64 "),%s",
			    (i / 64 % 2 == 0 ? "\t    " : " "),
			    sum,
			    (i / 64 % 2 == 1 ? "\n" : ""));
			sum = 0;
		}
	}
	if (i % 64 != 0) {
		fprintf(nbperf->output, "%sUINT64_C(0x%016" PRIx64 "),%s",
		    (i / 64 % 2 == 0 ? "\t    " : " "),
		    sum,
		    (i / 64 % 2 == 1 ? "\n" : ""));
	}
	fprintf(nbperf->output, "%s\t};\n", (i % 2 ? "\n" : ""));

	fprintf(nbperf->output,
	    "\tstatic const uint64_t g2[%" PRId32 "] = {\n",
	    (state->graph.v + 63) / 64);
	sum = 0;
	for (i = 0; i < state->graph.v; ++i) {
		sum |= (((uint64_t)state->g[i] & 2) >> 1) << (i & 63);
		if (i % 64 == 63) {
			fprintf(nbperf->output, "%sUINT64_C(0x%016" PRIx64 "),%s",
			    (i / 64 % 2 == 0 ? "\t    " : " "),
			    sum,
			    (i / 64 % 2 == 1 ? "\n" : ""));
			sum = 0;
		}
	}
	if (i % 64 != 0) {
		fprintf(nbperf->output, "%sUINT64_C(0x%016" PRIx64 "),%s",
		    (i / 64 % 2 == 0 ? "\t    " : " "),
		    sum,
		    (i / 64 % 2 == 1 ? "\n" : ""));
	}
	fprintf(nbperf->output, "%s\t};\n", (i % 2 ? "\n" : ""));

	fprintf(nbperf->output,
	    "\tstatic const uint32_t holes64k[%" PRId32 "] = {\n",
	    (state->graph.v + 65535) / 65536);
	for (i = 0; i < state->graph.v; i += 65536)
		fprintf(nbperf->output, "%sUINT32_C(0x%08" PRIx32 "),%s",
		    (i / 65536 % 4 == 0 ? "\t    " : " "),
		    state->holes64k[i >> 16],
		    (i / 65536 % 4 == 3 ? "\n" : ""));
	fprintf(nbperf->output, "%s\t};\n", (i / 65536 % 4 ? "\n" : ""));

	fprintf(nbperf->output,
	    "\tstatic const uint16_t holes64[%" PRId32 "] = {\n",
	    (state->graph.v + 63) / 64);
	for (i = 0; i < state->graph.v; i += 64)
		fprintf(nbperf->output, "%sUINT32_C(0x%04" PRIx32 "),%s",
		    (i / 64 % 4 == 0 ? "\t    " : " "),
		    state->holes64[i >> 6],
		    (i / 64 % 4 == 3 ? "\n" : ""));
	fprintf(nbperf->output, "%s\t};\n", (i / 64 % 4 ? "\n" : "")); 

	fprintf(nbperf->output, "\tuint32_t idx, idx2;\n");
        if (nbperf->hashes16)
                fprintf(nbperf->output, "\tuint16_t h[%u];\n\n", nbperf->hash_size * 2);
        else
                fprintf(nbperf->output, "\tuint32_t h[%u];\n\n", nbperf->hash_size);

	(*nbperf->print_hash)(nbperf, "\t", "key", "keylen", "h");

	fprintf(nbperf->output, "\n\th[0] = h[0] %% UINT32_C(%" PRIu32 ");\n",
	    state->graph.v);
	fprintf(nbperf->output, "\th[1] = h[1] %% UINT32_C(%" PRIu32 ");\n",
	    state->graph.v);
        if (nbperf->hash_size > 2)
                fprintf(nbperf->output, "\th[2] = h[2] %% UINT32_C(%" PRIu32 ");\n",
                        state->graph.v);

	if (state->graph.hash_fudge & 1)
		fprintf(nbperf->output, "\th[1] ^= (h[0] == h[1]);\n");

	if (state->graph.hash_fudge & 2 && nbperf->hash_size > 2) {
		fprintf(nbperf->output,
		    "\th[2] ^= (h[0] == h[2] || h[1] == h[2]);\n");
		fprintf(nbperf->output,
		    "\th[2] ^= 2 * (h[0] == h[2] || h[1] == h[2]);\n");
	}

        if (nbperf->hash_size > 2) {
                fprintf(nbperf->output,
                        "\tidx = 9 + ((g1[h[0] >> 6] >> (h[0] & 63)) & 1)\n"
                        "\t        + ((g1[h[1] >> 6] >> (h[1] & 63)) & 1)\n"
                        "\t        + ((g1[h[2] >> 6] >> (h[2] & 63)) & 1)\n"
                        "\t        - ((g2[h[0] >> 6] >> (h[0] & 63)) & 1)\n"
                        "\t        - ((g2[h[1] >> 6] >> (h[1] & 63)) & 1)\n"
                        "\t        - ((g2[h[2] >> 6] >> (h[2] & 63)) & 1);\n"
                        );
                fprintf(nbperf->output,
                        "\tidx = h[idx %% 3];\n");
        }
        else {
                fprintf(nbperf->output,
                        "\tidx = 9 + ((g1[h[0] >> 6] >> (h[0] & 63)) & 1)\n"
                        "\t        + ((g1[h[1] >> 6] >> (h[1] & 63)) & 1)\n"
                        "\t        - ((g2[h[0] >> 6] >> (h[0] & 63)) & 1)\n"
                        "\t        - ((g2[h[1] >> 6] >> (h[1] & 63)) & 1);\n"
                        );
                fprintf(nbperf->output,
                        "\tidx = h[idx %% 2];\n");
        }

	fprintf(nbperf->output,
	    "\tidx2 = idx - holes64[idx >> 6] - holes64k[idx >> 16];\n"
	    "\tidx2 -= popcount64(  g1[idx >> 6]\n"
            "\t                   & g2[idx >> 6]\n"
	    "\t                   & (((uint64_t)1 << (idx & 63)) - 1));\n"
	    "\treturn idx2;\n");

	fprintf(nbperf->output, "}\n");

	if (nbperf->map_output != NULL) {
		for (i = 0; i < state->graph.e; ++i)
			fprintf(nbperf->map_output, "%" PRIu32 "\n",
			    state->result_map[i]);
	}
}

int
bpz_compute(struct nbperf *nbperf)
{
	struct state state;
	int retval = -1;
	uint32_t v, e;

	if (nbperf->c == 0)
		nbperf->c = 1.24;
	if (nbperf->c < 1.24)
		errx(1, "The argument for option -c must be at least 1.24");
	if (nbperf->hash_size < 3)
                errx(1, "The hash function must generate at least 3 values");

	(*nbperf->seed_hash)(nbperf);
	e = nbperf->n;
	v = nbperf->c * nbperf->n;
	if (1.24 * nbperf->n > v)
		++v;
	if (v < 10)
		v = 10;
	if (nbperf->allow_hash_fudging)
		v |= 3;

	graph3_setup(&state.graph, v, e);

	state.holes64k = calloc(sizeof(uint32_t), (v + 65535) / 65536);
	state.holes64 = calloc(sizeof(uint16_t), (v + 63) / 64 );
	state.g = calloc(sizeof(uint32_t), v | 63);
	state.visited = calloc(sizeof(uint32_t), v);
	state.result_map = calloc(sizeof(uint32_t), e);

	if (state.holes64k == NULL || state.holes64 == NULL ||
	    state.g == NULL || state.visited == NULL ||
	    state.result_map == NULL)
		err(1, "malloc failed");

	if (SIZED2(_hash)(nbperf, &state.graph))
		goto failed;
	if (SIZED2(_output_order)(&state.graph))
		goto failed;
	assign_nodes(&state);
	print_hash(nbperf, &state);

	retval = 0;

failed:
	SIZED2(_free)(&state.graph);
	free(state.visited);
	free(state.g);
	free(state.holes64k);
	free(state.holes64);
	free(state.result_map);
	return retval;
}

/* $OpenBSD: res_random.c,v 1.4 1997/04/23 22:37:50 provos Exp $ */

/*
 * Copyright 1997 Niels Provos <provos@physnet.uni-hamburg.de>
 * All rights reserved.
 *
 * Theo de Raadt <deraadt@openbsd.org> came up with the idea of using
 * such a mathematical system to generate more random (yet non-repeating)
 * ids to solve the resolver/named problem.  But Niels designed the
 * actual system based on the constraints.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *      This product includes software developed by Niels Provos.
 * 4. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/* 
 * seed = random 15bit
 * n = prime, g0 = generator to n,
 * j = random so that gcd(j,n-1) == 1
 * g = g0^j mod n will be a generator again.
 *
 * X[0] = random seed.
 * X[n] = a*X[n-1]+b mod m is a Linear Congruential Generator
 * with a = 7^(even random) mod m, 
 *      b = random with gcd(b,m) == 1
 *      m = 31104 and a maximal period of m-1.
 *
 * The transaction id is determined by:
 * id[n] = seed xor (g^X[n] mod n)
 *
 * Effectivly the id is restricted to the lower 15 bits, thus
 * yielding two different cycles by toggling the msb on and off.
 * This avoids reuse issues caused by reseeding.
 *
 * The 16 bit space is very small and brute force attempts are
 * entirly feasible, we skip a random number of transaction ids
 * so that an attacker will not get sequential ids.
 */

#include <sys/types.h>
#include <netinet/in.h>
#include <sys/time.h>
#include <resolv.h>

#include <unistd.h>
#include <stdlib.h>
#include <string.h>

#define RU_OUT  180             /* Time after wich will be reseeded */
#define RU_MAX	30000		/* Uniq cycle, avoid blackjack prediction */
#define RU_GEN	2		/* Starting generator */
#define RU_N	32749		/* RU_N-1 = 2*2*3*2729 */
#define RU_AGEN	7               /* determine ru_a as RU_AGEN^(2*rand) */
#define RU_M	31104           /* RU_M = 2^7*3^5 - don't change */

#define PFAC_N 3
const static u_int16_t pfacts[PFAC_N] = {
	2, 
	3,
	2729
};

static u_int16_t ru_x;
static u_int16_t ru_seed;
static u_int16_t ru_a, ru_b;
static u_int16_t ru_g;
static u_int16_t ru_counter = 0;
static u_int16_t ru_msb = 0;
static long ru_reseed;
static u_int32_t tmp;                /* Storage for unused random */
static struct timeval tv;

static u_int16_t pmod __P((u_int16_t, u_int16_t, u_int16_t));
static void res_initid __P((void));

/*
 * Do a fast modular exponation, returned value will be in the range
 * of 0 - (mod-1)
 */

static u_int16_t
pmod(gen, exp, mod)
	u_int16_t gen, exp, mod;
{
	u_int16_t s, t, u;

	s = 1;
	t = gen;
	u = exp;

	while (u) {
		if (u & 1)
			s = (s*t) % mod;
		u >>= 1;
		t = (t*t) % mod;
	}
	return (s);
}

/* 
 * Initalizes the seed and chooses a suitable generator. Also toggles 
 * the msb flag. The msb flag is used to generate two distinct
 * cycles of random numbers and thus avoiding reuse of ids.
 *
 * This function is called from res_randomid() when needed, an 
 * application does not have to worry about it.
 */
static void 
res_initid()
{
	u_int16_t j, i;
	int noprime = 1;

	tmp = arc4random();
	ru_x = (tmp & 0xFFFF) % RU_M;

	/* 15 bits of random seed */
	ru_seed = (tmp >> 16) & 0x7FFF;

	tmp = arc4random();

	/* Determine the LCG we use */
	ru_b = (tmp & 0xfffe) | 1;
	ru_a = pmod(RU_AGEN, (tmp >> 16) & 0xfffe, RU_M);
	while (ru_b % 3 == 0)
	  ru_b += 2;
	
	tmp = arc4random();
	j = tmp % RU_N;
	tmp = tmp >> 16;

	/* 
	 * Do a fast gcd(j,RU_N-1), so we can find a j with
	 * gcd(j, RU_N-1) == 1, giving a new generator for
	 * RU_GEN^j mod RU_N
	 */

	while (noprime) {
		for (i=0; i<PFAC_N; i++)
			if (j%pfacts[i] == 0)
				break;

		if (i>=PFAC_N)
			noprime = 0;
		else 
			j = (j+1) % RU_N;
	}

	ru_g = pmod(RU_GEN,j,RU_N);
	ru_counter = 0;

	gettimeofday(&tv, NULL);
	ru_reseed = tv.tv_sec + RU_OUT;
	ru_msb = ru_msb == 0x8000 ? 0 : 0x8000; 
}

u_int
res_randomid()
{
        int i, n;

	gettimeofday(&tv, NULL);
	if (ru_counter >= RU_MAX || tv.tv_sec > ru_reseed)
		res_initid();

	if (!tmp)
	        tmp = arc4random();

	/* Skip a random number of ids */
	n = tmp & 0x2f; tmp = tmp >> 6;
	if (ru_counter + n >= RU_MAX)
                res_initid();

	for (i=0; i<=n; i++)
	        /* Linear Congruential Generator */
	        ru_x = (ru_a*ru_x + ru_b) % RU_M;

	ru_counter += i;

	return (ru_seed ^ pmod(ru_g,ru_x,RU_N)) | ru_msb;
}

#if 0
void
main(int argc, char **argv)
{
	int i, n;
	u_int16_t wert;

	res_initid();

	printf("Generator: %d\n", ru_g);
	printf("Seed: %d\n", ru_seed);
	printf("Reseed at %ld\n", ru_reseed);
	printf("Ru_X: %d\n", ru_x);
	printf("Ru_A: %d\n", ru_a);
	printf("Ru_B: %d\n", ru_b);

	n = atoi(argv[1]);
	for (i=0;i<n;i++) {
		wert = res_randomid();
		printf("%06d\n", wert);
	}
}
#endif


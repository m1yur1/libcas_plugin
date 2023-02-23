//-D USE_FP16

#define SRGB_SOURCE 1
#define A_GPU 1
#define A_HLSL 1
#define CAS_SLOW 1
#define CAS_GO_SLOWER 1
#define CAS_BETTER_DIAGONALS 1

#if USE_FP16
#define A_HALF 1
#define CAS_PACKED_ONLY 1
#endif

cbuffer Arguments: register(b0)
{
	uint4 const0;
	uint4 const1;
};
Texture2D InputTexture: register(t0);
RWTexture2D<float4> OutputTexture: register(u0);


#include "ffx_a.h"


#if USE_FP16
AH3 CasLoadH(ASW2 p)
{
	return InputTexture.Load(ASU3(p, 0)).rgb;
}

void CasInputH(inout AH2 r, inout AH2 g, inout AH2 b)
{
#if SRGB_SOURCE
	r = AFromSrgbH2(r);
	g = AFromSrgbH2(g);
	b = AFromSrgbH2(b);
#endif
}
#else
AF3 CasLoad(ASU2 p)
{
	return InputTexture.Load(ASU3(p, 0)).rgb;
}

void CasInput(inout AF1 r, inout AF1 g, inout AF1 b)
{
#if SRGB_SOURCE
	r = AFromSrgbF1(r);
	g = AFromSrgbF1(g);
	b = AFromSrgbF1(b);
#endif
}
#endif


#include "ffx_cas.h"


[numthreads(64, 1, 1)]
void main
(
	uint3 LocalThreadId: SV_GroupThreadID,
	uint3 WorkGroupId: SV_GroupID
)
{
	AU2 gxy = ARmp8x8(LocalThreadId.x) + AU2(WorkGroupId.x << 4u, WorkGroupId.y << 4u);
#if USE_FP16
	AH4 c0, c1;
	AH2 cR, cG, cB;

	CasFilterH(cR, cG, cB, gxy, const0, const1, true);
#if SRGB_SOURCE
	cR = AToSrgbH2(cR);
	cG = AToSrgbH2(cG);
	cB = AToSrgbH2(cB);
#endif
	CasDepack(c0, c1, cR, cG, cB);
	OutputTexture[ASU2(gxy)] = AF4(c0);
	OutputTexture[ASU2(gxy) + ASU2(8, 0)] = AF4(c1);
	gxy.y += 8u;

	CasFilterH(cR, cG, cB, gxy, const0, const1, true);
#if SRGB_SOURCE
	cR = AToSrgbH2(cR);
	cG = AToSrgbH2(cG);
	cB = AToSrgbH2(cB);
#endif
	CasDepack(c0, c1, cR, cG, cB);
	OutputTexture[ASU2(gxy)] = AF4(c0);
	OutputTexture[ASU2(gxy) + ASU2(8, 0)] = AF4(c1);
#else
	AF3 c;

	CasFilter(c.r, c.g, c.b, gxy, const0, const1, true);
#if SRGB_SOURCE
	c.r = AToSrgbF1(c.r);
	c.g = AToSrgbF1(c.g);
	c.b = AToSrgbF1(c.b);
#endif
	OutputTexture[ASU2(gxy)] = AF4(c, 1);
	gxy.x += 8u;

	CasFilter(c.r, c.g, c.b, gxy, const0, const1, true);
#if SRGB_SOURCE
	c.r = AToSrgbF1(c.r);
	c.g = AToSrgbF1(c.g);
	c.b = AToSrgbF1(c.b);
#endif
	OutputTexture[ASU2(gxy)] = AF4(c, 1);
	gxy.y += 8u;

	CasFilter(c.r, c.g, c.b, gxy, const0, const1, true);
#if SRGB_SOURCE
	c.r = AToSrgbF1(c.r);
	c.g = AToSrgbF1(c.g);
	c.b = AToSrgbF1(c.b);
#endif
	OutputTexture[ASU2(gxy)] = AF4(c, 1);
	gxy.x -= 8u;

	CasFilter(c.r, c.g, c.b, gxy, const0, const1, true);
#if SRGB_SOURCE
	c.r = AToSrgbF1(c.r);
	c.g = AToSrgbF1(c.g);
	c.b = AToSrgbF1(c.b);
#endif
	OutputTexture[ASU2(gxy)] = AF4(c, 1);
#endif
}

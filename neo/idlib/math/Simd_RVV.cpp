/*
===========================================================================

Doom 3 GPL Source Code
Copyright (C) 1999-2011 id Software LLC

===========================================================================
*/

#include "sys/platform.h"
#include "idlib/math/Simd_RVV.h"
#include "idlib/math/Vector.h"
#include "idlib/math/Plane.h"
#include "idlib/math/Matrix.h"
#include "idlib/geometry/DrawVert.h"
#include "idlib/geometry/JointTransform.h"
#include "renderer/Model.h"

#if defined(__riscv_vector)
#include <stdint.h>
#include <riscv_vector.h>
#endif

const char * VPCALL idSIMD_RVV::GetName( void ) const {
#if defined(__riscv_vector)
	return "RVV";
#else
	return "RISC-V generic";
#endif
}

#if defined(__riscv_vector)

static ID_INLINE const float *RVV_Vec2Ptr( const idVec2 *v, int component ) {
	return v[0].ToFloatPtr() + component;
}

static ID_INLINE const float *RVV_Vec3Ptr( const idVec3 *v, int component ) {
	return v[0].ToFloatPtr() + component;
}

static ID_INLINE const float *RVV_PlanePtr( const idPlane *p, int component ) {
	return p[0].ToFloatPtr() + component;
}

static ID_INLINE const float *RVV_DrawVertXYZPtr( const idDrawVert *v, int component ) {
	return v[0].xyz.ToFloatPtr() + component;
}

static ID_INLINE void RVV_MinMaxStrided( float &min, float &max, const float *base, ptrdiff_t stride, const int count ) {
	min = __builtin_inff();
	max = -__builtin_inff();
	int i = 0;
	while ( i < count ) {
		size_t vl = __riscv_vsetvl_e32m8( count - i );
		vfloat32m8_t v = __riscv_vlse32_v_f32m8( base + i * ( stride / (ptrdiff_t)sizeof( float ) ), stride, vl );
		vfloat32m1_t vmin = __riscv_vfmv_s_f_f32m1( min, vl );
		vfloat32m1_t vmax = __riscv_vfmv_s_f_f32m1( max, vl );
		vmin = __riscv_vfredmin_vs_f32m8_f32m1( v, vmin, vl );
		vmax = __riscv_vfredmax_vs_f32m8_f32m1( v, vmax, vl );
		min = __riscv_vfmv_f_s_f32m1_f32( vmin );
		max = __riscv_vfmv_f_s_f32m1_f32( vmax );
		i += vl;
	}
}

void VPCALL idSIMD_RVV::Add( float *dst, const float constant, const float *src, const int count ) {
	int i = 0;
	while ( i < count ) {
		size_t vl = __riscv_vsetvl_e32m8( count - i );
		vfloat32m8_t v = __riscv_vle32_v_f32m8( src + i, vl );
		v = __riscv_vfadd_vf_f32m8( v, constant, vl );
		__riscv_vse32_v_f32m8( dst + i, v, vl );
		i += vl;
	}
}

void VPCALL idSIMD_RVV::Add( float *dst, const float *src0, const float *src1, const int count ) {
	int i = 0;
	while ( i < count ) {
		size_t vl = __riscv_vsetvl_e32m8( count - i );
		vfloat32m8_t v0 = __riscv_vle32_v_f32m8( src0 + i, vl );
		vfloat32m8_t v1 = __riscv_vle32_v_f32m8( src1 + i, vl );
		__riscv_vse32_v_f32m8( dst + i, __riscv_vfadd_vv_f32m8( v0, v1, vl ), vl );
		i += vl;
	}
}

void VPCALL idSIMD_RVV::Sub( float *dst, const float constant, const float *src, const int count ) {
	int i = 0;
	while ( i < count ) {
		size_t vl = __riscv_vsetvl_e32m8( count - i );
		vfloat32m8_t v = __riscv_vle32_v_f32m8( src + i, vl );
		v = __riscv_vfrsub_vf_f32m8( v, constant, vl );
		__riscv_vse32_v_f32m8( dst + i, v, vl );
		i += vl;
	}
}

void VPCALL idSIMD_RVV::Sub( float *dst, const float *src0, const float *src1, const int count ) {
	int i = 0;
	while ( i < count ) {
		size_t vl = __riscv_vsetvl_e32m8( count - i );
		vfloat32m8_t v0 = __riscv_vle32_v_f32m8( src0 + i, vl );
		vfloat32m8_t v1 = __riscv_vle32_v_f32m8( src1 + i, vl );
		__riscv_vse32_v_f32m8( dst + i, __riscv_vfsub_vv_f32m8( v0, v1, vl ), vl );
		i += vl;
	}
}

void VPCALL idSIMD_RVV::Mul( float *dst, const float constant, const float *src, const int count ) {
	int i = 0;
	while ( i < count ) {
		size_t vl = __riscv_vsetvl_e32m8( count - i );
		vfloat32m8_t v = __riscv_vle32_v_f32m8( src + i, vl );
		v = __riscv_vfmul_vf_f32m8( v, constant, vl );
		__riscv_vse32_v_f32m8( dst + i, v, vl );
		i += vl;
	}
}

void VPCALL idSIMD_RVV::Mul( float *dst, const float *src0, const float *src1, const int count ) {
	int i = 0;
	while ( i < count ) {
		size_t vl = __riscv_vsetvl_e32m8( count - i );
		vfloat32m8_t v0 = __riscv_vle32_v_f32m8( src0 + i, vl );
		vfloat32m8_t v1 = __riscv_vle32_v_f32m8( src1 + i, vl );
		__riscv_vse32_v_f32m8( dst + i, __riscv_vfmul_vv_f32m8( v0, v1, vl ), vl );
		i += vl;
	}
}

void VPCALL idSIMD_RVV::MulAdd( float *dst, const float constant, const float *src, const int count ) {
	int i = 0;
	while ( i < count ) {
		size_t vl = __riscv_vsetvl_e32m8( count - i );
		vfloat32m8_t d = __riscv_vle32_v_f32m8( dst + i, vl );
		vfloat32m8_t s = __riscv_vle32_v_f32m8( src + i, vl );
		d = __riscv_vfmacc_vf_f32m8( d, constant, s, vl );
		__riscv_vse32_v_f32m8( dst + i, d, vl );
		i += vl;
	}
}

void VPCALL idSIMD_RVV::MulAdd( float *dst, const float *src0, const float *src1, const int count ) {
	int i = 0;
	while ( i < count ) {
		size_t vl = __riscv_vsetvl_e32m8( count - i );
		vfloat32m8_t d = __riscv_vle32_v_f32m8( dst + i, vl );
		vfloat32m8_t v0 = __riscv_vle32_v_f32m8( src0 + i, vl );
		vfloat32m8_t v1 = __riscv_vle32_v_f32m8( src1 + i, vl );
		d = __riscv_vfmacc_vv_f32m8( d, v0, v1, vl );
		__riscv_vse32_v_f32m8( dst + i, d, vl );
		i += vl;
	}
}

void VPCALL idSIMD_RVV::MulSub( float *dst, const float constant, const float *src, const int count ) {
	int i = 0;
	while ( i < count ) {
		size_t vl = __riscv_vsetvl_e32m8( count - i );
		vfloat32m8_t d = __riscv_vle32_v_f32m8( dst + i, vl );
		vfloat32m8_t s = __riscv_vle32_v_f32m8( src + i, vl );
		vfloat32m8_t p = __riscv_vfmul_vf_f32m8( s, constant, vl );
		__riscv_vse32_v_f32m8( dst + i, __riscv_vfsub_vv_f32m8( d, p, vl ), vl );
		i += vl;
	}
}

void VPCALL idSIMD_RVV::MulSub( float *dst, const float *src0, const float *src1, const int count ) {
	int i = 0;
	while ( i < count ) {
		size_t vl = __riscv_vsetvl_e32m8( count - i );
		vfloat32m8_t d = __riscv_vle32_v_f32m8( dst + i, vl );
		vfloat32m8_t v0 = __riscv_vle32_v_f32m8( src0 + i, vl );
		vfloat32m8_t v1 = __riscv_vle32_v_f32m8( src1 + i, vl );
		vfloat32m8_t p = __riscv_vfmul_vv_f32m8( v0, v1, vl );
		__riscv_vse32_v_f32m8( dst + i, __riscv_vfsub_vv_f32m8( d, p, vl ), vl );
		i += vl;
	}
}

void VPCALL idSIMD_RVV::Div( float *dst, const float constant, const float *divisor, const int count ) {
	int i = 0;
	while ( i < count ) {
		size_t vl = __riscv_vsetvl_e32m8( count - i );
		vfloat32m8_t d = __riscv_vle32_v_f32m8( divisor + i, vl );
		__riscv_vse32_v_f32m8( dst + i, __riscv_vfrdiv_vf_f32m8( d, constant, vl ), vl );
		i += vl;
	}
}

void VPCALL idSIMD_RVV::Div( float *dst, const float *src0, const float *src1, const int count ) {
	int i = 0;
	while ( i < count ) {
		size_t vl = __riscv_vsetvl_e32m8( count - i );
		vfloat32m8_t v0 = __riscv_vle32_v_f32m8( src0 + i, vl );
		vfloat32m8_t v1 = __riscv_vle32_v_f32m8( src1 + i, vl );
		__riscv_vse32_v_f32m8( dst + i, __riscv_vfdiv_vv_f32m8( v0, v1, vl ), vl );
		i += vl;
	}
}

void VPCALL idSIMD_RVV::Dot( float &dot, const float *src1, const float *src2, const int count ) {
	float sum = 0.0f;
	int i = 0;
	while ( i < count ) {
		size_t vl = __riscv_vsetvl_e32m8( count - i );
		vfloat32m8_t v1 = __riscv_vle32_v_f32m8( src1 + i, vl );
		vfloat32m8_t v2 = __riscv_vle32_v_f32m8( src2 + i, vl );
		vfloat32m8_t prod = __riscv_vfmul_vv_f32m8( v1, v2, vl );
		vfloat32m1_t acc = __riscv_vfmv_s_f_f32m1( 0.0f, vl );
		acc = __riscv_vfredusum_vs_f32m8_f32m1( prod, acc, vl );
		sum += __riscv_vfmv_f_s_f32m1_f32( acc );
		i += vl;
	}
	dot = sum;
}

void VPCALL idSIMD_RVV::Dot( float *dst, const idVec3 &constant, const idVec3 *src, const int count ) {
	const float *c = constant.ToFloatPtr();
	const ptrdiff_t stride = (ptrdiff_t)sizeof( idVec3 );
	int i = 0;
	while ( i < count ) {
		size_t vl = __riscv_vsetvl_e32m8( count - i );
		const idVec3 *s = src + i;
		vfloat32m8_t x = __riscv_vlse32_v_f32m8( RVV_Vec3Ptr( s, 0 ), stride, vl );
		vfloat32m8_t y = __riscv_vlse32_v_f32m8( RVV_Vec3Ptr( s, 1 ), stride, vl );
		vfloat32m8_t z = __riscv_vlse32_v_f32m8( RVV_Vec3Ptr( s, 2 ), stride, vl );
		vfloat32m8_t r = __riscv_vfmul_vf_f32m8( x, c[0], vl );
		r = __riscv_vfmacc_vf_f32m8( r, c[1], y, vl );
		r = __riscv_vfmacc_vf_f32m8( r, c[2], z, vl );
		__riscv_vse32_v_f32m8( dst + i, r, vl );
		i += vl;
	}
}

void VPCALL idSIMD_RVV::Dot( float *dst, const idPlane &constant, const idVec3 *src, const int count ) {
	const float *c = constant.ToFloatPtr();
	const ptrdiff_t stride = (ptrdiff_t)sizeof( idVec3 );
	int i = 0;
	while ( i < count ) {
		size_t vl = __riscv_vsetvl_e32m8( count - i );
		const idVec3 *s = src + i;
		vfloat32m8_t x = __riscv_vlse32_v_f32m8( RVV_Vec3Ptr( s, 0 ), stride, vl );
		vfloat32m8_t y = __riscv_vlse32_v_f32m8( RVV_Vec3Ptr( s, 1 ), stride, vl );
		vfloat32m8_t z = __riscv_vlse32_v_f32m8( RVV_Vec3Ptr( s, 2 ), stride, vl );
		vfloat32m8_t r = __riscv_vfmul_vf_f32m8( x, c[0], vl );
		r = __riscv_vfmacc_vf_f32m8( r, c[1], y, vl );
		r = __riscv_vfmacc_vf_f32m8( r, c[2], z, vl );
		r = __riscv_vfadd_vf_f32m8( r, c[3], vl );
		__riscv_vse32_v_f32m8( dst + i, r, vl );
		i += vl;
	}
}

void VPCALL idSIMD_RVV::Dot( float *dst, const idPlane &constant, const idPlane *src, const int count ) {
	const float *c = constant.ToFloatPtr();
	const ptrdiff_t stride = (ptrdiff_t)sizeof( idPlane );
	int i = 0;
	while ( i < count ) {
		size_t vl = __riscv_vsetvl_e32m8( count - i );
		const idPlane *s = src + i;
		vfloat32m8_t x = __riscv_vlse32_v_f32m8( RVV_PlanePtr( s, 0 ), stride, vl );
		vfloat32m8_t y = __riscv_vlse32_v_f32m8( RVV_PlanePtr( s, 1 ), stride, vl );
		vfloat32m8_t z = __riscv_vlse32_v_f32m8( RVV_PlanePtr( s, 2 ), stride, vl );
		vfloat32m8_t w = __riscv_vlse32_v_f32m8( RVV_PlanePtr( s, 3 ), stride, vl );
		vfloat32m8_t r = __riscv_vfmul_vf_f32m8( x, c[0], vl );
		r = __riscv_vfmacc_vf_f32m8( r, c[1], y, vl );
		r = __riscv_vfmacc_vf_f32m8( r, c[2], z, vl );
		r = __riscv_vfmacc_vf_f32m8( r, c[3], w, vl );
		__riscv_vse32_v_f32m8( dst + i, r, vl );
		i += vl;
	}
}

void VPCALL idSIMD_RVV::Dot( float *dst, const idPlane &constant, const idDrawVert *src, const int count ) {
	const float *c = constant.ToFloatPtr();
	const ptrdiff_t stride = (ptrdiff_t)sizeof( idDrawVert );
	int i = 0;
	while ( i < count ) {
		size_t vl = __riscv_vsetvl_e32m8( count - i );
		const idDrawVert *s = src + i;
		vfloat32m8_t x = __riscv_vlse32_v_f32m8( RVV_DrawVertXYZPtr( s, 0 ), stride, vl );
		vfloat32m8_t y = __riscv_vlse32_v_f32m8( RVV_DrawVertXYZPtr( s, 1 ), stride, vl );
		vfloat32m8_t z = __riscv_vlse32_v_f32m8( RVV_DrawVertXYZPtr( s, 2 ), stride, vl );
		vfloat32m8_t r = __riscv_vfmul_vf_f32m8( x, c[0], vl );
		r = __riscv_vfmacc_vf_f32m8( r, c[1], y, vl );
		r = __riscv_vfmacc_vf_f32m8( r, c[2], z, vl );
		r = __riscv_vfadd_vf_f32m8( r, c[3], vl );
		__riscv_vse32_v_f32m8( dst + i, r, vl );
		i += vl;
	}
}

void VPCALL idSIMD_RVV::Dot( float *dst, const idVec3 &constant, const idPlane *src, const int count ) {
	const float *c = constant.ToFloatPtr();
	const ptrdiff_t stride = (ptrdiff_t)sizeof( idPlane );
	int i = 0;
	while ( i < count ) {
		size_t vl = __riscv_vsetvl_e32m8( count - i );
		const idPlane *s = src + i;
		vfloat32m8_t x = __riscv_vlse32_v_f32m8( RVV_PlanePtr( s, 0 ), stride, vl );
		vfloat32m8_t y = __riscv_vlse32_v_f32m8( RVV_PlanePtr( s, 1 ), stride, vl );
		vfloat32m8_t z = __riscv_vlse32_v_f32m8( RVV_PlanePtr( s, 2 ), stride, vl );
		vfloat32m8_t w = __riscv_vlse32_v_f32m8( RVV_PlanePtr( s, 3 ), stride, vl );
		vfloat32m8_t r = __riscv_vfmul_vf_f32m8( x, c[0], vl );
		r = __riscv_vfmacc_vf_f32m8( r, c[1], y, vl );
		r = __riscv_vfmacc_vf_f32m8( r, c[2], z, vl );
		r = __riscv_vfadd_vv_f32m8( r, w, vl );
		__riscv_vse32_v_f32m8( dst + i, r, vl );
		i += vl;
	}
}

void VPCALL idSIMD_RVV::Dot( float *dst, const idVec3 &constant, const idDrawVert *src, const int count ) {
	const float *c = constant.ToFloatPtr();
	const ptrdiff_t stride = (ptrdiff_t)sizeof( idDrawVert );
	int i = 0;
	while ( i < count ) {
		size_t vl = __riscv_vsetvl_e32m8( count - i );
		const idDrawVert *s = src + i;
		vfloat32m8_t x = __riscv_vlse32_v_f32m8( RVV_DrawVertXYZPtr( s, 0 ), stride, vl );
		vfloat32m8_t y = __riscv_vlse32_v_f32m8( RVV_DrawVertXYZPtr( s, 1 ), stride, vl );
		vfloat32m8_t z = __riscv_vlse32_v_f32m8( RVV_DrawVertXYZPtr( s, 2 ), stride, vl );
		vfloat32m8_t r = __riscv_vfmul_vf_f32m8( x, c[0], vl );
		r = __riscv_vfmacc_vf_f32m8( r, c[1], y, vl );
		r = __riscv_vfmacc_vf_f32m8( r, c[2], z, vl );
		__riscv_vse32_v_f32m8( dst + i, r, vl );
		i += vl;
	}
}

void VPCALL idSIMD_RVV::Dot( float *dst, const idVec3 *src0, const idVec3 *src1, const int count ) {
	const ptrdiff_t stride = (ptrdiff_t)sizeof( idVec3 );
	int i = 0;
	while ( i < count ) {
		size_t vl = __riscv_vsetvl_e32m8( count - i );
		const idVec3 *s0 = src0 + i;
		const idVec3 *s1 = src1 + i;
		vfloat32m8_t x0 = __riscv_vlse32_v_f32m8( RVV_Vec3Ptr( s0, 0 ), stride, vl );
		vfloat32m8_t y0 = __riscv_vlse32_v_f32m8( RVV_Vec3Ptr( s0, 1 ), stride, vl );
		vfloat32m8_t z0 = __riscv_vlse32_v_f32m8( RVV_Vec3Ptr( s0, 2 ), stride, vl );
		vfloat32m8_t x1 = __riscv_vlse32_v_f32m8( RVV_Vec3Ptr( s1, 0 ), stride, vl );
		vfloat32m8_t y1 = __riscv_vlse32_v_f32m8( RVV_Vec3Ptr( s1, 1 ), stride, vl );
		vfloat32m8_t z1 = __riscv_vlse32_v_f32m8( RVV_Vec3Ptr( s1, 2 ), stride, vl );
		vfloat32m8_t r = __riscv_vfmul_vv_f32m8( x0, x1, vl );
		r = __riscv_vfmacc_vv_f32m8( r, y0, y1, vl );
		r = __riscv_vfmacc_vv_f32m8( r, z0, z1, vl );
		__riscv_vse32_v_f32m8( dst + i, r, vl );
		i += vl;
	}
}

void VPCALL idSIMD_RVV::MinMax( float &min, float &max, const float *src, const int count ) {
	min = __builtin_inff();
	max = -__builtin_inff();
	int i = 0;
	while ( i < count ) {
		size_t vl = __riscv_vsetvl_e32m8( count - i );
		vfloat32m8_t v = __riscv_vle32_v_f32m8( src + i, vl );
		vfloat32m1_t vmin = __riscv_vfmv_s_f_f32m1( min, vl );
		vfloat32m1_t vmax = __riscv_vfmv_s_f_f32m1( max, vl );
		vmin = __riscv_vfredmin_vs_f32m8_f32m1( v, vmin, vl );
		vmax = __riscv_vfredmax_vs_f32m8_f32m1( v, vmax, vl );
		min = __riscv_vfmv_f_s_f32m1_f32( vmin );
		max = __riscv_vfmv_f_s_f32m1_f32( vmax );
		i += vl;
	}
}

void VPCALL idSIMD_RVV::MinMax( idVec2 &min, idVec2 &max, const idVec2 *src, const int count ) {
	const ptrdiff_t stride = (ptrdiff_t)sizeof( idVec2 );
	RVV_MinMaxStrided( min[0], max[0], RVV_Vec2Ptr( src, 0 ), stride, count );
	RVV_MinMaxStrided( min[1], max[1], RVV_Vec2Ptr( src, 1 ), stride, count );
}

void VPCALL idSIMD_RVV::MinMax( idVec3 &min, idVec3 &max, const idVec3 *src, const int count ) {
	const ptrdiff_t stride = (ptrdiff_t)sizeof( idVec3 );
	RVV_MinMaxStrided( min[0], max[0], RVV_Vec3Ptr( src, 0 ), stride, count );
	RVV_MinMaxStrided( min[1], max[1], RVV_Vec3Ptr( src, 1 ), stride, count );
	RVV_MinMaxStrided( min[2], max[2], RVV_Vec3Ptr( src, 2 ), stride, count );
}

void VPCALL idSIMD_RVV::MinMax( idVec3 &min, idVec3 &max, const idDrawVert *src, const int count ) {
	const ptrdiff_t stride = (ptrdiff_t)sizeof( idDrawVert );
	RVV_MinMaxStrided( min[0], max[0], RVV_DrawVertXYZPtr( src, 0 ), stride, count );
	RVV_MinMaxStrided( min[1], max[1], RVV_DrawVertXYZPtr( src, 1 ), stride, count );
	RVV_MinMaxStrided( min[2], max[2], RVV_DrawVertXYZPtr( src, 2 ), stride, count );
}

void VPCALL idSIMD_RVV::MinMax( idVec3 &min, idVec3 &max, const idDrawVert *src, const int *indexes, const int count ) {
	min[0] = min[1] = min[2] = __builtin_inff();
	max[0] = max[1] = max[2] = -__builtin_inff();
	const ptrdiff_t stride = (ptrdiff_t)sizeof( idDrawVert );
	int i = 0;
	while ( i < count ) {
		size_t vl = __riscv_vsetvl_e32m8( count - i );
		vuint32m8_t idx = __riscv_vle32_v_u32m8( (const uint32_t *)( indexes + i ), vl );
		vuint32m8_t byte_off = __riscv_vmul_vx_u32m8( idx, stride, vl );
		vfloat32m8_t x = __riscv_vluxei32_v_f32m8( (const float *)src, byte_off, vl );
		vuint32m8_t off_y = __riscv_vadd_vx_u32m8( byte_off, sizeof( float ), vl );
		vfloat32m8_t y = __riscv_vluxei32_v_f32m8( (const float *)src, off_y, vl );
		vuint32m8_t off_z = __riscv_vadd_vx_u32m8( byte_off, sizeof( float ) * 2, vl );
		vfloat32m8_t z = __riscv_vluxei32_v_f32m8( (const float *)src, off_z, vl );
		vfloat32m1_t vmin_x = __riscv_vfmv_s_f_f32m1( min[0], vl );
		vfloat32m1_t vmax_x = __riscv_vfmv_s_f_f32m1( max[0], vl );
		vfloat32m1_t vmin_y = __riscv_vfmv_s_f_f32m1( min[1], vl );
		vfloat32m1_t vmax_y = __riscv_vfmv_s_f_f32m1( max[1], vl );
		vfloat32m1_t vmin_z = __riscv_vfmv_s_f_f32m1( min[2], vl );
		vfloat32m1_t vmax_z = __riscv_vfmv_s_f_f32m1( max[2], vl );
		vmin_x = __riscv_vfredmin_vs_f32m8_f32m1( x, vmin_x, vl );
		vmax_x = __riscv_vfredmax_vs_f32m8_f32m1( x, vmax_x, vl );
		vmin_y = __riscv_vfredmin_vs_f32m8_f32m1( y, vmin_y, vl );
		vmax_y = __riscv_vfredmax_vs_f32m8_f32m1( y, vmax_y, vl );
		vmin_z = __riscv_vfredmin_vs_f32m8_f32m1( z, vmin_z, vl );
		vmax_z = __riscv_vfredmax_vs_f32m8_f32m1( z, vmax_z, vl );
		min[0] = __riscv_vfmv_f_s_f32m1_f32( vmin_x );
		max[0] = __riscv_vfmv_f_s_f32m1_f32( vmax_x );
		min[1] = __riscv_vfmv_f_s_f32m1_f32( vmin_y );
		max[1] = __riscv_vfmv_f_s_f32m1_f32( vmax_y );
		min[2] = __riscv_vfmv_f_s_f32m1_f32( vmin_z );
		max[2] = __riscv_vfmv_f_s_f32m1_f32( vmax_z );
		i += vl;
	}
}

void VPCALL idSIMD_RVV::Clamp( float *dst, const float *src, const float min, const float max, const int count ) {
	int i = 0;
	while ( i < count ) {
		size_t vl = __riscv_vsetvl_e32m8( count - i );
		vfloat32m8_t v = __riscv_vle32_v_f32m8( src + i, vl );
		v = __riscv_vfmax_vf_f32m8( v, min, vl );
		v = __riscv_vfmin_vf_f32m8( v, max, vl );
		__riscv_vse32_v_f32m8( dst + i, v, vl );
		i += vl;
	}
}

void VPCALL idSIMD_RVV::ClampMin( float *dst, const float *src, const float min, const int count ) {
	int i = 0;
	while ( i < count ) {
		size_t vl = __riscv_vsetvl_e32m8( count - i );
		vfloat32m8_t v = __riscv_vle32_v_f32m8( src + i, vl );
		v = __riscv_vfmax_vf_f32m8( v, min, vl );
		__riscv_vse32_v_f32m8( dst + i, v, vl );
		i += vl;
	}
}

void VPCALL idSIMD_RVV::ClampMax( float *dst, const float *src, const float max, const int count ) {
	int i = 0;
	while ( i < count ) {
		size_t vl = __riscv_vsetvl_e32m8( count - i );
		vfloat32m8_t v = __riscv_vle32_v_f32m8( src + i, vl );
		v = __riscv_vfmin_vf_f32m8( v, max, vl );
		__riscv_vse32_v_f32m8( dst + i, v, vl );
		i += vl;
	}
}

void VPCALL idSIMD_RVV::CmpGT( byte *dst, const float *src0, const float constant, const int count ) {
	int i = 0;
	while ( i < count ) {
		size_t vl = __riscv_vsetvl_e32m4( count - i );
		vfloat32m4_t v = __riscv_vle32_v_f32m4( src0 + i, vl );
		vbool8_t mask = __riscv_vmfgt_vf_f32m4_b8( v, constant, vl );
		vuint8m1_t ones = __riscv_vmv_v_x_u8m1( 1, vl );
		vuint8m1_t zeros = __riscv_vmv_v_x_u8m1( 0, vl );
		__riscv_vse8_v_u8m1( dst + i, __riscv_vmerge_vvm_u8m1( zeros, ones, mask, vl ), vl );
		i += vl;
	}
}

void VPCALL idSIMD_RVV::CmpGT( byte *dst, const byte bitNum, const float *src0, const float constant, const int count ) {
	int i = 0;
	while ( i < count ) {
		size_t vl = __riscv_vsetvl_e32m4( count - i );
		vfloat32m4_t v = __riscv_vle32_v_f32m4( src0 + i, vl );
		vbool8_t mask = __riscv_vmfgt_vf_f32m4_b8( v, constant, vl );
		vuint8m1_t existing = __riscv_vle8_v_u8m1( dst + i, vl );
		vuint8m1_t bit = __riscv_vmv_v_x_u8m1( 1 << bitNum, vl );
		vuint8m1_t zeros = __riscv_vmv_v_x_u8m1( 0, vl );
		__riscv_vse8_v_u8m1( dst + i, __riscv_vor_vv_u8m1( existing, __riscv_vmerge_vvm_u8m1( zeros, bit, mask, vl ), vl ), vl );
		i += vl;
	}
}

void VPCALL idSIMD_RVV::CmpGE( byte *dst, const float *src0, const float constant, const int count ) {
	int i = 0;
	while ( i < count ) {
		size_t vl = __riscv_vsetvl_e32m4( count - i );
		vfloat32m4_t v = __riscv_vle32_v_f32m4( src0 + i, vl );
		vbool8_t mask = __riscv_vmfge_vf_f32m4_b8( v, constant, vl );
		vuint8m1_t ones = __riscv_vmv_v_x_u8m1( 1, vl );
		vuint8m1_t zeros = __riscv_vmv_v_x_u8m1( 0, vl );
		__riscv_vse8_v_u8m1( dst + i, __riscv_vmerge_vvm_u8m1( zeros, ones, mask, vl ), vl );
		i += vl;
	}
}

void VPCALL idSIMD_RVV::CmpGE( byte *dst, const byte bitNum, const float *src0, const float constant, const int count ) {
	int i = 0;
	while ( i < count ) {
		size_t vl = __riscv_vsetvl_e32m4( count - i );
		vfloat32m4_t v = __riscv_vle32_v_f32m4( src0 + i, vl );
		vbool8_t mask = __riscv_vmfge_vf_f32m4_b8( v, constant, vl );
		vuint8m1_t existing = __riscv_vle8_v_u8m1( dst + i, vl );
		vuint8m1_t bit = __riscv_vmv_v_x_u8m1( 1 << bitNum, vl );
		vuint8m1_t zeros = __riscv_vmv_v_x_u8m1( 0, vl );
		__riscv_vse8_v_u8m1( dst + i, __riscv_vor_vv_u8m1( existing, __riscv_vmerge_vvm_u8m1( zeros, bit, mask, vl ), vl ), vl );
		i += vl;
	}
}

void VPCALL idSIMD_RVV::CmpLT( byte *dst, const float *src0, const float constant, const int count ) {
	int i = 0;
	while ( i < count ) {
		size_t vl = __riscv_vsetvl_e32m4( count - i );
		vfloat32m4_t v = __riscv_vle32_v_f32m4( src0 + i, vl );
		vbool8_t mask = __riscv_vmflt_vf_f32m4_b8( v, constant, vl );
		vuint8m1_t ones = __riscv_vmv_v_x_u8m1( 1, vl );
		vuint8m1_t zeros = __riscv_vmv_v_x_u8m1( 0, vl );
		__riscv_vse8_v_u8m1( dst + i, __riscv_vmerge_vvm_u8m1( zeros, ones, mask, vl ), vl );
		i += vl;
	}
}

void VPCALL idSIMD_RVV::CmpLT( byte *dst, const byte bitNum, const float *src0, const float constant, const int count ) {
	int i = 0;
	while ( i < count ) {
		size_t vl = __riscv_vsetvl_e32m4( count - i );
		vfloat32m4_t v = __riscv_vle32_v_f32m4( src0 + i, vl );
		vbool8_t mask = __riscv_vmflt_vf_f32m4_b8( v, constant, vl );
		vuint8m1_t existing = __riscv_vle8_v_u8m1( dst + i, vl );
		vuint8m1_t bit = __riscv_vmv_v_x_u8m1( 1 << bitNum, vl );
		vuint8m1_t zeros = __riscv_vmv_v_x_u8m1( 0, vl );
		__riscv_vse8_v_u8m1( dst + i, __riscv_vor_vv_u8m1( existing, __riscv_vmerge_vvm_u8m1( zeros, bit, mask, vl ), vl ), vl );
		i += vl;
	}
}

void VPCALL idSIMD_RVV::CmpLE( byte *dst, const float *src0, const float constant, const int count ) {
	int i = 0;
	while ( i < count ) {
		size_t vl = __riscv_vsetvl_e32m4( count - i );
		vfloat32m4_t v = __riscv_vle32_v_f32m4( src0 + i, vl );
		vbool8_t mask = __riscv_vmfle_vf_f32m4_b8( v, constant, vl );
		vuint8m1_t ones = __riscv_vmv_v_x_u8m1( 1, vl );
		vuint8m1_t zeros = __riscv_vmv_v_x_u8m1( 0, vl );
		__riscv_vse8_v_u8m1( dst + i, __riscv_vmerge_vvm_u8m1( zeros, ones, mask, vl ), vl );
		i += vl;
	}
}

void VPCALL idSIMD_RVV::CmpLE( byte *dst, const byte bitNum, const float *src0, const float constant, const int count ) {
	int i = 0;
	while ( i < count ) {
		size_t vl = __riscv_vsetvl_e32m4( count - i );
		vfloat32m4_t v = __riscv_vle32_v_f32m4( src0 + i, vl );
		vbool8_t mask = __riscv_vmfle_vf_f32m4_b8( v, constant, vl );
		vuint8m1_t existing = __riscv_vle8_v_u8m1( dst + i, vl );
		vuint8m1_t bit = __riscv_vmv_v_x_u8m1( 1 << bitNum, vl );
		vuint8m1_t zeros = __riscv_vmv_v_x_u8m1( 0, vl );
		__riscv_vse8_v_u8m1( dst + i, __riscv_vor_vv_u8m1( existing, __riscv_vmerge_vvm_u8m1( zeros, bit, mask, vl ), vl ), vl );
		i += vl;
	}
}

void VPCALL idSIMD_RVV::Memcpy( void *dst, const void *src, const int count ) {
	byte *d = reinterpret_cast<byte *>( dst );
	const byte *s = reinterpret_cast<const byte *>( src );
	int i = 0;
	while ( i < count ) {
		size_t vl = __riscv_vsetvl_e8m8( count - i );
		vuint8m8_t v = __riscv_vle8_v_u8m8( s + i, vl );
		__riscv_vse8_v_u8m8( d + i, v, vl );
		i += vl;
	}
}

void VPCALL idSIMD_RVV::Memset( void *dst, const int val, const int count ) {
	byte *d = reinterpret_cast<byte *>( dst );
	int i = 0;
	while ( i < count ) {
		size_t vl = __riscv_vsetvl_e8m8( count - i );
		vuint8m8_t v = __riscv_vmv_v_x_u8m8( static_cast<uint8_t>( val ), vl );
		__riscv_vse8_v_u8m8( d + i, v, vl );
		i += vl;
	}
}

void VPCALL idSIMD_RVV::Zero16( float *dst, const int count ) {
	int i = 0;
	while ( i < count ) {
		size_t vl = __riscv_vsetvl_e32m8( count - i );
		vfloat32m8_t v = __riscv_vfmv_v_f_f32m8( 0.0f, vl );
		__riscv_vse32_v_f32m8( dst + i, v, vl );
		i += vl;
	}
}

void VPCALL idSIMD_RVV::Copy16( float *dst, const float *src, const int count ) {
	int i = 0;
	while ( i < count ) {
		size_t vl = __riscv_vsetvl_e32m8( count - i );
		vfloat32m8_t v = __riscv_vle32_v_f32m8( src + i, vl );
		__riscv_vse32_v_f32m8( dst + i, v, vl );
		i += vl;
	}
}

void VPCALL idSIMD_RVV::Negate16( float *dst, const int count ) {
	int i = 0;
	while ( i < count ) {
		size_t vl = __riscv_vsetvl_e32m8( count - i );
		vuint32m8_t v = __riscv_vle32_v_u32m8( (const uint32_t *)dst + i, vl );
		v = __riscv_vxor_vx_u32m8( v, 0x80000000U, vl );
		__riscv_vse32_v_u32m8( (uint32_t *)dst + i, v, vl );
		i += vl;
	}
}

void VPCALL idSIMD_RVV::Add16( float *dst, const float *src1, const float *src2, const int count ) {
	Add( dst, src1, src2, count );
}

void VPCALL idSIMD_RVV::Sub16( float *dst, const float *src1, const float *src2, const int count ) {
	Sub( dst, src1, src2, count );
}

void VPCALL idSIMD_RVV::Mul16( float *dst, const float *src1, const float constant, const int count ) {
	Mul( dst, constant, src1, count );
}

void VPCALL idSIMD_RVV::AddAssign16( float *dst, const float *src, const int count ) {
	int i = 0;
	while ( i < count ) {
		size_t vl = __riscv_vsetvl_e32m8( count - i );
		vfloat32m8_t d = __riscv_vle32_v_f32m8( dst + i, vl );
		vfloat32m8_t s = __riscv_vle32_v_f32m8( src + i, vl );
		__riscv_vse32_v_f32m8( dst + i, __riscv_vfadd_vv_f32m8( d, s, vl ), vl );
		i += vl;
	}
}

void VPCALL idSIMD_RVV::SubAssign16( float *dst, const float *src, const int count ) {
	int i = 0;
	while ( i < count ) {
		size_t vl = __riscv_vsetvl_e32m8( count - i );
		vfloat32m8_t d = __riscv_vle32_v_f32m8( dst + i, vl );
		vfloat32m8_t s = __riscv_vle32_v_f32m8( src + i, vl );
		__riscv_vse32_v_f32m8( dst + i, __riscv_vfsub_vv_f32m8( d, s, vl ), vl );
		i += vl;
	}
}

void VPCALL idSIMD_RVV::MulAssign16( float *dst, const float constant, const int count ) {
	int i = 0;
	while ( i < count ) {
		size_t vl = __riscv_vsetvl_e32m8( count - i );
		vfloat32m8_t d = __riscv_vle32_v_f32m8( dst + i, vl );
		d = __riscv_vfmul_vf_f32m8( d, constant, vl );
		__riscv_vse32_v_f32m8( dst + i, d, vl );
		i += vl;
	}
}

int VPCALL idSIMD_RVV::CreateVertexProgramShadowCache( idVec4 *vertexCache, const idDrawVert *verts, const int numVerts ) {
	const ptrdiff_t in_stride = (ptrdiff_t)sizeof( idDrawVert );
	const ptrdiff_t out_stride = (ptrdiff_t)sizeof( idVec4 ) * 2;
	float *vf = (float *)vertexCache;
	int i = 0;
	while ( i < numVerts ) {
		size_t vl = __riscv_vsetvl_e32m8( numVerts - i );
		const idDrawVert *s = verts + i;
		vfloat32m8_t x = __riscv_vlse32_v_f32m8( RVV_DrawVertXYZPtr( s, 0 ), in_stride, vl );
		vfloat32m8_t y = __riscv_vlse32_v_f32m8( RVV_DrawVertXYZPtr( s, 1 ), in_stride, vl );
		vfloat32m8_t z = __riscv_vlse32_v_f32m8( RVV_DrawVertXYZPtr( s, 2 ), in_stride, vl );
		vfloat32m8_t ones = __riscv_vfmv_v_f_f32m8( 1.0f, vl );
		vfloat32m8_t zeros = __riscv_vfmv_v_f_f32m8( 0.0f, vl );
		__riscv_vsse32_v_f32m8( vf + i * 8 + 0, out_stride, x, vl );
		__riscv_vsse32_v_f32m8( vf + i * 8 + 1, out_stride, y, vl );
		__riscv_vsse32_v_f32m8( vf + i * 8 + 2, out_stride, z, vl );
		__riscv_vsse32_v_f32m8( vf + i * 8 + 3, out_stride, ones, vl );
		__riscv_vsse32_v_f32m8( vf + i * 8 + 4, out_stride, x, vl );
		__riscv_vsse32_v_f32m8( vf + i * 8 + 5, out_stride, y, vl );
		__riscv_vsse32_v_f32m8( vf + i * 8 + 6, out_stride, z, vl );
		__riscv_vsse32_v_f32m8( vf + i * 8 + 7, out_stride, zeros, vl );
		i += vl;
	}
	return numVerts * 2;
}

void VPCALL idSIMD_RVV::MixSoundTwoSpeakerMono( float *mixBuffer, const float *samples, const int numSamples, const float lastV[2], const float currentV[2] ) {
	float sL = lastV[0];
	float sR = lastV[1];
	float incL = ( currentV[0] - lastV[0] ) / (float)MIXBUFFER_SAMPLES;
	float incR = ( currentV[1] - lastV[1] ) / (float)MIXBUFFER_SAMPLES;
	assert( numSamples == MIXBUFFER_SAMPLES );
	int j = 0;
	while ( j < MIXBUFFER_SAMPLES ) {
		size_t vl = __riscv_vsetvl_e32m4( MIXBUFFER_SAMPLES - j );
		vuint32m4_t idx = __riscv_vid_v_u32m4( vl );
		idx = __riscv_vadd_vx_u32m4( idx, j, vl );
		vfloat32m4_t idx_f = __riscv_vfcvt_f_xu_v_f32m4( idx, vl );
		vfloat32m4_t volL = __riscv_vfmv_v_f_f32m4( sL, vl );
		volL = __riscv_vfmacc_vf_f32m4( volL, incL, idx_f, vl );
		vfloat32m4_t volR = __riscv_vfmv_v_f_f32m4( sR, vl );
		volR = __riscv_vfmacc_vf_f32m4( volR, incR, idx_f, vl );
		vfloat32m4_t s = __riscv_vle32_v_f32m4( samples + j, vl );
		/* mixBuffer is interleaved L,R,L,R — use stride 2 so the L and R lanes stay
		 * disjoint (unit-stride loads/stores overlap and corrupt each other). */
		vfloat32m4_t ml = __riscv_vlse32_v_f32m4( mixBuffer + j * 2, 2 * sizeof( float ), vl );
		vfloat32m4_t mr = __riscv_vlse32_v_f32m4( mixBuffer + j * 2 + 1, 2 * sizeof( float ), vl );
		ml = __riscv_vfmacc_vv_f32m4( ml, s, volL, vl );
		mr = __riscv_vfmacc_vv_f32m4( mr, s, volR, vl );
		__riscv_vsse32_v_f32m4( mixBuffer + j * 2, 2 * sizeof( float ), ml, vl );
		__riscv_vsse32_v_f32m4( mixBuffer + j * 2 + 1, 2 * sizeof( float ), mr, vl );
		j += vl;
	}
}

void VPCALL idSIMD_RVV::MixSoundTwoSpeakerStereo( float *mixBuffer, const float *samples, const int numSamples, const float lastV[2], const float currentV[2] ) {
	float sL = lastV[0];
	float sR = lastV[1];
	float incL = ( currentV[0] - lastV[0] ) / (float)MIXBUFFER_SAMPLES;
	float incR = ( currentV[1] - lastV[1] ) / (float)MIXBUFFER_SAMPLES;
	assert( numSamples == MIXBUFFER_SAMPLES );
	int j = 0;
	while ( j < MIXBUFFER_SAMPLES ) {
		size_t vl = __riscv_vsetvl_e32m4( MIXBUFFER_SAMPLES - j );
		vuint32m4_t idx = __riscv_vid_v_u32m4( vl );
		idx = __riscv_vadd_vx_u32m4( idx, j, vl );
		vfloat32m4_t idx_f = __riscv_vfcvt_f_xu_v_f32m4( idx, vl );
		vfloat32m4_t volL = __riscv_vfmv_v_f_f32m4( sL, vl );
		volL = __riscv_vfmacc_vf_f32m4( volL, incL, idx_f, vl );
		vfloat32m4_t volR = __riscv_vfmv_v_f_f32m4( sR, vl );
		volR = __riscv_vfmacc_vf_f32m4( volR, incR, idx_f, vl );
		/* Both the interleaved stereo source AND the interleaved mix buffer need
		 * stride 2 so L/R channels stay disjoint. */
		vfloat32m4_t s0 = __riscv_vlse32_v_f32m4( samples + j * 2, 2 * sizeof( float ), vl );
		vfloat32m4_t s1 = __riscv_vlse32_v_f32m4( samples + j * 2 + 1, 2 * sizeof( float ), vl );
		vfloat32m4_t ml = __riscv_vlse32_v_f32m4( mixBuffer + j * 2, 2 * sizeof( float ), vl );
		vfloat32m4_t mr = __riscv_vlse32_v_f32m4( mixBuffer + j * 2 + 1, 2 * sizeof( float ), vl );
		ml = __riscv_vfmacc_vv_f32m4( ml, s0, volL, vl );
		mr = __riscv_vfmacc_vv_f32m4( mr, s1, volR, vl );
		__riscv_vsse32_v_f32m4( mixBuffer + j * 2, 2 * sizeof( float ), ml, vl );
		__riscv_vsse32_v_f32m4( mixBuffer + j * 2 + 1, 2 * sizeof( float ), mr, vl );
		j += vl;
	}
}

void VPCALL idSIMD_RVV::MixedSoundToSamples( short *samples, const float *mixBuffer, const int numSamples ) {
	int i = 0;
	while ( i < numSamples ) {
		size_t vl = __riscv_vsetvl_e32m8( numSamples - i );
		vfloat32m8_t v = __riscv_vle32_v_f32m8( mixBuffer + i, vl );
		v = __riscv_vfmax_vf_f32m8( v, -32768.0f, vl );
		v = __riscv_vfmin_vf_f32m8( v, 32767.0f, vl );
		vint16m4_t vs = __riscv_vfncvt_rtz_x_f_w_i16m4( v, vl );
		__riscv_vse16_v_i16m4( samples + i, vs, vl );
		i += vl;
	}
}

static ID_INLINE const float *RVV_DrawVertNormalPtr( const idDrawVert *v, int component ) {
	return v[0].normal.ToFloatPtr() + component;
}

static ID_INLINE float *RVV_DrawVertNormalPtr( idDrawVert *v, int component ) {
	return v[0].normal.ToFloatPtr() + component;
}

static ID_INLINE const float *RVV_DrawVertTangentPtr( const idDrawVert *v, int tangentIdx, int component ) {
	return v[0].tangents[tangentIdx].ToFloatPtr() + component;
}

static ID_INLINE float *RVV_DrawVertTangentPtr( idDrawVert *v, int tangentIdx, int component ) {
	return v[0].tangents[tangentIdx].ToFloatPtr() + component;
}

void VPCALL idSIMD_RVV::NormalizeTangents( idDrawVert *verts, const int numVerts ) {
	const ptrdiff_t stride = (ptrdiff_t)sizeof( idDrawVert );
	const float half_f = 0.5f;
	const float one_five_f = 1.5f;
	int i = 0;
	while ( i < numVerts ) {
		size_t vl = __riscv_vsetvl_e32m4( numVerts - i );
		idDrawVert *v = verts + i;
		vfloat32m4_t nx = __riscv_vlse32_v_f32m4( RVV_DrawVertNormalPtr( v, 0 ), stride, vl );
		vfloat32m4_t ny = __riscv_vlse32_v_f32m4( RVV_DrawVertNormalPtr( v, 1 ), stride, vl );
		vfloat32m4_t nz = __riscv_vlse32_v_f32m4( RVV_DrawVertNormalPtr( v, 2 ), stride, vl );
		vfloat32m4_t lensq = __riscv_vfmul_vv_f32m4( nx, nx, vl );
		lensq = __riscv_vfmacc_vv_f32m4( lensq, ny, ny, vl );
		lensq = __riscv_vfmacc_vv_f32m4( lensq, nz, nz, vl );
		// Guard vfrsqrt7(+0) = +inf -> 0*inf = NaN; see DeriveTriPlanes below.
		lensq = __riscv_vfmax_vf_f32m4( lensq, 1.17549435e-38f, vl );
		vfloat32m4_t inv_len = __riscv_vfrsqrt7_v_f32m4( lensq, vl );
		vfloat32m4_t tmp = __riscv_vfmul_vv_f32m4( lensq, inv_len, vl );
		tmp = __riscv_vfmul_vv_f32m4( tmp, inv_len, vl );
		tmp = __riscv_vfmul_vf_f32m4( tmp, half_f, vl );
		inv_len = __riscv_vfmul_vv_f32m4( inv_len, __riscv_vfrsub_vf_f32m4( tmp, one_five_f, vl ), vl );
		nx = __riscv_vfmul_vv_f32m4( nx, inv_len, vl );
		ny = __riscv_vfmul_vv_f32m4( ny, inv_len, vl );
		nz = __riscv_vfmul_vv_f32m4( nz, inv_len, vl );
		__riscv_vsse32_v_f32m4( RVV_DrawVertNormalPtr( v, 0 ), stride, nx, vl );
		__riscv_vsse32_v_f32m4( RVV_DrawVertNormalPtr( v, 1 ), stride, ny, vl );
		__riscv_vsse32_v_f32m4( RVV_DrawVertNormalPtr( v, 2 ), stride, nz, vl );
		for ( int j = 0; j < 2; j++ ) {
			vfloat32m4_t tx = __riscv_vlse32_v_f32m4( RVV_DrawVertTangentPtr( v, j, 0 ), stride, vl );
			vfloat32m4_t ty = __riscv_vlse32_v_f32m4( RVV_DrawVertTangentPtr( v, j, 1 ), stride, vl );
			vfloat32m4_t tz = __riscv_vlse32_v_f32m4( RVV_DrawVertTangentPtr( v, j, 2 ), stride, vl );
			vfloat32m4_t dot = __riscv_vfmul_vv_f32m4( tx, nx, vl );
			dot = __riscv_vfmacc_vv_f32m4( dot, ty, ny, vl );
			dot = __riscv_vfmacc_vv_f32m4( dot, tz, nz, vl );
			tx = __riscv_vfnmsac_vv_f32m4( tx, dot, nx, vl );
			ty = __riscv_vfnmsac_vv_f32m4( ty, dot, ny, vl );
			tz = __riscv_vfnmsac_vv_f32m4( tz, dot, nz, vl );
			vfloat32m4_t tlensq = __riscv_vfmul_vv_f32m4( tx, tx, vl );
			tlensq = __riscv_vfmacc_vv_f32m4( tlensq, ty, ty, vl );
			tlensq = __riscv_vfmacc_vv_f32m4( tlensq, tz, tz, vl );
			// Guard vfrsqrt7(+0) = +inf -> 0*inf = NaN (degenerate tangent).
			tlensq = __riscv_vfmax_vf_f32m4( tlensq, 1.17549435e-38f, vl );
			vfloat32m4_t inv_lt = __riscv_vfrsqrt7_v_f32m4( tlensq, vl );
			tmp = __riscv_vfmul_vv_f32m4( tlensq, inv_lt, vl );
			tmp = __riscv_vfmul_vv_f32m4( tmp, inv_lt, vl );
			tmp = __riscv_vfmul_vf_f32m4( tmp, half_f, vl );
			inv_lt = __riscv_vfmul_vv_f32m4( inv_lt, __riscv_vfrsub_vf_f32m4( tmp, one_five_f, vl ), vl );
			tx = __riscv_vfmul_vv_f32m4( tx, inv_lt, vl );
			ty = __riscv_vfmul_vv_f32m4( ty, inv_lt, vl );
			tz = __riscv_vfmul_vv_f32m4( tz, inv_lt, vl );
			__riscv_vsse32_v_f32m4( RVV_DrawVertTangentPtr( v, j, 0 ), stride, tx, vl );
			__riscv_vsse32_v_f32m4( RVV_DrawVertTangentPtr( v, j, 1 ), stride, ty, vl );
			__riscv_vsse32_v_f32m4( RVV_DrawVertTangentPtr( v, j, 2 ), stride, tz, vl );
		}
		i += vl;
	}
}

void VPCALL idSIMD_RVV::TracePointCull( byte *cullBits, byte &totalOr, const float radius, const idPlane *planes, const idDrawVert *verts, const int numVerts ) {
	int i = 0;
	byte tOr = 0;
	const int stride = sizeof( idDrawVert );

	while ( i < numVerts ) {
		size_t vl = __riscv_vsetvl_e32m4( numVerts - i );

		vfloat32m4_t vx = __riscv_vlse32_v_f32m4( &verts[i].xyz.x, stride, vl );
		vfloat32m4_t vy = __riscv_vlse32_v_f32m4( &verts[i].xyz.y, stride, vl );
		vfloat32m4_t vz = __riscv_vlse32_v_f32m4( &verts[i].xyz.z, stride, vl );

		vuint8m1_t cull = __riscv_vmv_v_x_u8m1( 0, vl );
		vuint8m1_t zeros = __riscv_vmv_v_x_u8m1( 0, vl );

		for ( int p = 0; p < 4; p++ ) {
			float pnx = planes[p].Normal()[0];
			float pny = planes[p].Normal()[1];
			float pnz = planes[p].Normal()[2];
			float pd = planes[p].Dist();

			vfloat32m4_t d = __riscv_vfmul_vf_f32m4( vx, pnx, vl );
			d = __riscv_vfmacc_vf_f32m4( d, pny, vy, vl );
			d = __riscv_vfmacc_vf_f32m4( d, pnz, vz, vl );
			d = __riscv_vfadd_vf_f32m4( d, pd, vl );

			{
				vbool8_t mask = __riscv_vmflt_vf_f32m4_b8( d, -radius, vl );
				vuint8m1_t bit = __riscv_vmv_v_x_u8m1( 1 << p, vl );
				cull = __riscv_vor_vv_u8m1( cull, __riscv_vmerge_vvm_u8m1( zeros, bit, mask, vl ), vl );
			}

			{
				vbool8_t mask = __riscv_vmflt_vf_f32m4_b8( d, radius, vl );
				vuint8m1_t bit = __riscv_vmv_v_x_u8m1( 1 << ( p + 4 ), vl );
				cull = __riscv_vor_vv_u8m1( cull, __riscv_vmerge_vvm_u8m1( zeros, bit, mask, vl ), vl );
			}
		}

		cull = __riscv_vxor_vx_u8m1( cull, 0x0F, vl );

		__riscv_vse8_v_u8m1( cullBits + i, cull, vl );

		tOr |= __riscv_vmv_x_s_u8m1_u8( __riscv_vredor_vs_u8m1_u8m1( cull, __riscv_vmv_v_x_u8m1( 0, vl ), vl ) );

		i += vl;
	}

	totalOr = tOr;
}

void VPCALL idSIMD_RVV::DecalPointCull( byte *cullBits, const idPlane *planes, const idDrawVert *verts, const int numVerts ) {
	int i = 0;
	const int stride = sizeof( idDrawVert );

	while ( i < numVerts ) {
		size_t vl = __riscv_vsetvl_e32m4( numVerts - i );

		vfloat32m4_t vx = __riscv_vlse32_v_f32m4( &verts[i].xyz.x, stride, vl );
		vfloat32m4_t vy = __riscv_vlse32_v_f32m4( &verts[i].xyz.y, stride, vl );
		vfloat32m4_t vz = __riscv_vlse32_v_f32m4( &verts[i].xyz.z, stride, vl );

		vuint8m1_t cull = __riscv_vmv_v_x_u8m1( 0, vl );
		vuint8m1_t zeros = __riscv_vmv_v_x_u8m1( 0, vl );

		for ( int p = 0; p < 6; p++ ) {
			float pnx = planes[p].Normal()[0];
			float pny = planes[p].Normal()[1];
			float pnz = planes[p].Normal()[2];
			float pd = planes[p].Dist();

			vfloat32m4_t d = __riscv_vfmul_vf_f32m4( vx, pnx, vl );
			d = __riscv_vfmacc_vf_f32m4( d, pny, vy, vl );
			d = __riscv_vfmacc_vf_f32m4( d, pnz, vz, vl );
			d = __riscv_vfadd_vf_f32m4( d, pd, vl );

			vbool8_t mask = __riscv_vmflt_vf_f32m4_b8( d, 0.0f, vl );
			vuint8m1_t bit = __riscv_vmv_v_x_u8m1( 1 << p, vl );
			cull = __riscv_vor_vv_u8m1( cull, __riscv_vmerge_vvm_u8m1( zeros, bit, mask, vl ), vl );
		}

		cull = __riscv_vxor_vx_u8m1( cull, 0x3F, vl );

		__riscv_vse8_v_u8m1( cullBits + i, cull, vl );
		i += vl;
	}
}

void VPCALL idSIMD_RVV::OverlayPointCull( byte *cullBits, idVec2 *texCoords, const idPlane *planes, const idDrawVert *verts, const int numVerts ) {
	int i = 0;
	const int stride = sizeof( idDrawVert );

	while ( i < numVerts ) {
		size_t vl = __riscv_vsetvl_e32m4( numVerts - i );

		vfloat32m4_t vx = __riscv_vlse32_v_f32m4( &verts[i].xyz.x, stride, vl );
		vfloat32m4_t vy = __riscv_vlse32_v_f32m4( &verts[i].xyz.y, stride, vl );
		vfloat32m4_t vz = __riscv_vlse32_v_f32m4( &verts[i].xyz.z, stride, vl );

		float pnx0 = planes[0].Normal()[0];
		float pny0 = planes[0].Normal()[1];
		float pnz0 = planes[0].Normal()[2];
		float pd0 = planes[0].Dist();

		vfloat32m4_t d0 = __riscv_vfmul_vf_f32m4( vx, pnx0, vl );
		d0 = __riscv_vfmacc_vf_f32m4( d0, pny0, vy, vl );
		d0 = __riscv_vfmacc_vf_f32m4( d0, pnz0, vz, vl );
		d0 = __riscv_vfadd_vf_f32m4( d0, pd0, vl );

		float pnx1 = planes[1].Normal()[0];
		float pny1 = planes[1].Normal()[1];
		float pnz1 = planes[1].Normal()[2];
		float pd1 = planes[1].Dist();

		vfloat32m4_t d1 = __riscv_vfmul_vf_f32m4( vx, pnx1, vl );
		d1 = __riscv_vfmacc_vf_f32m4( d1, pny1, vy, vl );
		d1 = __riscv_vfmacc_vf_f32m4( d1, pnz1, vz, vl );
		d1 = __riscv_vfadd_vf_f32m4( d1, pd1, vl );

		__riscv_vsse32_v_f32m4( &texCoords[i][0], sizeof( idVec2 ), d0, vl );
		__riscv_vsse32_v_f32m4( &texCoords[i][1], sizeof( idVec2 ), d1, vl );

		vfloat32m4_t d0_m1 = __riscv_vfrsub_vf_f32m4( d0, 1.0f, vl );
		vfloat32m4_t d1_m1 = __riscv_vfrsub_vf_f32m4( d1, 1.0f, vl );

		vuint8m1_t cull = __riscv_vmv_v_x_u8m1( 0, vl );
		vuint8m1_t zeros = __riscv_vmv_v_x_u8m1( 0, vl );

		{
			vbool8_t mask = __riscv_vmflt_vf_f32m4_b8( d0, 0.0f, vl );
			vuint8m1_t bit = __riscv_vmv_v_x_u8m1( 1 << 0, vl );
			cull = __riscv_vor_vv_u8m1( cull, __riscv_vmerge_vvm_u8m1( zeros, bit, mask, vl ), vl );
		}

		{
			vbool8_t mask = __riscv_vmflt_vf_f32m4_b8( d1, 0.0f, vl );
			vuint8m1_t bit = __riscv_vmv_v_x_u8m1( 1 << 1, vl );
			cull = __riscv_vor_vv_u8m1( cull, __riscv_vmerge_vvm_u8m1( zeros, bit, mask, vl ), vl );
		}

		{
			vbool8_t mask = __riscv_vmflt_vf_f32m4_b8( d0_m1, 0.0f, vl );
			vuint8m1_t bit = __riscv_vmv_v_x_u8m1( 1 << 2, vl );
			cull = __riscv_vor_vv_u8m1( cull, __riscv_vmerge_vvm_u8m1( zeros, bit, mask, vl ), vl );
		}

		{
			vbool8_t mask = __riscv_vmflt_vf_f32m4_b8( d1_m1, 0.0f, vl );
			vuint8m1_t bit = __riscv_vmv_v_x_u8m1( 1 << 3, vl );
			cull = __riscv_vor_vv_u8m1( cull, __riscv_vmerge_vvm_u8m1( zeros, bit, mask, vl ), vl );
		}

		__riscv_vse8_v_u8m1( cullBits + i, cull, vl );
		i += vl;
	}
}

void VPCALL idSIMD_RVV::DeriveTriPlanes( idPlane *planes, const idDrawVert *verts, const int numVerts, const int *indexes, const int numIndexes ) {
	const int vf = sizeof( idDrawVert );		// BYTE stride: vluxei32 indexes are byte offsets
	const float *vf_base = (const float *)verts;
	float *pf = (float *)planes;
	int i = 0;
	int numTris = numIndexes / 3;
	while ( i < numTris ) {
		size_t vl = __riscv_vsetvl_e32m1( numTris - i );

		const unsigned int *ui = (const unsigned int *)indexes;
		vuint32m1_t ai = __riscv_vlse32_v_u32m1( ui + i*3 + 0, 3 * sizeof( unsigned int ), vl );
		vuint32m1_t bi = __riscv_vlse32_v_u32m1( ui + i*3 + 1, 3 * sizeof( unsigned int ), vl );
		vuint32m1_t ci = __riscv_vlse32_v_u32m1( ui + i*3 + 2, 3 * sizeof( unsigned int ), vl );

		vuint32m1_t ab = __riscv_vmul_vx_u32m1( ai, vf, vl );
		vuint32m1_t bb = __riscv_vmul_vx_u32m1( bi, vf, vl );
		vuint32m1_t cb = __riscv_vmul_vx_u32m1( ci, vf, vl );

		vfloat32m1_t ax = __riscv_vluxei32_v_f32m1( vf_base, ab, vl );
		vfloat32m1_t ay = __riscv_vluxei32_v_f32m1( vf_base, __riscv_vadd_vx_u32m1( ab, sizeof( float ), vl ), vl );
		vfloat32m1_t az = __riscv_vluxei32_v_f32m1( vf_base, __riscv_vadd_vx_u32m1( ab, 2 * sizeof( float ), vl ), vl );

		vfloat32m1_t bx = __riscv_vluxei32_v_f32m1( vf_base, bb, vl );
		vfloat32m1_t by = __riscv_vluxei32_v_f32m1( vf_base, __riscv_vadd_vx_u32m1( bb, sizeof( float ), vl ), vl );
		vfloat32m1_t bz = __riscv_vluxei32_v_f32m1( vf_base, __riscv_vadd_vx_u32m1( bb, 2 * sizeof( float ), vl ), vl );

		vfloat32m1_t cx = __riscv_vluxei32_v_f32m1( vf_base, cb, vl );
		vfloat32m1_t cy = __riscv_vluxei32_v_f32m1( vf_base, __riscv_vadd_vx_u32m1( cb, sizeof( float ), vl ), vl );
		vfloat32m1_t cz = __riscv_vluxei32_v_f32m1( vf_base, __riscv_vadd_vx_u32m1( cb, 2 * sizeof( float ), vl ), vl );

		vfloat32m1_t d0x = __riscv_vfsub_vv_f32m1( bx, ax, vl );
		vfloat32m1_t d0y = __riscv_vfsub_vv_f32m1( by, ay, vl );
		vfloat32m1_t d0z = __riscv_vfsub_vv_f32m1( bz, az, vl );

		vfloat32m1_t d1x = __riscv_vfsub_vv_f32m1( cx, ax, vl );
		vfloat32m1_t d1y = __riscv_vfsub_vv_f32m1( cy, ay, vl );
		vfloat32m1_t d1z = __riscv_vfsub_vv_f32m1( cz, az, vl );

		/* n = d1 x d0 (matches generic); the reverse d0 x d1 would negate the plane. */
		vfloat32m1_t nx = __riscv_vfmul_vv_f32m1( d0z, d1y, vl );
		nx = __riscv_vfnmsac_vv_f32m1( nx, d0y, d1z, vl );

		vfloat32m1_t ny = __riscv_vfmul_vv_f32m1( d0x, d1z, vl );
		ny = __riscv_vfnmsac_vv_f32m1( ny, d0z, d1x, vl );

		vfloat32m1_t nz = __riscv_vfmul_vv_f32m1( d0y, d1x, vl );
		nz = __riscv_vfnmsac_vv_f32m1( nz, d0x, d1y, vl );

		vfloat32m1_t len2 = __riscv_vfmul_vv_f32m1( nx, nx, vl );
		len2 = __riscv_vfmacc_vv_f32m1( len2, ny, ny, vl );
		len2 = __riscv_vfmacc_vv_f32m1( len2, nz, nz, vl );
		// Degenerate triangles are common in Doom 3 meshes and land here with
		// len2 == 0. RVV defines vfrsqrt7(+0) = +inf, so the Newton step below
		// computes len2*inv_len = 0*inf = NaN and every plane derived from it is
		// NaN -- and NaN comparisons return false, which silently flips facing
		// tests in the shadow-volume code. The generic idMath::RSqrt returns a
		// finite ~1.98e19 for zero, giving a zero normal instead. Clamping to
		// FLT_MIN reproduces that finite behaviour; anything below FLT_MIN is a
		// degenerate triangle whose normal is meaningless either way.
		len2 = __riscv_vfmax_vf_f32m1( len2, 1.17549435e-38f, vl );
		vfloat32m1_t inv_len = __riscv_vfrsqrt7_v_f32m1( len2, vl );
		vfloat32m1_t yr = __riscv_vfmul_vv_f32m1( len2, inv_len, vl );
		yr = __riscv_vfmul_vv_f32m1( yr, inv_len, vl );
		yr = __riscv_vfrsub_vf_f32m1( yr, 3.0f, vl );
		inv_len = __riscv_vfmul_vv_f32m1( inv_len, yr, vl );
		inv_len = __riscv_vfmul_vf_f32m1( inv_len, 0.5f, vl );
		nx = __riscv_vfmul_vv_f32m1( nx, inv_len, vl );
		ny = __riscv_vfmul_vv_f32m1( ny, inv_len, vl );
		nz = __riscv_vfmul_vv_f32m1( nz, inv_len, vl );

		vfloat32m1_t pd = __riscv_vfmul_vv_f32m1( nx, ax, vl );
		pd = __riscv_vfmacc_vv_f32m1( pd, ny, ay, vl );
		pd = __riscv_vfmacc_vv_f32m1( pd, nz, az, vl );
		pd = __riscv_vfneg_v_f32m1( pd, vl );

		ptrdiff_t ps = sizeof( idPlane );
		__riscv_vsse32_v_f32m1( pf + i*4 + 0, ps, nx, vl );
		__riscv_vsse32_v_f32m1( pf + i*4 + 1, ps, ny, vl );
		__riscv_vsse32_v_f32m1( pf + i*4 + 2, ps, nz, vl );
		__riscv_vsse32_v_f32m1( pf + i*4 + 3, ps, pd, vl );
		i += vl;
	}
}

void VPCALL idSIMD_RVV::DeriveTangents( idPlane *planes, idDrawVert *verts, const int numVerts, const int *indexes, const int numIndexes ) {
	idSIMD_Generic::DeriveTangents( planes, verts, numVerts, indexes, numIndexes );
}

void VPCALL idSIMD_RVV::DeriveUnsmoothedTangents( idDrawVert *verts, const dominantTri_s *dominantTris, const int numVerts ) {
	const int vs = sizeof( idDrawVert );
	const int ds = sizeof( dominantTri_s );
	const float *vf = verts[0].xyz.ToFloatPtr();

	int i = 0;
	while ( i < numVerts ) {
		size_t vl = __riscv_vsetvl_e32m1( numVerts - i );

		vuint32m1_t v2_idx = __riscv_vlse32_v_u32m1( (const uint32_t *)&dominantTris[i].v2, ds, vl );
		vuint32m1_t v3_idx = __riscv_vlse32_v_u32m1( (const uint32_t *)&dominantTris[i].v3, ds, vl );

		/* BYTE offsets (vluxei32 indexes are bytes): idx*sizeof(idDrawVert), and the
		 * y/z/st[1] components at +4/+8/+16 bytes from the vert base. */
		vuint32m1_t b_off = __riscv_vmul_vx_u32m1( v2_idx, vs, vl );
		vuint32m1_t c_off = __riscv_vmul_vx_u32m1( v3_idx, vs, vl );

		vfloat32m1_t bx = __riscv_vluxei32_v_f32m1( vf, b_off, vl );
		vfloat32m1_t by = __riscv_vluxei32_v_f32m1( vf, __riscv_vadd_vx_u32m1( b_off, sizeof( float ), vl ), vl );
		vfloat32m1_t bz = __riscv_vluxei32_v_f32m1( vf, __riscv_vadd_vx_u32m1( b_off, 2 * sizeof( float ), vl ), vl );
		vfloat32m1_t bs1 = __riscv_vluxei32_v_f32m1( vf, __riscv_vadd_vx_u32m1( b_off, 4 * sizeof( float ), vl ), vl );

		vfloat32m1_t cx = __riscv_vluxei32_v_f32m1( vf, c_off, vl );
		vfloat32m1_t cy = __riscv_vluxei32_v_f32m1( vf, __riscv_vadd_vx_u32m1( c_off, sizeof( float ), vl ), vl );
		vfloat32m1_t cz = __riscv_vluxei32_v_f32m1( vf, __riscv_vadd_vx_u32m1( c_off, 2 * sizeof( float ), vl ), vl );
		vfloat32m1_t cs1 = __riscv_vluxei32_v_f32m1( vf, __riscv_vadd_vx_u32m1( c_off, 4 * sizeof( float ), vl ), vl );

		vfloat32m1_t ax = __riscv_vlse32_v_f32m1( &verts[i].xyz[0], vs, vl );
		vfloat32m1_t ay = __riscv_vlse32_v_f32m1( &verts[i].xyz[1], vs, vl );
		vfloat32m1_t az = __riscv_vlse32_v_f32m1( &verts[i].xyz[2], vs, vl );
		vfloat32m1_t as1 = __riscv_vlse32_v_f32m1( &verts[i].st[1], vs, vl );

		vfloat32m1_t s0 = __riscv_vlse32_v_f32m1( &dominantTris[i].normalizationScale[0], ds, vl );
		vfloat32m1_t s1 = __riscv_vlse32_v_f32m1( &dominantTris[i].normalizationScale[1], ds, vl );
		vfloat32m1_t s2 = __riscv_vlse32_v_f32m1( &dominantTris[i].normalizationScale[2], ds, vl );

		vfloat32m1_t vx = __riscv_vfsub_vv_f32m1( bx, ax, vl );
		vfloat32m1_t vy = __riscv_vfsub_vv_f32m1( by, ay, vl );
		vfloat32m1_t vz = __riscv_vfsub_vv_f32m1( bz, az, vl );
		vfloat32m1_t st_b_diff = __riscv_vfsub_vv_f32m1( bs1, as1, vl );

		vfloat32m1_t ux = __riscv_vfsub_vv_f32m1( cx, ax, vl );
		vfloat32m1_t uy = __riscv_vfsub_vv_f32m1( cy, ay, vl );
		vfloat32m1_t uz = __riscv_vfsub_vv_f32m1( cz, az, vl );
		vfloat32m1_t st_c_diff = __riscv_vfsub_vv_f32m1( cs1, as1, vl );

		vfloat32m1_t n0 = __riscv_vfmul_vv_f32m1( uy, vz, vl );
		n0 = __riscv_vfnmsac_vv_f32m1( n0, uz, vy, vl );
		n0 = __riscv_vfmul_vv_f32m1( n0, s2, vl );

		vfloat32m1_t n1 = __riscv_vfmul_vv_f32m1( uz, vx, vl );
		n1 = __riscv_vfnmsac_vv_f32m1( n1, ux, vz, vl );
		n1 = __riscv_vfmul_vv_f32m1( n1, s2, vl );

		vfloat32m1_t n2 = __riscv_vfmul_vv_f32m1( ux, vy, vl );
		n2 = __riscv_vfnmsac_vv_f32m1( n2, uy, vx, vl );
		n2 = __riscv_vfmul_vv_f32m1( n2, s2, vl );

		vfloat32m1_t t0 = __riscv_vfmul_vv_f32m1( vx, st_c_diff, vl );
		t0 = __riscv_vfnmsac_vv_f32m1( t0, st_b_diff, ux, vl );
		t0 = __riscv_vfmul_vv_f32m1( t0, s0, vl );

		vfloat32m1_t t1 = __riscv_vfmul_vv_f32m1( vy, st_c_diff, vl );
		t1 = __riscv_vfnmsac_vv_f32m1( t1, st_b_diff, uy, vl );
		t1 = __riscv_vfmul_vv_f32m1( t1, s0, vl );

		vfloat32m1_t t2 = __riscv_vfmul_vv_f32m1( vz, st_c_diff, vl );
		t2 = __riscv_vfnmsac_vv_f32m1( t2, st_b_diff, uz, vl );
		t2 = __riscv_vfmul_vv_f32m1( t2, s0, vl );

		/* bitangent = s1 * (normal x tangent): t3=n2*t1-n1*t2, t4=n0*t2-n2*t0,
		 * t5=n1*t0-n0*t1 (matches generic; the swapped operands negated the handedness). */
		vfloat32m1_t t3 = __riscv_vfmul_vv_f32m1( n2, t1, vl );
		t3 = __riscv_vfnmsac_vv_f32m1( t3, n1, t2, vl );
		t3 = __riscv_vfmul_vv_f32m1( t3, s1, vl );

		vfloat32m1_t t4 = __riscv_vfmul_vv_f32m1( n0, t2, vl );
		t4 = __riscv_vfnmsac_vv_f32m1( t4, n2, t0, vl );
		t4 = __riscv_vfmul_vv_f32m1( t4, s1, vl );

		vfloat32m1_t t5 = __riscv_vfmul_vv_f32m1( n1, t0, vl );
		t5 = __riscv_vfnmsac_vv_f32m1( t5, n0, t1, vl );
		t5 = __riscv_vfmul_vv_f32m1( t5, s1, vl );

		__riscv_vsse32_v_f32m1( &verts[i].normal[0], vs, n0, vl );
		__riscv_vsse32_v_f32m1( &verts[i].normal[1], vs, n1, vl );
		__riscv_vsse32_v_f32m1( &verts[i].normal[2], vs, n2, vl );

		__riscv_vsse32_v_f32m1( &verts[i].tangents[0][0], vs, t0, vl );
		__riscv_vsse32_v_f32m1( &verts[i].tangents[0][1], vs, t1, vl );
		__riscv_vsse32_v_f32m1( &verts[i].tangents[0][2], vs, t2, vl );

		__riscv_vsse32_v_f32m1( &verts[i].tangents[1][0], vs, t3, vl );
		__riscv_vsse32_v_f32m1( &verts[i].tangents[1][1], vs, t4, vl );
		__riscv_vsse32_v_f32m1( &verts[i].tangents[1][2], vs, t5, vl );

		i += vl;
	}
}

void VPCALL idSIMD_RVV::BlendJoints( idJointQuat *joints, const idJointQuat *blendJoints, const float lerp, const int *index, const int numJoints ) {
	static const float TWO_PI = 6.28318530718f;
	static const float PI = 3.14159265359f;
	static const float HALF_PI = 1.57079632679f;
	const int js = sizeof( idJointQuat );	// BYTE stride: vluxei/vsoxei indexes are byte offsets, not element counts
	const float *jf = (const float *)joints;
	const float *bf = (const float *)blendJoints;

	if ( lerp <= 0.0f ) { return; }

	if ( lerp >= 1.0f ) {
		int i = 0;
		while ( i < numJoints ) {
			size_t vl = __riscv_vsetvl_e32m1( numJoints - i );
			vuint32m1_t idx = __riscv_vlse32_v_u32m1( (const unsigned int *)( index + i ), sizeof( unsigned int ), vl );
			vuint32m1_t off = __riscv_vmul_vx_u32m1( idx, js, vl );
			for ( int c = 0; c < 7; c++ ) {
				vuint32m1_t co = __riscv_vadd_vx_u32m1( off, c * sizeof( float ), vl );
				vfloat32m1_t v = __riscv_vluxei32_v_f32m1( bf, co, vl );
				__riscv_vsoxei32_v_f32m1( (float *)joints, co, v, vl );
			}
			i += vl;
		}
		return;
	}

	float lerp_m1 = 1.0f - lerp;
	int i = 0;
	while ( i < numJoints ) {
		size_t vl = __riscv_vsetvl_e32m1( numJoints - i );

		vuint32m1_t idx = __riscv_vlse32_v_u32m1( (const unsigned int *)( index + i ), sizeof( unsigned int ), vl );
		vuint32m1_t off = __riscv_vmul_vx_u32m1( idx, js, vl );

		vuint32m1_t o0 = off;
		vuint32m1_t o1 = __riscv_vadd_vx_u32m1( off, 1 * sizeof( float ), vl );
		vuint32m1_t o2 = __riscv_vadd_vx_u32m1( off, 2 * sizeof( float ), vl );
		vuint32m1_t o3 = __riscv_vadd_vx_u32m1( off, 3 * sizeof( float ), vl );
		vuint32m1_t o4 = __riscv_vadd_vx_u32m1( off, 4 * sizeof( float ), vl );
		vuint32m1_t o5 = __riscv_vadd_vx_u32m1( off, 5 * sizeof( float ), vl );
		vuint32m1_t o6 = __riscv_vadd_vx_u32m1( off, 6 * sizeof( float ), vl );

		vfloat32m1_t f0 = __riscv_vluxei32_v_f32m1( jf, o0, vl );
		vfloat32m1_t f1 = __riscv_vluxei32_v_f32m1( jf, o1, vl );
		vfloat32m1_t f2 = __riscv_vluxei32_v_f32m1( jf, o2, vl );
		vfloat32m1_t f3 = __riscv_vluxei32_v_f32m1( jf, o3, vl );

		vfloat32m1_t t0 = __riscv_vluxei32_v_f32m1( bf, o0, vl );
		vfloat32m1_t t1 = __riscv_vluxei32_v_f32m1( bf, o1, vl );
		vfloat32m1_t t2 = __riscv_vluxei32_v_f32m1( bf, o2, vl );
		vfloat32m1_t t3 = __riscv_vluxei32_v_f32m1( bf, o3, vl );

		vfloat32m1_t cosom = __riscv_vfmul_vv_f32m1( f0, t0, vl );
		cosom = __riscv_vfmacc_vv_f32m1( cosom, f1, t1, vl );
		cosom = __riscv_vfmacc_vv_f32m1( cosom, f2, t2, vl );
		cosom = __riscv_vfmacc_vv_f32m1( cosom, f3, t3, vl );

		vbool32_t neg_cosom = __riscv_vmflt_vf_f32m1_b32( cosom, 0.0f, vl );
		vfloat32m1_t cn = __riscv_vfneg_v_f32m1( cosom, vl );
		cosom = __riscv_vmerge_vvm_f32m1( cosom, cn, neg_cosom, vl );

		vfloat32m1_t tn0 = __riscv_vfneg_v_f32m1( t0, vl );
		vfloat32m1_t tn1 = __riscv_vfneg_v_f32m1( t1, vl );
		vfloat32m1_t tn2 = __riscv_vfneg_v_f32m1( t2, vl );
		vfloat32m1_t tn3 = __riscv_vfneg_v_f32m1( t3, vl );
		t0 = __riscv_vmerge_vvm_f32m1( t0, tn0, neg_cosom, vl );
		t1 = __riscv_vmerge_vvm_f32m1( t1, tn1, neg_cosom, vl );
		t2 = __riscv_vmerge_vvm_f32m1( t2, tn2, neg_cosom, vl );
		t3 = __riscv_vmerge_vvm_f32m1( t3, tn3, neg_cosom, vl );

		vfloat32m1_t one_mcos = __riscv_vfrsub_vf_f32m1( cosom, 1.0f, vl );
		vbool32_t use_trig = __riscv_vmfgt_vf_f32m1_b32( one_mcos, 1e-6f, vl );

		float lerp_v = lerp;
		float lerpm1_v = lerp_m1;

		vfloat32m1_t tr_s0, tr_s1;

		{ // trig path (computed for all, merged later)
			vfloat32m1_t sc = __riscv_vfmul_vv_f32m1( cosom, cosom, vl );
			sc = __riscv_vfrsub_vf_f32m1( sc, 1.0f, vl );

			// Guard vfrsqrt7(+0) = +inf -> 0*inf = NaN (parallel vectors).
			sc = __riscv_vfmax_vf_f32m1( sc, 1.17549435e-38f, vl );
			vfloat32m1_t si = __riscv_vfrsqrt7_v_f32m1( sc, vl );
			vfloat32m1_t nr = __riscv_vfmul_vv_f32m1( sc, si, vl );
			nr = __riscv_vfmul_vv_f32m1( nr, si, vl );
			nr = __riscv_vfrsub_vf_f32m1( nr, 3.0f, vl );
			si = __riscv_vfmul_vv_f32m1( si, nr, vl );
			si = __riscv_vfmul_vf_f32m1( si, 0.5f, vl );
			/* second Newton step: vfrsqrt7 (7-bit) + one step is ~9e-5, over the test's
			 * 1e-5 bar; slerp's sinom scales the blend weights directly, so match
			 * idMath::InvSqrt (table + 2 steps) precision. */
			vfloat32m1_t nr2 = __riscv_vfmul_vv_f32m1( sc, si, vl );
			nr2 = __riscv_vfmul_vv_f32m1( nr2, si, vl );
			nr2 = __riscv_vfrsub_vf_f32m1( nr2, 3.0f, vl );
			si = __riscv_vfmul_vv_f32m1( si, nr2, vl );
			si = __riscv_vfmul_vf_f32m1( si, 0.5f, vl );

			vfloat32m1_t y = __riscv_vfmul_vv_f32m1( sc, si, vl );
			vfloat32m1_t x = cosom;

			{ // atan2(y, x) — compute omega
				vfloat32m1_t ay = __riscv_vfabs_v_f32m1( y, vl );
				vfloat32m1_t ax = __riscv_vfabs_v_f32m1( x, vl );
				vbool32_t swap = __riscv_vmfgt_vv_f32m1_b32( ay, ax, vl );

				vfloat32m1_t numa = __riscv_vmerge_vvm_f32m1( y, x, swap, vl );
				vfloat32m1_t dena = __riscv_vmerge_vvm_f32m1( x, y, swap, vl );
				vfloat32m1_t a = __riscv_vfdiv_vv_f32m1( numa, dena, vl );

				vfloat32m1_t ss = __riscv_vfmul_vv_f32m1( a, a, vl );
				vfloat32m1_t pp = __riscv_vfmul_vf_f32m1( ss, 0.0028662257f, vl );
				pp = __riscv_vfadd_vf_f32m1( pp, -0.0161657367f, vl );
				pp = __riscv_vfmul_vv_f32m1( pp, ss, vl );
				pp = __riscv_vfadd_vf_f32m1( pp, 0.0429096138f, vl );
				pp = __riscv_vfmul_vv_f32m1( pp, ss, vl );
				pp = __riscv_vfadd_vf_f32m1( pp, -0.0752896400f, vl );
				pp = __riscv_vfmul_vv_f32m1( pp, ss, vl );
				pp = __riscv_vfadd_vf_f32m1( pp, 0.1065626393f, vl );
				pp = __riscv_vfmul_vv_f32m1( pp, ss, vl );
				pp = __riscv_vfadd_vf_f32m1( pp, -0.1420889944f, vl );
				pp = __riscv_vfmul_vv_f32m1( pp, ss, vl );
				pp = __riscv_vfadd_vf_f32m1( pp, 0.1999355085f, vl );
				pp = __riscv_vfmul_vv_f32m1( pp, ss, vl );
				pp = __riscv_vfadd_vf_f32m1( pp, -0.3333314528f, vl );
				pp = __riscv_vfmul_vv_f32m1( pp, ss, vl );

				vfloat32m1_t pa = __riscv_vfmul_vv_f32m1( __riscv_vfadd_vf_f32m1( pp, 1.0f, vl ), a, vl );

				vfloat32m1_t result_swapped = __riscv_vfneg_v_f32m1( pa, vl );
				vbool32_t a_neg = __riscv_vmflt_vf_f32m1_b32( a, 0.0f, vl );
				vfloat32m1_t adj = __riscv_vmerge_vvm_f32m1(
					__riscv_vfmv_v_f_f32m1( HALF_PI, vl ),
					__riscv_vfmv_v_f_f32m1( -HALF_PI, vl ),
					a_neg, vl );
				result_swapped = __riscv_vfadd_vv_f32m1( result_swapped, adj, vl );

				vfloat32m1_t result_direct = pa;
				vfloat32m1_t omega = __riscv_vmerge_vvm_f32m1( result_direct, result_swapped, swap, vl );

				{ // sin((1-t)*omega) and sin(t*omega)
					vfloat32m1_t sx_a = __riscv_vfmul_vf_f32m1( omega, lerpm1_v, vl );
					vfloat32m1_t cx_a = __riscv_vfmul_vf_f32m1( omega, lerp_v, vl );

					auto sin16 = [&]( vfloat32m1_t a_in, size_t vli ) -> vfloat32m1_t {
						vfloat32m1_t ared = a_in;
						vbool32_t n2 = __riscv_vmflt_vf_f32m1_b32( ared, 0.0f, vli );
						ared = __riscv_vfadd_vv_f32m1( ared,
							__riscv_vmerge_vvm_f32m1( __riscv_vfmv_v_f_f32m1( 0.0f, vli ),
								__riscv_vfmv_v_f_f32m1( TWO_PI, vli ), n2, vli ), vli );

						vbool32_t ii = __riscv_vmflt_vf_f32m1_b32( ared, PI, vli );
						vfloat32m1_t pi_ma = __riscv_vfrsub_vf_f32m1( ared, PI, vli );
						ared = __riscv_vmerge_vvm_f32m1( ared, pi_ma, ii, vli );
						vbool32_t iii = __riscv_vmfgt_vf_f32m1_b32( ared, HALF_PI, vli );
						pi_ma = __riscv_vfrsub_vf_f32m1( ared, PI, vli );
						ared = __riscv_vmerge_vvm_f32m1( ared, pi_ma, iii, vli );

						vfloat32m1_t ss_ = __riscv_vfmul_vv_f32m1( ared, ared, vli );
						vfloat32m1_t rr = __riscv_vfmul_vf_f32m1( ss_, -2.39e-08f, vli );
						rr = __riscv_vfadd_vf_f32m1( rr, 2.7526e-06f, vli );
						rr = __riscv_vfmul_vv_f32m1( rr, ss_, vli );
						rr = __riscv_vfadd_vf_f32m1( rr, -1.98409e-04f, vli );
						rr = __riscv_vfmul_vv_f32m1( rr, ss_, vli );
						rr = __riscv_vfadd_vf_f32m1( rr, 8.3333315e-03f, vli );
						rr = __riscv_vfmul_vv_f32m1( rr, ss_, vli );
						rr = __riscv_vfadd_vf_f32m1( rr, -1.666666664e-01f, vli );
						rr = __riscv_vfmul_vv_f32m1( rr, ss_, vli );
						rr = __riscv_vfadd_vf_f32m1( rr, 1.0f, vli );
						return __riscv_vfmul_vv_f32m1( ared, rr, vli );
					};

					vfloat32m1_t s0_sin = sin16( sx_a, vl );
					vfloat32m1_t s1_sin = sin16( cx_a, vl );

					tr_s0 = __riscv_vfmul_vv_f32m1( s0_sin, si, vl );
					tr_s1 = __riscv_vfmul_vv_f32m1( s1_sin, si, vl );
				}
			}
		}

		vfloat32m1_t lin_s0 = __riscv_vfmv_v_f_f32m1( lerpm1_v, vl );
		vfloat32m1_t lin_s1 = __riscv_vfmv_v_f_f32m1( lerp_v, vl );

		vfloat32m1_t scale0 = __riscv_vmerge_vvm_f32m1( lin_s0, tr_s0, use_trig, vl );
		vfloat32m1_t scale1 = __riscv_vmerge_vvm_f32m1( lin_s1, tr_s1, use_trig, vl );

		vfloat32m1_t r0 = __riscv_vfmacc_vv_f32m1( __riscv_vfmul_vv_f32m1( f0, scale0, vl ), t0, scale1, vl );
		vfloat32m1_t r1 = __riscv_vfmacc_vv_f32m1( __riscv_vfmul_vv_f32m1( f1, scale0, vl ), t1, scale1, vl );
		vfloat32m1_t r2 = __riscv_vfmacc_vv_f32m1( __riscv_vfmul_vv_f32m1( f2, scale0, vl ), t2, scale1, vl );
		vfloat32m1_t r3 = __riscv_vfmacc_vv_f32m1( __riscv_vfmul_vv_f32m1( f3, scale0, vl ), t3, scale1, vl );

		__riscv_vsoxei32_v_f32m1( (float *)joints, o0, r0, vl );
		__riscv_vsoxei32_v_f32m1( (float *)joints, o1, r1, vl );
		__riscv_vsoxei32_v_f32m1( (float *)joints, o2, r2, vl );
		__riscv_vsoxei32_v_f32m1( (float *)joints, o3, r3, vl );

		{ // translation Lerp
			vfloat32m1_t ft0 = __riscv_vluxei32_v_f32m1( jf, o4, vl );
			vfloat32m1_t ft1 = __riscv_vluxei32_v_f32m1( jf, o5, vl );
			vfloat32m1_t ft2 = __riscv_vluxei32_v_f32m1( jf, o6, vl );

			vfloat32m1_t bt0 = __riscv_vluxei32_v_f32m1( bf, o4, vl );
			vfloat32m1_t bt1 = __riscv_vluxei32_v_f32m1( bf, o5, vl );
			vfloat32m1_t bt2 = __riscv_vluxei32_v_f32m1( bf, o6, vl );

			vfloat32m1_t d0 = __riscv_vfsub_vv_f32m1( bt0, ft0, vl );
			vfloat32m1_t rt0 = __riscv_vfmacc_vf_f32m1( ft0, lerp, d0, vl );
			vfloat32m1_t d1 = __riscv_vfsub_vv_f32m1( bt1, ft1, vl );
			vfloat32m1_t rt1 = __riscv_vfmacc_vf_f32m1( ft1, lerp, d1, vl );
			vfloat32m1_t d2 = __riscv_vfsub_vv_f32m1( bt2, ft2, vl );
			vfloat32m1_t rt2 = __riscv_vfmacc_vf_f32m1( ft2, lerp, d2, vl );

			__riscv_vsoxei32_v_f32m1( (float *)joints, o4, rt0, vl );
			__riscv_vsoxei32_v_f32m1( (float *)joints, o5, rt1, vl );
			__riscv_vsoxei32_v_f32m1( (float *)joints, o6, rt2, vl );
		}
		i += vl;
	}
}

void VPCALL idSIMD_RVV::ConvertJointQuatsToJointMats( idJointMat *jointMats, const idJointQuat *jointQuats, const int numJoints ) {
	const int qs = sizeof( idJointQuat ) / sizeof( float );	// 7
	const int ms = sizeof( idJointMat ) / sizeof( float );		// 12
	const float *qf = (const float *)jointQuats;
	float *mf = (float *)jointMats;
	int i = 0;
	while ( i < numJoints ) {
		size_t vl = __riscv_vsetvl_e32m1( numJoints - i );
		vfloat32m1_t qx = __riscv_vlse32_v_f32m1( qf + i*qs + 0, qs * sizeof( float ), vl );
		vfloat32m1_t qy = __riscv_vlse32_v_f32m1( qf + i*qs + 1, qs * sizeof( float ), vl );
		vfloat32m1_t qz = __riscv_vlse32_v_f32m1( qf + i*qs + 2, qs * sizeof( float ), vl );
		vfloat32m1_t qw = __riscv_vlse32_v_f32m1( qf + i*qs + 3, qs * sizeof( float ), vl );
		vfloat32m1_t tx = __riscv_vlse32_v_f32m1( qf + i*qs + 4, qs * sizeof( float ), vl );
		vfloat32m1_t ty = __riscv_vlse32_v_f32m1( qf + i*qs + 5, qs * sizeof( float ), vl );
		vfloat32m1_t tz = __riscv_vlse32_v_f32m1( qf + i*qs + 6, qs * sizeof( float ), vl );

		vfloat32m1_t w2 = __riscv_vfmul_vv_f32m1( qx, qx, vl );
		w2 = __riscv_vfmacc_vv_f32m1( w2, qy, qy, vl );
		w2 = __riscv_vfmacc_vv_f32m1( w2, qz, qz, vl );
		w2 = __riscv_vfmacc_vv_f32m1( w2, qw, qw, vl );
		vfloat32m1_t s = __riscv_vfrdiv_vf_f32m1( w2, 2.0f, vl );
		vbool32_t ok = __riscv_vmfgt_vf_f32m1_b32( w2, 0.0f, vl );
		s = __riscv_vmerge_vvm_f32m1( __riscv_vfmv_v_f_f32m1( 0.0f, vl ), s, ok, vl );

		vfloat32m1_t xs = __riscv_vfmul_vv_f32m1( qx, s, vl );
		vfloat32m1_t ys = __riscv_vfmul_vv_f32m1( qy, s, vl );
		vfloat32m1_t zs = __riscv_vfmul_vv_f32m1( qz, s, vl );

		vfloat32m1_t wx = __riscv_vfmul_vv_f32m1( qw, xs, vl );
		vfloat32m1_t wy = __riscv_vfmul_vv_f32m1( qw, ys, vl );
		vfloat32m1_t wz = __riscv_vfmul_vv_f32m1( qw, zs, vl );

		vfloat32m1_t xx = __riscv_vfmul_vv_f32m1( qx, xs, vl );
		vfloat32m1_t xy = __riscv_vfmul_vv_f32m1( qx, ys, vl );
		vfloat32m1_t xz = __riscv_vfmul_vv_f32m1( qx, zs, vl );

		vfloat32m1_t yy = __riscv_vfmul_vv_f32m1( qy, ys, vl );
		vfloat32m1_t yz = __riscv_vfmul_vv_f32m1( qy, zs, vl );

		vfloat32m1_t zz = __riscv_vfmul_vv_f32m1( qz, zs, vl );

		vfloat32m1_t one = __riscv_vfmv_v_f_f32m1( 1.0f, vl );

		/* idJointMat stores R^T (SetRotation transposes q.ToMat3()): jointMat[row*4+col]
		 * = R[col][row]. So every off-diagonal w-term is negated vs the plain R matrix. */
		vfloat32m1_t col0x = __riscv_vfsub_vv_f32m1( one, __riscv_vfadd_vv_f32m1( yy, zz, vl ), vl );
		vfloat32m1_t col1x = __riscv_vfadd_vv_f32m1( xy, wz, vl );
		vfloat32m1_t col2x = __riscv_vfsub_vv_f32m1( xz, wy, vl );

		vfloat32m1_t col0y = __riscv_vfsub_vv_f32m1( xy, wz, vl );
		vfloat32m1_t col1y = __riscv_vfsub_vv_f32m1( one, __riscv_vfadd_vv_f32m1( xx, zz, vl ), vl );
		vfloat32m1_t col2y = __riscv_vfadd_vv_f32m1( yz, wx, vl );

		vfloat32m1_t col0z = __riscv_vfadd_vv_f32m1( xz, wy, vl );
		vfloat32m1_t col1z = __riscv_vfsub_vv_f32m1( yz, wx, vl );
		vfloat32m1_t col2z = __riscv_vfsub_vv_f32m1( one, __riscv_vfadd_vv_f32m1( xx, yy, vl ), vl );

		ptrdiff_t stride = ms * sizeof( float );
		__riscv_vsse32_v_f32m1( mf + i*ms + 0,  stride, col0x, vl );
		__riscv_vsse32_v_f32m1( mf + i*ms + 1,  stride, col1x, vl );
		__riscv_vsse32_v_f32m1( mf + i*ms + 2,  stride, col2x, vl );
		__riscv_vsse32_v_f32m1( mf + i*ms + 3,  stride, tx,    vl );
		__riscv_vsse32_v_f32m1( mf + i*ms + 4,  stride, col0y, vl );
		__riscv_vsse32_v_f32m1( mf + i*ms + 5,  stride, col1y, vl );
		__riscv_vsse32_v_f32m1( mf + i*ms + 6,  stride, col2y, vl );
		__riscv_vsse32_v_f32m1( mf + i*ms + 7,  stride, ty,    vl );
		__riscv_vsse32_v_f32m1( mf + i*ms + 8,  stride, col0z, vl );
		__riscv_vsse32_v_f32m1( mf + i*ms + 9,  stride, col1z, vl );
		__riscv_vsse32_v_f32m1( mf + i*ms + 10, stride, col2z, vl );
		__riscv_vsse32_v_f32m1( mf + i*ms + 11, stride, tz,    vl );
		i += vl;
	}
}

void VPCALL idSIMD_RVV::ConvertJointMatsToJointQuats( idJointQuat *jointQuats, const idJointMat *jointMats, const int numJoints ) {
	idSIMD_Generic::ConvertJointMatsToJointQuats( jointQuats, jointMats, numJoints );
}

void VPCALL idSIMD_RVV::TransformJoints( idJointMat *jointMats, const int *parents, const int firstJoint, const int lastJoint ) {
	int i;
	for ( i = firstJoint; i <= lastJoint; i++ ) {
		assert( parents[i] < i );
		float *child = jointMats[i].ToFloatPtr();
		const float *parent = jointMats[parents[i]].ToFloatPtr();

		vfloat32m1_t cr0 = __riscv_vle32_v_f32m1( child,     4 );
		vfloat32m1_t cr1 = __riscv_vle32_v_f32m1( child + 4, 4 );
		vfloat32m1_t cr2 = __riscv_vle32_v_f32m1( child + 8, 4 );

		for ( int r = 0; r < 3; r++ ) {
			vfloat32m1_t vr = __riscv_vfmul_vf_f32m1( cr0, parent[r * 4 + 0], 4 );
			vr = __riscv_vfmacc_vf_f32m1( vr, parent[r * 4 + 1], cr1, 4 );
			vr = __riscv_vfmacc_vf_f32m1( vr, parent[r * 4 + 2], cr2, 4 );
			/* store width 4 (not 3): lane 3 is the parent-rotated child translation
			 * (m[0][3]*a[r][0]+m[1][3]*a[r][1]+m[2][3]*a[r][2]); the next line then adds
			 * the parent translation, matching idJointMat::operator*=. */
			__riscv_vse32_v_f32m1( child + r * 4, vr, 4 );
			child[r * 4 + 3] += parent[r * 4 + 3];
		}
	}
}

void VPCALL idSIMD_RVV::UntransformJoints( idJointMat *jointMats, const int *parents, const int firstJoint, const int lastJoint ) {
	int i;
	for ( i = lastJoint; i >= firstJoint; i-- ) {
		assert( parents[i] < i );
		float *child = jointMats[i].ToFloatPtr();
		const float *parent = jointMats[parents[i]].ToFloatPtr();

		child[3]  -= parent[3];
		child[7]  -= parent[7];
		child[11] -= parent[11];

		vfloat32m1_t cr0 = __riscv_vle32_v_f32m1( child,     4 );
		vfloat32m1_t cr1 = __riscv_vle32_v_f32m1( child + 4, 4 );
		vfloat32m1_t cr2 = __riscv_vle32_v_f32m1( child + 8, 4 );

		for ( int r = 0; r < 3; r++ ) {
			vfloat32m1_t vr = __riscv_vfmul_vf_f32m1( cr0, parent[r], 4 );
			vr = __riscv_vfmacc_vf_f32m1( vr, parent[r + 4], cr1, 4 );
			vr = __riscv_vfmacc_vf_f32m1( vr, parent[r + 8], cr2, 4 );
			__riscv_vse32_v_f32m1( child + r * 4, vr, 4 );
		}
	}
}

void VPCALL idSIMD_RVV::TransformVerts( idDrawVert *verts, const int numVerts, const idJointMat *joints, const idVec4 *weights, const int *index, const int numWeights ) {
	idSIMD_Generic::TransformVerts( verts, numVerts, joints, weights, index, numWeights );
}

void VPCALL idSIMD_RVV::CreateTextureSpaceLightVectors( idVec3 *lightVectors, const idVec3 &lightOrigin, const idDrawVert *verts, const int numVerts, const int *indexes, const int numIndexes ) {
	const int vs = sizeof( idDrawVert );
	int i = 0;
	while ( i < numVerts ) {
		const idDrawVert *v = verts + i;
		size_t vl = __riscv_vsetvl_e32m1( numVerts - i );

		vfloat32m1_t vx = __riscv_vlse32_v_f32m1( &v->xyz.x, vs, vl );
		vfloat32m1_t vy = __riscv_vlse32_v_f32m1( &v->xyz.y, vs, vl );
		vfloat32m1_t vz = __riscv_vlse32_v_f32m1( &v->xyz.z, vs, vl );

		vfloat32m1_t tx0 = __riscv_vlse32_v_f32m1( &v->tangents[0].x, vs, vl );
		vfloat32m1_t tx1 = __riscv_vlse32_v_f32m1( &v->tangents[0].y, vs, vl );
		vfloat32m1_t tx2 = __riscv_vlse32_v_f32m1( &v->tangents[0].z, vs, vl );

		vfloat32m1_t bx0 = __riscv_vlse32_v_f32m1( &v->tangents[1].x, vs, vl );
		vfloat32m1_t bx1 = __riscv_vlse32_v_f32m1( &v->tangents[1].y, vs, vl );
		vfloat32m1_t bx2 = __riscv_vlse32_v_f32m1( &v->tangents[1].z, vs, vl );

		vfloat32m1_t nx0 = __riscv_vlse32_v_f32m1( &v->normal.x, vs, vl );
		vfloat32m1_t nx1 = __riscv_vlse32_v_f32m1( &v->normal.y, vs, vl );
		vfloat32m1_t nx2 = __riscv_vlse32_v_f32m1( &v->normal.z, vs, vl );

		vfloat32m1_t dx = __riscv_vfrsub_vf_f32m1( vx, lightOrigin.x, vl );
		vfloat32m1_t dy = __riscv_vfrsub_vf_f32m1( vy, lightOrigin.y, vl );
		vfloat32m1_t dz = __riscv_vfrsub_vf_f32m1( vz, lightOrigin.z, vl );

		vfloat32m1_t r0 = __riscv_vfmul_vv_f32m1( dx, tx0, vl );
		r0 = __riscv_vfmacc_vv_f32m1( r0, dy, tx1, vl );
		r0 = __riscv_vfmacc_vv_f32m1( r0, dz, tx2, vl );

		vfloat32m1_t r1 = __riscv_vfmul_vv_f32m1( dx, bx0, vl );
		r1 = __riscv_vfmacc_vv_f32m1( r1, dy, bx1, vl );
		r1 = __riscv_vfmacc_vv_f32m1( r1, dz, bx2, vl );

		vfloat32m1_t r2 = __riscv_vfmul_vv_f32m1( dx, nx0, vl );
		r2 = __riscv_vfmacc_vv_f32m1( r2, dy, nx1, vl );
		r2 = __riscv_vfmacc_vv_f32m1( r2, dz, nx2, vl );

		__riscv_vsse32_v_f32m1( &lightVectors[i].x, sizeof( idVec3 ), r0, vl );
		__riscv_vsse32_v_f32m1( &lightVectors[i].y, sizeof( idVec3 ), r1, vl );
		__riscv_vsse32_v_f32m1( &lightVectors[i].z, sizeof( idVec3 ), r2, vl );

		i += vl;
	}
}

void VPCALL idSIMD_RVV::CreateSpecularTextureCoords( idVec4 *texCoords, const idVec3 &lightOrigin, const idVec3 &viewOrigin, const idDrawVert *verts, const int numVerts, const int *indexes, const int numIndexes ) {
	const int vs = sizeof( idDrawVert );
	int i = 0;
	while ( i < numVerts ) {
		const idDrawVert *v = verts + i;
		size_t vl = __riscv_vsetvl_e32m1( numVerts - i );

		vfloat32m1_t vx = __riscv_vlse32_v_f32m1( &v->xyz.x, vs, vl );
		vfloat32m1_t vy = __riscv_vlse32_v_f32m1( &v->xyz.y, vs, vl );
		vfloat32m1_t vz = __riscv_vlse32_v_f32m1( &v->xyz.z, vs, vl );

		vfloat32m1_t tx0 = __riscv_vlse32_v_f32m1( &v->tangents[0].x, vs, vl );
		vfloat32m1_t tx1 = __riscv_vlse32_v_f32m1( &v->tangents[0].y, vs, vl );
		vfloat32m1_t tx2 = __riscv_vlse32_v_f32m1( &v->tangents[0].z, vs, vl );

		vfloat32m1_t bx0 = __riscv_vlse32_v_f32m1( &v->tangents[1].x, vs, vl );
		vfloat32m1_t bx1 = __riscv_vlse32_v_f32m1( &v->tangents[1].y, vs, vl );
		vfloat32m1_t bx2 = __riscv_vlse32_v_f32m1( &v->tangents[1].z, vs, vl );

		vfloat32m1_t nx0 = __riscv_vlse32_v_f32m1( &v->normal.x, vs, vl );
		vfloat32m1_t nx1 = __riscv_vlse32_v_f32m1( &v->normal.y, vs, vl );
		vfloat32m1_t nx2 = __riscv_vlse32_v_f32m1( &v->normal.z, vs, vl );

		vfloat32m1_t ldx = __riscv_vfrsub_vf_f32m1( vx, lightOrigin.x, vl );
		vfloat32m1_t ldy = __riscv_vfrsub_vf_f32m1( vy, lightOrigin.y, vl );
		vfloat32m1_t ldz = __riscv_vfrsub_vf_f32m1( vz, lightOrigin.z, vl );

		vfloat32m1_t vdx = __riscv_vfrsub_vf_f32m1( vx, viewOrigin.x, vl );
		vfloat32m1_t vdy = __riscv_vfrsub_vf_f32m1( vy, viewOrigin.y, vl );
		vfloat32m1_t vdz = __riscv_vfrsub_vf_f32m1( vz, viewOrigin.z, vl );

		vfloat32m1_t ls = __riscv_vfmul_vv_f32m1( ldx, ldx, vl );
		ls = __riscv_vfmacc_vv_f32m1( ls, ldy, ldy, vl );
		ls = __riscv_vfmacc_vv_f32m1( ls, ldz, ldz, vl );

		vfloat32m1_t vs_ = __riscv_vfmul_vv_f32m1( vdx, vdx, vl );
		vs_ = __riscv_vfmacc_vv_f32m1( vs_, vdy, vdy, vl );
		vs_ = __riscv_vfmacc_vv_f32m1( vs_, vdz, vdz, vl );

		{
			// Guard vfrsqrt7(+0) = +inf -> inf*inf*0 = NaN (zero-length light dir).
			ls = __riscv_vfmax_vf_f32m1( ls, 1.17549435e-38f, vl );
			vfloat32m1_t rs = __riscv_vfrsqrt7_v_f32m1( ls, vl );
			vfloat32m1_t nr = __riscv_vfmul_vv_f32m1( rs, rs, vl );
			nr = __riscv_vfmul_vv_f32m1( nr, ls, vl );
			nr = __riscv_vfrsub_vf_f32m1( nr, 3.0f, vl );
			rs = __riscv_vfmul_vv_f32m1( rs, nr, vl );
			rs = __riscv_vfmul_vf_f32m1( rs, 0.5f, vl );

			ldx = __riscv_vfmul_vv_f32m1( ldx, rs, vl );
			ldy = __riscv_vfmul_vv_f32m1( ldy, rs, vl );
			ldz = __riscv_vfmul_vv_f32m1( ldz, rs, vl );
		}

		{
			// Guard vfrsqrt7(+0) = +inf -> inf*inf*0 = NaN (zero-length view dir).
			vs_ = __riscv_vfmax_vf_f32m1( vs_, 1.17549435e-38f, vl );
			vfloat32m1_t rs = __riscv_vfrsqrt7_v_f32m1( vs_, vl );
			vfloat32m1_t nr = __riscv_vfmul_vv_f32m1( rs, rs, vl );
			nr = __riscv_vfmul_vv_f32m1( nr, vs_, vl );
			nr = __riscv_vfrsub_vf_f32m1( nr, 3.0f, vl );
			rs = __riscv_vfmul_vv_f32m1( rs, nr, vl );
			rs = __riscv_vfmul_vf_f32m1( rs, 0.5f, vl );

			vdx = __riscv_vfmul_vv_f32m1( vdx, rs, vl );
			vdy = __riscv_vfmul_vv_f32m1( vdy, rs, vl );
			vdz = __riscv_vfmul_vv_f32m1( vdz, rs, vl );
		}

		vfloat32m1_t dx = __riscv_vfadd_vv_f32m1( ldx, vdx, vl );
		vfloat32m1_t dy = __riscv_vfadd_vv_f32m1( ldy, vdy, vl );
		vfloat32m1_t dz = __riscv_vfadd_vv_f32m1( ldz, vdz, vl );

		vfloat32m1_t r0 = __riscv_vfmul_vv_f32m1( dx, tx0, vl );
		r0 = __riscv_vfmacc_vv_f32m1( r0, dy, tx1, vl );
		r0 = __riscv_vfmacc_vv_f32m1( r0, dz, tx2, vl );

		vfloat32m1_t r1 = __riscv_vfmul_vv_f32m1( dx, bx0, vl );
		r1 = __riscv_vfmacc_vv_f32m1( r1, dy, bx1, vl );
		r1 = __riscv_vfmacc_vv_f32m1( r1, dz, bx2, vl );

		vfloat32m1_t r2 = __riscv_vfmul_vv_f32m1( dx, nx0, vl );
		r2 = __riscv_vfmacc_vv_f32m1( r2, dy, nx1, vl );
		r2 = __riscv_vfmacc_vv_f32m1( r2, dz, nx2, vl );

		vfloat32m1_t r3 = __riscv_vfmv_v_f_f32m1( 1.0f, vl );

		__riscv_vsse32_v_f32m1( &texCoords[i].x, sizeof( idVec4 ), r0, vl );
		__riscv_vsse32_v_f32m1( &texCoords[i].y, sizeof( idVec4 ), r1, vl );
		__riscv_vsse32_v_f32m1( &texCoords[i].z, sizeof( idVec4 ), r2, vl );
		__riscv_vsse32_v_f32m1( &texCoords[i].w, sizeof( idVec4 ), r3, vl );

		i += vl;
	}
}

int VPCALL idSIMD_RVV::CreateShadowCache( idVec4 *vertexCache, int *vertRemap, const idVec3 &lightOrigin, const idDrawVert *verts, const int numVerts ) {
	return idSIMD_Generic::CreateShadowCache( vertexCache, vertRemap, lightOrigin, verts, numVerts );
}

void VPCALL idSIMD_RVV::UpSamplePCMTo44kHz( float *dest, const short *src, const int numSamples, const int kHz, const int numChannels ) {
	if ( kHz == 11025 ) {
		if ( numChannels == 1 ) {
			int i = 0;
			while ( i < numSamples ) {
				size_t vl = __riscv_vsetvl_e16m2( numSamples - i );
				vint16m2_t vs = __riscv_vle16_v_i16m2( src + i, vl );
				vfloat32m4_t vf = __riscv_vfwcvt_f_x_v_f32m4( vs, vl );
				__riscv_vsse32_v_f32m4( dest + i*4 + 0, 4 * sizeof( float ), vf, vl );
				__riscv_vsse32_v_f32m4( dest + i*4 + 1, 4 * sizeof( float ), vf, vl );
				__riscv_vsse32_v_f32m4( dest + i*4 + 2, 4 * sizeof( float ), vf, vl );
				__riscv_vsse32_v_f32m4( dest + i*4 + 3, 4 * sizeof( float ), vf, vl );
				i += vl;
			}
		} else {
			int i = 0;
			int pairs = numSamples >> 1;
			while ( i < pairs ) {
				size_t vl = __riscv_vsetvl_e16m2( pairs - i );
				vint16m2_t vL = __riscv_vlse16_v_i16m2( src + 2*i + 0, 2 * sizeof( int16_t ), vl );
				vint16m2_t vR = __riscv_vlse16_v_i16m2( src + 2*i + 1, 2 * sizeof( int16_t ), vl );
				vfloat32m4_t vLf = __riscv_vfwcvt_f_x_v_f32m4( vL, vl );
				vfloat32m4_t vRf = __riscv_vfwcvt_f_x_v_f32m4( vR, vl );
				__riscv_vsse32_v_f32m4( dest + 8*i + 0, 8 * sizeof( float ), vLf, vl );
				__riscv_vsse32_v_f32m4( dest + 8*i + 2, 8 * sizeof( float ), vLf, vl );
				__riscv_vsse32_v_f32m4( dest + 8*i + 4, 8 * sizeof( float ), vLf, vl );
				__riscv_vsse32_v_f32m4( dest + 8*i + 6, 8 * sizeof( float ), vLf, vl );
				__riscv_vsse32_v_f32m4( dest + 8*i + 1, 8 * sizeof( float ), vRf, vl );
				__riscv_vsse32_v_f32m4( dest + 8*i + 3, 8 * sizeof( float ), vRf, vl );
				__riscv_vsse32_v_f32m4( dest + 8*i + 5, 8 * sizeof( float ), vRf, vl );
				__riscv_vsse32_v_f32m4( dest + 8*i + 7, 8 * sizeof( float ), vRf, vl );
				i += vl;
			}
		}
	} else if ( kHz == 22050 ) {
		if ( numChannels == 1 ) {
			int i = 0;
			while ( i < numSamples ) {
				size_t vl = __riscv_vsetvl_e16m2( numSamples - i );
				vint16m2_t vs = __riscv_vle16_v_i16m2( src + i, vl );
				vfloat32m4_t vf = __riscv_vfwcvt_f_x_v_f32m4( vs, vl );
				__riscv_vsse32_v_f32m4( dest + i*2 + 0, 2 * sizeof( float ), vf, vl );
				__riscv_vsse32_v_f32m4( dest + i*2 + 1, 2 * sizeof( float ), vf, vl );
				i += vl;
			}
		} else {
			int i = 0;
			int pairs = numSamples >> 1;
			while ( i < pairs ) {
				size_t vl = __riscv_vsetvl_e16m2( pairs - i );
				vint16m2_t vL = __riscv_vlse16_v_i16m2( src + 2*i + 0, 2 * sizeof( int16_t ), vl );
				vint16m2_t vR = __riscv_vlse16_v_i16m2( src + 2*i + 1, 2 * sizeof( int16_t ), vl );
				vfloat32m4_t vLf = __riscv_vfwcvt_f_x_v_f32m4( vL, vl );
				vfloat32m4_t vRf = __riscv_vfwcvt_f_x_v_f32m4( vR, vl );
				__riscv_vsse32_v_f32m4( dest + 4*i + 0, 4 * sizeof( float ), vLf, vl );
				__riscv_vsse32_v_f32m4( dest + 4*i + 2, 4 * sizeof( float ), vLf, vl );
				__riscv_vsse32_v_f32m4( dest + 4*i + 1, 4 * sizeof( float ), vRf, vl );
				__riscv_vsse32_v_f32m4( dest + 4*i + 3, 4 * sizeof( float ), vRf, vl );
				i += vl;
			}
		}
	} else if ( kHz == 44100 ) {
		int i = 0;
		while ( i < numSamples ) {
			size_t vl = __riscv_vsetvl_e16m2( numSamples - i );
			vint16m2_t vs = __riscv_vle16_v_i16m2( src + i, vl );
			vfloat32m4_t vf = __riscv_vfwcvt_f_x_v_f32m4( vs, vl );
			__riscv_vse32_v_f32m4( dest + i, vf, vl );
			i += vl;
		}
	} else {
		assert( 0 );
	}
}

void VPCALL idSIMD_RVV::UpSampleOGGTo44kHz( float *dest, const float * const *ogg, const int numSamples, const int kHz, const int numChannels ) {
	static const float SCALE = 32768.0f;

	if ( kHz == 11025 ) {
		if ( numChannels == 1 ) {
			int i = 0;
			while ( i < numSamples ) {
				size_t vl = __riscv_vsetvl_e32m4( numSamples - i );
				vfloat32m4_t vf = __riscv_vle32_v_f32m4( ogg[0] + i, vl );
				vf = __riscv_vfmul_vf_f32m4( vf, SCALE, vl );
				__riscv_vsse32_v_f32m4( dest + i*4 + 0, 4 * sizeof( float ), vf, vl );
				__riscv_vsse32_v_f32m4( dest + i*4 + 1, 4 * sizeof( float ), vf, vl );
				__riscv_vsse32_v_f32m4( dest + i*4 + 2, 4 * sizeof( float ), vf, vl );
				__riscv_vsse32_v_f32m4( dest + i*4 + 3, 4 * sizeof( float ), vf, vl );
				i += vl;
			}
		} else {
			int i = 0;
			int pairs = numSamples >> 1;
			while ( i < pairs ) {
				size_t vl = __riscv_vsetvl_e32m4( pairs - i );
				vfloat32m4_t vL = __riscv_vle32_v_f32m4( ogg[0] + i, vl );
				vfloat32m4_t vR = __riscv_vle32_v_f32m4( ogg[1] + i, vl );
				vL = __riscv_vfmul_vf_f32m4( vL, SCALE, vl );
				vR = __riscv_vfmul_vf_f32m4( vR, SCALE, vl );
				__riscv_vsse32_v_f32m4( dest + 8*i + 0, 8 * sizeof( float ), vL, vl );
				__riscv_vsse32_v_f32m4( dest + 8*i + 2, 8 * sizeof( float ), vL, vl );
				__riscv_vsse32_v_f32m4( dest + 8*i + 4, 8 * sizeof( float ), vL, vl );
				__riscv_vsse32_v_f32m4( dest + 8*i + 6, 8 * sizeof( float ), vL, vl );
				__riscv_vsse32_v_f32m4( dest + 8*i + 1, 8 * sizeof( float ), vR, vl );
				__riscv_vsse32_v_f32m4( dest + 8*i + 3, 8 * sizeof( float ), vR, vl );
				__riscv_vsse32_v_f32m4( dest + 8*i + 5, 8 * sizeof( float ), vR, vl );
				__riscv_vsse32_v_f32m4( dest + 8*i + 7, 8 * sizeof( float ), vR, vl );
				i += vl;
			}
		}
	} else if ( kHz == 22050 ) {
		if ( numChannels == 1 ) {
			int i = 0;
			while ( i < numSamples ) {
				size_t vl = __riscv_vsetvl_e32m4( numSamples - i );
				vfloat32m4_t vf = __riscv_vle32_v_f32m4( ogg[0] + i, vl );
				vf = __riscv_vfmul_vf_f32m4( vf, SCALE, vl );
				__riscv_vsse32_v_f32m4( dest + i*2 + 0, 2 * sizeof( float ), vf, vl );
				__riscv_vsse32_v_f32m4( dest + i*2 + 1, 2 * sizeof( float ), vf, vl );
				i += vl;
			}
		} else {
			int i = 0;
			int pairs = numSamples >> 1;
			while ( i < pairs ) {
				size_t vl = __riscv_vsetvl_e32m4( pairs - i );
				vfloat32m4_t vL = __riscv_vle32_v_f32m4( ogg[0] + i, vl );
				vfloat32m4_t vR = __riscv_vle32_v_f32m4( ogg[1] + i, vl );
				vL = __riscv_vfmul_vf_f32m4( vL, SCALE, vl );
				vR = __riscv_vfmul_vf_f32m4( vR, SCALE, vl );
				__riscv_vsse32_v_f32m4( dest + 4*i + 0, 4 * sizeof( float ), vL, vl );
				__riscv_vsse32_v_f32m4( dest + 4*i + 2, 4 * sizeof( float ), vL, vl );
				__riscv_vsse32_v_f32m4( dest + 4*i + 1, 4 * sizeof( float ), vR, vl );
				__riscv_vsse32_v_f32m4( dest + 4*i + 3, 4 * sizeof( float ), vR, vl );
				i += vl;
			}
		}
	} else if ( kHz == 44100 ) {
		if ( numChannels == 1 ) {
			int i = 0;
			while ( i < numSamples ) {
				size_t vl = __riscv_vsetvl_e32m4( numSamples - i );
				vfloat32m4_t vf = __riscv_vle32_v_f32m4( ogg[0] + i, vl );
				vf = __riscv_vfmul_vf_f32m4( vf, SCALE, vl );
				__riscv_vse32_v_f32m4( dest + i, vf, vl );
				i += vl;
			}
		} else {
			int i = 0;
			int pairs = numSamples >> 1;
			while ( i < pairs ) {
				size_t vl = __riscv_vsetvl_e32m4( pairs - i );
				vfloat32m4_t vL = __riscv_vle32_v_f32m4( ogg[0] + i, vl );
				vfloat32m4_t vR = __riscv_vle32_v_f32m4( ogg[1] + i, vl );
				vL = __riscv_vfmul_vf_f32m4( vL, SCALE, vl );
				vR = __riscv_vfmul_vf_f32m4( vR, SCALE, vl );
				__riscv_vsse32_v_f32m4( dest + 2*i + 0, 2 * sizeof( float ), vL, vl );
				__riscv_vsse32_v_f32m4( dest + 2*i + 1, 2 * sizeof( float ), vR, vl );
				i += vl;
			}
		}
	} else {
		assert( 0 );
	}
}

void VPCALL idSIMD_RVV::MixSoundSixSpeakerMono( float *mixBuffer, const float *samples, const int numSamples, const float lastV[6], const float currentV[6] ) {
	float sL0 = lastV[0];
	float sL1 = lastV[1];
	float sL2 = lastV[2];
	float sL3 = lastV[3];
	float sL4 = lastV[4];
	float sL5 = lastV[5];
	float incL0 = ( currentV[0] - lastV[0] ) / (float)MIXBUFFER_SAMPLES;
	float incL1 = ( currentV[1] - lastV[1] ) / (float)MIXBUFFER_SAMPLES;
	float incL2 = ( currentV[2] - lastV[2] ) / (float)MIXBUFFER_SAMPLES;
	float incL3 = ( currentV[3] - lastV[3] ) / (float)MIXBUFFER_SAMPLES;
	float incL4 = ( currentV[4] - lastV[4] ) / (float)MIXBUFFER_SAMPLES;
	float incL5 = ( currentV[5] - lastV[5] ) / (float)MIXBUFFER_SAMPLES;
	assert( numSamples == MIXBUFFER_SAMPLES );
	int j = 0;
	while ( j < MIXBUFFER_SAMPLES ) {
		size_t vl = __riscv_vsetvl_e32m2( MIXBUFFER_SAMPLES - j );
		vuint32m2_t idx = __riscv_vid_v_u32m2( vl );
		idx = __riscv_vadd_vx_u32m2( idx, j, vl );
		vfloat32m2_t idx_f = __riscv_vfcvt_f_xu_v_f32m2( idx, vl );
		vfloat32m2_t vol0 = __riscv_vfmv_v_f_f32m2( sL0, vl );
		vol0 = __riscv_vfmacc_vf_f32m2( vol0, incL0, idx_f, vl );
		vfloat32m2_t vol1 = __riscv_vfmv_v_f_f32m2( sL1, vl );
		vol1 = __riscv_vfmacc_vf_f32m2( vol1, incL1, idx_f, vl );
		vfloat32m2_t vol2 = __riscv_vfmv_v_f_f32m2( sL2, vl );
		vol2 = __riscv_vfmacc_vf_f32m2( vol2, incL2, idx_f, vl );
		vfloat32m2_t vol3 = __riscv_vfmv_v_f_f32m2( sL3, vl );
		vol3 = __riscv_vfmacc_vf_f32m2( vol3, incL3, idx_f, vl );
		vfloat32m2_t vol4 = __riscv_vfmv_v_f_f32m2( sL4, vl );
		vol4 = __riscv_vfmacc_vf_f32m2( vol4, incL4, idx_f, vl );
		vfloat32m2_t vol5 = __riscv_vfmv_v_f_f32m2( sL5, vl );
		vol5 = __riscv_vfmacc_vf_f32m2( vol5, incL5, idx_f, vl );
		vfloat32m2_t s = __riscv_vle32_v_f32m2( samples + j, vl );
		const ptrdiff_t s6 = (ptrdiff_t)( 6 * sizeof( float ) );
		vfloat32m2_t m0 = __riscv_vlse32_v_f32m2( mixBuffer + j * 6 + 0, s6, vl );
		vfloat32m2_t m1 = __riscv_vlse32_v_f32m2( mixBuffer + j * 6 + 1, s6, vl );
		vfloat32m2_t m2 = __riscv_vlse32_v_f32m2( mixBuffer + j * 6 + 2, s6, vl );
		vfloat32m2_t m3 = __riscv_vlse32_v_f32m2( mixBuffer + j * 6 + 3, s6, vl );
		vfloat32m2_t m4 = __riscv_vlse32_v_f32m2( mixBuffer + j * 6 + 4, s6, vl );
		vfloat32m2_t m5 = __riscv_vlse32_v_f32m2( mixBuffer + j * 6 + 5, s6, vl );
		m0 = __riscv_vfmacc_vv_f32m2( m0, s, vol0, vl );
		m1 = __riscv_vfmacc_vv_f32m2( m1, s, vol1, vl );
		m2 = __riscv_vfmacc_vv_f32m2( m2, s, vol2, vl );
		m3 = __riscv_vfmacc_vv_f32m2( m3, s, vol3, vl );
		m4 = __riscv_vfmacc_vv_f32m2( m4, s, vol4, vl );
		m5 = __riscv_vfmacc_vv_f32m2( m5, s, vol5, vl );
		__riscv_vsse32_v_f32m2( mixBuffer + j * 6 + 0, s6, m0, vl );
		__riscv_vsse32_v_f32m2( mixBuffer + j * 6 + 1, s6, m1, vl );
		__riscv_vsse32_v_f32m2( mixBuffer + j * 6 + 2, s6, m2, vl );
		__riscv_vsse32_v_f32m2( mixBuffer + j * 6 + 3, s6, m3, vl );
		__riscv_vsse32_v_f32m2( mixBuffer + j * 6 + 4, s6, m4, vl );
		__riscv_vsse32_v_f32m2( mixBuffer + j * 6 + 5, s6, m5, vl );
		j += vl;
	}
}

void VPCALL idSIMD_RVV::MixSoundSixSpeakerStereo( float *mixBuffer, const float *samples, const int numSamples, const float lastV[6], const float currentV[6] ) {
	float sL0 = lastV[0];
	float sL1 = lastV[1];
	float sL2 = lastV[2];
	float sL3 = lastV[3];
	float sL4 = lastV[4];
	float sL5 = lastV[5];
	float incL0 = ( currentV[0] - lastV[0] ) / (float)MIXBUFFER_SAMPLES;
	float incL1 = ( currentV[1] - lastV[1] ) / (float)MIXBUFFER_SAMPLES;
	float incL2 = ( currentV[2] - lastV[2] ) / (float)MIXBUFFER_SAMPLES;
	float incL3 = ( currentV[3] - lastV[3] ) / (float)MIXBUFFER_SAMPLES;
	float incL4 = ( currentV[4] - lastV[4] ) / (float)MIXBUFFER_SAMPLES;
	float incL5 = ( currentV[5] - lastV[5] ) / (float)MIXBUFFER_SAMPLES;
	assert( numSamples == MIXBUFFER_SAMPLES );
	int j = 0;
	while ( j < MIXBUFFER_SAMPLES ) {
		size_t vl = __riscv_vsetvl_e32m2( MIXBUFFER_SAMPLES - j );
		vuint32m2_t idx = __riscv_vid_v_u32m2( vl );
		idx = __riscv_vadd_vx_u32m2( idx, j, vl );
		vfloat32m2_t idx_f = __riscv_vfcvt_f_xu_v_f32m2( idx, vl );
		vfloat32m2_t vol0 = __riscv_vfmv_v_f_f32m2( sL0, vl );
		vol0 = __riscv_vfmacc_vf_f32m2( vol0, incL0, idx_f, vl );
		vfloat32m2_t vol1 = __riscv_vfmv_v_f_f32m2( sL1, vl );
		vol1 = __riscv_vfmacc_vf_f32m2( vol1, incL1, idx_f, vl );
		vfloat32m2_t vol2 = __riscv_vfmv_v_f_f32m2( sL2, vl );
		vol2 = __riscv_vfmacc_vf_f32m2( vol2, incL2, idx_f, vl );
		vfloat32m2_t vol3 = __riscv_vfmv_v_f_f32m2( sL3, vl );
		vol3 = __riscv_vfmacc_vf_f32m2( vol3, incL3, idx_f, vl );
		vfloat32m2_t vol4 = __riscv_vfmv_v_f_f32m2( sL4, vl );
		vol4 = __riscv_vfmacc_vf_f32m2( vol4, incL4, idx_f, vl );
		vfloat32m2_t vol5 = __riscv_vfmv_v_f_f32m2( sL5, vl );
		vol5 = __riscv_vfmacc_vf_f32m2( vol5, incL5, idx_f, vl );
		/* interleaved stereo source: stride 2 so left (s0) and right (s1) stay disjoint
		 * (the mix buffer below is already correctly 6-strided). */
		vfloat32m2_t s0 = __riscv_vlse32_v_f32m2( samples + j * 2, 2 * sizeof( float ), vl );
		vfloat32m2_t s1 = __riscv_vlse32_v_f32m2( samples + j * 2 + 1, 2 * sizeof( float ), vl );
		const ptrdiff_t s6 = (ptrdiff_t)( 6 * sizeof( float ) );
		vfloat32m2_t m0 = __riscv_vlse32_v_f32m2( mixBuffer + j * 6 + 0, s6, vl );
		vfloat32m2_t m1 = __riscv_vlse32_v_f32m2( mixBuffer + j * 6 + 1, s6, vl );
		vfloat32m2_t m2 = __riscv_vlse32_v_f32m2( mixBuffer + j * 6 + 2, s6, vl );
		vfloat32m2_t m3 = __riscv_vlse32_v_f32m2( mixBuffer + j * 6 + 3, s6, vl );
		vfloat32m2_t m4 = __riscv_vlse32_v_f32m2( mixBuffer + j * 6 + 4, s6, vl );
		vfloat32m2_t m5 = __riscv_vlse32_v_f32m2( mixBuffer + j * 6 + 5, s6, vl );
		m0 = __riscv_vfmacc_vv_f32m2( m0, s0, vol0, vl );
		m1 = __riscv_vfmacc_vv_f32m2( m1, s1, vol1, vl );
		m2 = __riscv_vfmacc_vv_f32m2( m2, s0, vol2, vl );
		m3 = __riscv_vfmacc_vv_f32m2( m3, s0, vol3, vl );
		m4 = __riscv_vfmacc_vv_f32m2( m4, s0, vol4, vl );
		m5 = __riscv_vfmacc_vv_f32m2( m5, s1, vol5, vl );
		__riscv_vsse32_v_f32m2( mixBuffer + j * 6 + 0, s6, m0, vl );
		__riscv_vsse32_v_f32m2( mixBuffer + j * 6 + 1, s6, m1, vl );
		__riscv_vsse32_v_f32m2( mixBuffer + j * 6 + 2, s6, m2, vl );
		__riscv_vsse32_v_f32m2( mixBuffer + j * 6 + 3, s6, m3, vl );
		__riscv_vsse32_v_f32m2( mixBuffer + j * 6 + 4, s6, m4, vl );
		__riscv_vsse32_v_f32m2( mixBuffer + j * 6 + 5, s6, m5, vl );
		j += vl;
	}
}
void VPCALL idSIMD_RVV::MatX_MultiplyVecX( idVecX &dst, const idMatX &mat, const idVecX &vec ) {
	int i, numRows;
	const float *mPtr, *vPtr;
	float *dstPtr;

	assert( vec.GetSize() >= mat.GetNumColumns() );
	assert( dst.GetSize() >= mat.GetNumRows() );

	mPtr = mat.ToFloatPtr();
	vPtr = vec.ToFloatPtr();
	dstPtr = dst.ToFloatPtr();
	numRows = mat.GetNumRows();
	switch( mat.GetNumColumns() ) {
		case 1:
			for ( i = 0; i < numRows; i++ ) {
				dstPtr[i] = mPtr[0] * vPtr[0];
				mPtr++;
			}
			break;
		case 2:
			for ( i = 0; i < numRows; i++ ) {
				dstPtr[i] = mPtr[0] * vPtr[0] + mPtr[1] * vPtr[1];
				mPtr += 2;
			}
			break;
		case 3:
			for ( i = 0; i < numRows; i++ ) {
				dstPtr[i] = mPtr[0] * vPtr[0] + mPtr[1] * vPtr[1] + mPtr[2] * vPtr[2];
				mPtr += 3;
			}
			break;
		case 4:
			for ( i = 0; i < numRows; i++ ) {
				dstPtr[i] = mPtr[0] * vPtr[0] + mPtr[1] * vPtr[1] + mPtr[2] * vPtr[2] +
							mPtr[3] * vPtr[3];
				mPtr += 4;
			}
			break;
		case 5:
			for ( i = 0; i < numRows; i++ ) {
				dstPtr[i] = mPtr[0] * vPtr[0] + mPtr[1] * vPtr[1] + mPtr[2] * vPtr[2] +
							mPtr[3] * vPtr[3] + mPtr[4] * vPtr[4];
				mPtr += 5;
			}
			break;
		case 6:
			for ( i = 0; i < numRows; i++ ) {
				dstPtr[i] = mPtr[0] * vPtr[0] + mPtr[1] * vPtr[1] + mPtr[2] * vPtr[2] +
							mPtr[3] * vPtr[3] + mPtr[4] * vPtr[4] + mPtr[5] * vPtr[5];
				mPtr += 6;
			}
			break;
		default:
			int numColumns = mat.GetNumColumns();
			for ( i = 0; i < numRows; i++ ) {
				float sum = 0.0f;
				int k = 0;
				while ( k < numColumns ) {
					size_t vl = __riscv_vsetvl_e32m8( numColumns - k );
					vfloat32m8_t mv = __riscv_vle32_v_f32m8( mPtr + k, vl );
					vfloat32m8_t vv = __riscv_vle32_v_f32m8( vPtr + k, vl );
					vfloat32m1_t acc = __riscv_vfmv_s_f_f32m1( 0.0f, 1 );
					acc = __riscv_vfredusum_vs_f32m8_f32m1( __riscv_vfmul_vv_f32m8( mv, vv, vl ), acc, vl );
					sum += __riscv_vfmv_f_s_f32m1_f32( acc );
					k += vl;
				}
				dstPtr[i] = sum;
				mPtr += numColumns;
			}
			break;
	}
}

void VPCALL idSIMD_RVV::MatX_MultiplyAddVecX( idVecX &dst, const idMatX &mat, const idVecX &vec ) {
	int i, numRows;
	const float *mPtr, *vPtr;
	float *dstPtr;

	assert( vec.GetSize() >= mat.GetNumColumns() );
	assert( dst.GetSize() >= mat.GetNumRows() );

	mPtr = mat.ToFloatPtr();
	vPtr = vec.ToFloatPtr();
	dstPtr = dst.ToFloatPtr();
	numRows = mat.GetNumRows();
	switch( mat.GetNumColumns() ) {
		case 1:
			for ( i = 0; i < numRows; i++ ) {
				dstPtr[i] += mPtr[0] * vPtr[0];
				mPtr++;
			}
			break;
		case 2:
			for ( i = 0; i < numRows; i++ ) {
				dstPtr[i] += mPtr[0] * vPtr[0] + mPtr[1] * vPtr[1];
				mPtr += 2;
			}
			break;
		case 3:
			for ( i = 0; i < numRows; i++ ) {
				dstPtr[i] += mPtr[0] * vPtr[0] + mPtr[1] * vPtr[1] + mPtr[2] * vPtr[2];
				mPtr += 3;
			}
			break;
		case 4:
			for ( i = 0; i < numRows; i++ ) {
				dstPtr[i] += mPtr[0] * vPtr[0] + mPtr[1] * vPtr[1] + mPtr[2] * vPtr[2] +
							mPtr[3] * vPtr[3];
				mPtr += 4;
			}
			break;
		case 5:
			for ( i = 0; i < numRows; i++ ) {
				dstPtr[i] += mPtr[0] * vPtr[0] + mPtr[1] * vPtr[1] + mPtr[2] * vPtr[2] +
							mPtr[3] * vPtr[3] + mPtr[4] * vPtr[4];
				mPtr += 5;
			}
			break;
		case 6:
			for ( i = 0; i < numRows; i++ ) {
				dstPtr[i] += mPtr[0] * vPtr[0] + mPtr[1] * vPtr[1] + mPtr[2] * vPtr[2] +
							mPtr[3] * vPtr[3] + mPtr[4] * vPtr[4] + mPtr[5] * vPtr[5];
				mPtr += 6;
			}
			break;
		default:
			int numColumns = mat.GetNumColumns();
			for ( i = 0; i < numRows; i++ ) {
				float sum = 0.0f;
				int k = 0;
				while ( k < numColumns ) {
					size_t vl = __riscv_vsetvl_e32m8( numColumns - k );
					vfloat32m8_t mv = __riscv_vle32_v_f32m8( mPtr + k, vl );
					vfloat32m8_t vv = __riscv_vle32_v_f32m8( vPtr + k, vl );
					vfloat32m1_t acc = __riscv_vfmv_s_f_f32m1( 0.0f, 1 );
					acc = __riscv_vfredusum_vs_f32m8_f32m1( __riscv_vfmul_vv_f32m8( mv, vv, vl ), acc, vl );
					sum += __riscv_vfmv_f_s_f32m1_f32( acc );
					k += vl;
				}
				dstPtr[i] += sum;
				mPtr += numColumns;
			}
			break;
	}
}

void VPCALL idSIMD_RVV::MatX_MultiplySubVecX( idVecX &dst, const idMatX &mat, const idVecX &vec ) {
	int i, numRows;
	const float *mPtr, *vPtr;
	float *dstPtr;

	assert( vec.GetSize() >= mat.GetNumColumns() );
	assert( dst.GetSize() >= mat.GetNumRows() );

	mPtr = mat.ToFloatPtr();
	vPtr = vec.ToFloatPtr();
	dstPtr = dst.ToFloatPtr();
	numRows = mat.GetNumRows();
	switch( mat.GetNumColumns() ) {
		case 1:
			for ( i = 0; i < numRows; i++ ) {
				dstPtr[i] -= mPtr[0] * vPtr[0];
				mPtr++;
			}
			break;
		case 2:
			for ( i = 0; i < numRows; i++ ) {
				dstPtr[i] -= mPtr[0] * vPtr[0] + mPtr[1] * vPtr[1];
				mPtr += 2;
			}
			break;
		case 3:
			for ( i = 0; i < numRows; i++ ) {
				dstPtr[i] -= mPtr[0] * vPtr[0] + mPtr[1] * vPtr[1] + mPtr[2] * vPtr[2];
				mPtr += 3;
			}
			break;
		case 4:
			for ( i = 0; i < numRows; i++ ) {
				dstPtr[i] -= mPtr[0] * vPtr[0] + mPtr[1] * vPtr[1] + mPtr[2] * vPtr[2] +
							mPtr[3] * vPtr[3];
				mPtr += 4;
			}
			break;
		case 5:
			for ( i = 0; i < numRows; i++ ) {
				dstPtr[i] -= mPtr[0] * vPtr[0] + mPtr[1] * vPtr[1] + mPtr[2] * vPtr[2] +
							mPtr[3] * vPtr[3] + mPtr[4] * vPtr[4];
				mPtr += 5;
			}
			break;
		case 6:
			for ( i = 0; i < numRows; i++ ) {
				dstPtr[i] -= mPtr[0] * vPtr[0] + mPtr[1] * vPtr[1] + mPtr[2] * vPtr[2] +
							mPtr[3] * vPtr[3] + mPtr[4] * vPtr[4] + mPtr[5] * vPtr[5];
				mPtr += 6;
			}
			break;
		default:
			int numColumns = mat.GetNumColumns();
			for ( i = 0; i < numRows; i++ ) {
				float sum = 0.0f;
				int k = 0;
				while ( k < numColumns ) {
					size_t vl = __riscv_vsetvl_e32m8( numColumns - k );
					vfloat32m8_t mv = __riscv_vle32_v_f32m8( mPtr + k, vl );
					vfloat32m8_t vv = __riscv_vle32_v_f32m8( vPtr + k, vl );
					vfloat32m1_t acc = __riscv_vfmv_s_f_f32m1( 0.0f, 1 );
					acc = __riscv_vfredusum_vs_f32m8_f32m1( __riscv_vfmul_vv_f32m8( mv, vv, vl ), acc, vl );
					sum += __riscv_vfmv_f_s_f32m1_f32( acc );
					k += vl;
				}
				dstPtr[i] -= sum;
				mPtr += numColumns;
			}
			break;
	}
}

void VPCALL idSIMD_RVV::MatX_TransposeMultiplyVecX( idVecX &dst, const idMatX &mat, const idVecX &vec ) {
	int i, numColumns;
	const float *mPtr, *vPtr;
	float *dstPtr;

	assert( vec.GetSize() >= mat.GetNumRows() );
	assert( dst.GetSize() >= mat.GetNumColumns() );

	mPtr = mat.ToFloatPtr();
	vPtr = vec.ToFloatPtr();
	dstPtr = dst.ToFloatPtr();
	numColumns = mat.GetNumColumns();
	switch( mat.GetNumRows() ) {
		case 1:
			for ( i = 0; i < numColumns; i++ ) {
				dstPtr[i] = *(mPtr) * vPtr[0];
				mPtr++;
			}
			break;
		case 2:
			for ( i = 0; i < numColumns; i++ ) {
				dstPtr[i] = *(mPtr) * vPtr[0] + *(mPtr+numColumns) * vPtr[1];
				mPtr++;
			}
			break;
		case 3:
			for ( i = 0; i < numColumns; i++ ) {
				dstPtr[i] = *(mPtr) * vPtr[0] + *(mPtr+numColumns) * vPtr[1] + *(mPtr+2*numColumns) * vPtr[2];
				mPtr++;
			}
			break;
		case 4:
			for ( i = 0; i < numColumns; i++ ) {
				dstPtr[i] = *(mPtr) * vPtr[0] + *(mPtr+numColumns) * vPtr[1] + *(mPtr+2*numColumns) * vPtr[2] + *(mPtr+3*numColumns) * vPtr[3];
				mPtr++;
			}
			break;
		case 5:
			for ( i = 0; i < numColumns; i++ ) {
				dstPtr[i] = *(mPtr) * vPtr[0] + *(mPtr+numColumns) * vPtr[1] + *(mPtr+2*numColumns) * vPtr[2] + *(mPtr+3*numColumns) * vPtr[3] + *(mPtr+4*numColumns) * vPtr[4];
				mPtr++;
			}
			break;
		case 6:
			for ( i = 0; i < numColumns; i++ ) {
				dstPtr[i] = *(mPtr) * vPtr[0] + *(mPtr+numColumns) * vPtr[1] + *(mPtr+2*numColumns) * vPtr[2] + *(mPtr+3*numColumns) * vPtr[3] + *(mPtr+4*numColumns) * vPtr[4] + *(mPtr+5*numColumns) * vPtr[5];
				mPtr++;
			}
			break;
		default: {
			// Reduction-FREE, unit-stride: dst[j] = sum_i mat[i][j]*vec[i]. Tile output
			// columns by vl, ONE vfmacc per row (mat row i is numColumns contiguous
			// floats -> unit-stride vle). Removes the old per-column strided vlse load
			// AND the per-output-element vfredusum. Row-order sum matches generic; the
			// small (<=6) sizes stay scalar above (a vl=numColumns vector wastes lanes).
			const int numRows = mat.GetNumRows();
			int j = 0;
			while ( j < numColumns ) {
				size_t vl = __riscv_vsetvl_e32m8( numColumns - j );
				vfloat32m8_t acc = __riscv_vfmv_v_f_f32m8( 0.0f, vl );
				for ( i = 0; i < numRows; i++ ) {
					vfloat32m8_t row = __riscv_vle32_v_f32m8( mPtr + (ptrdiff_t)i * numColumns + j, vl );
					acc = __riscv_vfmacc_vf_f32m8( acc, vPtr[i], row, vl );
				}
				__riscv_vse32_v_f32m8( dstPtr + j, acc, vl );
				j += (int)vl;
			}
		}
		break;
	}
}

void VPCALL idSIMD_RVV::MatX_TransposeMultiplyAddVecX( idVecX &dst, const idMatX &mat, const idVecX &vec ) {
	int i, numColumns;
	const float *mPtr, *vPtr;
	float *dstPtr;

	assert( vec.GetSize() >= mat.GetNumRows() );
	assert( dst.GetSize() >= mat.GetNumColumns() );

	mPtr = mat.ToFloatPtr();
	vPtr = vec.ToFloatPtr();
	dstPtr = dst.ToFloatPtr();
	numColumns = mat.GetNumColumns();
	switch( mat.GetNumRows() ) {
		case 1:
			for ( i = 0; i < numColumns; i++ ) {
				dstPtr[i] += *(mPtr) * vPtr[0];
				mPtr++;
			}
			break;
		case 2:
			for ( i = 0; i < numColumns; i++ ) {
				dstPtr[i] += *(mPtr) * vPtr[0] + *(mPtr+numColumns) * vPtr[1];
				mPtr++;
			}
			break;
		case 3:
			for ( i = 0; i < numColumns; i++ ) {
				dstPtr[i] += *(mPtr) * vPtr[0] + *(mPtr+numColumns) * vPtr[1] + *(mPtr+2*numColumns) * vPtr[2];
				mPtr++;
			}
			break;
		case 4:
			for ( i = 0; i < numColumns; i++ ) {
				dstPtr[i] += *(mPtr) * vPtr[0] + *(mPtr+numColumns) * vPtr[1] + *(mPtr+2*numColumns) * vPtr[2] +
						*(mPtr+3*numColumns) * vPtr[3];
				mPtr++;
			}
			break;
		case 5:
			for ( i = 0; i < numColumns; i++ ) {
				dstPtr[i] += *(mPtr) * vPtr[0] + *(mPtr+numColumns) * vPtr[1] + *(mPtr+2*numColumns) * vPtr[2] +
						*(mPtr+3*numColumns) * vPtr[3] + *(mPtr+4*numColumns) * vPtr[4];
				mPtr++;
			}
			break;
		case 6:
			for ( i = 0; i < numColumns; i++ ) {
				dstPtr[i] += *(mPtr) * vPtr[0] + *(mPtr+numColumns) * vPtr[1] + *(mPtr+2*numColumns) * vPtr[2] +
						*(mPtr+3*numColumns) * vPtr[3] + *(mPtr+4*numColumns) * vPtr[4] + *(mPtr+5*numColumns) * vPtr[5];
				mPtr++;
			}
			break;
		default:
			int numRows = mat.GetNumRows();
			ptrdiff_t stride = numColumns * sizeof( float );
			for ( i = 0; i < numColumns; i++ ) {
				mPtr = mat.ToFloatPtr() + i;
				float sum = 0.0f;
				int k = 0;
				while ( k < numRows ) {
					size_t vl = __riscv_vsetvl_e32m8( numRows - k );
					vfloat32m8_t mv = __riscv_vlse32_v_f32m8( mPtr, stride, vl );
					vfloat32m8_t vv = __riscv_vle32_v_f32m8( vPtr + k, vl );
					vfloat32m1_t acc = __riscv_vfmv_s_f_f32m1( 0.0f, 1 );
					acc = __riscv_vfredusum_vs_f32m8_f32m1( __riscv_vfmul_vv_f32m8( mv, vv, vl ), acc, vl );
					sum += __riscv_vfmv_f_s_f32m1_f32( acc );
					mPtr += vl * numColumns;
					k += vl;
				}
				dstPtr[i] += sum;
			}
			break;
	}
}

void VPCALL idSIMD_RVV::MatX_TransposeMultiplySubVecX( idVecX &dst, const idMatX &mat, const idVecX &vec ) {
	int i, numColumns;
	const float *mPtr, *vPtr;
	float *dstPtr;

	assert( vec.GetSize() >= mat.GetNumRows() );
	assert( dst.GetSize() >= mat.GetNumColumns() );

	mPtr = mat.ToFloatPtr();
	vPtr = vec.ToFloatPtr();
	dstPtr = dst.ToFloatPtr();
	numColumns = mat.GetNumColumns();
	switch( mat.GetNumRows() ) {
		case 1:
			for ( i = 0; i < numColumns; i++ ) {
				dstPtr[i] -= *(mPtr) * vPtr[0];
				mPtr++;
			}
			break;
		case 2:
			for ( i = 0; i < numColumns; i++ ) {
				dstPtr[i] -= *(mPtr) * vPtr[0] + *(mPtr+numColumns) * vPtr[1];
				mPtr++;
			}
			break;
		case 3:
			for ( i = 0; i < numColumns; i++ ) {
				dstPtr[i] -= *(mPtr) * vPtr[0] + *(mPtr+numColumns) * vPtr[1] + *(mPtr+2*numColumns) * vPtr[2];
				mPtr++;
			}
			break;
		case 4:
			for ( i = 0; i < numColumns; i++ ) {
				dstPtr[i] -= *(mPtr) * vPtr[0] + *(mPtr+numColumns) * vPtr[1] + *(mPtr+2*numColumns) * vPtr[2] +
						*(mPtr+3*numColumns) * vPtr[3];
				mPtr++;
			}
			break;
		case 5:
			for ( i = 0; i < numColumns; i++ ) {
				dstPtr[i] -= *(mPtr) * vPtr[0] + *(mPtr+numColumns) * vPtr[1] + *(mPtr+2*numColumns) * vPtr[2] +
						*(mPtr+3*numColumns) * vPtr[3] + *(mPtr+4*numColumns) * vPtr[4];
				mPtr++;
			}
			break;
		case 6:
			for ( i = 0; i < numColumns; i++ ) {
				dstPtr[i] -= *(mPtr) * vPtr[0] + *(mPtr+numColumns) * vPtr[1] + *(mPtr+2*numColumns) * vPtr[2] +
						*(mPtr+3*numColumns) * vPtr[3] + *(mPtr+4*numColumns) * vPtr[4] + *(mPtr+5*numColumns) * vPtr[5];
				mPtr++;
			}
			break;
		default:
			int numRows = mat.GetNumRows();
			ptrdiff_t stride = numColumns * sizeof( float );
			for ( i = 0; i < numColumns; i++ ) {
				mPtr = mat.ToFloatPtr() + i;
				float sum = 0.0f;
				int k = 0;
				while ( k < numRows ) {
					size_t vl = __riscv_vsetvl_e32m8( numRows - k );
					vfloat32m8_t mv = __riscv_vlse32_v_f32m8( mPtr, stride, vl );
					vfloat32m8_t vv = __riscv_vle32_v_f32m8( vPtr + k, vl );
					vfloat32m1_t acc = __riscv_vfmv_s_f_f32m1( 0.0f, 1 );
					acc = __riscv_vfredusum_vs_f32m8_f32m1( __riscv_vfmul_vv_f32m8( mv, vv, vl ), acc, vl );
					sum += __riscv_vfmv_f_s_f32m1_f32( acc );
					mPtr += vl * numColumns;
					k += vl;
				}
				dstPtr[i] -= sum;
			}
			break;
	}
}

void VPCALL idSIMD_RVV::MatX_MultiplyMatX( idMatX &dst, const idMatX &m1, const idMatX &m2 ) {
	int i, j, k, l;
	float *dstPtr;
	const float *m1Ptr, *m2Ptr;

	assert( m1.GetNumColumns() == m2.GetNumRows() );

	dstPtr = dst.ToFloatPtr();
	m1Ptr = m1.ToFloatPtr();
	m2Ptr = m2.ToFloatPtr();
	k = m1.GetNumRows();
	l = m2.GetNumColumns();

	switch( m1.GetNumColumns() ) {
		case 1: {
			if ( l == 6 ) {
				for ( i = 0; i < k; i++ ) {		// Nx1 * 1x6
					*dstPtr++ = m1Ptr[i] * m2Ptr[0];
					*dstPtr++ = m1Ptr[i] * m2Ptr[1];
					*dstPtr++ = m1Ptr[i] * m2Ptr[2];
					*dstPtr++ = m1Ptr[i] * m2Ptr[3];
					*dstPtr++ = m1Ptr[i] * m2Ptr[4];
					*dstPtr++ = m1Ptr[i] * m2Ptr[5];
				}
				return;
			}
			for ( i = 0; i < k; i++ ) {
				m2Ptr = m2.ToFloatPtr();
				for ( j = 0; j < l; j++ ) {
					*dstPtr++ = m1Ptr[0] * m2Ptr[0];
					m2Ptr++;
				}
				m1Ptr++;
			}
			break;
		}
		case 2: {
			if ( l == 6 ) {
				for ( i = 0; i < k; i++ ) {		// Nx2 * 2x6
					*dstPtr++ = m1Ptr[0] * m2Ptr[0] + m1Ptr[1] * m2Ptr[6];
					*dstPtr++ = m1Ptr[0] * m2Ptr[1] + m1Ptr[1] * m2Ptr[7];
					*dstPtr++ = m1Ptr[0] * m2Ptr[2] + m1Ptr[1] * m2Ptr[8];
					*dstPtr++ = m1Ptr[0] * m2Ptr[3] + m1Ptr[1] * m2Ptr[9];
					*dstPtr++ = m1Ptr[0] * m2Ptr[4] + m1Ptr[1] * m2Ptr[10];
					*dstPtr++ = m1Ptr[0] * m2Ptr[5] + m1Ptr[1] * m2Ptr[11];
					m1Ptr += 2;
				}
				return;
			}
			for ( i = 0; i < k; i++ ) {
				m2Ptr = m2.ToFloatPtr();
				for ( j = 0; j < l; j++ ) {
					*dstPtr++ = m1Ptr[0] * m2Ptr[0] + m1Ptr[1] * m2Ptr[l];
					m2Ptr++;
				}
				m1Ptr += 2;
			}
			break;
		}
		case 3: {
			if ( l == 6 ) {
				for ( i = 0; i < k; i++ ) {		// Nx3 * 3x6
					*dstPtr++ = m1Ptr[0] * m2Ptr[0] + m1Ptr[1] * m2Ptr[6] + m1Ptr[2] * m2Ptr[12];
					*dstPtr++ = m1Ptr[0] * m2Ptr[1] + m1Ptr[1] * m2Ptr[7] + m1Ptr[2] * m2Ptr[13];
					*dstPtr++ = m1Ptr[0] * m2Ptr[2] + m1Ptr[1] * m2Ptr[8] + m1Ptr[2] * m2Ptr[14];
					*dstPtr++ = m1Ptr[0] * m2Ptr[3] + m1Ptr[1] * m2Ptr[9] + m1Ptr[2] * m2Ptr[15];
					*dstPtr++ = m1Ptr[0] * m2Ptr[4] + m1Ptr[1] * m2Ptr[10] + m1Ptr[2] * m2Ptr[16];
					*dstPtr++ = m1Ptr[0] * m2Ptr[5] + m1Ptr[1] * m2Ptr[11] + m1Ptr[2] * m2Ptr[17];
					m1Ptr += 3;
				}
				return;
			}
			for ( i = 0; i < k; i++ ) {
				m2Ptr = m2.ToFloatPtr();
				for ( j = 0; j < l; j++ ) {
					*dstPtr++ = m1Ptr[0] * m2Ptr[0] + m1Ptr[1] * m2Ptr[l] + m1Ptr[2] * m2Ptr[2*l];
					m2Ptr++;
				}
				m1Ptr += 3;
			}
			break;
		}
		case 4: {
			if ( l == 6 ) {
				for ( i = 0; i < k; i++ ) {		// Nx4 * 4x6
					*dstPtr++ = m1Ptr[0] * m2Ptr[0] + m1Ptr[1] * m2Ptr[6] + m1Ptr[2] * m2Ptr[12] + m1Ptr[3] * m2Ptr[18];
					*dstPtr++ = m1Ptr[0] * m2Ptr[1] + m1Ptr[1] * m2Ptr[7] + m1Ptr[2] * m2Ptr[13] + m1Ptr[3] * m2Ptr[19];
					*dstPtr++ = m1Ptr[0] * m2Ptr[2] + m1Ptr[1] * m2Ptr[8] + m1Ptr[2] * m2Ptr[14] + m1Ptr[3] * m2Ptr[20];
					*dstPtr++ = m1Ptr[0] * m2Ptr[3] + m1Ptr[1] * m2Ptr[9] + m1Ptr[2] * m2Ptr[15] + m1Ptr[3] * m2Ptr[21];
					*dstPtr++ = m1Ptr[0] * m2Ptr[4] + m1Ptr[1] * m2Ptr[10] + m1Ptr[2] * m2Ptr[16] + m1Ptr[3] * m2Ptr[22];
					*dstPtr++ = m1Ptr[0] * m2Ptr[5] + m1Ptr[1] * m2Ptr[11] + m1Ptr[2] * m2Ptr[17] + m1Ptr[3] * m2Ptr[23];
					m1Ptr += 4;
				}
				return;
			}
			for ( i = 0; i < k; i++ ) {
				m2Ptr = m2.ToFloatPtr();
				for ( j = 0; j < l; j++ ) {
					*dstPtr++ = m1Ptr[0] * m2Ptr[0] + m1Ptr[1] * m2Ptr[l] + m1Ptr[2] * m2Ptr[2*l] +
									 m1Ptr[3] * m2Ptr[3*l];
					m2Ptr++;
				}
				m1Ptr += 4;
			}
			break;
		}
		case 5: {
			if ( l == 6 ) {
				for ( i = 0; i < k; i++ ) {		// Nx5 * 5x6
					*dstPtr++ = m1Ptr[0] * m2Ptr[0] + m1Ptr[1] * m2Ptr[6] + m1Ptr[2] * m2Ptr[12] + m1Ptr[3] * m2Ptr[18] + m1Ptr[4] * m2Ptr[24];
					*dstPtr++ = m1Ptr[0] * m2Ptr[1] + m1Ptr[1] * m2Ptr[7] + m1Ptr[2] * m2Ptr[13] + m1Ptr[3] * m2Ptr[19] + m1Ptr[4] * m2Ptr[25];
					*dstPtr++ = m1Ptr[0] * m2Ptr[2] + m1Ptr[1] * m2Ptr[8] + m1Ptr[2] * m2Ptr[14] + m1Ptr[3] * m2Ptr[20] + m1Ptr[4] * m2Ptr[26];
					*dstPtr++ = m1Ptr[0] * m2Ptr[3] + m1Ptr[1] * m2Ptr[9] + m1Ptr[2] * m2Ptr[15] + m1Ptr[3] * m2Ptr[21] + m1Ptr[4] * m2Ptr[27];
					*dstPtr++ = m1Ptr[0] * m2Ptr[4] + m1Ptr[1] * m2Ptr[10] + m1Ptr[2] * m2Ptr[16] + m1Ptr[3] * m2Ptr[22] + m1Ptr[4] * m2Ptr[28];
					*dstPtr++ = m1Ptr[0] * m2Ptr[5] + m1Ptr[1] * m2Ptr[11] + m1Ptr[2] * m2Ptr[17] + m1Ptr[3] * m2Ptr[23] + m1Ptr[4] * m2Ptr[29];
					m1Ptr += 5;
				}
				return;
			}
			for ( i = 0; i < k; i++ ) {
				m2Ptr = m2.ToFloatPtr();
				for ( j = 0; j < l; j++ ) {
					*dstPtr++ = m1Ptr[0] * m2Ptr[0] + m1Ptr[1] * m2Ptr[l] + m1Ptr[2] * m2Ptr[2*l] +
									 m1Ptr[3] * m2Ptr[3*l] + m1Ptr[4] * m2Ptr[4*l];
					m2Ptr++;
				}
				m1Ptr += 5;
			}
			break;
		}
		case 6: {
			switch( k ) {
				case 1: {
					if ( l == 1 ) {		// 1x6 * 6x1
						dstPtr[0] = m1Ptr[0] * m2Ptr[0] + m1Ptr[1] * m2Ptr[1] + m1Ptr[2] * m2Ptr[2] +
									 m1Ptr[3] * m2Ptr[3] + m1Ptr[4] * m2Ptr[4] + m1Ptr[5] * m2Ptr[5];
						return;
					}
					break;
				}
				case 2: {
					if ( l == 2 ) {		// 2x6 * 6x2
						for ( i = 0; i < 2; i++ ) {
							for ( j = 0; j < 2; j++ ) {
								*dstPtr = m1Ptr[0] * m2Ptr[ 0 * 2 + j ]
										+ m1Ptr[1] * m2Ptr[ 1 * 2 + j ]
										+ m1Ptr[2] * m2Ptr[ 2 * 2 + j ]
										+ m1Ptr[3] * m2Ptr[ 3 * 2 + j ]
										+ m1Ptr[4] * m2Ptr[ 4 * 2 + j ]
										+ m1Ptr[5] * m2Ptr[ 5 * 2 + j ];
								dstPtr++;
							}
							m1Ptr += 6;
						}
						return;
					}
					break;
				}
				case 3: {
					if ( l == 3 ) {		// 3x6 * 6x3
						for ( i = 0; i < 3; i++ ) {
							for ( j = 0; j < 3; j++ ) {
								*dstPtr = m1Ptr[0] * m2Ptr[ 0 * 3 + j ]
										+ m1Ptr[1] * m2Ptr[ 1 * 3 + j ]
										+ m1Ptr[2] * m2Ptr[ 2 * 3 + j ]
										+ m1Ptr[3] * m2Ptr[ 3 * 3 + j ]
										+ m1Ptr[4] * m2Ptr[ 4 * 3 + j ]
										+ m1Ptr[5] * m2Ptr[ 5 * 3 + j ];
								dstPtr++;
							}
							m1Ptr += 6;
						}
						return;
					}
					break;
				}
				case 4: {
					if ( l == 4 ) {		// 4x6 * 6x4
						for ( i = 0; i < 4; i++ ) {
							for ( j = 0; j < 4; j++ ) {
								*dstPtr = m1Ptr[0] * m2Ptr[ 0 * 4 + j ]
										+ m1Ptr[1] * m2Ptr[ 1 * 4 + j ]
										+ m1Ptr[2] * m2Ptr[ 2 * 4 + j ]
										+ m1Ptr[3] * m2Ptr[ 3 * 4 + j ]
										+ m1Ptr[4] * m2Ptr[ 4 * 4 + j ]
										+ m1Ptr[5] * m2Ptr[ 5 * 4 + j ];
								dstPtr++;
							}
							m1Ptr += 6;
						}
						return;
					}
				}
				case 5: {
					if ( l == 5 ) {		// 5x6 * 6x5
						for ( i = 0; i < 5; i++ ) {
							for ( j = 0; j < 5; j++ ) {
								*dstPtr = m1Ptr[0] * m2Ptr[ 0 * 5 + j ]
										+ m1Ptr[1] * m2Ptr[ 1 * 5 + j ]
										+ m1Ptr[2] * m2Ptr[ 2 * 5 + j ]
										+ m1Ptr[3] * m2Ptr[ 3 * 5 + j ]
										+ m1Ptr[4] * m2Ptr[ 4 * 5 + j ]
										+ m1Ptr[5] * m2Ptr[ 5 * 5 + j ];
								dstPtr++;
							}
							m1Ptr += 6;
						}
						return;
					}
				}
				case 6: {
					switch( l ) {
						case 1: {		// 6x6 * 6x1
							for ( i = 0; i < 6; i++ ) {
								*dstPtr = m1Ptr[0] * m2Ptr[ 0 * 1 ]
										+ m1Ptr[1] * m2Ptr[ 1 * 1 ]
										+ m1Ptr[2] * m2Ptr[ 2 * 1 ]
										+ m1Ptr[3] * m2Ptr[ 3 * 1 ]
										+ m1Ptr[4] * m2Ptr[ 4 * 1 ]
										+ m1Ptr[5] * m2Ptr[ 5 * 1 ];
								dstPtr++;
								m1Ptr += 6;
							}
							return;
						}
						case 2: {		// 6x6 * 6x2
							for ( i = 0; i < 6; i++ ) {
								for ( j = 0; j < 2; j++ ) {
									*dstPtr = m1Ptr[0] * m2Ptr[ 0 * 2 + j ]
											+ m1Ptr[1] * m2Ptr[ 1 * 2 + j ]
											+ m1Ptr[2] * m2Ptr[ 2 * 2 + j ]
											+ m1Ptr[3] * m2Ptr[ 3 * 2 + j ]
											+ m1Ptr[4] * m2Ptr[ 4 * 2 + j ]
											+ m1Ptr[5] * m2Ptr[ 5 * 2 + j ];
									dstPtr++;
								}
								m1Ptr += 6;
							}
							return;
						}
						case 3: {		// 6x6 * 6x3
							for ( i = 0; i < 6; i++ ) {
								for ( j = 0; j < 3; j++ ) {
									*dstPtr = m1Ptr[0] * m2Ptr[ 0 * 3 + j ]
											+ m1Ptr[1] * m2Ptr[ 1 * 3 + j ]
											+ m1Ptr[2] * m2Ptr[ 2 * 3 + j ]
											+ m1Ptr[3] * m2Ptr[ 3 * 3 + j ]
											+ m1Ptr[4] * m2Ptr[ 4 * 3 + j ]
											+ m1Ptr[5] * m2Ptr[ 5 * 3 + j ];
									dstPtr++;
								}
								m1Ptr += 6;
							}
							return;
						}
						case 4: {		// 6x6 * 6x4
							for ( i = 0; i < 6; i++ ) {
								for ( j = 0; j < 4; j++ ) {
									*dstPtr = m1Ptr[0] * m2Ptr[ 0 * 4 + j ]
											+ m1Ptr[1] * m2Ptr[ 1 * 4 + j ]
											+ m1Ptr[2] * m2Ptr[ 2 * 4 + j ]
											+ m1Ptr[3] * m2Ptr[ 3 * 4 + j ]
											+ m1Ptr[4] * m2Ptr[ 4 * 4 + j ]
											+ m1Ptr[5] * m2Ptr[ 5 * 4 + j ];
									dstPtr++;
								}
								m1Ptr += 6;
							}
							return;
						}
						case 5: {		// 6x6 * 6x5
							for ( i = 0; i < 6; i++ ) {
								for ( j = 0; j < 5; j++ ) {
									*dstPtr = m1Ptr[0] * m2Ptr[ 0 * 5 + j ]
											+ m1Ptr[1] * m2Ptr[ 1 * 5 + j ]
											+ m1Ptr[2] * m2Ptr[ 2 * 5 + j ]
											+ m1Ptr[3] * m2Ptr[ 3 * 5 + j ]
											+ m1Ptr[4] * m2Ptr[ 4 * 5 + j ]
											+ m1Ptr[5] * m2Ptr[ 5 * 5 + j ];
									dstPtr++;
								}
								m1Ptr += 6;
							}
							return;
						}
						case 6: {		// 6x6 * 6x6
							for ( i = 0; i < 6; i++ ) {
								for ( j = 0; j < 6; j++ ) {
									*dstPtr = m1Ptr[0] * m2Ptr[ 0 * 6 + j ]
											+ m1Ptr[1] * m2Ptr[ 1 * 6 + j ]
											+ m1Ptr[2] * m2Ptr[ 2 * 6 + j ]
											+ m1Ptr[3] * m2Ptr[ 3 * 6 + j ]
											+ m1Ptr[4] * m2Ptr[ 4 * 6 + j ]
											+ m1Ptr[5] * m2Ptr[ 5 * 6 + j ];
									dstPtr++;
								}
								m1Ptr += 6;
							}
							return;
						}
					}
				}
			}
			for ( i = 0; i < k; i++ ) {
				m2Ptr = m2.ToFloatPtr();
				for ( j = 0; j < l; j++ ) {
					*dstPtr++ = m1Ptr[0] * m2Ptr[0] + m1Ptr[1] * m2Ptr[l] + m1Ptr[2] * m2Ptr[2*l] +
									 m1Ptr[3] * m2Ptr[3*l] + m1Ptr[4] * m2Ptr[4*l] + m1Ptr[5] * m2Ptr[5*l];
					m2Ptr++;
				}
				m1Ptr += 6;
			}
			break;
		}
		default: {
			int nc = m1.GetNumColumns();
			ptrdiff_t stride = l * sizeof( float );
			for ( i = 0; i < k; i++ ) {
				for ( j = 0; j < l; j++ ) {
					m2Ptr = m2.ToFloatPtr() + j;
					double sum = 0.0;
					int n = 0;
					while ( n < nc ) {
						size_t vl = __riscv_vsetvl_e32m8( nc - n );
						vfloat32m8_t m1v = __riscv_vle32_v_f32m8( m1Ptr + n, vl );
						vfloat32m8_t m2v = __riscv_vlse32_v_f32m8( m2Ptr, stride, vl );
						vfloat32m1_t acc = __riscv_vfmv_s_f_f32m1( 0.0f, 1 );
						acc = __riscv_vfredusum_vs_f32m8_f32m1( __riscv_vfmul_vv_f32m8( m1v, m2v, vl ), acc, vl );
						sum += __riscv_vfmv_f_s_f32m1_f32( acc );
						m2Ptr += vl * l;
						n += vl;
					}
					*dstPtr++ = (float)sum;
				}
				m1Ptr += nc;
			}
			break;
		}
	}
}

void VPCALL idSIMD_RVV::MatX_TransposeMultiplyMatX( idMatX &dst, const idMatX &m1, const idMatX &m2 ) {
	int i, j, k, l;
	float *dstPtr;
	const float *m1Ptr, *m2Ptr;

	assert( m1.GetNumRows() == m2.GetNumRows() );

	m1Ptr = m1.ToFloatPtr();
	m2Ptr = m2.ToFloatPtr();
	dstPtr = dst.ToFloatPtr();
	k = m1.GetNumColumns();
	l = m2.GetNumColumns();

	switch( m1.GetNumRows() ) {
		case 1:
			if ( k == 6 && l == 1 ) {			// 1x6 * 1x1
				for ( i = 0; i < 6; i++ ) {
					*dstPtr++ = m1Ptr[0] * m2Ptr[0];
					m1Ptr++;
				}
				return;
			}
			for ( i = 0; i < k; i++ ) {
				m2Ptr = m2.ToFloatPtr();
				for ( j = 0; j < l; j++ ) {
					*dstPtr++ = m1Ptr[0] * m2Ptr[0];
					m2Ptr++;
				}
				m1Ptr++;
			}
			break;
		case 2:
			if ( k == 6 && l == 2 ) {			// 2x6 * 2x2
				for ( i = 0; i < 6; i++ ) {
					*dstPtr++ = m1Ptr[0*6] * m2Ptr[0*2+0] + m1Ptr[1*6] * m2Ptr[1*2+0];
					*dstPtr++ = m1Ptr[0*6] * m2Ptr[0*2+1] + m1Ptr[1*6] * m2Ptr[1*2+1];
					m1Ptr++;
				}
				return;
			}
			for ( i = 0; i < k; i++ ) {
				m2Ptr = m2.ToFloatPtr();
				for ( j = 0; j < l; j++ ) {
					*dstPtr++ = m1Ptr[0] * m2Ptr[0] + m1Ptr[k] * m2Ptr[l];
					m2Ptr++;
				}
				m1Ptr++;
			}
			break;
		case 3:
			if ( k == 6 && l == 3 ) {			// 3x6 * 3x3
				for ( i = 0; i < 6; i++ ) {
					*dstPtr++ = m1Ptr[0*6] * m2Ptr[0*3+0] + m1Ptr[1*6] * m2Ptr[1*3+0] + m1Ptr[2*6] * m2Ptr[2*3+0];
					*dstPtr++ = m1Ptr[0*6] * m2Ptr[0*3+1] + m1Ptr[1*6] * m2Ptr[1*3+1] + m1Ptr[2*6] * m2Ptr[2*3+1];
					*dstPtr++ = m1Ptr[0*6] * m2Ptr[0*3+2] + m1Ptr[1*6] * m2Ptr[1*3+2] + m1Ptr[2*6] * m2Ptr[2*3+2];
					m1Ptr++;
				}
				return;
			}
			for ( i = 0; i < k; i++ ) {
				m2Ptr = m2.ToFloatPtr();
				for ( j = 0; j < l; j++ ) {
					*dstPtr++ = m1Ptr[0] * m2Ptr[0] + m1Ptr[k] * m2Ptr[l] + m1Ptr[2*k] * m2Ptr[2*l];
					m2Ptr++;
				}
				m1Ptr++;
			}
			break;
		case 4:
			if ( k == 6 && l == 4 ) {			// 4x6 * 4x4
				for ( i = 0; i < 6; i++ ) {
					*dstPtr++ = m1Ptr[0*6] * m2Ptr[0*4+0] + m1Ptr[1*6] * m2Ptr[1*4+0] + m1Ptr[2*6] * m2Ptr[2*4+0] + m1Ptr[3*6] * m2Ptr[3*4+0];
					*dstPtr++ = m1Ptr[0*6] * m2Ptr[0*4+1] + m1Ptr[1*6] * m2Ptr[1*4+1] + m1Ptr[2*6] * m2Ptr[2*4+1] + m1Ptr[3*6] * m2Ptr[3*4+1];
					*dstPtr++ = m1Ptr[0*6] * m2Ptr[0*4+2] + m1Ptr[1*6] * m2Ptr[1*4+2] + m1Ptr[2*6] * m2Ptr[2*4+2] + m1Ptr[3*6] * m2Ptr[3*4+2];
					*dstPtr++ = m1Ptr[0*6] * m2Ptr[0*4+3] + m1Ptr[1*6] * m2Ptr[1*4+3] + m1Ptr[2*6] * m2Ptr[2*4+3] + m1Ptr[3*6] * m2Ptr[3*4+3];
					m1Ptr++;
				}
				return;
			}
			for ( i = 0; i < k; i++ ) {
				m2Ptr = m2.ToFloatPtr();
				for ( j = 0; j < l; j++ ) {
					*dstPtr++ = m1Ptr[0] * m2Ptr[0] + m1Ptr[k] * m2Ptr[l] + m1Ptr[2*k] * m2Ptr[2*l] +
									m1Ptr[3*k] * m2Ptr[3*l];
					m2Ptr++;
				}
				m1Ptr++;
			}
			break;
		case 5:
			if ( k == 6 && l == 5 ) {			// 5x6 * 5x5
				for ( i = 0; i < 6; i++ ) {
					*dstPtr++ = m1Ptr[0*6] * m2Ptr[0*5+0] + m1Ptr[1*6] * m2Ptr[1*5+0] + m1Ptr[2*6] * m2Ptr[2*5+0] + m1Ptr[3*6] * m2Ptr[3*5+0] + m1Ptr[4*6] * m2Ptr[4*5+0];
					*dstPtr++ = m1Ptr[0*6] * m2Ptr[0*5+1] + m1Ptr[1*6] * m2Ptr[1*5+1] + m1Ptr[2*6] * m2Ptr[2*5+1] + m1Ptr[3*6] * m2Ptr[3*5+1] + m1Ptr[4*6] * m2Ptr[4*5+1];
					*dstPtr++ = m1Ptr[0*6] * m2Ptr[0*5+2] + m1Ptr[1*6] * m2Ptr[1*5+2] + m1Ptr[2*6] * m2Ptr[2*5+2] + m1Ptr[3*6] * m2Ptr[3*5+2] + m1Ptr[4*6] * m2Ptr[4*5+2];
					*dstPtr++ = m1Ptr[0*6] * m2Ptr[0*5+3] + m1Ptr[1*6] * m2Ptr[1*5+3] + m1Ptr[2*6] * m2Ptr[2*5+3] + m1Ptr[3*6] * m2Ptr[3*5+3] + m1Ptr[4*6] * m2Ptr[4*5+3];
					*dstPtr++ = m1Ptr[0*6] * m2Ptr[0*5+4] + m1Ptr[1*6] * m2Ptr[1*5+4] + m1Ptr[2*6] * m2Ptr[2*5+4] + m1Ptr[3*6] * m2Ptr[3*5+4] + m1Ptr[4*6] * m2Ptr[4*5+4];
					m1Ptr++;
				}
				return;
			}
			for ( i = 0; i < k; i++ ) {
				m2Ptr = m2.ToFloatPtr();
				for ( j = 0; j < l; j++ ) {
					*dstPtr++ = m1Ptr[0] * m2Ptr[0] + m1Ptr[k] * m2Ptr[l] + m1Ptr[2*k] * m2Ptr[2*l] +
									m1Ptr[3*k] * m2Ptr[3*l] + m1Ptr[4*k] * m2Ptr[4*l];
					m2Ptr++;
				}
				m1Ptr++;
			}
			break;
		case 6:
			if ( l == 6 ) {
				switch( k ) {
					case 1:						// 6x1 * 6x6
						m2Ptr = m2.ToFloatPtr();
						for ( j = 0; j < 6; j++ ) {
							*dstPtr++ = m1Ptr[0*1] * m2Ptr[0*6] +
										m1Ptr[1*1] * m2Ptr[1*6] +
										m1Ptr[2*1] * m2Ptr[2*6] +
										m1Ptr[3*1] * m2Ptr[3*6] +
										m1Ptr[4*1] * m2Ptr[4*6] +
										m1Ptr[5*1] * m2Ptr[5*6];
							m2Ptr++;
						}
						return;
					case 2:						// 6x2 * 6x6
						for ( i = 0; i < 2; i++ ) {
							m2Ptr = m2.ToFloatPtr();
							for ( j = 0; j < 6; j++ ) {
								*dstPtr++ = m1Ptr[0*2] * m2Ptr[0*6] +
											m1Ptr[1*2] * m2Ptr[1*6] +
											m1Ptr[2*2] * m2Ptr[2*6] +
											m1Ptr[3*2] * m2Ptr[3*6] +
											m1Ptr[4*2] * m2Ptr[4*6] +
											m1Ptr[5*2] * m2Ptr[5*6];
								m2Ptr++;
							}
							m1Ptr++;
						}
						return;
					case 3:						// 6x3 * 6x6
						for ( i = 0; i < 3; i++ ) {
							m2Ptr = m2.ToFloatPtr();
							for ( j = 0; j < 6; j++ ) {
								*dstPtr++ = m1Ptr[0*3] * m2Ptr[0*6] +
											m1Ptr[1*3] * m2Ptr[1*6] +
											m1Ptr[2*3] * m2Ptr[2*6] +
											m1Ptr[3*3] * m2Ptr[3*6] +
											m1Ptr[4*3] * m2Ptr[4*6] +
											m1Ptr[5*3] * m2Ptr[5*6];
								m2Ptr++;
							}
							m1Ptr++;
						}
						return;
					case 4:						// 6x4 * 6x6
						for ( i = 0; i < 4; i++ ) {
							m2Ptr = m2.ToFloatPtr();
							for ( j = 0; j < 6; j++ ) {
								*dstPtr++ = m1Ptr[0*4] * m2Ptr[0*6] +
											m1Ptr[1*4] * m2Ptr[1*6] +
											m1Ptr[2*4] * m2Ptr[2*6] +
											m1Ptr[3*4] * m2Ptr[3*6] +
											m1Ptr[4*4] * m2Ptr[4*6] +
											m1Ptr[5*4] * m2Ptr[5*6];
								m2Ptr++;
							}
							m1Ptr++;
						}
						return;
					case 5:						// 6x5 * 6x6
						for ( i = 0; i < 5; i++ ) {
							m2Ptr = m2.ToFloatPtr();
							for ( j = 0; j < 6; j++ ) {
								*dstPtr++ = m1Ptr[0*5] * m2Ptr[0*6] +
											m1Ptr[1*5] * m2Ptr[1*6] +
											m1Ptr[2*5] * m2Ptr[2*6] +
											m1Ptr[3*5] * m2Ptr[3*6] +
											m1Ptr[4*5] * m2Ptr[4*6] +
											m1Ptr[5*5] * m2Ptr[5*6];
								m2Ptr++;
							}
							m1Ptr++;
						}
						return;
					case 6:						// 6x6 * 6x6
						for ( i = 0; i < 6; i++ ) {
							m2Ptr = m2.ToFloatPtr();
							for ( j = 0; j < 6; j++ ) {
								*dstPtr++ = m1Ptr[0*6] * m2Ptr[0*6] +
											m1Ptr[1*6] * m2Ptr[1*6] +
											m1Ptr[2*6] * m2Ptr[2*6] +
											m1Ptr[3*6] * m2Ptr[3*6] +
											m1Ptr[4*6] * m2Ptr[4*6] +
											m1Ptr[5*6] * m2Ptr[5*6];
								m2Ptr++;
							}
							m1Ptr++;
						}
						return;
				}
			}
			for ( i = 0; i < k; i++ ) {
				m2Ptr = m2.ToFloatPtr();
				for ( j = 0; j < l; j++ ) {
					*dstPtr++ = m1Ptr[0] * m2Ptr[0] + m1Ptr[k] * m2Ptr[l] + m1Ptr[2*k] * m2Ptr[2*l] +
									m1Ptr[3*k] * m2Ptr[3*l] + m1Ptr[4*k] * m2Ptr[4*l] + m1Ptr[5*k] * m2Ptr[5*l];
					m2Ptr++;
				}
				m1Ptr++;
			}
			break;
		default:
			int nr = m1.GetNumRows();
			ptrdiff_t stride1 = k * sizeof( float );
			ptrdiff_t stride2 = l * sizeof( float );
			for ( i = 0; i < k; i++ ) {
				for ( j = 0; j < l; j++ ) {
					m1Ptr = m1.ToFloatPtr() + i;
					m2Ptr = m2.ToFloatPtr() + j;
					double sum = 0.0;
					int n = 0;
					while ( n < nr ) {
						size_t vl = __riscv_vsetvl_e32m8( nr - n );
						vfloat32m8_t m1v = __riscv_vlse32_v_f32m8( m1Ptr, stride1, vl );
						vfloat32m8_t m2v = __riscv_vlse32_v_f32m8( m2Ptr, stride2, vl );
						vfloat32m1_t acc = __riscv_vfmv_s_f_f32m1( 0.0f, 1 );
						acc = __riscv_vfredusum_vs_f32m8_f32m1( __riscv_vfmul_vv_f32m8( m1v, m2v, vl ), acc, vl );
						sum += __riscv_vfmv_f_s_f32m1_f32( acc );
						m1Ptr += vl * k;
						m2Ptr += vl * l;
						n += vl;
					}
					*dstPtr++ = (float)sum;
				}
			}
		break;
	}
}

void VPCALL idSIMD_RVV::MatX_LowerTriangularSolve( const idMatX &L, float *x, const float *b, const int n, int skip ) {
	idSIMD_Generic::MatX_LowerTriangularSolve( L, x, b, n, skip );
}

void VPCALL idSIMD_RVV::MatX_LowerTriangularSolveTranspose( const idMatX &L, float *x, const float *b, const int n ) {
	idSIMD_Generic::MatX_LowerTriangularSolveTranspose( L, x, b, n );
}

bool VPCALL idSIMD_RVV::MatX_LDLTFactor( idMatX &mat, idVecX &invDiag, const int n ) {
	return idSIMD_Generic::MatX_LDLTFactor( mat, invDiag, n );
}

#else

void VPCALL idSIMD_RVV::Add( float *dst, const float constant, const float *src, const int count ) { idSIMD_Generic::Add( dst, constant, src, count ); }
void VPCALL idSIMD_RVV::Add( float *dst, const float *src0, const float *src1, const int count ) { idSIMD_Generic::Add( dst, src0, src1, count ); }
void VPCALL idSIMD_RVV::Sub( float *dst, const float constant, const float *src, const int count ) { idSIMD_Generic::Sub( dst, constant, src, count ); }
void VPCALL idSIMD_RVV::Sub( float *dst, const float *src0, const float *src1, const int count ) { idSIMD_Generic::Sub( dst, src0, src1, count ); }
void VPCALL idSIMD_RVV::Mul( float *dst, const float constant, const float *src, const int count ) { idSIMD_Generic::Mul( dst, constant, src, count ); }
void VPCALL idSIMD_RVV::Mul( float *dst, const float *src0, const float *src1, const int count ) { idSIMD_Generic::Mul( dst, src0, src1, count ); }
void VPCALL idSIMD_RVV::Div( float *dst, const float constant, const float *src, const int count ) { idSIMD_Generic::Div( dst, constant, src, count ); }
void VPCALL idSIMD_RVV::Div( float *dst, const float *src0, const float *src1, const int count ) { idSIMD_Generic::Div( dst, src0, src1, count ); }
void VPCALL idSIMD_RVV::MulAdd( float *dst, const float constant, const float *src, const int count ) { idSIMD_Generic::MulAdd( dst, constant, src, count ); }
void VPCALL idSIMD_RVV::MulAdd( float *dst, const float *src0, const float *src1, const int count ) { idSIMD_Generic::MulAdd( dst, src0, src1, count ); }
void VPCALL idSIMD_RVV::MulSub( float *dst, const float constant, const float *src, const int count ) { idSIMD_Generic::MulSub( dst, constant, src, count ); }
void VPCALL idSIMD_RVV::MulSub( float *dst, const float *src0, const float *src1, const int count ) { idSIMD_Generic::MulSub( dst, src0, src1, count ); }
void VPCALL idSIMD_RVV::Dot( float *dst, const idVec3 &constant, const idVec3 *src, const int count ) { idSIMD_Generic::Dot( dst, constant, src, count ); }
void VPCALL idSIMD_RVV::Dot( float *dst, const idVec3 &constant, const idPlane *src, const int count ) { idSIMD_Generic::Dot( dst, constant, src, count ); }
void VPCALL idSIMD_RVV::Dot( float *dst, const idVec3 &constant, const idDrawVert *src, const int count ) { idSIMD_Generic::Dot( dst, constant, src, count ); }
void VPCALL idSIMD_RVV::Dot( float *dst, const idPlane &constant, const idVec3 *src, const int count ) { idSIMD_Generic::Dot( dst, constant, src, count ); }
void VPCALL idSIMD_RVV::Dot( float *dst, const idPlane &constant, const idPlane *src, const int count ) { idSIMD_Generic::Dot( dst, constant, src, count ); }
void VPCALL idSIMD_RVV::Dot( float *dst, const idPlane &constant, const idDrawVert *src, const int count ) { idSIMD_Generic::Dot( dst, constant, src, count ); }
void VPCALL idSIMD_RVV::Dot( float *dst, const idVec3 *src0, const idVec3 *src1, const int count ) { idSIMD_Generic::Dot( dst, src0, src1, count ); }
void VPCALL idSIMD_RVV::Dot( float &dot, const float *src1, const float *src2, const int count ) { idSIMD_Generic::Dot( dot, src1, src2, count ); }
void VPCALL idSIMD_RVV::MinMax( float &min, float &max, const float *src, const int count ) { idSIMD_Generic::MinMax( min, max, src, count ); }
void VPCALL idSIMD_RVV::MinMax( idVec2 &min, idVec2 &max, const idVec2 *src, const int count ) { idSIMD_Generic::MinMax( min, max, src, count ); }
void VPCALL idSIMD_RVV::MinMax( idVec3 &min, idVec3 &max, const idVec3 *src, const int count ) { idSIMD_Generic::MinMax( min, max, src, count ); }
void VPCALL idSIMD_RVV::MinMax( idVec3 &min, idVec3 &max, const idDrawVert *src, const int count ) { idSIMD_Generic::MinMax( min, max, src, count ); }
void VPCALL idSIMD_RVV::MinMax( idVec3 &min, idVec3 &max, const idDrawVert *src, const int *indexes, const int count ) { idSIMD_Generic::MinMax( min, max, src, indexes, count ); }
void VPCALL idSIMD_RVV::CmpGT( byte *dst, const float *src0, const float constant, const int count ) { idSIMD_Generic::CmpGT( dst, src0, constant, count ); }
void VPCALL idSIMD_RVV::CmpGT( byte *dst, const byte bitNum, const float *src0, const float constant, const int count ) { idSIMD_Generic::CmpGT( dst, bitNum, src0, constant, count ); }
void VPCALL idSIMD_RVV::CmpGE( byte *dst, const float *src0, const float constant, const int count ) { idSIMD_Generic::CmpGE( dst, src0, constant, count ); }
void VPCALL idSIMD_RVV::CmpGE( byte *dst, const byte bitNum, const float *src0, const float constant, const int count ) { idSIMD_Generic::CmpGE( dst, bitNum, src0, constant, count ); }
void VPCALL idSIMD_RVV::CmpLT( byte *dst, const float *src0, const float constant, const int count ) { idSIMD_Generic::CmpLT( dst, src0, constant, count ); }
void VPCALL idSIMD_RVV::CmpLT( byte *dst, const byte bitNum, const float *src0, const float constant, const int count ) { idSIMD_Generic::CmpLT( dst, bitNum, src0, constant, count ); }
void VPCALL idSIMD_RVV::CmpLE( byte *dst, const float *src0, const float constant, const int count ) { idSIMD_Generic::CmpLE( dst, src0, constant, count ); }
void VPCALL idSIMD_RVV::CmpLE( byte *dst, const byte bitNum, const float *src0, const float constant, const int count ) { idSIMD_Generic::CmpLE( dst, bitNum, src0, constant, count ); }
void VPCALL idSIMD_RVV::Clamp( float *dst, const float *src, const float min, const float max, const int count ) { idSIMD_Generic::Clamp( dst, src, min, max, count ); }
void VPCALL idSIMD_RVV::ClampMin( float *dst, const float *src, const float min, const int count ) { idSIMD_Generic::ClampMin( dst, src, min, count ); }
void VPCALL idSIMD_RVV::ClampMax( float *dst, const float *src, const float max, const int count ) { idSIMD_Generic::ClampMax( dst, src, max, count ); }
void VPCALL idSIMD_RVV::Memcpy( void *dst, const void *src, const int count ) { idSIMD_Generic::Memcpy( dst, src, count ); }
void VPCALL idSIMD_RVV::Memset( void *dst, const int val, const int count ) { idSIMD_Generic::Memset( dst, val, count ); }
void VPCALL idSIMD_RVV::Zero16( float *dst, const int count ) { idSIMD_Generic::Zero16( dst, count ); }
void VPCALL idSIMD_RVV::Negate16( float *dst, const int count ) { idSIMD_Generic::Negate16( dst, count ); }
void VPCALL idSIMD_RVV::Copy16( float *dst, const float *src, const int count ) { idSIMD_Generic::Copy16( dst, src, count ); }
void VPCALL idSIMD_RVV::Add16( float *dst, const float *src1, const float *src2, const int count ) { idSIMD_Generic::Add16( dst, src1, src2, count ); }
void VPCALL idSIMD_RVV::Sub16( float *dst, const float *src1, const float *src2, const int count ) { idSIMD_Generic::Sub16( dst, src1, src2, count ); }
void VPCALL idSIMD_RVV::Mul16( float *dst, const float *src1, const float constant, const int count ) { idSIMD_Generic::Mul16( dst, src1, constant, count ); }
void VPCALL idSIMD_RVV::AddAssign16( float *dst, const float *src, const int count ) { idSIMD_Generic::AddAssign16( dst, src, count ); }
void VPCALL idSIMD_RVV::SubAssign16( float *dst, const float *src, const int count ) { idSIMD_Generic::SubAssign16( dst, src, count ); }
void VPCALL idSIMD_RVV::MulAssign16( float *dst, const float constant, const int count ) { idSIMD_Generic::MulAssign16( dst, constant, count ); }
int VPCALL idSIMD_RVV::CreateVertexProgramShadowCache( idVec4 *vertexCache, const idDrawVert *verts, const int numVerts ) { return idSIMD_Generic::CreateVertexProgramShadowCache( vertexCache, verts, numVerts ); }
void VPCALL idSIMD_RVV::MixSoundTwoSpeakerMono( float *mixBuffer, const float *samples, const int numSamples, const float lastV[2], const float currentV[2] ) { idSIMD_Generic::MixSoundTwoSpeakerMono( mixBuffer, samples, numSamples, lastV, currentV ); }
void VPCALL idSIMD_RVV::MixSoundTwoSpeakerStereo( float *mixBuffer, const float *samples, const int numSamples, const float lastV[2], const float currentV[2] ) { idSIMD_Generic::MixSoundTwoSpeakerStereo( mixBuffer, samples, numSamples, lastV, currentV ); }
void VPCALL idSIMD_RVV::MixedSoundToSamples( short *samples, const float *mixBuffer, const int numSamples ) { idSIMD_Generic::MixedSoundToSamples( samples, mixBuffer, numSamples ); }


void VPCALL idSIMD_RVV::BlendJoints( idJointQuat *joints, const idJointQuat *blendJoints, const float lerp, const int *index, const int numJoints ) { idSIMD_Generic::BlendJoints( joints, blendJoints, lerp, index, numJoints ); }
void VPCALL idSIMD_RVV::ConvertJointQuatsToJointMats( idJointMat *jointMats, const idJointQuat *jointQuats, const int numJoints ) { idSIMD_Generic::ConvertJointQuatsToJointMats( jointMats, jointQuats, numJoints ); }
void VPCALL idSIMD_RVV::ConvertJointMatsToJointQuats( idJointQuat *jointQuats, const idJointMat *jointMats, const int numJoints ) { idSIMD_Generic::ConvertJointMatsToJointQuats( jointQuats, jointMats, numJoints ); }
void VPCALL idSIMD_RVV::TransformJoints( idJointMat *jointMats, const int *parents, const int firstJoint, const int lastJoint ) { idSIMD_Generic::TransformJoints( jointMats, parents, firstJoint, lastJoint ); }
void VPCALL idSIMD_RVV::UntransformJoints( idJointMat *jointMats, const int *parents, const int firstJoint, const int lastJoint ) { idSIMD_Generic::UntransformJoints( jointMats, parents, firstJoint, lastJoint ); }
void VPCALL idSIMD_RVV::TransformVerts( idDrawVert *verts, const int numVerts, const idJointMat *joints, const idVec4 *weights, const int *index, const int numWeights ) { idSIMD_Generic::TransformVerts( verts, numVerts, joints, weights, index, numWeights ); }
void VPCALL idSIMD_RVV::TracePointCull( byte *cullBits, byte &totalOr, const float radius, const idPlane *planes, const idDrawVert *verts, const int numVerts ) { idSIMD_Generic::TracePointCull( cullBits, totalOr, radius, planes, verts, numVerts ); }
void VPCALL idSIMD_RVV::DecalPointCull( byte *cullBits, const idPlane *planes, const idDrawVert *verts, const int numVerts ) { idSIMD_Generic::DecalPointCull( cullBits, planes, verts, numVerts ); }
void VPCALL idSIMD_RVV::OverlayPointCull( byte *cullBits, idVec2 *texCoords, const idPlane *planes, const idDrawVert *verts, const int numVerts ) { idSIMD_Generic::OverlayPointCull( cullBits, texCoords, planes, verts, numVerts ); }
void VPCALL idSIMD_RVV::DeriveTriPlanes( idPlane *planes, const idDrawVert *verts, const int numVerts, const int *indexes, const int numIndexes ) { idSIMD_Generic::DeriveTriPlanes( planes, verts, numVerts, indexes, numIndexes ); }
void VPCALL idSIMD_RVV::DeriveTangents( idPlane *planes, idDrawVert *verts, const int numVerts, const int *indexes, const int numIndexes ) { idSIMD_Generic::DeriveTangents( planes, verts, numVerts, indexes, numIndexes ); }
void VPCALL idSIMD_RVV::DeriveUnsmoothedTangents( idDrawVert *verts, const dominantTri_s *dominantTris, const int numVerts ) { idSIMD_Generic::DeriveUnsmoothedTangents( verts, dominantTris, numVerts ); }
void VPCALL idSIMD_RVV::NormalizeTangents( idDrawVert *verts, const int numVerts ) { idSIMD_Generic::NormalizeTangents( verts, numVerts ); }
void VPCALL idSIMD_RVV::CreateTextureSpaceLightVectors( idVec3 *lightVectors, const idVec3 &lightOrigin, const idDrawVert *verts, const int numVerts, const int *indexes, const int numIndexes ) { idSIMD_Generic::CreateTextureSpaceLightVectors( lightVectors, lightOrigin, verts, numVerts, indexes, numIndexes ); }
void VPCALL idSIMD_RVV::CreateSpecularTextureCoords( idVec4 *texCoords, const idVec3 &lightOrigin, const idVec3 &viewOrigin, const idDrawVert *verts, const int numVerts, const int *indexes, const int numIndexes ) { idSIMD_Generic::CreateSpecularTextureCoords( texCoords, lightOrigin, viewOrigin, verts, numVerts, indexes, numIndexes ); }
int VPCALL idSIMD_RVV::CreateShadowCache( idVec4 *vertexCache, int *vertRemap, const idVec3 &lightOrigin, const idDrawVert *verts, const int numVerts ) { return idSIMD_Generic::CreateShadowCache( vertexCache, vertRemap, lightOrigin, verts, numVerts ); }
void VPCALL idSIMD_RVV::UpSamplePCMTo44kHz( float *dest, const short *src, const int numSamples, const int kHz, const int numChannels ) { idSIMD_Generic::UpSamplePCMTo44kHz( dest, src, numSamples, kHz, numChannels ); }
void VPCALL idSIMD_RVV::UpSampleOGGTo44kHz( float *dest, const float * const *ogg, const int numSamples, const int kHz, const int numChannels ) { idSIMD_Generic::UpSampleOGGTo44kHz( dest, ogg, numSamples, kHz, numChannels ); }
void VPCALL idSIMD_RVV::MixSoundSixSpeakerMono( float *mixBuffer, const float *samples, const int numSamples, const float lastV[6], const float currentV[6] ) { idSIMD_Generic::MixSoundSixSpeakerMono( mixBuffer, samples, numSamples, lastV, currentV ); }
void VPCALL idSIMD_RVV::MixSoundSixSpeakerStereo( float *mixBuffer, const float *samples, const int numSamples, const float lastV[6], const float currentV[6] ) { idSIMD_Generic::MixSoundSixSpeakerStereo( mixBuffer, samples, numSamples, lastV, currentV ); }
void VPCALL idSIMD_RVV::MatX_MultiplyVecX( idVecX &dst, const idMatX &mat, const idVecX &vec ) { idSIMD_Generic::MatX_MultiplyVecX( dst, mat, vec ); }
void VPCALL idSIMD_RVV::MatX_MultiplyAddVecX( idVecX &dst, const idMatX &mat, const idVecX &vec ) { idSIMD_Generic::MatX_MultiplyAddVecX( dst, mat, vec ); }
void VPCALL idSIMD_RVV::MatX_MultiplySubVecX( idVecX &dst, const idMatX &mat, const idVecX &vec ) { idSIMD_Generic::MatX_MultiplySubVecX( dst, mat, vec ); }
void VPCALL idSIMD_RVV::MatX_TransposeMultiplyVecX( idVecX &dst, const idMatX &mat, const idVecX &vec ) { idSIMD_Generic::MatX_TransposeMultiplyVecX( dst, mat, vec ); }
void VPCALL idSIMD_RVV::MatX_TransposeMultiplyAddVecX( idVecX &dst, const idMatX &mat, const idVecX &vec ) { idSIMD_Generic::MatX_TransposeMultiplyAddVecX( dst, mat, vec ); }
void VPCALL idSIMD_RVV::MatX_TransposeMultiplySubVecX( idVecX &dst, const idMatX &mat, const idVecX &vec ) { idSIMD_Generic::MatX_TransposeMultiplySubVecX( dst, mat, vec ); }
void VPCALL idSIMD_RVV::MatX_MultiplyMatX( idMatX &dst, const idMatX &m1, const idMatX &m2 ) { idSIMD_Generic::MatX_MultiplyMatX( dst, m1, m2 ); }
void VPCALL idSIMD_RVV::MatX_TransposeMultiplyMatX( idMatX &dst, const idMatX &m1, const idMatX &m2 ) { idSIMD_Generic::MatX_TransposeMultiplyMatX( dst, m1, m2 ); }
void VPCALL idSIMD_RVV::MatX_LowerTriangularSolve( const idMatX &L, float *x, const float *b, const int n, int skip ) { idSIMD_Generic::MatX_LowerTriangularSolve( L, x, b, n, skip ); }
void VPCALL idSIMD_RVV::MatX_LowerTriangularSolveTranspose( const idMatX &L, float *x, const float *b, const int n ) { idSIMD_Generic::MatX_LowerTriangularSolveTranspose( L, x, b, n ); }
bool VPCALL idSIMD_RVV::MatX_LDLTFactor( idMatX &mat, idVecX &invDiag, const int n ) { return idSIMD_Generic::MatX_LDLTFactor( mat, invDiag, n ); }

#endif

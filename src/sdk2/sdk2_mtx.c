// SDK2 MTX/VEC: portable C re-implementation of the Dolphin matrix/vector
// library (the originals are paired-single ASM and don't compile on PC).
//
// Conventions match retail/libogc observable behavior:
// - Mtx is row-major 3x4 affine; Mtx44 row-major 4x4.
// - MTXPerspective/MTXFrustum/MTXOrtho produce standard clip matrices that
//   GXSetProjection() transposes into GL column-major (see gx_hal.c).
// - Every PS* name is provided as an alias of the C implementation
//   (release builds call the PS* names via mtx.h macros).

#include <dolphin/mtx.h>

#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ------------------------------------------------------------ projection
void MTXFrustum(Mtx m, f32 t, f32 b, f32 l, f32 r, f32 n, f32 f)
{
    memset(m, 0, sizeof(Mtx));
    m[0][0] = (2.f * n) / (r - l);
    m[0][2] = (r + l) / (r - l);
    m[1][1] = (2.f * n) / (t - b);
    m[1][2] = (t + b) / (t - b);
    m[2][2] = -(f + n) / (f - n);
    m[2][3] = -(2.f * f * n) / (f - n);
}

void MTXPerspective(Mtx m, f32 fovY, f32 aspect, f32 n, f32 f)
{
    f32 cot = 1.f / tanf(fovY * (f32) (M_PI / 360.0));
    memset(m, 0, sizeof(Mtx));
    m[0][0] = cot / aspect;
    m[1][1] = cot;
    m[2][2] = -(f + n) / (f - n);
    m[2][3] = -(2.f * f * n) / (f - n);
}

void MTXOrtho(Mtx m, f32 t, f32 b, f32 l, f32 r, f32 n, f32 f)
{
    memset(m, 0, sizeof(Mtx));
    m[0][0] = 2.f / (r - l);
    m[0][3] = -(r + l) / (r - l);
    m[1][1] = 2.f / (t - b);
    m[1][3] = -(t + b) / (t - b);
    m[2][2] = -2.f / (f - n);
    m[2][3] = -(f + n) / (f - n);
}

void MTXLightFrustum(Mtx m, f32 t, f32 b, f32 l, f32 r, f32 n, f32 scaleS,
                     f32 scaleT, f32 transS, f32 transT)
{
    MTXFrustum(m, t, b, l, r, n, 1.f);
    m[0][0] *= scaleS;
    m[0][1] *= scaleS;
    m[0][2] *= scaleS;
    m[0][3] = m[0][3] * scaleS + transS;
    m[1][0] *= scaleT;
    m[1][1] *= scaleT;
    m[1][2] *= scaleT;
    m[1][3] = m[1][3] * scaleT + transT;
}

void MTXLightPerspective(Mtx m, f32 fovY, f32 aspect, f32 scaleS,
                         f32 scaleT, f32 transS, f32 transT)
{
    MTXPerspective(m, fovY, aspect, 1.f, 2.f);
    m[0][0] *= scaleS;
    m[0][2] *= scaleS;
    m[0][3] = m[0][3] * scaleS + transS;
    m[1][1] *= scaleT;
    m[1][2] *= scaleT;
    m[1][3] = m[1][3] * scaleT + transT;
}

void MTXLightOrtho(Mtx m, f32 t, f32 b, f32 l, f32 r, f32 scaleS,
                   f32 scaleT, f32 transS, f32 transT)
{
    MTXOrtho(m, t, b, l, r, 0.f, 1.f);
    m[0][0] *= scaleS;
    m[0][3] = m[0][3] * scaleS + transS;
    m[1][1] *= scaleT;
    m[1][3] = m[1][3] * scaleT + transT;
}

// ------------------------------------------------------------ basic mtx
static void mtx_identity(Mtx m)
{
    memset(m, 0, sizeof(Mtx));
    m[0][0] = m[1][1] = m[2][2] = 1.f;
}

void C_MTXIdentity(Mtx m)
{
    mtx_identity(m);
}

void PSMTXIdentity(Mtx m)
{
    mtx_identity(m);
}

void C_MTXCopy(Mtx src, Mtx dst)
{
    memcpy(dst, src, sizeof(Mtx));
}

void PSMTXCopy(Mtx src, Mtx dst)
{
    memcpy(dst, src, sizeof(Mtx));
}

static void mtx_concat(const Mtx a, const Mtx b, Mtx ab)
{
    Mtx tmp;
    int i, j;
    for (i = 0; i < 3; i++) {
        for (j = 0; j < 4; j++) {
            tmp[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] +
                        a[i][2] * b[2][j] + (j == 3 ? a[i][3] : 0.f);
        }
    }
    memcpy(ab, tmp, sizeof(Mtx));
}

void C_MTXConcat(Mtx a, Mtx b, Mtx ab)
{
    mtx_concat(a, b, ab);
}

void PSMTXConcat(Mtx mA, Mtx mB, Mtx mAB)
{
    mtx_concat(mA, mB, mAB);
}

void C_MTXTranspose(Mtx src, Mtx xPose)
{
    Mtx tmp;
    tmp[0][0] = src[0][0];
    tmp[0][1] = src[1][0];
    tmp[0][2] = src[2][0];
    tmp[0][3] = 0;
    tmp[1][0] = src[0][1];
    tmp[1][1] = src[1][1];
    tmp[1][2] = src[2][1];
    tmp[1][3] = 0;
    tmp[2][0] = src[0][2];
    tmp[2][1] = src[1][2];
    tmp[2][2] = src[2][2];
    tmp[2][3] = 0;
    memcpy(xPose, tmp, sizeof(Mtx));
}

void PSMTXTranspose(Mtx src, Mtx xPose)
{
    C_MTXTranspose(src, xPose);
}

void C_MTXScale(Mtx m, f32 xS, f32 yS, f32 zS)
{
    mtx_identity(m);
    m[0][0] = xS;
    m[1][1] = yS;
    m[2][2] = zS;
}

void PSMTXScale(Mtx m, f32 xS, f32 yS, f32 zS)
{
    C_MTXScale(m, xS, yS, zS);
}

void MTXRotRad(Mtx m, char axis, f32 rad)
{
    f32 s = sinf(rad), c = cosf(rad);
    mtx_identity(m);
    switch (axis) {
    case 'x':
    case 'X':
        m[1][1] = c;
        m[1][2] = -s;
        m[2][1] = s;
        m[2][2] = c;
        break;
    case 'y':
    case 'Y':
        m[0][0] = c;
        m[0][2] = s;
        m[2][0] = -s;
        m[2][2] = c;
        break;
    default:
        m[0][0] = c;
        m[0][1] = -s;
        m[1][0] = s;
        m[1][1] = c;
        break;
    }
}

void C_MTXRotTrig(Mtx m, char axis, f32 sinA, f32 cosA)
{
    mtx_identity(m);
    switch (axis) {
    case 'x':
    case 'X':
        m[1][1] = cosA;
        m[1][2] = -sinA;
        m[2][1] = sinA;
        m[2][2] = cosA;
        break;
    case 'y':
    case 'Y':
        m[0][0] = cosA;
        m[0][2] = sinA;
        m[2][0] = -sinA;
        m[2][2] = cosA;
        break;
    default:
        m[0][0] = cosA;
        m[0][1] = -sinA;
        m[1][0] = sinA;
        m[1][1] = cosA;
        break;
    }
}

void PSMTXRotTrig(Mtx m, char axis, f32 sinA, f32 cosA)
{
    C_MTXRotTrig(m, axis, sinA, cosA);
}

void C_MTXRotAxisRad(Mtx m, Vec* axis, f32 rad)
{
    f32 s = sinf(rad), c = cosf(rad), t;
    f32 x = axis->x, y = axis->y, z = axis->z;
    f32 mag = sqrtf(x * x + y * y + z * z);
    if (mag > 0) {
        x /= mag;
        y /= mag;
        z /= mag;
    }
    t = 1.f - c;
    m[0][0] = t * x * x + c;
    m[0][1] = t * x * y - s * z;
    m[0][2] = t * x * z + s * y;
    m[0][3] = 0;
    m[1][0] = t * x * y + s * z;
    m[1][1] = t * y * y + c;
    m[1][2] = t * y * z - s * x;
    m[1][3] = 0;
    m[2][0] = t * x * z - s * y;
    m[2][1] = t * y * z + s * x;
    m[2][2] = t * z * z + c;
    m[2][3] = 0;
}

void PSMTXRotAxisRad(Mtx m, Vec* axis, f32 rad)
{
    C_MTXRotAxisRad(m, axis, rad);
}

void C_MTXQuat(Mtx m, QuaternionPtr q)
{
    f32 x = q->x, y = q->y, z = q->z, w = q->w;
    m[0][0] = 1 - 2 * (y * y + z * z);
    m[0][1] = 2 * (x * y - z * w);
    m[0][2] = 2 * (x * z + y * w);
    m[0][3] = 0;
    m[1][0] = 2 * (x * y + z * w);
    m[1][1] = 1 - 2 * (x * x + z * z);
    m[1][2] = 2 * (y * z - x * w);
    m[1][3] = 0;
    m[2][0] = 2 * (x * z - y * w);
    m[2][1] = 2 * (y * z + x * w);
    m[2][2] = 1 - 2 * (x * x + y * y);
    m[2][3] = 0;
}

void PSMTXQuat(Mtx m, QuaternionPtr q)
{
    C_MTXQuat(m, q);
}

u32 C_MTXInverse(Mtx src, Mtx inv)
{
    // affine inverse: invert 3x3, negate translation through it
    f32 a00 = src[0][0], a01 = src[0][1], a02 = src[0][2];
    f32 a10 = src[1][0], a11 = src[1][1], a12 = src[1][2];
    f32 a20 = src[2][0], a21 = src[2][1], a22 = src[2][2];
    f32 det = a00 * (a11 * a22 - a12 * a21) -
              a01 * (a10 * a22 - a12 * a20) +
              a02 * (a10 * a21 - a11 * a20);
    Mtx tmp;
    if (det == 0)
        return 0;
    tmp[0][0] = (a11 * a22 - a12 * a21) / det;
    tmp[0][1] = (a02 * a21 - a01 * a22) / det;
    tmp[0][2] = (a01 * a12 - a02 * a11) / det;
    tmp[1][0] = (a12 * a20 - a10 * a22) / det;
    tmp[1][1] = (a00 * a22 - a02 * a20) / det;
    tmp[1][2] = (a02 * a10 - a00 * a12) / det;
    tmp[2][0] = (a10 * a21 - a11 * a20) / det;
    tmp[2][1] = (a01 * a20 - a00 * a21) / det;
    tmp[2][2] = (a00 * a11 - a01 * a10) / det;
    tmp[0][3] = -(tmp[0][0] * src[0][3] + tmp[0][1] * src[1][3] +
                  tmp[0][2] * src[2][3]);
    tmp[1][3] = -(tmp[1][0] * src[0][3] + tmp[1][1] * src[1][3] +
                  tmp[1][2] * src[2][3]);
    tmp[2][3] = -(tmp[2][0] * src[0][3] + tmp[2][1] * src[1][3] +
                  tmp[2][2] * src[2][3]);
    memcpy(inv, tmp, sizeof(Mtx));
    return 1;
}

u32 PSMTXInverse(Mtx src, Mtx inv)
{
    return C_MTXInverse(src, inv);
}

u32 C_MTXInvXpose(Mtx src, Mtx invX)
{
    Mtx inv, xp;
    if (!C_MTXInverse(src, inv))
        return 0;
    C_MTXTranspose(inv, xp);
    memcpy(invX, xp, sizeof(Mtx));
    return 1;
}

u32 PSMTXInvXpose(Mtx src, Mtx invX)
{
    return C_MTXInvXpose(src, invX);
}

void C_MTXTrans(Mtx m, f32 xT, f32 yT, f32 zT)
{
    mtx_identity(m);
    m[0][3] = xT;
    m[1][3] = yT;
    m[2][3] = zT;
}

void PSMTXTrans(Mtx m, f32 xT, f32 yT, f32 zT)
{
    C_MTXTrans(m, xT, yT, zT);
}

void MTXTransApply(Mtx src, Mtx dst, f32 xT, f32 yT, f32 zT)
{
    Mtx t, out;
    MTXTrans(t, xT, yT, zT);
    mtx_concat(t, src, out);
    memcpy(dst, out, sizeof(Mtx));
}

void MTXScaleApply(Mtx src, Mtx dst, f32 xS, f32 yS, f32 zS)
{
    Mtx s, out;
    C_MTXScale(s, xS, yS, zS);
    mtx_concat(s, src, out);
    memcpy(dst, out, sizeof(Mtx));
}

void MTXReflect(Mtx m, Vec* p, Vec* n)
{
    f32 mag = sqrtf(n->x * n->x + n->y * n->y + n->z * n->z);
    f32 nx = n->x / mag, ny = n->y / mag, nz = n->z / mag;
    f32 d = -(nx * p->x + ny * p->y + nz * p->z);
    m[0][0] = 1 - 2 * nx * nx;
    m[0][1] = -2 * nx * ny;
    m[0][2] = -2 * nx * nz;
    m[0][3] = -2 * nx * d;
    m[1][0] = -2 * ny * nx;
    m[1][1] = 1 - 2 * ny * ny;
    m[1][2] = -2 * ny * nz;
    m[1][3] = -2 * ny * d;
    m[2][0] = -2 * nz * nx;
    m[2][1] = -2 * nz * ny;
    m[2][2] = 1 - 2 * nz * nz;
    m[2][3] = -2 * nz * d;
}

static void look_at(Mtx m, Vec* camPos, Vec* camUp, Vec* target)
{
    Vec f, s, u;
    f.x = target->x - camPos->x;
    f.y = target->y - camPos->y;
    f.z = target->z - camPos->z;
    {
        f32 mag = sqrtf(f.x * f.x + f.y * f.y + f.z * f.z);
        if (mag > 0) {
            f.x /= mag;
            f.y /= mag;
            f.z /= mag;
        }
    }
    // s = f x up
    s.x = f.y * camUp->z - f.z * camUp->y;
    s.y = f.z * camUp->x - f.x * camUp->z;
    s.z = f.x * camUp->y - f.y * camUp->x;
    {
        f32 mag = sqrtf(s.x * s.x + s.y * s.y + s.z * s.z);
        if (mag > 0) {
            s.x /= mag;
            s.y /= mag;
            s.z /= mag;
        }
    }
    // u = s x f
    u.x = s.y * f.z - s.z * f.y;
    u.y = s.z * f.x - s.x * f.z;
    u.z = s.x * f.y - s.y * f.x;
    m[0][0] = s.x;
    m[0][1] = s.y;
    m[0][2] = s.z;
    m[0][3] = -(s.x * camPos->x + s.y * camPos->y + s.z * camPos->z);
    m[1][0] = u.x;
    m[1][1] = u.y;
    m[1][2] = u.z;
    m[1][3] = -(u.x * camPos->x + u.y * camPos->y + u.z * camPos->z);
    m[2][0] = -f.x;
    m[2][1] = -f.y;
    m[2][2] = -f.z;
    m[2][3] = f.x * camPos->x + f.y * camPos->y + f.z * camPos->z;
}

void MTXLookAt(Mtx m, Vec* camPos, Vec* camUp, Vec* target)
{
    look_at(m, camPos, camUp, target);
}

void C_MTXLookAt(Mtx m, Point3dPtr camPos, VecPtr camUp, Point3dPtr target)
{
    look_at(m, camPos, camUp, target);
}

// ------------------------------------------------------------ mtx stack
void MTXInitStack(MTXStack* sPtr, u32 numMtx)
{
    if (!sPtr)
        return;
    sPtr->numMtx = numMtx;
    sPtr->stackPtr = sPtr->stackBase;
}

Mtx* MTXPush(MTXStack* sPtr, Mtx m)
{
    Mtx tmp;
    if (!sPtr || !sPtr->stackPtr)
        return NULL;
    mtx_concat(*sPtr->stackPtr, m, tmp);
    sPtr->stackPtr++;
    memcpy(sPtr->stackPtr, tmp, sizeof(Mtx));
    return sPtr->stackPtr;
}

Mtx* MTXPushFwd(MTXStack* sPtr, Mtx m)
{
    if (!sPtr || !sPtr->stackPtr)
        return NULL;
    sPtr->stackPtr++;
    memcpy(sPtr->stackPtr, m, sizeof(Mtx));
    return sPtr->stackPtr;
}

Mtx* MTXPushInv(MTXStack* sPtr, Mtx m)
{
    Mtx inv;
    if (!sPtr || !sPtr->stackPtr)
        return NULL;
    if (!C_MTXInverse(*sPtr->stackPtr, inv))
        return NULL;
    mtx_concat(inv, m, inv);
    sPtr->stackPtr++;
    memcpy(sPtr->stackPtr, inv, sizeof(Mtx));
    return sPtr->stackPtr;
}

Mtx* MTXPushInvXpose(MTXStack* sPtr, Mtx m)
{
    Mtx inv, xp;
    if (!sPtr || !sPtr->stackPtr)
        return NULL;
    if (!C_MTXInverse(*sPtr->stackPtr, inv))
        return NULL;
    C_MTXTranspose(inv, xp);
    mtx_concat(xp, m, inv);
    sPtr->stackPtr++;
    memcpy(sPtr->stackPtr, inv, sizeof(Mtx));
    return sPtr->stackPtr;
}

Mtx* MTXPop(MTXStack* sPtr)
{
    if (!sPtr || !sPtr->stackPtr)
        return NULL;
    if (sPtr->stackPtr > sPtr->stackBase)
        sPtr->stackPtr--;
    return sPtr->stackPtr;
}

Mtx* MTXGetStackPtr(MTXStack* sPtr)
{
    return sPtr ? sPtr->stackPtr : NULL;
}

// ------------------------------------------------------------ mtx * vec
void C_MTXMultVec(Mtx44 m, Vec* src, Vec* dst)
{
    Vec tmp;
    tmp.x = m[0][0] * src->x + m[0][1] * src->y + m[0][2] * src->z +
            m[0][3];
    tmp.y = m[1][0] * src->x + m[1][1] * src->y + m[1][2] * src->z +
            m[1][3];
    tmp.z = m[2][0] * src->x + m[2][1] * src->y + m[2][2] * src->z +
            m[2][3];
    *dst = tmp;
}

void PSMTXMultVec(Mtx44 m, Vec* src, Vec* dst)
{
    C_MTXMultVec(m, src, dst);
}

void C_MTXMultVecSR(Mtx44 m, Vec* src, Vec* dst)
{
    Vec tmp;
    tmp.x = m[0][0] * src->x + m[0][1] * src->y + m[0][2] * src->z;
    tmp.y = m[1][0] * src->x + m[1][1] * src->y + m[1][2] * src->z;
    tmp.z = m[2][0] * src->x + m[2][1] * src->y + m[2][2] * src->z;
    *dst = tmp;
}

void PSMTXMultVecSR(Mtx44 m, Vec* src, Vec* dst)
{
    C_MTXMultVecSR(m, src, dst);
}

void C_MTXMultVecArray(Mtx m, Vec* srcBase, Vec* dstBase, u32 count)
{
    u32 i;
    for (i = 0; i < count; i++) {
        Vec* s = &srcBase[i];
        Vec* d = &dstBase[i];
        d->x = m[0][0] * s->x + m[0][1] * s->y + m[0][2] * s->z + m[0][3];
        d->y = m[1][0] * s->x + m[1][1] * s->y + m[1][2] * s->z + m[1][3];
        d->z = m[2][0] * s->x + m[2][1] * s->y + m[2][2] * s->z + m[2][3];
    }
}

void PSMTXMultVecArray(Mtx m, Vec* srcBase, Vec* dstBase, u32 count)
{
    C_MTXMultVecArray(m, srcBase, dstBase, count);
}

void MTXMultVecArraySR(Mtx44 m, Vec* srcBase, Vec* dstBase, u32 count)
{
    u32 i;
    for (i = 0; i < count; i++)
        C_MTXMultVecSR(m, &srcBase[i], &dstBase[i]);
}

// psmtx ROM order variants (ROMtx = column-major 4x3)
void PSMTXReorder(Mtx src, ROMtx dest)
{
    int i, j;
    for (i = 0; i < 4; i++) {
        for (j = 0; j < 3; j++)
            dest[i][j] = src[j][i];
    }
}

void PSMTXROMultVecArray(ROMtx* m, Vec* srcBase, Vec* dstBase, u32 count)
{
    u32 i;
    for (i = 0; i < count; i++) {
        Vec* s = &srcBase[i];
        Vec* d = &dstBase[i];
        d->x = (*m)[0][0] * s->x + (*m)[1][0] * s->y + (*m)[2][0] * s->z +
               (*m)[3][0];
        d->y = (*m)[0][1] * s->x + (*m)[1][1] * s->y + (*m)[2][1] * s->z +
               (*m)[3][1];
        d->z = (*m)[0][2] * s->x + (*m)[1][2] * s->y + (*m)[2][2] * s->z +
               (*m)[3][2];
    }
}

void PSMTXROSkin2VecArray(ROMtx* m0, ROMtx* m1, f32* wtBase, Vec* srcBase,
                          Vec* dstBase, u32 count)
{
    u32 i;
    for (i = 0; i < count; i++) {
        Vec a, b, s = srcBase[i];
        f32 w = wtBase[i];
        a.x = (*m0)[0][0] * s.x + (*m0)[1][0] * s.y + (*m0)[2][0] * s.z +
              (*m0)[3][0];
        a.y = (*m0)[0][1] * s.x + (*m0)[1][1] * s.y + (*m0)[2][1] * s.z +
              (*m0)[3][1];
        a.z = (*m0)[0][2] * s.x + (*m0)[1][2] * s.y + (*m0)[2][2] * s.z +
              (*m0)[3][2];
        b.x = (*m1)[0][0] * s.x + (*m1)[1][0] * s.y + (*m1)[2][0] * s.z +
              (*m1)[3][0];
        b.y = (*m1)[0][1] * s.x + (*m1)[1][1] * s.y + (*m1)[2][1] * s.z +
              (*m1)[3][1];
        b.z = (*m1)[0][2] * s.x + (*m1)[1][2] * s.y + (*m1)[2][2] * s.z +
              (*m1)[3][2];
        dstBase[i].x = a.x * w + b.x * (1.f - w);
        dstBase[i].y = a.y * w + b.y * (1.f - w);
        dstBase[i].z = a.z * w + b.z * (1.f - w);
    }
}

void PSMTXROMultS16VecArray(ROMtx* m, S16Vec* srcBase, Vec* dstBase,
                            u32 count)
{
    u32 i;
    for (i = 0; i < count; i++) {
        Vec s = { (f32) srcBase[i].x, (f32) srcBase[i].y,
                  (f32) srcBase[i].z };
        Vec* d = &dstBase[i];
        d->x = (*m)[0][0] * s.x + (*m)[1][0] * s.y + (*m)[2][0] * s.z +
               (*m)[3][0];
        d->y = (*m)[0][1] * s.x + (*m)[1][1] * s.y + (*m)[2][1] * s.z +
               (*m)[3][1];
        d->z = (*m)[0][2] * s.x + (*m)[1][2] * s.y + (*m)[2][2] * s.z +
               (*m)[3][2];
    }
}

void PSMTXMultS16VecArray(Mtx44* m, S16Vec* srcBase, Vec* dstBase,
                          u32 count)
{
    u32 i;
    for (i = 0; i < count; i++) {
        Vec* d = &dstBase[i];
        f32 x = (f32) srcBase[i].x, y = (f32) srcBase[i].y,
            z = (f32) srcBase[i].z;
        d->x = (*m)[0][0] * x + (*m)[0][1] * y + (*m)[0][2] * z +
               (*m)[0][3];
        d->y = (*m)[1][0] * x + (*m)[1][1] * y + (*m)[1][2] * z +
               (*m)[1][3];
        d->z = (*m)[2][0] * x + (*m)[2][1] * y + (*m)[2][2] * z +
               (*m)[2][3];
    }
}

// ------------------------------------------------------------ vec
f32 C_VECMag(Vec* v)
{
    return sqrtf(v->x * v->x + v->y * v->y + v->z * v->z);
}

f32 PSVECMag(Vec* v)
{
    return C_VECMag(v);
}

void VECHalfAngle(Vec* a, Vec* b, Vec* half)
{
    half->x = a->x + b->x;
    half->y = a->y + b->y;
    half->z = a->z + b->z;
    C_VECNormalize(half, half);
}

void VECReflect(Vec* src, Vec* normal, Vec* dst)
{
    f32 d = 2.f * (src->x * normal->x + src->y * normal->y +
                   src->z * normal->z);
    dst->x = src->x - d * normal->x;
    dst->y = src->y - d * normal->y;
    dst->z = src->z - d * normal->z;
}

f32 VECDistance(Vec* a, Vec* b)
{
    f32 dx = a->x - b->x, dy = a->y - b->y, dz = a->z - b->z;
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

void C_VECAdd(Vec* a, Vec* b, Vec* c)
{
    c->x = a->x + b->x;
    c->y = a->y + b->y;
    c->z = a->z + b->z;
}

void PSVECAdd(Vec* a, Vec* b, Vec* c)
{
    C_VECAdd(a, b, c);
}

void C_VECSubtract(Vec* a, Vec* b, Vec* c)
{
    c->x = a->x - b->x;
    c->y = a->y - b->y;
    c->z = a->z - b->z;
}

void PSVECSubtract(Vec* a, Vec* b, Vec* c)
{
    C_VECSubtract(a, b, c);
}

void C_VECScale(Vec* src, Vec* dst, f32 scale)
{
    dst->x = src->x * scale;
    dst->y = src->y * scale;
    dst->z = src->z * scale;
}

void PSVECScale(Vec* src, Vec* dst, f32 mult)
{
    C_VECScale(src, dst, mult);
}

void C_VECNormalize(Vec* src, Vec* unit)
{
    f32 mag = C_VECMag(src);
    if (mag > 0) {
        unit->x = src->x / mag;
        unit->y = src->y / mag;
        unit->z = src->z / mag;
    } else {
        unit->x = unit->y = unit->z = 0;
    }
}

void PSVECNormalize(Vec* vec1, Vec* dst)
{
    C_VECNormalize(vec1, dst);
}

f32 C_VECSquareMag(Vec* v)
{
    return v->x * v->x + v->y * v->y + v->z * v->z;
}

f32 PSVECSquareMag(Vec* vec1)
{
    return C_VECSquareMag(vec1);
}

f32 C_VECDotProduct(Vec* a, Vec* b)
{
    return a->x * b->x + a->y * b->y + a->z * b->z;
}

f32 PSVECDotProduct(Vec* vec1, Vec* vec2)
{
    return C_VECDotProduct(vec1, vec2);
}

void C_VECCrossProduct(Vec* a, Vec* b, Vec* axb)
{
    axb->x = a->y * b->z - a->z * b->y;
    axb->y = a->z * b->x - a->x * b->z;
    axb->z = a->x * b->y - a->y * b->x;
}

void PSVECCrossProduct(Vec* vec1, Vec* vec2, Vec* dst)
{
    C_VECCrossProduct(vec1, vec2, dst);
}

f32 C_VECSquareDistance(Vec* a, Vec* b)
{
    f32 dx = a->x - b->x, dy = a->y - b->y, dz = a->z - b->z;
    return dx * dx + dy * dy + dz * dz;
}

f32 PSVECSquareDistance(Vec* vec1, Vec* vec2)
{
    return C_VECSquareDistance(vec1, vec2);
}

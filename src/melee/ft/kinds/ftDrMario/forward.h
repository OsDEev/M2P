#ifndef MELEE_FT_CHARA_FTDRMARIO_FORWARD_H
#define MELEE_FT_CHARA_FTDRMARIO_FORWARD_H

#include <melee/ft/forward.h>

/* MotionFlags combinators are object-like macros (untyped constant
 * expressions): MSVC C rejects static initializers that reference
 * other static objects (C2099), and per-declaration anonymous enums
 * give every constant a distinct type, tripping C5287 on `|`.
 * Same pattern as FtMotionFlags in melee/ft/forward.h: names, values
 * and order are unchanged. */
#define ftDr_MF_Appeal (Ft_MF_KeepFastFall | Ft_MF_SkipModel | Ft_MF_SkipAnimVel | Ft_MF_Unk06)

#endif

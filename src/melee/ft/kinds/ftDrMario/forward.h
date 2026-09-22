#ifndef MELEE_FT_CHARA_FTDRMARIO_FORWARD_H
#define MELEE_FT_CHARA_FTDRMARIO_FORWARD_H

#include <melee/ft/forward.h>

/* MotionFlags combinators are enum constants, not static const
 * objects: MSVC C rejects static initializers that reference other
 * static objects (C2099). Same pattern as FtMotionFlags in
 * melee/ft/forward.h. */
enum { ftDr_MF_Appeal =
    Ft_MF_KeepFastFall | Ft_MF_SkipModel | Ft_MF_SkipAnimVel | Ft_MF_Unk06 };

#endif

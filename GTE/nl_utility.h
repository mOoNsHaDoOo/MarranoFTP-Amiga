

#ifndef HEADERS_NL_UTILITY_H
#define HEADERS_NL_UTILITY_H 1


/*  New.lib/utility.h  $ 27/01/93 MT $  */
/*                                      */
/*  File header per l'uso di GTE.lib.   */
/*  Per usare queste funzioni deve      */
/*  essere aperta (solo sotto 2.x+) la  */
/*  utility.library.                    */


#include "exec/types.h"
#include "utility/tagitem.h"


/* ---------------------- Funzioni di interfaccia ---------------------- */

/* Funzioni di gestione dei Tags */

struct TagItem *NL_FindTagItem(Tag tagval, struct TagItem *taglist);
ULONG NL_GetTagData(Tag tagval, ULONG defaultdata, struct TagItem *taglist);
struct TagItem *NL_NextTagItem(struct TagItem **taglistptr);

/* --------------------------------------------------------------------- */


/* ---------------------- Funzioni di emulazione ----------------------- */

/* Funzioni di gestione dei Tags */

struct TagItem *EF_FindTagItem(Tag tagval, struct TagItem *taglist);
ULONG EF_GetTagData(Tag tagval, ULONG defaultdata, struct TagItem *taglist);
struct TagItem *EF_NextTagItem(struct TagItem **taglistptr);

/* --------------------------------------------------------------------- */


#endif



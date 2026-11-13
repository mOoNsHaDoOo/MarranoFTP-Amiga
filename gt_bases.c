/*
 * GT_BASES.C - library bases of MarranoFTP13.
 *
 * Defined here (not left "extern") so sc.lib does not auto-open them; GTE.lib
 * was compiled with near data (A4 relative references), so they must live in
 * the near data section. A separate file because the __near definition
 * conflicts with the declarations of the proto/ headers.
 */
#include <exec/types.h>

struct IntuitionBase;
struct GfxBase;

struct IntuitionBase * __near IntuitionBase = NULL;
struct GfxBase * __near GfxBase = NULL;

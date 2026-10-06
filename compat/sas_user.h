// compat/sas_user.h
//
// Missing from the source dump but #included unguarded by 28 files. It selected
// which developer's personal debug/tuning code was compiled in -- every use is
// a preprocessor comparison like:
//
//     #if SAS_ACTIVE_USER == SAS_USER_ELLIOTT
//
// so the symbols must be macros (an enum would not be visible to #if) and the
// default has to be a real member of the set.
//
// SAS_ACTIVE_USER is set to SAS_USER_NONE, which disables all per-developer
// blocks. That is the behaviour we want: retail-like, no leftover debug hacks.
// To re-enable one, e.g. -DSAS_ACTIVE_USER=SAS_USER_ELLIOTT.

#pragma once

#define SAS_USER_NONE     0
#define SAS_USER_ELLIOTT  1
#define SAS_USER_CHRIS    2
#define SAS_USER_DAN      3
#define SAS_USER_JEREMY   4
#define SAS_USER_JOHN     5
#define SAS_USER_MIKE     6
#define SAS_USER_NATHAN   7
#define SAS_USER_PAT      8
#define SAS_USER_RUSS     9
#define SAS_USER_SCHOLZ  10
#define SAS_USER_STEVE   11

#ifndef SAS_ACTIVE_USER
#define SAS_ACTIVE_USER SAS_USER_NONE
#endif

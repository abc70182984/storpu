#ifndef FMGR_H
#define FMGR_H

#include "types.h"

/* We don't want to include primnodes.h here, so make some stub references */
typedef struct Node* fmNodePtr;
typedef struct Aggref* fmAggrefPtr;

/* Likewise, avoid including execnodes.h here */
typedef void (*fmExprContextCallbackFunction)(Datum arg);

/* Likewise, avoid including stringinfo.h here */
typedef struct StringInfoData* fmStringInfo;

/*
 * All functions that can be called directly by fmgr must have this signature.
 * (Other functions can be called by using a handler that does have this
 * signature.)
 */

typedef struct FunctionCallInfoBaseData* FunctionCallInfo;

typedef Datum (*PGFunction)(FunctionCallInfo fcinfo);

/*
 * This struct holds the system-catalog information that must be looked up
 * before a function can be called through fmgr.  If the same function is
 * to be called multiple times, the lookup need be done only once and the
 * info struct saved for re-use.
 *
 * Note that fn_expr really is parse-time-determined information about the
 * arguments, rather than about the function itself.  But it's convenient to
 * store it here rather than in FunctionCallInfoBaseData, where it might more
 * logically belong.
 *
 * fn_extra is available for use by the called function; all other fields
 * should be treated as read-only after the struct is created.
 */
typedef struct FmgrInfo {
    PGFunction fn_addr;     /* pointer to function or handler to be called */
    Oid fn_oid;             /* OID of function (NOT of handler, if any) */
    short fn_nargs;         /* number of input args (0..FUNC_MAX_ARGS) */
    bool fn_strict;         /* function is "strict" (NULL in => NULL out) */
    bool fn_retset;         /* function returns a set */
    unsigned char fn_stats; /* collect stats if track_functions > this */
    void* fn_extra;         /* extra space for use by handler */
    void* fn_mcxt;          /* memory context to store fn_extra in */
    fmNodePtr fn_expr;      /* expression parse tree for call, or NULL */
} FmgrInfo;

typedef struct {
    Oid foid;             /* OID of the function */
    short nargs;          /* 0..FUNC_MAX_ARGS, or -1 if variable count */
    bool strict;          /* T if function is "strict" */
    bool retset;          /* T if function returns a set */
    const char* funcName; /* C name of the function */
    PGFunction func;      /* pointer to compiled function */
} FmgrBuiltin;

#define InvalidOidBuiltinMapping UINT16_MAX
extern const FmgrBuiltin fmgr_builtins[];

/*
 * This struct is the data actually passed to an fmgr-called function.
 *
 * The called function is expected to set isnull, and possibly resultinfo or
 * fields in whatever resultinfo points to.  It should not change any other
 * fields.  (In particular, scribbling on the argument arrays is a bad idea,
 * since some callers assume they can re-call with the same arguments.)
 *
 * Note that enough space for arguments needs to be provided, either by using
 * SizeForFunctionCallInfo() in dynamic allocations, or by using
 * LOCAL_FCINFO() for on-stack allocations.
 *
 * This struct is named *BaseData, rather than *Data, to break pre v12 code
 * that allocated FunctionCallInfoData itself, as it'd often silently break
 * old code due to no space for arguments being provided.
 */
typedef struct FunctionCallInfoBaseData {
    FmgrInfo* flinfo;     /* ptr to lookup info used for this call */
    fmNodePtr context;    /* pass info about context of call */
    fmNodePtr resultinfo; /* pass or return extra info about result */
    Oid fncollation;      /* collation for function to use */
#define FIELDNO_FUNCTIONCALLINFODATA_ISNULL 4
    bool isnull; /* function must set true if result is NULL */
    short nargs; /* # arguments actually passed */
#define FIELDNO_FUNCTIONCALLINFODATA_ARGS 6
    NullableDatum args[];
} FunctionCallInfoBaseData;

/*
 * Space needed for a FunctionCallInfoBaseData struct with sufficient space
 * for `nargs` arguments.
 */
#define SizeForFunctionCallInfo(nargs) \
    (offsetof(FunctionCallInfoBaseData, args) + sizeof(NullableDatum) * (nargs))

/*
 * This macro ensures that `name` points to a stack-allocated
 * FunctionCallInfoBaseData struct with sufficient space for `nargs` arguments.
 */
#define LOCAL_FCINFO(name, nargs)                                        \
    /* use union with FunctionCallInfoBaseData to guarantee alignment */ \
    union {                                                              \
        FunctionCallInfoBaseData fcinfo;                                 \
        /* ensure enough space for nargs args is available */            \
        char fcinfo_data[SizeForFunctionCallInfo(nargs)];                \
    } name##data;                                                        \
    FunctionCallInfo name = &name##data.fcinfo

#define InitFunctionCallInfoData(Fcinfo, Flinfo, Nargs, Collation, Context, \
                                 Resultinfo)                                \
    do {                                                                    \
        (Fcinfo).flinfo = (Flinfo);                                         \
        (Fcinfo).context = (Context);                                       \
        (Fcinfo).resultinfo = (Resultinfo);                                 \
        (Fcinfo).fncollation = (Collation);                                 \
        (Fcinfo).isnull = false;                                            \
        (Fcinfo).nargs = (Nargs);                                           \
    } while (0)

#define FunctionCallInvoke(fcinfo) ((*(fcinfo)->flinfo->fn_addr)(fcinfo))

#define PG_FUNCTION_ARGS FunctionCallInfo fcinfo

#define PG_GETARG_DATUM(n) (fcinfo->args[n].value)

#define PG_ARGISNULL(n) (fcinfo->args[n].isnull)

#define PG_RETURN_NULL()       \
    do {                       \
        fcinfo->isnull = true; \
        return (Datum)0;       \
    } while (0)

__BEGIN_DECLS

const FmgrBuiltin* fmgr_isbuiltin(Oid id);

extern Datum FunctionCall0Coll(FmgrInfo* flinfo, Oid collation);
extern Datum FunctionCall1Coll(FmgrInfo* flinfo, Oid collation, Datum arg1);
extern Datum FunctionCall2Coll(FmgrInfo* flinfo, Oid collation, Datum arg1,
                               Datum arg2);
extern Datum FunctionCall3Coll(FmgrInfo* flinfo, Oid collation, Datum arg1,
                               Datum arg2, Datum arg3);
extern Datum FunctionCall4Coll(FmgrInfo* flinfo, Oid collation, Datum arg1,
                               Datum arg2, Datum arg3, Datum arg4);

__END_DECLS

#endif

#include "fmgr.h"

#include <stddef.h>

Datum FunctionCall0Coll(FmgrInfo* flinfo, Oid collation)
{
    LOCAL_FCINFO(fcinfo, 0);
    Datum result;

    InitFunctionCallInfoData(*fcinfo, flinfo, 0, collation, NULL, NULL);

    result = FunctionCallInvoke(fcinfo);

    return result;
}

Datum FunctionCall1Coll(FmgrInfo* flinfo, Oid collation, Datum arg1)
{
    LOCAL_FCINFO(fcinfo, 1);
    Datum result;

    InitFunctionCallInfoData(*fcinfo, flinfo, 1, collation, NULL, NULL);

    fcinfo->args[0].value = arg1;
    fcinfo->args[0].isnull = false;

    result = FunctionCallInvoke(fcinfo);

    return result;
}

Datum FunctionCall2Coll(FmgrInfo* flinfo, Oid collation, Datum arg1, Datum arg2)
{
    LOCAL_FCINFO(fcinfo, 2);
    Datum result;

    InitFunctionCallInfoData(*fcinfo, flinfo, 2, collation, NULL, NULL);

    fcinfo->args[0].value = arg1;
    fcinfo->args[0].isnull = false;
    fcinfo->args[1].value = arg2;
    fcinfo->args[1].isnull = false;

    result = FunctionCallInvoke(fcinfo);

    return result;
}

Datum FunctionCall3Coll(FmgrInfo* flinfo, Oid collation, Datum arg1, Datum arg2,
                        Datum arg3)
{
    LOCAL_FCINFO(fcinfo, 3);
    Datum result;

    InitFunctionCallInfoData(*fcinfo, flinfo, 3, collation, NULL, NULL);

    fcinfo->args[0].value = arg1;
    fcinfo->args[0].isnull = false;
    fcinfo->args[1].value = arg2;
    fcinfo->args[1].isnull = false;
    fcinfo->args[2].value = arg3;
    fcinfo->args[2].isnull = false;

    result = FunctionCallInvoke(fcinfo);

    return result;
}

Datum FunctionCall4Coll(FmgrInfo* flinfo, Oid collation, Datum arg1, Datum arg2,
                        Datum arg3, Datum arg4)
{
    LOCAL_FCINFO(fcinfo, 4);
    Datum result;

    InitFunctionCallInfoData(*fcinfo, flinfo, 4, collation, NULL, NULL);

    fcinfo->args[0].value = arg1;
    fcinfo->args[0].isnull = false;
    fcinfo->args[1].value = arg2;
    fcinfo->args[1].isnull = false;
    fcinfo->args[2].value = arg3;
    fcinfo->args[2].isnull = false;
    fcinfo->args[3].value = arg4;
    fcinfo->args[3].isnull = false;

    result = FunctionCallInvoke(fcinfo);

    return result;
}
